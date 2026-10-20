# Soundbending Live for Mac

A native version of Soundbending Live (formerly 2110 DAW), built with [JUCE](https://juce.com) so it isn't limited by the browser:
no 32-channel cap, low latency, and (coming next) VST3/AU plugins.

## What works now

- Pick any Core Audio interface, sample rate and buffer size (**Audio settings**). Every input of the interface becomes a track, up to 256.
- Name tracks (Tab or Enter moves to the next name), arm them, and watch input meters with peak hold and a clip light (click a meter to reset it).
- **Record** (or press `R`) writes each armed track as a 24-bit mono WAV, streamed to disk on a background thread. Files are named like the web version: `T03_01_Kick.wav`. Takes continue numbering from what's already in the folder.
- **Save folder** picks where takes go (default `~/Music/Soundbending Live`, or `~/Music/2110 DAW` if you already used it); the bar shows free space and roughly how many hours fit.
- **Import names / Export names** use the same `Channel,Name` CSV as the web version.
- If the disk can't keep up, the app says so after the take instead of failing silently.

Coming next: the Live console (gain, plugins, faders, pan, mute/solo and a stereo mix out), VST3/AU plugins, playback and Soundcheck.

## Download

Each change to `mac/` on `main` is built by GitHub Actions and published to the
[mac-latest release](https://github.com/stefanhoagland/2110-DAW/releases/tag/mac-latest).
Unzip, move **Soundbending Live** to Applications, then **right-click it and choose Open** the first
time (the app isn't notarized by Apple). If macOS still refuses, run
`xattr -dr com.apple.quarantine "/Applications/Soundbending Live.app"` in Terminal. Allow microphone
access when asked; macOS uses that permission for audio interfaces too.

## Build it yourself

Needs Xcode command line tools and CMake 3.22+ (`brew install cmake`).

```sh
cmake -S mac -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target DAW2110 -j 8
open "build/DAW2110_artefacts/Release/Soundbending Live.app"
```

`NamesTest` and `EngineTest` are small test programs (the engine test records through a pretend 64-input interface and checks the files).

JUCE is used under its AGPLv3 option, which fits this open-source project.
