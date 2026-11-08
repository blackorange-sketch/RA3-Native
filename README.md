# RA3 Native

An experimental native Android port of **Command & Conquer: Red Alert 3** (patch 1.12), built by **static recompilation** of the original x86 executable into ARM64 code. No Winlator, no Box64, no emulator layer at run time.

> **No game files are included or distributed.** This repository contains only tools and a runtime. You must supply your own legally obtained copy of the game. Generated code is never committed.

## How it works

1. `tools/` – a static analyser and a lifter (built on [Capstone](https://www.capstone-engine.org/)) that read `ra3_1.12.game` and emit C++ for every function (`f_XXXXXXXX(CPU*)`), with registers held as host locals.
2. `runtime/` – the Android-side runtime that the lifted code links against:
   - PE loader and a 4 GB guest address window
   - Win32 / MSVCRT / DirectSound / network import layer
   - Direct3D 9 → OpenGL ES 3 renderer (including FX9 effects)
   - NEON implementations of the MMX helpers (used by the VP6 video decoder)
   - Touch input mapped to RTS mouse/keyboard controls
3. `android/` – the APK wrapper. The CI workflow builds the lifted code and the APK on GitHub Actions.

## Status

Working: 3D world rendering, resolution switching, minimap, touch controls (select / pan / rotate / zoom), video cutscenes at full speed.
In progress: faster startup, 60 fps rendering, widescreen UI scaling, missing graphics settings, cleanup of diagnostics.

See [`docs/PLAN.md`](docs/PLAN.md) for the analysis results and roadmap (the plan document is currently in Ukrainian).

## Using it

1. Install the APK from the `latest` GitHub release.
2. Place your own game install at `/sdcard/Command and Conquer - Red Alert 3/`.
3. Launch the app. A log is written to `/sdcard/ra3_native.log`.
4. Optional: create an empty file `/sdcard/ra3_play_movies` to enable video cutscenes.

## Building the analyser

```
g++ -O2 -std=c++17 -I<capstone>/include tools/analyze.cpp <capstone>/libcapstone.a -o analyze
./analyze ra3_1.12.game out
```

## Legal

This is an unofficial fan/research project, not affiliated with or endorsed by Electronic Arts. Red Alert 3 and Command & Conquer are trademarks of Electronic Arts Inc. Bring your own game files.
