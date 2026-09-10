/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "app/ShowFile.h"

#include <sys/stat.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>

#include "core/Ini.h"

namespace kloud::app {

namespace {

std::string defaultShowPath() {
    std::string base;
    if (const char* xdg = getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        base = xdg;
    else if (const char* home = getenv("HOME"); home && *home)
        base = std::string(home) + "/.config";
    else
        base = ".";
    return base + "/8KloudSwitcher/show.ini";
}

int64_t toInt64(const std::string& s, int64_t def) {
    char* end = nullptr;
    const long long v = strtoll(s.c_str(), &end, 10);
    return (end == s.c_str() || *end) ? def : v;
}

}  // namespace

bool ShowFile::State::cfgEquals(const EngineConfig& a, const EngineConfig& b) {
    if (a.inputs.size() != b.inputs.size()) return false;
    for (size_t i = 0; i < a.inputs.size(); ++i)
        if (a.inputs[i].type != b.inputs[i].type ||
            a.inputs[i].ref != b.inputs[i].ref ||
            a.inputs[i].syncFrames != b.inputs[i].syncFrames ||
            a.inputs[i].mediaLoop != b.inputs[i].mediaLoop ||
            a.inputs[i].mediaPlaylist != b.inputs[i].mediaPlaylist)
            return false;
    return a.show == b.show && a.omtOut == b.omtOut &&
           a.omtOutName == b.omtOutName &&
           a.cleanOmtOut == b.cleanOmtOut &&
           a.cleanOmtOutName == b.cleanOmtOutName &&
           a.mvOmtOut == b.mvOmtOut && a.mvOmtOutName == b.mvOmtOutName &&
           a.mvW == b.mvW && a.mvH == b.mvH &&
           a.sdiOutRef == b.sdiOutRef &&
           a.cleanSdiOutRef == b.cleanSdiOutRef &&
           a.srtUrl == b.srtUrl && a.srtSend == b.srtSend &&
           a.srtBitrateKbps == b.srtBitrateKbps &&
           a.srtCodec == b.srtCodec &&
           a.srtKeyframeMs == b.srtKeyframeMs &&
           a.srtAudioKbps == b.srtAudioKbps &&
           a.recordBitrateKbps == b.recordBitrateKbps &&
           a.encoder == b.encoder && a.encoderPreset == b.encoderPreset &&
           a.audio == b.audio &&
           a.masterAudioDelayMs == b.masterAudioDelayMs;
}

ShowFile::ShowFile(std::string path) : path_(std::move(path)) {
    if (path_.empty()) path_ = defaultShowPath();
    std::error_code ec;
    std::filesystem::create_directories(
        std::filesystem::path(path_).parent_path(), ec);
}

bool ShowFile::exists() const {
    struct stat st{};
    return stat(path_.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool ShowFile::load(State& st) const {
    if (!exists()) return false;
    IniFile s;
    if (!s.load(path_)) return false;

    // Keep the defaults rather than refusing to start when a show file has
    // been hand-edited into something Engine::start would reject.
    const int w = s.getInt("show/width", st.cfg.show.width);
    const int h = s.getInt("show/height", st.cfg.show.height);
    if (w >= 2 && h >= 2 && !(w & 1) && !(h & 1) && w <= 8192 && h <= 8192) {
        st.cfg.show.width = w;
        st.cfg.show.height = h;
    }
    const int64_t fpsN = s.getInt64("show/fpsN", st.cfg.show.fpsN);
    const int64_t fpsD = s.getInt64("show/fpsD", st.cfg.show.fpsD);
    if (fpsN > 0 && fpsD > 0) {
        st.cfg.show.fpsN = fpsN;
        st.cfg.show.fpsD = fpsD;
    }
    st.cfg.show.colorimetry =
        VideoFormatDesc::colorimetryForHeight(st.cfg.show.height);
    st.cfg.omtOut = s.getBool("show/omtOut", st.cfg.omtOut);
    st.cfg.omtOutName = s.getString("show/omtOutName", st.cfg.omtOutName);
    st.cfg.cleanOmtOut = s.getBool("show/cleanOmtOut", st.cfg.cleanOmtOut);
    st.cfg.cleanOmtOutName =
        s.getString("show/cleanOmtOutName", st.cfg.cleanOmtOutName);
    st.cfg.mvOmtOut = s.getBool("show/mvOmtOut", st.cfg.mvOmtOut);
    st.cfg.mvOmtOutName = s.getString("show/mvOmtOutName", st.cfg.mvOmtOutName);
    // Guard the stored geometry: the pack ring and every OMT receiver size
    // themselves from it once, at start.
    const int mvW = s.getInt("show/mvW", st.cfg.mvW);
    const int mvH = s.getInt("show/mvH", st.cfg.mvH);
    if (mvW >= 320 && mvH >= 180 && mvW <= 7680 && mvH <= 4320 &&
        !(mvW & 1) && !(mvH & 1)) {
        st.cfg.mvW = mvW;
        st.cfg.mvH = mvH;
    }
    st.cfg.sdiOutRef = s.getString("show/sdiOut", st.cfg.sdiOutRef);
    st.cfg.cleanSdiOutRef =
        s.getString("show/cleanSdiOut", st.cfg.cleanSdiOutRef);
    st.cfg.srtUrl = s.getString("show/srtOut", st.cfg.srtUrl);
    st.cfg.srtBitrateKbps =
        s.getInt("show/srtBitrateKbps", st.cfg.srtBitrateKbps);
    media::parseVideoCodec(
        s.getString("show/srtCodec", media::videoCodecName(st.cfg.srtCodec)),
        st.cfg.srtCodec);
    // Absent in pre-0.8 show files: a stored URL was always live.
    st.cfg.srtSend = s.getBool("show/srtSend", st.cfg.srtSend);
    st.cfg.srtKeyframeMs =
        std::clamp(s.getInt("show/srtKeyframeMs", st.cfg.srtKeyframeMs), 0, 60000);
    st.cfg.srtAudioKbps =
        std::clamp(s.getInt("show/srtAudioKbps", st.cfg.srtAudioKbps), 0, 512);
    st.cfg.recordBitrateKbps =
        s.getInt("show/recordBitrateKbps", st.cfg.recordBitrateKbps);
    media::parseEncoderBackend(
        s.getString("show/encoder", media::encoderBackendName(st.cfg.encoder)),
        st.cfg.encoder);
    media::parseEncoderPreset(
        s.getString("show/encoderPreset",
                    media::encoderPresetName(st.cfg.encoderPreset)),
        st.cfg.encoderPreset);
    st.cfg.audio = s.getBool("show/audio", st.cfg.audio);
    st.cfg.masterAudioDelayMs =
        s.getInt("show/masterDelayMs", st.cfg.masterAudioDelayMs);

    const int n = s.arraySize("inputs");
    if (n > 0) st.cfg.inputs.clear();
    for (int i = 0; i < n; ++i) {
        auto key = [&](const char* k) { return IniFile::arrayKey("inputs", i, k); };
        InputSpec spec;
        // Shared with the remote-control state so the two names cannot drift.
        // Unknown types (notably "ndi" from a pre-OMT show) fall through to
        // OMT: the ref will not resolve, so the input stays black until the
        // operator re-patches it.
        spec.type = inputTypeFromName(s.getString(key("type")));
        spec.ref = s.getString(key("ref"));
        // Absent in v1 show files -> stays off (-1).
        spec.syncFrames = s.getInt(key("framesync"), spec.syncFrames);
        if (spec.syncFrames < -1 || spec.syncFrames > 4) spec.syncFrames = -1;
        spec.mediaLoop = s.getBool(key("mediaLoop"), spec.mediaLoop);
        if (spec.type == InputSpec::Type::Media) {
            const auto playlist = s.getList(key("mediaPlaylist"));
            const auto trimIns = s.getList(key("mediaTrimInMs"));
            const auto trimOuts = s.getList(key("mediaTrimOutMs"));
            const auto speeds = s.getList(key("mediaSpeedPermille"));
            for (size_t item = 0; item < playlist.size(); ++item) {
                if (playlist[item].empty()) continue;
                const int64_t inMs =
                    item < trimIns.size() ? toInt64(trimIns[item], 0) : 0;
                const int64_t outMs =
                    item < trimOuts.size() ? toInt64(trimOuts[item], 0) : 0;
                const int speedPermille =
                    item < speeds.size() ? int(toInt64(speeds[item], 1000)) : 1000;
                media::PlaylistItem clip{playlist[item], inMs, outMs,
                                         speedPermille};
                media::normalizePlaylistItem(clip);
                spec.mediaPlaylist.push_back(std::move(clip));
            }
            if (spec.mediaPlaylist.empty() && !spec.ref.empty())
                spec.mediaPlaylist.emplace_back(spec.ref);
            if (!spec.mediaPlaylist.empty())
                spec.ref = spec.mediaPlaylist.front().path;
        }
        st.cfg.inputs.push_back(std::move(spec));
    }

    st.program = s.getInt("switcher/program", st.program);
    st.preview = s.getInt("switcher/preview", st.preview);
    st.transType = s.getInt("switcher/transType", st.transType);
    st.transDurTicks = s.getInt("switcher/transDurTicks", st.transDurTicks);

    st.chans.assign(st.cfg.inputs.size(), ChannelState{});
    const int c = s.arraySize("audioChannels");
    for (int i = 0; i < c && i < int(st.chans.size()); ++i) {
        auto key = [&](const char* k) {
            return IniFile::arrayKey("audioChannels", i, k);
        };
        auto& ch = st.chans[size_t(i)];
        // Same range the control protocol enforces; a non-finite or wild
        // stored gain would otherwise go straight into the mixer.
        const double g = s.getDouble(key("gain"), ch.gain);
        if (std::isfinite(g)) ch.gain = std::clamp(float(g), 0.f, 4.f);
        ch.mute = s.getBool(key("mute"), ch.mute);
        ch.solo = s.getBool(key("solo"), ch.solo);
        ch.delayMs = s.getInt(key("delayMs"), ch.delayMs);
    }

    // Absent in pre-DSK show files -> defaults (off).
    const int nd = s.arraySize("dsk");
    for (int i = 0; i < nd && i < kDskCount; ++i) {
        auto key = [&](const char* k) { return IniFile::arrayKey("dsk", i, k); };
        auto& d = st.dsk[i];
        d.source = std::clamp(s.getInt(key("source"), d.source), 0,
                              std::max(0, int(st.cfg.inputs.size()) - 1));
        d.fadeDurTicks =
            std::clamp(s.getInt(key("fadeDurTicks"), d.fadeDurTicks), 1, 600);
        d.on = s.getBool(key("on"), d.on);
        d.tie = s.getBool(key("tie"), d.tie);
        d.audioFollow = s.getBool(key("audioFollow"), d.audioFollow);
    }
    return true;
}

bool ShowFile::save(const State& st) const {
    IniFile s;
    s.set("show/width", st.cfg.show.width);
    s.set("show/height", st.cfg.show.height);
    s.set("show/fpsN", int64_t(st.cfg.show.fpsN));
    s.set("show/fpsD", int64_t(st.cfg.show.fpsD));
    s.set("show/omtOut", st.cfg.omtOut);
    s.set("show/omtOutName", st.cfg.omtOutName);
    s.set("show/cleanOmtOut", st.cfg.cleanOmtOut);
    s.set("show/cleanOmtOutName", st.cfg.cleanOmtOutName);
    s.set("show/mvOmtOut", st.cfg.mvOmtOut);
    s.set("show/mvOmtOutName", st.cfg.mvOmtOutName);
    s.set("show/mvW", st.cfg.mvW);
    s.set("show/mvH", st.cfg.mvH);
    s.set("show/sdiOut", st.cfg.sdiOutRef);
    s.set("show/cleanSdiOut", st.cfg.cleanSdiOutRef);
    s.set("show/srtOut", st.cfg.srtUrl);
    s.set("show/srtBitrateKbps", st.cfg.srtBitrateKbps);
    s.set("show/srtCodec", media::videoCodecName(st.cfg.srtCodec));
    s.set("show/srtSend", st.cfg.srtSend);
    s.set("show/srtKeyframeMs", st.cfg.srtKeyframeMs);
    s.set("show/srtAudioKbps", st.cfg.srtAudioKbps);
    s.set("show/recordBitrateKbps", st.cfg.recordBitrateKbps);
    s.set("show/encoder", media::encoderBackendName(st.cfg.encoder));
    s.set("show/encoderPreset", media::encoderPresetName(st.cfg.encoderPreset));
    s.set("show/audio", st.cfg.audio);
    s.set("show/masterDelayMs", st.cfg.masterAudioDelayMs);

    s.setArraySize("inputs", int(st.cfg.inputs.size()));
    for (int i = 0; i < int(st.cfg.inputs.size()); ++i) {
        auto key = [&](const char* k) { return IniFile::arrayKey("inputs", i, k); };
        const auto& spec = st.cfg.inputs[size_t(i)];
        s.set(key("type"), inputTypeName(spec.type));
        s.set(key("ref"), spec.ref);
        s.set(key("framesync"), spec.syncFrames);
        if (spec.type == InputSpec::Type::Media) {
            s.set(key("mediaLoop"), spec.mediaLoop);
            std::vector<std::string> playlist, trimIns, trimOuts, speeds;
            for (const auto& item : spec.mediaPlaylist) {
                playlist.push_back(item.path);
                trimIns.push_back(std::to_string(item.inMs));
                trimOuts.push_back(std::to_string(item.outMs));
                speeds.push_back(std::to_string(item.speedPermille));
            }
            if (playlist.empty() && !spec.ref.empty()) playlist.push_back(spec.ref);
            s.setList(key("mediaPlaylist"), playlist);
            s.setList(key("mediaTrimInMs"), trimIns);
            s.setList(key("mediaTrimOutMs"), trimOuts);
            s.setList(key("mediaSpeedPermille"), speeds);
        }
        // A restored show cues media from its start and plays; pause is an
        // ephemeral transport state, not a startup mode.
    }

    s.set("switcher/program", st.program);
    s.set("switcher/preview", st.preview);
    s.set("switcher/transType", st.transType);
    s.set("switcher/transDurTicks", st.transDurTicks);

    s.setArraySize("audioChannels", int(st.chans.size()));
    for (int i = 0; i < int(st.chans.size()); ++i) {
        auto key = [&](const char* k) {
            return IniFile::arrayKey("audioChannels", i, k);
        };
        const auto& ch = st.chans[size_t(i)];
        s.set(key("gain"), double(ch.gain));
        s.set(key("mute"), ch.mute);
        s.set(key("solo"), ch.solo);
        s.set(key("delayMs"), ch.delayMs);
    }

    s.setArraySize("dsk", kDskCount);
    for (int i = 0; i < kDskCount; ++i) {
        auto key = [&](const char* k) { return IniFile::arrayKey("dsk", i, k); };
        s.set(key("source"), st.dsk[i].source);
        s.set(key("fadeDurTicks"), st.dsk[i].fadeDurTicks);
        s.set(key("on"), st.dsk[i].on);
        s.set(key("tie"), st.dsk[i].tie);
        s.set(key("audioFollow"), st.dsk[i].audioFollow);
    }
    return s.save(path_);
}

}  // namespace kloud::app
