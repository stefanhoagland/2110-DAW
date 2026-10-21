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
- **Live** tab: a console strip per input with gain, pan, fader, post-fader meter, mute and solo, plus a master fader and stereo meter in the top bar. **Go live** sends the mix to the first two outputs chosen in Audio settings, through an optional master limiter (ceiling -1 dB). Double-click a control to reset it. Settings are remembered.

- **Patch** tab: XY crosspoint grids. *Inputs to tracks* picks which interface input feeds each track (Studio records and Live mixes the tracks). *Outputs* sends the live mix L/R or any track's direct input to any interface outputs. Click a cell to connect, drag across cells to do several, **Reset to 1:1** for the default. The patch applies to every tab and is remembered.

- **Plugins**: VST3 and Audio Unit effects on each Live strip, between gain and pan (up to 8 per channel). Click **Plugins...** in the Live bar, then **Options > Scan for new or updated VST3/AudioUnit plug-ins** the first time. **+ Plugin** adds one; click it to open its window, right-click to bypass, move or remove it (Alt-click also toggles bypass). Plugins run in mono when they support it, otherwise in stereo with the two sides averaged back to the channel. Every plugin and its settings are saved and come back next launch.

- **Timecode**: click **Timecode** in the setup bar to follow external timecode, either LTC on one of your interface inputs or MIDI timecode (MTC) from a MIDI input. The frame rate (23.976, 24, 25, 29.97, 29.97 drop-frame, 30) is detected automatically. The clock then shows the timecode (dimmed with "no signal" if it stops), and every take is stamped with the timecode of its first sample as a Broadcast WAV, so Pro Tools, Logic, Reaper and others can place the files at the right time.

- **AES67 network audio**: click **AES67 network** in the setup bar to receive AES67 / SMPTE ST 2110-30 streams with no driver to install. Pick the network port on your media network; streams announced with SAP are listed automatically, or paste/load a stream's SDP. Tick a stream and its channels become tracks after your interface's inputs (up to 128 network channels), ready to record, mix and patch. Uncompressed L16/L24 at any packet time is supported. The app follows each stream's clock by resampling gently against your interface, so PTP isn't needed to receive; the **Buffer** setting (5 ms by default) trades latency for resilience. Sending AES67 from the app needs PTP and is not built yet.

Coming next: playback and Soundcheck (its track playback will appear as sources in the Outputs grid).

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
