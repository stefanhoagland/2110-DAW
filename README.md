# 2110 DAW

A browser-based digital audio workstation with three modes:

- **Studio**: multitrack recording now; effects and mixdown for post production next.
- **Soundcheck** (planned): play recorded tracks back out to separate outputs on your interface so each lands on its own console channel for a virtual soundcheck.
- **Live** (planned): low-latency live input with real-time effects and a stereo mix for streaming.

## What works today: 16-channel recorder

- Pick your audio interface and sample rate.
- 16 tracks with names, record arm, level meters (peak hold and clip light), volume, pan, mute and solo.
- Record all armed tracks at once, sample-aligned, as **24-bit mono WAV files, one per channel**.
- **Choose save folder** (Chrome or Edge) streams every track straight to disk while recording, so long takes don't fill memory. Without a folder, takes stay in memory and **Download WAVs** saves them.
- Timeline with waveforms, zoom, click-to-locate and playback of the last take.

Keyboard: `R` record, `Space` play/stop, `Home` or `Enter` return to zero.

## Running it

Browsers only allow microphone and interface access from `https://` or `localhost`, so open it through a local server:

```sh
npm start            # serves on http://localhost:8110
```

(Any static server works, for example `python3 -m http.server`.) You can also publish the repo with GitHub Pages and use it from there.

## Getting all 16 channels into the browser

Browsers see an audio interface the same way they see a microphone, and some drivers only hand the browser the first 2 channels. The app shows how many channels it actually received next to the input picker; tracks without an input record silence.

- Use **Chrome**, which supports multichannel capture.
- The interface needs to appear to the computer as **one device with 16 inputs**. On macOS, check the input count in Audio MIDI Setup; an Aggregate Device can combine inputs. On Windows, a class-compliant or WASAPI driver that exposes all inputs as one device works best; ASIO-only drivers are not visible to browsers.
- Turn off other apps holding the interface in exclusive mode.

## Development

```sh
npm test               # WAV encoding unit tests (Node)
npm run test:browser   # records 2 seconds in headless Chromium with a fake mic (needs Playwright)
```

No build step: plain HTML, CSS and JavaScript modules.

| File | Purpose |
| --- | --- |
| `js/main.js` | UI, tracks, transport, timeline drawing |
| `js/audio-engine.js` | Input device, recording control, playback |
| `js/recorder-worklet.js` | Audio-thread processor: splits channels, meters, captures samples |
| `js/storage.js` | Streaming WAV writer for a chosen folder, in-memory fallback |
| `js/wav.js` | WAV header and 24-bit encoding |

## Roadmap

1. 16-channel recorder ✅
2. Soundcheck mode: 16 recorded tracks out to 16 interface outputs
3. Studio mode: per-track effects (EQ, compressor, gate, reverb) and mixdown export
4. Live mode: low-latency monitoring, real-time effects, stream mix output
