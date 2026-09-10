/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// kloud-headless: runs the switcher engine without a GUI. Verification driver
// for M1: connects OMT/SDI/SRT inputs, renders the multiview, dumps PPM frames,
// exercises cut via a scripted schedule, prints stats.

#include <csignal>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/ppm.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "core/Stats.h"
#include "ctl/ControlServer.h"
#include "decklink/DeckLinkRef.h"
#include "engine/Engine.h"
#include "media/StillImage.h"

namespace {
volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }
}  // namespace

int main(int argc, char** argv) {
    kloud::EngineConfig cfg;
    double duration = 10;
    std::string dumpDir;
    double dumpEvery = 2.0;
    bool scriptedCuts = false;
    bool scriptedAutos = false;
    std::string recordPath;
    std::string cleanRecordPath;
    double recordStopAfter = -1;
    double cleanRecordStopAfter = -1;
    bool recordStopped = false;
    bool cleanRecordStopped = false;
    int lastMediaInput = -1;  // --media-item appends to this playlist
    std::vector<std::pair<int, int>> inputDelays;  // {input, ms} audio trims
    struct Replace {
        double afterS;
        int idx;
        std::string ref;
        bool done = false;
    };
    std::vector<Replace> replaces;  // --replace-after S:IDX:REF
    std::vector<std::pair<int, int>> syncSpecs;  // --framesync IDX[:FRAMES]
    std::vector<std::pair<int, int>> dskArms;   // --dsk K:SRC
    std::vector<std::pair<int, int>> dskFades;  // --dsk-fade K:TICKS
    std::vector<int> dskTies;                   // --dsk-tie K
    std::vector<int> dskAfvs;                   // --dsk-afv K
    struct DskToggle {
        double afterS;
        int k;
        bool done = false;
    };
    std::vector<DskToggle> dskToggles;  // --dsk-toggle-after S:K
    int controlPort = 0;  // 0 = off (benches run several instances at once)

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--input" || a == "--omt-input") {
            if (const char* v = next()) {
                cfg.inputs.push_back({kloud::InputSpec::Type::Omt, v});
                lastMediaInput = -1;
            }
        } else if (a == "--srt-input") {
            if (const char* v = next()) {
                cfg.inputs.push_back({kloud::InputSpec::Type::Srt, v});
                lastMediaInput = -1;
            }
        } else if (a == "--decklink-input") {
            // Accepts a bare index ("0") for convenience as well as a full
            // decklink:// ref.
            if (const char* v = next()) {
                std::string ref = v;
                if (ref.rfind("decklink://", 0) != 0) ref = "decklink://" + ref;
                cfg.inputs.push_back({kloud::InputSpec::Type::DeckLink, ref});
                lastMediaInput = -1;
            }
        } else if (a == "--media-input") {
            if (const char* v = next()) {
                kloud::InputSpec spec{kloud::InputSpec::Type::Media, v};
                spec.mediaPlaylist.emplace_back(v);
                cfg.inputs.push_back(std::move(spec));
                lastMediaInput = int(cfg.inputs.size()) - 1;
            }
        } else if (a == "--still-input") {
            if (const char* v = next()) {
                cfg.inputs.push_back({kloud::InputSpec::Type::Still, v});
                lastMediaInput = -1;
            }
        } else if (a == "--media-item") {
            const char* v = next();
            if (!v || lastMediaInput < 0) {
                fprintf(stderr,
                        "--media-item must follow the --media-input whose "
                        "playlist it extends\n");
                return 2;
            }
            cfg.inputs[size_t(lastMediaInput)].mediaPlaylist.emplace_back(v);
        } else if (a == "--media-trim") {
            const char* v = next();
            long long inMs = 0;
            long long outMs = 0;
            const int fields =
                v ? sscanf(v, "%lld:%lld", &inMs, &outMs) : 0;
            if (lastMediaInput < 0 || fields < 1 || inMs < 0 ||
                outMs < 0 || (outMs > 0 && outMs <= inMs)) {
                fprintf(stderr,
                        "--media-trim IN_MS[:OUT_MS] must follow the media "
                        "item it configures; OUT must be after IN\n");
                return 2;
            }
            auto& item =
                cfg.inputs[size_t(lastMediaInput)].mediaPlaylist.back();
            item.inMs = inMs;
            item.outMs = fields == 2 ? outMs : 0;
        } else if (a == "--media-speed") {
            const char* v = next();
            char* end = nullptr;
            const double speed = v ? std::strtod(v, &end) : 0.0;
            if (lastMediaInput < 0 || !v || end == v || *end ||
                speed < 0.25 || speed > 4.0) {
                fprintf(stderr,
                        "--media-speed RATE must follow the media item it "
                        "configures; RATE is 0.25 through 4.0\n");
                return 2;
            }
            cfg.inputs[size_t(lastMediaInput)]
                .mediaPlaylist.back()
                .speedPermille = int(std::lround(speed * 1000.0));
        } else if (a == "--media-no-loop") {
            if (lastMediaInput < 0) {
                fprintf(stderr,
                        "--media-no-loop must follow the --media-input it "
                        "configures\n");
                return 2;
            }
            cfg.inputs[size_t(lastMediaInput)].mediaLoop = false;
        } else if (a == "--show") {
            const char* v = next();
            if (!v || sscanf(v, "%dx%d", &cfg.show.width, &cfg.show.height) != 2)
                return 2;
        } else if (a == "--duration") {
            const char* v = next();
            if (v) duration = atof(v);
        } else if (a == "--dump-dir") {
            if (const char* v = next()) dumpDir = v;
        } else if (a == "--dump-every") {
            const char* v = next();
            if (v) dumpEvery = atof(v);
        } else if (a == "--cuts") {
            scriptedCuts = true;  // cut every 2 seconds
        } else if (a == "--autos") {
            scriptedAutos = true;  // auto-transition every 2.5s, cycling types
        } else if (a == "--no-omt-out") {
            cfg.omtOut = false;
        } else if (a == "--omt-out-name") {
            if (const char* v = next()) cfg.omtOutName = v;
        } else if (a == "--clean-omt-out") {
            if (const char* v = next()) {
                cfg.cleanOmtOut = true;
                cfg.cleanOmtOutName = v;
            }
        } else if (a == "--mv-omt-out") {
            if (const char* v = next()) {
                cfg.mvOmtOut = true;
                cfg.mvOmtOutName = v;
            }
        } else if (a == "--multiview") {
            int w = 0, h = 0;
            const char* v = next();
            if (!v || sscanf(v, "%dx%d", &w, &h) != 2) return 2;
            cfg.mvW = w;
            cfg.mvH = h;
        } else if (a == "--sdi-out" || a == "--clean-sdi-out") {
            // Bare index ("0") accepted, like --decklink-input.
            if (const char* v = next()) {
                std::string ref = v;
                if (ref.rfind("decklink://", 0) != 0) ref = "decklink://" + ref;
                (a == "--sdi-out" ? cfg.sdiOutRef : cfg.cleanSdiOutRef) = ref;
            }
        } else if (a == "--srt-out") {
            if (const char* v = next()) cfg.srtUrl = v;
        } else if (a == "--srt-bitrate") {
            const char* v = next();
            if (v) cfg.srtBitrateKbps = atoi(v);
        } else if (a == "--srt-codec") {
            const char* v = next();
            if (!v || !kloud::media::parseVideoCodec(v, cfg.srtCodec)) return 2;
        } else if (a == "--srt-keyframe") {
            const char* v = next();
            if (v) cfg.srtKeyframeMs = int(atof(v) * 1000.0 + 0.5);
        } else if (a == "--srt-audio-bitrate") {
            const char* v = next();
            if (v) cfg.srtAudioKbps = atoi(v);
        } else if (a == "--encoder") {
            const char* v = next();
            if (!v || !kloud::media::parseEncoderBackend(v, cfg.encoder)) return 2;
        } else if (a == "--encoder-preset") {
            const char* v = next();
            if (!v || !kloud::media::parseEncoderPreset(v, cfg.encoderPreset))
                return 2;
        } else if (a == "--record") {
            if (const char* v = next()) recordPath = v;
        } else if (a == "--clean-record") {
            if (const char* v = next()) cleanRecordPath = v;
        } else if (a == "--record-bitrate") {
            const char* v = next();
            if (v) cfg.recordBitrateKbps = atoi(v);
        } else if (a == "--record-stop-after") {
            const char* v = next();
            if (v) recordStopAfter = atof(v);
        } else if (a == "--clean-record-stop-after") {
            const char* v = next();
            if (v) cleanRecordStopAfter = atof(v);
        } else if (a == "--no-audio") {
            cfg.audio = false;
        } else if (a == "--audio-delay") {
            const char* v = next();
            if (v) cfg.masterAudioDelayMs = atoi(v);
        } else if (a == "--input-delay") {
            const char* v = next();
            int idx = 0, ms = 0;
            if (!v || sscanf(v, "%d:%d", &idx, &ms) != 2) return 2;
            inputDelays.emplace_back(idx, ms);
        } else if (a == "--replace-after") {
            const char* v = next();
            double s = 0;
            int idx = 0, off = 0;
            if (!v || sscanf(v, "%lf:%d:%n", &s, &idx, &off) != 2 || !v[off])
                return 2;
            replaces.push_back({s, idx, std::string(v + off)});
        } else if (a == "--framesync") {
            // IDX[:FRAMES] -- FRAMES 0 = measure-only, default 1. Applied
            // after defaults so it also works with the implicit KloudBench
            // inputs.
            const char* v = next();
            int idx = 0, fr = 1;
            if (!v || sscanf(v, "%d:%d", &idx, &fr) < 1) return 2;
            if (idx < 0 || fr < 0 || fr > 4) return 2;
            syncSpecs.emplace_back(idx, fr);
        } else if (a == "--dsk") {
            const char* v = next();
            int k = 0, src = 0;
            if (!v || sscanf(v, "%d:%d", &k, &src) != 2) return 2;
            dskArms.emplace_back(k, src);
        } else if (a == "--dsk-fade") {
            const char* v = next();
            int k = 0, ticks = 0;
            if (!v || sscanf(v, "%d:%d", &k, &ticks) != 2) return 2;
            dskFades.emplace_back(k, ticks);
        } else if (a == "--dsk-tie") {
            const char* v = next();
            if (!v) return 2;
            dskTies.push_back(atoi(v));
        } else if (a == "--dsk-afv") {
            const char* v = next();
            if (!v) return 2;
            dskAfvs.push_back(atoi(v));
        } else if (a == "--dsk-toggle-after") {
            const char* v = next();
            double s = 0;
            int k = 0;
            if (!v || sscanf(v, "%lf:%d", &s, &k) != 2) return 2;
            dskToggles.push_back({s, k});
        } else if (a == "--control-port") {
            const char* v = next();
            if (v) controlPort = atoi(v);
        } else if (a == "--validate") {
            cfg.validation = true;
        } else {
            fprintf(stderr,
                    "usage: kloud-headless --input NAME_OR_URL "
                    "[--input NAME_OR_URL ...] "
                    "[--srt-input URL] [--omt-input NAME_OR_URL] "
                    "[--decklink-input INDEX|decklink://REF] "
                    "[--still-input PATH] "
                    "[--media-input PATH [--media-trim IN_MS[:OUT_MS]] "
                    "[--media-speed RATE] [--media-item PATH "
                    "[--media-trim IN_MS[:OUT_MS]] [--media-speed RATE] ...] "
                    "[--media-no-loop]] "
                    "[--record PATH.mkv] [--clean-record PATH.mkv] "
                    "[--record-bitrate KBPS] [--record-stop-after S] "
                    "[--clean-record-stop-after S] "
                    "[--no-omt-out] [--omt-out-name NAME] "
                    "[--clean-omt-out NAME] [--mv-omt-out NAME] "
                    "[--multiview WxH] "
                    "[--sdi-out INDEX|decklink://REF] "
                    "[--clean-sdi-out INDEX|decklink://REF] "
                    "[--show WxH] [--duration S] [--dump-dir D] "
                    "[--dump-every S] [--cuts] [--no-audio] "
                    "[--audio-delay MS] [--framesync IDX[:FRAMES]] "
                    "[--dsk K:SRC] [--dsk-fade K:TICKS] "
                    "[--dsk-tie K] [--dsk-afv K] "
                    "[--dsk-toggle-after S:K] [--control-port PORT] "
                    "[--encoder auto|ffmpeg|direct] "
                    "[--encoder-preset auto|p1..p7] "
                    "[--srt-codec hevc|av1] [--srt-keyframe SEC] "
                    "[--srt-audio-bitrate KBPS] "
                    "[--validate]\n");
            return 2;
        }
    }
    if (cfg.inputs.empty())
        cfg.inputs = {{kloud::InputSpec::Type::Omt, "KloudBenchA"},
                      {kloud::InputSpec::Type::Omt, "KloudBenchB"}};
    for (auto [idx, fr] : syncSpecs) {
        if (size_t(idx) >= cfg.inputs.size()) {
            fprintf(stderr, "--framesync %d: no such input\n", idx);
            return 2;
        }
        cfg.inputs[size_t(idx)].syncFrames = fr;
    }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    kloud::Engine engine;
    if (!engine.start(cfg)) {
        KLOUD_LOGE("engine start failed");
        return 1;
    }
    if (!recordPath.empty()) engine.requestRecording(recordPath);
    if (!cleanRecordPath.empty())
        engine.requestCleanRecording(cleanRecordPath);
    if (auto* aud = engine.audio())
        for (auto [idx, ms] : inputDelays)
            if (idx >= 0 && idx < aud->inputCount())
                aud->channel(idx).delayMs.store(ms);
    for (auto [k, src] : dskArms)
        engine.post({kloud::Command::Type::SetDskSource, k, src, 0.f});
    for (auto [k, ticks] : dskFades)
        engine.post({kloud::Command::Type::SetDskFade, k, ticks, 0.f});
    for (int k : dskTies)
        engine.post({kloud::Command::Type::SetDskTie, k, 1, 0.f});
    for (int k : dskAfvs)
        engine.post({kloud::Command::Type::SetDskAudioFollow, k, 1, 0.f});
    std::unique_ptr<kloud::ctl::ControlServer> control;
    if (controlPort > 0)
        control = std::make_unique<kloud::ctl::ControlServer>(engine, controlPort);

    const int64_t t0 = kloud::MediaClock::nowNs();
    const int64_t endNs = t0 + int64_t(duration * 1e9);
    int64_t nextDumpNs = t0 + int64_t(dumpEvery * 1e9);
    int64_t nextCutNs = t0 + 2'000'000'000;
    int64_t nextLogNs = t0 + 1'000'000'000;
    int dumpIdx = 0;
    int transCycle = 1;  // start with WipeLR

    std::vector<uint8_t> mv;
    uint64_t mvSeq = 0;
    int mvW = 0, mvH = 0;

    while (!g_stop && kloud::MediaClock::nowNs() < endNs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const int64_t now = kloud::MediaClock::nowNs();

        if (!recordStopped && recordStopAfter >= 0 &&
            now >= t0 + int64_t(recordStopAfter * 1e9)) {
            recordStopped = true;
            engine.requestRecording({});
        }
        if (!cleanRecordStopped && cleanRecordStopAfter >= 0 &&
            now >= t0 + int64_t(cleanRecordStopAfter * 1e9)) {
            cleanRecordStopped = true;
            engine.requestCleanRecording({});
        }

        if (scriptedCuts && now >= nextCutNs) {
            engine.post({kloud::Command::Type::Cut, 0, 0, 0.f});
            nextCutNs += 2'000'000'000;
        }
        for (auto& r : replaces) {
            if (!r.done && now >= t0 + int64_t(r.afterS * 1e9)) {
                r.done = true;
                const auto type =
                    r.ref.rfind("srt://", 0) == 0 ? kloud::InputSpec::Type::Srt
                    : kloud::isDeckLinkRef(r.ref)
                        ? kloud::InputSpec::Type::DeckLink
                    : std::filesystem::is_regular_file(r.ref)
                        ? (kloud::media::isStillImagePath(r.ref)
                               ? kloud::InputSpec::Type::Still
                               : kloud::InputSpec::Type::Media)
                        : kloud::InputSpec::Type::Omt;
                engine.requestInputReplace(r.idx, {type, r.ref});
            }
        }
        for (auto& t : dskToggles) {
            if (!t.done && now >= t0 + int64_t(t.afterS * 1e9)) {
                t.done = true;
                engine.post({kloud::Command::Type::DskToggle, t.k, 0, 0.f});
            }
        }
        if (scriptedAutos && now >= nextCutNs) {
            // Cycle transition types (mix + all wipes), 45-frame duration.
            engine.post({kloud::Command::Type::SetTransition, transCycle % 7, 45, 0.05f});
            engine.post({kloud::Command::Type::Auto, 0, 0, 0.f});
            ++transCycle;
            nextCutNs += 2'500'000'000;
        }
        if (!dumpDir.empty() && now >= nextDumpNs) {
            if (engine.copyMultiview(mv, mvSeq, mvW, mvH)) {
                char path[512];
                snprintf(path, sizeof path, "%s/mv_%03d.ppm", dumpDir.c_str(),
                         dumpIdx++);
                kloud::ppm::writeRgba(path, mvW, mvH, mv.data());
                KLOUD_LOGI("dumped %s (seq %llu)", path, (unsigned long long)mvSeq);
            }
            nextDumpNs += int64_t(dumpEvery * 1e9);
        }
        if (now >= nextLogNs) {
            std::string line = "ticks=" + std::to_string(engine.renderedTicks()) +
                               " skips=" + std::to_string(engine.skippedTicks());
            if (auto* aud = engine.audio())
                line += "  aud[t=" + std::to_string(aud->mixTicks()) +
                        " sk=" + std::to_string(aud->mixSkips()) +
                        " un=" + std::to_string(aud->underruns()) + "]";
            if (engine.srtFramesEncoded() || engine.srtConnected())
                line += "  srt[" + std::string(engine.srtConnected() ? "up" : "down") +
                        " enc=" + std::to_string(engine.srtFramesEncoded()) + "]";
            if (const auto rec = engine.recordingState();
                rec.active || rec.pending || rec.error)
                line += "  rec[" +
                        std::string(rec.error   ? "error"
                                    : rec.active ? "on"
                                                 : "starting") +
                        " f=" + std::to_string(rec.frames) + "]";
            if (const auto rec = engine.cleanRecordingState();
                rec.active || rec.pending || rec.error)
                line += "  cleanRec[" +
                        std::string(rec.error   ? "error"
                                    : rec.active ? "on"
                                                 : "starting") +
                        " f=" + std::to_string(rec.frames) + "]";
            if (engine.cleanOmtOutFrames())
                line += "  cleanOmt[f=" +
                        std::to_string(engine.cleanOmtOutFrames()) + "]";
            if (!cfg.sdiOutRef.empty())
                line += "  sdi[" +
                        std::string(engine.sdiOutOk() ? "up" : "down") +
                        " f=" + std::to_string(engine.sdiOutFrames()) + "]";
            if (engine.cleanSdiOutFrames())
                line += "  cleanSdi[f=" +
                        std::to_string(engine.cleanSdiOutFrames()) + "]";
            for (int i = 0; i < engine.inputCount(); ++i) {
                const auto s = engine.inputStatus(i);
                line += "  in" + std::to_string(i) + "[" +
                        (s.connected ? "up" : "down") + " " +
                        std::to_string(s.desc.width) + "x" +
                        std::to_string(s.desc.height) +
                        " f=" + std::to_string(s.frames) +
                        " d=" + std::to_string(s.drops);
                if (auto* aud = engine.audio())
                    line += " a=" + std::to_string(
                                       aud->channel(i).pushedFrames.load(
                                           std::memory_order_relaxed));
                line += "]";
            }
            KLOUD_LOGI("%s", line.c_str());
            nextLogNs += 1'000'000'000;
        }
    }

    control.reset();  // its poll thread reads engine state; stop it first
    engine.stop();

    KLOUD_LOGI("-- final counters (nonzero) --");
    for (const auto& s : kloud::Stats::snapshot())
        if (s.value)
            KLOUD_LOGI("  %-28s %lld", s.name.c_str(), (long long)s.value);
    return 0;
}
