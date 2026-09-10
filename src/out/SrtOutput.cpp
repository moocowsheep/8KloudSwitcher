/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "out/SrtOutput.h"

#include <mutex>

#include "core/Log.h"
#include "core/MediaClock.h"
#include "core/Stats.h"

extern "C" {
#include <libavutil/opt.h>
}

namespace kloud {

SrtOutput::SrtOutput(gpu::VkEngine& vk, media::CudaCtx& cuda,
                     gpu::Compositor& comp, gpu::Timeline& renderTL,
                     SrtOutConfig cfg, const VideoFormatDesc& show,
                     bool withAudio)
    : vk_(vk), cuda_(cuda), comp_(comp), renderTL_(renderTL),
      cfg_(std::move(cfg)), show_(show) {
    static std::once_flag netInit;
    std::call_once(netInit, [] { avformat_network_init(); });

    enc_ = media::openVideoEncoder(cuda_, show_, cfg_.encoder);
    if (!enc_) return;
    const int audioKbps =
        cfg_.audioKbps > 0 ? cfg_.audioKbps : kDefaultSrtAudioKbps;
    if (withAudio && !aac_.open(48000, audioKbps * 1000))
        KLOUD_LOGW("srt out: aac encoder unavailable; sending video-only");

    for (int f = 0; f < gpu::Compositor::kFramesInFlight; ++f) {
        const int fd = comp_.nvPackExportFd(f);
        if (fd < 0 || !cuda_.importVkFd(fd, comp_.nvPackBytes(), imports_[f])) {
            KLOUD_LOGE("srt out: NV12 pack buffer import failed (fif %d)", f);
            return;
        }
    }

    ok_ = true;
    encodeThread_ = std::jthread([this](std::stop_token st) { encodeLoop(st); });
    muxThread_ = std::jthread([this](std::stop_token st) { muxLoop(st); });
    KLOUD_LOGI("srt out: '%s' ready", cfg_.url.c_str());
}

SrtOutput::~SrtOutput() {
    stopFlag_.store(true);  // unblocks avio_open2/writes via interrupt callback
    encodeThread_ = {};
    muxThread_ = {};
    AVPacket* p = nullptr;
    while (pkts_.pop(p)) av_packet_free(&p);
    while (audioPkts_.pop(p)) av_packet_free(&p);
    for (auto& im : imports_) cuda_.release(im);
}

void SrtOutput::pushAudio(const float* lr, int frames, int64_t firstSample) {
    if (!ok_ || !aac_.ok()) return;
    // Emission alignment, audio half: video pts already carry +1 tick (see
    // encodeLoop), but the frame pipeline's true emission lag is ~1.5 ticks.
    // Rather than fractional video pts, shift audio the remaining half tick
    // (400 samples = 8.33 ms). Measured flash+tone 1080p: SRT av moves from
    // -12 ms to ~-4 ms, the network-sender path untouched.
    constexpr int64_t kAudioPtsBiasSamples = 400;
    aacScratch_.clear();
    if (!aac_.encode(lr, frames, firstSample + kAudioPtsBiasSamples,
                     aacScratch_)) {
        for (auto* pkt : aacScratch_) av_packet_free(&pkt);
        return;
    }
    for (auto* pkt : aacScratch_) {
        if (!audioPkts_.push(pkt)) {
            av_packet_free(&pkt);
            Stats::counter("out.srt.audioRingDrops").add();
        }
    }
}

int SrtOutput::interruptCb(void* opaque) {
    return static_cast<SrtOutput*>(opaque)->stopFlag_.load(std::memory_order_relaxed);
}

void SrtOutput::encodeLoop(std::stop_token st) {
    auto& encCtr = Stats::counter("out.srt.encoded");
    auto& lateCtr = Stats::counter("out.srt.renderWaitTimeouts");
    auto& dropCtr = Stats::counter("out.srt.packetRingDrops");

    cuda_.makeCurrent();
    std::vector<AVPacket*> pkts;

    while (!st.stop_requested()) {
        PackEvent e;
        if (!evts_.pop(e)) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        while (!renderTL_.waitCompleted(e.value, 100'000'000)) {
            lateCtr.add();
            if (st.stop_requested()) return;
        }
        pkts.clear();
        // pts = tick + 1: the frame composited at tick n is packed/readable
        // no earlier than tick n+1, while the mixer emits audio for a wall
        // moment within 10 ms. Stamping emission time (like the OMT output's
        // synthesized timecodes) keeps both outputs' A/V skew centered by
        // the same master delay. Measured flash+tone, 1080p: without this
        // the SRT path reads ~1.5 ticks audio-late vs the OMT path.
        if (enc_->encode(imports_[e.fif].ptr, e.tick + 1, pkts)) {
            copied_[e.fif].store(e.value, std::memory_order_release);
            encoded_.fetch_add(1, std::memory_order_relaxed);
            encCtr.add();
            for (auto* p : pkts) {
                if (!pkts_.push(p)) {
                    av_packet_free(&p);
                    dropped_.fetch_add(1, std::memory_order_relaxed);
                    dropCtr.add();
                }
            }
        } else {
            copied_[e.fif].store(e.value, std::memory_order_release);  // never wedge render
        }
    }
    pkts.clear();
    enc_->drain(pkts);
    for (auto* p : pkts) {
        if (!pkts_.push(p)) av_packet_free(&p);
    }
}

bool SrtOutput::openMux() {
    if (avformat_alloc_output_context2(&oc_, nullptr, "mpegts",
                                       cfg_.url.c_str()) < 0)
        return false;
    oc_->interrupt_callback = {&SrtOutput::interruptCb, this};

    AVStream* st = avformat_new_stream(oc_, nullptr);
    if (!st) return false;
    if (!enc_->fillCodecpar(st->codecpar)) return false;
    st->time_base = {1, 90000};

    if (aac_.ok()) {
        AVStream* as = avformat_new_stream(oc_, nullptr);
        if (!as) return false;
        avcodec_parameters_from_context(as->codecpar, aac_.codecCtx());
        as->time_base = {1, 90000};
        audIdx_ = as->index;
        // Don't let a stalled stream buffer the other for long (default 10s).
        oc_->max_interleave_delta = 500'000;  // 0.5 s in AV_TIME_BASE units
    }

    if (cfg_.encoder.codec == media::VideoCodec::Av1) {
        const int r = av_opt_set_int(oc_->priv_data, "av1_mpegts_draft", 1, 0);
        if (r < 0) {
            KLOUD_LOGE("srt out: FFmpeg lacks AV1 MPEG-TS support; rebuild "
                     "with packaging/build-ffmpeg-lgpl.sh");
            return false;
        }
    }

    if (avio_open2(&oc_->pb, cfg_.url.c_str(), AVIO_FLAG_WRITE,
                   &oc_->interrupt_callback, nullptr) < 0)
        return false;
    if (avformat_write_header(oc_, nullptr) < 0) return false;
    connected_.store(true, std::memory_order_relaxed);
    KLOUD_LOGI("srt out: connected (%s)", cfg_.url.c_str());
    return true;
}

void SrtOutput::closeMux(bool writeTrailer) {
    if (!oc_) return;
    if (writeTrailer && oc_->pb) av_write_trailer(oc_);
    if (oc_->pb) avio_closep(&oc_->pb);
    avformat_free_context(oc_);
    oc_ = nullptr;
    connected_.store(false, std::memory_order_relaxed);
}

void SrtOutput::muxLoop(std::stop_token st) {
    auto& sentCtr = Stats::counter("out.srt.packets");
    auto& reconnCtr = Stats::counter("out.srt.reconnects");
    const AVRational vidTb = enc_->timeBase();
    const AVRational audTb = aac_.timeBase();
    int64_t nextRetryNs = 0;

    while (!st.stop_requested()) {
        AVPacket* pkt = nullptr;
        int sidx = 0;
        AVRational tb = vidTb;
        if (!pkts_.pop(pkt) && aac_.ok() && audioPkts_.pop(pkt)) {
            sidx = audIdx_;
            tb = audTb;
        }
        if (!pkt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        if (!oc_) {
            const int64_t now = MediaClock::nowNs();
            if (now >= nextRetryNs) {
                if (openMux()) {
                    // The accept/handshake blocks this loop, so the rings
                    // backed up with stale packets. A switcher output is
                    // live: drop the backlog (including the packet in hand)
                    // and join the viewer at now. Video resumes at the next
                    // in-band IDR.
                    int64_t flushed = 1;
                    av_packet_free(&pkt);
                    for (AVPacket* q = nullptr; pkts_.pop(q); ++flushed)
                        av_packet_free(&q);
                    for (AVPacket* q = nullptr; audioPkts_.pop(q); ++flushed)
                        av_packet_free(&q);
                    Stats::counter("out.srt.connectFlushed").add(flushed);
                    KLOUD_LOGI("srt out: connect-flushed %lld stale packets",
                             (long long)flushed);
                    continue;
                }
                closeMux(false);
                nextRetryNs = now + 1'000'000'000;  // 1s backoff
                if (!st.stop_requested()) reconnCtr.add();
            }
        }
        if (!oc_) {
            av_packet_free(&pkt);  // disconnected: drop, never backpressure
            continue;
        }
        pkt->stream_index = sidx;
        av_packet_rescale_ts(pkt, tb, oc_->streams[size_t(sidx)]->time_base);
        const int r = av_interleaved_write_frame(oc_, pkt);
        av_packet_free(&pkt);
        if (r < 0) {
            KLOUD_LOGW("srt out: write failed (%d); reconnecting", r);
            closeMux(false);
            nextRetryNs = MediaClock::nowNs() + 1'000'000'000;
        } else {
            sent_.fetch_add(1, std::memory_order_relaxed);
            sentCtr.add();
        }
    }
    closeMux(true);
}

}  // namespace kloud
