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
- **Live** tab: 128 console strips (one per track, patched or not) with gain, pan, fader, meter, mute and solo, plus a master fader and stereo meter in the top bar. The mix always goes to the outputs it is patched to (no Go live step), through an optional master limiter (ceiling -1 dB). Double-click a control to reset it. The meter shows the level before the fader, so it keeps moving when the fader is down, muted or soloed out; only the audio stops. Tracks you haven't renamed take the name of the AES67 channel that feeds them. Settings are remembered.

- **Patch** tab, *Inputs to tracks* has two views. **List** (like Dante Controller's Receive tab): your tracks on the left with what feeds each one, and every available channel on the right (interface inputs, received AES67 streams, and announced streams not received yet), with a **Filter** box. Drag a channel, several, or a whole stream onto a track to patch them there and onto the tracks after it (an announced stream starts receiving when you drop it). Double-click a channel to patch it to the selected track; Delete or **Unpatch** clears a track. **Grid** is the XY crosspoint view.
- **Patch** tab grids: XY crosspoint grids. *Inputs to tracks* picks which interface input feeds each track (Studio records and Live mixes the tracks). *Outputs* sends the live mix L/R or any track's direct input to any interface outputs. Click a cell to connect, drag across cells to do several, **Reset to 1:1** for the default. The patch applies to every tab and is remembered.

- **Buses**: 12 mono aux buses and 8 stereo mains. Each channel has **Mains 1-8** buttons (main 1 is on by default; main 1 is the master, with the master fader and limiter). Pick **Faders: Aux N sends** in the Live bar to set every channel's send to that aux with the channel faders, with a **Pre fader** switch per channel (on by default, for monitor mixes; solo never changes an aux). **Buses** shows the aux and main masters with fader, meter and mute. Patch any bus to any output in **Patch > Outputs**. Name the buses in the **Bus names** tab (or double-click a bus name); the names show on the bus faders, the Faders menu and in Patch, and are saved with the show.
- **Plugins**: VST3 and Audio Unit effects on each Live strip, between gain and pan (up to 8 per channel). Click **Plugins...** in the Live bar, then **Options > Scan for new or updated VST3/AudioUnit plug-ins** the first time. **+ Plugin** adds one; click it to open its window, right-click to bypass, move or remove it (Alt-click also toggles bypass). Plugins run in mono when they support it, otherwise in stereo with the two sides averaged back to the channel. Every plugin and its settings are saved and come back next launch.

- **Timecode**: click **Timecode** in the setup bar to follow external timecode, either LTC on one of your interface inputs or MIDI timecode (MTC) from a MIDI input. The frame rate (23.976, 24, 25, 29.97, 29.97 drop-frame, 30) is detected automatically. The clock then shows the timecode (dimmed with "no signal" if it stops), and every take is stamped with the timecode of its first sample as a Broadcast WAV, so Pro Tools, Logic, Reaper and others can place the files at the right time.

- **Shows**: the **Show** button (left of Audio settings) saves everything about a gig in one `.slshow` file: track names and arming, the input and output patch, the AES67 streams, the live mix, every plugin with its settings, and the timecode source. **Save show** (Cmd-S), **Save show as...** (Shift-Cmd-S) and **Open show...** (Cmd-O). The app still remembers where you left off when you quit; a show is a snapshot you can go back to. The audio interface and save folder belong to the computer, so they aren't in the show.
- Track names follow the patch: patching or unpatching a track names it after its new source ("Ch N" when nothing is patched). Type a name to keep it until you patch that track again.
- **Usage readout** in the bar under the transport: **Audio** is how much of each audio buffer's time the processing takes (it turns orange at 70% and red at 90%; near 100% you get dropouts, so raise the buffer size in Audio settings), **CPU** is the app's share of all your Mac's cores, and **Memory** is what the app holds, the same figure Activity Monitor shows. It hides on a narrow window.
- **AES67 network audio**: click **AES67 network** in the setup bar to receive AES67 / SMPTE ST 2110-30 streams with no driver to install. Pick the network port on your media network; streams announced with SAP are listed automatically, or paste/load a stream's SDP. Tick a stream and its channels become tracks after your interface's inputs (up to 128 network channels), ready to record, mix and patch. Uncompressed L16/L24 at any packet time is supported. The app follows each stream's clock by resampling gently against your interface, so PTP isn't needed to receive; the **Buffer** setting (5 ms by default) trades latency for resilience, and a stream that drops out gets 2 ms more each time (up to 40 ms more). **Copy report** copies each stream's packet timing, lost packets and dropouts for troubleshooting. Use a wired network port: Wi-Fi delivers multicast in bursts that cause dropouts.
- **AES67 sending**: in the AES67 window, the **Send** tab creates streams the app sends. Give one a name, pick the channel count (1-64), the packet time (1 ms for up to 8 channels, which is what Dante in AES67 mode takes; 250 µs up to 32; 125 µs up to 64), a multicast address (a free one is filled in) and what feeds it (a main, an aux, or tracks), then **Create stream**. Its channels appear as outputs in **Patch > Outputs**, so any main, aux or track can be patched to any of them. Streams are L24 at 48 kHz, announced with SAP (so Dante Controller and other receivers list them) and **Copy SDP** gives the SDP for receivers that need it pasted. The app follows the network's PTP grandmaster (PTPv2, the domain is set in the Send tab; it never becomes a master itself) and stamps the audio with PTP time (`mediaclk:direct=0`), resampling from your interface's clock, so the audio interface can run at any rate. Without a grandmaster the streams still go out on the Mac's own clock, but most AES67 gear won't play them. Streams are saved with the settings and in shows.

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
