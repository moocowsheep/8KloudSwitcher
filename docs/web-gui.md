# Web GUI and OMT multiview

`8kloud-switcher` has no local window. The operator console is a web page the
switcher serves itself, and the multiview wall is published as an OMT source
so it can sit on a real monitor anywhere on the network. Both replace the Qt
desktop GUI of earlier releases; the show file, the remote-control port and
the Companion module are unchanged.

## Console

```sh
./build/8kloud-switcher --input "HOST (CamA)" --input "HOST (CamB)"
# open http://<switcher-host>:9924/
```

- `--web-port N` picks the port (default **9924**; `0` turns the web GUI off,
  leaving a headless engine with just the TCP control port).
- `--web-mv-fps N` (default 12), `--web-mv-width N` (default 1920, the wall's
  own width) and `--web-mv-quality 2..31` (default 5, lower is better) shape
  the browser multiview stream: at the defaults a 1080p wall costs roughly
  100 KB per frame, about 10 Mbit/s per open tab. A Wi-Fi tablet is happier
  with `--web-mv-width 1280`; a wired LAN can take `--web-mv-fps 30`.
- The page shows the input matrix and the PROGRAM/PREVIEW pair as separate
  panes sized independently, so a wide window enlarges the thumbnails rather
  than adding letterbox. The MONITORS selector above the multiview chooses
  PGM over PVW (default), PGM beside PVW (bigger monitors in a wide, short
  window), or the wall exactly as the OMT receiver sees it; the bar between
  the monitor and the control deck drags to resize. Both are remembered per
  browser, and `?layout=side&deck=280` in the URL pins them for a kiosk.
- The page is a single HTML/JS bundle embedded in the binary at build time
  from `web/`; there is no web root to install or locate.
- It binds `0.0.0.0` and has **no authentication**, exactly like the TCP
  control port (`docs/remote-control.md`): the production LAN is trusted. The
  file browser used to pick clips and stills lists the switcher host's disk
  for whoever can reach the port. Do not expose it to the internet.

Any modern browser works (Chromium, Firefox, Safari; a tablet is fine at the
1050 px minimum width). Several operators can be connected at once; every
tab sees the same state within a frame.

### What is on it

The layout follows the old desktop console: a top bar with PGM/PVW readouts,
record and clean-record decks, health and link badges; the multiview
(left-click a cell for preview, right-click for program); the OUTPUT FORMAT
selectors; a tabbed left workspace — SWITCHER (program/preview buses), INPUTS
(patch grid), AUDIO MIXER (per-input strips with meters, fader, mute/solo,
delay, frame-sync auto-trim readout, master A/V calibration), MEDIA (playlist
transport), OUTPUTS (restart-to-apply settings); the TRANSITION module with
CUT/AUTO/FTB and a T-bar; and two DSK cards. The footer is the runtime
counter line.

Clicking an input's name (INPUTS tab or the mixer) opens the source dialog:
discovered OMT and SDI sources, a manual field (`srt://`, `omt://`,
`decklink://`, an OMT discovery name, or a still path), ADD CLIPS / ADD STILL
through a server-side file browser, playlist ordering with per-clip trim and
speed, and frame sync. Recording prompts for a path on the switcher host,
pre-filled with a timestamped file in `~/Videos`.

Keyboard: `Space` cut, `Enter` auto, `F` fade to black, `1–9` program,
`Shift+1–9` preview, `D` / `Shift+D` DSK 1 / 2. Shortcuts are suspended while
a text field or dialog has focus.

Settings on the OUTPUTS tab (output format, OMT senders, SDI outputs, SRT
stream, bitrates) are written to the show file the moment they change and
take effect on the next start; the amber RESTART TO APPLY badge tracks the
difference between what is saved and what is running.

The SRT STREAM card has two halves. Transport: SEND, mode (listener waits
for the viewer, caller connects out, rendezvous meets in the middle), host,
port, latency in milliseconds, passphrase with AES key length, stream id,
and an EXTRA line for any other FFmpeg `srt` option (`pkt_size=1316&maxbw=0`).
The URL line shows the FFmpeg URL those fields compose (`latency=` there is
in microseconds, as FFmpeg reads it); it is what the show file stores under
`srtOut` and what `--srt-out` takes, so pasting a complete URL into it fills
the fields from that URL, and a hand-typed option the card has no field for
lands in EXTRA. Turning SEND off parks the URL rather than clearing it.
Encoding: codec (HEVC, or AV1 with the patched FFmpeg), video bitrate (empty
= auto, the placeholder says how much), AAC bitrate, keyframe interval in
seconds (a viewer joins at the next IDR), NVENC preset and NVENC path. The
preset and path are shared with recording, as `--encoder-preset` and
`--encoder` always were; the rest of the tuning (CBR, ultra-low-latency,
single-frame VBV, no B-frames, no lookahead) is fixed, see
`docs/design-encoder.md`. A passphrase outside libsrt's 10..79 characters
or a `&` inside a value is refused with an error event and the field snaps
back.

## OMT multiview

The multiview wall (`--multiview WxH`, default 1920×1080) is packed and sent
as an OMT source named **"8Kloud Switcher MV"** by default. Labels
("01 HOST (CamA)", PROGRAM, PREVIEW) and the red/green tally borders are
rendered into the frame by the compositor, so a receiver shows a finished
monitor without any client-side overlay. Master audio is embedded, so a
remote operator position hears the mix too.

- The console turns it on by default; `--no-mv-omt-out` or the OUTPUTS tab
  turns it off, `--mv-omt-out NAME` renames it. Its enabled state, name and
  wall size persist in the show file. `kloud-headless` still defaults to
  off (benches run several instances side by side) and takes the same
  `--mv-omt-out NAME` flag.
- The browser's JPEG stream is a downscaled copy of this same image (the
  engine's host readback), so the browser and the OMT receiver always agree
  on labels, tally and layout.
- Health: the remote-control state carries `mvOmtOut {configured, up, name,
  frames}`; `configured && !up` is the alarm, and the console's health banner
  reports "multiview OMT out down".

Input labels use the compositor's built-in 5×7 pixel font (uppercase, digits
and common punctuation), so a long discovery name is cut at the cell edge on
a 21-input wall; the browser shows the full name in the bus keys and the
INPUTS tab.

## WebSocket protocol

`GET /ws` upgrades to a WebSocket. Text messages **to** the server are either

- a line in the remote-control protocol (`PGM 2`, `TRANSITION wipebox 45`,
  `RECORD START /path/x.mkv`, …; `SUBSCRIBE` is implied), or
- a JSON object with a `cmd` field for the operations the text protocol has
  no words for:

| `cmd` | Fields | Effect |
| --- | --- | --- |
| `replaceInput` | `input` (1-based), `type` (`omt` `srt` `decklink` `media` `still` `black` `auto`), `ref`, `sync` (-1..4) | Patch a source (`auto` infers the transport from the ref) |
| `playlist` | `input`, `items:[{path,in,out,speed}]`, `sync` | Replace a media input's playlist (ms, ms, permille) |
| `sync` | `input`, `sync` | Frame sync only, keeping the source and playlist |
| `audioDelay` | `input`, `ms` | Manual audio delay trim (0..500) |
| `masterDelay` | `ms` | Master A/V calibration delay (0..200) |
| `settings` | any of `show:{width,height,fpsN,fpsD}`, `omtOut`, `omtOutName`, `cleanOmtOut`, `cleanOmtOutName`, `mvOmtOut`, `mvOmtOutName`, `mvW`+`mvH`, `sdiOut`, `cleanSdiOut`, `srtOut` (whole URL) or the fields `srtMode` `srtHost` `srtPort` `srtLatencyMs` `srtPassphrase` `srtKeyLen` (0/16/24/32) `srtStreamId` `srtExtra` that fold into it, `srtSend`, `srtBitrateKbps`, `srtCodec`, `srtKeyframeMs`, `srtAudioKbps`, `encoder` (`auto` `ffmpeg` `direct`), `encoderPreset` (`auto` `p1`..`p7`), `recordBitrateKbps` | Restart-to-apply settings, saved to the show file. A refused value (passphrase length, `&` in a field) is an `error` event; the rest still applies. The `ui` document's `settings.pending`/`active` carry every key, the SRT fields as parsed from the stored URL |
| `sources` | | Reply with `{"event":"sources","omt":[…],"decklink":[{label,ref}]}` |
| `ls` | `path` | Reply with `{"event":"ls",path,parent,dirs:[…],files:[{name,size,still}]}` |
| `ui` | | Reply with the rich state document |
| `line` | `text` | A control-protocol line, for clients that only send JSON |

Messages **from** the server:

- `{"event":"hello",…}` on connect, then a `state` event and a `ui` event.
- `state`: the remote-control state document, on every change (same JSON as
  the TCP port; `docs/remote-control.md`).
- `ui`: the richer document the page is built from (input names, formats,
  frame counters, media position/duration/trim, mixer delays and auto trims,
  health list, status line, restart-to-apply settings vs. running config,
  multiview geometry). At most 4 Hz, only when it changed. `GET /api/state`
  returns the same document.
- `meters`: `{"v":[in1L,in1R,…,masterL,masterR]}` linear peaks since the
  last message, ~30 Hz, only when audio is on.
- `error`: `{"message"}` for a rejected command; never a disconnect.
- **Binary** messages are JPEG multiview frames at the stream size. A client
  that stops reading simply gets the newest frame when it catches up; nothing
  queues.

Plain HTTP endpoints, handy from `curl`: `/api/state`, `/api/control` (the
`state` document), `/api/sources`, `/api/fs?path=/dir`, and `/mv.jpg` (the
latest multiview frame).

Implementation: `src/web/Http.*` (HTTP parsing, WebSocket handshake and
frames, unit-tested), `src/web/WebServer.*` (one poll thread for all sockets,
plus an encode thread), `src/web/MjpegEncoder.*` (libavcodec `mjpeg`),
`src/app/Session.*` (show-file collection and the 2 s debounced save). The
`web:` test in `tests/web_server_test.cpp` drives the real server over a raw
socket, including the JPEG stream, and needs a GPU.
