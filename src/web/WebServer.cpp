/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "web/WebServer.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include "app/Session.h"
#include "core/Log.h"
#include "core/MediaClock.h"
#include "core/SrtUrl.h"
#include "core/Stats.h"
#include "ctl/ControlApply.h"
#include "decklink/DeckLinkRef.h"
#include "engine/Engine.h"
#include "media/StillImage.h"
#include "web/MjpegEncoder.h"
#include "web/web_index_html.spv.h"
#include "web/web_app_js.spv.h"
#include "web/web_app_css.spv.h"
#include "web/web_logo_svg.spv.h"

namespace kloud::web {

namespace {

constexpr size_t kMaxClients = 32;
constexpr size_t kMaxOutBuf = 8 << 20;   // slow reader: drop past 8 MiB
constexpr size_t kMaxWsMessage = 1 << 20;
constexpr int kPollMs = 30;              // state / meter cadence
constexpr int64_t kUiEveryNs = 250'000'000;  // rich document, at most 4 Hz

bool setNonBlock(int fd) {
    const int fl = fcntl(fd, F_GETFL, 0);
    return fl >= 0 && fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

std::string_view asset(const uint8_t* data, size_t size) {
    return std::string_view(reinterpret_cast<const char*>(data), size);
}

json::Value fmtJson(const VideoFormatDesc& f) {
    return json::Value{{"width", f.width},
                       {"height", f.height},
                       {"fpsN", int64_t(f.fpsN)},
                       {"fpsD", int64_t(f.fpsD)}};
}

json::Value recordJson(const Engine::RecordingState& r) {
    return json::Value{{"active", r.active},
                       {"pending", r.pending},
                       {"error", r.error},
                       {"frames", r.frames},
                       {"path", r.path}};
}

json::Value outputJson(bool configured, bool up, const std::string& name,
                       int64_t frames) {
    return json::Value{{"configured", configured},
                       {"up", up},
                       {"name", name},
                       {"frames", frames}};
}

double round3(double v) { return std::round(v * 1000.0) / 1000.0; }

std::string homeDir() {
    const char* home = getenv("HOME");
    return home && *home ? home : "/";
}

std::string videosDir() {
    const std::string videos = homeDir() + "/Videos";
    struct stat st{};
    if (stat(videos.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) return videos;
    return homeDir();
}

}  // namespace

WebServer::WebServer(Engine& engine, app::Session& session,
                     const WebConfig& cfg)
    : engine_(engine), session_(session), cfg_(cfg) {
    if (cfg_.port <= 0) return;
    listenFd_ = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd_ < 0) {
        KLOUD_LOGE("web: socket: %s", strerror(errno));
        return;
    }
    const int one = 1;
    setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(uint16_t(cfg_.port));
    if (bind(listenFd_, (const sockaddr*)&addr, sizeof addr) != 0 ||
        listen(listenFd_, 16) != 0 || !setNonBlock(listenFd_)) {
        KLOUD_LOGE("web: cannot listen on tcp/%d: %s (web GUI off)", cfg_.port,
                 strerror(errno));
        close(listenFd_);
        listenFd_ = -1;
        return;
    }
    wakeFd_ = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    encodeThread_ = std::jthread([this](std::stop_token st) { encodeLoop(st); });
    thread_ = std::jthread([this](std::stop_token st) { run(st); });
    KLOUD_LOGI("web: GUI at http://<this-host>:%d/", cfg_.port);
}

WebServer::~WebServer() {
    if (encodeThread_.joinable()) {
        encodeThread_.request_stop();
        encodeThread_.join();
    }
    if (thread_.joinable()) {
        thread_.request_stop();
        if (wakeFd_ >= 0) {
            const uint64_t one = 1;
            [[maybe_unused]] const ssize_t n = write(wakeFd_, &one, sizeof one);
        }
        thread_.join();
    }
    for (const auto& c : clients_) close(c.fd);
    if (wakeFd_ >= 0) close(wakeFd_);
    if (listenFd_ >= 0) close(listenFd_);
}

// -- multiview encode thread -------------------------------------------------

void WebServer::encodeLoop(std::stop_token st) {
    MjpegEncoder enc;
    std::vector<uint8_t> rgba;
    uint64_t seq = 0;
    int w = 0, h = 0;
    const int64_t periodNs =
        int64_t(1e9 / double(std::clamp(cfg_.mvFps, 1, 60)));
    int64_t next = MediaClock::nowNs();
    bool encoderFailed = false;
    while (!st.stop_requested()) {
        const int64_t now = MediaClock::nowNs();
        if (now < next) {
            std::this_thread::sleep_for(
                std::chrono::nanoseconds(std::min<int64_t>(next - now, 20'000'000)));
            continue;
        }
        next += periodNs;
        if (next < now - periodNs) next = now;  // fell behind: resync
        if (clients_.empty() && seq) continue;  // nobody watching: idle
        if (!engine_.copyMultiview(rgba, seq, w, h)) continue;
        if (!enc.ok() && !encoderFailed) {
            if (!enc.open(w, h, cfg_.mvWidth, cfg_.mvQuality)) {
                encoderFailed = true;
                continue;
            }
            streamW_.store(enc.width(), std::memory_order_relaxed);
            streamH_.store(enc.height(), std::memory_order_relaxed);
        }
        if (encoderFailed) continue;
        auto jpeg = std::make_shared<std::string>();
        if (!enc.encode(rgba.data(), *jpeg)) continue;
        {
            std::lock_guard lk(frameM_);
            frame_ = std::move(jpeg);
        }
        frameSeq_.fetch_add(1, std::memory_order_release);
    }
}

// -- state documents ---------------------------------------------------------

json::Value WebServer::settingsJson() const {
    auto encode = [](const EngineConfig& c) {
        // The URL split into the fields the SRT card edits. An unparsable
        // URL (not srt://) reports the defaults; the raw line still shows it.
        SrtUrl srt;
        SrtUrl::parse(c.srtUrl, srt);
        return json::Value{
            {"show", fmtJson(c.show)},
            {"omtOut", c.omtOut},
            {"omtOutName", c.omtOutName},
            {"cleanOmtOut", c.cleanOmtOut},
            {"cleanOmtOutName", c.cleanOmtOutName},
            {"mvOmtOut", c.mvOmtOut},
            {"mvOmtOutName", c.mvOmtOutName},
            {"mvW", c.mvW},
            {"mvH", c.mvH},
            {"sdiOut", c.sdiOutRef},
            {"cleanSdiOut", c.cleanSdiOutRef},
            {"srtOut", c.srtUrl},
            {"srtSend", c.srtSend},
            {"srtMode", SrtUrl::modeName(srt.mode)},
            {"srtHost", srt.host},
            {"srtPort", srt.port},
            {"srtLatencyMs", srt.latencyMs},
            {"srtPassphrase", srt.passphrase},
            {"srtKeyLen", srt.keyLen},
            {"srtStreamId", srt.streamId},
            {"srtExtra", srt.extra},
            {"srtBitrateKbps", c.srtBitrateKbps},
            {"srtCodec", media::videoCodecName(c.srtCodec)},
            {"srtKeyframeMs", c.srtKeyframeMs},
            {"srtAudioKbps", c.srtAudioKbps},
            {"recordBitrateKbps", c.recordBitrateKbps},
            {"encoder", media::encoderBackendName(c.encoder)},
            {"encoderPreset", media::encoderPresetName(c.encoderPreset)},
            {"audio", c.audio},
        };
    };
    return json::Value{{"pending", encode(session_.pendingConfig())},
                       {"active", encode(session_.activeConfig())},
                       {"restartRequired", session_.restartRequired()}};
}

std::string WebServer::uiJson() const {
    json::Value doc = json::Value::object();
    doc.set("event", "ui");
    const auto ui = engine_.uiState();
    const auto fmt = engine_.outputFormat();
    doc.set("format", fmtJson(fmt));
    doc.set("inputCount", engine_.inputCount());
    doc.set("transType", ui.transType);
    doc.set("transDur", ui.transDur);
    json::Value dsk = json::Value::array();
    for (int k = 0; k < kDskCount; ++k)
        dsk.push(json::Value{{"src", ui.dskSrc[k] + 1},
                             {"fade", ui.dskDur[k]},
                             {"on", ui.dskOn[k]},
                             {"tie", ui.dskTie[k]},
                             {"afv", ui.dskAudioFollow[k]}});
    doc.set("dsk", std::move(dsk));
    const auto& active = session_.activeConfig();
    doc.set("mv", json::Value{{"w", active.mvW},
                              {"h", active.mvH},
                              {"fps", cfg_.mvFps},
                              {"streamW", streamW_.load(std::memory_order_relaxed)},
                              {"streamH", streamH_.load(std::memory_order_relaxed)},
                              {"omt", engine_.mvOmtOutActive()},
                              {"omtName", engine_.mvOmtOutName()}});
    doc.set("settings", settingsJson());
    doc.set("showPath", session_.showPath());
    doc.set("recordDir", videosDir());
    doc.set("homeDir", homeDir());

    auto* aud = engine_.audio();
    json::Value inputs = json::Value::array();
    std::vector<std::string> problems;
    for (int i = 0; i < engine_.inputCount(); ++i) {
        const std::string ref = engine_.inputRef(i);
        const auto type = engine_.inputType(i);
        const auto status = engine_.inputStatus(i);
        json::Value in = json::Value::object();
        in.set("n", i + 1);
        in.set("ref", ref);
        in.set("type", inputTypeName(type));
        in.set("name", inputDisplayName(type, ref));
        in.set("connected", status.connected);
        in.set("width", status.desc.width);
        in.set("height", status.desc.height);
        in.set("frames", status.frames);
        in.set("drops", status.drops);
        in.set("sync", engine_.inputSyncFrames(i));
        if (!status.connected && !ref.empty())
            problems.push_back("IN" + std::to_string(i + 1) + " no signal");
        if (aud && i < aud->inputCount()) {
            const auto& ch = aud->channel(i);
            in.set("gain", round3(ch.gain.load(std::memory_order_relaxed)));
            in.set("mute", ch.mute.load(std::memory_order_relaxed));
            in.set("solo", ch.solo.load(std::memory_order_relaxed));
            in.set("delayMs", ch.delayMs.load(std::memory_order_relaxed));
            in.set("autoTrimMs",
                   ch.autoDelayFrames.load(std::memory_order_relaxed) * 1000 /
                       audio::kSampleRate);
        }
        const auto m = engine_.inputMediaState(i);
        if (m.available) {
            json::Value playlist = json::Value::array();
            for (const auto& item : engine_.inputMediaPlaylist(i))
                playlist.push(json::Value{{"path", item.path},
                                          {"in", item.inMs},
                                          {"out", item.outMs},
                                          {"speed", item.speedPermille}});
            in.set("media", json::Value{{"playing", m.playing},
                                        {"loop", m.loop},
                                        {"atEnd", m.atEnd},
                                        {"positionMs", m.positionMs},
                                        {"durationMs", m.durationMs},
                                        {"index", m.playlistIndex + 1},
                                        {"size", m.playlistSize},
                                        {"trimInMs", m.trimInMs},
                                        {"trimOutMs", m.trimOutMs},
                                        {"speed", m.speedPermille},
                                        {"currentRef", m.currentRef},
                                        {"playlist", std::move(playlist)}});
        } else if (type == InputSpec::Type::Media) {
            json::Value playlist = json::Value::array();
            for (const auto& item : engine_.inputMediaPlaylist(i))
                playlist.push(json::Value{{"path", item.path},
                                          {"in", item.inMs},
                                          {"out", item.outMs},
                                          {"speed", item.speedPermille}});
            in.set("playlist", std::move(playlist));
        }
        inputs.push(std::move(in));
    }
    doc.set("inputs", std::move(inputs));
    if (engine_.srtConfigured() && !engine_.srtConnected())
        problems.push_back("SRT out down (reconnecting)");
    auto outputAlarm = [&](bool configured, bool up, const char* what) {
        if (configured && !up) problems.push_back(std::string(what) + " down");
    };
    outputAlarm(engine_.omtOutRequested(), engine_.omtOutActive(), "OMT out");
    outputAlarm(engine_.cleanOmtOutRequested(), engine_.cleanOmtOutActive(),
                "clean OMT out");
    outputAlarm(engine_.mvOmtOutRequested(), engine_.mvOmtOutActive(),
                "multiview OMT out");
    outputAlarm(!engine_.sdiOutRef().empty(), engine_.sdiOutOk(), "SDI out");
    outputAlarm(!engine_.cleanSdiOutRef().empty(), engine_.cleanSdiOutOk(),
                "clean SDI out");
    json::Value health = json::Value::array();
    for (const auto& p : problems) health.push(p);
    doc.set("health", std::move(health));

    json::Value audio = json::Value::object();
    audio.set("available", aud != nullptr);
    if (aud) {
        audio.set("masterDelayMs",
                  aud->masterDelayMs.load(std::memory_order_relaxed));
        audio.set("mixSkips", aud->mixSkips());
        audio.set("underruns", aud->underruns());
    }
    doc.set("audio", std::move(audio));
    doc.set("record", recordJson(engine_.recordingState()));
    doc.set("cleanRecord", recordJson(engine_.cleanRecordingState()));
    doc.set("outputs",
            json::Value{
                {"omtOut", outputJson(engine_.omtOutRequested(),
                                      engine_.omtOutActive(), engine_.omtOutName(),
                                      engine_.omtOutFrames())},
                {"cleanOmtOut",
                 outputJson(engine_.cleanOmtOutRequested(),
                            engine_.cleanOmtOutActive(),
                            engine_.cleanOmtOutName(), engine_.cleanOmtOutFrames())},
                {"mvOmtOut",
                 outputJson(engine_.mvOmtOutRequested(), engine_.mvOmtOutActive(),
                            engine_.mvOmtOutName(), engine_.mvOmtOutFrames())},
                {"sdiOut", outputJson(!engine_.sdiOutRef().empty(),
                                      engine_.sdiOutOk(), engine_.sdiOutRef(),
                                      engine_.sdiOutFrames())},
                {"cleanSdiOut",
                 outputJson(!engine_.cleanSdiOutRef().empty(),
                            engine_.cleanSdiOutOk(), engine_.cleanSdiOutRef(),
                            engine_.cleanSdiOutFrames())},
                {"srt", json::Value{{"configured", engine_.srtConfigured()},
                                    {"connected", engine_.srtConnected()},
                                    {"frames", engine_.srtFramesEncoded()}}},
            });
    doc.set("counters", json::Value{{"ticks", engine_.renderedTicks()},
                                    {"skips", engine_.skippedTicks()}});

    // The same runtime line the old status bar showed.
    std::string status = "ticks " + std::to_string(engine_.renderedTicks()) +
                         "  skips " + std::to_string(engine_.skippedTicks()) +
                         "  omt-out " + std::to_string(engine_.omtOutFrames());
    if (engine_.cleanOmtOutFrames())
        status += "  clean-omt " + std::to_string(engine_.cleanOmtOutFrames());
    if (engine_.mvOmtOutFrames())
        status += "  mv-omt " + std::to_string(engine_.mvOmtOutFrames());
    if (engine_.sdiOutFrames() || engine_.sdiOutOk())
        status += "  sdi-out " + std::to_string(engine_.sdiOutFrames());
    if (engine_.cleanSdiOutFrames())
        status += "  clean-sdi " + std::to_string(engine_.cleanSdiOutFrames());
    if (engine_.srtConfigured())
        status += std::string("  srt[") +
                  (engine_.srtConnected() ? "up" : "down") +
                  " enc=" + std::to_string(engine_.srtFramesEncoded()) + "]";
    if (aud)
        status += "  aud[sk " + std::to_string(aud->mixSkips()) + " un " +
                  std::to_string(aud->underruns()) + "]";
    for (int i = 0; i < engine_.inputCount(); ++i) {
        if (engine_.inputRef(i).empty()) continue;  // unassigned = quiet
        const auto s = engine_.inputStatus(i);
        status += "   in" + std::to_string(i) + ": " +
                  (s.connected ? "up" : "down") + " " +
                  std::to_string(s.desc.width) + "x" +
                  std::to_string(s.desc.height) + " f=" +
                  std::to_string(s.frames) + " d=" + std::to_string(s.drops) +
                  " r=" +
                  std::to_string(
                      Stats::counter("in" + std::to_string(i) + ".repeats")
                          .value());
    }
    doc.set("status", status);
    return doc.dump();
}

std::string WebServer::metersJson() const {
    auto* a = engine_.audio();
    if (!a) return {};
    std::string out = "{\"event\":\"meters\",\"v\":[";
    char buf[16];
    auto put = [&](float v, bool first) {
        snprintf(buf, sizeof buf, "%s%.3f", first ? "" : ",", double(v));
        out += buf;
    };
    for (int i = 0; i < a->inputCount(); ++i) {
        put(a->channel(i).peakL.exchange(0.f, std::memory_order_relaxed), i == 0);
        put(a->channel(i).peakR.exchange(0.f, std::memory_order_relaxed), false);
    }
    put(a->masterPeakL.exchange(0.f, std::memory_order_relaxed),
        a->inputCount() == 0);
    put(a->masterPeakR.exchange(0.f, std::memory_order_relaxed), false);
    out += "]}";
    return out;
}

json::Value WebServer::sourcesJson() const {
    json::Value omt = json::Value::array();
    for (const auto& name : engine_.omtSources()) omt.push(name);
    json::Value decklink = json::Value::array();
    const auto cards = engine_.decklinkSources();
    for (size_t i = 0; i < cards.size(); ++i)
        decklink.push(json::Value{{"label", cards[i]},
                                  {"ref", "decklink://" + std::to_string(i)}});
    return json::Value{{"event", "sources"},
                       {"omt", std::move(omt)},
                       {"decklink", std::move(decklink)}};
}

json::Value WebServer::listDirectory(const std::string& requested) const {
    namespace fs = std::filesystem;
    std::string path = requested.empty() ? videosDir() : requested;
    if (path.rfind("~", 0) == 0) path = homeDir() + path.substr(1);
    std::error_code ec;
    fs::path p = fs::absolute(path, ec).lexically_normal();
    if (ec || !fs::is_directory(p, ec)) {
        return json::Value{{"event", "ls"},
                           {"path", path},
                           {"error", "not a directory"}};
    }
    json::Value dirs = json::Value::array();
    json::Value files = json::Value::array();
    std::vector<std::pair<std::string, bool>> names;
    std::vector<std::pair<std::string, uint64_t>> fileSizes;
    for (const auto& entry : fs::directory_iterator(
             p, fs::directory_options::skip_permission_denied, ec)) {
        const std::string name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
        std::error_code e2;
        if (entry.is_directory(e2)) {
            names.emplace_back(name, true);
        } else if (entry.is_regular_file(e2)) {
            fileSizes.emplace_back(name, uint64_t(entry.file_size(e2)));
        }
    }
    std::sort(names.begin(), names.end());
    std::sort(fileSizes.begin(), fileSizes.end());
    for (const auto& [name, isDir] : names) dirs.push(name);
    for (const auto& [name, size] : fileSizes)
        files.push(json::Value{{"name", name},
                               {"size", size},
                               {"still", media::isStillImagePath(name)}});
    const std::string parent =
        p.has_parent_path() && p != p.root_path() ? p.parent_path().string()
                                                  : std::string();
    return json::Value{{"event", "ls"},
                       {"path", p.string()},
                       {"parent", parent},
                       {"dirs", std::move(dirs)},
                       {"files", std::move(files)}};
}

std::string WebServer::applySettings(const json::Value& s) {
    EngineConfig cfg = session_.pendingConfig();
    std::string refused;
    if (s["show"].isObject()) {
        const auto& f = s["show"];
        const int w = f["width"].asInt(cfg.show.width);
        const int h = f["height"].asInt(cfg.show.height);
        const int64_t n = f["fpsN"].asInt64(cfg.show.fpsN);
        const int64_t d = f["fpsD"].asInt64(cfg.show.fpsD);
        if (w >= 2 && h >= 2 && !(w & 1) && !(h & 1) && w <= 8192 && h <= 8192) {
            cfg.show.width = w;
            cfg.show.height = h;
        }
        if (n > 0 && d > 0 && n / d <= 240) {
            cfg.show.fpsN = n;
            cfg.show.fpsD = d;
        }
    }
    if (s["omtOut"].isBool()) cfg.omtOut = s["omtOut"].asBool();
    if (s["omtOutName"].isString()) cfg.omtOutName = s["omtOutName"].asString();
    if (s["cleanOmtOut"].isBool()) cfg.cleanOmtOut = s["cleanOmtOut"].asBool();
    if (s["cleanOmtOutName"].isString())
        cfg.cleanOmtOutName = s["cleanOmtOutName"].asString();
    if (s["mvOmtOut"].isBool()) cfg.mvOmtOut = s["mvOmtOut"].asBool();
    if (s["mvOmtOutName"].isString()) cfg.mvOmtOutName = s["mvOmtOutName"].asString();
    if (s["mvW"].isNumber() && s["mvH"].isNumber()) {
        const int w = s["mvW"].asInt(), h = s["mvH"].asInt();
        if (w >= 320 && h >= 180 && w <= 7680 && h <= 4320 && !(w & 1) &&
            !(h & 1)) {
            cfg.mvW = w;
            cfg.mvH = h;
        }
    }
    auto sdiRef = [](std::string ref) {
        if (!ref.empty() && !isDeckLinkRef(ref)) ref = "decklink://" + ref;
        return ref;
    };
    if (s["sdiOut"].isString()) cfg.sdiOutRef = sdiRef(s["sdiOut"].asString());
    if (s["cleanSdiOut"].isString())
        cfg.cleanSdiOutRef = sdiRef(s["cleanSdiOut"].asString());
    // The SRT address: either the whole URL, or one or more of the fields
    // it splits into, folded back into the stored URL. A pasted URL wins
    // over fields in the same message.
    auto trimmed = [](const std::string& text) {
        const auto b = text.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) return std::string();
        const auto e = text.find_last_not_of(" \t\r\n");
        return text.substr(b, e - b + 1);
    };
    if (s["srtOut"].isString()) {
        cfg.srtUrl = trimmed(s["srtOut"].asString());
    } else {
        SrtUrl url;
        SrtUrl::parse(cfg.srtUrl, url);
        bool edited = false;
        // FFmpeg reads the query verbatim, so a '&' or '?' inside a value
        // would split it; a passphrase outside libsrt's length range is
        // refused at connect time with a far less helpful message.
        auto field = [&](const char* key, std::string& into) {
            if (!s[key].isString()) return;
            const std::string v = trimmed(s[key].asString());
            if (v.find_first_of("&?") != std::string::npos) {
                refused = std::string(key) + " cannot contain '&' or '?'";
                return;
            }
            into = v;
            edited = true;
        };
        if (s["srtMode"].isString() &&
            SrtUrl::parseMode(s["srtMode"].asString(), url.mode))
            edited = true;
        field("srtHost", url.host);
        if (s["srtPort"].isNumber()) {
            const int port = s["srtPort"].asInt();
            if (port >= 1 && port <= 65535) {
                url.port = port;
                edited = true;
            } else {
                refused = "srtPort must be 1..65535";
            }
        }
        if (s["srtLatencyMs"].isNumber()) {
            url.latencyMs = std::clamp(s["srtLatencyMs"].asInt(), 0, 10000);
            edited = true;
        }
        std::string passphrase = url.passphrase;
        field("srtPassphrase", passphrase);
        if (passphrase != url.passphrase) {
            if (!passphrase.empty() &&
                (passphrase.size() < 10 || passphrase.size() > 79))
                refused = "passphrase must be 10..79 characters (or empty)";
            else
                url.passphrase = passphrase;
        }
        if (s["srtKeyLen"].isNumber()) {
            const int k = s["srtKeyLen"].asInt();
            if (k == 0 || k == 16 || k == 24 || k == 32) {
                url.keyLen = k;
                edited = true;
            }
        }
        field("srtStreamId", url.streamId);
        if (s["srtExtra"].isString()) {
            std::string extra = trimmed(s["srtExtra"].asString());
            while (!extra.empty() && (extra.front() == '?' || extra.front() == '&'))
                extra.erase(0, 1);
            url.extra = extra;
            edited = true;
        }
        if (edited) cfg.srtUrl = url.compose();
    }
    if (s["srtSend"].isBool()) {
        cfg.srtSend = s["srtSend"].asBool();
        // Switching on with nothing configured starts from the README's
        // listener; the operator then edits the fields.
        if (cfg.srtSend && cfg.srtUrl.empty()) cfg.srtUrl = SrtUrl{}.compose();
    }
    if (s["srtBitrateKbps"].isNumber())
        cfg.srtBitrateKbps = std::max(0, s["srtBitrateKbps"].asInt());
    if (s["srtCodec"].isString())
        media::parseVideoCodec(s["srtCodec"].asString(), cfg.srtCodec);
    if (s["srtKeyframeMs"].isNumber())
        cfg.srtKeyframeMs = std::clamp(s["srtKeyframeMs"].asInt(), 0, 60000);
    if (s["srtAudioKbps"].isNumber()) {
        const int kbps = s["srtAudioKbps"].asInt();
        cfg.srtAudioKbps = kbps <= 0 ? 0 : std::clamp(kbps, 32, 512);
    }
    if (s["recordBitrateKbps"].isNumber())
        cfg.recordBitrateKbps = std::max(0, s["recordBitrateKbps"].asInt());
    if (s["encoder"].isString())
        media::parseEncoderBackend(s["encoder"].asString(), cfg.encoder);
    if (s["encoderPreset"].isString())
        media::parseEncoderPreset(s["encoderPreset"].asString(),
                                  cfg.encoderPreset);
    for (auto* name : {&cfg.omtOutName, &cfg.cleanOmtOutName, &cfg.mvOmtOutName})
        if (name->empty()) *name = "8Kloud Switcher";
    session_.setPendingConfig(cfg);
    return refused;
}

// -- request handling --------------------------------------------------------

void WebServer::send(Client& c, std::string bytes) {
    if (c.out.size() + bytes.size() > kMaxOutBuf) {
        c.closing = true;  // slow reader: drop rather than buffer forever
        return;
    }
    c.out += bytes;
}

void WebServer::sendJson(Client& c, const json::Value& v) {
    send(c, wsText(v.dump()));
}

void WebServer::sendError(Client& c, const std::string& msg) {
    sendJson(c, json::Value{{"event", "error"}, {"message", msg}});
}

void WebServer::handleHttp(Client& c, const HttpRequest& req) {
    if (req.wantsUpgradeToWebSocket()) {
        if (req.path != "/ws") {
            send(c, httpResponse(404, "text/plain", "no such socket"));
            return;
        }
        send(c, websocketHandshakeResponse(req.header("sec-websocket-key")));
        c.ws = true;
        c.frameSeq = 0;
        sendJson(c, json::Value{{"event", "hello"},
                                {"name", "8Kloud Switcher"},
                                {"protocol", 1}});
        send(c, wsText(lastState_.empty() ? ctl::toJson(ctl::snapshot(engine_))
                                          : lastState_));
        send(c, wsText(uiJson()));
        return;
    }
    if (req.method != "GET" && req.method != "HEAD") {
        send(c, httpResponse(405, "text/plain", "GET only",
                             "Allow: GET, HEAD\r\n"));
        return;
    }
    const std::string& path = req.path;
    if (path == "/" || path == "/index.html") {
        send(c, httpResponse(200, contentTypeFor("index.html"),
                             asset(shaders::web_index_html,
                                   shaders::web_index_html_size)));
    } else if (path == "/app.js") {
        send(c, httpResponse(200, contentTypeFor(path),
                             asset(shaders::web_app_js, shaders::web_app_js_size)));
    } else if (path == "/app.css") {
        send(c, httpResponse(200, contentTypeFor(path),
                             asset(shaders::web_app_css, shaders::web_app_css_size)));
    } else if (path == "/logo.svg" || path == "/favicon.svg" ||
               path == "/favicon.ico") {
        send(c, httpResponse(200, contentTypeFor("logo.svg"),
                             asset(shaders::web_logo_svg,
                                   shaders::web_logo_svg_size),
                             "Cache-Control: max-age=86400\r\n"));
    } else if (path == "/api/state") {
        send(c, httpResponse(200, contentTypeFor("x.json"), uiJson()));
    } else if (path == "/api/control") {
        send(c, httpResponse(200, contentTypeFor("x.json"),
                             ctl::toJson(ctl::snapshot(engine_))));
    } else if (path == "/api/sources") {
        send(c, httpResponse(200, contentTypeFor("x.json"), sourcesJson().dump()));
    } else if (path == "/api/fs") {
        send(c, httpResponse(200, contentTypeFor("x.json"),
                             listDirectory(queryParam(req.query, "path")).dump()));
    } else if (path == "/mv.jpg") {
        std::shared_ptr<const std::string> frame;
        {
            std::lock_guard lk(frameM_);
            frame = frame_;
        }
        if (frame)
            send(c, httpResponse(200, "image/jpeg", *frame));
        else
            send(c, httpResponse(404, "text/plain", "no multiview frame yet"));
    } else {
        send(c, httpResponse(404, "text/plain", "not found"));
    }
}

void WebServer::handleWsJson(Client& c, const json::Value& msg) {
    const std::string cmd = msg["cmd"].asString();
    const int nIn = engine_.inputCount();
    auto inputIndex = [&](int& idx) {
        idx = msg["input"].asInt(0) - 1;
        if (idx >= 0 && idx < nIn) return true;
        sendError(c, "input " + std::to_string(idx + 1) + " out of range (1.." +
                         std::to_string(nIn) + ")");
        return false;
    };
    auto syncOf = [&](int def) {
        const int s = msg["sync"].asInt(def);
        return std::clamp(s, -1, 4);
    };

    if (cmd == "line") {
        handleWsText(c, msg["text"].asString());
    } else if (cmd == "sources") {
        sendJson(c, sourcesJson());
    } else if (cmd == "ls") {
        sendJson(c, listDirectory(msg["path"].asString()));
    } else if (cmd == "ui") {
        send(c, wsText(uiJson()));
    } else if (cmd == "replaceInput") {
        int idx;
        if (!inputIndex(idx)) return;
        std::string ref = msg["ref"].asString();
        const std::string typeName = msg["type"].asString("auto");
        InputSpec::Type type;
        if (typeName == "black" || ref.empty()) {
            ref.clear();
            type = InputSpec::Type::Omt;
        } else if (typeName == "auto") {
            std::error_code ec;
            type = ref.rfind("srt://", 0) == 0     ? InputSpec::Type::Srt
                   : isDeckLinkRef(ref)             ? InputSpec::Type::DeckLink
                   : std::filesystem::is_regular_file(ref, ec)
                       ? (media::isStillImagePath(ref) ? InputSpec::Type::Still
                                                       : InputSpec::Type::Media)
                       : InputSpec::Type::Omt;
        } else {
            type = inputTypeFromName(typeName);
        }
        if (type == InputSpec::Type::Srt && ref.rfind("srt://", 0) != 0)
            ref = "srt://" + ref;
        const int sync = syncOf(engine_.inputSyncFrames(idx));
        if (ref == engine_.inputRef(idx) && type == engine_.inputType(idx) &&
            sync == engine_.inputSyncFrames(idx))
            return;  // no-op
        engine_.requestInputReplace(idx, {type, ref, sync});
    } else if (cmd == "playlist") {
        int idx;
        if (!inputIndex(idx)) return;
        InputSpec spec;
        spec.type = InputSpec::Type::Media;
        spec.syncFrames = syncOf(engine_.inputSyncFrames(idx));
        if (const auto current = engine_.inputMediaState(idx); current.available) {
            spec.mediaPlaying = current.playing;
            spec.mediaLoop = current.loop;
        }
        for (const auto& item : msg["items"].asArray()) {
            media::PlaylistItem clip{item["path"].asString(),
                                     item["in"].asInt64(0),
                                     item["out"].asInt64(0),
                                     item["speed"].asInt(1000)};
            if (clip.path.empty()) continue;
            media::normalizePlaylistItem(clip);
            spec.mediaPlaylist.push_back(std::move(clip));
        }
        if (spec.mediaPlaylist.empty()) {
            sendError(c, "playlist is empty");
            return;
        }
        spec.ref = spec.mediaPlaylist.front().path;
        if (engine_.inputType(idx) == InputSpec::Type::Media &&
            spec.mediaPlaylist == engine_.inputMediaPlaylist(idx) &&
            spec.syncFrames == engine_.inputSyncFrames(idx))
            return;
        engine_.requestInputReplace(idx, std::move(spec));
    } else if (cmd == "sync") {
        // Frame sync only; keeps the current source (and a media playlist).
        int idx;
        if (!inputIndex(idx)) return;
        const int sync = syncOf(engine_.inputSyncFrames(idx));
        if (sync == engine_.inputSyncFrames(idx)) return;
        InputSpec spec{engine_.inputType(idx), engine_.inputRef(idx), sync};
        if (spec.type == InputSpec::Type::Media) {
            spec.mediaPlaylist = engine_.inputMediaPlaylist(idx);
            if (const auto cur = engine_.inputMediaState(idx); cur.available) {
                spec.mediaPlaying = cur.playing;
                spec.mediaLoop = cur.loop;
            }
        }
        if (spec.ref.empty()) return;
        engine_.requestInputReplace(idx, std::move(spec));
    } else if (cmd == "audioDelay" || cmd == "masterDelay") {
        auto* aud = engine_.audio();
        if (!aud) {
            sendError(c, "audio is disabled");
            return;
        }
        const int ms = std::clamp(msg["ms"].asInt(0), 0, 500);
        if (cmd == "masterDelay") {
            aud->masterDelayMs.store(std::min(ms, 200), std::memory_order_relaxed);
            return;
        }
        int idx;
        if (!inputIndex(idx) || idx >= aud->inputCount()) return;
        aud->channel(idx).delayMs.store(ms, std::memory_order_relaxed);
    } else if (cmd == "settings") {
        if (const std::string refused = applySettings(msg); !refused.empty())
            sendError(c, refused);
        send(c, wsText(uiJson()));
    } else {
        sendError(c, "unknown command '" + cmd + "'");
    }
}

void WebServer::handleWsText(Client& c, const std::string& text) {
    std::string_view line = text;
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
        line.remove_prefix(1);
    if (!line.empty() && line.front() == '{') {
        json::Value msg;
        std::string err;
        if (!json::parse(line, msg, err)) {
            sendError(c, "bad JSON: " + err);
            return;
        }
        handleWsJson(c, msg);
        return;
    }
    std::string err;
    const auto req = ctl::parseLine(line, err);
    if (!req) {
        if (!err.empty()) sendError(c, err);
        return;
    }
    if (req->op == ctl::Request::Op::Subscribe ||
        req->op == ctl::Request::Op::Unsubscribe)
        return;  // every web client is subscribed
    const auto res = ctl::apply(engine_, *req);
    if (!res.ok) sendError(c, res.error);
    if (!res.reply.empty()) send(c, wsText(res.reply));
}

// -- socket loop ------------------------------------------------------------

void WebServer::run(std::stop_token st) {
    int64_t nextUiNs = 0;
    while (!st.stop_requested()) {
        std::vector<pollfd> fds;
        fds.push_back({listenFd_, POLLIN, 0});
        fds.push_back({wakeFd_, POLLIN, 0});
        for (const auto& c : clients_)
            fds.push_back({c.fd, short(POLLIN | (c.out.empty() ? 0 : POLLOUT)), 0});
        if (poll(fds.data(), nfds_t(fds.size()), kPollMs) < 0 && errno != EINTR) {
            KLOUD_LOGE("web: poll: %s", strerror(errno));
            return;
        }
        if (st.stop_requested()) return;

        const size_t nPolled = clients_.size();
        if (fds[0].revents & POLLIN) {
            const int fd = accept4(listenFd_, nullptr, nullptr,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd >= 0) {
                if (clients_.size() >= kMaxClients) {
                    close(fd);
                } else {
                    const int one = 1;
                    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
                    Client c;
                    c.fd = fd;
                    clients_.push_back(std::move(c));
                }
            }
        }

        for (size_t i = 0; i < nPolled; ++i) {
            Client& c = clients_[i];
            if (!(fds[i + 2].revents & (POLLIN | POLLERR | POLLHUP))) continue;
            char buf[16384];
            for (;;) {
                const ssize_t n = recv(c.fd, buf, sizeof buf, 0);
                if (n > 0) {
                    c.in.append(buf, size_t(n));
                    if (c.in.size() > kMaxWsMessage * 2) {
                        c.closing = true;
                        break;
                    }
                    continue;
                }
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
                close(c.fd);
                c.fd = -1;
                break;
            }
            if (c.fd < 0 || c.closing) continue;

            if (!c.ws) {
                for (;;) {
                    HttpRequest req;
                    size_t consumed = 0;
                    const Parse p = parseHttpRequest(c.in, req, consumed);
                    if (p == Parse::Incomplete) break;
                    if (p == Parse::Bad) {
                        send(c, httpResponse(400, "text/plain", "bad request"));
                        c.closing = true;
                        break;
                    }
                    c.in.erase(0, consumed);
                    handleHttp(c, req);
                    if (c.ws) break;  // remaining bytes are WebSocket frames
                }
            }
            if (c.ws && !c.closing) {
                for (;;) {
                    WsFrame f;
                    size_t consumed = 0;
                    const Parse p = parseWsFrame(c.in, f, consumed, kMaxWsMessage);
                    if (p == Parse::Incomplete) break;
                    if (p == Parse::Bad) {
                        send(c, wsClose(1002));
                        c.closing = true;
                        break;
                    }
                    c.in.erase(0, consumed);
                    switch (f.opcode) {
                        case 8:  // close
                            send(c, wsClose());
                            c.closing = true;
                            break;
                        case 9: send(c, wsFrame(10, f.payload)); break;
                        case 10: break;
                        case 0:  // continuation
                            c.fragments += f.payload;
                            if (c.fragments.size() > kMaxWsMessage) {
                                c.closing = true;
                                break;
                            }
                            if (f.fin) {
                                if (c.fragmentOp == 1) handleWsText(c, c.fragments);
                                c.fragments.clear();
                                c.fragmentOp = 0;
                            }
                            break;
                        case 1:
                        case 2:
                            if (!f.fin) {
                                c.fragmentOp = f.opcode;
                                c.fragments = std::move(f.payload);
                            } else if (f.opcode == 1) {
                                handleWsText(c, f.payload);
                            }
                            break;
                        default:
                            send(c, wsClose(1002));
                            c.closing = true;
                    }
                    if (c.closing) break;
                }
            }
        }

        // Publish: control state on change, meters every poll, the rich
        // document on a slower clock, and the newest multiview frame to
        // anyone who has drained the previous one.
        const std::string state = ctl::toJson(ctl::snapshot(engine_));
        const bool stateChanged = state != lastState_;
        if (stateChanged) lastState_ = state;
        const int64_t now = MediaClock::nowNs();
        std::string ui;
        if (now >= nextUiNs) {
            nextUiNs = now + kUiEveryNs;
            ui = uiJson();
            if (ui == lastUi_) ui.clear();
            else lastUi_ = ui;
        }
        const std::string meters = metersJson();
        std::shared_ptr<const std::string> frame;
        const uint64_t frameSeq = frameSeq_.load(std::memory_order_acquire);
        if (frameSeq) {
            std::lock_guard lk(frameM_);
            frame = frame_;
        }
        for (auto& c : clients_) {
            if (c.fd < 0) continue;
            if (c.ws && !c.closing) {
                if (stateChanged) send(c, wsText(lastState_));
                if (!ui.empty()) send(c, wsText(ui));
                if (!meters.empty()) send(c, wsText(meters));
                if (frame && c.frameSeq != frameSeq && c.out.size() < 65536) {
                    c.frameSeq = frameSeq;
                    send(c, wsBinary(*frame));
                }
            }
            if (!c.out.empty()) {
                const ssize_t n =
                    ::send(c.fd, c.out.data(), c.out.size(), MSG_NOSIGNAL);
                if (n > 0)
                    c.out.erase(0, size_t(n));
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    close(c.fd);
                    c.fd = -1;
                    continue;
                }
            }
            if (c.closing && c.out.empty()) {
                close(c.fd);
                c.fd = -1;
            }
        }
        std::erase_if(clients_, [](const Client& c) { return c.fd < 0; });
    }
}

}  // namespace kloud::web
