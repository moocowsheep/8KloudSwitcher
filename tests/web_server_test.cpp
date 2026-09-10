/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Drives the real web server over a raw socket: the HTTP page, the
// WebSocket handshake, a control line, a JSON web command, a directory
// listing, and the multiview JPEG stream. Needs the GPU (engine start), so
// the "web:" prefix is excluded on off-GPU builders like the gpu suites.

#include <catch2/catch_test_macros.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "app/Session.h"
#include "core/Json.h"
#include "engine/Engine.h"
#include "web/Http.h"
#include "web/WebServer.h"

using namespace kloud;

namespace {

int connectTo(int port) {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (sockaddr*)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        const ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

// Reads until `done(buffer)` says so or the timeout passes.
bool readUntil(int fd, std::string& buf,
               const std::function<bool(const std::string&)>& done,
               int timeoutMs = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!done(buf)) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        pollfd p{fd, POLLIN, 0};
        if (poll(&p, 1, int(left.count())) <= 0) continue;
        char tmp[65536];
        const ssize_t n = recv(fd, tmp, sizeof tmp, 0);
        if (n <= 0) return false;
        buf.append(tmp, size_t(n));
    }
    return true;
}

// Masked client frame.
std::string clientText(const std::string& payload) {
    std::string out;
    out += char(0x81);
    if (payload.size() < 126) {
        out += char(0x80 | payload.size());
    } else {
        out += char(0x80 | 126);
        out += char(payload.size() >> 8);
        out += char(payload.size() & 0xff);
    }
    const uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    for (const uint8_t m : mask) out += char(m);
    for (size_t i = 0; i < payload.size(); ++i)
        out += char(uint8_t(payload[i]) ^ mask[i & 3]);
    return out;
}

// Pops complete server frames (unmasked) off the front of buf.
struct Frame {
    uint8_t opcode;
    std::string payload;
};
bool popFrame(std::string& buf, Frame& f) {
    if (buf.size() < 2) return false;
    const uint8_t b0 = uint8_t(buf[0]), b1 = uint8_t(buf[1]);
    REQUIRE((b1 & 0x80) == 0);  // server never masks
    uint64_t len = b1 & 0x7f;
    size_t pos = 2;
    if (len == 126) {
        if (buf.size() < 4) return false;
        len = (uint64_t(uint8_t(buf[2])) << 8) | uint8_t(buf[3]);
        pos = 4;
    } else if (len == 127) {
        if (buf.size() < 10) return false;
        len = 0;
        for (int i = 0; i < 8; ++i) len = (len << 8) | uint8_t(buf[2 + i]);
        pos = 10;
    }
    if (buf.size() < pos + len) return false;
    f.opcode = b0 & 0x0f;
    f.payload = buf.substr(pos, size_t(len));
    buf.erase(0, pos + size_t(len));
    return true;
}

// Waits for a frame matching `want`; other frames are consumed.
bool awaitFrame(int fd, std::string& buf,
                const std::function<bool(const Frame&)>& want, Frame& out,
                int timeoutMs = 5000) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        Frame f;
        while (popFrame(buf, f))
            if (want(f)) {
                out = f;
                return true;
            }
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) return false;
        std::string more;
        if (!readUntil(fd, more, [](const std::string& s) { return !s.empty(); },
                       int(left.count())))
            return false;
        buf += more;
    }
}

json::Value eventNamed(const Frame& f, const char* name) {
    json::Value v;
    std::string err;
    if (f.opcode != 1 || !json::parse(f.payload, v, err)) return {};
    return v["event"].asString() == name ? v : json::Value();
}

}  // namespace

TEST_CASE("web: page, handshake, commands, listing and multiview stream") {
    Engine engine;
    EngineConfig cfg;
    cfg.omtOut = false;
    cfg.audio = false;
    cfg.mvW = 960;
    cfg.mvH = 540;
    cfg.inputs = {{InputSpec::Type::Omt, ""}, {InputSpec::Type::Omt, ""}};
    REQUIRE(engine.start(cfg));
    {
        app::ShowFile::State initial;
        initial.cfg = cfg;
        app::Session session(engine, nullptr, initial);
        web::WebConfig wc;
        wc.port = 39000 + int(getpid() % 1000);
        wc.mvFps = 20;
        wc.mvWidth = 480;
        web::WebServer server(engine, session, wc);
        REQUIRE(server.listening());

        // Plain HTTP: the console page and the JSON state.
        {
            const int fd = connectTo(wc.port);
            REQUIRE(fd >= 0);
            REQUIRE(sendAll(fd, "GET / HTTP/1.1\r\nHost: x\r\n\r\n"));
            std::string buf;
            REQUIRE(readUntil(fd, buf, [](const std::string& s) {
                return s.find("</html>") != std::string::npos;
            }));
            CHECK(buf.rfind("HTTP/1.1 200 OK\r\n", 0) == 0);
            CHECK(buf.find("Content-Type: text/html") != std::string::npos);
            CHECK(buf.find("8KLOUD//SWITCHER") != std::string::npos);
            // Keep-alive: a second request on the same connection.
            REQUIRE(sendAll(fd, "GET /api/state HTTP/1.1\r\nHost: x\r\n\r\n"));
            buf.clear();
            REQUIRE(readUntil(fd, buf, [](const std::string& s) {
                return s.find("\"event\":\"ui\"") != std::string::npos &&
                       s.find("\"status\"") != std::string::npos;
            }));
            REQUIRE(sendAll(fd, "GET /nope HTTP/1.1\r\nHost: x\r\n\r\n"));
            buf.clear();
            REQUIRE(readUntil(fd, buf, [](const std::string& s) {
                return s.find("404") != std::string::npos;
            }));
            close(fd);
        }

        // WebSocket session.
        const int fd = connectTo(wc.port);
        REQUIRE(fd >= 0);
        REQUIRE(sendAll(fd,
                        "GET /ws HTTP/1.1\r\nHost: x\r\nUpgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                        "Sec-WebSocket-Version: 13\r\n\r\n"));
        std::string buf;
        REQUIRE(readUntil(fd, buf, [](const std::string& s) {
            return s.find("\r\n\r\n") != std::string::npos;
        }));
        CHECK(buf.find("HTTP/1.1 101") != std::string::npos);
        CHECK(buf.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
              std::string::npos);
        buf.erase(0, buf.find("\r\n\r\n") + 4);

        Frame f;
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            return !eventNamed(x, "hello").isNull();
        }, f));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            return !eventNamed(x, "state").isNull();
        }, f));
        CHECK(eventNamed(f, "state")["program"].asInt() == 1);
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            return !eventNamed(x, "ui").isNull();
        }, f));
        const json::Value ui = eventNamed(f, "ui");
        CHECK(ui["inputCount"].asInt() == 2);
        CHECK(ui["mv"]["w"].asInt() == 960);
        CHECK(ui["inputs"][0]["name"].asString() == "BLACK");
        CHECK(ui["settings"]["restartRequired"].asBool() == false);

        // A control-protocol line, confirmed by the next state push.
        REQUIRE(sendAll(fd, clientText("PGM 2")));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            const auto v = eventNamed(x, "state");
            return !v.isNull() && v["program"].asInt() == 2;
        }, f));
        // A bad line comes back as an error event, never a disconnect.
        REQUIRE(sendAll(fd, clientText("PGM 9")));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            return !eventNamed(x, "error").isNull();
        }, f));
        CHECK(eventNamed(f, "error")["message"].asString().find("out of range") !=
              std::string::npos);

        // A JSON web command patches an input; the ui document follows.
        REQUIRE(sendAll(fd, clientText(
            R"({"cmd":"replaceInput","input":1,"type":"omt","ref":"KloudCam","sync":1})")));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            const auto v = eventNamed(x, "ui");
            return !v.isNull() && v["inputs"][0]["ref"].asString() == "KloudCam";
        }, f));
        CHECK(engine.inputRef(0) == "KloudCam");
        CHECK(engine.inputSyncFrames(0) == 1);
        CHECK(eventNamed(f, "ui")["inputs"][0]["sync"].asInt() == 1);

        // Settings edits are restart-to-apply and flagged as such.
        REQUIRE(sendAll(fd, clientText(
            R"({"cmd":"settings","mvOmtOut":true,"mvOmtOutName":"Wall","show":{"width":3840,"height":2160,"fpsN":30000,"fpsD":1001}})")));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            const auto v = eventNamed(x, "ui");
            return !v.isNull() && v["settings"]["restartRequired"].asBool();
        }, f));
        CHECK(eventNamed(f, "ui")["settings"]["pending"]["mvOmtOutName"].asString() == "Wall");
        CHECK(eventNamed(f, "ui")["settings"]["pending"]["show"]["width"].asInt() == 3840);
        CHECK(session.restartRequired());
        CHECK(session.collect().cfg.mvOmtOut);

        // The SRT card's fields fold into the stored URL; the parsed view
        // comes back alongside it, and a refused value is an error event
        // that does not lose the rest of the message.
        REQUIRE(sendAll(fd, clientText(
            R"({"cmd":"settings","srtSend":true,"srtMode":"caller","srtHost":"relay","srtPort":5000,)"
            R"("srtLatencyMs":250,"srtPassphrase":"correcthorse","srtKeyLen":32,)"
            R"("srtStreamId":"#!::r=live/a,m=publish","srtKeyframeMs":500,"srtAudioKbps":256,)"
            R"("encoderPreset":"p6","encoder":"direct"})")));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            const auto v = eventNamed(x, "ui");
            return !v.isNull() && v["settings"]["pending"]["srtHost"].asString() == "relay";
        }, f));
        {
            const json::Value p = eventNamed(f, "ui")["settings"]["pending"];
            CHECK(p["srtOut"].asString() ==
                  "srt://relay:5000?mode=caller&latency=250000&passphrase=correcthorse"
                  "&pbkeylen=32&streamid=#!::r=live/a,m=publish");
            CHECK(p["srtSend"].asBool());
            CHECK(p["srtMode"].asString() == "caller");
            CHECK(p["srtPort"].asInt() == 5000);
            CHECK(p["srtLatencyMs"].asInt() == 250);
            CHECK(p["srtKeyLen"].asInt() == 32);
            CHECK(p["srtKeyframeMs"].asInt() == 500);
            CHECK(p["srtAudioKbps"].asInt() == 256);
            CHECK(p["encoderPreset"].asString() == "p6");
            CHECK(p["encoder"].asString() == "direct");
        }
        CHECK(session.pendingConfig().srtKeyframeMs == 500);
        CHECK(session.pendingConfig().encoderPreset == media::EncoderPreset::P6);
        REQUIRE(sendAll(fd, clientText(
            R"({"cmd":"settings","srtPassphrase":"short","srtPort":6000})")));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            return !eventNamed(x, "error").isNull();
        }, f));
        CHECK(eventNamed(f, "error")["message"].asString().find("passphrase") !=
              std::string::npos);
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            const auto v = eventNamed(x, "ui");
            return !v.isNull() && v["settings"]["pending"]["srtPort"].asInt() == 6000;
        }, f));
        CHECK(eventNamed(f, "ui")["settings"]["pending"]["srtPassphrase"].asString() ==
              "correcthorse");
        // A pasted URL replaces everything; SEND off parks it.
        REQUIRE(sendAll(fd, clientText(
            R"({"cmd":"settings","srtOut":"srt://:9710?mode=listener&latency=120000","srtSend":false})")));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            const auto v = eventNamed(x, "ui");
            return !v.isNull() && !v["settings"]["pending"]["srtSend"].asBool(true);
        }, f));
        CHECK(eventNamed(f, "ui")["settings"]["pending"]["srtMode"].asString() == "listener");
        CHECK(eventNamed(f, "ui")["settings"]["pending"]["srtOut"].asString() ==
              "srt://:9710?mode=listener&latency=120000");
        CHECK_FALSE(session.pendingConfig().srtEnabled());

        // Directory listing for the clip/still browser.
        REQUIRE(sendAll(fd, clientText(R"({"cmd":"ls","path":"/"})")));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) {
            return !eventNamed(x, "ls").isNull();
        }, f));
        CHECK(eventNamed(f, "ls")["path"].asString() == "/");
        CHECK(eventNamed(f, "ls")["dirs"].size() > 0);

        // The multiview arrives as JPEG binary frames at the stream size.
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) { return x.opcode == 2; }, f,
                           8000));
        REQUIRE(f.payload.size() > 4);
        CHECK(uint8_t(f.payload[0]) == 0xFF);
        CHECK(uint8_t(f.payload[1]) == 0xD8);
        CHECK(uint8_t(f.payload[f.payload.size() - 2]) == 0xFF);
        CHECK(uint8_t(f.payload[f.payload.size() - 1]) == 0xD9);

        // Ping/pong and an orderly close.
        std::string ping = clientText("x");
        ping[0] = char(0x89);
        REQUIRE(sendAll(fd, ping));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) { return x.opcode == 10; }, f));
        CHECK(f.payload == "x");
        std::string closeFrame = clientText("");
        closeFrame[0] = char(0x88);
        REQUIRE(sendAll(fd, closeFrame));
        REQUIRE(awaitFrame(fd, buf, [](const Frame& x) { return x.opcode == 8; }, f));
        close(fd);
    }
    // Replaced inputs are destroyed on detached threads; give them time to
    // finish before process teardown tears the runtime out from under them.
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    engine.stop();
}
