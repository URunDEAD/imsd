// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts®

// lint-disable-file fixed-width-types
// Imsd:Sdp unit tests — offer building (byte-exact) and answer parsing
// (payload-type preference, octet-align, address family). The IPv6 answer
// mirrors what a live commercial IMS core actually returns (AMR-WB pt=97
// octet-aligned, IPv6 media gateway).
import std;
import Imsd;

using namespace imsd::sdp;

namespace {
    int Failures = 0;

    void Check(bool cond, std::string_view msg) {
        if (!cond) {
            std::println(std::cerr, "FAIL: {}", msg);
            ++Failures;
        }
    }
}

int main() {
    // ---- BuildOffer, IPv6 + preconditions (the shape a VoLTE UE sends)
    {
        std::string sdp = BuildOffer({
            .local = "2001:db8:29e9:a05f::1",
            .rtpPort = 22222,
            .sessionId = 1234567,
            .precond = true,
        });
        std::string expected =
            "v=0\r\n"
            "o=- 1234567 1234567 IN IP6 2001:db8:29e9:a05f::1\r\n"
            "s=-\r\n"
            "c=IN IP6 2001:db8:29e9:a05f::1\r\n"
            "t=0 0\r\n"
            "m=audio 22222 RTP/AVP 97 99 98 100\r\n"
            "b=AS:41\r\n"
            "b=RS:512\r\n"
            "b=RR:1536\r\n"
            "a=rtpmap:97 AMR-WB/16000/1\r\n"
            "a=fmtp:97 octet-align=1;mode-change-capability=2;max-red=0\r\n"
            "a=rtpmap:99 AMR/8000/1\r\n"
            "a=fmtp:99 octet-align=1;mode-change-capability=2;max-red=0\r\n"
            "a=rtpmap:98 telephone-event/16000\r\n"
            "a=fmtp:98 0-15\r\n"
            "a=rtpmap:100 telephone-event/8000\r\n"
            "a=fmtp:100 0-15\r\n"
            "a=ptime:20\r\n"
            "a=maxptime:240\r\n"
            "a=curr:qos local sendrecv\r\n"
            "a=curr:qos remote none\r\n"
            "a=des:qos mandatory local sendrecv\r\n"
            "a=des:qos mandatory remote sendrecv\r\n"
            "a=sendrecv\r\n";
        Check(sdp == expected, "IPv6 precondition offer is byte-exact");
    }
    // ---- BuildOffer, IPv4 without preconditions
    {
        std::string sdp = BuildOffer({
            .local = "10.0.0.2",
            .rtpPort = 4000,
            .sessionId = 42,
            .precond = false,
        });
        Check(sdp.contains("o=- 42 42 IN IP4 10.0.0.2\r\n"), "IPv4 origin line");
        Check(sdp.contains("c=IN IP4 10.0.0.2\r\n"), "IPv4 connection line");
        Check(!sdp.contains("a=curr:qos"), "no precondition block");
        Check(sdp.ends_with("a=sendrecv\r\n"), "trailing sendrecv + CRLF");
    }

    // ---- ParseAnswer, real-world IPv6 answer
    {
        Answer a = ParseAnswer(
            "v=0\r\n"
            "o=- 99 99 IN IP6 2001:db8::307\r\n"
            "s=-\r\n"
            "c=IN IP6 2001:db8::307\r\n"
            "t=0 0\r\n"
            "m=audio 35868 RTP/AVP 97 98\r\n"
            "a=rtpmap:97 AMR-WB/16000/1\r\n"
            "a=fmtp:97 octet-align=1;mode-change-capability=2\r\n"
            "a=rtpmap:98 telephone-event/16000\r\n"
            "a=ptime:20\r\n");
        Check(a.ip == "2001:db8::307", "answer IP6");
        Check(a.port == 35868, "answer port");
        Check(a.payloadType == 97, "answer payload type");
        Check(a.octetAlign, "answer octet-align");
        Check(a.codec == "AMR-WB", "answer codec");
    }
    // ---- payload-type preference: AMR remembered, later AMR-WB overrides
    {
        Answer a = ParseAnswer("c=IN IP4 192.0.2.1\r\n" "m=audio 5000 RTP/AVP 96 97\r\n" "a=rtpmap:96 AMR/8000/1\r\n" "a=rtpmap:97 AMR-WB/16000/1\r\n");
        Check(a.payloadType == 97, "AMR-WB preferred over earlier AMR");
        Check(a.codec == "AMR-WB", "codec follows the preferred rtpmap");
    }
    // ---- octet-aligned AMR-WB preferred over an earlier BE AMR-WB
    // (the s56 MT-static shape: an offer listing bandwidth-efficient first)
    {
        Answer a = ParseAnswer(
            "c=IN IP6 2001:db8::1\r\n"
            "m=audio 5000 RTP/AVP 116 118 110\r\n"
            "a=rtpmap:116 AMR-WB/16000/1\r\n"
            "a=fmtp:116 mode-change-capability=2;max-red=0\r\n"
            "a=rtpmap:118 AMR-WB/16000/1\r\n"
            "a=fmtp:118 octet-align=1;mode-change-capability=2\r\n"
            "a=rtpmap:110 telephone-event/16000\r\n");
        Check(a.payloadType == 118, "octet-aligned AMR-WB wins over earlier BE");
        Check(a.octetAlign, "octet-align follows the chosen pt");
        Check(a.codec == "AMR-WB", "codec is AMR-WB");
    }
    // ---- BE-only AMR-WB offer stays BE at the first pt
    {
        Answer a = ParseAnswer(
            "c=IN IP6 2001:db8::1\r\n"
            "m=audio 5000 RTP/AVP 116 96\r\n"
            "a=rtpmap:116 AMR-WB/16000/1\r\n"
            "a=fmtp:116 mode-change-capability=2\r\n"
            "a=rtpmap:96 AMR/8000/1\r\n"
            "a=fmtp:96 octet-align=1\r\n");
        Check(a.payloadType == 116, "BE-only AMR-WB beats octet-aligned AMR");
        Check(!a.octetAlign, "BE-only offer negotiates bandwidth-efficient");
    }
    // ---- AMR-only answer
    {
        Answer a = ParseAnswer("c=IN IP4 192.0.2.1\r\n" "m=audio 5000 RTP/AVP 96\r\n" "a=rtpmap:96 AMR/8000/1\r\n" "a=fmtp:96 octet-align=1\r\n");
        Check(a.payloadType == 96 && a.codec == "AMR", "plain AMR fallback");
        Check(a.octetAlign, "octet-align seen for AMR too");
    }
    // ---- G.711 by static payload type, no rtpmap at all (RFC 3551 §6):
    // the shape a PSTN gateway sends for a landline caller
    {
        Answer a = ParseAnswer("c=IN IP4 192.0.2.1\r\n" "m=audio 5000 RTP/AVP 0 8\r\n");
        Check(a.payloadType == 8, "PCMA preferred over an earlier PCMU");
        Check(a.codec == "PCMA", "static pt 8 -> PCMA without an rtpmap line");
        Check(!a.octetAlign, "G.711 never octet-aligned");
    }
    {
        Answer a = ParseAnswer("c=IN IP4 192.0.2.1\r\n" "m=audio 5000 RTP/AVP 0\r\n");
        Check(a.payloadType == 0 && a.codec == "PCMU", "static pt 0 -> PCMU");
    }
    // ---- G.711 with an rtpmap, lower-case name (RFC 4566: case-insensitive)
    {
        Answer a = ParseAnswer("c=IN IP4 192.0.2.1\r\n" "m=audio 5000 RTP/AVP 8 101\r\n" "a=rtpmap:8 pcma/8000\r\n" "a=rtpmap:101 telephone-event/8000\r\n");
        Check(a.payloadType == 8 && a.codec == "PCMA", "lower-case rtpmap name still recognised");
    }
    {
        Answer a = ParseAnswer("c=IN IP6 2001:db8::1\r\n" "m=audio 5000 RTP/AVP 96\r\n" "a=rtpmap:96 amr-wb/16000/1\r\n" "a=fmtp:96 octet-align=1\r\n");
        Check(a.payloadType == 96 && a.codec == "AMR-WB" && a.octetAlign, "lower-case amr-wb recognised, octet-align kept");
    }
    // ---- narrowband AMR beats G.711 (a gateway listing both)
    {
        Answer a = ParseAnswer("c=IN IP4 192.0.2.1\r\n" "m=audio 5000 RTP/AVP 8 100 101\r\n" "a=rtpmap:100 AMR/8000/1\r\n" "a=fmtp:100 octet-align=1\r\n" "a=rtpmap:101 telephone-event/8000\r\n");
        Check(a.payloadType == 100 && a.codec == "AMR", "AMR preferred over PCMA listed first");
        Check(a.octetAlign, "AMR octet-align seen");
    }
    // ---- wideband beats everything: KPN's real mobile-origin list
    // (AMR-WB BE 116, AMR-WB OA 107, PCMA 8, AMR 100, DTMF 111/110, CN 13)
    {
        Answer a = ParseAnswer(
            "c=IN IP6 2001:db8::310\r\n"
            "m=audio 5000 RTP/AVP 116 107 8 100 111 110 13\r\n"
            "a=rtpmap:116 AMR-WB/16000/1\r\n"
            "a=fmtp:116 mode-change-capability=2;max-red=0\r\n"
            "a=rtpmap:107 AMR-WB/16000/1\r\n"
            "a=fmtp:107 octet-align=1;mode-change-capability=2;max-red=0\r\n"
            "a=rtpmap:100 AMR/8000/1\r\n"
            "a=rtpmap:111 telephone-event/16000\r\n"
            "a=rtpmap:110 telephone-event/8000\r\n"
            "a=rtpmap:13 CN/8000\r\n");
        Check(a.payloadType == 107 && a.codec == "AMR-WB" && a.octetAlign, "mobile caller still lands on octet-aligned AMR-WB");
    }
    // ---- nothing we play -> first payload type, codec EMPTY (the 488 case)
    {
        Answer a = ParseAnswer("c=IN IP4 192.0.2.1\r\n" "m=audio 5000 RTP/AVP 18 101\r\n" "a=rtpmap:18 G729/8000\r\n" "a=rtpmap:101 telephone-event/8000\r\n");
        Check(a.payloadType == 18, "no playable codec -> first payload type");
        Check(!a.octetAlign, "no fmtp -> bandwidth-efficient");
        Check(a.codec.empty(), "G729-only -> codec unknown, not assumed");
    }

    // ---- OfferedCodecs: the 488 log summary
    {
        Check(OfferedCodecs("m=audio 5000 RTP/AVP 18 101\r\na=rtpmap:18 G729/8000\r\na=rtpmap:101 telephone-event/8000\r\n") == "18 G729, 101 TELEPHONE-EVENT", "offered list names each pt");
        Check(OfferedCodecs("m=audio 5000 RTP/AVP 0 8\r\n") == "0 PCMU, 8 PCMA", "static G.711 types named without rtpmap");
        Check(OfferedCodecs("m=audio 5000 RTP/AVP 96\r\n") == "96", "unknown dynamic pt listed bare");
        Check(OfferedCodecs("v=0\r\n") == "no m=audio line", "no audio line says so");
    }
    // ---- CodecRate
    {
        Check(CodecRate("AMR-WB") == 16000, "AMR-WB is 16 kHz");
        Check(CodecRate("AMR") == 8000 && CodecRate("PCMA") == 8000 && CodecRate("PCMU") == 8000, "narrowband codecs are 8 kHz");
    }
    // ---- degenerate body
    {
        Answer a = ParseAnswer("v=0\r\n");
        Check(a.ip.empty() && a.port == -1 && a.payloadType == -1, "empty answer parses to absent fields");
    }

    // ---- TelephoneEventPt
    {
        std::string_view offer =
            "m=audio 27864 RTP/AVP 104 105\r\n"
            "a=rtpmap:104 AMR-WB/16000/1\r\n"
            "a=rtpmap:105 telephone-event/16000\r\n"
            "a=rtpmap:106 telephone-event/8000\r\n";
        Check(TelephoneEventPt(offer, 16000) == 105, "telephone-event pt by clock rate");
        Check(TelephoneEventPt(offer, 8000) == 106, "narrowband telephone-event pt");
        Check(!TelephoneEventPt(offer, 48000).has_value(), "absent clock rate -> nullopt");
        Check(!TelephoneEventPt("m=audio 1 RTP/AVP 0\r\n", 16000).has_value(), "no rtpmap -> nullopt");
    }

    // ---- BuildAnswer: the offer's own payload types, octet-align mirrored
    {
        std::string sdp = BuildAnswer({
            .local = "2001:db8:29e9:a05f::1",
            .rtpPort = 50004,
            .sessionId = 777,
            .payloadType = 104,
            .octetAlign = true,
            .codec = "AMR-WB",
            .dtmfPt = 105,
        });
        std::string expected =
            "v=0\r\n"
            "o=- 777 777 IN IP6 2001:db8:29e9:a05f::1\r\n"
            "s=-\r\n"
            "c=IN IP6 2001:db8:29e9:a05f::1\r\n"
            "t=0 0\r\n"
            "m=audio 50004 RTP/AVP 104 105\r\n"
            "b=AS:41\r\n"
            "b=RS:512\r\n"
            "b=RR:1536\r\n"
            "a=rtpmap:104 AMR-WB/16000/1\r\n"
            "a=fmtp:104 octet-align=1\r\n"
            "a=rtpmap:105 telephone-event/16000\r\n"
            "a=fmtp:105 0-15\r\n"
            "a=ptime:20\r\n"
            "a=maxptime:240\r\n"
            "a=sendrecv\r\n";
        Check(sdp == expected, "AMR-WB answer is byte-exact");
    }
    // ---- BuildAnswer: bandwidth-efficient offer, no DTMF, narrowband
    {
        std::string sdp = BuildAnswer({
            .local = "10.0.0.2",
            .rtpPort = 4000,
            .sessionId = 8,
            .payloadType = 96,
            .octetAlign = false,
            .codec = "AMR",
        });
        Check(sdp.contains("m=audio 4000 RTP/AVP 96\r\n"), "single-pt m-line");
        Check(sdp.contains("a=rtpmap:96 AMR/8000/1\r\n"), "narrowband rtpmap");
        Check(!sdp.contains("octet-align"), "bandwidth-efficient stays unmarked");
        Check(!sdp.contains("telephone-event"), "no DTMF when the offer had none");
        Check(sdp.contains("b=AS:30\r\n"), "narrowband bandwidth");
    }
    // ---- BuildAnswer: G.711 for a landline caller — static pt, no fmtp,
    // narrowband DTMF, byte-exact
    {
        std::string sdp = BuildAnswer({
            .local = "2001:db8:29e9:a05f::1",
            .rtpPort = 50004,
            .sessionId = 778,
            .payloadType = 8,
            .octetAlign = false,
            .codec = "PCMA",
            .dtmfPt = 101,
        });
        std::string expected =
            "v=0\r\n"
            "o=- 778 778 IN IP6 2001:db8:29e9:a05f::1\r\n"
            "s=-\r\n"
            "c=IN IP6 2001:db8:29e9:a05f::1\r\n"
            "t=0 0\r\n"
            "m=audio 50004 RTP/AVP 8 101\r\n"
            "b=AS:80\r\n"
            "b=RS:512\r\n"
            "b=RR:1536\r\n"
            "a=rtpmap:8 PCMA/8000/1\r\n"
            "a=rtpmap:101 telephone-event/8000\r\n"
            "a=fmtp:101 0-15\r\n"
            "a=ptime:20\r\n"
            "a=maxptime:240\r\n"
            "a=sendrecv\r\n";
        Check(sdp == expected, "PCMA answer is byte-exact");
    }
    // ---- BuildAnswer never marks G.711 octet-aligned even if asked
    {
        std::string sdp = BuildAnswer({.local = "10.0.0.2", .rtpPort = 4000, .sessionId = 9, .payloadType = 0, .octetAlign = true, .codec = "PCMU"});
        Check(sdp.contains("a=rtpmap:0 PCMU/8000/1\r\n") && !sdp.contains("fmtp:0"), "PCMU answer has no fmtp");
    }

    if (Failures == 0) std::println("Sdp: all tests passed");
    return Failures;
}
