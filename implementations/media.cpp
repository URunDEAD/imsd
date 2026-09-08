// SPDX-License-Identifier: GPL-3.0-only
// SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts®

// lint-disable-file fixed-width-types no-char-pointer
/*
imsd-media — the RTP media leg for a userspace VoLTE call, spawned as its
own process by the daemon (imsd) exactly as the Python prototype spawned
rtpcap.py: one media leg per call, argv-configured, torn down on SIGTERM or
when the downlink dries up. Keeping it a separate process preserves the
far-end-hangup contract (exit code 3 = downlink RTP stopped, which on carriers
whose network BYE never reaches our SAs is the reliable teardown trigger) and
isolates a media crash from the control-plane daemon.

Codecs (CODEC env, set by the daemon from the negotiated SDP): AMR-WB
(16 kHz; the mobile-to-mobile VoLTE codec, the default), AMR narrowband and
G.711 PCMA/PCMU (8 kHz; what a PSTN gateway offers when a landline calls).
AMR frames ride RFC 4867 payloads, octet-aligned or bandwidth-efficient
(OCTET_ALIGN, mirrored from the SDP); G.711 is raw samples. The AMR codecs
are dlopen'd (libvo-amrwbenc + libopencore-amrwb for WB, libopencore-amrnb
for NB) so the binary has no link-time dependency on them; G.711 is a table.

Binds the advertised local RTP port, sends uplink frames toward the media
gateway, captures the downlink, and — with PLAY=1 — reconstructs the media
clock from RTP timestamps so pw-play stays real-time-paced through far-end
DTX silence (every missing 20 ms slot is decoded as a NO_DATA frame for
CNG/PLC; zeros for G.711). MIC=1 feeds live pw-record audio through the
encoder.

Usage: imsd-media <local-ip> <rtp-port> <remote-ip> <remote-port> <pt> <secs> <out-base>
       imsd-media --selftest   (payload pack/depay roundtrip, both formats,
                                both AMR codecs; G.711 table roundtrip)
Env: CODEC MIC PLAY GAIN PLAY_GAIN AMR_MODE DTX MEDIA_TIMEOUT RTP_DUMP
OCTET_ALIGN AUDIO_USER MIC_SRC PCM_DUMP
(OCTET_ALIGN=0 selects RFC 4867 bandwidth-efficient payloads both ways; the
daemon sets it from the negotiated SDP — 1 is the default/legacy behavior.
AMR_MODE is the encoder mode of whichever AMR codec is active; default 2
for AMR-WB (12.65 kbit/s), 7 for AMR (12.2 kbit/s). MIC_SRC=<file> feeds
raw s16 PCM at the codec rate through the encoder instead of pw-record,
paced in real time; PCM_DUMP=1 writes the decoded downlink to <out-base>.pcm
— both are test seams for driving the leg against a synthetic RTP peer
with no PipeWire and no network.)
*/

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pwd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

import std;

namespace {

using Clock = std::chrono::steady_clock;
double Now() {
    return std::chrono::duration<double>(Clock::now().time_since_epoch()).count();
}

enum class Codec { AmrWb, AmrNb, Pcma, Pcmu };

Codec ParseCodec(std::string_view name) {
    if (name == "AMR") return Codec::AmrNb;
    if (name == "PCMA") return Codec::Pcma;
    if (name == "PCMU") return Codec::Pcmu;
    return Codec::AmrWb;
}
std::string_view CodecName(Codec c) {
    switch (c) {
        case Codec::AmrWb: return "AMR-WB";
        case Codec::AmrNb: return "AMR";
        case Codec::Pcma: return "PCMA";
        case Codec::Pcmu: return "PCMU";
    }
    return "AMR-WB";
}
bool IsAmr(Codec c) { return c == Codec::AmrWb || c == Codec::AmrNb; }
// Sample rate = RTP clock rate; one 20 ms frame is 320 or 160 samples.
int CodecRate(Codec c) { return c == Codec::AmrWb ? 16000 : 8000; }
int FrameSamples(Codec c) { return CodecRate(c) / 50; }

// AMR speech bits per frame type — the exact payload bit counts of the
// bandwidth-efficient format. AMR-WB: RFC 4867 table 2 / TS 26.201; AMR:
// RFC 4867 table 1 / TS 26.101 (FT 8 = SID, 9-11 = the other systems' SID
// frames a gateway may forward). FT 14/15 (SPEECH_LOST/NO_DATA) carry 0
// bits; unknown FTs return -1 so a corrupt ToC aborts the packet instead of
// shifting every later bit.
constexpr int AmrBits(Codec c, int ft) {
    if (c == Codec::AmrWb) {
        switch (ft) {
            case 0: return 132; case 1: return 177; case 2: return 253; case 3: return 285;
            case 4: return 317; case 5: return 365; case 6: return 397; case 7: return 461;
            case 8: return 477; case 9: return 40; case 14: return 0; case 15: return 0;
            default: return -1;
        }
    }
    switch (ft) {
        case 0: return 95; case 1: return 103; case 2: return 118; case 3: return 134;
        case 4: return 148; case 5: return 159; case 6: return 204; case 7: return 244;
        case 8: return 39; case 9: return 43; case 10: return 38; case 11: return 37;
        case 15: return 0;
        default: return -1;
    }
}

// octet-aligned speech-frame byte size by frame type: the bits rounded up
// (AMR-WB: 17 23 32 36 40 46 50 58 60, SID 5; AMR: 12 13 15 17 19 20 26 31,
// SID 5). 0 for frame types that carry no speech.
constexpr int AmrBytes(Codec c, int ft) {
    int bits = AmrBits(c, ft);
    return bits <= 0 ? 0 : (bits + 7) / 8;
}

// ---- G.711 (ITU-T, the classic Sun g711.c formulation) --------------------
// 16-bit linear <-> 8-bit companded. Byte 0xD5 (A-law) / 0xFF (mu-law) is
// digital zero. Idempotent: encoding a decoded sample gives the same byte.

constexpr int UlawBias = 0x84;
constexpr int UlawClip = 8159;

int SegmentOf(int val, std::span<const int> ends) {
    for (std::size_t i = 0; i < ends.size(); i++)
        if (val <= ends[i]) return static_cast<int>(i);
    return static_cast<int>(ends.size());
}

std::uint8_t LinearToAlaw(std::int16_t pcm) {
    static constexpr std::array<int, 8> ends = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    int val = pcm >> 3;  // 13-bit magnitude space
    int mask = 0xD5;
    if (val < 0) {
        mask = 0x55;
        val = -val - 1;
    }
    int seg = SegmentOf(val, ends);
    if (seg >= 8) return static_cast<std::uint8_t>(0x7F ^ mask);
    int aval = seg << 4;
    aval |= seg < 2 ? (val >> 1) & 0x0F : (val >> seg) & 0x0F;
    return static_cast<std::uint8_t>(aval ^ mask);
}

std::int16_t AlawToLinear(std::uint8_t a) {
    a ^= 0x55;
    int t = (a & 0x0F) << 4;
    int seg = (a & 0x70) >> 4;
    if (seg == 0) t += 8;
    else if (seg == 1) t += 0x108;
    else t = (t + 0x108) << (seg - 1);
    return static_cast<std::int16_t>((a & 0x80) ? t : -t);
}

std::uint8_t LinearToUlaw(std::int16_t pcm) {
    static constexpr std::array<int, 8> ends = {0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF, 0x1FFF};
    int val = pcm >> 2;  // 14-bit magnitude space
    int mask = 0xFF;
    if (val < 0) {
        val = -val;
        mask = 0x7F;
    }
    if (val > UlawClip) val = UlawClip;
    val += UlawBias >> 2;
    int seg = SegmentOf(val, ends);
    if (seg >= 8) return static_cast<std::uint8_t>(0x7F ^ mask);
    int uval = (seg << 4) | ((val >> (seg + 1)) & 0x0F);
    return static_cast<std::uint8_t>(uval ^ mask);
}

std::int16_t UlawToLinear(std::uint8_t u) {
    u = static_cast<std::uint8_t>(~u);
    int t = ((u & 0x0F) << 3) + UlawBias;
    t <<= (u & 0x70) >> 4;
    return static_cast<std::int16_t>((u & 0x80) ? (UlawBias - t) : (t - UlawBias));
}

std::uint8_t G711Encode(Codec c, std::int16_t pcm) { return c == Codec::Pcma ? LinearToAlaw(pcm) : LinearToUlaw(pcm); }
std::int16_t G711Decode(Codec c, std::uint8_t b) { return c == Codec::Pcma ? AlawToLinear(b) : UlawToLinear(b); }
std::uint8_t G711Zero(Codec c) { return c == Codec::Pcma ? 0xD5 : 0xFF; }

std::string EnvOr(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(d);
}
bool EnvBool(const char* k, bool d) {
    const char* v = std::getenv(k);
    return v ? std::string_view(v) == "1" : d;
}

bool Is6(std::string_view a) { return a.contains(':'); }

// Fill a sockaddr_storage from an IP literal + port; returns its length.
socklen_t MakeAddr(std::string_view ip, int port, sockaddr_storage& ss) {
    std::memset(&ss, 0, sizeof ss);
    std::string s(ip);
    if (Is6(ip)) {
        auto* a = reinterpret_cast<sockaddr_in6*>(&ss);
        a->sin6_family = AF_INET6;
        a->sin6_port = htons(static_cast<uint16_t>(port));
        inet_pton(AF_INET6, s.c_str(), &a->sin6_addr);
        return sizeof(sockaddr_in6);
    }
    auto* a = reinterpret_cast<sockaddr_in*>(&ss);
    a->sin_family = AF_INET;
    a->sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, s.c_str(), &a->sin_addr);
    return sizeof(sockaddr_in);
}

// ---- AMR codecs via dlopen ------------------------------------------------
// AMR-WB: vo-amrwbenc E_IF_* (encode), opencore-amrwb D_IF_* (decode).
// AMR:    opencore-amrnb Encoder_Interface_* / Decoder_Interface_*.
// Both families speak the RFC 4867 §5.3 storage format: [header byte][speech],
// header = (FT << 3) | (Q << 2) — exactly the octet-aligned ToC byte with F=0.

class Encoder {
public:
    bool Open(Codec c, int dtx) {
        codec_ = c;
        if (c == Codec::AmrWb) {
            lib_ = dlopen("libvo-amrwbenc.so.0", RTLD_NOW);
            if (!lib_) return false;
            wbInit_ = reinterpret_cast<WbInitFn>(dlsym(lib_, "E_IF_init"));
            wbEnc_ = reinterpret_cast<WbEncFn>(dlsym(lib_, "E_IF_encode"));
            exit_ = reinterpret_cast<ExitFn>(dlsym(lib_, "E_IF_exit"));
            if (!wbInit_ || !wbEnc_ || !exit_) return false;
            st_ = wbInit_();
            return st_ != nullptr;
        }
        lib_ = dlopen("libopencore-amrnb.so.0", RTLD_NOW);
        if (!lib_) return false;
        nbInit_ = reinterpret_cast<NbInitFn>(dlsym(lib_, "Encoder_Interface_init"));
        nbEnc_ = reinterpret_cast<NbEncFn>(dlsym(lib_, "Encoder_Interface_Encode"));
        exit_ = reinterpret_cast<ExitFn>(dlsym(lib_, "Encoder_Interface_exit"));
        if (!nbInit_ || !nbEnc_ || !exit_) return false;
        st_ = nbInit_(dtx);  // NB: DTX is an init-time choice
        return st_ != nullptr;
    }
    // one 20 ms frame of s16 samples -> one storage frame (header byte + speech).
    std::vector<std::uint8_t> Encode(std::int16_t* samples, int mode, int dtx) {
        std::uint8_t out[128];
        int n = codec_ == Codec::AmrWb
            ? wbEnc_(st_, static_cast<std::int16_t>(mode), samples, out, static_cast<std::int16_t>(dtx))
            : nbEnc_(st_, mode, samples, out, 0);
        if (n <= 0) return {};
        return std::vector<std::uint8_t>(out, out + n);
    }
    ~Encoder() {
        if (st_ && exit_) exit_(st_);
        if (lib_) dlclose(lib_);
    }
private:
    using WbInitFn = void* (*)();
    using WbEncFn = int (*)(void*, std::int16_t, std::int16_t*, std::uint8_t*, std::int16_t);
    using NbInitFn = void* (*)(int);
    using NbEncFn = int (*)(void*, int, const std::int16_t*, std::uint8_t*, int);
    using ExitFn = void (*)(void*);
    Codec codec_ = Codec::AmrWb;
    void* lib_ = nullptr;
    void* st_ = nullptr;
    WbInitFn wbInit_ = nullptr;
    WbEncFn wbEnc_ = nullptr;
    NbInitFn nbInit_ = nullptr;
    NbEncFn nbEnc_ = nullptr;
    ExitFn exit_ = nullptr;
};

class Decoder {
public:
    bool Open(Codec c) {
        codec_ = c;
        bool wb = c == Codec::AmrWb;
        lib_ = dlopen(wb ? "libopencore-amrwb.so.0" : "libopencore-amrnb.so.0", RTLD_NOW);
        if (!lib_) return false;
        init_ = reinterpret_cast<InitFn>(dlsym(lib_, wb ? "D_IF_init" : "Decoder_Interface_init"));
        dec_ = reinterpret_cast<DecFn>(dlsym(lib_, wb ? "D_IF_decode" : "Decoder_Interface_Decode"));
        exit_ = reinterpret_cast<ExitFn>(dlsym(lib_, wb ? "D_IF_exit" : "Decoder_Interface_exit"));
        if (!init_ || !dec_ || !exit_) return false;
        st_ = init_();
        return st_ != nullptr;
    }
    // one storage frame ([header][speech]) -> one 20 ms frame of s16 PCM.
    std::vector<std::int16_t> Decode(const std::uint8_t* frame, int len) {
        std::vector<std::int16_t> out(static_cast<std::size_t>(FrameSamples(codec_)), 0);
        std::vector<std::uint8_t> in(frame, frame + len);
        dec_(st_, in.data(), out.data(), 0);
        return out;
    }
    ~Decoder() {
        if (st_ && exit_) exit_(st_);
        if (lib_) dlclose(lib_);
    }
private:
    using InitFn = void* (*)();
    using DecFn = void (*)(void*, const std::uint8_t*, std::int16_t*, int);
    using ExitFn = void (*)(void*);
    Codec codec_ = Codec::AmrWb;
    void* lib_ = nullptr;
    void* st_ = nullptr;
    InitFn init_ = nullptr;
    DecFn dec_ = nullptr;
    ExitFn exit_ = nullptr;
};

// MSB-first bit cursor over an RTP payload (bandwidth-efficient AMR-WB is
// bit-packed: 4-bit CMR, 6-bit ToC entries, then the speech bits back to
// back with only the final octet padded — RFC 4867 §4.3).
struct BitReader {
    std::span<const std::uint8_t> d;
    std::size_t pos = 0;
    bool Ok(std::size_t n) const { return pos + n <= d.size() * 8; }
    std::uint32_t Take(int n) {
        std::uint32_t v = 0;
        for (int i = 0; i < n; i++, pos++)
            v = (v << 1) | ((d[pos >> 3] >> (7 - (pos & 7))) & 1);
        return v;
    }
};

// bandwidth-efficient AMR de-payload. Same contract as DepayOctet:
// [(storage-header-byte, speech-bytes)...], speech re-aligned to octets.
std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>>
DepayBe(Codec c, std::span<const std::uint8_t> pl) {
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> out;
    BitReader br{pl};
    if (!br.Ok(4)) return out;
    br.Take(4);  // CMR
    struct Toc { int ft; int q; };
    std::vector<Toc> tocs;
    for (;;) {
        if (!br.Ok(6)) return out;
        int f = static_cast<int>(br.Take(1));
        int ft = static_cast<int>(br.Take(4));
        int q = static_cast<int>(br.Take(1));
        tocs.push_back({ft, q});
        if (!f) break;
    }
    for (auto [ft, q] : tocs) {
        int bits = AmrBits(c, ft);
        if (bits < 0 || !br.Ok(static_cast<std::size_t>(bits))) break;
        std::vector<std::uint8_t> speech(AmrBytes(c, ft), 0);
        for (int i = 0; i < bits; i++)
            if (br.Take(1)) speech[i >> 3] |= 0x80 >> (i & 7);
        out.emplace_back(static_cast<std::uint8_t>((ft << 3) | (q ? 0x04 : 0)), std::move(speech));
    }
    return out;
}

// storage-format frame (header byte + octet-aligned speech) -> RTP payload.
// Octet-aligned: CMR byte + the frame verbatim (the storage header doubles
// as a ToC byte with F=0). Bandwidth-efficient: 10 header bits + exactly
// AmrBits(ft) speech bits, final octet zero-padded.
std::vector<std::uint8_t> PayloadFromFrame(Codec c, std::span<const std::uint8_t> frame, bool octetAlign) {
    if (octetAlign) {
        std::vector<std::uint8_t> pl = {0xF0};
        pl.insert(pl.end(), frame.begin(), frame.end());
        return pl;
    }
    int ft = (frame[0] >> 3) & 0x0F;
    int q = (frame[0] >> 2) & 1;
    int bits = AmrBits(c, ft);
    if (bits < 0) bits = 0;
    std::vector<std::uint8_t> pl((10 + bits + 7) / 8, 0);
    auto put = [&](int pos, int n, std::uint32_t v) {
        for (int i = 0; i < n; i++)
            if ((v >> (n - 1 - i)) & 1) pl[(pos + i) >> 3] |= 0x80 >> ((pos + i) & 7);
    };
    put(0, 4, 15);  // CMR: no mode request
    put(4, 1, 0);   // F: single frame
    put(5, 4, static_cast<std::uint32_t>(ft));
    put(9, 1, static_cast<std::uint32_t>(q));
    for (int i = 0; i < bits; i++)
        if (frame[1 + (i >> 3)] & (0x80 >> (i & 7))) pl[(10 + i) >> 3] |= 0x80 >> ((10 + i) & 7);
    return pl;
}

// octet-aligned AMR de-payload: skip CMR, read ToC bytes until F=0, then
// the speech runs. Returns [(storage-header-byte, speech-bytes)...].
std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>>
DepayOctet(Codec c, std::span<const std::uint8_t> pl) {
    std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>> out;
    if (pl.empty()) return out;
    std::size_t i = 1;  // skip CMR
    std::vector<std::uint8_t> tocs;
    while (i < pl.size()) {
        std::uint8_t toc = pl[i++];
        tocs.push_back(toc);
        if (!(toc & 0x80)) break;
    }
    for (std::uint8_t toc : tocs) {
        int ft = (toc >> 3) & 0x0F;
        int n = AmrBytes(c, ft);
        std::vector<std::uint8_t> speech;
        if (n > 0 && i + n <= pl.size()) speech.assign(pl.begin() + i, pl.begin() + i + n);
        out.emplace_back(static_cast<std::uint8_t>(toc & 0x7C), std::move(speech));
        i += n;
    }
    return out;
}

std::size_t RtpPayloadOffset(std::span<const std::uint8_t> pkt) {
    int cc = pkt[0] & 0x0F;
    int ext = (pkt[0] >> 4) & 1;
    std::size_t off = 12 + cc * 4;
    if (ext && off + 4 <= pkt.size()) {
        std::uint16_t extlen = (pkt[off + 2] << 8) | pkt[off + 3];
        off += 4 + extlen * 4;
    }
    return off;
}

// The desktop user's uid, resolved ONCE and thread-safely. getpwnam() hands
// out one static buffer, and the mic and playout threads spawn their pw
// helpers at the same instant — the race sent both into /run/user/0, where
// no PipeWire session lives, and a call was answered into static (dev phone,
// 2026-09-08 20:01: tx_mic=0 rx_played=0 qdrop=883). Logged per call so a
// journal shows which session the helpers were aimed at.
uid_t AudioUid() {
    static std::once_flag once;
    static uid_t uid = 0;
    std::call_once(once, [] {
        std::string user = EnvOr("AUDIO_USER", "user");
        passwd pw{};
        passwd* res = nullptr;
        char buf[4096];
        if (getpwnam_r(user.c_str(), &pw, buf, sizeof buf, &res) == 0 && res) uid = res->pw_uid;
        else
            std::println(std::cerr, "imsd-media: AUDIO_USER '{}' not found; no PipeWire session for uid 0", user);
        std::println(std::cerr, "imsd-media: audio session: {} -> /run/user/{}", user, uid);
    });
    return uid;
}

// Spawn pw-record/pw-play in the desktop user's PipeWire session (sudo -u
// when run as root). AUDIO_USER names the session owner; the default "user"
// is postmarketOS's standard account. `toChild` true = we write the child's
// stdin (pw-play); false = we read its stdout (pw-record). Returns {pid, fd}.
struct Child { pid_t pid = -1; int fd = -1; };
Child SpawnPw(bool play, bool toChild, int rate) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {};
    std::vector<std::string> argv;
    if (geteuid() == 0) {
        std::string user = EnvOr("AUDIO_USER", "user");
        argv = {"sudo", "-u", user, "env",
                std::format("XDG_RUNTIME_DIR=/run/user/{}", AudioUid())};
    }
    const char* tool = play ? "pw-play" : "pw-record";
    const char* lat = play ? "40ms" : "20ms";
    std::string rateStr = std::to_string(rate);
    for (const char* a : {tool, "--raw", "--rate", rateStr.c_str(), "--channels", "1", "--format", "s16", "--latency", lat, "-"})
        argv.emplace_back(a);
    pid_t pid = fork();
    if (pid == 0) {
        if (toChild) { dup2(pipefd[0], STDIN_FILENO); }
        else { dup2(pipefd[1], STDOUT_FILENO); }
        close(pipefd[0]); close(pipefd[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); close(devnull); }
        std::vector<char*> cargv;
        for (auto& s : argv) cargv.push_back(const_cast<char*>(s.c_str()));
        cargv.push_back(nullptr);
        execvp(cargv[0], cargv.data());
        _exit(127);
    }
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return {}; }
    if (toChild) { close(pipefd[0]); return {pid, pipefd[1]}; }
    close(pipefd[1]);
    return {pid, pipefd[0]};
}

bool ReadExact(int fd, std::uint8_t* buf, std::size_t n) {
    std::size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf + got, n - got);
        if (r <= 0) return false;
        got += static_cast<std::size_t>(r);
    }
    return true;
}

// ---- shared RTP tx state (main + mic threads both send) -------------------
struct TxState {
    std::mutex lock;
    int pt = 0;
    std::uint32_t tsStep = 320;  // samples per 20 ms frame at the codec's clock
    std::uint32_t ssrc = 0x5EED1234;
    std::uint32_t seq = 1000;
    std::uint32_t ts = 160000;
    sockaddr_storage dst{}; socklen_t dstLen = 0;
    sockaddr_storage latched{}; socklen_t latchedLen = 0;
    std::uint64_t tx = 0;
    std::uint64_t txMic = 0;
    std::atomic<bool> micOn{false};
};

void RtpSend(int sock, TxState& st, std::span<const std::uint8_t> payload) {
    std::uint8_t hdr[12];
    sockaddr_storage a1, a2; socklen_t l1, l2;
    {
        std::scoped_lock g(st.lock);
        hdr[0] = 0x80;
        hdr[1] = static_cast<std::uint8_t>(st.pt & 0x7F);
        hdr[2] = (st.seq >> 8) & 0xFF; hdr[3] = st.seq & 0xFF;
        hdr[4] = (st.ts >> 24) & 0xFF; hdr[5] = (st.ts >> 16) & 0xFF;
        hdr[6] = (st.ts >> 8) & 0xFF;  hdr[7] = st.ts & 0xFF;
        hdr[8] = (st.ssrc >> 24) & 0xFF; hdr[9] = (st.ssrc >> 16) & 0xFF;
        hdr[10] = (st.ssrc >> 8) & 0xFF; hdr[11] = st.ssrc & 0xFF;
        st.seq = (st.seq + 1) & 0xFFFF;
        st.ts = (st.ts + st.tsStep) & 0xFFFFFFFF;
        a1 = st.dst; l1 = st.dstLen; a2 = st.latched; l2 = st.latchedLen;
    }
    std::vector<std::uint8_t> pkt(hdr, hdr + 12);
    pkt.insert(pkt.end(), payload.begin(), payload.end());
    auto sendTo = [&](sockaddr_storage& a, socklen_t l) {
        if (l && sendto(sock, pkt.data(), pkt.size(), 0, reinterpret_cast<sockaddr*>(&a), l) >= 0) {
            std::scoped_lock g(st.lock);
            st.tx++;
        }
    };
    sendTo(a1, l1);
    // avoid double-send when latched == dst
    if (l2 && (l2 != l1 || std::memcmp(&a1, &a2, l1) != 0)) sendTo(a2, l2);
}

std::vector<std::pair<std::uint8_t, std::vector<std::uint8_t>>>
Depay(Codec c, std::span<const std::uint8_t> pl, bool octetAlign) {
    return octetAlign ? DepayOctet(c, pl) : DepayBe(c, pl);
}

// What goes out every 20 ms while the mic is not feeding frames: for AMR a
// storage frame ToC(F=0,FT,Q=1) + zeroed speech, payloaded per mode; for
// G.711 one frame of digital zero.
std::vector<std::uint8_t> SilenceFrame(Codec c, int ft, bool octetAlign) {
    if (!IsAmr(c)) return std::vector<std::uint8_t>(static_cast<std::size_t>(FrameSamples(c)), G711Zero(c));
    std::vector<std::uint8_t> f = {static_cast<std::uint8_t>((ft << 3) | 0x04)};
    f.resize(1 + static_cast<std::size_t>(AmrBytes(c, ft)), 0);
    return PayloadFromFrame(c, f, octetAlign);
}

// G.711 uplink: one 20 ms frame of samples -> one payload of companded bytes.
std::vector<std::uint8_t> G711Payload(Codec c, std::span<const std::int16_t> pcm) {
    std::vector<std::uint8_t> pl(pcm.size());
    for (std::size_t i = 0; i < pcm.size(); i++)
        pl[i] = G711Encode(c, pcm[i]);
    return pl;
}

// G.711 downlink: one frame's worth of companded bytes -> PCM.
std::vector<std::int16_t> G711DecodeFrame(Codec c, std::span<const std::uint8_t> bytes) {
    std::vector<std::int16_t> pcm(bytes.size());
    for (std::size_t i = 0; i < bytes.size(); i++)
        pcm[i] = G711Decode(c, bytes[i]);
    return pcm;
}

std::atomic<bool> Quit{false};
void OnSig(int) { Quit.store(true); }

// playout queue item
struct PktItem { std::uint32_t ts; std::vector<std::uint8_t> payload; };

// --selftest: pack->depay roundtrip of every frame type, both payload
// formats, both AMR codecs; G.711 digital zero, idempotence over the whole
// 16-bit range, and a 1 kHz sine surviving with the codec's nominal SNR.
int SelfTest() {
    for (Codec c : {Codec::AmrWb, Codec::AmrNb}) {
        for (int ft = 0; ft < 16; ft++) {
            int bits = AmrBits(c, ft);
            if (bits <= 0) continue;
            // BE carries exactly AmrBits(ft) bits, so the pattern's padding
            // bits in the last speech byte must be zero for equality to hold.
            std::vector<std::uint8_t> frame = {static_cast<std::uint8_t>((ft << 3) | 0x04)};
            for (int i = 0; i < AmrBytes(c, ft); i++)
                frame.push_back(static_cast<std::uint8_t>(0xA5 + i * 31));
            if (bits % 8) frame.back() &= static_cast<std::uint8_t>(0xFF << (8 - bits % 8));
            for (bool oa : {true, false}) {
                auto got = Depay(c, PayloadFromFrame(c, frame, oa), oa);
                if (got.size() != 1 || got[0].first != frame[0] || !std::equal(got[0].second.begin(), got[0].second.end(), frame.begin() + 1, frame.end())) {
                    std::println(std::cerr, "selftest FAIL {} ft={} oa={}", CodecName(c), ft, oa);
                    return 1;
                }
            }
        }
    }
    if (LinearToAlaw(0) != 0xD5 || LinearToUlaw(0) != 0xFF) {
        std::println(std::cerr, "selftest FAIL G.711 digital zero");
        return 1;
    }
    for (Codec c : {Codec::Pcma, Codec::Pcmu}) {
        for (int v = -32768; v <= 32767; v++) {
            std::uint8_t b = G711Encode(c, static_cast<std::int16_t>(v));
            if (G711Encode(c, G711Decode(c, b)) != b) {
                std::println(std::cerr, "selftest FAIL {} not idempotent at {}", CodecName(c), v);
                return 1;
            }
        }
        double sig = 0;
        double err = 0;
        for (int i = 0; i < 8000; i++) {
            double x = 10000.0 * std::sin(2 * std::numbers::pi * 1000.0 * i / 8000.0);
            auto sample = static_cast<std::int16_t>(std::lround(x));
            std::int16_t d = G711Decode(c, G711Encode(c, sample));
            sig += x * x;
            err += (x - d) * (x - d);
        }
        double snr = 10 * std::log10(sig / err);
        if (snr < 30) {
            std::println(std::cerr, "selftest FAIL {} sine SNR {:.1f} dB", CodecName(c), snr);
            return 1;
        }
    }
    std::println("selftest OK");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--selftest") return SelfTest();
    if (argc < 8) {
        std::println(std::cerr, "usage: imsd-media local rtp_port remote_ip remote_port pt secs out");
        return 2;
    }
    std::string local = argv[1];
    int rtpPort = std::atoi(argv[2]);
    std::string rIp = argv[3];
    int rPort = std::atoi(argv[4]);
    int pt = std::atoi(argv[5]);
    double secs = std::atof(argv[6]);
    std::string out = argv[7];

    Codec codec = ParseCodec(EnvOr("CODEC", "AMR-WB"));
    bool amr = IsAmr(codec);
    int rate = CodecRate(codec);
    auto frameSamples = static_cast<std::size_t>(FrameSamples(codec));
    bool mic = EnvBool("MIC", false);
    bool play = EnvBool("PLAY", false);
    int amrMode = std::atoi(EnvOr("AMR_MODE", codec == Codec::AmrWb ? "2" : "7").c_str());
    double gain = std::atof(EnvOr("GAIN", "1.0").c_str());
    double playGain = std::atof(EnvOr("PLAY_GAIN", "1.0").c_str());
    int dtx = EnvBool("DTX", false) ? 1 : 0;
    bool octetAlign = EnvBool("OCTET_ALIGN", true);
    double mediaTimeout = std::atof(EnvOr("MEDIA_TIMEOUT", "6.0").c_str());
    bool rtpDump = EnvBool("RTP_DUMP", false);
    std::string micSrc = EnvOr("MIC_SRC", "");
    bool pcmDump = EnvBool("PCM_DUMP", false);
    constexpr int ExitMediaTimeout = 3;
    constexpr int PrimeFrames = 8;
    constexpr int MaxFill = 25;
    constexpr std::size_t PlayqMax = 256;

    int sock = socket(Is6(local) ? AF_INET6 : AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::println(std::cerr, "imsd-media: socket failed");
        return 1;
    }
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_storage bindA;
    socklen_t bindL = MakeAddr(local, rtpPort, bindA);
    if (bind(sock, reinterpret_cast<sockaddr*>(&bindA), bindL) != 0) {
        std::println(std::cerr, "imsd-media: bind [{}]:{} failed", local, rtpPort);
        return 1;
    }
    timeval tv{0, 20000};  // 20 ms recv timeout
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);

    TxState st;
    st.pt = pt;
    st.tsStep = static_cast<std::uint32_t>(frameSamples);
    st.dstLen = MakeAddr(rIp, rPort, st.dst);
    st.latched = st.dst;
    st.latchedLen = st.dstLen;

    signal(SIGTERM, OnSig);
    signal(SIGINT, OnSig);
    signal(SIGPIPE, SIG_IGN);

    // ---- mic uplink thread: pw-record (or the MIC_SRC file, real-time paced)
    // through the codec's encoder, one RTP packet per 20 ms frame
    std::atomic<bool> stop{false};
    std::jthread micThread;
    if (mic) {
        micThread = std::jthread([&] {
            Encoder enc;
            if (amr && !enc.Open(codec, dtx)) {
                std::println(std::cerr, "imsd-media: mic: {} encoder unavailable; silence fallback", CodecName(codec));
                return;
            }
            int fd = -1;
            pid_t pid = -1;
            if (!micSrc.empty()) {
                fd = open(micSrc.c_str(), O_RDONLY);
                if (fd < 0) {
                    std::println(std::cerr, "imsd-media: mic: cannot open MIC_SRC {}", micSrc);
                    return;
                }
            } else {
                Child rec = SpawnPw(false, /*toChild=*/false, rate);
                if (rec.pid < 0) return;
                fd = rec.fd;
                pid = rec.pid;
            }
            std::vector<std::uint8_t> raw(frameSamples * 2);
            std::vector<std::int16_t> samples(frameSamples);
            auto next = Clock::now();
            while (!stop.load()) {
                if (!ReadExact(fd, raw.data(), raw.size())) {
                    std::println(std::cerr, "imsd-media: mic: {} EOF; silence fallback", pid > 0 ? "pw-record" : "MIC_SRC");
                    break;
                }
                if (pid < 0) {
                    // a file delivers instantly; pace it like a microphone
                    next += std::chrono::milliseconds(20);
                    std::this_thread::sleep_until(next);
                }
                std::memcpy(samples.data(), raw.data(), raw.size());
                if (gain != 1.0)
                    for (auto& sample : samples) {
                        int v = static_cast<int>(sample * gain);
                        sample = static_cast<std::int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
                    }
                std::vector<std::uint8_t> payload;
                if (amr) {
                    std::vector<std::uint8_t> frame = enc.Encode(samples.data(), amrMode, dtx);
                    if (frame.empty()) continue;
                    payload = PayloadFromFrame(codec, frame, octetAlign);
                } else {
                    payload = G711Payload(codec, samples);
                }
                st.micOn.store(true);
                { std::scoped_lock g(st.lock); st.txMic++; }
                RtpSend(sock, st, payload);
            }
            st.micOn.store(false);
            close(fd);
            if (pid > 0) {
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
            }
        });
    }

    // ---- downlink playout (decode + RTP-timestamp clock reconstruction).
    // Runs for pw-play (PLAY=1) and/or the PCM dump (PCM_DUMP=1).
    Decoder dec;
    Child playCh;
    bool playOn = false;
    bool decodeOn = false;
    if (play || pcmDump) {
        if (!amr || dec.Open(codec)) {
            if (play) {
                playCh = SpawnPw(true, /*toChild=*/true, rate);
                playOn = playCh.pid >= 0;
            }
            decodeOn = playOn || pcmDump;
        } else {
            std::println(std::cerr, "imsd-media: play: {} decoder unavailable; capture only", CodecName(codec));
        }
    }
    std::FILE* pcmFile = decodeOn && pcmDump ? std::fopen((std::format("{}.pcm", out)).c_str(), "wb") : nullptr;
    std::mutex qlock;
    std::condition_variable qcv;
    std::deque<PktItem> playq;
    std::uint64_t rxPlayed = 0;
    std::uint64_t cng = 0;
    std::uint64_t late = 0;
    std::uint64_t qdrop = 0;
    std::jthread playThread;
    if (decodeOn) {
        playThread = std::jthread([&] {
            auto writePcm = [&](std::span<const std::int16_t> pcm) {
                if (pcmFile) std::fwrite(pcm.data(), 2, pcm.size(), pcmFile);
                if (!playOn) return true;
                std::size_t bytes = pcm.size() * 2;
                const char* p = reinterpret_cast<const char*>(pcm.data());
                std::size_t off = 0;
                while (off < bytes) {
                    ssize_t w = write(playCh.fd, p + off, bytes - off);
                    if (w <= 0) return false;
                    off += static_cast<std::size_t>(w);
                }
                return true;
            };
            auto applyGain = [&](std::vector<std::int16_t>& pcm) {
                if (playGain == 1.0) return;
                for (auto& sample : pcm) {
                    int v = static_cast<int>(sample * playGain);
                    sample = static_cast<std::int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
                }
            };
            // Decoder state is time-ordered: the CNG fill for a gap must be
            // decoded BEFORE the frames that follow the gap, so depay first,
            // decode in playout order.
            auto decodeFrame = [&](std::span<const std::uint8_t> f) {
                return amr ? dec.Decode(f.data(), static_cast<int>(f.size())) : G711DecodeFrame(codec, f);
            };
            std::vector<std::int16_t> zero(frameSamples, 0);
            std::optional<std::uint32_t> expect;
            for (;;) {
                PktItem item;
                {
                    std::unique_lock g(qlock);
                    qcv.wait(g, [&] { return !playq.empty() || stop.load(); });
                    if (playq.empty()) return;
                    item = std::move(playq.front());
                    playq.pop_front();
                }
                std::vector<std::vector<std::uint8_t>> frames;
                if (amr) {
                    for (auto& [hdr, speech] : Depay(codec, item.payload, octetAlign)) {
                        std::vector<std::uint8_t> f = {hdr};
                        f.insert(f.end(), speech.begin(), speech.end());
                        frames.push_back(std::move(f));
                    }
                } else {
                    // whole frames back to back (a gateway may pack 2 at ptime 40)
                    for (std::size_t off = 0; off + frameSamples <= item.payload.size(); off += frameSamples)
                        frames.emplace_back(item.payload.begin() + static_cast<std::ptrdiff_t>(off), item.payload.begin() + static_cast<std::ptrdiff_t>(off + frameSamples));
                }
                if (frames.empty()) continue;
                int fill = 0;
                if (!expect) {
                    for (int i = 0; i < PrimeFrames; i++)
                        if (!writePcm(zero)) return;
                } else {
                    std::uint32_t diff = (item.ts - *expect) & 0xFFFFFFFF;
                    if (diff >= 0x80000000u) {
                        late++;
                        continue;
                    }
                    fill = static_cast<int>(diff / frameSamples);
                    if (fill > MaxFill) fill = 0;
                }
                for (int i = 0; i < fill; i++) {
                    cng++;
                    std::uint8_t nodata = 0x7C;
                    std::vector<std::int16_t> pcm = amr ? dec.Decode(&nodata, 1) : zero;
                    applyGain(pcm);
                    if (!writePcm(pcm)) return;
                }
                for (auto& f : frames) {
                    rxPlayed++;
                    std::vector<std::int16_t> pcm = decodeFrame(f);
                    applyGain(pcm);
                    if (!writePcm(pcm)) return;
                }
                expect = (item.ts + static_cast<std::uint32_t>(frameSamples * frames.size())) & 0xFFFFFFFF;
            }
        });
    }

    // ---- main recv loop
    std::vector<std::uint8_t> silence = SilenceFrame(codec, 0, octetAlign);
    for (int i = 0; i < 5; i++) RtpSend(sock, st, silence);  // latch burst

    std::FILE* dump = rtpDump ? std::fopen((std::format("{}.rtp", out)).c_str(), "wb") : nullptr;
    double t0 = Now();
    double lastTx = 0;
    double lastRx = Now();
    std::uint64_t rx = 0;
    std::uint64_t rxBytes = 0;
    bool gotMedia = false;
    bool mediaEnded = false;
    std::string firstSrc;

    while (Now() - t0 < secs && !Quit.load()) {
        double now = Now();
        if (mediaTimeout > 0 && gotMedia && now - lastRx > mediaTimeout) {
            mediaEnded = true;
            break;
        }
        if (now - lastTx >= 0.02 && !st.micOn.load()) {
            RtpSend(sock, st, silence);
            lastTx = now;
        }
        std::uint8_t buf[65535];
        sockaddr_storage src;
        socklen_t srcLen = sizeof src;
        ssize_t n = recvfrom(sock, buf, sizeof buf, 0, reinterpret_cast<sockaddr*>(&src), &srcLen);
        if (n <= 0) continue;
        lastRx = Now();
        gotMedia = true;
        if (firstSrc.empty()) {
            char host[INET6_ADDRSTRLEN] = {};
            int port = 0;
            if (src.ss_family == AF_INET6) {
                auto* a = reinterpret_cast<sockaddr_in6*>(&src);
                inet_ntop(AF_INET6, &a->sin6_addr, host, sizeof host);
                port = ntohs(a->sin6_port);
            } else {
                auto* a = reinterpret_cast<sockaddr_in*>(&src);
                inet_ntop(AF_INET, &a->sin_addr, host, sizeof host);
                port = ntohs(a->sin_port);
            }
            firstSrc = std::format("{}:{}", host, port);
            std::scoped_lock g(st.lock);
            std::memcpy(&st.latched, &src, srcLen);
            st.latchedLen = srcLen;  // relatch to the actual media source
        }
        rx++;
        rxBytes += static_cast<std::uint64_t>(n);
        if (dump) {
            std::uint32_t len = static_cast<std::uint32_t>(n);
            std::uint8_t lb[4] = {static_cast<std::uint8_t>((len >> 24) & 0xFF),
                                  static_cast<std::uint8_t>((len >> 16) & 0xFF),
                                  static_cast<std::uint8_t>((len >> 8) & 0xFF),
                                  static_cast<std::uint8_t>(len & 0xFF)};
            std::fwrite(lb, 1, 4, dump);
            std::fwrite(buf, 1, static_cast<std::size_t>(n), dump);
        }
        if (playThread.joinable() && n >= 12 && (buf[1] & 0x7F) == pt) {
            std::size_t off = RtpPayloadOffset(std::span(buf, static_cast<std::size_t>(n)));
            std::uint32_t pktTs = (static_cast<std::uint32_t>(buf[4]) << 24) |
                                  (buf[5] << 16) | (buf[6] << 8) | buf[7];
            std::scoped_lock g(qlock);
            if (playq.size() >= PlayqMax) {
                playq.pop_front();
                qdrop++;
            }
            playq.push_back({pktTs, std::vector<std::uint8_t>(buf + off, buf + n)});
            qcv.notify_one();
        }
    }

    stop.store(true);
    qcv.notify_all();
    if (micThread.joinable()) micThread.join();
    if (playThread.joinable()) playThread.join();
    if (dump) std::fclose(dump);
    if (pcmFile) std::fclose(pcmFile);
    if (playOn) {
        close(playCh.fd);
        int status;
        for (int i = 0; i < 20; i++) {
            if (waitpid(playCh.pid, &status, WNOHANG) != 0) break;
            usleep(100000);
        }
        kill(playCh.pid, SIGKILL);
        waitpid(playCh.pid, nullptr, 0);
    }
    close(sock);

    // .stats sidecar (tiny; always written)
    if (std::FILE* sf = std::fopen((std::format("{}.stats", out)).c_str(), "w")) {
        std::print(sf,
            "{{\"codec\": \"{}\", \"tx\": {}, \"tx_mic\": {}, \"rx\": {}, \"rx_bytes\": {}, "
            "\"rx_played\": {}, \"cng\": {}, \"late\": {}, \"qdrop\": {}, "
            "\"first_src\": \"{}\", \"dst\": \"{}:{}\", \"pt\": {}, \"mic\": {}, "
            "\"play\": {}, \"amr_mode\": {}, \"media_ended\": {}}}",
            CodecName(codec), st.tx, st.txMic, rx, rxBytes, rxPlayed, cng, late, qdrop, firstSrc,
            rIp, rPort, pt, mic, play, amrMode, mediaEnded);
        std::fclose(sf);
    }
    std::println("imsd-media: codec={} tx={} tx_mic={} rx={} rx_played={} cng={} late={} " "qdrop={} rx_bytes={} first_src={} media_ended={}", CodecName(codec), st.tx, st.txMic, rx, rxPlayed, cng, late, qdrop, rxBytes, firstSrc, mediaEnded);
    return mediaEnded ? ExitMediaTimeout : 0;
}
