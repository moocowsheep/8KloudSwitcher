/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <catch2/catch_test_macros.hpp>

#include "core/SrtUrl.h"

using kloud::SrtUrl;

TEST_CASE("empty SRT URL parses to the README listener") {
    SrtUrl u;
    REQUIRE(SrtUrl::parse("", u));
    CHECK(u.mode == SrtUrl::Mode::Listener);
    CHECK(u.host.empty());
    CHECK(u.port == 9710);
    CHECK(u.latencyMs == 120);
    CHECK(u.passphrase.empty());
    CHECK(u.keyLen == 0);
    CHECK(u.compose() == "srt://:9710?mode=listener&latency=120000");
    CHECK(SrtUrl::parse("  \t", u));
}

TEST_CASE("the README listener URL round-trips") {
    const std::string url = "srt://:9710?mode=listener&latency=120000";
    SrtUrl u;
    REQUIRE(SrtUrl::parse(url, u));
    CHECK(u == SrtUrl{});
    CHECK(u.compose() == url);
}

TEST_CASE("caller URL with encryption, stream id and extras splits into fields") {
    SrtUrl u;
    REQUIRE(SrtUrl::parse(
        "srt://relay.example.net:5000?mode=caller&latency=250000"
        "&passphrase=correcthorse&pbkeylen=32"
        "&streamid=#!::r=live/show,m=publish&pkt_size=1316&maxbw=0",
        u));
    CHECK(u.mode == SrtUrl::Mode::Caller);
    CHECK(u.host == "relay.example.net");
    CHECK(u.port == 5000);
    CHECK(u.latencyMs == 250);
    CHECK(u.passphrase == "correcthorse");
    CHECK(u.keyLen == 32);
    CHECK(u.streamId == "#!::r=live/show,m=publish");
    CHECK(u.extra == "pkt_size=1316&maxbw=0");
    // compose() normalizes the option order but loses nothing.
    CHECK(u.compose() ==
          "srt://relay.example.net:5000?mode=caller&latency=250000"
          "&passphrase=correcthorse&pbkeylen=32"
          "&streamid=#!::r=live/show,m=publish&pkt_size=1316&maxbw=0");
    SrtUrl again;
    REQUIRE(SrtUrl::parse(u.compose(), again));
    CHECK(again == u);
}

TEST_CASE("a URL without mode or latency reports FFmpeg's defaults") {
    SrtUrl u;
    REQUIRE(SrtUrl::parse("srt://10.0.0.5:9000", u));
    CHECK(u.mode == SrtUrl::Mode::Caller);
    CHECK(u.latencyMs == 0);
    CHECK(u.compose() == "srt://10.0.0.5:9000?mode=caller");
}

TEST_CASE("latency rounds to whole milliseconds") {
    SrtUrl u;
    REQUIRE(SrtUrl::parse("srt://:1?latency=120400", u));
    CHECK(u.latencyMs == 120);
    REQUIRE(SrtUrl::parse("srt://:1?latency=120600", u));
    CHECK(u.latencyMs == 121);
}

TEST_CASE("IPv6 hosts keep their brackets") {
    SrtUrl u;
    REQUIRE(SrtUrl::parse("srt://[fe80::1]:9710?mode=rendezvous", u));
    CHECK(u.host == "fe80::1");
    CHECK(u.port == 9710);
    CHECK(u.mode == SrtUrl::Mode::Rendezvous);
    CHECK(u.compose() == "srt://[fe80::1]:9710?mode=rendezvous");
    CHECK_FALSE(SrtUrl::parse("srt://[fe80::1:9710", u));
}

TEST_CASE("odd query items are kept verbatim as extras") {
    SrtUrl u;
    REQUIRE(SrtUrl::parse("srt://:9710?mode=sideways&pbkeylen=7&tlpktdrop&&latency=x", u));
    CHECK(u.mode == SrtUrl::Mode::Caller);  // unknown mode is not a mode
    CHECK(u.keyLen == 0);
    CHECK(u.latencyMs == 0);
    CHECK(u.extra == "mode=sideways&pbkeylen=7&tlpktdrop&latency=x");
}

TEST_CASE("a path is dropped and other schemes are refused") {
    SrtUrl u;
    REQUIRE(SrtUrl::parse("srt://host:4000/ignored?mode=caller", u));
    CHECK(u.host == "host");
    CHECK(u.port == 4000);
    CHECK_FALSE(SrtUrl::parse("rtmp://host/live", u));
    CHECK(u == SrtUrl{});
    REQUIRE(SrtUrl::parse("srt://host:99999", u));
    CHECK(u.port == 0);
}

TEST_CASE("mode names round-trip") {
    for (auto m : {SrtUrl::Mode::Listener, SrtUrl::Mode::Caller,
                   SrtUrl::Mode::Rendezvous}) {
        SrtUrl::Mode back = SrtUrl::Mode::Listener;
        REQUIRE(SrtUrl::parseMode(SrtUrl::modeName(m), back));
        CHECK(back == m);
    }
    SrtUrl::Mode untouched = SrtUrl::Mode::Rendezvous;
    CHECK_FALSE(SrtUrl::parseMode("Listener", untouched));
    CHECK(untouched == SrtUrl::Mode::Rendezvous);
}
