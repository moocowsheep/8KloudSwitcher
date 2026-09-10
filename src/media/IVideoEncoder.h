/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#pragma once
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#include "core/Format.h"
#include "media/CudaCtx.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace kloud::media {

enum class VideoCodec { Hevc, Av1 };

const char* videoCodecName(VideoCodec codec);
// Accepts "hevc" and "av1"; false (and `out` untouched) otherwise.
bool parseVideoCodec(std::string_view text, VideoCodec& out);

// Which NVENC path a program encoder drives.
enum class EncoderBackend {
    Auto,    // FFmpeg, falling back to Direct if it will not open
    Ffmpeg,  // hevc_nvenc or av1_nvenc through libavcodec
    Direct,  // NVENC API through libnvidia-encode (HEVC or AV1)
};

// NVENC speed/quality preset. Measured on real 4K content at our tuning
// (ull + CBR + no B-frames + no lookahead), P2 and P4 are quality-identical
// at the auto bitrate (~0.04 bpp): PSNR Y differs by 0.003 dB at 8K and
// 0.01 dB at 4K. P4 only pulls ahead when the bitrate is starved (+0.05 dB at
// 1080p/4 Mbps, +0.08 dB at 2 Mbps), while P2 encodes fast enough at 8K to
// stop the SRT pack slot from being busy at the next tick check. Hence Auto:
// quality preset where it is free, speed preset where the tick needs it.
enum class EncoderPreset { Auto, P1, P2, P3, P4, P5, P6, P7 };

const char* encoderBackendName(EncoderBackend backend);
// Accepts "auto", "ffmpeg", "direct"; false (and `out` untouched) otherwise.
bool parseEncoderBackend(std::string_view text, EncoderBackend& out);

const char* encoderPresetName(EncoderPreset preset);
// Accepts "auto" and "p1".."p7"; false (and `out` untouched) otherwise.
bool parseEncoderPreset(std::string_view text, EncoderPreset& out);
// Resolves Auto for this show: P2 above 4K, P4 at or below it.
EncoderPreset resolveEncoderPreset(EncoderPreset preset,
                                   const VideoFormatDesc& show);

struct EncoderConfig {
    EncoderBackend backend = EncoderBackend::Auto;
    EncoderPreset preset = EncoderPreset::Auto;
    int bitrateKbps = 0;        // 0 = auto from resolution/fps
    bool globalHeader = false;  // muxer takes codec headers as extradata
    VideoCodec codec = VideoCodec::Hevc;
    // IDR interval. A stream viewer joins at the next IDR, so a short
    // interval trades bits for join time; 0 = the 2 s default.
    int keyframeMs = 0;
};

constexpr int kDefaultKeyframeMs = 2000;
// GOP length in frames for this show: keyframeMs (or the default) at the
// show rate, never below one frame.
int gopFrames(const EncoderConfig& cfg, const VideoFormatDesc& show);

// Program encoder fed by the render thread's tight-pitch NV12 pack
// buffers (luma rows then interleaved chroma, pitch == width, device memory).
// Both backends are tuned the same way: no B-frames, CBR with a single-frame
// VBV, IDR every keyframeMs (~2 s by default), and the codec headers
// required by the muxer.
// PTS is the media tick index in show timebase (fpsD/fpsN).
//
// `encode` returns only once `src` is free for reuse -- that return is the
// render thread's recycle handshake, so an encoder must never retain the
// input past the call.
class IVideoEncoder {
public:
    virtual ~IVideoEncoder() = default;

    virtual bool open(CudaCtx& cuda, const VideoFormatDesc& show,
                      const EncoderConfig& cfg) = 0;
    virtual void close() = 0;
    virtual bool ok() const = 0;

    virtual AVRational timeBase() const = 0;
    // Fills a muxer stream's codec parameters (id, geometry, extradata).
    virtual bool fillCodecpar(AVCodecParameters* par) const = 0;

    // Emitted packets are appended to `out`; the caller frees them.
    virtual bool encode(CUdeviceptr src, int64_t pts,
                        std::vector<AVPacket*>& out) = 0;
    virtual bool drain(std::vector<AVPacket*>& out) = 0;
};

// Opens `backend` for this show; returns null if no backend opened. Auto tries
// FFmpeg first (the tuned, measured default) and falls back to direct NVENC
// when this FFmpeg build has no usable wrapper for the selected codec.
std::unique_ptr<IVideoEncoder> openVideoEncoder(CudaCtx& cuda,
                                                const VideoFormatDesc& show,
                                                const EncoderConfig& cfg);

}  // namespace kloud::media
