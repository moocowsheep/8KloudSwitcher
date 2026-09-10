# Tuning for 8K / remote sources

The switcher itself needs no tuning for same-host validation (see
`docs/bench-m5.md`: full 8K pipeline < 2 cores, zero overruns untuned).
This page is for **remote** OMT/SRT at high bitrates and for hosts with
less headroom.

## Network buffers (needs sudo)

Remote 8K is 1–2.5 Gbps; SRT at 120 ms latency wants deep buffers.
Defaults on Arch (4 MB rmem_max) drop bursts long before the wire is full.

```sh
sudo cp scripts/tuning-sysctl.conf /etc/sysctl.d/90-8kloud-switcher.conf
sudo sysctl --system
```

SRT URLs: `latency` is in **microseconds** in FFmpeg URLs (the SRT STREAM card
takes ms and writes the URL for you).
Leave `maxbw` unset (libsrt's live-mode default ~1 Gbps cap is fine for
80–200 Mbps); never set `maxbw=0` without an explicit `inputbw`.

## Same-host topology limits (measured, 9900X + RTX 5090)

Every 8K60 hop moves ~4 GB/s of pixels through system RAM. This box
sustains **one full 8K pipeline** (testgen → switcher → OMT out + SRT out
→ latmeter ≈ 5 streams ≈ 20 GB/s) cleanly. Two 8K inputs *plus* an 8K
network out *plus* a local 8K receiver saturates memory bandwidth (stale
frames, gaps). For 2×8K production use, source cameras from the network
or from SDI, not from local senders — the plan's two-box topology.

## CPU affinity

Not needed at current load (render+mixer+capture < 2 cores of 24, zero
overruns in the 30-min soak). If a smaller host shows `render.skips` or
`audio.skips` in the counters, pin the noisy neighbors (browsers,
compilers) away from the app rather than pinning the app: libomt runs a
.NET thread pool and reacts badly to a shrunken cpuset.

## Codec budgets

OMT/VMX encodes on the sending thread and decodes on the receiving one,
so neither direction is free. Measurements for OMT at each resolution,
including the 8K ingest ceiling, are in `docs/bench-omt.md`; the OMT
build steps are in `docs/omt.md`.

For 8K ingest prefer SRT/HEVC (NVDEC) or SDI, both of which cost no CPU
to decode. `docs/bench-m5.md` records the equivalent budgets for the NDI
path this project used before v1.1; it is kept for the pipeline numbers,
not as current transport advice.

Re-run the codec bench anytime with `scripts/bench_codec.sh` and
measure with `scripts/cpu_sample.py`. Same-host runs measure the
loopback path; run the sender and receiver on two machines to measure
what actually goes on the wire.

## Clock discipline

The pipeline is locked to CLOCK_MONOTONIC and does not drift, and the
OMT senders stamp video and audio from that same clock. Measurement
tools that compare against CLOCK_REALTIME (`kloud-latmeter`'s latency
column, `scripts/av_offset_ts.py`) do see NTP slew — a few ppm of
apparent drift under systemd-timesyncd or chrony. Use PTP instead of NTP
when a measurement has to hold across hours.
