// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts®

// lint-disable-file fixed-width-types
// Imsd:Aka unit tests — the AKAv1-MD5 digest against a vector cross-computed
// with the Python reference, identity assembly, the qmicli/mmcli text
// parsers (fed real device output), and the AUTHENTICATE APDU build/parse.
import std;
import Imsd;

using namespace imsd::aka;

namespace {
    int Failures = 0;
    void Check(bool cond, std::string_view msg) {
        if (!cond) {
            std::println(std::cerr, "FAIL: {}", msg);
            ++Failures;
        }
    }

    // `qmicli --uim-get-card-status` in the shape a real FP6 eSIM (slot 2)
    // produces.
    constexpr std::string_view CardStatus =
        "[qrtr://0] Successfully got card status\n"
        "Provisioning applications:\n"
        "\tPrimary GW:   slot '2', application '1'\n"
        "Slot [1]:\n"
        "\tCard state: 'error: no-atr-received (3)'\n"
        "\tUPIN state: 'not-initialized'\n"
        "Slot [2]:\n"
        "\tCard state: 'present'\n"
        "\tApplication [1]:\n"
        "\t\tApplication type:  'usim (2)'\n"
        "\t\tApplication state: 'ready'\n"
        "\t\tApplication ID:\n"
        "\t\t\tA0:00:00:00:87:10:02:FF:31:F0:04:89:0E:00:00:01\n"
        "\t\tPIN1 state: 'disabled'\n";

    constexpr std::string_view MmcliSim =
        "  Properties |            active: yes\n"
        "             |              imsi: 001010123456789\n"
        "             |             iccid: 8900101234567890123\n"
        "             |       operator id: 00101\n"
        "             |     operator name: Test PLMN 1-1\n";
}

int main() {
    // ---- AKAv1-MD5 digest: pinned vector (the algorithm was cross-checked
    // against the Python reference; this vector is recomputed for the
    // synthetic test identity)
    {
        std::array<std::uint8_t, 8> res{0xa1,0xb2,0xc3,0xd4,0xe5,0xf6,0x07,0x18};
        std::string d = DigestAkav1("abc123nonce==", res, "cnonce0123456789", "sip:ims.mnc001.mcc001.3gppnetwork.org", "001010123456789@ims.mnc001.mcc001.3gppnetwork.org", "ims.mnc001.mcc001.3gppnetwork.org");
        Check(d == "09fd8f8f7ef716c029666a7c6df1db1e", "AKAv1-MD5 digest matches pinned vector");
    }

    // ---- identity assembly
    {
        Identity id = MakeIdentity("001010123456789", "001", "01");
        Check(id.mnc == "001", "MNC zero-padded to 3");
        Check(id.domain == "ims.mnc001.mcc001.3gppnetwork.org", "domain");
        Check(id.impi == "001010123456789@ims.mnc001.mcc001.3gppnetwork.org", "IMPI");
        Check(id.impu == "sip:001010123456789@ims.mnc001.mcc001.3gppnetwork.org", "IMPU");
        Check(id.regUri == "sip:ims.mnc001.mcc001.3gppnetwork.org", "reg URI");
    }

    // ---- card-status parse (real device output)
    {
        auto sel = ParseCardStatus(CardStatus);
        Check(sel.has_value(), "card status yields a ready USIM");
        Check(sel && sel->slot == 2, "slot 2 selected (slot 1 is error/no-atr)");
        Check(sel && sel->aid == "A0000000871002FF31F004890E000001", "AID uppercased, colons stripped");
    }
    Check(!ParseCardStatus("Slot [1]:\n\tCard state: 'absent'\n").has_value(), "no ready USIM -> nullopt");

    // ---- mmcli parsers
    Check(ParsePrimarySimPath("  SIM      |       primary sim path: /org/freedesktop/ModemManager1/SIM/0\n") == "/org/freedesktop/ModemManager1/SIM/0", "primary sim path");
    {
        auto si = ParseSimInfo(MmcliSim);
        Check(si.has_value(), "sim info parses");
        Check(si && si->imsi == "001010123456789", "imsi");
        Check(si && si->mcc == "001" && si->mnc == "01", "mcc/mnc split");
        // full pipeline: parsed info -> identity
        Identity id = MakeIdentity(si->imsi, si->mcc, si->mnc);
        Check(id.domain == "ims.mnc001.mcc001.3gppnetwork.org", "info->identity domain");
    }
    Check(ParseEquipmentId("  Hardware |          equipment id: 359999990000001\n") == "359999990000001", "equipment id");
    Check(ImeiUrn("359999990000001") == "urn:gsma:imei:35999999-000000-1", "IMEI -> RFC 7254 URN");
    Check(!ImeiUrn("35999999000000").has_value(), "14-digit IMEI rejected");
    Check(!ImeiUrn("35999999000000x").has_value(), "non-digit IMEI rejected");

    // ---- AUTHENTICATE APDU build + response parse
    {
        std::vector<std::uint8_t> rnd(16, 0x11), autn(16, 0x22);
        std::string apdu = BuildAkaApdu(0x01, rnd, autn);
        // CLA=01 INS=88 P1=00 P2=81 Lc=22(34) 10 <rand> 10 <autn>
        Check(apdu.starts_with("0188008122"), "APDU header CLA/INS/P1/P2/Lc");
        Check(apdu == "0188008122" "10" "11111111111111111111111111111111" "10" "22222222222222222222222222222222", "APDU byte-exact");
        Check(BuildGetResponseApdu(0x01, 0x2E) == "01c000002e", "GET RESPONSE APDU");
    }
    {
        // DB | 08 RES | 10 CK | 10 IK | SW1 SW2(9000)
        std::vector<std::uint8_t> body = {0xDB, 0x08};
        for (int i = 0; i < 8; i++) body.push_back(0xA0 + i);   // RES
        body.push_back(0x10);
        for (int i = 0; i < 16; i++) body.push_back(0xC0 + i);  // CK
        body.push_back(0x10);
        for (int i = 0; i < 16; i++) body.push_back(0xD0 + i);  // IK
        body.push_back(0x90); body.push_back(0x00);             // SW
        auto r = ParseAkaResponse(body);
        Check(r.has_value(), "AKA response parses");
        Check(r && r->res.size() == 8 && r->res[0] == 0xA0, "RES extracted");
        Check(r && r->ck.size() == 16 && r->ck[0] == 0xC0, "CK extracted");
        Check(r && r->ik.size() == 16 && r->ik[0] == 0xD0, "IK extracted");
    }
    {
        // sync-failure tag (0xDC) or truncated body -> nullopt
        std::vector<std::uint8_t> dc = {0xDC, 0x0E, 0x90, 0x00};
        Check(!ParseAkaResponse(dc).has_value(), "non-DB tag rejected");
        std::vector<std::uint8_t> tr = {0xDB, 0x08, 0x00, 0x90, 0x00};
        Check(!ParseAkaResponse(tr).has_value(), "length past end rejected");
    }
    {
        // P-Access-Network-Info from `mmcli --location-get` (real-device
        // output shape; TAC/CI zero-padded wider than the 4/7 hex digits
        // PANI uses). The mcc+mnc+TAC+ECI concatenation was validated
        // against a stock modem's on-wire PANI; values here are synthetic.
        constexpr std::string_view Loc =
            "  --------------------------\n"
            "  3GPP |       operator mcc: 001\n"
            "       |       operator mnc: 01\n"
            "       | location area code: 0000\n"
            "       | tracking area code: 001234\n"
            "       |            cell id: 00ABCDE0\n";
        auto p = ParsePani(Loc);
        Check(p.has_value(), "PANI parses");
        Check(p == "3GPP-E-UTRAN-FDD;utran-cell-id-3gpp=0010112340ABCDE0", "PANI concatenation mcc+mnc+TAC+ECI");
        Check(!ParsePani("no location here").has_value(), "PANI absent fields rejected");
        constexpr std::string_view Zeros =
            "  3GPP |       operator mcc: 001\n"
            "       |       operator mnc: 01\n"
            "       | tracking area code: 000000\n"
            "       |            cell id: 00000000\n";
        Check(!ParsePani(Zeros).has_value(), "PANI all-zero cell rejected");
    }

    if (Failures == 0) std::println("Aka: all tests passed");
    return Failures;
}
