/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "engine/Engine.h"

#include <algorithm>
#include <cstring>

#include "core/Font5x7.h"
#include "core/Log.h"
#include "core/Stats.h"
#include "engine/FrameSync.h"
#include "in/SrtInput.h"
#include "in/StillInput.h"
#include "out/DeckLinkOutput.h"
#include "out/FileRecorder.h"
#include "out/OmtOutput.h"
#include "out/SrtOutput.h"
#ifdef KLOUD_HAVE_OMT
#include <libomt.h>

#include "omt/OmtInput.h"
#endif

#ifdef KLOUD_HAVE_DECKLINK
#include "decklink/DeckLinkInput.h"
#endif

namespace kloud {

namespace {
// Stand-in for an input whose transport was not compiled in: it satisfies the
// interface and never publishes, so the render loop shows the placeholder.
class NullInput final : public IInputSource {
public:
    std::optional<Mailbox::Item> newer(uint64_t) const override { return {}; }
    Status status() const override { return {}; }
};

void normalizeInputSpec(InputSpec& spec) {
    if (spec.type == InputSpec::Type::Still) {
        // A static frame has no source cadence or audio to re-time.
        spec.syncFrames = -1;
        return;
    }
    if (spec.type != InputSpec::Type::Media) return;
    if (spec.mediaPlaylist.empty() && !spec.ref.empty())
        spec.mediaPlaylist.emplace_back(spec.ref);
    for (auto& item : spec.mediaPlaylist)
        media::normalizePlaylistItem(item);
    if (!spec.mediaPlaylist.empty())
        spec.ref = spec.mediaPlaylist.front().path;
}
}  // namespace

Engine::Engine() = default;
Engine::~Engine() { stop(); }

int64_t Engine::omtOutFrames() const { return omtOut_ ? omtOut_->framesSent() : 0; }
int64_t Engine::cleanOmtOutFrames() const {
    return cleanOmtOut_ ? cleanOmtOut_->framesSent() : 0;
}
int64_t Engine::mvOmtOutFrames() const {
    return mvOmtOut_ ? mvOmtOut_->framesSent() : 0;
}
int64_t Engine::sdiOutFrames() const {
    return sdiOut_ ? sdiOut_->framesSent() : 0;
}
int64_t Engine::cleanSdiOutFrames() const {
    return cleanSdiOut_ ? cleanSdiOut_->framesSent() : 0;
}
bool Engine::sdiOutOk() const { return sdiOut_ && sdiOut_->ok(); }
bool Engine::cleanSdiOutOk() const {
    return cleanSdiOut_ && cleanSdiOut_->ok();
}
int64_t Engine::srtFramesEncoded() const {
    return srtOut_ ? srtOut_->framesEncoded() : 0;
}
bool Engine::srtConnected() const { return srtOut_ && srtOut_->connected(); }

void Engine::requestInputReplace(int index, InputSpec spec) {
    normalizeInputSpec(spec);
    std::lock_guard lk(replaceM_);
    pendingReplace_.emplace_back(index, std::move(spec));
}

std::vector<std::string> Engine::omtSources() const {
    std::vector<std::string> out;
#ifdef KLOUD_HAVE_OMT
    int count = 0;
    char** addrs = omt_discovery_getaddresses(&count);
    for (int i = 0; addrs && i < count; ++i)
        if (addrs[i]) out.emplace_back(addrs[i]);
#endif
    return out;
}

std::vector<std::string> Engine::decklinkSources() const {
#ifdef KLOUD_HAVE_DECKLINK
    return DeckLinkInput::devices();
#else
    return {};
#endif
}

std::string Engine::inputRef(int i) const {
    std::lock_guard lk(replaceM_);
    if (i < 0 || i >= int(cfg_.inputs.size())) return {};
    return cfg_.inputs[size_t(i)].ref;
}

InputSpec::Type Engine::inputType(int i) const {
    std::lock_guard lk(replaceM_);
    if (i < 0 || i >= int(cfg_.inputs.size())) return InputSpec::Type::Omt;
    return cfg_.inputs[size_t(i)].type;
}

int Engine::inputSyncFrames(int i) const {
    std::lock_guard lk(replaceM_);
    if (i < 0 || i >= int(cfg_.inputs.size())) return -1;
    return cfg_.inputs[size_t(i)].syncFrames;
}

IInputSource::MediaState Engine::inputMediaState(int i) const {
    if (i < 0 || i >= int(inputs_.size())) return {};
    return inputs_[size_t(i)]->mediaState();
}

std::vector<media::PlaylistItem> Engine::inputMediaPlaylist(int i) const {
    std::lock_guard lk(replaceM_);
    if (i < 0 || i >= int(cfg_.inputs.size()) ||
        cfg_.inputs[size_t(i)].type != InputSpec::Type::Media)
        return {};
    return cfg_.inputs[size_t(i)].mediaPlaylist;
}

void Engine::requestRecording(std::string path) {
    requestRecordingImpl(std::move(path), false);
}

void Engine::requestCleanRecording(std::string path) {
    requestRecordingImpl(std::move(path), true);
}

void Engine::requestRecordingImpl(std::string path, bool clean) {
    auto& pending =
        clean ? cleanRecordingPending_ : recordingPending_;
    auto& error = clean ? cleanRecordingError_ : recordingError_;
    auto& recorderSlot = clean ? cleanRecorder_ : recorder_;
    auto& generation =
        clean ? cleanRecorderGeneration_ : recorderGeneration_;
    {
        std::lock_guard lock(recordM_);
        (clean ? requestedCleanRecordingPath_ : requestedRecordingPath_) =
            path;
    }
    error.store(false, std::memory_order_relaxed);
    pending.store(true, std::memory_order_release);

    std::shared_ptr<FileRecorder> next;
    if (!path.empty()) {
        if (!started_.load(std::memory_order_acquire) || !cuda_.ok()) {
            KLOUD_LOGE("record: engine/CUDA is not ready");
        } else {
            next = std::make_shared<FileRecorder>(
                cuda_, *comp_, renderTL_, path, cfg_.show, cfg_.audio,
                clock_.currentTick(),
                media::EncoderConfig{cfg_.encoder, cfg_.encoderPreset,
                                     cfg_.recordBitrateKbps, true},
                clean ? gpu::Compositor::Feed::Clean
                      : gpu::Compositor::Feed::Program);
            if (!next->ok()) next.reset();
        }
    }
    const bool failed = !path.empty() && !next;
    auto previous =
        recorderSlot.exchange(std::move(next), std::memory_order_acq_rel);
    generation.fetch_add(1, std::memory_order_release);
    while (previous && previous.use_count() > 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    previous.reset();
    error.store(failed, std::memory_order_relaxed);
    pending.store(false, std::memory_order_release);
}

Engine::RecordingState Engine::recordingState() const {
    return recordingStateImpl(false);
}

Engine::RecordingState Engine::cleanRecordingState() const {
    return recordingStateImpl(true);
}

Engine::RecordingState Engine::recordingStateImpl(bool clean) const {
    RecordingState state;
    const auto recorder =
        (clean ? cleanRecorder_ : recorder_).load(std::memory_order_acquire);
    state.active = recorder && recorder->ok();
    state.pending =
        (clean ? cleanRecordingPending_ : recordingPending_)
            .load(std::memory_order_acquire);
    state.error =
        (clean ? cleanRecordingError_ : recordingError_)
            .load(std::memory_order_relaxed) ||
                  (recorder && recorder->failed());
    state.frames = recorder ? recorder->framesEncoded() : 0;
    {
        std::lock_guard lock(recordM_);
        state.path =
            state.active
                ? recorder->path()
                : (clean ? requestedCleanRecordingPath_
                         : requestedRecordingPath_);
    }
    return state;
}

bool Engine::start(const EngineConfig& cfg) {
    cfg_ = cfg;
    for (auto& spec : cfg_.inputs) normalizeInputSpec(spec);
    // Reject degenerate geometry before it reaches Vulkan: a zero or negative
    // extent (`--show 0x0`, `--multiview 0x0`, a hand-edited show file) builds
    // a zero-sized VkImage and zero-sized pack buffers, and the process dies
    // in the driver with nothing pointing at the typo that caused it.
    // Both extents must be even: UYVY packs two luma per word and the NV12
    // pass dispatches at half height.
    if (!cfg_.show.valid() || (cfg_.show.width & 1) || (cfg_.show.height & 1)) {
        KLOUD_LOGE("show format %dx%d is invalid (need even extents, at least "
                 "2x2)",
                 cfg_.show.width, cfg_.show.height);
        return false;
    }
    if (cfg_.mvW < 2 || cfg_.mvH < 2) {
        KLOUD_LOGE("multiview %dx%d is invalid (need at least 2x2)", cfg_.mvW,
                 cfg_.mvH);
        return false;
    }
#ifdef KLOUD_HAVE_OMT_PREWARM
    {
        // libomt-c: build VMX decoders for the show format now, so they are
        // ready by the time discovery resolves the inputs -- Vulkan init alone
        // takes longer than the 8K decoder build. Receivers take one from the
        // pool and return it on re-patch. Sized to the assigned OMT inputs
        // (pool max 4; at least one so pooling is on for later patches).
        int omtInputs = 0;
        for (const auto& s : cfg_.inputs)
            if (s.type == InputSpec::Type::Omt && !s.ref.empty()) ++omtInputs;
        const int count = std::clamp(omtInputs, 1, 4);
        omt_video_decoder_prewarm(cfg_.show.width, cfg_.show.height,
                                  int(cfg_.show.fpsN / cfg_.show.fpsD),
                                  OMTColorSpace_Undefined, count);
        KLOUD_LOGI("omt: prewarming %d decoder%s for %dx%d", count,
                 count == 1 ? "" : "s", cfg_.show.width, cfg_.show.height);
    }
#endif
    if (!vk_.init(cfg.validation)) return false;

    comp_ = std::make_unique<gpu::Compositor>(vk_, cfg_.show, cfg_.mvW, cfg_.mvH,
                                              int(cfg_.inputs.size()));
    renderTL_ = vk_.createTimeline();
    readbackTL_ = vk_.createTimeline();
    gfxPool_ = vk_.createCommandPool(vk_.gfx().family);
    downPool_ = vk_.createCommandPool(vk_.xferDown().family);

    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = uint32_t(cmdBufs_.size());
    cai.commandPool = gfxPool_;
    vkAllocateCommandBuffers(vk_.device(), &cai, cmdBufs_.data());
    cai.commandPool = downPool_;
    vkAllocateCommandBuffers(vk_.device(), &cai, downBufs_.data());

    if (!createPlaceholder()) return false;
    if (!buildLabelAtlas()) return false;

    const bool needCuda =
        cfg_.srtEnabled() ||
        std::any_of(cfg_.inputs.begin(), cfg_.inputs.end(), [](const InputSpec& s) {
            return s.type == InputSpec::Type::Srt ||
                   s.type == InputSpec::Type::Media;
        });
    // Warm CUDA before the render clock starts so pressing RECORD later does
    // not pay context creation latency on-air. OMT/SDI-only operation still
    // works if interop is unavailable; SRT/media require it.
    const bool cudaAvailable =
        vk_.hasExternalMemoryFd && cuda_.init(vk_.deviceUuid());
    if (needCuda && !cudaAvailable) {
        KLOUD_LOGE("SRT/media requested but Vulkan/CUDA interop unavailable");
        return false;
    }

    // Senders before receivers, and never the other way round: libomt's
    // Linux discovery registers a sender by calling the avahi client straight
    // from the caller's thread (OMTDiscoveryAvahi.RegisterAddressInternal)
    // while its own event thread iterates the same simple-poll. avahi's
    // simple-poll is not thread-safe, so a registration racing a receiver's
    // first resolve wedges discovery for the whole process -- the sender comes
    // up and every discovery-name input stays dark forever. Bringing the
    // senders up first makes the registrations happen before any receiver
    // exists; stop() unwinds in the mirror order.
    if (cfg_.omtOut) {
        omtOut_ = std::make_unique<OmtOutput>(cfg_.omtOutName, *comp_,
                                              readbackTL_);
        if (!omtOut_->ok()) omtOut_.reset();
    }
    if (cfg_.cleanOmtOut) {
        cleanOmtOut_ = std::make_unique<OmtOutput>(
            cfg_.cleanOmtOutName, *comp_, readbackTL_,
            gpu::Compositor::Feed::Clean);
        if (!cleanOmtOut_->ok()) cleanOmtOut_.reset();
    }
    if (cfg_.mvOmtOut) {
        mvOmtOut_ = std::make_unique<OmtOutput>(
            cfg_.mvOmtOutName, *comp_, readbackTL_,
            gpu::Compositor::Feed::Multiview);
        if (!mvOmtOut_->ok()) mvOmtOut_.reset();
    }
    // SDI senders open asynchronously (the card may be busy at start), so
    // unlike the OMT ones there is nothing to test for here.
    if (!cfg_.sdiOutRef.empty())
        sdiOut_ = std::make_unique<DeckLinkOutput>(
            cfg_.sdiOutRef, *comp_, readbackTL_, cfg_.show);
    if (!cfg_.cleanSdiOutRef.empty())
        cleanSdiOut_ = std::make_unique<DeckLinkOutput>(
            cfg_.cleanSdiOutRef, *comp_, readbackTL_, cfg_.show,
            gpu::Compositor::Feed::Clean);

    for (size_t i = 0; i < cfg_.inputs.size(); ++i) {
        // Spread inputs across both DMA engines; serialized 66MB 8K copies on
        // one queue otherwise hold ring slots in flight long enough to starve
        // the ring.
        auto& q = (i % 2) ? vk_.xferDown() : vk_.xferUp();
        const auto& spec = cfg_.inputs[i];
        // An unassigned slot renders black: no capture thread, and nothing
        // hammering the transport with an empty address.
        if (spec.ref.empty())
            inputs_.push_back(std::make_unique<NullInput>());
        else if (spec.type == InputSpec::Type::Srt)
            inputs_.push_back(std::make_unique<SrtInput>(
                vk_, q, cuda_, spec.ref, int(i), spec.syncFrames));
        else if (spec.type == InputSpec::Type::Media)
            inputs_.push_back(std::make_unique<MediaInput>(
                vk_, q, cuda_, spec.ref, int(i), spec.syncFrames,
                spec.mediaPlaying, spec.mediaLoop, spec.mediaPlaylist));
        else if (spec.type == InputSpec::Type::Still)
            inputs_.push_back(std::make_unique<StillInput>(
                vk_, q, spec.ref, int(i), cfg_.show.fpsN, cfg_.show.fpsD));
#ifdef KLOUD_HAVE_DECKLINK
        else if (spec.type == InputSpec::Type::DeckLink)
            inputs_.push_back(std::make_unique<DeckLinkInput>(
                vk_, q, spec.ref, int(i), spec.syncFrames));
#endif
#ifdef KLOUD_HAVE_OMT
        else if (spec.type == InputSpec::Type::Omt)
            inputs_.push_back(std::make_unique<OmtInput>(
                vk_, q, spec.ref, int(i), spec.syncFrames, cfg_.show));
#endif
        else {
            KLOUD_LOGE("in%d: transport for '%s' was not built in; input will "
                     "stay dark", int(i), spec.ref.c_str());
            inputs_.push_back(std::make_unique<NullInput>());
        }
    }

    if (cfg_.srtEnabled()) {
        srtOut_ = std::make_unique<SrtOutput>(
            vk_, cuda_, *comp_, renderTL_,
            SrtOutConfig{cfg_.srtUrl,
                         {cfg_.encoder, cfg_.encoderPreset,
                          cfg_.srtBitrateKbps, /*globalHeader=*/false,
                          cfg_.srtCodec, cfg_.srtKeyframeMs},
                         cfg_.srtAudioKbps},
            cfg_.show,
            cfg_.audio);
        if (!srtOut_->ok()) {
            KLOUD_LOGE("SRT out init failed; disabling");
            srtOut_.reset();
        }
    }

    if (cfg_.audio) {
        audio_ = std::make_unique<audio::AudioEngine>(int(inputs_.size()));
        audio_->masterDelayMs.store(
            std::clamp(cfg_.masterAudioDelayMs, 0, audio::kMaxMasterDelayMs));
        for (size_t i = 0; i < inputs_.size(); ++i) {
            inputs_[i]->attachAudioSink(&audio_->channel(int(i)));
            audio_->channel(int(i)).syncManaged.store(
                cfg_.inputs[i].syncFrames >= 0, std::memory_order_relaxed);
        }
        if (omtOut_)
            audio_->addSink(
                [out = omtOut_.get()](const float* lr, int frames, int64_t s0) {
                    out->sendAudio(lr, frames, s0);
                });
        if (cleanOmtOut_)
            audio_->addSink([out = cleanOmtOut_.get()](
                                const float* lr, int frames, int64_t s0) {
                out->sendAudio(lr, frames, s0);
            });
        // The multiview carries the master bus too: a remote operator watching
        // the wall needs program audio with it.
        if (mvOmtOut_)
            audio_->addSink([out = mvOmtOut_.get()](const float* lr, int frames,
                                                    int64_t s0) {
                out->sendAudio(lr, frames, s0);
            });
        if (sdiOut_)
            audio_->addSink(
                [out = sdiOut_.get()](const float* lr, int frames, int64_t s0) {
                    out->sendAudio(lr, frames, s0);
                });
        if (cleanSdiOut_)
            audio_->addSink([out = cleanSdiOut_.get()](
                                const float* lr, int frames, int64_t s0) {
                out->sendAudio(lr, frames, s0);
            });
        if (srtOut_)
            audio_->addSink(
                [out = srtOut_.get()](const float* lr, int frames, int64_t s0) {
                    out->pushAudio(lr, frames, s0);
                });
        audio_->addSink([this](const float* lr, int frames, int64_t s0) {
            const auto recorder = recorder_.load(std::memory_order_acquire);
            if (recorder) recorder->pushAudio(lr, frames, s0);
            const auto clean =
                cleanRecorder_.load(std::memory_order_acquire);
            if (clean) clean->pushAudio(lr, frames, s0);
        });
    }

    // One origin for everything: video ticks, audio samples, and mux PTS all
    // count from here (the audio thread starts on the same origin below).
    clock_ = MediaClock(cfg_.show.fpsN, cfg_.show.fpsD);
    clock_.start();
    // The OMT senders stamp audio against the same origin the mixer counts
    // samples from (see OmtOutput's timestamp contract).
    if (omtOut_) omtOut_->setClockOrigin(clock_.originNs());
    if (cleanOmtOut_) cleanOmtOut_->setClockOrigin(clock_.originNs());
    if (mvOmtOut_) mvOmtOut_->setClockOrigin(clock_.originNs());
    if (sdiOut_) sdiOut_->setClockOrigin(clock_.originNs());
    if (cleanSdiOut_) cleanSdiOut_->setClockOrigin(clock_.originNs());
    if (audio_) audio_->start(clock_.originNs());
    renderThread_ = std::jthread([this](std::stop_token st) { renderLoop(st); });
    started_ = true;
    KLOUD_LOGI("engine started: show %dx%d @ %lld/%lld, %zu inputs, mv %dx%d, "
             "omtOut=%s, cleanOmt=%s, mvOmt=%s, sdiOut=%s, cleanSdi=%s",
             cfg_.show.width, cfg_.show.height, (long long)cfg_.show.fpsN,
             (long long)cfg_.show.fpsD, inputs_.size(), comp_->mvWidth(),
             comp_->mvHeight(), omtOut_ ? cfg_.omtOutName.c_str() : "off",
             cleanOmtOut_ ? cfg_.cleanOmtOutName.c_str() : "off",
             mvOmtOut_ ? cfg_.mvOmtOutName.c_str() : "off",
             sdiOut_ ? cfg_.sdiOutRef.c_str() : "off",
             cleanSdiOut_ ? cfg_.cleanSdiOutRef.c_str() : "off");
    return true;
}

bool Engine::createPlaceholder() {
    VideoFormatDesc d;
    d.width = 32;
    d.height = 18;
    placeholderRing_ = std::make_shared<gpu::UploadRing>(vk_, d, vk_.xferUp());
    const int slot = placeholderRing_->acquire();
    if (slot < 0) return false;
    uint8_t* p = placeholderRing_->stagingPtr(slot);
    for (size_t i = 0; i < d.frameBytes(); i += 2) {
        p[i] = 128;     // U/V neutral
        p[i + 1] = 16;  // Y black
    }
    const uint64_t v = placeholderRing_->submit(slot);
    if (!placeholderRing_->timeline().waitCompleted(v, 1'000'000'000)) return false;
    placeholder_ = std::make_shared<const gpu::GpuFrame>(placeholderRing_, slot, v);
    return true;
}

bool Engine::buildLabelAtlas() {
    // A label may span either half of the multiview (a single input or one of
    // the output monitors), so its background row must cover that full width.
    const int rowW = std::max(512, comp_->mvWidth() / 2);
    const int rowH = gpu::Compositor::kLabelRowH;
    const int rows = 2 + int(cfg_.inputs.size());
    std::vector<uint8_t> pixels(size_t(rowW) * rowH * rows * 4);
    std::vector<int> used(static_cast<size_t>(rows));

    auto renderRow = [&](int row, const std::string& text) {
        used[size_t(row)] =
            font::renderLabel(text, pixels.data() + size_t(row) * rowW * rowH * 4,
                              rowW, rowH);
    };
    // The text is baked into the wall: the OMT multiview sender and the
    // web GUI's MJPEG stream both show this image as-is, so the labels must
    // be legible in the frame itself. Each strip spans its full cell width
    // (the tile samples the whole row, text left-aligned).
    renderRow(0, "PROGRAM");
    renderRow(1, "PREVIEW");
    used[0] = rowW;
    used[1] = rowW;
    {
        std::lock_guard lk(replaceM_);
        for (size_t i = 0; i < cfg_.inputs.size(); ++i) {
            renderRow(2 + int(i), inputLabel(cfg_.inputs[i], int(i)));
            used[2 + i] = rowW;
        }
    }

    gpu::Image atlas = vk_.createImage2D(
        uint32_t(rowW), uint32_t(rowH * rows), VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    gpu::Buffer staging = vk_.createBuffer(
        pixels.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (!atlas.img || !staging.buf) return false;
    memcpy(staging.mapped, pixels.data(), pixels.size());

    // One-shot upload on the gfx queue.
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = gfxPool_;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(vk_.device(), &cai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = atlas.img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);

    VkBufferImageCopy2 region{VK_STRUCTURE_TYPE_BUFFER_IMAGE_COPY_2};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {uint32_t(rowW), uint32_t(rowH * rows), 1};
    VkCopyBufferToImageInfo2 ci{VK_STRUCTURE_TYPE_COPY_BUFFER_TO_IMAGE_INFO_2};
    ci.srcBuffer = staging.buf;
    ci.dstImage = atlas.img;
    ci.dstImageLayout = VK_IMAGE_LAYOUT_GENERAL;
    ci.regionCount = 1;
    ci.pRegions = &region;
    vkCmdCopyBufferToImage2(cmd, &ci);
    vkEndCommandBuffer(cmd);

    gpu::Timeline tl = vk_.createTimeline();
    const uint64_t v = tl.reserve();
    const VkSemaphoreSubmitInfo sig = gpu::VkEngine::timelineSignal(
        tl, v, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
    gpu::VkEngine::SubmitDesc sd;
    sd.cmd = cmd;
    sd.signalInfos = {&sig, 1};
    vk_.submit(vk_.gfx(), sd);
    const bool ok = tl.waitCompleted(v, 2'000'000'000);
    vk_.destroyTimeline(tl);
    vk_.destroyBuffer(staging);
    vkFreeCommandBuffers(vk_.device(), gfxPool_, 1, &cmd);
    if (!ok) return false;

    comp_->setLabelAtlas(atlas, std::move(used));
    return true;
}

void Engine::stop() {
    if (!started_) return;
    renderThread_ = {};  // request stop + join
    // End the file at the final rendered frame. Input teardown can take a
    // receive timeout or two; leaving the recorder attached to the audio
    // mixer during that interval would append seconds of audio-only tail.
    auto recorder = recorder_.exchange({}, std::memory_order_acq_rel);
    auto cleanRecorder =
        cleanRecorder_.exchange({}, std::memory_order_acq_rel);
    while (recorder && recorder.use_count() > 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (cleanRecorder && cleanRecorder.use_count() > 1)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    recorder.reset();
    cleanRecorder.reset();
    inputs_.clear();     // capture threads stop writing the audio rings
    audio_.reset();      // mixer stops; no more sink calls into the outputs
    srtOut_.reset();     // drains encoder, releases CUDA imports (device alive)
    cleanSdiOut_.reset();
    sdiOut_.reset();     // stops playback before the pack buffers go away
    mvOmtOut_.reset();
    cleanOmtOut_.reset();
    omtOut_.reset();     // stops sender before its buffers go away
    if (renderTL_.lastReserved())
        renderTL_.waitCompleted(renderTL_.lastReserved(), 2'000'000'000);
    vkDeviceWaitIdle(vk_.device());
    for (auto& r : retention_) r.clear();
    placeholder_.reset();
    placeholderRing_.reset();
    if (gfxPool_) vkDestroyCommandPool(vk_.device(), gfxPool_, nullptr);
    if (downPool_) vkDestroyCommandPool(vk_.device(), downPool_, nullptr);
    gfxPool_ = downPool_ = VK_NULL_HANDLE;
    vk_.destroyTimeline(renderTL_);
    vk_.destroyTimeline(readbackTL_);
    comp_.reset();
    vk_.destroy();
    cuda_.destroy();
    started_ = false;
    KLOUD_LOGI("engine stopped");
}

bool Engine::copyMultiview(std::vector<uint8_t>& out, uint64_t& lastSeq, int& w,
                           int& h) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        const uint64_t v = mvReady_.load(std::memory_order_acquire);
        if (!v || v == lastSeq) return false;
        const int slot = int(v % gpu::Compositor::kReadbackSlots);
        out.resize(comp_->readbackBytes());
        memcpy(out.data(), comp_->readbackPtr(slot), out.size());
        if (rbStamp_[slot].load(std::memory_order_acquire) == v) {
            lastSeq = v;
            w = comp_->mvWidth();
            h = comp_->mvHeight();
            return true;
        }
    }
    return false;
}

void Engine::renderLoop(std::stop_token st) {
    const int N = int(inputs_.size());
    std::vector<std::shared_ptr<const gpu::GpuFrame>> cur(N);
    std::vector<uint64_t> seq(N, 0);
    std::vector<int64_t> lastNewTick(N, -1000000);
    const int64_t staleTicks = 2 * cfg_.show.fpsN / cfg_.show.fpsD;  // ~2s

    auto& tickCtr = Stats::counter("render.ticks");
    auto& skipCtr = Stats::counter("render.skips");
    auto& lateWaitCtr = Stats::counter("render.gpuLateWaits");
    auto& packSkipCtr = Stats::counter("out.omt.packSlotBusy");
    auto& cleanPackSkipCtr =
        Stats::counter("out.omt.clean.packSlotBusy");
    auto& mvPackSkipCtr = Stats::counter("out.omt.mv.packSlotBusy");
    std::vector<Stats::Counter*> repeatCtr(static_cast<size_t>(N));
    std::vector<Stats::Counter*> burstCtr(static_cast<size_t>(N));
    std::vector<Stats::Counter*> lateCtr(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        // repeats: tick reused the previous frame (nothing usable was newer);
        // mailboxSkips: source outpaced us, latest-frame policy passed frames by;
        // lateFallbacks: newest upload still in flight, showed the previous
        // publish instead (the marginal-phase absorber -- without it a bad
        // source/tick phase judders as steady repeat+skip pairs).
        repeatCtr[size_t(i)] =
            &Stats::counter("in" + std::to_string(i) + ".repeats");
        burstCtr[size_t(i)] =
            &Stats::counter("in" + std::to_string(i) + ".mailboxSkips");
        lateCtr[size_t(i)] =
            &Stats::counter("in" + std::to_string(i) + ".lateFallbacks");
    }

    // Frame-sync state (docs/design-framesync.md): one scheduler per
    // sync-enabled input, render-thread-owned, rebuilt on cadence change.
    // Stats counters are cumulative across rebuilds/replaces via the base.
    using EngineSync = FrameSync<IInputSource::FramePtr>;
    std::vector<std::unique_ptr<EngineSync>> sync(static_cast<size_t>(N));
    std::vector<EngineSync::Counters> syncBase(static_cast<size_t>(N));
    std::vector<int> syncFramesCfg(static_cast<size_t>(N), -1);
    struct SyncCtrs {
        Stats::Counter *starves, *waits, *lateUploads, *slipDrops, *overflows,
            *resyncs, *depth, *delayUs, *trimUs, *trimClamped;
    };
    std::vector<SyncCtrs> syncCtr(static_cast<size_t>(N));
    for (int i = 0; i < N; ++i) {
        syncFramesCfg[size_t(i)] = cfg_.inputs[size_t(i)].syncFrames;
        const std::string p = "in" + std::to_string(i) + ".sync.";
        syncCtr[size_t(i)] = {
            &Stats::counter(p + "starves"),   &Stats::counter(p + "waits"),
            &Stats::counter(p + "lateUploads"), &Stats::counter(p + "slipDrops"),
            &Stats::counter(p + "overflows"), &Stats::counter(p + "resyncs"),
            &Stats::counter(p + "depth"),     &Stats::counter(p + "videoDelayUs"),
            &Stats::counter(p + "autoTrimUs"),
            &Stats::counter(p + "trimClamped")};
    }
    // Path-tail asymmetry between the video measurement point (composite
    // tick) and the audio one (mixer ring pull): mixer chunk quantization
    // and sink fan-out. Calibrated with flash+tone against the TS-capture
    // ground truth: N=1 network-in draws centered -5.5 ms vs the v1 SRT-path
    // center of -3.8 ms (docs/bench-framesync.md).
    constexpr int64_t kSyncTrimBiasNs = 1'700'000;
    auto foldSyncBase = [](EngineSync::Counters& b, const EngineSync::Counters& c) {
        b.starves += c.starves;
        b.waits += c.waits;
        b.lateUploads += c.lateUploads;
        b.slipDrops += c.slipDrops;
        b.overflows += c.overflows;
        b.resyncs += c.resyncs;
    };

    uint64_t lastTallyKey = ~0ull;
    uint64_t seenRecorderGeneration = 0;
    uint64_t seenCleanRecorderGeneration = 0;

    // The clock was started in start() (audio shares the origin); the first
    // few ticks may already be past, which sleepUntilTick treats as "go now".
    int64_t n = clock_.currentTick();

    while (!st.stop_requested()) {
        clock_.sleepUntilTick(n);

        // -- commands --
        Command c;
        while (cmds_.pop(c)) {
            switch (c.type) {
                case Command::Type::SetProgram: switcher_.setProgram(c.arg); break;
                case Command::Type::SetPreview: switcher_.setPreview(c.arg); break;
                case Command::Type::Cut: switcher_.cut(); break;
                case Command::Type::Auto: switcher_.autoTransition(n); break;
                case Command::Type::TbarBegin: switcher_.tbarBegin(); break;
                case Command::Type::TbarSet: switcher_.tbarSet(c.farg); break;
                case Command::Type::TbarEnd: switcher_.tbarEnd(); break;
                case Command::Type::FadeToBlack: switcher_.fadeToBlack(); break;
                case Command::Type::SetTransition:
                    switcher_.setTransition(TransitionType(c.arg), c.arg2, c.farg);
                    break;
                case Command::Type::DskToggle: switcher_.dskToggle(c.arg); break;
                case Command::Type::SetDskSource:
                    switcher_.setDskSource(c.arg, c.arg2);
                    break;
                case Command::Type::SetDskFade:
                    switcher_.setDskDuration(c.arg, c.arg2);
                    break;
                case Command::Type::SetDskTie:
                    switcher_.setDskTie(c.arg, c.arg2 != 0);
                    break;
                case Command::Type::SetDskAudioFollow:
                    switcher_.setDskAudioFollow(c.arg, c.arg2 != 0);
                    break;
                case Command::Type::MediaSetPlaying:
                    if (c.arg >= 0 && c.arg < N) {
                        inputs_[size_t(c.arg)]->setMediaPlaying(c.arg2 != 0);
                        std::lock_guard lock(replaceM_);
                        cfg_.inputs[size_t(c.arg)].mediaPlaying = c.arg2 != 0;
                    }
                    break;
                case Command::Type::MediaRestart:
                    if (c.arg >= 0 && c.arg < N) {
                        inputs_[size_t(c.arg)]->restartMedia();
                        std::lock_guard lock(replaceM_);
                        cfg_.inputs[size_t(c.arg)].mediaPlaying = true;
                    }
                    break;
                case Command::Type::MediaSetLoop:
                    if (c.arg >= 0 && c.arg < N) {
                        inputs_[size_t(c.arg)]->setMediaLoop(c.arg2 != 0);
                        std::lock_guard lock(replaceM_);
                        cfg_.inputs[size_t(c.arg)].mediaLoop = c.arg2 != 0;
                    }
                    break;
                case Command::Type::MediaStep:
                    if (c.arg >= 0 && c.arg < N)
                        inputs_[size_t(c.arg)]->stepMedia(c.arg2);
                    break;
            }
        }

        // -- source picker: swap inputs between ticks --
        {
            std::vector<std::pair<int, InputSpec>> reps;
            {
                std::lock_guard lk(replaceM_);
                reps.swap(pendingReplace_);
            }
            bool relabel = false;
            for (auto& [idx, spec] : reps) {
                if (idx < 0 || idx >= N) continue;
                if ((spec.type == InputSpec::Type::Srt ||
                     spec.type == InputSpec::Type::Media) &&
                    !cuda_.ok()) {
                    // One-time on-demand interop bring-up (a user action; the
                    // stall is bounded and counted as skips if it overruns).
                    if (!vk_.hasExternalMemoryFd || !cuda_.init(vk_.deviceUuid())) {
                        KLOUD_LOGE("in%d: SRT/media source needs Vulkan/CUDA "
                                 "interop; replace refused", idx);
                        continue;
                    }
                }
                auto old = std::move(inputs_[size_t(idx)]);
                auto& q = (idx % 2) ? vk_.xferDown() : vk_.xferUp();
                // Unassigned (the picker's BLACK) tears the source down
                // without starting another one -- see start().
                if (spec.ref.empty())
                    inputs_[size_t(idx)] = std::make_unique<NullInput>();
                else if (spec.type == InputSpec::Type::Srt)
                    inputs_[size_t(idx)] = std::make_unique<SrtInput>(
                        vk_, q, cuda_, spec.ref, idx, spec.syncFrames);
                else if (spec.type == InputSpec::Type::Media)
                    inputs_[size_t(idx)] = std::make_unique<MediaInput>(
                        vk_, q, cuda_, spec.ref, idx, spec.syncFrames,
                        spec.mediaPlaying, spec.mediaLoop,
                        spec.mediaPlaylist);
                else if (spec.type == InputSpec::Type::Still)
                    inputs_[size_t(idx)] = std::make_unique<StillInput>(
                        vk_, q, spec.ref, idx, cfg_.show.fpsN,
                        cfg_.show.fpsD);
#ifdef KLOUD_HAVE_OMT
                else if (spec.type == InputSpec::Type::Omt)
                    inputs_[size_t(idx)] = std::make_unique<OmtInput>(
                        vk_, q, spec.ref, idx, spec.syncFrames, cfg_.show);
#endif
#ifdef KLOUD_HAVE_DECKLINK
                else if (spec.type == InputSpec::Type::DeckLink)
                    inputs_[size_t(idx)] = std::make_unique<DeckLinkInput>(
                        vk_, q, spec.ref, idx, spec.syncFrames);
#endif
                else {
                    KLOUD_LOGE("in%d: transport for '%s' was not built in; "
                             "input will stay dark", idx, spec.ref.c_str());
                    inputs_[size_t(idx)] = std::make_unique<NullInput>();
                }
                if (audio_) {
                    inputs_[size_t(idx)]->attachAudioSink(&audio_->channel(idx));
                    // New source: drop the auto trim now (the lane restarts
                    // silent, no need to slew away from the old source's).
                    audio_->channel(idx).autoDelayFrames.store(
                        0, std::memory_order_relaxed);
                    audio_->channel(idx).syncManaged.store(
                        spec.syncFrames >= 0, std::memory_order_relaxed);
                    audio_->channel(idx).autoDelayGen.fetch_add(
                        1, std::memory_order_relaxed);
                }
                cur[size_t(idx)].reset();  // placeholder until first frame
                seq[size_t(idx)] = 0;
                lastNewTick[size_t(idx)] = n - staleTicks - 1;
                if (sync[size_t(idx)]) {
                    foldSyncBase(syncBase[size_t(idx)],
                                 sync[size_t(idx)]->counters());
                    sync[size_t(idx)].reset();
                }
                syncFramesCfg[size_t(idx)] = spec.syncFrames;
                {
                    std::lock_guard lk(replaceM_);
                    cfg_.inputs[size_t(idx)] = spec;
                }
                relabel = true;
                const char* kind =
                    spec.type == InputSpec::Type::Srt        ? " (srt)"
                    : spec.type == InputSpec::Type::Media    ? " (media)"
                    : spec.type == InputSpec::Type::Still    ? " (still)"
                    : spec.type == InputSpec::Type::DeckLink ? " (sdi)"
                                                             : " (omt)";
                KLOUD_LOGI("in%d: replaced with '%s'%s", idx, spec.ref.c_str(),
                         kind);
                // The dtor joins the capture thread (bounded by its receive
                // timeout) -- never on the render thread.
                std::thread([o = std::move(old)]() mutable { o.reset(); })
                    .detach();
            }
            if (relabel) {
                // The old atlas may still be sampled by in-flight ticks; a
                // few-ms drain on a user action is fine (counted if it skips).
                if (renderTL_.lastReserved())
                    renderTL_.waitCompleted(renderTL_.lastReserved(),
                                            1'000'000'000);
                if (!buildLabelAtlas()) KLOUD_LOGE("label atlas rebuild failed");
                lastTallyKey = ~0ull;  // re-send tally to the new source
            }
        }

        const CompositeJob job = switcher_.tick(n);

        if (audio_)  // audio follows video: crossfade along alpha, FTB dip
            audio_->publishMix(job.programSrc, job.previewSrc, job.alpha, job.ftb);

        {
            std::lock_guard lk(uiM_);
            ui_ = {switcher_.program(),
                   switcher_.preview(),
                   job.transitionActive,
                   switcher_.ftbEngaged(),
                   job.ftb,
                   {job.dskOn[0], job.dskOn[1]},
                   {job.dskLevel[0], job.dskLevel[1]},
                   {job.dskSrc[0], job.dskSrc[1]},
                   {job.dskTie[0], job.dskTie[1]},
                   {job.dskAudioFollow[0], job.dskAudioFollow[1]},
                   int(switcher_.transitionType()),
                   int(switcher_.transitionDuration()),
                   switcher_.transitionSoftness(),
                   {int(switcher_.dskDuration(0)),
                    int(switcher_.dskDuration(1))}};
        }

        // -- tally to sources (both buses are hot during a transition; a
        //    DSK source is program while engaged or still fading out) --
        const int tPgmA = job.programSrc;
        const int tPgmB =
            (job.transitionActive || job.alpha > 0.f) ? job.previewSrc : -1;
        const int tPvw = job.previewSrc;
        int tDsk[kDskCount];
        for (int k = 0; k < kDskCount; ++k)
            tDsk[k] = (job.dskOn[k] || job.dskLevel[k] > 0.f) ? job.dskSrc[k]
                                                              : -1;
        const uint64_t tallyKey = uint64_t(uint32_t(tPgmA + 1)) |
                                  (uint64_t(uint32_t(tPgmB + 1)) << 10) |
                                  (uint64_t(uint32_t(tPvw + 1)) << 20) |
                                  (uint64_t(uint32_t(tDsk[0] + 1)) << 30) |
                                  (uint64_t(uint32_t(tDsk[1] + 1)) << 40);
        if (tallyKey != lastTallyKey) {
            for (int i = 0; i < N; ++i)
                inputs_[size_t(i)]->setTally(
                    i == tPgmA || i == tPgmB || i == tDsk[0] || i == tDsk[1],
                    i == tPvw);
            lastTallyKey = tallyKey;
        }

        // -- refresh inputs. Sync-off (v1 policy): newest COMPLETED upload
        //    from the mailbox, falling back up to two publishes while the
        //    newest DMA is in flight. Sync-on: drain the timestamped feed
        //    into the scheduler and present whatever is due at this tick. --
        for (int i = 0; i < N; ++i) {
            if (auto* feed = inputs_[size_t(i)]->syncFeed()) {
                IInputSource::TimedFrame tf;
                while (feed->pop(tf)) {
                    const auto& fd = tf.frame->desc;
                    const int64_t ts = 1'000'000'000LL * fd.fpsD / fd.fpsN;
                    auto& fs = sync[size_t(i)];
                    if (!fs || fs->config().framePeriodNs != ts) {
                        if (fs) foldSyncBase(syncBase[size_t(i)], fs->counters());
                        fs = std::make_unique<EngineSync>(EngineSync::Config{
                            ts, std::max(0, syncFramesCfg[size_t(i)])});
                    }
                    fs->push(std::move(tf.frame), tf.seq, tf.srcPtsNs, tf.arrNs,
                             tf.senderClock);
                }
                if (auto& fs = sync[size_t(i)]) {
                    auto f = fs->present(
                        clock_.nsForTick(n),
                        [](const IInputSource::FramePtr& p) {
                            return p && p->uploaded();
                        });
                    if (f) {
                        cur[size_t(i)] = std::move(*f);
                        lastNewTick[size_t(i)] = n;
                    } else if (cur[size_t(i)]) {
                        repeatCtr[size_t(i)]->add();
                    }
                    const auto& b = syncBase[size_t(i)];
                    const auto& c = fs->counters();
                    const auto& sc = syncCtr[size_t(i)];
                    sc.starves->set(b.starves + c.starves);
                    sc.waits->set(b.waits + c.waits);
                    sc.lateUploads->set(b.lateUploads + c.lateUploads);
                    sc.slipDrops->set(b.slipDrops + c.slipDrops);
                    sc.overflows->set(b.overflows + c.overflows);
                    sc.resyncs->set(b.resyncs + c.resyncs);
                    sc.depth->set(fs->depth());
                    sc.delayUs->set(fs->locked() ? fs->videoDelayNs() / 1000
                                                 : 0);
                    // Auto A/V trim: mirror the realized video re-timing
                    // into the input's audio lane (shared-pts domains only).
                    if (audio_ && fs->locked() && fs->senderClock()) {
                        auto& ch = audio_->channel(i);
                        const int64_t apm =
                            ch.playoutMinusPtsNs(clock_.nsForTick(n));
                        if (apm != audio::InputChannel::kNoPts) {
                            const int64_t raw =
                                fs->presentMinusPtsNs() - apm + kSyncTrimBiasNs;
                            // Negative = audio already later than the video:
                            // undelayable; needs syncFrames headroom instead.
                            if (raw < 0) sc.trimClamped->set(-raw / 1000);
                            const int64_t trimNs =
                                std::clamp(raw, int64_t(0), int64_t(500'000'000));
                            const int tf =
                                int(trimNs * audio::kSampleRate / 1'000'000'000);
                            ch.autoDelayFrames.store(tf,
                                                     std::memory_order_relaxed);
                            sc.trimUs->set(trimNs / 1000);
                        }
                    }
                }
            } else {
                IInputSource::Mailbox::Item cand[IInputSource::Mailbox::kKeep];
                const int nc =
                    inputs_[size_t(i)]->newerCandidates(seq[size_t(i)], cand);
                const IInputSource::Mailbox::Item* take = nullptr;
                for (int k = 0; k < nc; ++k) {
                    if (cand[k].value && (*cand[k].value).uploaded()) {
                        take = &cand[k];
                        if (k > 0) lateCtr[size_t(i)]->add();
                        break;
                    }
                }
                if (take) {
                    if (seq[size_t(i)] && take->seq > seq[size_t(i)] + 1)
                        burstCtr[size_t(i)]->add(
                            int64_t(take->seq - seq[size_t(i)] - 1));
                    cur[size_t(i)] = take->value;
                    seq[size_t(i)] = take->seq;
                    lastNewTick[size_t(i)] = n;
                } else if (cur[size_t(i)] &&
                           !inputs_[size_t(i)]->frameIsPersistent()) {
                    repeatCtr[size_t(i)]->add();
                }
            }
            if (cur[size_t(i)] &&
                !inputs_[size_t(i)]->frameIsPersistent() &&
                n - lastNewTick[size_t(i)] > staleTicks)
                cur[size_t(i)].reset();  // no signal -> placeholder
        }

        auto pick = [&](int idx) -> const gpu::GpuFrame* {
            if (idx >= 0 && idx < N && cur[size_t(idx)]) return cur[size_t(idx)].get();
            return placeholder_.get();
        };

        gpu::Compositor::TickJob tj;
        tj.a = pick(job.programSrc);
        tj.b = pick(job.previewSrc);
        tj.sw = job;
        tj.tallyPgmA = tPgmA;
        tj.tallyPgmB = tPgmB;
        tj.tallyPvw = tPvw;
        tj.mvInputs.resize(size_t(N));
        for (int i = 0; i < N; ++i) tj.mvInputs[size_t(i)].frame = pick(i);
        // DSK fill frames. A dead/stale source picks the placeholder --
        // force that keyer dark instead of overlaying opaque black; the
        // overlay returns with the signal (state machine untouched).
        for (int k = 0; k < kDskCount; ++k) {
            const gpu::GpuFrame* f = pick(job.dskSrc[k]);
            if (f == placeholder_.get()) {
                tj.dsk[k] = nullptr;
                tj.sw.dskLevel[k] = 0.f;
            } else {
                tj.dsk[k] = f;
            }
            // Look-ahead preview: keyers at their post-transition state
            // (dark ones stay dark -- same guard as above).
            tj.pvwDskLevel[k] =
                (tj.dsk[k] && job.dskFutureOn[k]) ? 1.f : 0.f;
            tj.tallyDsk[k] = tDsk[k];
        }
        if (audio_)  // audio-follow-DSK rides the guarded on-screen levels
            audio_->publishDsk(
                job.dskAudioFollow[0] ? job.dskSrc[0] : -1,
                job.dskAudioFollow[0] ? tj.sw.dskLevel[0] : 0.f,
                job.dskAudioFollow[1] ? job.dskSrc[1] : -1,
                job.dskAudioFollow[1] ? tj.sw.dskLevel[1] : 0.f);

        // -- frame slot: wait out the submission from 2 ticks ago --
        const uint64_t value = renderTL_.reserve();
        (void)readbackTL_.reserve();  // keep both timelines on the same numbering
        const int fif = int(value % gpu::Compositor::kFramesInFlight);
        if (fifValues_[fif]) {
            while (!renderTL_.waitCompleted(fifValues_[fif], 500'000'000)) {
                lateWaitCtr.add();
                KLOUD_LOGW("render: GPU >500ms behind (value %llu)",
                         (unsigned long long)fifValues_[fif]);
                if (st.stop_requested()) return;
            }
        }
        if (downValues_[fif]) {
            while (!readbackTL_.waitCompleted(downValues_[fif], 500'000'000)) {
                lateWaitCtr.add();
                if (st.stop_requested()) return;
            }
        }
        retention_[fif].clear();

        // -- pick a pack slot for the senders (skip if the ring is busy).
        //    Acquiring the slot here holds it until the stamp is published
        //    below, so no sender can pin the buffer we are about to fill. --
        const auto pickPackSlot = [&](bool wanted,
                                      gpu::Compositor::Feed feed,
                                      Stats::Counter& busyCounter) {
            if (!wanted) return -1;
            const uint64_t rbDone = readbackTL_.completed();
            for (int s = 0; s < gpu::Compositor::kPackSlots; ++s) {
                if (comp_->packStamp(s, feed).load(
                        std::memory_order_acquire) > rbDone)
                    continue;  // DMA still in flight
                if (comp_->packTryAcquireWrite(s, feed)) return s;
            }
            busyCounter.add();
            return -1;
        };
        const int packSlot = pickPackSlot(omtOut_ || sdiOut_,
                                          gpu::Compositor::Feed::Program,
                                          packSkipCtr);
        const int cleanPackSlot = pickPackSlot(cleanOmtOut_ || cleanSdiOut_,
                                               gpu::Compositor::Feed::Clean,
                                               cleanPackSkipCtr);
        const int mvPackSlot = pickPackSlot(mvOmtOut_ != nullptr,
                                            gpu::Compositor::Feed::Multiview,
                                            mvPackSkipCtr);
        tj.packProgram = packSlot >= 0;
        tj.packClean = cleanPackSlot >= 0;
        tj.packMultiview = mvPackSlot >= 0;

        // -- Shared NV12 pack for SRT and recording: only when every active
        //    consumer has released this FIF's buffer. A slow consumer costs
        //    encoded-output frames, never render ticks. --
        const auto recorder = recorder_.load(std::memory_order_acquire);
        const uint64_t recorderGeneration =
            recorderGeneration_.load(std::memory_order_acquire);
        if (recorderGeneration != seenRecorderGeneration) {
            recordNvPushed_.fill(0);
            seenRecorderGeneration = recorderGeneration;
        }
        const auto cleanRecorder =
            cleanRecorder_.load(std::memory_order_acquire);
        const uint64_t cleanRecorderGeneration =
            cleanRecorderGeneration_.load(std::memory_order_acquire);
        if (cleanRecorderGeneration != seenCleanRecorderGeneration) {
            cleanRecordNvPushed_.fill(0);
            seenCleanRecorderGeneration = cleanRecorderGeneration;
        }
        bool doNv = srtOut_ || recorder;
        if (srtOut_ &&
            srtOut_->copiedValue(fif) < srtNvPushed_[fif]) {
            doNv = false;
            Stats::counter("out.srt.fifBusySkips").add();
        }
        if (recorder &&
            recorder->copiedValue(fif) < recordNvPushed_[fif]) {
            doNv = false;
            Stats::counter("record.fifBusySkips").add();
        }
        tj.packNv12 = doNv;
        bool doCleanNv = bool(cleanRecorder);
        if (cleanRecorder &&
            cleanRecorder->copiedValue(fif) <
                cleanRecordNvPushed_[fif]) {
            doCleanNv = false;
            Stats::counter("cleanRecord.fifBusySkips").add();
        }
        tj.packCleanNv12 = doCleanNv;

        const int rbSlot = int(value % gpu::Compositor::kReadbackSlots);
        rbStamp_[rbSlot].store(value, std::memory_order_release);

        VkCommandBuffer cmd = cmdBufs_[fif];
        vkResetCommandBuffer(cmd, 0);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(cmd, &bi);
        comp_->record(cmd, tj, fif, rbSlot);
        vkEndCommandBuffer(cmd);

        std::vector<VkSemaphoreSubmitInfo> waits;
        auto addWait = [&](const gpu::GpuFrame* f) {
            if (!f) return;
            for (const auto& w : waits)
                if (w.semaphore == f->timeline().handle() &&
                    w.value >= f->uploadValue)
                    return;
            waits.push_back(gpu::VkEngine::timelineWait(
                f->timeline(), f->uploadValue,
                VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT));
        };
        addWait(tj.a);
        addWait(tj.b);
        for (auto& s : tj.mvInputs) addWait(s.frame);
        for (const auto* f : tj.dsk) addWait(f);  // dedup'd above

        const VkSemaphoreSubmitInfo signal = gpu::VkEngine::timelineSignal(
            renderTL_, value, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT);
        gpu::VkEngine::SubmitDesc sd;
        sd.cmd = cmd;
        sd.waits = waits;
        sd.signalInfos = {&signal, 1};
        if (vk_.submit(vk_.gfx(), sd) != VK_SUCCESS) KLOUD_LOGE("render submit failed");
        fifValues_[fif] = value;

        if (doNv && srtOut_ && srtOut_->push({value, n, fif}))
            srtNvPushed_[fif] = value;
        if (doNv && recorder && recorder->push({value, n, fif}))
            recordNvPushed_[fif] = value;
        if (doCleanNv && cleanRecorder &&
            cleanRecorder->push({value, n, fif}))
            cleanRecordNvPushed_[fif] = value;

        // -- pack readback on the down queue (chained on this render) --
        if (packSlot >= 0 || cleanPackSlot >= 0 || mvPackSlot >= 0) {
            // The senders timestamp this frame with the tick's presentation
            // time, so the readback lag never skews their A/V; the packStamp
            // release is what publishes the pts, and dropping the writer bit
            // after it is what lets a sender pin the slot.
            const int64_t tickNs = clock_.nsForTick(n);
            if (packSlot >= 0) {
                if (omtOut_) omtOut_->stampSlot(packSlot, tickNs);
                if (sdiOut_) sdiOut_->stampSlot(packSlot, tickNs);
                comp_->packStamp(packSlot, gpu::Compositor::Feed::Program)
                    .store(value, std::memory_order_release);
                comp_->packReleaseWrite(packSlot,
                                        gpu::Compositor::Feed::Program);
            }
            if (cleanPackSlot >= 0) {
                if (cleanOmtOut_) cleanOmtOut_->stampSlot(cleanPackSlot, tickNs);
                if (cleanSdiOut_) cleanSdiOut_->stampSlot(cleanPackSlot, tickNs);
                comp_->packStamp(cleanPackSlot,
                                 gpu::Compositor::Feed::Clean)
                    .store(value, std::memory_order_release);
                comp_->packReleaseWrite(cleanPackSlot,
                                        gpu::Compositor::Feed::Clean);
            }
            if (mvPackSlot >= 0) {
                mvOmtOut_->stampSlot(mvPackSlot, tickNs);
                comp_->packStamp(mvPackSlot,
                                 gpu::Compositor::Feed::Multiview)
                    .store(value, std::memory_order_release);
                comp_->packReleaseWrite(mvPackSlot,
                                        gpu::Compositor::Feed::Multiview);
            }
            VkCommandBuffer dcmd = downBufs_[fif];
            vkResetCommandBuffer(dcmd, 0);
            vkBeginCommandBuffer(dcmd, &bi);
            if (packSlot >= 0)
                comp_->recordDownCopy(
                    dcmd, fif, packSlot,
                    gpu::Compositor::Feed::Program);
            if (cleanPackSlot >= 0)
                comp_->recordDownCopy(
                    dcmd, fif, cleanPackSlot,
                    gpu::Compositor::Feed::Clean);
            if (mvPackSlot >= 0)
                comp_->recordDownCopy(
                    dcmd, fif, mvPackSlot,
                    gpu::Compositor::Feed::Multiview);
            vkEndCommandBuffer(dcmd);

            const VkSemaphoreSubmitInfo dwait = gpu::VkEngine::timelineWait(
                renderTL_, value, VK_PIPELINE_STAGE_2_COPY_BIT);
            const VkSemaphoreSubmitInfo dsig = gpu::VkEngine::timelineSignal(
                readbackTL_, value, VK_PIPELINE_STAGE_2_COPY_BIT);
            gpu::VkEngine::SubmitDesc dsd;
            dsd.cmd = dcmd;
            dsd.waits = {&dwait, 1};
            dsd.signalInfos = {&dsig, 1};
            if (vk_.submit(vk_.xferDown(), dsd) != VK_SUCCESS)
                KLOUD_LOGE("down submit failed");
            downValues_[fif] = value;
        }

        auto& keep = retention_[fif];
        for (int i = 0; i < N; ++i)
            if (cur[size_t(i)]) keep.push_back(cur[size_t(i)]);
        keep.push_back(placeholder_);

        mvReady_.store(renderTL_.completed(), std::memory_order_release);

        ticks_.fetch_add(1, std::memory_order_relaxed);
        tickCtr.add();

        int64_t next = n + 1;
        const int64_t curTick = clock_.currentTick();
        if (curTick > next) {
            skips_.fetch_add(curTick - next, std::memory_order_relaxed);
            skipCtr.add(curTick - next);
            next = curTick;
        }
        n = next;
    }
}

}  // namespace kloud
