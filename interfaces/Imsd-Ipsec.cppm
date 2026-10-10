// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts®

// lint-disable-file fixed-width-types
/*
Imsd:Ipsec — IMS security-association plumbing expressed as `ip xfrm` command
lines, plus a reader for an SA already installed in the kernel.

A registered IMS UE keeps two ESP SA pairs with the P-CSCF (TS 33.203):
reqid 1 carries the UE-initiated client flow (protected client port ⇄ P-CSCF
server port), reqid 2 the terminating server flow. Integrity is the negotiated
alg (HMAC-SHA-1-96 or HMAC-MD5-96) keyed with IK; confidentiality is the
negotiated ealg keyed with CK. Both come from the Security-Server mechanism
the P-CSCF selected in its 401 (parsed here), and this module turns the
negotiated SPIs/ports/keys into the exact argv lists the daemon shell hands to
`ip`; for a warm resume it reconstructs the SPIs and ports back out of `ip
xfrm state`/`policy` text — the P-CSCF ports for the resume's flow, and the
SPIs in use, which a reregistration's new SPIs must differ from (and the
state file falls back to) — and a one-mechanism Security-Server value that
stands in for the Security-Verify when the state file holds no list for the
SA pair in the kernel (`ChooseResumeSecurityVerify` decides; since 0.3.12
the 401's own list is stored there).

Pure std C++ (the shell runs the commands and captures the text). Command
shapes and the parser are pinned by tests/Ipsec.
*/

export module Imsd:Ipsec;
import std;
import :Util;

export namespace imsd::ipsec {

    // UE protected ports are fixed by the stack; P-CSCF ports come from the
    // Security-Server the network offered (fresh) or the kernel (resume).
    struct SaParams {
        std::string local;      // UE global IPv6/4 on the ims PDN
        std::string pcscf;      // P-CSCF address
        std::vector<std::uint8_t> ik;  // integrity key (auth)
        std::vector<std::uint8_t> ck;  // cipher key (enc)
        std::uint32_t spiUc = 0;  // UE client  inbound  SA (P->UE, reqid 1)
        std::uint32_t spiUs = 0;  // UE server  inbound  SA (P->UE, reqid 2)
        std::uint32_t spiPc = 0;  // P-CSCF client outbound SA (UE->P, reqid 2)
        std::uint32_t spiPs = 0;  // P-CSCF server outbound SA (UE->P, reqid 1)
        int portUc = imsd::util::PortUc;  // UE protected client port
        int portUs = imsd::util::PortUs;  // UE protected server port
        int portPs = 0;         // P-CSCF server port (UE connects its TCP here)
        int portPc = 0;         // P-CSCF client port
        std::string alg = "hmac-sha-1-96";  // hmac-sha-1-96 | hmac-md5-96
        std::string ealg = "aes-cbc";  // aes-cbc | des-ede3-cbc/3des | null
    };

    // Kernel name of a TS 33.203 ESP integrity algorithm; nullopt for one we
    // cannot install. Both take IK itself as the key: HMAC zero-pads a key
    // shorter than its block, so the IK||0x00000000 the spec writes for
    // SHA-1 hashes identically to the 128-bit IK handed to the kernel.
    std::optional<std::string> KernelAuth(std::string_view alg) {
        if (alg == "hmac-sha-1-96") return "hmac(sha1)";
        if (alg == "hmac-md5-96") return "hmac(md5)";
        return std::nullopt;
    }

    // Canonical ealg name: absent is the null cipher (RFC 3329 §2.2), "3des"
    // (imsd's EALG alias) is des-ede3-cbc. Used on both sides of the
    // offer/answer comparison.
    std::string NormalizeEalg(std::string_view ealg) {
        if (ealg.empty()) return "null";
        if (ealg == "3des") return "des-ede3-cbc";
        return std::string(ealg);
    }

    // The ESP ciphers EncArgs can key from CK (canonical names). A P-CSCF
    // selecting anything else must be refused by the caller, never
    // installed as a silent null.
    bool KnownEalg(std::string_view ealg) {
        return ealg == "null" || ealg == "aes-cbc" || ealg == "des-ede3-cbc";
    }

    namespace detail {
        // ESP cipher argv triple for `ip xfrm state add ... <enc>`, matching
        // imscall.setup_sa: aes-cbc keyed with CK; 3DES uses CK||CK[:8] to
        // reach 24 bytes; anything else is the null cipher (empty key).
        inline std::vector<std::string> EncArgs(const SaParams& p) {
            if (p.ealg == "aes-cbc") return {"enc", "cbc(aes)", std::format("0x{}", imsd::util::ToHex(p.ck))};
            if (p.ealg == "des-ede3-cbc" || p.ealg == "3des") {
                std::vector<std::uint8_t> k = p.ck;
                k.insert(k.end(), p.ck.begin(), p.ck.begin() + 8);
                return {"enc", "cbc(des3_ede)", std::format("0x{}", imsd::util::ToHex(k))};
            }
            return {"enc", "cipher_null", ""};
        }

        inline std::vector<std::string> StateAdd(std::string_view src, std::string_view dst, std::uint32_t spi, int reqid, std::string_view auth, std::string_view ikx, const std::vector<std::string>& enc) {
            std::vector<std::string> a = {
                "ip", "xfrm", "state", "add",
                "src", std::string(src), "dst", std::string(dst),
                "proto", "esp", "spi", std::to_string(spi),
                "mode", "transport", "reqid", std::to_string(reqid),
                "auth-trunc", std::string(auth), std::string(ikx), "96"};
            a.insert(a.end(), enc.begin(), enc.end());
            return a;
        }

        inline std::vector<std::string> PolicyAdd(std::string_view src, std::string_view dst, int plen, int sport, int dport, std::string_view dir, std::string_view tsrc, std::string_view tdst, int reqid) {
            return {
                "ip", "xfrm", "policy", "add",
                "src", std::format("{}/{}", src, plen),
                "dst", std::format("{}/{}", dst, plen),
                "sport", std::to_string(sport), "dport", std::to_string(dport),
                "dir", std::string(dir),
                "tmpl", "src", std::string(tsrc), "dst", std::string(tdst),
                "proto", "esp", "reqid", std::to_string(reqid),
                "mode", "transport"};
        }
    }

    // The full `ip xfrm` argv sequence to install a fresh SA pair: flush
    // state+policy, add the four ESP states, add the four transport policies.
    // Order matches imscall.setup_sa exactly. Callers validate p.alg with
    // KernelAuth() first; an unknown alg goes to the kernel by its own name
    // and is rejected there, never mapped to a default.
    std::vector<std::vector<std::string>> BuildSetupCommands(const SaParams& p) {
        const std::string& local = p.local;
        const std::string& pcscf = p.pcscf;
        std::string ikx = std::format("0x{}", imsd::util::ToHex(p.ik));
        std::string auth = KernelAuth(p.alg).value_or(p.alg);
        std::vector<std::string> enc = detail::EncArgs(p);
        int plen = imsd::util::Is6(local) ? 128 : 32;
        std::vector<std::vector<std::string>> cmds;
        cmds.push_back({"ip", "xfrm", "state", "flush"});
        cmds.push_back({"ip", "xfrm", "policy", "flush"});
        cmds.push_back(detail::StateAdd(local, pcscf, p.spiPs, 1, auth, ikx, enc));
        cmds.push_back(detail::StateAdd(pcscf, local, p.spiUc, 1, auth, ikx, enc));
        cmds.push_back(detail::StateAdd(local, pcscf, p.spiPc, 2, auth, ikx, enc));
        cmds.push_back(detail::StateAdd(pcscf, local, p.spiUs, 2, auth, ikx, enc));
        cmds.push_back(detail::PolicyAdd(local, pcscf, plen, p.portUc, p.portPs, "out", local, pcscf, 1));
        cmds.push_back(detail::PolicyAdd(pcscf, local, plen, p.portPs, p.portUc, "in",  pcscf, local, 1));
        cmds.push_back(detail::PolicyAdd(local, pcscf, plen, p.portUs, p.portPc, "out", local, pcscf, 2));
        cmds.push_back(detail::PolicyAdd(pcscf, local, plen, p.portPc, p.portUs, "in",  pcscf, local, 2));
        return cmds;
    }

    std::vector<std::vector<std::string>> BuildTeardownCommands() {
        return {{"ip", "xfrm", "state", "flush"}, {"ip", "xfrm", "policy", "flush"}};
    }

    // ---- Security-Server (RFC 3329 §2.2) -----------------------------------
    //
    // The P-CSCF answers the challenge with the mechanisms it supports, each
    // with a preference, plus the SPIs and ports it assigned to this
    // registration. The UE takes the highest-preference mechanism that is
    // in that list AND that it offered (RFC 3329 §2.3.1, TS 24.229
    // §5.1.1.5.1, TS 33.203 §7.2); imsd offers exactly one combination
    // (hmac-sha-1-96 + EALG). The SPIs/ports are per registration, not per
    // mechanism: they come from whichever entry carries them (O2 UK's
    // Mavenir core lists six entries and puts them on the one it applied).
    // imsd ≤ 0.3.5 took the first `ealg=` anywhere in the header and
    // installed 3DES against a P-CSCF expecting the null cipher: every
    // protected packet was undecryptable to it (alyx, 2026-09-17; journal/ims
    // ledger F11).

    struct SecurityMechanism {
        std::string text;            // the mechanism verbatim (trimmed)
        std::string name;            // mechanism-name, lower-case ("ipsec-3gpp")
        std::optional<int> qMilli;   // q in thousandths (q=0.94 -> 940)
        std::string alg;             // lower-case; empty when absent
        std::string ealg;            // lower-case; empty when absent (= null cipher)
        std::string prot;            // lower-case; empty when absent (= esp)
        std::string mod;             // lower-case; empty when absent (= trans)
        std::optional<std::uint32_t> spiPc;
        std::optional<std::uint32_t> spiPs;
        std::optional<int> portPc;
        std::optional<int> portPs;
    };

    struct SecurityServer {
        std::vector<SecurityMechanism> mechanisms;   // in header order
        std::size_t selected = 0;    // algorithms: the ipsec-3gpp entry the UE selects
        std::size_t carrier = 0;     // SPIs/ports: the entry carrying spi-s (== selected when it does)
        bool offerMatched = false;   // selected is what we offered; false = the fallback below
        const SecurityMechanism& Selected() const { return mechanisms[selected]; }
        const SecurityMechanism& Carrier() const { return mechanisms[carrier]; }
    };

    namespace detail {
        inline std::string_view TrimWs(std::string_view s) {
            auto ws = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
            while (!s.empty() && ws(s.front())) s.remove_prefix(1);
            while (!s.empty() && ws(s.back())) s.remove_suffix(1);
            return s;
        }

        inline std::string Lower(std::string_view s) {
            std::string out(s);
            for (char& c : out) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            return out;
        }

        // Split on `sep` outside double quotes: a d-qop="auth,auth-int"
        // value must not split the mechanism list.
        inline std::vector<std::string_view> SplitUnquoted(std::string_view s, char sep) {
            std::vector<std::string_view> out;
            bool quoted = false;
            std::size_t start = 0;
            for (std::size_t i = 0; i < s.size(); i++) {
                if (s[i] == '"') quoted = !quoted;
                else if (s[i] == sep && !quoted) { out.push_back(s.substr(start, i - start)); start = i + 1; }
            }
            out.push_back(s.substr(start));
            return out;
        }

        // RFC 3261 qvalue ("0" ["." 0*3DIGIT] / "1" ["." 0*3("0")]) in thousandths.
        inline std::optional<int> ParseQMilli(std::string_view v) {
            std::size_t dot = v.find('.');
            std::string_view whole = v.substr(0, dot);
            std::string_view frac = dot == std::string_view::npos ? std::string_view{} : v.substr(dot + 1);
            if ((whole.empty() && frac.empty()) || whole.size() > 1 || frac.size() > 3) return std::nullopt;
            int q = 0;
            for (char c : whole) { if (c < '0' || c > '9') return std::nullopt; q = (c - '0') * 1000; }
            int scale = 100;
            for (char c : frac) { if (c < '0' || c > '9') return std::nullopt; q += (c - '0') * scale; scale /= 10; }
            return q;
        }

        template <class T>
        inline std::optional<T> ParseWhole(std::string_view v) {
            if (v.empty()) return std::nullopt;
            T out{};
            auto [p, ec] = std::from_chars(v.data(), v.data() + v.size(), out);
            if (ec != std::errc{} || p != v.data() + v.size()) return std::nullopt;
            return out;
        }

        inline std::optional<int> ParsePort(std::string_view v) {
            auto p = ParseWhole<int>(v);
            if (!p || *p < 1 || *p > 65535) return std::nullopt;
            return p;
        }

        // Index of the highest-q mechanism satisfying `pred` (absent q loses
        // to any q; ties keep the earlier entry); nullopt when none does.
        template <class Pred>
        inline std::optional<std::size_t> Best(const std::vector<SecurityMechanism>& ms, Pred pred) {
            std::optional<std::size_t> b;
            for (std::size_t i = 0; i < ms.size(); i++) {
                if (!pred(ms[i])) continue;
                if (!b || ms[i].qMilli.value_or(-1) > ms[*b].qMilli.value_or(-1)) b = i;
            }
            return b;
        }
    }

    // Parse a Security-Server value and select against our offer. Names,
    // parameter names and alg/ealg/prot/mod values are tokens
    // (case-insensitive) and come back lower-case. `selected` = the
    // highest-q ipsec-3gpp mechanism whose alg (absent = the offered one)
    // and ealg (absent = null) equal what we offered; when none matches,
    // the highest-q ipsec-3gpp entry carrying spi-s, else port-s, else the
    // first ipsec-3gpp entry, with offerMatched = false so the caller can
    // say so. `carrier` = selected when it carries spi-s, else the highest-q
    // ipsec-3gpp entry that does. nullopt when the value holds no ipsec-3gpp
    // mechanism at all.
    std::optional<SecurityServer> ParseSecurityServer(std::string_view value, std::string_view offeredAlg, std::string_view offeredEalg) {
        SecurityServer ss;
        for (std::string_view part : detail::SplitUnquoted(value, ',')) {
            part = detail::TrimWs(part);
            if (part.empty()) continue;
            SecurityMechanism m;
            m.text = std::string(part);
            bool first = true;
            for (std::string_view tok : detail::SplitUnquoted(part, ';')) {
                tok = detail::TrimWs(tok);
                if (first) { m.name = detail::Lower(tok); first = false; continue; }
                if (tok.empty()) continue;
                std::size_t eq = tok.find('=');
                std::string key = detail::Lower(detail::TrimWs(tok.substr(0, eq)));
                std::string_view val = eq == std::string_view::npos ? std::string_view{} : detail::TrimWs(tok.substr(eq + 1));
                if (val.size() >= 2 && val.front() == '"' && val.back() == '"') val = val.substr(1, val.size() - 2);
                if (key == "q") m.qMilli = detail::ParseQMilli(val);
                else if (key == "alg") m.alg = detail::Lower(val);
                else if (key == "ealg") m.ealg = detail::Lower(val);
                else if (key == "prot") m.prot = detail::Lower(val);
                else if (key == "mod") m.mod = detail::Lower(val);
                else if (key == "spi-c") m.spiPc = detail::ParseWhole<std::uint32_t>(val);
                else if (key == "spi-s") m.spiPs = detail::ParseWhole<std::uint32_t>(val);
                else if (key == "port-c") m.portPc = detail::ParsePort(val);
                else if (key == "port-s") m.portPs = detail::ParsePort(val);
            }
            ss.mechanisms.push_back(std::move(m));
        }
        auto ipsec = [](const SecurityMechanism& m) { return m.name == "ipsec-3gpp"; };
        auto carries = [&](const SecurityMechanism& m) { return ipsec(m) && m.spiPs.has_value(); };
        std::string wantAlg = detail::Lower(offeredAlg);
        std::string wantEalg = NormalizeEalg(detail::Lower(offeredEalg));
        auto offered = [&](const SecurityMechanism& m) {
            return ipsec(m) && (m.alg.empty() || m.alg == wantAlg) && NormalizeEalg(m.ealg) == wantEalg;
        };
        auto firstIpsec = std::ranges::find_if(ss.mechanisms, ipsec);
        if (firstIpsec == ss.mechanisms.end()) return std::nullopt;
        if (auto i = detail::Best(ss.mechanisms, offered)) { ss.selected = *i; ss.offerMatched = true; }
        else if (auto i = detail::Best(ss.mechanisms, carries)) ss.selected = *i;
        else if (auto i = detail::Best(ss.mechanisms, [&](const SecurityMechanism& m) { return ipsec(m) && m.portPs.has_value(); })) ss.selected = *i;
        else ss.selected = static_cast<std::size_t>(firstIpsec - ss.mechanisms.begin());
        ss.carrier = ss.selected;
        if (!ss.Selected().spiPs) if (auto i = detail::Best(ss.mechanisms, carries)) ss.carrier = *i;
        return ss;
    }

    // What a warm SA in the kernel tells us — enough for a refresh re-REGISTER
    // to re-bind without re-authenticating.
    struct ExistingSa {
        std::string securityServer;  // one-mechanism Security-Server value: the resume's Security-Verify when the state file holds no list for this pair
        int portPs = 0;
        int portPc = 0;
        std::uint32_t spiUc = 0;     // our inbound client SPI (a reregistration offers a different one)
        std::uint32_t spiUs = 0;     // our inbound server SPI
        std::uint32_t spiPs = 0;
        std::uint32_t spiPc = 0;
    };

    namespace detail {
        // The whitespace-delimited token immediately after `key` (which should
        // include its trailing space, e.g. "spi "); empty if `key` is absent.
        inline std::string_view TokenAfter(std::string_view text, std::string_view key) {
            std::size_t at = text.find(key);
            if (at == std::string_view::npos) return {};
            std::size_t start = at + key.size();
            std::size_t end = text.find_first_of(" \t\r\n", start);
            return text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
        }

        // Split into blocks, each starting at a line-anchored "src " and
        // running to just before the next one.
        inline std::vector<std::string_view> SrcBlocks(std::string_view text) {
            std::vector<std::string_view> blocks;
            std::size_t i = 0;
            std::optional<std::size_t> cur;
            std::size_t pos = 0;
            while (pos <= text.size()) {
                std::size_t nl = text.find('\n', pos);
                std::string_view line = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
                if (line.starts_with("src ")) {
                    if (cur) blocks.push_back(text.substr(*cur, pos - *cur));
                    cur = pos;
                }
                if (nl == std::string_view::npos) break;
                pos = nl + 1;
            }
            if (cur) blocks.push_back(text.substr(*cur));
            (void)i;
            return blocks;
        }

        inline std::optional<std::uint32_t> ParseHex32(std::string_view tok) {
            if (tok.starts_with("0x") || tok.starts_with("0X")) tok.remove_prefix(2);
            if (tok.empty()) return std::nullopt;
            std::uint32_t v = 0;
            auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v, 16);
            if (ec != std::errc{} || p != tok.data() + tok.size()) return std::nullopt;
            return v;
        }

        inline std::optional<int> ParseDec(std::string_view tok) {
            int v = 0;
            auto [p, ec] = std::from_chars(tok.data(), tok.data() + tok.size(), v);
            if (ec != std::errc{} || p != tok.data() + tok.size()) return std::nullopt;
            return v;
        }
    }

    // Reconstruct SA facts from `ip xfrm state` + `ip xfrm policy` text.
    // Mapping (matches imscall.parse_existing_sa):
    //   state: (src==LOCAL,dst==P): reqid1->spiPs reqid2->spiPc
    //          (src==P,dst==LOCAL): reqid1->spiUc reqid2->spiUs
    //   policy: dir out, reqid1->portPs=dport, reqid2->portPc=dport
    // Returns nullopt unless spiPs, spiPc, portPs, portPc are all found.
    std::optional<ExistingSa> ParseExistingSa(std::string_view stateText, std::string_view policyText, std::string_view pcscf, std::string_view local) {
        ExistingSa sa;
        bool havePs = false;
        bool havePc = false;
        bool havePortPs = false;
        bool havePortPc = false;
        for (std::string_view b : detail::SrcBlocks(stateText)) {
            std::string_view src = detail::TokenAfter(b, "src ");
            std::string_view dst = detail::TokenAfter(b, "dst ");
            auto spi = detail::ParseHex32(detail::TokenAfter(b, "spi "));
            auto reqid = detail::ParseDec(detail::TokenAfter(b, "reqid "));
            if (!spi || !reqid) continue;
            if (src == local && dst == pcscf && *reqid == 1) { sa.spiPs = *spi; havePs = true; }
            if (src == local && dst == pcscf && *reqid == 2) { sa.spiPc = *spi; havePc = true; }
            if (src == pcscf && dst == local && *reqid == 1) sa.spiUc = *spi;
            if (src == pcscf && dst == local && *reqid == 2) sa.spiUs = *spi;
        }
        for (std::string_view b : detail::SrcBlocks(policyText)) {
            if (b.find("dir out") == std::string_view::npos) continue;
            auto dport = detail::ParseDec(detail::TokenAfter(b, "dport "));
            auto reqid = detail::ParseDec(detail::TokenAfter(b, "reqid "));
            if (!dport || !reqid) continue;
            if (*reqid == 1) { sa.portPs = *dport; havePortPs = true; }
            else if (*reqid == 2) { sa.portPc = *dport; havePortPc = true; }
        }
        if (!(havePs && havePc && havePortPs && havePortPc)) return std::nullopt;
        sa.securityServer = std::format("ipsec-3gpp; q=0.1; alg=hmac-sha-1-96; ealg=aes-cbc; " "spi-c={}; spi-s={}; port-c={}; port-s={}", sa.spiPc, sa.spiPs, sa.portPc, sa.portPs);
        return sa;
    }

    // The Security-Verify a resume sends, and where it came from: the state
    // file's list when the entry carrying its SPIs names the SA pair the
    // kernel holds (a file an attempt the process did not live to record
    // left behind names another pair), else the one-mechanism entry rebuilt
    // from the SA — for an empty or unparseable value too.
    struct ResumeSecurityVerify {
        enum class Source { Stored, NoneStored, OtherSa };
        Source source = Source::NoneStored;
        std::string value;
        std::size_t mechanisms = 0;                // of the stored list, when it is used
        std::optional<std::uint32_t> storedSpiPs;  // the stored list's spi-s (OtherSa); nullopt = none parsed
    };
    ResumeSecurityVerify ChooseResumeSecurityVerify(std::string_view stored, const ExistingSa& sa, std::string_view offeredEalg) {
        ResumeSecurityVerify r;
        r.value = sa.securityServer;
        if (stored.empty()) return r;
        r.source = ResumeSecurityVerify::Source::OtherSa;
        if (auto parsed = ParseSecurityServer(stored, "hmac-sha-1-96", offeredEalg)) {
            const SecurityMechanism& c = parsed->Carrier();
            r.storedSpiPs = c.spiPs;
            if (c.spiPc == sa.spiPc && c.spiPs == sa.spiPs && c.portPc == sa.portPc && c.portPs == sa.portPs) {
                r.source = ResumeSecurityVerify::Source::Stored;
                r.value = std::string(stored);
                r.mechanisms = parsed->mechanisms.size();
            }
        }
        return r;
    }

    // Every local address holding an SA to the P-CSCF, once each, in kernel
    // order: where a registration made on another address of the link (the
    // kernel's temporary one, before 0.3.10 — ims ledger F18) still lives.
    std::vector<std::string> SaLocals(std::string_view stateText, std::string_view pcscf) {
        std::vector<std::string> locals;
        for (std::string_view b : detail::SrcBlocks(stateText)) {
            std::string_view src = detail::TokenAfter(b, "src ");
            if (src.empty() || detail::TokenAfter(b, "dst ") != pcscf) continue;
            if (std::ranges::find(locals, src) == locals.end()) locals.emplace_back(src);
        }
        return locals;
    }
}
