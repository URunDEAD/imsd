<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts® -->

# imsd

Userspace IMS/VoLTE daemon for mainline Linux phones.

Modem firmware on typical mainline-Linux phones will bring up the IMS PDN but
can't be used for actual call audio. imsd implements the whole IMS client in
userspace instead: SIP registration with USIM AKA authentication over kernel
IPsec (ESP), call signalling, and the AMR-WB RTP media plane wired into
PipeWire so a stock mobile dialer — Plasma Dialer, or GNOME Calls under Phosh
and GNOME Mobile — can place and receive real VoLTE calls on a commercial
network.

## Status

Call-capable, both directions. Outgoing VoLTE calls place, answer, carry
full-duplex AMR-WB audio, and tear down; incoming calls ring, answer via
`Accept`, and carry media the same way — driven end-to-end from a stock
dialer over the frozen D-Bus ABI. Current state:

The engine core (`imsd-core`) is a GLib-free, pure-`std` static library whose
every decision — SIP framing, SDP selection, the AKAv1-MD5 digest, the
`ip xfrm` command sequence, and the whole call FSM — is unit-tested on a dev
box with no bus, modem, or phone; several suites pin output byte-for-byte
against recorded network traffic. GLib/GDBus, sockets, and subprocess control
live only in the `imsd` daemon shell.

## Architecture

C++26 modules, built with Crafter Build. Five build products:

- **imsd-core** (static library) — the engine, pure `import std` C++ with no
  GLib and no I/O side effects. Everything here is unit-testable on any dev
  box without a modem, a bus, or a phone; several suites pin output
  byte-for-byte against recorded network traffic.
- **imsd** (executable) — the daemon shell: GLib main loop, GDBus service,
  sockets, subprocess control (`qmicli`/`mmcli`/`ip`), and the registration
  sequence + run loop that pumps the core. All GLib and I/O live here.
- **imsd-media** (executable) — the RTP/AMR-WB data plane, spawned once per
  call. Pure `import std` + POSIX, no GLib; links `-ldl` and dlopen's the
  codecs. A separate process for crash isolation and the far-end-hangup
  exit-code contract.
- **imsd-dialerd** (executable) — the Plasma Mobile integration: a session
  daemon owning `org.kde.telephony.{CallUtils,DeviceUtils,UssdUtils}`,
  bridging them to imsd on the system bus. GDBus translation only, no core.
  It is a separate process because those are per-user *session*-bus names
  that the root system daemon cannot own; built with `-- --product=dialerd`,
  autostarted from an `.desktop` file in place of plasma-dialer's
  `modem-daemon` (whose autostart must be disabled).
- **imsd-ofonod** (executable) — the GNOME Calls integration, for Phosh and
  GNOME Mobile: a system daemon owning `org.ofono` and presenting imsd as an
  oFono modem, which is the seam GNOME Calls' bundled `ofono` provider plugin
  drives. GDBus translation only, no core; built with `-- --product=ofonod`.
  See [GNOME Calls (Phosh, GNOME Mobile)](#gnome-calls-phosh-gnome-mobile).

### Desktop integration

imsd itself is desktop-agnostic: everything a dialer needs is on the system
bus as `net.catcrafts.IMS1`. Each desktop gets a small translation daemon
speaking whatever backend protocol its dialer already consumes, so the dialer,
the shell and the call-history/contacts stack all stay unmodified.

| Desktop | Daemon | Owns | Consumed by |
|---|---|---|---|
| Plasma Mobile | `imsd-dialerd` | `org.kde.telephony.*` (session bus) | plasma-dialer + kde-telephony-daemon, replacing `modem-daemon` |
| Phosh / GNOME Mobile | `imsd-ofonod` | `org.ofono` (system bus) | GNOME Calls' `ofono` provider plugin |

Phosh's own call UI needs nothing extra: it consumes `org.gnome.Calls`, which
GNOME Calls exports once it has a working origin.

### Terminating-network dependency (incoming)

A UE only receives a VoLTE call as a SIP INVITE if the network's Terminating
Access Domain Selection (T-ADS) routes it to the PS/IMS domain rather than
paging the modem over CS. That requires the IMS registration to advertise
voice capability — the MMTEL ICSI (`+g.3gpp.icsi-ref=...mmtel`) and
`+sip.instance` media-feature tags in the REGISTER Contact, which
`Imsd:Messages` emits. T-ADS may still select CS for reasons outside the
client's control (operator policy, the modem's own radio-capability
signalling, a competing registration). When it does, no INVITE reaches imsd;
that is a network/modem-integration matter, not a daemon defect.

## Supported hardware & carriers

imsd talks to the modem through ModemManager plus `qmicli` over QRTR (USIM
AKA via a UIM logical channel), so in practice it currently needs a Qualcomm
modem on a mainline kernel with working WWAN/MM integration, kernel ESP
(`ip xfrm`), and PipeWire for audio.

Everything below has been verified by community members to be fully workking:

| Device | OS | Carrier
|---|---|---|
| The Fairphone (Gen. 6) | postmarketOS, Linux 7.1.2 | KPN NL |
| The Fairphone (Gen. 6) | postmarketOS, Linux 7.1.2 | Telekom Deutschland GER |
| The Fairphone (Gen. 6) | postmarketOS, Linux 7.1.2 | Phonero |
| The Fairphone (Gen. 6) | postmarketOS, Linux 7.1.2 | Telia Norge |

## Build

[Crafter Build](https://forgejo.catcrafts.net/Catcrafts/Crafter.Build) +
gio-2.0 headers (`glib2` on Arch, `glib-dev` on Alpine).

```sh
crafter-build                       # bin/imsd-<target>-<march>/imsd (the daemon)
crafter-build -- --product=media    # bin/imsd-media-.../imsd-media (the media leg)
crafter-build -- --product=dialerd  # bin/imsd-dialerd-.../imsd-dialerd (Plasma backend)
crafter-build -- --product=ofonod   # bin/imsd-ofonod-.../imsd-ofonod (GNOME Calls backend)
crafter-build test                  # unit tests (Util, Aka, Ipsec, Messages, Engine, Sip, Sdp)
```

### Building with make (distro packaging)

For environments without crafter-build, the Makefile mirrors the same clang
module pipeline: GNU make, clang++ with libc++ and the libc++ std module
sources (`std.cppm` — package `llvm-runtimes` on Alpine, `libc++` on Arch),
lld, and gio-2.0 headers.

```sh
make            # build/make/{imsd,imsd-media,imsd-dialerd,imsd-ofonod}
make check      # the same 7 unit-test suites
make install    # DESTDIR/PREFIX staged install incl. the packaging/ files
```

`project.cpp` stays the canonical build description; the Makefile tracks it.

### Cross-compiling for a phone (aarch64 Alpine/postmarketOS)

```sh
packaging/make-sysroot.sh                  # one-time: Alpine aarch64 sysroot from the CDN
crafter-build -- --target=aarch64-alpine-linux-musl \
    --sysroot=$HOME/.cache/imsd/sysroot-aarch64-alpine \
    --march=armv8.6-a+fp16fml+aes+sha3+sm4 --mtune=cortex-a720
# same flags after `crafter-build test --target=aarch64-alpine-linux-musl`
# run the suite under qemu-aarch64 against the sysroot
```

## Run (development)

```sh
./bin/imsd-*/imsd --session   # own net.catcrafts.IMS1 on the session bus
busctl --user call net.catcrafts.IMS1 /net/catcrafts/IMS1 net.catcrafts.IMS1 GetStatus
```

On a phone the daemon runs as root on the system bus (policy in
`packaging/net.catcrafts.IMS1.conf` — it grants the postmarketOS default
account `user` access; adjust it alongside `AUDIO_USER` if your session
account is named differently), started by `packaging/imsd.service`
after `packaging/ims-pdn-up.sh` has brought up the ims PDN through
ModemManager. The bring-up never lets the ims PDN become the phone's IPv6
default route: the PDN's router advertisement is not accepted as a default
router (`accept_ra_defrtr=0` on the netdev; its prefix and MTU still apply)
and the P-CSCF gets a host route instead, so on IPv4-only wifi the phone's
other IPv6 traffic (apk, browsers) is not sent into the IMS walled garden,
where it would hang. Media peers get a host route per call; anything else
on the IMS network (its DNS servers, a traceroute target) needs its own
(`ip -6 route add <addr> dev qmapmux0.0`). The PDN MTU ModemManager reports
is set on the link. It spawns
`/usr/libexec/imsd-media` per call. Registration
resumes an in-kernel IPsec SA when one is present (avoiding the network's
fresh-SA throttle) and keeps itself alive with a re-REGISTER refresh at half
the granted lifetime. `packaging/APKBUILD` builds the apk from source via
the Makefile; `packaging/APKBUILD.binary` + `make-bin-tarball.sh` wrap a
cross-compiled build into an apk instead.

## GNOME Calls (Phosh, GNOME Mobile)

`imsd-ofonod` presents imsd on the system bus as an oFono modem — a Manager at
`/`, one Modem + VoiceCallManager at `/imsd`, and a VoiceCall object per live
call — which is what GNOME Calls' bundled `ofono` provider plugin drives.
Calls, Phosh and GNOME Shell run unmodified.

`org.ofono` is the seam because it is the only one a third party can
implement. `org.gnome.Calls.Call` is what Calls *exports* so a shell can
observe and control a call that already exists (Accept, Hangup, SendDtmf,
Silence — there is no Dial); Phosh consumes it from Calls. Calls' own backends
are libpeas provider plugins (`mm`, `ofono`, `sip`, `dummy`) linked against
private headers that no distro installs.

Setup:

```sh
apk add imsd-ofono                    # enables imsd-ofonod via a systemd preset
systemctl enable --now imsd-ofonod    # hand installs: system service, org.ofono is a system name
# point Calls at the ofono provider (the package ships this as a gschema
# override; this is the per-user equivalent)
gsettings set org.gnome.Calls autoload-plugins "['ofono']"
```

Drop `mm` from `autoload-plugins` rather than adding `ofono` alongside it: the
ModemManager provider would offer the modem's own CS voice path as a second
origin, and on a phone whose voice path is imsd that origin carries no audio.

Do not run this alongside a real `ofonod` — there is one `org.ofono` name and
one owner; the unit declares `Conflicts=ofono.service` and the daemon exits if
it loses the name. Running `imsd-dialerd` at the same time is fine (different
bus, different consumer).

Verified on a Fairphone 6 (postmarketOS, dbus-broker) with GNOME Calls 50.0:
outgoing and incoming calls with audio both ways, answered and hung up through
`org.gnome.Calls.Call` — the surface Phosh drives. One dbus-broker detail is
load-bearing and lives in the policy file: dbus-broker checks a broadcast
against the sender's send policy for every name the receiver owns, so root
(imsd) must be allowed `send_destination="org.ofono"` or imsd's call signals
never reach the daemon.

Known gaps, all inherited rather than introduced:

- **USSD is refused** with `org.ofono.Error.NotSupported`; the IMS stack has no
  USSD path. It is deliberately not advertised in the modem's `Interfaces`.
- **No hold, multiparty, transfer or call waiting** — imsd is a single-call
  stack, so those methods return `org.ofono.Error.NotImplemented`.
- **Per-call caller-ID withholding is refused** rather than silently ignored:
  imsd cannot honour a CLIR request, and failing is safer than dialling with
  the caller ID exposed.
- **Phosh's emergency-call screen lists nothing.** Not for want of the data:
  `imsd-ofonod` publishes `org.ofono.VoiceCallManager.EmergencyNumbers` (112,
  911, plus `EMERGENCY_NUMBERS`), which is oFono's standard place for it.
  Calls' ofono provider simply never reads it — it makes no `GetProperties`
  call on the VoiceCallManager, and returns NULL for the origin's
  `emergency-numbers`, which is what
  `org.gnome.Calls.EmergencyCalls.GetEmergencyContacts` is built from. (The
  ModemManager provider does read it, via `mm_sim_dup_emergency_numbers()`.)
  An upstream fix would close this. Dialling an emergency number from the
  normal dialpad is unaffected — that reaches imsd's own classifier.
- DTMF is forwarded to imsd, where `SendDtmf` is still a stub (RFC 4733 is not
  implemented yet) — the same limitation Plasma has.
- **Restart GNOME Calls after restarting `imsd-ofonod` itself.** When the
  modem's `VoiceCallManager` interface goes away and comes back, Calls 50.0
  adds a new origin but leaves the old origin's `CallAdded` handler connected:
  after one such cycle every call is added twice, after a few the stale handler
  runs on freed memory and Calls dies with SIGBUS in `g_hash_table_lookup`
  (measured on the FP6). Calls-side; an upstream fix belongs in
  `plugins/provider/ofono/calls-ofono-provider.c`. To keep imsd's own
  restarts and registration drops from triggering it, `imsd-ofonod` lists the
  interface permanently: `Online` tracks registration, and `Dial` fails with
  "imsd is not registered" / "imsd is not running" instead. Only `org.ofono`
  vanishing — this daemon restarting — still cycles the origin.

Development, without a phone or root:

```sh
./bin/imsd-*/imsd --session &          # imsd's ABI on the session bus
./bin/imsd-ofonod-*/imsd-ofonod --session
busctl --user call org.ofono / org.ofono.Manager GetModems
# or keep org.ofono on the system bus and imsd on the session bus:
IMSD_BUS=session imsd-ofonod
```

## Configuration

Everything is environment variables. The packaged unit reads
`/run/imsd.env` (written by `ims-pdn-up.sh`: the connected ims netdev) and
then `/etc/imsd.env` (your configuration, wins on conflict). Minimum viable
`/etc/imsd.env`:

```sh
# REQUIRED until P-CSCF discovery from the PDN's PCO is implemented: your
# carrier's P-CSCF address. Find it in a stock-firmware capture or your
# carrier's IMS documentation.
PCSCF=2001:db8::105
```

### imsd

| Variable | Default | Meaning |
|---|---|---|
| `PCSCF` | *(none — required)* | P-CSCF address; without it registration fails with a clear error |
| `PCSCF_PORT` | `5060` | unprotected SIP port for the initial REGISTER |
| `DEV` | `qmapmux0.0` | ims-PDN netdev; the packaged service passes the real one via `/run/imsd.env` |
| `LOCAL` | *(auto)* | UE address; default = the global IPv6 on `DEV` |
| `USER_AGENT` | `imsd/<version>` | REGISTER User-Agent. Some networks fingerprint UAs; setting your device's stock build string reproduces the stock modem's registration exactly. Empty omits the header |
| `STATE_FILE` | `/var/lib/imsd/imsreg.state` (root) / `$XDG_STATE_HOME/imsd/imsreg.state` | persisted registration context for warm resume |
| `RESUME` | `auto` | `0` forces a fresh registration (ignores a warm SA) |
| `REFRESH_INTERVAL` | *(auto)* | keepalive re-REGISTER period in s; default = half the granted expiry, clamped to [120, 1800] |
| `EALG` | `aes-cbc` | offered ESP cipher (`aes-cbc`, `des-ede3-cbc`, `null`). From the 401's `Security-Server` list imsd selects the highest-preference `ipsec-3gpp` mechanism that matches this offer (RFC 3329 §2.3.1) and takes the SPIs/ports from the entry carrying them (some cores list every mechanism they support and put the SPIs on the one they applied); when no entry matches, the P-CSCF's SPI-carrying entry is installed as listed (`hmac-md5-96`, an absent `ealg` = null) and the journal says so. The line, the selection and the SA parameters are logged at every fresh registration |
| `SIP_TRANSPORT` | `auto` | transport of the protected leg (the second REGISTER and everything after it): `tcp`, `udp`, or `auto` = TCP, falling back to a fresh registration over UDP when the P-CSCF leaves two TCP connects unanswered — but only on a phone that has never registered over TCP: once the state file (`/var/lib/imsd/imsreg.state`) records a TCP registration, a silent connect is treated as an outage, not a policy (delete the file or set `udp` to override, e.g. after a SIM change). The challenge (first REGISTER) is always UDP. Over UDP a REGISTER or INVITE larger than ~1.2 KB leaves as IPv6 fragments at the ims PDN's 1280-byte MTU (the journal says so once per flow); there are no SIP retransmission timers over UDP yet, so a lost datagram costs a timeout. The SAs, the listener and the firewall rule cover both transports |
| `EMERGENCY_NUMBERS` | *(empty)* | comma-separated additions to the builtin 112/911 emergency numbers (e.g. a private test core's short code). SIM `EF_ECC` is not read yet |
| `RTP_PORT` | `50004` | local RTP port advertised in SDP |
| `PRECOND` | `0` | `1` offers SDP QoS preconditions |
| `CODECS` | *(empty — defaults)* | comma-separated codec preference list over `AMR-WB`, `AMR` (or `AMR-NB`), `PCMA`, `PCMU`: restricts and orders both the codecs offered on an outgoing call and those accepted from an inbound offer (default: offer AMR-WB + AMR, accept all four in that order). A bench knob — a network whose gateway transcodes every caller up to AMR-WB otherwise never lets the narrowband path run |
| `DUMP_SIP` | `0` | `1` writes raw REGISTER-200/SUBSCRIBE-200 dumps, the last inbound INVITE (`imsd-invite-in.raw`) and the last outgoing INVITE (`imsd-invite-out.raw`), mode 0600, for debugging — they contain your IMSI/MSISDN and addresses |
| `DUMP_DIR`, `OUT_DIR` | state dir | where dumps / per-call media stats land |
| `IMSD_MEDIA` | *(auto)* | path to `imsd-media` (default: next to `imsd`, else `/usr/libexec/imsd-media`) |

The protected client/server ports are fixed at 45061/45062
(`imsd::util::kPortUc`/`kPortUs`) — changing them in source also requires
updating the firewall rules `ims-pdn-up.sh` installs.

### imsd-media (per-call, set through imsd's environment)

| Variable | Default | Meaning |
|---|---|---|
| `MIC`, `PLAY` | `1`, `1` | uplink mic capture / downlink playout via PipeWire |
| `AUDIO_USER` | `user` | desktop user whose PipeWire session carries call audio (postmarketOS default account) |
| `GAIN`, `PLAY_GAIN` | `10`, `1.0` | uplink / downlink gain |
| `AMR_MODE` | `2` | AMR-WB encoder mode (0–8) |
| `DTX` | `0` | encoder discontinuous transmission |
| `MEDIA_TIMEOUT` | `6.0` | seconds of downlink silence before exit 3 (far-end-hangup signal) |
| `RTP_DUMP` | `0` | `1` captures raw downlink RTP next to the stats file |

## D-Bus ABI

`net.catcrafts.IMS1` at `/net/catcrafts/IMS1`. The ABI is frozen — dialer
integrations depend on it:

| Member | Signature |
|---|---|
| `Dial(number)` | `s → s` (callUni) |
| `HangUp(callUni)` | `s` |
| `Accept(callUni)` | `s` |
| `SendDtmf(callUni, tones)` | `ss` |
| `GetCalls()` | `→ aa{sv}` |
| `GetStatus()` | `→ a{sv}` |
| `CallAdded` | `s a{sv}` |
| `CallStateChanged` | `s s s` (callUni, state, reason) |
| `CallDeleted` | `s` |
| `RegistrationChanged` | `b` |

Both `imsd-dialerd` and `imsd-ofonod` are pure translations of this ABI; a new
desktop needs a new translation daemon, not changes here.

## License

GPL-3.0-only — see [LICENSE](LICENSE).

## Copyright

Copyright (C) 2026 Catcrafts®
catcrafts.net
