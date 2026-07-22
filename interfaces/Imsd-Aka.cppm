// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts®

// lint-disable-file fixed-width-types
/*
Imsd:Aka — USIM AKA (3GPP TS 33.102) glue for IMS registration:

  * the AKAv1-MD5 HTTP-digest response (RFC 3310) the protected REGISTER
    carries in its Authorization header — MD5 over the binary RES;
  * the home-network identity (IMPI/IMPU/registration URI/realm) derived
    from IMSI + MCC/MNC;
  * text parsers for the SIM facts the daemon shell scrapes from qmicli
    (`--uim-get-card-status`: which slot holds a ready USIM and its AID) and
    ModemManager (`mmcli`: the primary SIM path, then IMSI + operator id);
  * the ISO 7816 APDU for the AUTHENTICATE(AKA) command and the parser for
    its 0xDB success response (RES / CK / IK TLV).

All pure std C++: the actual card I/O (open channel, transmit APDU) is the
shell's job — this module only shapes the bytes and reads the answers back.
Digest and parser behavior is pinned by tests/Aka.
*/

export module Imsd:Aka;
import std;
import :Util;

export namespace imsd::aka {

    // The home-network identity a USIM registers under.
    struct Identity {
        std::string imsi;
        std::string mcc;
        std::string mnc;      // already zero-padded to 3 digits
        std::string domain;   // ims.mncXXX.mccYYY.3gppnetwork.org
        std::string impi;     // <imsi>@<domain>
        std::string impu;     // sip:<imsi>@<domain>
        std::string regUri;   // sip:<domain>
    };

    // Build the identity from IMSI + MCC + a (2- or 3-digit) MNC. The MNC is
    // zero-padded to 3 digits, matching the Python detect_sim()/mnc.zfill(3)
    // — KPN's operator id 20408 becomes mnc008 in the domain.
    Identity MakeIdentity(std::string_view imsi, std::string_view mcc, std::string_view mnc) {
        std::string mnc3(mnc);
        while (mnc3.size() < 3)
            mnc3.insert(mnc3.begin(), '0');
        Identity id;
        id.imsi = imsi;
        id.mcc = mcc;
        id.mnc = mnc3;
        id.domain = std::format("ims.mnc{}.mcc{}.3gppnetwork.org", mnc3, mcc);
        id.impi = std::format("{}@{}", imsi, id.domain);
        id.impu = std::format("sip:{}@{}", imsi, id.domain);
        id.regUri = std::format("sip:{}", id.domain);
        return id;
    }

    // AKAv1-MD5 digest-response (RFC 3310 §3.2): HA1 = MD5(IMPI:realm:RES)
    // where RES is the *binary* AKA result, HA2 = MD5(method:uri), and the
    // response = MD5(HA1:nonce:nc:cnonce:qop:HA2). realm is the home domain.
    std::string DigestAkav1(std::string_view nonce,
                            std::span<const std::uint8_t> res,
                            std::string_view cnonce, std::string_view uri,
                            std::string_view impi, std::string_view realm,
                            std::string_view nc = "00000001",
                            std::string_view qop = "auth",
                            std::string_view method = "REGISTER") {
        imsd::util::Md5 h1;
        h1.Update(impi);
        h1.Update(":");
        h1.Update(realm);
        h1.Update(":");
        h1.Update(res);
        std::string ha1 = imsd::util::ToHex(h1.Digest());
        std::string ha2 = imsd::util::Md5::HexDigest(std::format("{}:{}", method, uri));
        return imsd::util::Md5::HexDigest(std::format("{}:{}:{}:{}:{}:{}", ha1, nonce, nc, cnonce, qop, ha2));
    }

    // Which USIM the modem should authenticate against.
    struct SimSelection {
        int slot = 0;
        std::string aid;   // uppercase, colons stripped
    };

    // Parse qmicli `--uim-get-card-status`: the first ready USIM application,
    // its slot number, and its Application ID (the hex line under
    // "Application ID:"). nullopt if no ready USIM is present. Mirrors the
    // Python detect_sim() scan exactly.
    std::optional<SimSelection> ParseCardStatus(std::string_view text) {
        int curSlot = 0;
        bool inUsim = false;
        bool ready = false;
        std::optional<SimSelection> out;
        std::size_t pos = 0;
        auto isAidLine = [](std::string_view s) {
            // trimmed line is HH(:HH)+ — two-or-more colon-separated hex pairs
            std::size_t i = 0;
            int pairs = 0;
            auto hex = [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
            };
            while (i < s.size()) {
                if (i + 2 > s.size() || !hex(s[i]) || !hex(s[i + 1])) return false;
                i += 2;
                pairs++;
                if (i == s.size()) break;
                if (s[i] != ':') return false;
                i++;
            }
            return pairs >= 2;
        };
        while (pos <= text.size()) {
            std::size_t nl = text.find('\n', pos);
            std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
            std::string_view t = line;
            while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
                t.remove_prefix(1);
            while (!t.empty() && (t.back() == ' ' || t.back() == '\t' || t.back() == '\r'))
                t.remove_suffix(1);

            if (t.starts_with("Slot [")) {
                std::size_t lb = t.find('[');
                std::size_t rb = t.find(']', lb);
                if (lb != std::string_view::npos && rb != std::string_view::npos) {
                    int v = 0;
                    std::from_chars(t.data() + lb + 1, t.data() + rb, v);
                    curSlot = v;
                }
                inUsim = ready = false;
            } else if (t.contains("Application type") && t.contains("usim")) {
                inUsim = true;
            } else if (inUsim && t.contains("Application state") && t.contains("ready")) {
                ready = true;
            } else if (inUsim && isAidLine(t)) {
                if (ready && !out) {
                    std::string aid;
                    for (char c : t)
                        if (c != ':') aid.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
                    out = SimSelection{curSlot, aid};
                }
                inUsim = false;
            }
            if (nl == std::string_view::npos) break;
            pos = nl + 1;
        }
        return out;
    }

    // "primary sim path: <path>" from `mmcli -m any`.
    std::optional<std::string> ParsePrimarySimPath(std::string_view text) {
        std::size_t at = text.find("primary sim path:");
        if (at == std::string_view::npos) return std::nullopt;
        std::string_view rest = text.substr(at + std::string_view("primary sim path:").size());
        std::size_t nl = rest.find('\n');
        std::string_view line = rest.substr(0, nl == std::string_view::npos ? rest.size() : nl);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
            line.remove_suffix(1);
        if (line.empty()) return std::nullopt;
        return std::string(line);
    }

    struct SimInfo {
        std::string imsi;
        std::string mcc;
        std::string mnc;   // as printed (operator id minus mcc), not padded
    };

    // Grab a labelled numeric value ("imsi:" / "operator id:") from mmcli -i
    // output. Values are digit runs.
    namespace detail {
        inline std::optional<std::string> Digits(std::string_view text, std::string_view label) {
            std::size_t at = text.find(label);
            if (at == std::string_view::npos) return std::nullopt;
            std::size_t i = at + label.size();
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
                i++;
            std::size_t start = i;
            while (i < text.size() && text[i] >= '0' && text[i] <= '9')
                i++;
            if (i == start) return std::nullopt;
            return std::string(text.substr(start, i - start));
        }
    }

    // "equipment id: <imei>" (15 digits) from `mmcli -m any`.
    std::optional<std::string> ParseEquipmentId(std::string_view text) {
        return detail::Digits(text, "equipment id:");
    }

    // RFC 7254 IMEI URN from a 15-digit IMEI (TAC-SNR-check digit) — the
    // +sip.instance value a UE registers with; nullopt for anything else.
    std::optional<std::string> ImeiUrn(std::string_view imei) {
        if (imei.size() != 15) return std::nullopt;
        for (char c : imei)
            if (c < '0' || c > '9') return std::nullopt;
        return std::format("urn:gsma:imei:{}-{}-{}", imei.substr(0, 8), imei.substr(8, 6), imei.substr(14));
    }

    // IMSI + operator id (MCC = first 3 digits, MNC = the rest) from `mmcli -i`.
    std::optional<SimInfo> ParseSimInfo(std::string_view text) {
        auto imsi = detail::Digits(text, "imsi:");
        auto op = detail::Digits(text, "operator id:");
        if (!imsi || !op || op->size() < 4) return std::nullopt;
        SimInfo s;
        s.imsi = *imsi;
        s.mcc = op->substr(0, 3);
        s.mnc = op->substr(3);
        return s;
    }

    // P-Access-Network-Info value from `mmcli -m any --location-get`:
    // "3GPP-E-UTRAN-FDD;utran-cell-id-3gpp=<mcc><mnc><tac:4hex><eci:7hex>"
    // (TS 24.229 7.2A.4: E-UTRAN cell id = MCC + MNC as broadcast + 16-bit
    // TAC + 28-bit ECI; mmcli prints both hex fields zero-padded wider, so
    // take the low digits). The stock modem sends exactly this shape in
    // every REGISTER/INVITE — the TAS reads it to see which access the UE
    // is reachable on. FDD is a constant: every KPN LTE band in use is FDD
    // and MM does not expose the duplex mode. nullopt when the fields are
    // missing or zero (no serving cell) — callers then omit the header
    // rather than fabricate one.
    std::optional<std::string> ParsePani(std::string_view text) {
        auto mcc = detail::Digits(text, "operator mcc:");
        auto mnc = detail::Digits(text, "operator mnc:");
        auto hexAfter = [&](std::string_view label) -> std::optional<std::string> {
            std::size_t at = text.find(label);
            if (at == std::string_view::npos) return std::nullopt;
            std::size_t i = at + label.size();
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
                i++;
            std::size_t start = i;
            while (i < text.size() && std::isxdigit(static_cast<unsigned char>(text[i])))
                i++;
            if (i == start) return std::nullopt;
            return std::string(text.substr(start, i - start));
        };
        auto tac = hexAfter("tracking area code:");
        auto ci = hexAfter("cell id:");
        if (!mcc || !mnc || !tac || !ci) return std::nullopt;
        if (tac->size() > 4) *tac = tac->substr(tac->size() - 4);
        while (tac->size() < 4) tac->insert(tac->begin(), '0');
        if (ci->size() > 7) *ci = ci->substr(ci->size() - 7);
        while (ci->size() < 7) ci->insert(ci->begin(), '0');
        if (tac->find_first_not_of('0') == std::string::npos && ci->find_first_not_of('0') == std::string::npos) return std::nullopt;
        return std::format("3GPP-E-UTRAN-FDD;utran-cell-id-3gpp={}{}{}{}", *mcc, *mnc, *tac, *ci);
    }

    // ---- AUTHENTICATE(AKA) APDU (ISO 7816-4 over the qmicli UIM channel) ----

    // The command APDU (hex, no separators) for a 3G AKA authentication:
    //   CLA=<channel byte>  INS=88  P1=00  P2=81  Lc  [10 RAND(16) 10 AUTN(16)]
    // `channel` is the logical-channel number qmicli returned; the CLA low
    // nibble carries it (Python: cla = "%02x" % ch).
    std::string BuildAkaApdu(int channel, std::span<const std::uint8_t> rand16, std::span<const std::uint8_t> autn16) {
        std::string data = std::format("10{}10{}", imsd::util::ToHex(rand16), imsd::util::ToHex(autn16));
        std::size_t lc = data.size() / 2;
        // CLA INS=88 P1=00 P2=81 Lc <data>
        return std::format("{:02x}880081{:02x}{}", channel, lc, data);
    }

    // GET RESPONSE APDU for when the card answers 61xx (xx bytes available).
    std::string BuildGetResponseApdu(int channel, int le) {
        return std::format("{:02x}c00000{:02x}", channel, le & 0xFF);
    }

    struct AkaResult {
        std::vector<std::uint8_t> res;
        std::vector<std::uint8_t> ck;
        std::vector<std::uint8_t> ik;
    };

    // Parse the card's AUTHENTICATE response body. `resp` is the raw APDU
    // response INCLUDING the trailing SW1 SW2 (two status bytes), which are
    // stripped here. A successful 3G AKA body is:
    //   DB  L_RES RES..  L_CK CK..  L_IK IK..
    // (tag 0xDB = "AKA success"; 0xDC would be sync-failure). nullopt on a
    // short buffer, a non-DB tag, or a length that runs past the end.
    std::optional<AkaResult> ParseAkaResponse(std::span<const std::uint8_t> resp) {
        if (resp.size() < 3) return std::nullopt;
        std::span<const std::uint8_t> body = resp.subspan(0, resp.size() - 2);
        if (body.empty() || body[0] != 0xDB) return std::nullopt;
        std::size_t i = 1;
        auto take = [&](std::vector<std::uint8_t>& out) -> bool {
            if (i >= body.size()) return false;
            std::size_t n = body[i++];
            if (i + n > body.size()) return false;
            out.assign(body.begin() + i, body.begin() + i + n);
            i += n;
            return true;
        };
        AkaResult r;
        if (!take(r.res) || !take(r.ck) || !take(r.ik)) return std::nullopt;
        return r;
    }
}
