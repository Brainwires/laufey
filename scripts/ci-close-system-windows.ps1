# CI only: keeps Windows' own prompts off the desktop while the native e2e
# runs, so its clicks and pointer checks land on the test window. GitHub's
# windows-11-arm image shows "Microsoft account" windows: the second-chance
# setup experience (SCOOBE, "finish setting up your device") as a
# Windows.UI.Core.CoreWindow, which sits in a system band above topmost
# windows (raising the test window can't pass it), and a Shell_OOBEProxy
# window of explorer's. It can open at any time during the run.
#
# Three modes; never run any of them outside a throwaway CI machine:
#
#   -OptOut     turns the prompt off for the runner's user before the e2e:
#               - HKCU\Software\Microsoft\Windows\CurrentVersion\
#                 UserProfileEngagement ScoobeSystemSettingEnabled = 0, the
#                 per-user SCOOBE switch (Settings > System > Notifications,
#                 "Suggest ways to get the most out of Windows and finish
#                 setting up this device"; Microsoft Q&A answers name it);
#               - the Group Policy values of Microsoft's Policy CSP
#                 (learn.microsoft.com, "Experience Policy CSP"), under
#                 Software\Policies\Microsoft\Windows\CloudContent:
#                 DisableWindowsSpotlightWindowsWelcomeExperience = 1 in HKCU
#                 (AllowWindowsSpotlightWindowsWelcomeExperience, user scope)
#                 and DisableSoftLanding = 1 in HKLM (AllowWindowsTips,
#                 device scope).
#               Windows may still show a prompt it had queued, hence:
#   (default)   waits up to -Seconds (30) for such a window and stops the
#               process that owns it, then returns;
#   -Watch      does the same every -Interval seconds (2) until -StopFile
#               exists, for the whole e2e, started detached before it and
#               stopped by creating the file after it. Logs to -LogFile.
#
# A window owned by the shell or the session (explorer, dwm, ...) is never
# stopped: the tray and focus checks need them; it is logged once.
param(
  [switch]$OptOut,
  [switch]$Watch,
  [int]$Seconds = 30,
  [int]$Interval = 2,
  [string]$StopFile = '',
  [string]$LogFile = ''
)
$ErrorActionPreference = 'Stop'

function Log([string]$line) {
  $stamped = "$(Get-Date -Format 'HH:mm:ss') $line"
  if ($LogFile) { Add-Content -Path $LogFile -Value $stamped } else { Write-Host $stamped }
}

if ($OptOut) {
  $values = @(
    @('HKCU:\Software\Microsoft\Windows\CurrentVersion\UserProfileEngagement', 'ScoobeSystemSettingEnabled', 0),
    @('HKCU:\Software\Policies\Microsoft\Windows\CloudContent', 'DisableWindowsSpotlightWindowsWelcomeExperience', 1),
    @('HKLM:\Software\Policies\Microsoft\Windows\CloudContent', 'DisableSoftLanding', 1)
  )
  foreach ($v in $values) {
    try {
      if (-not (Test-Path $v[0])) { New-Item -Path $v[0] -Force | Out-Null }
      New-ItemProperty -Path $v[0] -Name $v[1] -Value $v[2] -PropertyType DWord -Force | Out-Null
      Log "set $($v[0])\$($v[1]) = $($v[2])"
    } catch {
      Write-Host "::warning::could not set $($v[0])\$($v[1]): $_"
    }
  }
  exit 0
}

# Window titles to close (exact match).
$Titles = @('Microsoft account')
# Processes never stopped even when one owns such a window: the shell and
# the session's own.
$Keep = @('explorer', 'dwm', 'csrss', 'winlogon', 'wininit', 'lsass', 'services', 'svchost', 'sihost', 'smss', 'System', 'Idle')

Add-Type -Namespace LaufeyCi -Name User32 -MemberDefinition @'
public delegate bool EnumWindowsProc(System.IntPtr hwnd, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool EnumWindows(EnumWindowsProc callback, System.IntPtr lparam);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetWindowTextW(System.IntPtr hwnd, System.Text.StringBuilder text, int max);
[System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
public static extern int GetClassNameW(System.IntPtr hwnd, System.Text.StringBuilder text, int max);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(System.IntPtr hwnd, out uint pid);
[System.Runtime.InteropServices.DllImport("user32.dll")]
public static extern bool IsWindowVisible(System.IntPtr hwnd);
'@

# Every visible top-level window whose title is in $Titles: Pid, Title, Class, Name.
function Find-SystemWindows {
  $found = New-Object System.Collections.Generic.List[object]
  # The owning process's main window (a classic app).
  foreach ($p in Get-Process | Where-Object { $Titles -contains $_.MainWindowTitle }) {
    $found.Add([pscustomobject]@{ Pid = $p.Id; Title = $p.MainWindowTitle; Class = '(main window)'; Name = $p.ProcessName })
  }
  # A CoreWindow is often nobody's MainWindowTitle: every top-level window.
  $callback = [LaufeyCi.User32+EnumWindowsProc] {
    param($hwnd, $lparam)
    if ([LaufeyCi.User32]::IsWindowVisible($hwnd)) {
      $title = New-Object System.Text.StringBuilder 256
      [void][LaufeyCi.User32]::GetWindowTextW($hwnd, $title, 256)
      if ($Titles -contains $title.ToString()) {
        $class = New-Object System.Text.StringBuilder 256
        [void][LaufeyCi.User32]::GetClassNameW($hwnd, $class, 256)
        $windowPid = [uint32]0
        [void][LaufeyCi.User32]::GetWindowThreadProcessId($hwnd, [ref]$windowPid)
        $name = (Get-Process -Id $windowPid -ErrorAction SilentlyContinue).ProcessName
        $found.Add([pscustomobject]@{ Pid = [int]$windowPid; Title = $title.ToString(); Class = $class.ToString(); Name = $name })
      }
    }
    return $true
  }
  [void][LaufeyCi.User32]::EnumWindows($callback, [System.IntPtr]::Zero)
  $found | Sort-Object Pid, Class -Unique
}

$script:kept = @{}
$script:closed = 0
# One pass: stops the owner of every such window it may stop; returns how
# many windows are left that it may stop (0 when the desktop is clear).
function Clear-Once {
  $left = 0
  foreach ($w in Find-SystemWindows) {
    if ($w.Pid -le 4 -or $w.Pid -eq $PID) { continue }
    if ($Keep -contains $w.Name) {
      $key = "$($w.Pid)/$($w.Class)"
      if (-not $script:kept.ContainsKey($key)) {
        $script:kept[$key] = $true
        Log "leaving '$($w.Title)' ($($w.Class)) of $($w.Name) pid $($w.Pid): a shell process"
      }
      continue
    }
    Log "stopping '$($w.Title)' ($($w.Class)) of $($w.Name) pid $($w.Pid)"
    try {
      Stop-Process -Id $w.Pid -Force
      $script:closed++
    } catch {
      Log "could not stop pid $($w.Pid): $_"
      $left++
    }
  }
  $left
}

if ($Watch) {
  if (-not $StopFile) { throw '-Watch needs -StopFile' }
  Log "watching for system windows every $Interval s until $StopFile exists"
  while (-not (Test-Path $StopFile)) {
    try { [void](Clear-Once) } catch { Log "pass failed: $_" }
    Start-Sleep -Seconds $Interval
  }
  Log "watch ended: stopped $script:closed process(es)"
  exit 0
}

$deadline = (Get-Date).AddSeconds($Seconds)
do {
  $left = Clear-Once
  if ($script:closed -gt 0 -and $left -eq 0) {
    # A short settle and a last look (it can come back).
    Start-Sleep -Seconds 2
    if ((Clear-Once) -eq 0) { break }
  } else {
    Start-Sleep -Seconds 1
  }
} while ((Get-Date) -lt $deadline)
if ($script:closed -eq 0) {
  Log "no system window stopped (waited $Seconds s)"
} else {
  Log "stopped $script:closed process(es)"
}
