// Copyright 2025 Divy Srivastava. All rights reserved. MIT license.

//! Platform features (API 45) for the winit / servo backends: the same JSON
//! object the CEF and WebView backends build in backend-common
//! (`platform_features.cc`; docs/platform-features.md), probed in Rust. On
//! Linux it reads the session bus (zbus) and, for the XEmbed tray, the X
//! server (x11rb); nothing branches on `XDG_CURRENT_DESKTOP`, which is only
//! a hint for a reason's wording. Winit has no web engine, so
//! `cookieEncryption` is always `null`.
//!
//! The tray host is read live on every call (Winit runs no GLib loop to
//! follow NameOwnerChanged with), so a watcher that starts late counts on the
//! next call.

use std::collections::BTreeMap;

/// What the probe found (the fields of the JSON object).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct PlatformFeatures {
  pub os: &'static str,
  pub session_type: Option<String>,
  pub desktop_hint: Option<String>,
  pub session_bus: bool,
  pub tray_watcher: bool,
  pub tray_xembed: bool,
  pub secret_service: &'static str,
  pub secret_prompter: bool,
  pub portal_versions: BTreeMap<String, u32>,
}

impl PlatformFeatures {
  /// Whether a tray icon can be seen here.
  pub fn tray_host(&self) -> bool {
    self.os != "linux" || self.tray_watcher || self.tray_xembed
  }

  /// Why not, when it can't.
  pub fn tray_reason(&self) -> Option<String> {
    if self.tray_host() {
      return None;
    }
    let mut reason = String::from(
      "no tray host: nothing owns org.kde.StatusNotifierWatcher on the \
       session bus",
    );
    if self.session_type.as_deref() == Some("x11") {
      reason.push_str(" and no XEmbed system tray runs");
    }
    if let Some(hint) = &self.desktop_hint {
      reason.push_str(&format!(" (XDG_CURRENT_DESKTOP={hint})"));
    }
    reason.push_str(
      "; GNOME shows tray icons only with the AppIndicator extension enabled",
    );
    Some(reason)
  }

  /// The JSON object `platform_features` hands out.
  pub fn to_json(&self) -> String {
    fn quote(s: &str) -> String {
      let mut out = String::from("\"");
      for c in s.chars() {
        match c {
          '"' => out.push_str("\\\""),
          '\\' => out.push_str("\\\\"),
          '\n' => out.push_str("\\n"),
          '\r' => out.push_str("\\r"),
          '\t' => out.push_str("\\t"),
          c if (c as u32) < 0x20 => {
            out.push_str(&format!("\\u{:04x}", c as u32))
          }
          c => out.push(c),
        }
      }
      out.push('"');
      out
    }
    fn opt(s: &Option<String>) -> String {
      s.as_deref().map(quote).unwrap_or_else(|| "null".into())
    }
    let portals = self
      .portal_versions
      .iter()
      .map(|(k, v)| format!("{}:{v}", quote(k)))
      .collect::<Vec<_>>()
      .join(",");
    // tray-icon on Linux (libappindicator) reports no clicks; elsewhere it
    // does. Its tooltip is the indicator's title on Linux.
    let clicks = self.os != "linux";
    format!(
      "{{\"os\":{},\"sessionType\":{},\"desktopHint\":{},\"sessionBus\":{},\
       \"trayHost\":{},\"trayReason\":{},\"trayClicks\":{clicks},\
       \"trayTooltip\":true,\"secretService\":{},\"secretServicePrompt\":{},\
       \"portalVersions\":{{{portals}}},\"cookieEncryption\":null}}",
      quote(self.os),
      opt(&self.session_type),
      opt(&self.desktop_hint),
      self.session_bus,
      self.tray_host(),
      opt(&self.tray_reason()),
      quote(self.secret_service),
      self.secret_prompter,
    )
  }
}

/// The session type: XDG_SESSION_TYPE as set, `"unknown"` when unset. Never
/// guessed from `$DISPLAY` / `$WAYLAND_DISPLAY`: a display alone (Xvfb,
/// `xvfb-run` under cron or a systemd service, a forwarded X connection)
/// says nothing about who is in front of it.
#[cfg(any(target_os = "linux", test))]
fn session_type(xdg_session_type: Option<&str>) -> String {
  xdg_session_type
    .filter(|t| !t.is_empty())
    .unwrap_or("unknown")
    .to_string()
}

/// Whether someone could answer a prompt (a keyring unlock) here: the
/// session type is x11 or wayland, a display variable is set, and logind,
/// where it can say (`logind`: `Some((type, active))` for this process's
/// session), agrees it is an active x11 / wayland session.
#[cfg(any(target_os = "linux", test))]
fn graphical_session(
  session_type: &str,
  have_display: bool,
  logind: Option<(String, bool)>,
) -> bool {
  if !matches!(session_type, "x11" | "wayland") || !have_display {
    return false;
  }
  match logind {
    Some((t, active)) => active && matches!(t.as_str(), "x11" | "wayland"),
    None => true,
  }
}

/// Probe this session.
pub fn probe() -> PlatformFeatures {
  #[cfg(target_os = "linux")]
  {
    linux::probe()
  }
  #[cfg(not(target_os = "linux"))]
  {
    PlatformFeatures {
      os: if cfg!(target_os = "macos") {
        "macos"
      } else if cfg!(target_os = "windows") {
        "windows"
      } else {
        "unknown"
      },
      secret_service: "os",
      secret_prompter: true,
      ..Default::default()
    }
  }
}

/// Whether a tray icon can be seen here, and why not. Cheaper than
/// [`probe`]: no Secret Service or portal calls.
pub fn tray_unavailable_reason() -> Option<String> {
  #[cfg(target_os = "linux")]
  {
    linux::probe_tray().tray_reason()
  }
  #[cfg(not(target_os = "linux"))]
  {
    None
  }
}

/// Say once on stderr why a tray icon was refused.
pub fn log_tray_refused(reason: &str) {
  static ONCE: std::sync::Once = std::sync::Once::new();
  ONCE.call_once(|| eprintln!("laufey: tray icon refused: {reason}"));
}

#[cfg(target_os = "linux")]
mod linux {
  use std::collections::{BTreeMap, HashSet};
  use std::time::Duration;

  use zbus::blocking::Connection;

  use super::PlatformFeatures;

  const TIMEOUT: Duration = Duration::from_millis(1000);
  const WATCHER: &str = "org.kde.StatusNotifierWatcher";
  const SECRETS: &str = "org.freedesktop.secrets";
  const PORTAL: &str = "org.freedesktop.portal.Desktop";
  const PORTAL_INTERFACES: [&str; 4] =
    ["Notification", "FileChooser", "GlobalShortcuts", "Settings"];

  fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
  }

  fn has_session_bus_address() -> bool {
    if env("DBUS_SESSION_BUS_ADDRESS").is_some() {
      return true;
    }
    use std::os::unix::fs::FileTypeExt;
    env("XDG_RUNTIME_DIR")
      .and_then(|dir| std::fs::metadata(format!("{dir}/bus")).ok())
      .is_some_and(|m| m.file_type().is_socket())
  }

  fn connect() -> Option<Connection> {
    if !has_session_bus_address() {
      return None;
    }
    zbus::blocking::connection::Builder::session()
      .ok()?
      .method_timeout(TIMEOUT)
      .build()
      .ok()
  }

  fn names(conn: &Connection, method: &str) -> HashSet<String> {
    conn
      .call_method(
        Some("org.freedesktop.DBus"),
        "/org/freedesktop/DBus",
        Some("org.freedesktop.DBus"),
        method,
        &(),
      )
      .ok()
      .and_then(|m| m.body().deserialize::<Vec<String>>().ok())
      .map(|v| v.into_iter().collect())
      .unwrap_or_default()
  }

  fn has_owner(conn: &Connection, name: &str) -> bool {
    conn
      .call_method(
        Some("org.freedesktop.DBus"),
        "/org/freedesktop/DBus",
        Some("org.freedesktop.DBus"),
        "NameHasOwner",
        &(name,),
      )
      .ok()
      .and_then(|m| m.body().deserialize::<bool>().ok())
      .unwrap_or(false)
  }

  /// A property, read only from a service that is running (the callers
  /// check), so the call never starts one.
  fn get_property<T>(
    conn: &Connection,
    dest: &str,
    path: &str,
    iface: &str,
    prop: &str,
  ) -> Option<T>
  where
    T: TryFrom<zbus::zvariant::OwnedValue>,
  {
    let value: zbus::zvariant::OwnedValue = conn
      .call_method(
        Some(dest),
        path,
        Some("org.freedesktop.DBus.Properties"),
        "Get",
        &(iface, prop),
      )
      .ok()?
      .body()
      .deserialize()
      .ok()?;
    T::try_from(value).ok()
  }

  fn session_type() -> String {
    super::session_type(env("XDG_SESSION_TYPE").as_deref())
  }

  /// This process's logind session (XDG_SESSION_ID's, else the process's
  /// own) as `(Type, Active)`, or `None` when logind can't say (no system
  /// bus, no logind, no session: a systemd user service, a container).
  /// Property reads on the system bus; never starts logind.
  fn logind_session() -> Option<(String, bool)> {
    let conn = zbus::blocking::connection::Builder::system()
      .ok()?
      .method_timeout(TIMEOUT)
      .build()
      .ok()?;
    let manager = "/org/freedesktop/login1";
    let iface = Some("org.freedesktop.login1.Manager");
    let dest = Some("org.freedesktop.login1");
    let reply = match env("XDG_SESSION_ID") {
      Some(id) => conn.call_method(dest, manager, iface, "GetSession", &(id,)),
      None => conn.call_method(
        dest,
        manager,
        iface,
        "GetSessionByPID",
        &(std::process::id(),),
      ),
    }
    .ok()?;
    let path: zbus::zvariant::OwnedObjectPath =
      reply.body().deserialize().ok()?;
    let get = |prop: &str| {
      conn
        .call_method(
          Some("org.freedesktop.login1"),
          path.as_str(),
          Some("org.freedesktop.DBus.Properties"),
          "Get",
          &("org.freedesktop.login1.Session", prop),
        )
        .ok()?
        .body()
        .deserialize::<zbus::zvariant::OwnedValue>()
        .ok()
    };
    let session_type = String::try_from(get("Type")?).ok()?;
    let active = bool::try_from(get("Active")?).ok()?;
    Some((session_type, active))
  }

  fn xembed_tray() -> bool {
    use x11rb::connection::Connection as _;
    use x11rb::protocol::xproto::ConnectionExt as _;
    if env("DISPLAY").is_none() {
      return false;
    }
    let Ok((conn, screen)) = x11rb::connect(None) else {
      return false;
    };
    let name = format!("_NET_SYSTEM_TRAY_S{screen}");
    let Some(atom) = conn
      .intern_atom(true, name.as_bytes())
      .ok()
      .and_then(|c| c.reply().ok())
      .map(|r| r.atom)
    else {
      return false;
    };
    if atom == x11rb::NONE {
      return false;
    }
    let owner = conn
      .get_selection_owner(atom)
      .ok()
      .and_then(|c| c.reply().ok())
      .map(|r| r.owner)
      .unwrap_or(x11rb::NONE);
    let _ = conn.flush();
    owner != x11rb::NONE
  }

  pub(super) fn probe_tray() -> PlatformFeatures {
    let conn = connect();
    PlatformFeatures {
      os: "linux",
      session_type: Some(session_type()),
      desktop_hint: env("XDG_CURRENT_DESKTOP"),
      session_bus: conn.is_some(),
      tray_watcher: conn.as_ref().is_some_and(|c| has_owner(c, WATCHER)),
      tray_xembed: xembed_tray(),
      secret_service: "absent",
      secret_prompter: false,
      portal_versions: BTreeMap::new(),
    }
  }

  pub(super) fn probe() -> PlatformFeatures {
    let mut f = probe_tray();
    let session = f.session_type.clone().unwrap_or_default();
    let have_display =
      env("WAYLAND_DISPLAY").is_some() || env("DISPLAY").is_some();
    let graphical = super::graphical_session(
      &session,
      have_display,
      if matches!(session.as_str(), "x11" | "wayland") && have_display {
        logind_session()
      } else {
        None
      },
    );
    let Some(conn) = connect() else {
      f.secret_service = "no-session-bus";
      return f;
    };
    let owned = names(&conn, "ListNames");
    let activatable = names(&conn, "ListActivatableNames");
    let known = |n: &str| owned.contains(n) || activatable.contains(n);
    f.secret_service = if owned.contains(SECRETS) {
      match get_property::<bool>(
        &conn,
        SECRETS,
        "/org/freedesktop/secrets/aliases/default",
        "org.freedesktop.Secret.Collection",
        "Locked",
      ) {
        Some(false) => "available",
        _ => "locked",
      }
    } else if activatable.contains(SECRETS) {
      "activatable"
    } else {
      "absent"
    };
    let needs_gcr = known("org.gnome.keyring");
    f.secret_prompter =
      graphical && (!needs_gcr || known("org.gnome.keyring.SystemPrompter"));
    if known(PORTAL) {
      for iface in PORTAL_INTERFACES {
        if let Some(v) = get_property::<u32>(
          &conn,
          PORTAL,
          "/org/freedesktop/portal/desktop",
          &format!("org.freedesktop.portal.{iface}"),
          "version",
        ) {
          f.portal_versions.insert(iface.to_string(), v);
        }
      }
    }
    f
  }
}

#[cfg(test)]
mod tests {
  use super::*;

  #[test]
  fn session_type_from_env() {
    assert_eq!(session_type(Some("wayland")), "wayland");
    assert_eq!(session_type(Some("tty")), "tty");
    assert_eq!(session_type(Some("mir")), "mir");
    // Never from the display variables (Xvfb, cron, xvfb-run under
    // systemd): unset is unknown.
    assert_eq!(session_type(Some("")), "unknown");
    assert_eq!(session_type(None), "unknown");
  }

  #[test]
  fn graphical_only_by_session_type_and_logind() {
    // $DISPLAY alone (XDG_SESSION_TYPE unset): no one to answer a prompt.
    assert!(!graphical_session("unknown", true, None));
    assert!(!graphical_session("tty", true, None));
    // x11 / wayland with a display; logind can't say: trusted.
    assert!(graphical_session("x11", true, None));
    assert!(graphical_session("wayland", true, None));
    assert!(!graphical_session("wayland", false, None));
    // logind says: an active x11 / wayland session only.
    assert!(graphical_session("x11", true, Some(("x11".into(), true))));
    assert!(!graphical_session("x11", true, Some(("tty".into(), true))));
    assert!(!graphical_session(
      "wayland",
      true,
      Some(("wayland".into(), false))
    ));
  }

  #[test]
  fn tray_reason_and_json() {
    let mut f = PlatformFeatures {
      os: "linux",
      session_type: Some("wayland".into()),
      desktop_hint: Some("GNOME".into()),
      session_bus: true,
      secret_service: "available",
      secret_prompter: true,
      ..Default::default()
    };
    assert!(!f.tray_host());
    let reason = f.tray_reason().unwrap();
    assert!(reason.contains("org.kde.StatusNotifierWatcher"));
    assert!(reason.contains("XDG_CURRENT_DESKTOP=GNOME"));
    assert!(!reason.contains("XEmbed"));
    f.session_type = Some("x11".into());
    assert!(f.tray_reason().unwrap().contains("XEmbed"));
    f.tray_xembed = true;
    assert!(f.tray_host() && f.tray_reason().is_none());
    f.tray_xembed = false;
    f.tray_watcher = true;
    f.portal_versions.insert("Settings".into(), 2);
    f.portal_versions.insert("FileChooser".into(), 4);
    assert_eq!(
      f.to_json(),
      "{\"os\":\"linux\",\"sessionType\":\"x11\",\"desktopHint\":\"GNOME\",\
       \"sessionBus\":true,\"trayHost\":true,\"trayReason\":null,\
       \"trayClicks\":false,\"trayTooltip\":true,\
       \"secretService\":\"available\",\"secretServicePrompt\":true,\
       \"portalVersions\":{\"FileChooser\":4,\"Settings\":2},\
       \"cookieEncryption\":null}"
    );
    let mac = PlatformFeatures {
      os: "macos",
      secret_service: "os",
      secret_prompter: true,
      ..Default::default()
    };
    assert!(mac.tray_host());
    assert!(mac.to_json().contains("\"trayClicks\":true"));
    assert!(mac.to_json().contains("\"sessionType\":null"));
  }
}
