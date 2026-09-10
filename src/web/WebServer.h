/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/Json.h"
#include "web/Http.h"

namespace kloud {
class Engine;
namespace app {
class Session;
}
}  // namespace kloud

namespace kloud::web {

struct WebConfig {
    int port = 9924;   // 0 = off
    int mvFps = 12;    // browser multiview cadence
    int mvWidth = 1920;  // browser multiview width (aspect kept)
    int mvQuality = 5;   // mjpeg qscale, 2 best .. 31 worst
};

// The web GUI: one embedded HTTP server (docs/web-gui.md) that serves the
// single-page console and upgrades /ws to a WebSocket carrying the same
// text commands as the TCP control port, JSON web-only commands (source
// patching, playlists, settings), JSON state pushes, and the multiview as
// JPEG binary frames. One poll()-driven thread owns every socket; a second
// thread encodes the multiview at the configured cadence. Like the control
// port it binds 0.0.0.0 -- the LAN is trusted -- and a failed bind logs and
// leaves the GUI off without touching the show.
//
// Lifetime: both threads read engine state, so the server MUST be destroyed
// before Engine::stop().
class WebServer {
public:
    WebServer(Engine& engine, app::Session& session, const WebConfig& cfg);
    ~WebServer();

    bool listening() const { return listenFd_ >= 0; }
    int port() const { return cfg_.port; }

    // The rich state document (also GET /api/state).
    std::string uiJson() const;

private:
    struct Client {
        int fd = -1;
        bool ws = false;
        std::string in;
        std::string out;
        uint64_t frameSeq = 0;     // last multiview frame sent
        std::string fragments;     // continuation accumulation
        uint8_t fragmentOp = 0;
        bool closing = false;
    };

    void run(std::stop_token st);
    void encodeLoop(std::stop_token st);
    void handleHttp(Client& c, const HttpRequest& req);
    void handleWsText(Client& c, const std::string& text);
    void handleWsJson(Client& c, const json::Value& msg);
    void send(Client& c, std::string bytes);
    void sendJson(Client& c, const json::Value& v);
    void sendError(Client& c, const std::string& msg);
    std::string metersJson() const;
    json::Value sourcesJson() const;
    json::Value listDirectory(const std::string& path) const;
    json::Value settingsJson() const;
    // Returns a message for the operator when a value was refused (the rest
    // of the message is still applied); empty on success.
    std::string applySettings(const json::Value& s);

    Engine& engine_;
    app::Session& session_;
    WebConfig cfg_;
    int listenFd_ = -1;
    int wakeFd_ = -1;
    std::vector<Client> clients_;
    std::string lastState_;
    std::string lastUi_;

    // Latest multiview JPEG; the encode thread replaces it, the poll thread
    // hands it to clients that have drained the previous one.
    mutable std::mutex frameM_;
    std::shared_ptr<const std::string> frame_;
    std::atomic<uint64_t> frameSeq_{0};
    std::atomic<int> streamW_{0}, streamH_{0};

    std::jthread encodeThread_;
    std::jthread thread_;
};

}  // namespace kloud::web
