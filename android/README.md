# OpenSparX Agent — Android APK

On-device AI Agent companion for Snapdragon 8 Gen 3 (and later) devices.

## What is this?

A floating "desktop pet" style AI agent that runs **entirely on your phone's NPU** — no cloud, no API keys, no data leaves your device.

Two modes:
- **Reactive**: Tap the floating bubble → ask anything → watch the DAG unfold
- **Proactive**: The agent observes context (time, location, motion, usage patterns) and proactively offers assistance

## Requirements

- Android 14+ device with Snapdragon 8 Gen 3 / 8 Elite / similar
- ~3GB free storage (for the on-device model)
- Overlay permission (for floating widget)

## Architecture

```
┌─ Floating Widget (always visible) ─────────────────────┐
│                                                         │
│  Tap → Agent Panel (DAG visualization, chat, status)    │
│                                                         │
├─ Signal Layer ──────────────────────────────────────────┤
│  Time │ Location │ Motion │ Screen │ Notifications      │
│                                                         │
├─ Agent Core (C++ via JNI) ──────────────────────────────┤
│  ProactiveEngine → Planner → Scheduler → Verifier       │
│                                                         │
├─ Inference (QNN Runtime, dlopen from /vendor) ──────────┤
│  Qwen3-4B INT4 │ KV Cache │ Speculative Execution       │
│                                                         │
└─ NPU: Hexagon (on Snapdragon) ─────────────────────────┘
```

## Build

```bash
cd android
./gradlew assembleDebug
adb install app/build/outputs/apk/debug/app-debug.apk
```

## License

Apache 2.0 — same as the main OpenSparX project.
Note: QNN Runtime is NOT redistributed. The app dynamically loads it from
the device's existing Qualcomm system libraries at runtime.
