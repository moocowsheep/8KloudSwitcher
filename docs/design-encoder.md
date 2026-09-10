# Program encoder backends (FFmpeg NVENC / direct NVENC)

> **Note.** Written while NDI was the network transport. It was removed in
> v1.1 (OMT/SDI/SRT only); references to NDI below describe the sources the
> measurements were taken against. The design and the mechanics it
> describes are transport-independent and unchanged.

The v1 plan reserved an `IVideoEncoder` seam with `FfmpegNvenc` primary and
`NvencDirect` as the fallback tripwire. This document records the seam as
built, what the direct backend does differently, and what the two backends
actually measure at 1080p / 4K / 8K.

## The seam

`src/media/IVideoEncoder.h` is the whole contract:

```
open(cuda, show, EncoderConfig)               close()   ok()
timeBase()          -> muxer stream timebase (fpsD/fpsN)
fillCodecpar(par)   -> codec id, geometry, extradata for a new AVStream
encode(devptr, pts, out) / drain(out)         -> AVPacket* appended to `out`
```

Two rules hold for every implementation:

- **Input is tight-pitch NV12 in device memory** — the render thread's per-FIF
  pack buffer: luma rows, then interleaved chroma, pitch == width, chroma
  implicitly at `pitch * height`.
- **`encode()` returns only when the input buffer is free again.** `SrtOutput`
  and `FileRecorder` publish `copied_[fif]` right after `encode()` returns, and
  the render thread will not repack that slot until it sees that store. An
  encoder that retains the buffer past the call corrupts the next frame.

Packets are always `AVPacket*`, so the MPEG-TS and Matroska muxers are
identical across backends.

`openVideoEncoder(cuda, show, cfg)` builds one. `EncoderConfig` carries the
codec, backend, preset, bitrate (0 = auto from resolution/fps), and whether
the muxer wants a global header; `ffmpeg` and `direct` force a backend, and
`auto` (the default) opens FFmpeg, falling back to direct if the selected
codec's NVENC wrapper is unavailable.

SRT can instead set `VideoCodec::Av1` (`--srt-codec av1`). FFmpeg selects
`av1_nvenc`; the direct backend configures `NV_ENC_CODEC_AV1_GUID` and emits
low-overhead OBUs plus sequence-header extradata. The recording path leaves
the codec at its HEVC default. The private FFmpeg build applies the local
AV1/MPEG-TS draft patch, and `SrtOutput` explicitly enables its muxer option.

## What each backend does

| | `FfmpegNvenc` | `NvencDirect` |
|---|---|---|
| API | `hevc_nvenc` / `av1_nvenc` via libavcodec, CUDA hwframes on our primary context | `libnvidia-encode.so.1` (dlopen), HEVC/AV1 through ffnvcodec headers |
| Input handling | `cuMemcpy2DAsync` into an FFmpeg pool frame, stream-synced | pack buffers **registered once** (`NV_ENC_REGISTER_RESOURCE`), encoded in place |
| Per-frame GPU copy | one full frame (≈50 MB at 8K) | none |
| Completion | `send_frame` + `receive_packet`, FFmpeg may pipeline a frame | `nvEncLockBitstream` blocks until the picture is done |
| Headers | FFmpeg fills `extradata`; `GLOBAL_HEADER` sets `disableSPSPPS` | `nvEncGetSequenceParams`; `repeatSPSPPS`/`disableSPSPPS` set from `globalHeader` |

Both are configured the same way on purpose, so the two paths are swappable
mid-show: tuning ULTRA_LOW_LATENCY, CBR, single-frame VBV
(`bufsize = bitrate / fps`), `frameIntervalP = 1` (IPP, no B-frames), no
lookahead, `zeroReorderDelay`, IDR every ~2 s (`EncoderConfig::keyframeMs`,
the SRT card's KEYFRAME field or `--srt-keyframe`), HEVC Main 8-bit 4:2:0, and
in-band parameter sets for MPEG-TS (extradata for Matroska).

## Selection

```sh
./build/kloud-headless --encoder direct --srt-out "srt://:9720?mode=listener"
./build/8kloud-switcher  --encoder ffmpeg --encoder-preset p4
#   --encoder        auto | ffmpeg | direct
#   --encoder-preset auto | p1..p7
#   --srt-codec      hevc | av1
```

`EngineConfig::encoder` / `::encoderPreset` reach both SRT output and the file
recorders, so a run uses one configuration everywhere. They are startup flags,
not show-file settings — the encoder is a deployment property of the box, not
of the show.

**`--encoder-preset auto` (the default) picks P2 above 4K and P4 at or below
it.** The reasoning is measured, below: P4 costs whole frames at 8K and buys
nothing there, while it holds up marginally better at starved bitrates, which
only happens at the resolutions where speed is free.

## Measured (RTX 5090, driver 610.43.03, NVENC API 13.0)

45 s headless runs: 2 × 1080p NDI inputs, `--autos`, program NDI out + SRT out
(mpegts, in-band headers) + audio, receiver = local `ffmpeg` SRT caller.
**All rows in this first table are preset P4** (what both backends used before
the Auto policy existed); the P2 section below supersedes the 8K rows.
CPU is the steady-state average over the run (process `utime+stime`), `enc%`
is `nvidia-smi dmon` averaged over 20 s.

| show | backend | CPU (cores) | enc% | RSS | frames encoded / ticks | `fifBusySkips` |
|---|---|---|---|---|---|---|
| 1920×1080 | ffmpeg | 0.33 | 7.5 | 467 MB | 2699 / 2699 | 0 |
| 1920×1080 | direct | 0.32 | 9.2 | 476 MB | 2699 / 2699 | 0 |
| 3840×2160 | ffmpeg | 0.52 | 33.2 | 649 MB | 2699 / 2699 | 0 |
| 3840×2160 | direct | 0.58 | 38.7 | 678 MB | 2699 / 2699 | 0 |
| 7680×4320 | ffmpeg | 1.15 | 58.8 | 1362 MB | 1946 / 2700 | 754 |
| 7680×4320 | direct | 1.13 | 58.8 | 1445 MB | 1655 / 2699 | 1044 |

Decodability was verified on every run: 1080p and 4K decoded live to `-f null`
(1180–1183 frames per 20 s window ≈ 59.94), and the 8K runs were captured with
`-c copy` and decoded afterwards through NVDEC — both backends give clean
`hevc / Main / 7680x4320 / yuv420p` with no decoder errors.

**Through 4K the backends are a wash.** Same frame count, same zero skips, CPU
within noise (the direct path's saved copy is only ~3 MB/frame at 1080p and
~12 MB at 4K — invisible next to everything else the tick does).

**At P4, 8K sheds frames on both backends — and the direct backend sheds
more.** Not because encoding is slower — `enc%` is identical — but because of
how the pack slot is recycled. The render thread checks slot availability once
per tick; anything still holding a slot at that instant costs a whole frame.
FFmpeg releases our buffer after a device-to-device copy (well under a tick)
and encodes from its own pool surface; the zero-copy path holds the slot for
the entire encode (~10 ms at 8K), so it loses the next tick's check more often.
`out.srt.fifBusySkips` counts exactly this, and it is the difference between
1655 and 1946 frames.

Note the 8K shortfall is not introduced by this change: *both* backends shed
frames in this co-located topology (two upscaled 8K inputs + 8K NDI out + 8K
NVENC + audio on one box), consistent with the M5 finding that ~5 co-located
8K streams saturate DDR5. `render.ticks` stayed at 2699 with zero skips
throughout — the render clock never suffered, only the SRT feed thinned.

Forced split-frame encoding (`NV_ENC_SPLIT_AUTO_FORCED_MODE`) was tested as a
way to shorten the hold: **no effect** (1654 frames vs 1655, enc% 57.5 vs
58.8) — the driver already splits an 8K picture across engines in AUTO mode.
The line was removed rather than kept as dead configuration.

## Preset P2 removes the 8K shortfall entirely

The hold is encode time, so the next thing to try was a faster preset. Same 8K
runs, P2 instead of P4:

| 8K backend | preset | enc% | frames encoded / ticks | `fifBusySkips` |
|---|---|---|---|---|
| ffmpeg | P4 | 58.8 | 1946 / 2700 | 754 |
| ffmpeg | **P2** | 51.9 | **2700 / 2700** | **0** |
| direct | P4 | 58.8 | 1655 / 2699 | 1044 |
| direct | **P2** | 54.0 | **2700 / 2700** | **0** |

Both backends go to full rate, the receiver sees 1175–1182 frames per 20 s
(= 59.94), and CPU is unchanged (~1.14 cores). The zero-copy path is no longer
behind — at P2 the encode finishes well inside a tick, so holding the slot for
its duration costs nothing.

### What P2 costs in quality: nothing measurable at our bitrates

Reference: Big Buck Bunny 2160p60, 3–5 s segments, encoded through
`hevc_nvenc` with this file's exact settings (ull / CBR / no B-frames / no
lookahead / single-frame VBV), compared against the source with FFmpeg's
`psnr` and `ssim` filters.

| content | bitrate | PSNR Y (P2) | PSNR Y (P4) | SSIM Y (P2) | SSIM Y (P4) |
|---|---|---|---|---|---|
| 8K (upscaled) | 79.5 Mbps (auto) | 35.466 | 35.469 | 0.98246 | 0.98255 |
| 4K (native) | 19.9 Mbps (auto) | 34.252 | 34.262 | 0.95740 | 0.95780 |
| 1080p | 4 Mbps | 33.844 | 33.893 | 0.94698 | 0.94819 |
| 1080p | 2 Mbps | 33.254 | 33.332 | 0.93206 | 0.93404 |

At the auto bitrate (~0.04 bpp) the presets are indistinguishable — 0.003 dB
apart at 8K, 0.01 dB at 4K, with SSIM crossing over in P2's favour at 8K.
`tune ull` already disables the features that separate NVENC presets
(B-frames, lookahead, multi-pass), leaving mostly motion-search effort, which
does not bind when the bitrate is generous. P4 only earns anything when the
bitrate is starved: +0.05 dB at 1080p/4 Mbps, +0.08 dB at 1080p/2 Mbps — real
but an order of magnitude below anything visible.

That is the whole basis for the Auto policy: **speed is only needed above 4K,
and quality is only needed below it**, so the two never actually compete.

**Verdict: keep `auto` for both flags (FFmpeg first, P2 above 4K).** The
direct backend exists so an FFmpeg build without `hevc_nvenc` cannot take the
switcher's program output down; with P2 it is now equal at every resolution
tested, not just through 4K. If a future change ever makes the encode hold a
slot beyond a tick again, `out.srt.fifBusySkips` is the counter that says so.

## Gotchas

- **Pitch must be a multiple of 4** for a registered CUDA resource; `open()`
  rejects odd geometry rather than producing sheared output.
- **`NV_ENC_ERR_NEED_MORE_INPUT` must never happen here.** It would mean the
  encoder kept our input buffer, breaking the recycle handshake. The config
  guarantees it (no B-frames, no lookahead); the code treats it as a hard
  failure rather than pretending the frame was dropped cleanly.
- **Global header vs in-band.** With `globalHeader` the direct backend sets
  `disableSPSPPS` and hands VPS/SPS/PPS to the muxer through
  `fillCodecpar()` (Matroska); without it, `repeatSPSPPS` puts them on every
  IDR (MPEG-TS), which is what lets an SRT viewer join mid-stream.
- **Driver/header version skew** is checked at open
  (`NvEncodeAPIGetMaxSupportedVersion` vs `NVENCAPI_VERSION`) and logs the two
  versions instead of failing somewhere deeper.
- `libnvidia-encode.so.1` is dlopened once per process and never unloaded.

## Tests

`tests/nvenc_direct_test.cpp` (skips without GPU/CUDA/NVENC):

1. **Encodes registered CUDA NV12 into decodable HEVC** — 20 frames of moving
   content, one packet per picture, `pts == dts == frame index`, first packet
   is a keyframe, and the stream decodes with *no* extradata (proving in-band
   parameter sets).
2. **Exports codec parameters for a global-header muxer** — `fillCodecpar`
   gives HEVC, correct geometry, non-empty extradata, and the stream decodes
   when that extradata is supplied.
3. **Backends agree frame for frame** — same input buffer into both encoders;
   equal timebases, equal decoded frame counts and dimensions.
