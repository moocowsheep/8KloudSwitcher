# 8Kloud Switcher

A live video switcher for Linux + NVIDIA: OMT, SDI and SRT inputs, program/preview
switching with transitions, OMT, SDI and SRT (HEVC or AV1/NVENC) program outputs, full audio
mixer, a browser-based production console, and the multiview wall as an OMT source. Built
for low latency at up to 8K 59.94p.

Status: **v1 complete (M0–M6), v2 frame sync landed**. 8K-hardened engine (30-min soak: zero
tick overruns, 1.5-frame latency, <2 cores full pipeline, NVENC 54%), full audio mixer (A/V
within ±8 ms on network and SRT paths at 1080p and 8K), live source picker (swap
OMT/SDI/SRT sources per input mid-show), show-file persistence (restart restores
everything), health banners,
runtime counters in the console, HEVC/AAC program recording, paced local clip
playlists with trim, speed, and transport controls, and static raster inputs
with native alpha. A parallel clean feed can be recorded or sent over OMT
without DSK graphics. Per-input **frame sync**: re-times a source onto the output tick
grid (1–4 frame buffer absorbs bursty delivery, rate slip becomes counted repeats/drops) and
auto-aligns the input's audio to the re-timed video — the cross-session A/V phase lottery
collapses to a constant (design + measurements: `docs/design-framesync.md`,
`docs/bench-framesync.md`). Bench record: `docs/bench-m5.md`; tuning: `docs/tuning.md`.
For 8K ingest prefer SRT/HEVC (NVDEC) or SDI; OMT/VMX carries realistic 8K content
(`docs/bench-omt.md`; with libomt-c and its decoder pool a new 8K connection's
first frame lands on time, `docs/omt.md`). Milestones: M6 (v1 close), M5 (8K hardening), M4 (audio),
M3+M3.5 (SRT/HEVC both directions), M2 (switching/multiview), M1 (Vulkan engine), M0 (bench).

OMT sources are addressed by their full discovery name, `HOSTNAME (Name)` — the
GUI picker fills that in; on the CLI, quote it. An `omt://host:port` URL also works.

```sh
# SRT out (listener) + receive with any ffplay/OBS caller:
./build/8kloud-switcher --input "HOST (CamA)" --input "HOST (CamB)" \
    --srt-out "srt://:9710?mode=listener&latency=120000"
ffplay "srt://HOST:9710?mode=caller"     # latency option is MICROseconds
# ffplay buffers seconds on live streams; for a low-latency audio monitor use:
ffmpeg -fflags nobuffer -flags low_delay -i "srt://HOST:9710?mode=caller" -vn -f pulse Mon
# SRT ingest as an input (audio decodes too; trim its lateness per input):
./build/8kloud-switcher --srt-input "srt://HOST:9710?mode=caller&latency=120000" \
    --input "HOST (CamB)"
# AV1 output (requires AV1 NVENC hardware and the patched private FFmpeg build):
./build/8kloud-switcher --srt-codec av1 \
    --srt-out "srt://:9710?mode=listener&latency=120000"
# A/V sync check on a TS capture (needs testgen content on program):
ffmpeg -y -copyts -i "srt://HOST:9710?mode=caller" -c copy -avoid_negative_ts disabled \
       -muxpreload 0 -muxdelay 0 -t 20 cap.ts && python scripts/av_offset_ts.py cap.ts
```

Run it:
```sh
./build/kloud-testgen --name CamA &  ./build/kloud-testgen --name CamB &
# or kloud-headless for no console at all
./build/8kloud-switcher --input "$(hostname -s | tr a-z A-Z) (CamA)" \
                        --input "$(hostname -s | tr a-z A-Z) (CamB)"
# then open http://<switcher-host>:9924/ in a browser
```

There is no local window. The **console is a web page** served by `8kloud-switcher`
itself (`--web-port N`, default 9924, `0` to disable) with the multiview streamed to
the browser as JPEG frames, and the **multiview wall is an OMT source** ("8Kloud
Switcher MV" by default, labels and tally baked in) that any OMT receiver on the
network can put on a real monitor. Both are described in
[`docs/web-gui.md`](docs/web-gui.md). Like the remote-control port the web GUI
trusts the LAN: there is no authentication.

The show (inputs, outputs, transition, program/preview, full mixer state) persists to
`~/.config/8KloudSwitcher/show.ini` (or `--show-file PATH`) and restores on restart; CLI flags
override what they name. Click an input's name in the mixer to pick a different source live —
the browser lists OMT discovery (Open Media Transport, 8K-capable for realistic content:
build steps `docs/omt.md`, measurements `docs/bench-omt.md`; headless `--input` or
`--omt-input`) and any Blackmagic **DeckLink** cards as `SDI ·` entries
(8-bit UYVY capture up to 8K, auto-detecting the incoming mode: `docs/decklink.md`;
headless `--decklink-input 0`), and the manual field takes an `srt://`, `omt://` or
`decklink://` URL or an OMT discovery name.
The same dialog sets the input's frame sync (Off / Trim only /
1–4 frames; headless: `--framesync IDX[:FRAMES]`). Use 1 frame for free-running cameras
you switch between often (constant A/V at +1 frame latency); Trim only suits audio-early
sources like SRT loopbacks. Clips and stills are picked with a file browser that lists
the switcher host's disk. Keyboard shortcuts in the browser: `Space` cut, `Enter` auto,
`F` FTB, `1–9` program, `Shift+1–9` preview, `D`/`Shift+D` DSK 1/2.

Select the program output resolution and progressive frame rate from the **OUTPUT FORMAT**
controls above the multiview. The choice is saved immediately to the show file; restart
8Kloud Switcher when the amber **RESTART TO APPLY** badge appears. The selected format drives
both OMT and SRT program outputs on the next start. The **OUTPUTS** tab holds the rest of the
restart-to-apply settings the same way: the program, clean and multiview OMT senders (enable
and name, multiview wall size), SDI outputs, the **SRT STREAM** card, and the
recording bitrate. The SRT card sets the transport (send on/off, listener / caller /
rendezvous, host, port, latency in ms, passphrase and AES key length, stream id, any
other FFmpeg `srt` option) and the encoding (HEVC or AV1, video and AAC bitrates, keyframe
interval, NVENC preset and path). The fields are folded into one FFmpeg URL, shown on the
card's URL line, which is what the show file stores and `--srt-out` takes; paste a complete
URL there and the fields fill in from it. Headless: `--srt-out URL --srt-bitrate KBPS
--srt-codec hevc|av1 --srt-keyframe SEC --srt-audio-bitrate KBPS`.

Record the program mix with the red **RECORD** control in the top bar. Recordings
are HEVC video plus 48 kHz stereo AAC in a finalized Matroska (`.mkv`) file;
encoding and disk I/O run off the render thread, and recorder backpressure never
stalls program. Headless: `--record PATH.mkv [--record-bitrate KBPS]` (bitrate
defaults from the output format).

Put the program feed on **SDI** with `--sdi-out 0` (a bare device index or a
`decklink://` ref, on either executable; `--clean-sdi-out` does the same for the
clean feed). It reuses the same UYVY pack the network sender reads, so it costs
no conversion. The card cannot rescale — the sub-device must offer the show
format exactly — and half duplex means a sub-device sending SDI cannot also
capture, so use separate sub-devices for in and out. MediaClock stays master
rather than genlocking to the card; see `docs/decklink.md`.

Use **CLEAN REC** for the switched A/B mix without DSK graphics. The clean feed
retains transitions, FTB, and master audio. Program and clean recordings can
run simultaneously with independent NVENC sessions and backpressure. Headless:
`--clean-record PATH.mkv`; `--record-bitrate` applies to both. An optional
clean OMT sender is enabled with `--clean-omt-out "8Kloud Switcher CLEAN"` in
either executable and can run alongside the normal program sender. Its enabled state
and name persist in the show file, and the OUTPUTS tab toggles it. Design and validation:
`docs/design-clean-feed.md`.

Local H.264/HEVC clips can occupy any input: open the input source picker and
use **ADD CLIPS** to build and reorder a playlist, then use the **MEDIA** tab for
previous/next, play/pause, restart, and whole-list loop controls. Each clip is
timestamp-paced in real time, decoded through NVDEC, and its audio enters the
normal mixer lane. Set inclusive **IN** and exclusive **OUT** times per clip in
the playlist editor (`OUT=END` plays through EOF), plus a per-clip playback
speed from 25–400%; audio tempo follows without changing pitch. Playlist order,
trim points, speed, and loop mode persist in the show file; playback starts
from the first clip after an application restart. Headless:
`--media-input A.mkv --media-trim 500:2500 --media-speed 1.5 --media-item B.mkv`
(`--media-no-loop` stops at the end; trim values are milliseconds). Still
images use **ADD STILL** in the same source picker, decode and upload once,
then remain live until that input is replaced. PNG, JPEG, WebP, BMP, TIFF,
TGA, and EXR are recognized; non-opaque alpha is preserved for direct use as
a DSK graphic. Still selections persist in the show file. Headless:
`--still-input sponsor-logo.png`.
Design and validation notes: `docs/design-recorder-media.md`.

Two downstream keyers composite graphics with **native alpha (OMT UYVA or a local
raster still)** over program — point a DSK at an input carrying alpha (CasparCG, OBS with
alpha, `kloud-testgen --uyva`, or a transparent PNG/WebP still), and toggle it on; it fades
over its own duration, independent of transitions, and FTB takes it out with everything
else. A source without alpha keys fully opaque (a fadeable fullscreen overlay). Headless:
`--dsk K:SRC --dsk-fade K:TICKS --dsk-toggle-after S:K`. Design:
`docs/design-dsk.md`; measurements: `docs/bench-dsk.md` (an 8K UYVA key over an 8K program
holds full rate; keyers-off cost is nil).

## Encoder backends

HEVC program output (SRT and recording) runs through `hevc_nvenc` by default.
A patched FFmpeg adds draft AV1 carriage in MPEG-TS; `--srt-codec av1` selects
`av1_nvenc` for SRT output. SRT input detects and decodes either codec. AV1 is
an SRT-only choice: Matroska recordings remain HEVC, and AV1 requires an
AV1-capable NVIDIA encoder/decoder. Both FFmpeg and direct NVENC backends
support it.
A second backend drives NVENC directly through `libnvidia-encode`, so an FFmpeg
build without `hevc_nvenc` cannot take program output down; `--encoder
auto|ffmpeg|direct` selects one in either executable (`auto` falls back to the
direct path when FFmpeg has no usable HEVC or AV1 encoder). The two are configured
identically (ultra-low-latency, CBR, single-frame VBV, no B-frames, an IDR every
2 s or `--srt-keyframe SEC`) and measure
the same at every resolution tested.

`--encoder-preset auto|p1..p7` sets the NVENC speed/quality preset. `auto`
picks P2 above 4K and P4 at or below it: measured against a real 4K source,
the two are quality-identical at the auto bitrate (PSNR Y within 0.01 dB),
but at 8K a P4 picture takes long enough that the render thread finds the pack
slot still busy at the next tick and drops the frame — P2 holds full 59.94
where P4 loses 25–40% of the SRT feed. Design and measurements:
`docs/design-encoder.md`.

## Build

Requires: gcc 14+/clang, CMake 3.25+, Ninja, `glslc`, the Vulkan loader, and
FFmpeg development libraries (`libavcodec`, `libavformat`, `libavutil`,
`libavfilter`, `libswresample`, `libswscale`) plus the FFmpeg NVIDIA codec
headers (`ffnvcodec`). No GUI toolkit: the console is HTML/JS embedded into the
binary at build time from `web/`.

**The OMT and DeckLink SDKs are not in this repository** — `third_party/` is
gitignored, so a fresh clone does not have them and you must fetch them
yourself (step 1 below). They are technically optional, but the build only
prints a `STATUS` line when they are missing and then quietly produces a
crippled binary:

| Missing | What you lose |
| --- | --- |
| `third_party/omt` | OMT input, the OMT program/clean/multiview senders, and both bench tools (`kloud-testgen`, `kloud-latmeter`) |
| `third_party/decklink` | All SDI — DeckLink input and SDI program/clean output |

Check the configure output for `OMT SDK found` and `DeckLink SDK found` before
you trust a build, and always before you package one.

On Ubuntu 26.04 (the target platform):

```sh
sudo apt install build-essential clang cmake ninja-build pkgconf glslc \
    libvulkan-dev catch2 avahi-daemon \
    libavcodec-dev libavformat-dev libavutil-dev libavfilter-dev \
    libswresample-dev libswscale-dev libffmpeg-nvenc-dev
```

The CUDA driver API headers (`cuda.h`) are needed too, but Ubuntu 26.04 only
packages `nvidia-cuda-dev` at 12.4 — far behind the 610-series driver — so
install the CUDA toolkit from NVIDIA's runfile into `/usr/local/cuda`. CMake
accepts `/usr/local/cuda`, `/opt/cuda` or `/usr/include`.

Catch2 comes from the distro when installed; otherwise the build fetches
v3.8.0 from GitHub, which a package build cannot do.

**1. Vendor the SDKs into `third_party/`** (once per machine — they are
gitignored and never committed):

- **OMT** — run `packaging/build-omt.sh`. It clones and builds
  [libomt-c](https://github.com/8Kloud/libomt-c) (the native C implementation
  of the libomt API, with the pre-built decoder pool) and the VMX codec
  ([libvmx](https://github.com/8Kloud/libvmx), 8Kloud fork with the
  `VMX_Create` fix) and lays them out as `third_party/omt/{include,lib}`; needs
  clang++ and cmake. The stock .NET-based libomt also works in the same layout,
  without the decoder pool: [`docs/omt.md`](docs/omt.md).
- **DeckLink** — download the Blackmagic *Desktop Video SDK* from
  [blackmagicdesign.com/support](https://www.blackmagicdesign.com/support)
  (free, but registration-walled, so it cannot be scripted) and copy its Linux
  headers plus `DeckLinkAPIDispatch.cpp` into `third_party/decklink/include/`.
  See [`docs/decklink.md`](docs/decklink.md). Only the headers are needed to
  build; the Desktop Video *driver* is needed to run.

**2. Build:**

```sh
# Required for AV1/MPEG-TS and for redistributable packages. This builds the
# pinned LGPL FFmpeg and applies packaging/patches/ffmpeg automatically.
packaging/build-ffmpeg-lgpl.sh
cmake -B build -G Ninja -DKLOUD_FFMPEG_PREFIX=build/ffmpeg-lgpl
cmake --build build
ctest --test-dir build          # unit tests
```

For HEVC-only local development, omitting `KLOUD_FFMPEG_PREFIX` still uses the
system FFmpeg. AV1 output fails with a clear startup error if that FFmpeg does
not expose the patched `av1_mpegts_draft` muxer option.

## Debian package

`debian/` builds a single `8kloud-switcher` package with the console binary (web
GUI + engine), the headless engine, both bench tools, and the LGPL FFmpeg and OMT
runtime in a private libdir (`/usr/lib/*/8kloud-switcher`, reached through each
binary's RPATH).

**Do both prerequisite steps first** — neither is checked by `dpkg-buildpackage`
beyond the FFmpeg one, and a package built without the SDKs installs cleanly
while silently lacking OMT and SDI entirely:

1. Vendor `third_party/omt` and `third_party/decklink` as described under
   [Build](#build).
2. Run `packaging/build-ffmpeg-lgpl.sh`. `debian/rules` refuses to configure
   without it, since a distro FFmpeg cannot be linked into a redistributable
   build (see [License](#license)).

```sh
dpkg-buildpackage -b -us -uc      # -> ../8kloud-switcher_0.7.5_amd64.deb
sudo apt install ../8kloud-switcher_0.7.5_amd64.deb
```

The NVIDIA driver and NVENC runtime come in as package dependencies; the
DeckLink Desktop Video driver is a `Suggests` because SDI is optional. Link-time
optimization is disabled in `debian/rules` — GCC 15's LTO drops inline
`std::string` symbols across this project's static archives.

OMT discovery needs `avahi-daemon` running.

## Tools

```sh
# Test pattern sender: counter/timestamp strips, flash+tone A/V sync burst
./build/kloud-testgen --size 7680x4320 --fps 60000/1001 --name Cam1

# Latency analyzer: fps, frame gaps, end-to-end latency, A/V offset
./build/kloud-latmeter --source Cam1 --csv out.csv

# Transport bench (run the pair across two machines to measure the wire path)
./scripts/bench_codec.sh 7680x4320 30 build
```

## Vulkan validation

`--validate` (both binaries) loads `VK_LAYER_KHRONOS_validation` and installs a
debug-utils messenger, so layer warnings and errors come out through the normal
log as `vk: …`. Without the messenger the layer stays silent, so a clean run is
only meaningful with this flag's plumbing in place — sanity-check the pipe with
`VK_KHRONOS_VALIDATION_VALIDATE_BEST_PRACTICES=true`, which should produce
sub-allocation advice on any run.

```sh
./build/kloud-headless --validate --autos --duration 25 --dsk 0:1
```

## Remote control

A TCP control port (console default 9923) accepts plain-text commands and
pushes JSON state — see `docs/remote-control.md`. A ready-made Bitfocus
Companion module with tally feedbacks and presets lives in `companion/`. The
web console speaks the same command language over its WebSocket
(`docs/web-gui.md`), so anything Companion can do, a browser script can too.

## Layout

- `src/core/` — MediaClock (rational fps, drift-free), SPSC ring / latest-frame mailbox, stats
- `src/engine/` — SwitcherCore: pure program/preview + transition state machine (unit-tested)
- `src/omt/`, `src/decklink/`, `src/in/` — OMT, SDI and SRT/media/still inputs
- `src/out/` — OMT program + clean senders, SRT output, file recorder
- `src/ctl/` — remote-control wire protocol, the shared command→engine binding, TCP server
- `src/web/` — embedded HTTP/WebSocket server, MJPEG multiview stream; `web/` holds the page
- `src/app/` — the `8kloud-switcher` console: show file (QSettings-compatible INI), session, main
- `companion/` — Bitfocus Companion module (`npm run package` → installable tgz)
- `tools/` — kloud-testgen / kloud-latmeter + shared pattern layout (`tools/common/pattern.h`)
- `docs/` — bench reports; the full v1 plan lives with the project owner

## License

Copyright © 2026 Devin Block.

8Kloud Switcher is licensed under the **Mozilla Public License 2.0**
(MPL-2.0) — see [`LICENSE.md`](LICENSE.md). MPL-2.0 is file-level copyleft:
changes to these source files stay under the MPL, while the work as a whole may
be combined into a Larger Work under other terms, including proprietary ones
(MPL §3.3). No linking exception is needed, so the old `EXCEPTIONS.md` is gone.

That covers the Bitfocus Companion module in `companion/` too. It carries its own
copy of the license ([`companion/LICENSE`](companion/LICENSE)) because it is also
distributed standalone through Bitfocus's module registry, where MPL §3.1 requires
the license text to travel with the source.

That only holds if the binary's other pieces permit it, which is why the FFmpeg
build matters:

| Component | License | Note |
| --- | --- | --- |
| FFmpeg (via `packaging/build-ffmpeg-lgpl.sh`) | LGPL-2.1+ | **Do not link a distro FFmpeg** — see below |
| libsrt | MPL-2.0 | |
| dav1d | BSD-2-Clause | software AV1 decode inside the FFmpeg build |
| OMT — libomt, libvmx | MIT | |
| Vulkan loader | Apache-2.0 | |
| NVIDIA CUDA / NVENC / NVDEC runtime | proprietary | permitted under MPL §3.3 |
| Blackmagic DeckLink SDK | proprietary | dlopened at runtime |
| Catch2 | BSL-1.0 | tests only, not shipped |

**Ubuntu's FFmpeg is built `--enable-gpl`, so it is GPL-2.0-or-later as
shipped.** Linking it would force the whole binary to be conveyed under the GPL,
which defeats the MPL and cannot combine with the proprietary CUDA and DeckLink
pieces. Build the LGPL FFmpeg described under [Build](#build) and configure
against it with `-DKLOUD_FFMPEG_PREFIX=...`; the Debian package does this and
ships those libraries privately. A plain `cmake -B build` without the flag falls
back to the system FFmpeg and prints a loud warning — fine for development, not
for anything you distribute.
