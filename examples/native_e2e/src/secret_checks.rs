//! The secure store (API 47; LAUFEY_E2E_ONLY=secret-store,
//! scripts/native-e2e-run.sh --secret-store).
//!
//! On Linux (CEF and WebView) every call answers within its timeout: a
//! value, "not found", or "unavailable" with a reason (no provider, a locked
//! keyring no one unlocked). Never a hang, and never a bad-arguments failure
//! for good arguments. Elsewhere (macOS, Windows, Winit) the entry points are
//! NULL. Expectations for a real session (LAUFEY_E2E_HOST_SESSION=1):
//!
//! ```text
//! LAUFEY_E2E_EXPECT_SECRET=ok           a full round trip works here
//! LAUFEY_E2E_EXPECT_SECRET=unavailable  every call is refused, with a reason
//! LAUFEY_E2E_EXPECT_SECRET_REASON="…"   a substring of that reason
//! LAUFEY_E2E_SECRET_TIMEOUT_MS=5000     the bound passed to each call
//! ```

use std::time::{Duration, Instant};

use laufey::SecretError;

use crate::{check, na};

fn expect(name: &str) -> Option<String> {
  std::env::var(name).ok().filter(|v| !v.is_empty())
}

pub(crate) async fn run() {
  if !laufey::secret_store_supported() {
    check(
      "no secure store off Linux (CEF / WebView)",
      !cfg!(target_os = "linux")
        || std::env::var("LAUFEY_E2E_BACKEND").as_deref() == Ok("winit"),
    );
    na("the secure store (this backend has none)");
    return;
  }
  let timeout = Duration::from_millis(
    expect("LAUFEY_E2E_SECRET_TIMEOUT_MS")
      .and_then(|v| v.parse().ok())
      .unwrap_or(5000),
  );
  let service = "dev.laufey.e2e";
  let account = format!("e2e-{}", std::process::id());
  // Off the async runtime's worker: each call blocks.
  let (a, t) = (account.clone(), timeout);
  let start = Instant::now();
  let first =
    tokio::task::spawn_blocking(move || laufey::secret_lookup(service, &a, t))
      .await
      .expect("the lookup thread");
  let took = start.elapsed();
  eprintln!("[e2e] secret lookup ({took:?}): {first:?}");
  check(
    "a lookup answers within its timeout",
    took < timeout + Duration::from_secs(3),
  );
  check(
    "a lookup of a fresh account is \"not found\" or \"unavailable\" with a \
     reason",
    match &first {
      Ok(None) => true,
      Err(SecretError::Unavailable(r)) => !r.is_empty(),
      _ => false,
    },
  );
  let want = expect("LAUFEY_E2E_EXPECT_SECRET");
  if let Err(SecretError::Unavailable(reason)) = &first {
    check(
      "unavailable here as expected",
      want.as_deref().is_none_or(|w| w == "unavailable"),
    );
    if let Some(part) = expect("LAUFEY_E2E_EXPECT_SECRET_REASON") {
      check(&format!("the reason says {part:?}"), reason.contains(&part));
    }
    let a = account.clone();
    let stored = tokio::task::spawn_blocking(move || {
      laufey::secret_store(service, &a, "laufey e2e", "v", t)
    })
    .await
    .expect("the store thread");
    check(
      "a write is refused too (never a plaintext fallback)",
      matches!(stored, Err(SecretError::Unavailable(_))),
    );
    return;
  }
  check(
    "the store answers here as expected",
    want.as_deref().is_none_or(|w| w == "ok"),
  );
  let a = account.clone();
  let round = tokio::task::spawn_blocking(move || {
    let text = "e2e \u{2713} \"quoted\"\nline 2";
    let stored = laufey::secret_store(service, &a, "laufey e2e", "one", t)
      .and_then(|_| laufey::secret_store(service, &a, "laufey e2e", text, t));
    let read = laufey::secret_lookup(service, &a, t);
    let deleted = laufey::secret_delete(service, &a, t);
    let after = laufey::secret_lookup(service, &a, t);
    let again = laufey::secret_delete(service, &a, t);
    (stored, read, deleted, after, again, text)
  })
  .await
  .expect("the round-trip thread");
  let (stored, read, deleted, after, again, text) = round;
  check("store (and replace) succeed", stored.is_ok());
  check(
    "the lookup returns the last value",
    read == Ok(Some(text.to_string())),
  );
  check("delete succeeds", deleted.is_ok());
  check("a deleted secret is \"not found\"", after == Ok(None));
  check("deleting nothing succeeds", again.is_ok());
}
