# Soundbending Live

(Formerly 2110 DAW.) A browser-based digital audio workstation with three modes:

- **Studio**: multitrack recording now; effects and mixdown for post production next.
- **Soundcheck** (planned): play recorded tracks back out to separate outputs on your interface so each lands on its own console channel for a virtual soundcheck.
- **Live**: a mixing console for the live inputs, with EQ, compression and a stereo mix you can send to your streaming software.

**Mac app:** a native version (no browser limits) is being built in [`mac/`](mac/README.md); download it from the [mac-latest release](https://github.com/stefanhoagland/2110-DAW/releases/tag/mac-latest).

## What works today: 32-channel recorder

- Pick your audio interface and sample rate.
- 32 tracks with names, record arm, level meters with dB tick marks and a peak readout in dBFS (click to reset), clip light, volume, pan, mute and solo.
- A stereo **master meter** (left/right, with dB scale and peak readouts) next to the master volume slider shows the playback mix level.
- Per-track **volume** (wider slider) and **pan** (smaller slider) only affect playback, never what is recorded. Hover to see the value; double-click to reset to 0 dB or center.
- Record all armed tracks at once, sample-aligned, as **24-bit mono WAV files, one per channel**.
- **Choose save folder** (Chrome or Edge) streams every track straight to disk while recording, so long takes don't fill memory. Without a folder, takes stay in memory and **Download WAVs** saves them.
- Timeline with waveforms, zoom, click-to-locate and playback of the last take.

- **Import names / Export names** load and save channel names as a CSV file with `Channel,Name` columns (a plain list of names, one per line, also works). Handy for keeping a channel list per show or copying it from your console's input list.
- Click a clip to select it and press `Delete` (or the Delete button) to remove it. If it was saved to a folder, the WAV file is moved into a `Deleted` subfolder there, so it can be recovered.

### Timecode

Pick a source in **Timecode** (next to the input settings); the choice is remembered:

- **LTC on input N**: SMPTE linear timecode arriving as audio on one input channel of your interface (from a timecode generator, a video deck or another recorder). The frame rate is detected (23.976, 24, 25, 29.97, 30, and 29.97 drop-frame).
- **MTC: <MIDI input>**: MIDI timecode from a MIDI interface (Chrome/Edge; the browser asks for MIDI access).

The top-bar clock then shows timecode (`HH:MM:SS:FF`, `;` before the frames for drop-frame), with the source and rate underneath. When nothing arrives for half a second it shows **no signal**, dims and keeps running from the last value; while recording, the take's length shows next to the take info. Set it back to **Off** for the normal clock.

When timecode is locked as a take starts, every WAV of that take is a **Broadcast Wave** file stamped with the timecode of its first sample (the `bext` time reference, plus `sTC=… fps=…` in the description), so Pro Tools, Reaper, Logic and similar can spot the files to their original timecode position. Without timecode the files are plain WAVs as before.

Keyboard: while typing a channel name, `Tab` or `Enter` jumps to the next channel's name (`Shift+Tab` goes back). `R` record, `Space` play/stop, `Home` or `Enter` return to zero, `Delete` remove the selected clip, `Esc` clear the selection.

## Live mode

The **Live** tab is a mixing console for the inputs, for remixing a live source for a stream:

- One strip per input with gain, plugin slots, pan, a fader with a post-fader meter, mute and solo. Settings are remembered. Double-click any control to reset it.
- **Plugins:** use **+ Plugin** on a strip to insert High-pass, Gate, EQ (low shelf, parametric mid, high shelf), Compressor, Limiter, Delay or Reverb, as many as you like, processed top to bottom. Click a plugin to open its editor, where you can adjust it, bypass it, move it earlier or later in the chain, or remove it. Compressor and Limiter show their gain reduction.
- **Third-party plugins:** browsers can't load VST/AU plugins, but they can load **Web Audio Modules (WAM 2.0)**, plugins built for the web (see [webaudiomodules.com](https://www.webaudiomodules.com)). In **+ Plugin**, pick **Load plugin from URL…** and paste the address of the plugin's `index.js`. If the plugin has its own interface it shows in the editor; otherwise you get a slider per setting. Its settings are saved with the strip, and plugins you've loaded stay in the **+ Plugin** menu.
  - The Docker image comes with four free ones ready in **+ Plugin**: Simple EQ, Simple Delay, Simple Distortion and Microverb (from [burns-audio-wam](https://github.com/boourns/burns-audio-wam), MIT License). For local development, `npm run plugins` downloads them.
  - Plugins hosted on another website only load if that site allows it (CORS). Otherwise download the plugin and put its folder in the server's `plugins` folder (`/mnt/user/appdata/2110-daw/plugins` on Unraid; the folder must contain `index.js`). Everything there shows up in **+ Plugin** automatically. When running locally with `npm start`, use a `plugins` folder in the project.
  - A plugin is code that runs inside the app, so only load plugins you trust.
- **Go live** turns the live mix on (it always starts off, so opening the page never causes feedback).
- **Output** sends the whole mix to another audio device. Pick a virtual cable (BlackHole on macOS, VB-CABLE on Windows) and select that cable as the audio source in OBS to stream the mix.
- **Master limiter** keeps the stream from clipping. The master fader and meter in the top bar show the final output.
- You can record the multitrack in Studio while the live mix is running; both use the same inputs.

The browser adds some delay (the bar shows an estimate, usually 20–60 ms). That's fine for a stream but too much for in-ear monitoring.

## Running it

Browsers only allow microphone and interface access from `https://` or `localhost`, so open it through a local server:

```sh
npm start            # serves on http://localhost:8110
```

(Any static server works, for example `python3 -m http.server`.) You can also publish the repo with GitHub Pages and use it from there.

## Running on Unraid (Docker)

Every push to `main` builds a Docker image at `ghcr.io/stefanhoagland/2110-daw:latest`. It serves the app over HTTPS with a self-signed certificate, because browsers block audio interfaces on plain HTTP unless the page is on `localhost`.

1. In Unraid, open a terminal and download the template:
   ```sh
   wget -O /boot/config/plugins/dockerMan/templates-user/my-2110-DAW.xml \
     https://raw.githubusercontent.com/stefanhoagland/2110-DAW/main/unraid/2110-daw.xml
   ```
2. Go to **Docker → Add Container**, pick **2110-DAW** from the Template list.
3. Set **Certificate hostnames** to your server's IP (and hostname if you use one), e.g. `192.168.1.50,tower.local`. Leave the ports as 8443 (HTTPS) and 8110 (HTTP) unless they clash. Click **Apply**.
4. On the computer with your audio interface, open `https://<server-ip>:8443` in Chrome. Chrome warns about the self-signed certificate the first time: click **Advanced → Proceed**.

The certificate is stored in `/mnt/user/appdata/2110-daw/certs`. To use a real certificate, replace `cert.pem` and `key.pem` there and restart the container. If you already run a reverse proxy with HTTPS (Nginx Proxy Manager, SWAG), point it at the HTTP port instead.

The audio still runs in your browser, on the computer the interface is plugged into; the server only hosts the app. Recordings save to that computer (or wherever the save folder points), not to the server.

Without the template, the equivalent command is:

```sh
docker run -d --name 2110-daw -p 8443:443 -p 8110:80 \
  -e CERT_HOSTNAMES=192.168.1.50 -v /mnt/user/appdata/2110-daw:/config \
  ghcr.io/stefanhoagland/2110-daw:latest
```

## Getting all 32 channels into the browser

Browsers see an audio interface the same way they see a microphone, and some drivers only hand the browser the first 2 channels. The app shows how many channels it actually received next to the input picker; tracks without an input record silence.

- Use **Chrome**, which supports multichannel capture.
- The interface needs to appear to the computer as **one device with up to 32 inputs**. On macOS, check the input count in Audio MIDI Setup; an Aggregate Device can combine inputs. On Windows, a class-compliant or WASAPI driver that exposes all inputs as one device works best; ASIO-only drivers are not visible to browsers.
- Turn off other apps holding the interface in exclusive mode.

## Development

```sh
npm test               # WAV/BWF encoding and timecode decoding unit tests (Node)
npm run test:browser   # records 2 seconds in headless Chromium with a fake mic (needs Playwright)
```

No build step: plain HTML, CSS and JavaScript modules.

| File | Purpose |
| --- | --- |
| `js/main.js` | UI, tracks, transport, timeline drawing |
| `js/audio-engine.js` | Input device, recording control, playback |
| `js/recorder-worklet.js` | Audio-thread processor: splits channels, meters, captures samples |
| `js/storage.js` | Streaming WAV writer for a chosen folder, in-memory fallback |
| `js/wav.js` | WAV header (with optional BWF `bext` timecode stamp) and 24-bit encoding |
| `js/timecode.js` | LTC decoder, frame-rate detection, MTC parser, timecode math (no browser APIs) |
| `js/timecode-source.js` | Timecode setting, MIDI input, the running timecode for the clock and recordings |

## Roadmap

1. 32-channel recorder ✅ (32 is Chrome's per-device limit; 128 would need several devices combined or a desktop app)
2. Soundcheck mode: recorded tracks out to separate interface outputs
3. Studio mode: per-track effects (EQ, compressor, gate, reverb) and mixdown export
4. Live mode ✅ (first version: console with EQ, compressor, output choice and limiter)
