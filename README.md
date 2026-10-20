# 2110 DAW

A browser-based digital audio workstation with three modes:

- **Studio**: multitrack recording now; effects and mixdown for post production next.
- **Soundcheck** (planned): play recorded tracks back out to separate outputs on your interface so each lands on its own console channel for a virtual soundcheck.
- **Live** (planned): low-latency live input with real-time effects and a stereo mix for streaming.

## What works today: 16-channel recorder

- Pick your audio interface and sample rate.
- 16 tracks with names, record arm, level meters with dB tick marks and a peak readout in dBFS (click to reset), clip light, volume, pan, mute and solo.
- Record all armed tracks at once, sample-aligned, as **24-bit mono WAV files, one per channel**.
- **Choose save folder** (Chrome or Edge) streams every track straight to disk while recording, so long takes don't fill memory. Without a folder, takes stay in memory and **Download WAVs** saves them.
- Timeline with waveforms, zoom, click-to-locate and playback of the last take.

- **Import names / Export names** load and save channel names as a CSV file with `Channel,Name` columns (a plain list of names, one per line, also works). Handy for keeping a channel list per show or copying it from your console's input list.
- Click a clip to select it and press `Delete` (or the Delete button) to remove it. If it was saved to a folder, the WAV file is moved into a `Deleted` subfolder there, so it can be recovered.

Keyboard: while typing a channel name, `Tab` or `Enter` jumps to the next channel's name (`Shift+Tab` goes back). `R` record, `Space` play/stop, `Home` or `Enter` return to zero, `Delete` remove the selected clip, `Esc` clear the selection.

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
