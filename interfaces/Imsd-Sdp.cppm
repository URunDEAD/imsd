// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts®

// lint-disable-file fixed-width-types
/*
Imsd:Sdp — SDP offer/answer building + parsing for the voice media plane.

The offer is a fixed AMR-WB (octet-aligned) + AMR + telephone-event shape
with an optional GSMA IR.92 QoS-precondition block; it is what commercial IMS
cores expect from a VoLTE UE, and its exact bytes are pinned by tests/Sdp. The
parser extracts only what the media plane needs: remote address/port, the
negotiated payload type, the codec and octet-align mode — it reads answers
to our offers and inbound offers alike (same fields either way). The codecs
it knows are the ones imsd-media can play: AMR-WB, AMR (narrowband) and
G.711 (PCMA/PCMU) — the last two are what a PSTN gateway offers when a
landline calls. BuildAnswer is the terminating side: it answers an inbound
offer with the offer's own payload type numbers (RFC 3264) and mirrors its
octet-align mode (RFC 4867 requires both directions to use one mode). Pure
std C++.
*/

export module Imsd:Sdp;
import std;

namespace imsd::sdp {

    bool Is6(std::string_view addr) { return addr.contains(':'); }

    // First token (up to whitespace/CR/LF) starting at `pos`.
    std::string_view TokenAt(std::string_view body, std::size_t pos) {
        std::size_t end = body.find_first_of(" \t\r\n", pos);
        return body.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
    }

    std::optional<int> ParseInt(std::string_view s) {
        int v = 0;
        auto [p, ec] = std::from_chars(s.data(), s.data() + s.size(), v);
        if (ec != std::errc{} || p != s.data() + s.size()) return std::nullopt;
        return v;
    }

    // First occurrence of `needle` anywhere in the body; nullopt if absent.
    std::optional<std::size_t> Find(std::string_view body, std::string_view needle) {
        std::size_t at = body.find(needle);
        if (at == std::string_view::npos) return std::nullopt;
        return at;
    }
}

export namespace imsd::sdp {

    struct Offer {
        std::string local;              // connection address (v4 or v6)
        int rtpPort = 0;
        std::uint64_t sessionId = 0;    // o= sess-id/version; caller randomizes
        bool precond = false;           // IR.92 QoS preconditions block
        std::string currRemote = "none";  // a=curr:qos remote <this>
    };

    // Our offer: AMR-WB (octet-aligned) first, AMR narrowband second — the
    // 3GPP-mandatory codec every IMS core and PSTN gateway can answer, so a
    // call we place TO a landline no longer depends on the far side
    // transcoding up to wideband — each with telephone-event at its own
    // clock. Byte layout is pinned by tests/Sdp.
    std::string BuildOffer(const Offer& o) {
        std::string ipver = Is6(o.local) ? "IP6" : "IP4";
        std::string s;
        s += "v=0\r\n";
        s += std::format("o=- {} {} IN {} {}\r\n", o.sessionId, o.sessionId, ipver, o.local);
        s += "s=-\r\n";
        s += std::format("c=IN {} {}\r\n", ipver, o.local);
        s += "t=0 0\r\n";
        s += std::format("m=audio {} RTP/AVP 97 99 98 100\r\n", o.rtpPort);
        s += "b=AS:41\r\n";
        s += "b=RS:512\r\n";
        s += "b=RR:1536\r\n";
        s += "a=rtpmap:97 AMR-WB/16000/1\r\n";
        s += "a=fmtp:97 octet-align=1;mode-change-capability=2;max-red=0\r\n";
        s += "a=rtpmap:99 AMR/8000/1\r\n";
        s += "a=fmtp:99 octet-align=1;mode-change-capability=2;max-red=0\r\n";
        s += "a=rtpmap:98 telephone-event/16000\r\n";
        s += "a=fmtp:98 0-15\r\n";
        s += "a=rtpmap:100 telephone-event/8000\r\n";
        s += "a=fmtp:100 0-15\r\n";
        s += "a=ptime:20\r\n";
        s += "a=maxptime:240\r\n";
        if (o.precond) {
            s += "a=curr:qos local sendrecv\r\n";
            s += std::format("a=curr:qos remote {}\r\n", o.currRemote);
            s += "a=des:qos mandatory local sendrecv\r\n";
            s += "a=des:qos mandatory remote sendrecv\r\n";
        }
        s += "a=sendrecv\r\n";
        return s;
    }

    struct Answer {
        std::string ip;                 // empty = no c= line found
        int port = -1;                  // -1 = no m=audio line found
        int payloadType = -1;
        bool octetAlign = false;
        std::string codec = "AMR-WB";   // "AMR-WB" | "AMR" | "PCMA" | "PCMU" | "" (none we play)
    };

    // The encoding name an offer gives a payload type: its a=rtpmap line,
    // upper-cased (RFC 4566 §6: encoding names are case-insensitive), else
    // the static assignment for 0/8 (RFC 3551 §6 — a G.711 offer may carry
    // no rtpmap at all). Empty when neither applies.
    std::string CodecName(std::string_view body, int pt) {
        std::string key = std::format("a=rtpmap:{} ", pt);
        if (auto at = Find(body, key)) {
            std::size_t nameBegin = *at + key.size();
            std::size_t nameEnd = body.find_first_of("/\r\n", nameBegin);
            std::string name(body.substr(nameBegin, nameEnd == std::string_view::npos ? std::string_view::npos : nameEnd - nameBegin));
            for (char& c : name)
                c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            return name;
        }
        if (pt == 0) return "PCMU";
        if (pt == 8) return "PCMA";
        return "";
    }

    // The offer's audio payload types with their names, "8 PCMA, 96 AMR,
    // 101 telephone-event" — for the log line of a refused offer, so a
    // field journal says WHAT the gateway offered without a raw SIP dump.
    std::string OfferedCodecs(std::string_view body) {
        auto m = Find(body, "m=audio ");
        if (!m) return "no m=audio line";
        std::size_t lineEnd = body.find_first_of("\r\n", *m);
        std::string_view line = body.substr(*m, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - *m);
        std::string out;
        int field = 0;
        for (auto part : std::views::split(line, ' ')) {
            std::string_view tok(part);
            if (field++ < 3 || tok.empty()) continue;  // m=audio <port> <proto> <pt>...
            auto pt = ParseInt(tok);
            if (!pt) continue;
            std::string name = CodecName(body, *pt);
            if (!out.empty()) out += ", ";
            out += name.empty() ? std::format("{}", *pt) : std::format("{} {}", *pt, name);
        }
        return out.empty() ? std::string(line) : out;
    }

    // Payload-type preference: AMR-WB with octet-align=1 > AMR-WB > AMR with
    // octet-align=1 > AMR > PCMA > PCMU. Octet-aligned wins within a codec
    // because it is the media leg's native, call-proven format
    // (bandwidth-efficient is supported but was the s56 static-audio
    // culprit) — when the peer offers both variants, taking the
    // octet-aligned pt keeps real calls on the battle-tested path. Wideband
    // beats narrowband, AMR beats G.711 (a mobile-to-mobile call must never
    // land on a G.711 leg just because the gateway listed it). With no
    // playable codec at all, the first pt is used and codec comes back EMPTY
    // — pretending otherwise would defeat codec checks upstream.
    Answer ParseAnswer(std::string_view body) {
        Answer a;
        if (auto at = Find(body, "c=IN IP6 ")) a.ip = TokenAt(body, *at + 9);
        else if (auto at4 = Find(body, "c=IN IP4 ")) a.ip = TokenAt(body, *at4 + 9);

        auto m = Find(body, "m=audio ");
        if (!m) return a;
        std::string_view portTok = TokenAt(body, *m + 8);
        auto port = ParseInt(portTok);
        // both the port and the " RTP/AVP " literal are required
        std::size_t afterPort = *m + 8 + portTok.size();
        constexpr std::string_view Avp = " RTP/AVP ";
        if (!port || !body.substr(afterPort).starts_with(Avp)) return a;
        a.port = *port;

        // payload-type list: digits + spaces up to CR/LF
        std::size_t ptsBegin = afterPort + Avp.size();
        std::size_t ptsEnd = body.find_first_not_of("0123456789 ", ptsBegin);
        std::string_view ptsRun = body.substr(ptsBegin, ptsEnd == std::string_view::npos ? std::string_view::npos : ptsEnd - ptsBegin);
        std::vector<int> pts;
        for (auto part : std::views::split(ptsRun, ' '))
            if (auto v = ParseInt(std::string_view(part))) pts.push_back(*v);
        if (pts.empty()) return a;

        auto hasOctetAlign = [&](int p) {
            auto fm = Find(body, std::format("a=fmtp:{} ", p));
            if (!fm) return false;
            std::size_t lineEnd = body.find_first_of("\r\n", *fm);
            std::string_view line = body.substr(*fm, lineEnd == std::string_view::npos ? std::string_view::npos : lineEnd - *fm);
            return line.contains("octet-align=1");
        };
        int wbOa = -1;
        int wb = -1;
        int nbOa = -1;
        int nb = -1;
        int pcma = -1;
        int pcmu = -1;
        for (int p : pts) {
            std::string name = CodecName(body, p);
            if (name == "AMR-WB") {
                if (wb == -1) wb = p;
                if (wbOa == -1 && hasOctetAlign(p)) wbOa = p;
            } else if (name == "AMR") {
                if (nb == -1) nb = p;
                if (nbOa == -1 && hasOctetAlign(p)) nbOa = p;
            } else if (name == "PCMA") {
                if (pcma == -1) pcma = p;
            } else if (name == "PCMU") {
                if (pcmu == -1) pcmu = p;
            }
        }
        if (wbOa != -1 || wb != -1) {
            a.payloadType = wbOa != -1 ? wbOa : wb;
            a.codec = "AMR-WB";
        } else if (nbOa != -1 || nb != -1) {
            a.payloadType = nbOa != -1 ? nbOa : nb;
            a.codec = "AMR";
        } else if (pcma != -1) {
            a.payloadType = pcma;
            a.codec = "PCMA";
        } else if (pcmu != -1) {
            a.payloadType = pcmu;
            a.codec = "PCMU";
        } else {
            a.payloadType = pts.front();
            a.codec.clear();
        }
        a.octetAlign = a.codec.starts_with("AMR") && hasOctetAlign(a.payloadType);
        return a;
    }

    // Payload type of `telephone-event/<clockRate>` in a body's rtpmap lines
    // (DTMF, RFC 4733); nullopt when the offer doesn't carry one.
    std::optional<int> TelephoneEventPt(std::string_view body, int clockRate) {
        std::string needle = std::format("telephone-event/{}", clockRate);
        std::size_t pos = 0;
        while ((pos = body.find("a=rtpmap:", pos)) != std::string_view::npos) {
            std::size_t ptBegin = pos + 9;
            std::size_t lineEnd = body.find_first_of("\r\n", ptBegin);
            std::size_t sp = body.find(' ', ptBegin);
            if (sp != std::string_view::npos && (lineEnd == std::string_view::npos || sp < lineEnd)) {
                auto pt = ParseInt(body.substr(ptBegin, sp - ptBegin));
                std::string_view name = body.substr(sp + 1, (lineEnd == std::string_view::npos ? body.size() : lineEnd) - sp - 1);
                if (pt && name.starts_with(needle)) return pt;
            }
            pos = ptBegin;
        }
        return std::nullopt;
    }

    struct AnswerSpec {
        std::string local;              // connection address (v4 or v6)
        int rtpPort = 0;
        std::uint64_t sessionId = 0;    // o= sess-id/version; caller randomizes
        int payloadType = 97;           // the offer's pt for the chosen codec
        bool octetAlign = true;         // mirror of the offer's mode (AMR codecs only)
        std::string codec = "AMR-WB";   // "AMR-WB" | "AMR" | "PCMA" | "PCMU"
        std::optional<int> dtmfPt;      // the offer's telephone-event pt, if any
    };

    // Sample rate of a codec we play: AMR-WB is 16 kHz, everything else
    // (AMR, G.711) is narrowband 8 kHz. Also the RTP clock rate.
    int CodecRate(std::string_view codec) { return codec == "AMR-WB" ? 16000 : 8000; }

    // SDP answer to an inbound audio offer: single codec at the offer's own
    // payload type, octet-align echoed only when the offer used it (absence
    // = bandwidth-efficient, RFC 4867 §8.1; G.711 has no fmtp at all),
    // telephone-event kept when offered. b=AS is the codec's peak payload
    // rate plus IP/UDP/RTP overhead at 20 ms. Byte layout pinned by tests/Sdp.
    std::string BuildAnswer(const AnswerSpec& s) {
        std::string ipver = Is6(s.local) ? "IP6" : "IP4";
        int rate = CodecRate(s.codec);
        int as = s.codec == "AMR-WB" ? 41 : (s.codec == "AMR" ? 30 : 80);
        std::string out;
        out += "v=0\r\n";
        out += std::format("o=- {} {} IN {} {}\r\n", s.sessionId, s.sessionId, ipver, s.local);
        out += "s=-\r\n";
        out += std::format("c=IN {} {}\r\n", ipver, s.local);
        out += "t=0 0\r\n";
        if (s.dtmfPt) out += std::format("m=audio {} RTP/AVP {} {}\r\n", s.rtpPort, s.payloadType, *s.dtmfPt);
        else
            out += std::format("m=audio {} RTP/AVP {}\r\n", s.rtpPort, s.payloadType);
        out += std::format("b=AS:{}\r\n", as);
        out += "b=RS:512\r\n";
        out += "b=RR:1536\r\n";
        out += std::format("a=rtpmap:{} {}/{}/1\r\n", s.payloadType, s.codec, rate);
        if (s.octetAlign && s.codec.starts_with("AMR")) out += std::format("a=fmtp:{} octet-align=1\r\n", s.payloadType);
        if (s.dtmfPt) {
            out += std::format("a=rtpmap:{} telephone-event/{}\r\n", *s.dtmfPt, rate);
            out += std::format("a=fmtp:{} 0-15\r\n", *s.dtmfPt);
        }
        out += "a=ptime:20\r\n";
        out += "a=maxptime:240\r\n";
        out += "a=sendrecv\r\n";
        return out;
    }
}
