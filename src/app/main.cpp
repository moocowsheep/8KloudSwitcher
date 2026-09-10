/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// 8kloud-switcher: the production console. Runs the engine, restores and
// persists the show, and serves the web GUI (docs/web-gui.md) plus the TCP
// remote-control port. There is no local window: the multiview reaches the
// operator through the browser and, as a proper video feed, through the OMT
// multiview sender, which is on by default here.

#include <csignal>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>

#include "app/Session.h"
#include "app/ShowFile.h"
#include "core/Log.h"
#include "ctl/ControlServer.h"
#include "engine/Engine.h"
#include "web/WebServer.h"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void onSignal(int) { g_stop = 1; }

void usage() {
    fprintf(stderr,
            "usage: 8kloud-switcher [options]\n"
            "  --show-file PATH          show to restore/persist (default "
            "~/.config/8KloudSwitcher/show.ini)\n"
            "  --web-port N              web GUI port (default 9924; 0 = off)\n"
            "  --web-mv-fps N            browser multiview rate (default 12)\n"
            "  --web-mv-width N          browser multiview width (default 1920)\n"
            "  --web-mv-quality N        browser multiview JPEG qscale 2..31 "
            "(default 5)\n"
            "  --control-port N          TCP remote control (default 9923; 0 = off)\n"
            "  --input NAME|URL          OMT discovery name or omt:// URL "
            "(repeat; replaces the stored inputs)\n"
            "  --srt-input URL | --sdi-input REF | --media-input PATH | "
            "--still-input PATH\n"
            "  --show WxH                program format (also set from the GUI)\n"
            "  --multiview WxH           multiview wall geometry\n"
            "  --mv-omt-out NAME | --no-mv-omt-out   multiview OMT sender "
            "(default on, \"8Kloud Switcher MV\")\n"
            "  --clean-omt-out NAME      clean-feed OMT sender\n"
            "  --sdi-out REF | --clean-sdi-out REF   DeckLink outputs\n"
            "  --srt-out URL [--srt-bitrate KBPS] [--srt-codec hevc|av1]\n"
            "      [--srt-keyframe SEC] [--srt-audio-bitrate KBPS]\n"
            "  --record PATH.mkv | --clean-record PATH.mkv [--record-bitrate KBPS]\n"
            "  --encoder auto|ffmpeg|direct  --encoder-preset auto|p1..p7\n"
            "  --validate                Vulkan validation layer\n");
}

bool parseWxH(const char* v, int& w, int& h) {
    return v && sscanf(v, "%dx%d", &w, &h) == 2;
}

}  // namespace

int main(int argc, char** argv) {
    // The show file loads first; CLI flags override what they name.
    std::string showFilePath;
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], "--show-file") == 0) showFilePath = argv[i + 1];
    kloud::app::ShowFile showFile(showFilePath);
    kloud::app::ShowFile::State show;
    // The console's multiview is a network feed: on unless the show says no.
    show.cfg.mvOmtOut = true;
    if (showFile.load(show))
        KLOUD_LOGI("show restored from %s", showFile.path().c_str());
    else
        KLOUD_LOGI("no show file at %s; starting a fresh show",
                 showFile.path().c_str());

    kloud::EngineConfig& cfg = show.cfg;
    std::string recordPath, cleanRecordPath;
    int controlPort = 9923;
    kloud::web::WebConfig webCfg;
    bool cliInputs = false;
    auto addInput = [&](kloud::InputSpec::Type t, std::string ref) {
        if (!cliInputs) cfg.inputs.clear();  // CLI replaces the stored set
        cliInputs = true;
        kloud::InputSpec spec{t, std::move(ref)};
        if (t == kloud::InputSpec::Type::Media)
            spec.mediaPlaylist.emplace_back(spec.ref);
        cfg.inputs.push_back(std::move(spec));
    };
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s needs a value\n", a.c_str());
                exit(2);
            }
            return argv[++i];
        };
        if (a == "--input" || a == "--omt-input")
            addInput(kloud::InputSpec::Type::Omt, next());
        else if (a == "--srt-input")
            addInput(kloud::InputSpec::Type::Srt, next());
        else if (a == "--sdi-input" || a == "--decklink-input") {
            std::string ref = next();
            if (ref.rfind("decklink://", 0) != 0) ref = "decklink://" + ref;
            addInput(kloud::InputSpec::Type::DeckLink, ref);
        } else if (a == "--media-input")
            addInput(kloud::InputSpec::Type::Media, next());
        else if (a == "--still-input")
            addInput(kloud::InputSpec::Type::Still, next());
        else if (a == "--validate")
            cfg.validation = true;
        else if (a == "--clean-omt-out") {
            cfg.cleanOmtOut = true;
            cfg.cleanOmtOutName = next();
        } else if (a == "--mv-omt-out") {
            cfg.mvOmtOut = true;
            cfg.mvOmtOutName = next();
        } else if (a == "--no-mv-omt-out")
            cfg.mvOmtOut = false;
        else if (a == "--omt-out-name") {
            cfg.omtOut = true;
            cfg.omtOutName = next();
        } else if (a == "--no-omt-out")
            cfg.omtOut = false;
        else if (a == "--multiview") {
            if (!parseWxH(next(), cfg.mvW, cfg.mvH)) return 2;
        } else if (a == "--sdi-out" || a == "--clean-sdi-out") {
            std::string ref = next();
            if (ref.rfind("decklink://", 0) != 0) ref = "decklink://" + ref;
            (a == "--sdi-out" ? cfg.sdiOutRef : cfg.cleanSdiOutRef) = ref;
        } else if (a == "--show") {
            if (!parseWxH(next(), cfg.show.width, cfg.show.height)) return 2;
            cfg.show.colorimetry =
                kloud::VideoFormatDesc::colorimetryForHeight(cfg.show.height);
        } else if (a == "--srt-out") {
            cfg.srtUrl = next();
            cfg.srtSend = true;  // the flag means send, whatever the show parked
        } else if (a == "--srt-bitrate")
            cfg.srtBitrateKbps = atoi(next());
        else if (a == "--srt-codec") {
            const char* name = next();
            if (!kloud::media::parseVideoCodec(name, cfg.srtCodec))
                KLOUD_LOGW("unknown --srt-codec '%s'; using hevc", name);
        } else if (a == "--srt-keyframe")
            cfg.srtKeyframeMs = int(atof(next()) * 1000.0 + 0.5);
        else if (a == "--srt-audio-bitrate")
            cfg.srtAudioKbps = atoi(next());
        else if (a == "--encoder-preset") {
            const char* name = next();
            if (!kloud::media::parseEncoderPreset(name, cfg.encoderPreset))
                KLOUD_LOGW("unknown --encoder-preset '%s'; using auto", name);
        } else if (a == "--encoder") {
            const char* name = next();
            if (!kloud::media::parseEncoderBackend(name, cfg.encoder))
                KLOUD_LOGW("unknown --encoder '%s'; using auto", name);
        } else if (a == "--record")
            recordPath = next();
        else if (a == "--clean-record")
            cleanRecordPath = next();
        else if (a == "--record-bitrate")
            cfg.recordBitrateKbps = atoi(next());
        else if (a == "--no-audio")
            cfg.audio = false;
        else if (a == "--show-file")
            ++i;  // consumed above
        else if (a == "--control-port")
            controlPort = atoi(next());
        else if (a == "--web-port")
            webCfg.port = atoi(next());
        else if (a == "--web-mv-fps")
            webCfg.mvFps = atoi(next());
        else if (a == "--web-mv-width")
            webCfg.mvWidth = atoi(next());
        else if (a == "--web-mv-quality")
            webCfg.mvQuality = atoi(next());
        else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            fprintf(stderr, "unknown option %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    // A fixed 21-input frame (7 x 3 on the multiview). Unassigned slots are
    // black until the operator patches a source from the INPUTS tab.
    constexpr size_t kInputSlots = 21;
    while (cfg.inputs.size() < kInputSlots)
        cfg.inputs.push_back({kloud::InputSpec::Type::Omt, ""});
    show.chans.resize(cfg.inputs.size());

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);
    std::signal(SIGPIPE, SIG_IGN);

    kloud::Engine engine;
    if (!engine.start(cfg)) {
        KLOUD_LOGE("engine start failed");
        return 1;
    }
    if (!recordPath.empty()) engine.requestRecording(recordPath);
    if (!cleanRecordPath.empty()) engine.requestCleanRecording(cleanRecordPath);

    // Control-surface and mixer state restore.
    if (auto* aud = engine.audio())
        for (int i = 0; i < aud->inputCount() && i < int(show.chans.size()); ++i) {
            const auto& ch = show.chans[size_t(i)];
            aud->channel(i).gain.store(ch.gain);
            aud->channel(i).mute.store(ch.mute);
            aud->channel(i).solo.store(ch.solo);
            aud->channel(i).delayMs.store(ch.delayMs);
        }
    engine.post({kloud::Command::Type::SetProgram, show.program, 0, 0.f});
    engine.post({kloud::Command::Type::SetPreview, show.preview, 0, 0.f});
    engine.post({kloud::Command::Type::SetTransition, show.transType,
                 show.transDurTicks > 0 ? show.transDurTicks : 30, 0.02f});
    for (int k = 0; k < kloud::kDskCount; ++k) {
        const auto& d = show.dsk[k];
        engine.post({kloud::Command::Type::SetDskSource, k, d.source, 0.f});
        engine.post({kloud::Command::Type::SetDskFade, k, d.fadeDurTicks, 0.f});
        if (d.on) engine.post({kloud::Command::Type::DskToggle, k, 0, 0.f});
        if (d.tie) engine.post({kloud::Command::Type::SetDskTie, k, 1, 0.f});
        if (d.audioFollow)
            engine.post({kloud::Command::Type::SetDskAudioFollow, k, 1, 0.f});
    }

    kloud::app::Session session(engine, &showFile, show);
    std::unique_ptr<kloud::ctl::ControlServer> control;
    if (controlPort > 0)
        control = std::make_unique<kloud::ctl::ControlServer>(engine, controlPort);
    auto web = std::make_unique<kloud::web::WebServer>(engine, session, webCfg);
    if (webCfg.port > 0 && !web->listening())
        KLOUD_LOGE("web GUI is not reachable; the show is still running "
                 "(remote control on tcp/%d)", controlPort);

    while (!g_stop) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    KLOUD_LOGI("stopping");

    web.reset();      // both servers read engine state; stop them first
    control.reset();
    session.shutdown();  // final save while the engine still has its inputs
    engine.stop();
    return 0;
}
