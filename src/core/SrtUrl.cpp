/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include "core/SrtUrl.h"

#include <cstdlib>

namespace kloud {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' ||
                          s.back() == '\r' || s.back() == '\n'))
        s.remove_suffix(1);
    return s;
}

// Whole-string decimal; false on anything else (FFmpeg would reject it too).
bool toInt(std::string_view s, long long& out) {
    if (s.empty()) return false;
    const std::string copy(s);
    char* end = nullptr;
    out = strtoll(copy.c_str(), &end, 10);
    return end != copy.c_str() && *end == '\0';
}

void appendExtra(std::string& extra, std::string_view item) {
    if (!extra.empty()) extra += '&';
    extra += item;
}

}  // namespace

const char* SrtUrl::modeName(Mode mode) {
    switch (mode) {
        case Mode::Caller: return "caller";
        case Mode::Rendezvous: return "rendezvous";
        default: return "listener";
    }
}

bool SrtUrl::parseMode(std::string_view text, Mode& out) {
    if (text == "listener") out = Mode::Listener;
    else if (text == "caller") out = Mode::Caller;
    else if (text == "rendezvous") out = Mode::Rendezvous;
    else return false;
    return true;
}

bool SrtUrl::parse(std::string_view url, SrtUrl& out) {
    out = SrtUrl{};
    url = trim(url);
    if (url.empty()) return true;
    constexpr std::string_view kScheme = "srt://";
    if (url.substr(0, kScheme.size()) != kScheme) return false;
    url.remove_prefix(kScheme.size());

    std::string_view authority = url, query;
    if (const auto q = url.find('?'); q != std::string_view::npos) {
        authority = url.substr(0, q);
        query = url.substr(q + 1);
    }
    // A path is meaningless to SRT; FFmpeg ignores it, so drop it.
    if (const auto slash = authority.find('/'); slash != std::string_view::npos)
        authority = authority.substr(0, slash);

    std::string_view hostPart = authority, portPart;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) return false;
        hostPart = authority.substr(1, close - 1);
        const auto rest = authority.substr(close + 1);
        if (!rest.empty() && rest.front() == ':') portPart = rest.substr(1);
    } else if (const auto colon = authority.rfind(':');
               colon != std::string_view::npos) {
        hostPart = authority.substr(0, colon);
        portPart = authority.substr(colon + 1);
    }
    out.host = std::string(hostPart);
    long long port = 0;
    out.port = (toInt(portPart, port) && port >= 0 && port <= 65535) ? int(port)
                                                                      : 0;

    // Unset in the URL means FFmpeg's defaults: a caller with libsrt's own
    // latency. The parsed view reports exactly that.
    out.mode = Mode::Caller;
    out.latencyMs = 0;
    while (!query.empty()) {
        std::string_view item = query;
        if (const auto amp = query.find('&'); amp != std::string_view::npos) {
            item = query.substr(0, amp);
            query.remove_prefix(amp + 1);
        } else {
            query = {};
        }
        if (item.empty()) continue;
        const auto eq = item.find('=');
        const std::string_view key = item.substr(0, eq);
        const std::string_view value =
            eq == std::string_view::npos ? std::string_view{} : item.substr(eq + 1);
        long long n = 0;
        if (key == "mode" && parseMode(value, out.mode)) continue;
        if (key == "latency" && toInt(value, n) && n >= 0) {
            out.latencyMs = int((n + 500) / 1000);
            continue;
        }
        if (key == "passphrase") {
            out.passphrase = std::string(value);
            continue;
        }
        if (key == "pbkeylen" && toInt(value, n) &&
            (n == 0 || n == 16 || n == 24 || n == 32)) {
            out.keyLen = int(n);
            continue;
        }
        if (key == "streamid") {
            out.streamId = std::string(value);
            continue;
        }
        appendExtra(out.extra, item);
    }
    return true;
}

std::string SrtUrl::compose() const {
    std::string url = "srt://";
    const bool ipv6 = host.find(':') != std::string::npos && host.front() != '[';
    if (ipv6) url += '[';
    url += host;
    if (ipv6) url += ']';
    url += ':';
    url += std::to_string(port);
    url += "?mode=";
    url += modeName(mode);
    if (latencyMs > 0) {
        url += "&latency=";
        url += std::to_string(int64_t(latencyMs) * 1000);
    }
    if (!passphrase.empty()) {
        url += "&passphrase=";
        url += passphrase;
    }
    // Kept even without a passphrase (libsrt ignores it then), so a key
    // length chosen before the passphrase is typed does not snap back.
    if (keyLen > 0) {
        url += "&pbkeylen=";
        url += std::to_string(keyLen);
    }
    if (!streamId.empty()) {
        url += "&streamid=";
        url += streamId;
    }
    if (!extra.empty()) {
        url += '&';
        url += extra;
    }
    return url;
}

}  // namespace kloud
