/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <fstream>
#include <string>
#include <unistd.h>

#include "app/ShowFile.h"

using namespace kloud;
using kloud::app::ShowFile;

namespace {
struct TempShow {
    std::string path;
    TempShow() {
        const char* dir = getenv("TMPDIR");
        path = std::string(dir ? dir : "/tmp") + "/kloud-show-" +
               std::to_string(getpid()) + "-" + std::to_string(counter++) +
               ".ini";
    }
    ~TempShow() { std::remove(path.c_str()); }
    static int counter;
};
int TempShow::counter = 0;
}  // namespace

TEST_CASE("show file preserves the exact output format") {
    TempShow temporary;
    ShowFile file(temporary.path);
    ShowFile::State saved;
    saved.cfg.show.width = 4096;
    saved.cfg.show.height = 2160;
    saved.cfg.show.fpsN = 24000;
    saved.cfg.show.fpsD = 1001;
    saved.cfg.cleanOmtOut = true;
    saved.cfg.cleanOmtOutName = "8Kloud Switcher CLEAN TEST";
    saved.cfg.mvOmtOut = true;
    saved.cfg.mvOmtOutName = "8Kloud Switcher MV, wall";
    saved.cfg.srtCodec = media::VideoCodec::Av1;
    saved.cfg.srtUrl = "srt://relay:5000?mode=caller&latency=250000"
                       "&passphrase=correcthorse&pbkeylen=32";
    saved.cfg.srtSend = false;
    saved.cfg.srtKeyframeMs = 500;
    saved.cfg.srtAudioKbps = 256;
    saved.cfg.encoder = media::EncoderBackend::Direct;
    saved.cfg.encoderPreset = media::EncoderPreset::P6;
    REQUIRE(file.save(saved));

    ShowFile::State restored;
    REQUIRE(file.load(restored));
    CHECK(restored.cfg.srtUrl == saved.cfg.srtUrl);
    CHECK_FALSE(restored.cfg.srtSend);
    CHECK(restored.cfg.srtKeyframeMs == 500);
    CHECK(restored.cfg.srtAudioKbps == 256);
    CHECK(restored.cfg.encoder == media::EncoderBackend::Direct);
    CHECK(restored.cfg.encoderPreset == media::EncoderPreset::P6);
    CHECK(restored.cfg.show.width == 4096);
    CHECK(restored.cfg.show.height == 2160);
    CHECK(restored.cfg.show.fpsN == 24000);
    CHECK(restored.cfg.show.fpsD == 1001);
    CHECK(restored.cfg.cleanOmtOut);
    CHECK(restored.cfg.cleanOmtOutName == "8Kloud Switcher CLEAN TEST");
    CHECK(restored.cfg.mvOmtOut);
    CHECK(restored.cfg.mvOmtOutName == "8Kloud Switcher MV, wall");
    CHECK(restored.cfg.srtCodec == media::VideoCodec::Av1);
    CHECK(restored == saved);
}

TEST_CASE("show file restores playlists, trim, speed, and loop mode") {
    TempShow temporary;
    ShowFile file(temporary.path);
    ShowFile::State saved;
    InputSpec media;
    media.type = InputSpec::Type::Media;
    media.ref = "/shows/roll-in.mkv";
    media.mediaPlaylist = {
        {"/shows/roll in, take 2.mkv", 500, 3'500, 1250},
        {"/shows/guest-intro.mkv", 1'250, 0, 750},
        {"/shows/bumper.mkv", 0, 1'000, 2000},
    };
    media.ref = media.mediaPlaylist.front().path;
    media.syncFrames = -1;
    media.mediaPlaying = false;  // pause is deliberately session-only
    media.mediaLoop = false;
    saved.cfg.inputs = {media};
    saved.chans = {ShowFile::ChannelState{0.5f, true, false, 12}};
    saved.dsk[1] = {0, 45, true, true, true};
    REQUIRE(file.save(saved));

    ShowFile::State restored;
    REQUIRE(file.load(restored));
    REQUIRE(restored.cfg.inputs.size() == 1);
    CHECK(restored.cfg.inputs[0].type == InputSpec::Type::Media);
    CHECK(restored.cfg.inputs[0].ref == "/shows/roll in, take 2.mkv");
    CHECK(restored.cfg.inputs[0].mediaPlaylist == media.mediaPlaylist);
    CHECK(restored.cfg.inputs[0].syncFrames == -1);
    CHECK(restored.cfg.inputs[0].mediaPlaying);
    CHECK_FALSE(restored.cfg.inputs[0].mediaLoop);
    REQUIRE(restored.chans.size() == 1);
    CHECK(restored.chans[0] == saved.chans[0]);
    CHECK(restored.dsk[1] == saved.dsk[1]);
}

TEST_CASE("legacy single-clip show file becomes a one-item playlist") {
    TempShow temporary;
    {
        std::ofstream f(temporary.path);
        f << "[inputs]\n1\\ref=/shows/legacy.mkv\n1\\type=media\nsize=1\n";
    }
    ShowFile file(temporary.path);
    ShowFile::State restored;
    REQUIRE(file.load(restored));
    REQUIRE(restored.cfg.inputs.size() == 1);
    CHECK(restored.cfg.inputs[0].ref == "/shows/legacy.mkv");
    CHECK(restored.cfg.inputs[0].mediaPlaylist ==
          std::vector<media::PlaylistItem>{{"/shows/legacy.mkv"}});
}

TEST_CASE("show file preserves still-image inputs and discovery names") {
    TempShow temporary;
    ShowFile file(temporary.path);
    ShowFile::State saved;
    InputSpec still{InputSpec::Type::Still, "/shows/sponsor logo.png"};
    still.syncFrames = -1;
    InputSpec omt{InputSpec::Type::Omt, "STUDIO-PC (Cam A)", 1};
    InputSpec srt{InputSpec::Type::Srt,
                  "srt://host:9710?mode=caller&latency=120000", 0};
    InputSpec black{InputSpec::Type::Omt, ""};
    saved.cfg.inputs = {still, omt, srt, black};
    REQUIRE(file.save(saved));

    ShowFile::State restored;
    REQUIRE(file.load(restored));
    REQUIRE(restored.cfg.inputs.size() == 4);
    CHECK(restored.cfg.inputs[0].type == InputSpec::Type::Still);
    CHECK(restored.cfg.inputs[0].ref == "/shows/sponsor logo.png");
    CHECK(restored.cfg.inputs[0].mediaPlaylist.empty());
    CHECK(restored.cfg.inputs[1].ref == "STUDIO-PC (Cam A)");
    CHECK(restored.cfg.inputs[1].syncFrames == 1);
    CHECK(restored.cfg.inputs[2].type == InputSpec::Type::Srt);
    CHECK(restored.cfg.inputs[2].ref ==
          "srt://host:9710?mode=caller&latency=120000");
    CHECK(restored.cfg.inputs[2].syncFrames == 0);
    CHECK(restored.cfg.inputs[3].ref.empty());
}

TEST_CASE("show file written by the Qt GUI loads") {
    // Verbatim from a QSettings-written show.ini of the previous release.
    TempShow temporary;
    {
        std::ofstream f(temporary.path);
        f << "[audioChannels]\n"
             "1\\delayMs=0\n1\\gain=1\n1\\mute=false\n1\\solo=false\n"
             "2\\delayMs=15\n2\\gain=0.70794576406478882\n2\\mute=true\n"
             "2\\solo=false\nsize=2\n\n"
             "[dsk]\n1\\audioFollow=false\n1\\fadeDurTicks=30\n1\\on=false\n"
             "1\\source=1\n1\\tie=false\n2\\audioFollow=true\n"
             "2\\fadeDurTicks=12\n2\\on=true\n2\\source=0\n2\\tie=true\n"
             "size=2\n\n"
             "[inputs]\n1\\framesync=1\n1\\ref=SATURN (CamA)\n1\\type=omt\n"
             "2\\framesync=-1\n2\\mediaLoop=true\n"
             "2\\mediaPlaylist=/home/op/Videos/a.mkv, \"/home/op/Videos/b, c.mkv\"\n"
             "2\\mediaSpeedPermille=1000, 1500\n2\\mediaTrimInMs=0, 250\n"
             "2\\mediaTrimOutMs=0, 0\n2\\ref=/home/op/Videos/a.mkv\n"
             "2\\type=media\nsize=2\n\n"
             "[show]\naudio=true\ncleanOmtOut=false\n"
             "cleanOmtOutName=8Kloud Switcher CLEAN\ncleanSdiOut=\nfpsD=1001\n"
             "fpsN=60000\nheight=1080\nmasterDelayMs=10\nmvH=1080\n"
             "mvOmtOut=false\nmvOmtOutName=8Kloud Switcher MV\nmvW=1920\n"
             "omtOut=true\nomtOutName=8Kloud Switcher PGM\n"
             "recordBitrateKbps=0\nsdiOut=\nsrtBitrateKbps=0\nsrtCodec=hevc\n"
             "srtOut=\"srt://:9710?mode=listener&latency=120000\"\nwidth=1920\n\n"
             "[switcher]\npreview=2\nprogram=1\ntransDurTicks=45\n"
             "transType=3\n";
    }
    ShowFile file(temporary.path);
    ShowFile::State st;
    REQUIRE(file.load(st));
    CHECK(st.cfg.show.width == 1920);
    CHECK(st.cfg.show.fpsN == 60000);
    CHECK(st.cfg.srtUrl == "srt://:9710?mode=listener&latency=120000");
    // No srtSend key yet: a stored URL was live, and the encoder knobs that
    // used to be CLI-only are at their defaults.
    CHECK(st.cfg.srtSend);
    CHECK(st.cfg.srtEnabled());
    CHECK(st.cfg.srtKeyframeMs == 0);
    CHECK(st.cfg.srtAudioKbps == 0);
    CHECK(st.cfg.encoder == media::EncoderBackend::Auto);
    CHECK(st.cfg.encoderPreset == media::EncoderPreset::Auto);
    CHECK(st.cfg.masterAudioDelayMs == 10);
    REQUIRE(st.cfg.inputs.size() == 2);
    CHECK(st.cfg.inputs[0].ref == "SATURN (CamA)");
    CHECK(st.cfg.inputs[0].syncFrames == 1);
    CHECK(st.cfg.inputs[1].mediaPlaylist ==
          std::vector<media::PlaylistItem>{
              {"/home/op/Videos/a.mkv", 0, 0, 1000},
              {"/home/op/Videos/b, c.mkv", 250, 0, 1500}});
    REQUIRE(st.chans.size() == 2);
    CHECK(st.chans[1].mute);
    CHECK(st.chans[1].delayMs == 15);
    CHECK(st.chans[1].gain > 0.7f);
    CHECK(st.dsk[1].on);
    CHECK(st.dsk[1].tie);
    CHECK(st.dsk[1].fadeDurTicks == 12);
    CHECK(st.program == 1);
    CHECK(st.preview == 2);
    CHECK(st.transType == 3);
    CHECK(st.transDurTicks == 45);
}

TEST_CASE("show file rejects degenerate geometry and missing files") {
    TempShow temporary;
    {
        std::ofstream f(temporary.path);
        f << "[show]\nwidth=0\nheight=0\nmvW=10\nmvH=10\nfpsN=0\n";
    }
    ShowFile file(temporary.path);
    ShowFile::State st;
    REQUIRE(file.load(st));
    CHECK(st.cfg.show.width == 1920);
    CHECK(st.cfg.mvW == 1920);
    CHECK(st.cfg.show.fpsN == 60000);

    ShowFile missing("/nonexistent/dir/show.ini");
    ShowFile::State untouched;
    CHECK_FALSE(missing.load(untouched));
}
