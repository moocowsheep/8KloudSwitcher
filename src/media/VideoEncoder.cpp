/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */
#include <algorithm>

#include "core/Log.h"
#include "media/FfmpegNvenc.h"
#include "media/IVideoEncoder.h"
#include "media/NvencDirect.h"

namespace kloud::media {

const char* videoCodecName(VideoCodec codec) {
    return codec == VideoCodec::Av1 ? "av1" : "hevc";
}

bool parseVideoCodec(std::string_view text, VideoCodec& out) {
    if (text == "hevc") out = VideoCodec::Hevc;
    else if (text == "av1") out = VideoCodec::Av1;
    else return false;
    return true;
}

const char* encoderBackendName(EncoderBackend backend) {
    switch (backend) {
        case EncoderBackend::Ffmpeg: return "ffmpeg";
        case EncoderBackend::Direct: return "direct";
        default: return "auto";
    }
}

bool parseEncoderBackend(std::string_view text, EncoderBackend& out) {
    if (text == "auto") out = EncoderBackend::Auto;
    else if (text == "ffmpeg") out = EncoderBackend::Ffmpeg;
    else if (text == "direct") out = EncoderBackend::Direct;
    else return false;
    return true;
}

const char* encoderPresetName(EncoderPreset preset) {
    switch (preset) {
        case EncoderPreset::P1: return "p1";
        case EncoderPreset::P2: return "p2";
        case EncoderPreset::P3: return "p3";
        case EncoderPreset::P5: return "p5";
        case EncoderPreset::P6: return "p6";
        case EncoderPreset::P7: return "p7";
        case EncoderPreset::P4: return "p4";
        default: return "auto";
    }
}

bool parseEncoderPreset(std::string_view text, EncoderPreset& out) {
    if (text == "auto") {
        out = EncoderPreset::Auto;
        return true;
    }
    for (int p = int(EncoderPreset::P1); p <= int(EncoderPreset::P7); ++p) {
        if (text == encoderPresetName(EncoderPreset(p))) {
            out = EncoderPreset(p);
            return true;
        }
    }
    return false;
}

int gopFrames(const EncoderConfig& cfg, const VideoFormatDesc& show) {
    const int ms = cfg.keyframeMs > 0 ? cfg.keyframeMs : kDefaultKeyframeMs;
    const double fps = double(show.fpsN) / double(show.fpsD);
    return std::max(1, int(fps * ms / 1000.0 + 0.5));
}

EncoderPreset resolveEncoderPreset(EncoderPreset preset,
                                   const VideoFormatDesc& show) {
    if (preset != EncoderPreset::Auto) return preset;
    // Above 4K a P4 picture takes long enough that the pack slot is still busy
    // at the render thread's next tick check, costing whole frames
    // (out.srt.fifBusySkips); P2 encodes in time and measures identical in
    // quality at these bitrates. At or below 4K nothing is time-pressured, so
    // keep the preset that holds up better if an operator starves the bitrate.
    return show.width * show.height > 3840 * 2160 ? EncoderPreset::P2
                                                  : EncoderPreset::P4;
}

std::unique_ptr<IVideoEncoder> openVideoEncoder(CudaCtx& cuda,
                                                const VideoFormatDesc& show,
                                                const EncoderConfig& cfg) {
    if (cfg.backend != EncoderBackend::Direct) {
        auto enc = std::make_unique<FfmpegNvenc>();
        if (enc->open(cuda, show, cfg)) return enc;
        if (cfg.backend == EncoderBackend::Ffmpeg) return nullptr;
        KLOUD_LOGW("encoder: %s_nvenc unavailable; falling back to direct NVENC",
                 videoCodecName(cfg.codec));
    }
    auto enc = std::make_unique<NvencDirect>();
    if (enc->open(cuda, show, cfg)) return enc;
    return nullptr;
}

}  // namespace kloud::media
