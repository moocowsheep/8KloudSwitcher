/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <atomic>
#include <string>
#include <thread>

#include "core/Spsc.h"
#include "gpu/Compositor.h"
#include "gpu/VkEngine.h"
#include "media/AacEncoder.h"
#include "media/CudaCtx.h"
#include "media/IVideoEncoder.h"

extern "C" {
#include <libavformat/avformat.h>
}

namespace kloud {

struct SrtOutConfig {
    std::string url;  // srt://host:port?mode=...&latency=<usec>... (core/SrtUrl.h)
    media::EncoderConfig encoder;  // bitrate 0 = auto from resolution/fps
    int audioKbps = 0;             // AAC bitrate, 0 = 160
};

constexpr int kDefaultSrtAudioKbps = 160;

// SRT HEVC/AV1 program output. The render thread packs NV12 into per-FIF
// exportable buffers and pushes {value, tick, fif} events; the encode thread
// waits the render timeline, bridges the buffer into an FFmpeg CUDA hwframe
// (copiedValue() is the render-side reuse handshake), and encodes; the mux
// thread owns the MPEG-TS/SRT connection with reconnect. Audio arrives as
// master-bus PCM on the mixer thread (pushAudio), is AAC-encoded there, and
// joins the mux as stream 1 -- video PTS is the tick index (fpsD/fpsN) and
// audio PTS the sample index (1/48000) on the same origin, so the streams
// align without wallclock. A dead or slow SRT peer can never block the
// render clock or the mixer: events and packets are dropped.
class SrtOutput {
public:
    struct PackEvent {
        uint64_t value = 0;
        int64_t tick = 0;
        int fif = 0;
    };

    SrtOutput(gpu::VkEngine& vk, media::CudaCtx& cuda, gpu::Compositor& comp,
              gpu::Timeline& renderTL, SrtOutConfig cfg,
              const VideoFormatDesc& show, bool withAudio);
    ~SrtOutput();

    bool ok() const { return ok_; }
    bool push(const PackEvent& e) { return evts_.push(e); }

    // Mixer thread: encode one master-bus chunk (interleaved stereo, 48 kHz)
    // and queue the packets for the mux.
    void pushAudio(const float* lr, int frames, int64_t firstSample);
    uint64_t copiedValue(int fif) const { return copied_[fif].load(std::memory_order_acquire); }

    int64_t framesEncoded() const { return encoded_.load(std::memory_order_relaxed); }
    int64_t packetsSent() const { return sent_.load(std::memory_order_relaxed); }
    bool connected() const { return connected_.load(std::memory_order_relaxed); }

private:
    void encodeLoop(std::stop_token st);
    void muxLoop(std::stop_token st);
    bool openMux();
    void closeMux(bool writeTrailer);
    static int interruptCb(void* opaque);

    gpu::VkEngine& vk_;
    media::CudaCtx& cuda_;
    gpu::Compositor& comp_;
    gpu::Timeline& renderTL_;
    SrtOutConfig cfg_;
    VideoFormatDesc show_;

    media::CudaCtx::Imported imports_[gpu::Compositor::kFramesInFlight]{};
    std::unique_ptr<media::IVideoEncoder> enc_;
    media::AacEncoder aac_;
    std::vector<AVPacket*> aacScratch_;  // mixer-thread only

    SpscRing<PackEvent> evts_{16};
    SpscRing<AVPacket*> pkts_{512};
    SpscRing<AVPacket*> audioPkts_{512};
    int audIdx_ = -1;
    std::atomic<uint64_t> copied_[gpu::Compositor::kFramesInFlight]{};
    std::atomic<int64_t> encoded_{0}, sent_{0}, dropped_{0};
    std::atomic<bool> connected_{false};
    std::atomic<bool> stopFlag_{false};

    AVFormatContext* oc_ = nullptr;
    bool ok_ = false;

    std::jthread encodeThread_;
    std::jthread muxThread_;
};

}  // namespace kloud
