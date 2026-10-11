# Security Policy

This repository is Brainwires' fork of
[laufey](https://github.com/littledivy/laufey), maintained as the web-engine
backend of the denext Deno Desktop runtime
([Brainwires/deno](https://github.com/Brainwires/deno), the `2.9.7-denext.N`
releases).

## Reporting a vulnerability

Please report vulnerabilities **privately** through GitHub's private
vulnerability reporting:
[open a security advisory](https://github.com/Brainwires/laufey/security/advisories/new).

**Please do not open security issues in the public issue tracker, and do not
open public pull requests that disclose an unfixed vulnerability.**

Include as much as you can: the backend (CEF, WebView, Winit), the operating
system and architecture, the laufey commit or the denext runtime release (for
example `2.9.7-denext.13`) it shipped in, steps to reproduce, and the impact you
observed. We will acknowledge a report within 3 business days and keep you
informed while we work on a fix. We are happy to credit you in the advisory once
the issue is resolved.

Vulnerabilities in Chromium itself should go to the
[Chromium security team](https://www.chromium.org/Home/chromium-security/reporting-security-bugs/);
vulnerabilities in the system WebViews (WKWebView, WebView2, WebKitGTK) to their
vendors. Tell us too if a laufey release is affected, so we can ship the fixed
engine.

## Supported versions

Only the laufey revision built into the current denext runtime release receives
security fixes. Fixes land on this repository and ship in a new
`denext-runtime-v<deno>-denext.<N>` release of
[Brainwires/deno](https://github.com/Brainwires/deno/releases).

| Version                                                     | Supported |
| ----------------------------------------------------------- | --------- |
| The revision in the latest `2.9.7-denext.N` runtime release | Yes       |
| Older revisions and crates.io releases of upstream laufey   | No        |

## How we track upstream

- The CEF backend pins a CEF / Chromium build; we follow Chromium and CEF
  security releases and bump the pin.
- A scheduled `cargo audit` workflow (`.github/workflows/cargo_audit.yml`)
  checks `Cargo.lock` against the RustSec advisory database every week.
