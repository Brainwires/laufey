# Text to speech

From API 50 the CEF and WebView backends speak through the operating system's
engine, independent of the web engine (WebKitGTK has no Web Speech, and an app
may want speech from its runtime without a page):

```rust
use laufey::{speak, stop_speaking, SpeechOutcome, SpeechRequest};

let id = speak(
  &SpeechRequest {
    text: "Hello".into(),
    rate: Some(1.2),   // 0.1..10, 1 is normal
    pitch: Some(1.0),  // 0..2
    volume: Some(0.8), // 0..1
    voice: None,       // an id from speech_voices_json()
    lang: Some("en-US".into()),
  },
  |outcome| println!("{outcome:?}"), // exactly once
);
stop_speaking(); // the utterance reports Cancelled
```

- One utterance at a time: a new `speak` cancels the one speaking (its callback
  reports `Cancelled`). An empty or over-32-KiB text is refused (id 0, no
  callback).
- `speech_voices_json()` lists the installed voices as
  `[{"id", "name", "lang", "default"}]`.
- **macOS**: `AVSpeechSynthesizer` (the rate scales
  `AVSpeechUtteranceDefaultSpeechRate`; the pitch multiplier is clamped to
  0.5..2).
- **Windows**: SAPI's `ISpVoice` on a worker thread of its own (rate −10..10 on
  a log scale, the pitch through SAPI XML's `<pitch absmiddle>`).
- **Linux**: speech-dispatcher through `libspeechd.so.2`, loaded at runtime (a
  package `Recommends`). Without it, or without a reachable server, the callback
  reports `Unsupported`; fall back to the page's `speechSynthesis` where the
  engine has one.
- The Winit backend has no speech (`Unsupported`).
