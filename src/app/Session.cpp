/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "app/Session.h"

#include <chrono>

#include "core/Log.h"

namespace kloud::app {

Session::Session(Engine& engine, ShowFile* showFile,
                 const ShowFile::State& initial)
    : engine_(engine), showFile_(showFile), active_(initial.cfg),
      pending_(initial.cfg) {
    // The engine may have normalized what it was handed (media refs, sync on
    // stills); the live parts are re-read at collect time anyway.
    active_.show = engine.outputFormat();
    pending_.show = active_.show;
    if (showFile_) {
        lastSaved_ = initial;
        thread_ = std::jthread([this](std::stop_token st) { saver(st); });
    }
}

Session::~Session() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
}

void Session::shutdown() {
    if (thread_.joinable()) {
        thread_.request_stop();
        thread_.join();
    }
    saveIfChanged();
}

EngineConfig Session::pendingConfig() const {
    std::lock_guard lk(m_);
    return pending_;
}

void Session::setPendingConfig(const EngineConfig& cfg) {
    {
        std::lock_guard lk(m_);
        pending_ = cfg;
        pending_.show.colorimetry =
            VideoFormatDesc::colorimetryForHeight(pending_.show.height);
        dirty_ = true;
    }
    saveIfChanged();
}

bool Session::restartRequired() const {
    std::lock_guard lk(m_);
    const EngineConfig& p = pending_;
    const EngineConfig& a = active_;
    return !(p.show == a.show) || p.omtOut != a.omtOut ||
           p.omtOutName != a.omtOutName || p.cleanOmtOut != a.cleanOmtOut ||
           p.cleanOmtOutName != a.cleanOmtOutName ||
           p.mvOmtOut != a.mvOmtOut || p.mvOmtOutName != a.mvOmtOutName ||
           p.mvW != a.mvW || p.mvH != a.mvH || p.sdiOutRef != a.sdiOutRef ||
           p.cleanSdiOutRef != a.cleanSdiOutRef || p.srtUrl != a.srtUrl ||
           p.srtSend != a.srtSend ||
           p.srtBitrateKbps != a.srtBitrateKbps || p.srtCodec != a.srtCodec ||
           p.srtKeyframeMs != a.srtKeyframeMs ||
           p.srtAudioKbps != a.srtAudioKbps ||
           p.recordBitrateKbps != a.recordBitrateKbps ||
           p.encoder != a.encoder || p.encoderPreset != a.encoderPreset ||
           p.audio != a.audio;
}

ShowFile::State Session::collect() const {
    ShowFile::State state;
    {
        std::lock_guard lk(m_);
        state.cfg = pending_;
    }
    state.cfg.inputs.clear();
    for (int i = 0; i < engine_.inputCount(); ++i) {
        // The engine knows each input's true type; re-deriving it from the
        // ref would misfile scheme-less refs.
        InputSpec spec{engine_.inputType(i), engine_.inputRef(i),
                       engine_.inputSyncFrames(i)};
        const auto media = engine_.inputMediaState(i);
        if (media.available) {
            spec.mediaPlaying = media.playing;
            spec.mediaLoop = media.loop;
            spec.mediaPlaylist = engine_.inputMediaPlaylist(i);
            if (!spec.mediaPlaylist.empty())
                spec.ref = spec.mediaPlaylist.front().path;
        }
        state.cfg.inputs.push_back(std::move(spec));
    }
    const auto ui = engine_.uiState();
    state.program = ui.program;
    state.preview = ui.preview;
    state.transType = ui.transType;
    state.transDurTicks = ui.transDur;
    for (int k = 0; k < kDskCount; ++k)
        state.dsk[k] = {ui.dskSrc[k], ui.dskDur[k], ui.dskOn[k], ui.dskTie[k],
                        ui.dskAudioFollow[k]};
    if (auto* aud = engine_.audio()) {
        state.cfg.masterAudioDelayMs =
            aud->masterDelayMs.load(std::memory_order_relaxed);
        for (int i = 0; i < engine_.inputCount(); ++i) {
            if (i >= aud->inputCount()) break;
            const auto& ch = aud->channel(i);
            state.chans.push_back(
                {ch.gain.load(std::memory_order_relaxed),
                 ch.mute.load(std::memory_order_relaxed),
                 ch.solo.load(std::memory_order_relaxed),
                 ch.delayMs.load(std::memory_order_relaxed)});
        }
    } else {
        state.chans.assign(size_t(engine_.inputCount()), {});
    }
    return state;
}

void Session::saveIfChanged() {
    if (!showFile_ || !engine_.running()) return;
    ShowFile::State state = collect();
    {
        std::lock_guard lk(m_);
        if (everSaved_ && !dirty_ && state == lastSaved_) return;
        dirty_ = false;
    }
    if (!showFile_->save(state)) {
        KLOUD_LOGE("show: cannot write %s", showFile_->path().c_str());
        return;
    }
    std::lock_guard lk(m_);
    lastSaved_ = std::move(state);
    everSaved_ = true;
}

void Session::saver(std::stop_token st) {
    while (!st.stop_requested()) {
        for (int i = 0; i < 20 && !st.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (st.stop_requested()) return;
        saveIfChanged();
    }
}

}  // namespace kloud::app
