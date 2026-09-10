/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/AudioEngine.h"
#include "core/Format.h"
#include "core/MediaClock.h"
#include "core/Spsc.h"
#include "engine/Commands.h"
#include "engine/IInputSource.h"
#include "engine/InputSpec.h"
#include "engine/SwitcherCore.h"
#include "gpu/Compositor.h"
#include "gpu/UploadRing.h"
#include "gpu/VkEngine.h"
#include "media/IVideoEncoder.h"

#include "media/CudaCtx.h"
#include "media/Playlist.h"

namespace kloud {

class OmtOutput;
class DeckLinkOutput;
class FileRecorder;
class SrtOutput;

struct EngineConfig {
    VideoFormatDesc show{1920, 1080, 60000, 1001};
    // 1080p keeps 21 input thumbnails (7 x 3 grid) legible on the monitor.
    int mvW = 1920;
    int mvH = 1080;
    std::vector<InputSpec> inputs;
    bool validation = false;
    bool omtOut = true;
    std::string omtOutName = "8Kloud Switcher PGM";
    bool cleanOmtOut = false;
    std::string cleanOmtOutName = "8Kloud Switcher CLEAN";
    // The mvW x mvH monitor wall as an OMT source, labels and tally included:
    // a remote operator position sees exactly what the GUI multiview shows.
    bool mvOmtOut = false;
    std::string mvOmtOutName = "8Kloud Switcher MV";
    // SDI program/clean output: a decklink:// ref each, empty = off. The card
    // cannot rescale, so the sub-device must offer the show format exactly.
    // Half duplex means a sub-device doing this cannot also capture.
    std::string sdiOutRef;
    std::string cleanSdiOutRef;
    // SRT program output: the FFmpeg URL (core/SrtUrl.h splits it into the
    // fields the console edits); empty = off. srtSend=false parks a
    // configured URL so the OUTPUTS tab can switch the stream off and on
    // without retyping it.
    std::string srtUrl;
    bool srtSend = true;
    int srtBitrateKbps = 0;  // 0 = auto
    media::VideoCodec srtCodec = media::VideoCodec::Hevc;
    int srtKeyframeMs = 0;   // IDR interval, 0 = 2 s
    int srtAudioKbps = 0;    // AAC bitrate, 0 = 160
    bool srtEnabled() const { return srtSend && !srtUrl.empty(); }
    int recordBitrateKbps = 0;  // 0 = auto; independent of SRT output
    // NVENC path and speed/quality preset for SRT output and recording
    // (--encoder, --encoder-preset).
    media::EncoderBackend encoder = media::EncoderBackend::Auto;
    media::EncoderPreset encoderPreset = media::EncoderPreset::Auto;
    bool audio = true;
    // A/V calibration. Measured on this box: with 0, offsets land at
    // 1080p network-in ~-1ms / SRT ~-7ms, 8K ~+7ms (audio ages ~one capture
    // iteration more at 8K) -- all inside the +-10ms gate. Operators
    // re-trim per show from the GUI master strip.
    int masterAudioDelayMs = 0;
};

// Owns the GPU, the inputs and program output, the switcher state machine,
// and the 59.94-paced render thread. Qt-free.
class Engine {
public:
    Engine();   // out-of-line: members hold unique_ptrs to fwd-declared types
    ~Engine();

    bool start(const EngineConfig& cfg);
    void stop();
    bool running() const { return started_.load(std::memory_order_acquire); }

    // Serialized: the command ring is SPSC and both the GUI thread and the
    // remote-control server post (control rate, contention negligible).
    void post(const Command& c) {
        std::lock_guard lk(postM_);
        cmds_.push(c);
    }

    // Source picker: swap input `index` to a new source. The render thread
    // performs the swap between ticks (placeholder until the new source
    // delivers); the old input is destroyed on a detached thread. SRT and
    // media specs require Vulkan/CUDA interop (initialized on demand; refused
    // if absent).
    void requestInputReplace(int index, InputSpec spec);
    // OMT discovery snapshot (names in "HOST (Name)" form). Empty when built
    // without OMT. First call may return empty until mDNS answers arrive.
    std::vector<std::string> omtSources() const;
    // DeckLink device display names, in SDK order (index = decklink://N).
    // Empty when built without the SDK or when no card/driver is present.
    std::vector<std::string> decklinkSources() const;
    std::string inputRef(int i) const;  // current source ref (UI labels)
    InputSpec::Type inputType(int i) const;
    int inputSyncFrames(int i) const;   // current frame-sync setting (-1 off)
    IInputSource::MediaState inputMediaState(int i) const;
    std::vector<media::PlaylistItem> inputMediaPlaylist(int i) const;

    struct RecordingState {
        bool active = false;
        bool pending = false;
        bool error = false;
        int64_t frames = 0;
        std::string path;
    };
    // Empty path stops recording. Encoder construction and finalization run
    // on the requesting thread; render/audio consumers switch atomically.
    void requestRecording(std::string path);
    void requestCleanRecording(std::string path);
    RecordingState recordingState() const;
    RecordingState cleanRecordingState() const;

    bool copyMultiview(std::vector<uint8_t>& out, uint64_t& lastSeq, int& w, int& h);

    using InputStatus = IInputSource::Status;

    struct UiState {
        int program = 0, preview = 1;
        bool inTransition = false;
        bool ftbEngaged = false;
        float ftbLevel = 0.f;
        bool dskOn[kDskCount] = {false, false};
        float dskLevel[kDskCount] = {0.f, 0.f};
        int dskSrc[kDskCount] = {0, 0};
        bool dskTie[kDskCount] = {false, false};
        bool dskAudioFollow[kDskCount] = {false, false};
        int transType = 0;  // TransitionType as int
        int transDur = 30;  // ticks
        float transSoftness = 0.02f;
        int dskDur[kDskCount] = {30, 30};  // keyer fade ticks
    };
    UiState uiState() const {
        std::lock_guard lk(uiM_);
        return ui_;
    }

    int inputCount() const { return int(inputs_.size()); }
    InputStatus inputStatus(int i) const { return inputs_[i]->status(); }

    // Audio mixer controls/meters (null when cfg.audio == false). Channel
    // atomics and meters are safe to poke from the GUI thread.
    audio::AudioEngine* audio() { return audio_.get(); }
    const audio::AudioEngine* audio() const { return audio_.get(); }
    int64_t renderedTicks() const { return ticks_.load(std::memory_order_relaxed); }
    int64_t skippedTicks() const { return skips_.load(std::memory_order_relaxed); }
    int64_t omtOutFrames() const;
    int64_t cleanOmtOutFrames() const;
    int64_t mvOmtOutFrames() const;
    int64_t sdiOutFrames() const;
    int64_t cleanSdiOutFrames() const;

    // Output status for the GUI and remote-control surfaces. The names are
    // fixed by start(); the flags are live. An OMT sender either came up with
    // the engine or not at all, so its pointer is its health; the SDI senders
    // open asynchronously and can lose the card later, so they report their
    // own.
    bool omtOutActive() const { return omtOut_ != nullptr; }
    bool cleanOmtOutActive() const { return cleanOmtOut_ != nullptr; }
    bool mvOmtOutActive() const { return mvOmtOut_ != nullptr; }
    bool sdiOutOk() const;
    bool cleanSdiOutOk() const;
    bool omtOutRequested() const { return cfg_.omtOut; }
    bool cleanOmtOutRequested() const { return cfg_.cleanOmtOut; }
    bool mvOmtOutRequested() const { return cfg_.mvOmtOut; }
    const std::string& omtOutName() const { return cfg_.omtOutName; }
    const std::string& cleanOmtOutName() const { return cfg_.cleanOmtOutName; }
    const std::string& mvOmtOutName() const { return cfg_.mvOmtOutName; }
    const std::string& sdiOutRef() const { return cfg_.sdiOutRef; }
    const std::string& cleanSdiOutRef() const { return cfg_.cleanSdiOutRef; }
    int64_t srtFramesEncoded() const;
    bool srtConnected() const;
    bool srtConfigured() const { return srtOut_ != nullptr; }
    VideoFormatDesc outputFormat() const { return cfg_.show; }

private:
    void renderLoop(std::stop_token st);
    void requestRecordingImpl(std::string path, bool clean);
    RecordingState recordingStateImpl(bool clean) const;
    bool createPlaceholder();
    bool buildLabelAtlas();

    EngineConfig cfg_;
    gpu::VkEngine vk_;
    std::unique_ptr<gpu::Compositor> comp_;
    std::vector<std::unique_ptr<IInputSource>> inputs_;
    std::unique_ptr<OmtOutput> omtOut_;
    std::unique_ptr<OmtOutput> cleanOmtOut_;
    std::unique_ptr<OmtOutput> mvOmtOut_;
    std::unique_ptr<DeckLinkOutput> sdiOut_;
    std::unique_ptr<DeckLinkOutput> cleanSdiOut_;
    media::CudaCtx cuda_;
    std::unique_ptr<SrtOutput> srtOut_;
    // Atomic shared ownership lets the audio and render threads take a
    // non-blocking snapshot while start/stop swaps the active recorder.
    std::atomic<std::shared_ptr<FileRecorder>> recorder_;
    std::atomic<std::shared_ptr<FileRecorder>> cleanRecorder_;
    std::unique_ptr<audio::AudioEngine> audio_;
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> srtNvPushed_{};
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> recordNvPushed_{};
    std::array<uint64_t, gpu::Compositor::kFramesInFlight>
        cleanRecordNvPushed_{};

    std::shared_ptr<gpu::UploadRing> placeholderRing_;
    std::shared_ptr<const gpu::GpuFrame> placeholder_;

    std::mutex postM_;  // post() producers (GUI + control server)
    SpscRing<Command> cmds_{64};
    MediaClock clock_;
    SwitcherCore switcher_;  // render thread only

    gpu::Timeline renderTL_;
    gpu::Timeline readbackTL_;  // pack -> host copies (xferDown)
    VkCommandPool gfxPool_ = VK_NULL_HANDLE;
    VkCommandPool downPool_ = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, gpu::Compositor::kFramesInFlight> cmdBufs_{};
    std::array<VkCommandBuffer, gpu::Compositor::kFramesInFlight> downBufs_{};
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> fifValues_{};
    std::array<uint64_t, gpu::Compositor::kFramesInFlight> downValues_{};
    std::array<std::vector<std::shared_ptr<const gpu::GpuFrame>>,
               gpu::Compositor::kFramesInFlight>
        retention_;

    std::atomic<uint64_t> mvReady_{0};
    std::array<std::atomic<uint64_t>, gpu::Compositor::kReadbackSlots> rbStamp_{};
    std::atomic<int64_t> ticks_{0}, skips_{0};

    mutable std::mutex uiM_;
    UiState ui_;

    mutable std::mutex replaceM_;  // pending replaces + current input specs
    std::vector<std::pair<int, InputSpec>> pendingReplace_;

    mutable std::mutex recordM_;
    std::string requestedRecordingPath_;
    std::string requestedCleanRecordingPath_;
    std::atomic<bool> recordingPending_{false};
    std::atomic<bool> recordingError_{false};
    std::atomic<uint64_t> recorderGeneration_{0};
    std::atomic<bool> cleanRecordingPending_{false};
    std::atomic<bool> cleanRecordingError_{false};
    std::atomic<uint64_t> cleanRecorderGeneration_{0};

    std::jthread renderThread_;
    std::atomic<bool> started_{false};
};

}  // namespace kloud
