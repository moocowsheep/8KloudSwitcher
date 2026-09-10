/* 8Kloud Switcher — a live video switcher for Linux + NVIDIA.
 * Copyright (c) 2026 Devin Block
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once
#include <string>
#include <string_view>

namespace kloud {

// The SRT output address as the console edits it. The show file, the CLI
// and the engine keep the FFmpeg URL string (srt://host:port?opt=...); this
// splits that string into the fields the OUTPUTS tab shows and composes it
// back, so the URL stays the single stored form and an operator can still
// paste a complete URL from elsewhere. Options FFmpeg's srt protocol takes
// but the console has no field for (pkt_size, maxbw, oheadbw, tlpktdrop,
// ...) survive a parse/compose round trip verbatim in `extra`.
//
// Values are kept exactly as written: FFmpeg does not percent-decode the
// query either, so a '&' cannot appear inside a passphrase or stream id.
struct SrtUrl {
    enum class Mode { Listener, Caller, Rendezvous };

    Mode mode = Mode::Listener;
    std::string host;      // empty in listener mode = every interface
    int port = 9710;
    int latencyMs = 120;   // 0 = leave it to libsrt (also 120 ms)
    std::string passphrase;  // empty = no encryption; libsrt wants 10..79 chars
    int keyLen = 0;        // pbkeylen: 0 = libsrt default, else 16, 24 or 32
    std::string streamId;
    std::string extra;     // remaining query options, "k=v&k2=v2"

    static const char* modeName(Mode mode);
    // Accepts "listener", "caller", "rendezvous"; false (out untouched)
    // otherwise.
    static bool parseMode(std::string_view text, Mode& out);

    // Splits an srt:// URL. An empty (or blank) URL yields the defaults, a
    // listener on 9710 at 120 ms. Any other scheme returns false and leaves
    // `out` at the defaults. A URL without mode= is a caller, FFmpeg's own
    // default, so compose() then merely spells that out.
    static bool parse(std::string_view url, SrtUrl& out);

    // srt://host:port?mode=...&latency=<usec>[&passphrase=..][&pbkeylen=..]
    // [&streamid=..][&extra]. latency is written in microseconds, the unit
    // FFmpeg's srt protocol reads.
    std::string compose() const;

    bool operator==(const SrtUrl&) const = default;
};

}  // namespace kloud
