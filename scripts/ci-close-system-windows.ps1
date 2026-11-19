# CI only: closes system windows a hosted Windows image shows over the desktop
# before the native e2e runs, so the clicks and pointer checks land on the
# test window. GitHub's windows-11-arm image opens a "Microsoft account"
# window (a Windows.UI.Core.CoreWindow, topmost) some time after the job
# starts; it sat over the click points of the navigation guard and the
# pointer checks.
#
# Waits up to -Seconds (default 30) for such a window, closes every one it
# finds (the process that owns it, by id), and logs what it closed. Finding
# none is not an error. Never run outside a throwaway CI machine.
#
#   pwsh -File scripts/ci-close-system-windows.ps1 [-Seconds 30]
param([int]$Seconds = 30)
$ErrorActionPreference = 'Stop'

# Window titles to close (exact match).
$Titles = @('Microsoft account')
# Processes never stopped even when one owns such a window: the shell and
# the session's own (the tray and focus checks need them).
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

# Every visible top-level window whose title is in $Titles: @{ Pid; Title; Class }.
function Find-SystemWindows {
  $found = New-Object System.Collections.Generic.List[object]
  # The owning process's main window first (a classic app).
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
  $found | Sort-Object Pid -Unique
}

$deadline = (Get-Date).AddSeconds($Seconds)
$closed = 0
do {
  foreach ($w in Find-SystemWindows) {
    if ($w.Pid -le 4 -or $w.Pid -eq $PID) { continue }
    if ($Keep -contains $w.Name) {
      Write-Host "::warning::'$($w.Title)' ($($w.Class)) belongs to $($w.Name) pid $($w.Pid): not stopped"
      continue
    }
    Write-Host "closing '$($w.Title)' ($($w.Class)) of $($w.Name) pid $($w.Pid)"
    try {
      Stop-Process -Id $w.Pid -Force
      $closed++
    } catch {
      Write-Host "::warning::could not stop pid $($w.Pid): $_"
    }
  }
  # Once one was closed, a short settle and a last look (it can come back).
  if ($closed -gt 0) {
    Start-Sleep -Seconds 2
    if (-not (Find-SystemWindows)) { break }
  } else {
    Start-Sleep -Seconds 1
  }
} while ((Get-Date) -lt $deadline)

$left = @(Find-SystemWindows)
if ($left.Count -gt 0) {
  Write-Host "::warning::still open after $Seconds s: $(($left | ForEach-Object { "'$($_.Title)' pid $($_.Pid)" }) -join ', ')"
} elseif ($closed -eq 0) {
  Write-Host "no system window to close (waited $Seconds s)"
} else {
  Write-Host "closed $closed system window(s)"
}
