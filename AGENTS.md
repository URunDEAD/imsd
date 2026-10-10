<!-- SPDX-License-Identifier: GPL-3.0-only -->
<!-- SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts® -->

# AGENTS.md — working on imsd

This file is for coding agents and for anyone changing imsd: what the
repository contains, how it is built, tested and packaged, how the daemon
behaves at runtime, and the conventions a change has to follow. The README
is for users: what imsd is, how to install and configure it. The
configuration variables are documented there, not here (§10).
Where this file and the code disagree, the code is right and this file has a
bug — fix it in the same change.

## 1. What imsd is

imsd is a userspace IMS/VoLTE client for mainline Linux phones. The modem
firmware on such phones brings up the IMS PDN but cannot carry call audio, so
imsd does the whole IMS client itself: SIP registration with USIM AKA
authentication over kernel IPsec (ESP), call signalling for outgoing and
incoming calls, and the RTP media plane (AMR-WB, AMR, G.711) wired into
PipeWire. A stock mobile dialer — Plasma Dialer, or GNOME Calls under Phosh
and GNOME Mobile — places and receives real VoLTE calls on a commercial
network through it.

It talks to the modem through ModemManager (`mmcli`) and `qmicli` over QRTR
(the USIM AKA run goes through a UIM logical channel), installs its security
associations with `ip xfrm`, and plays and records audio with the PipeWire
tools (`pw-play`, `pw-record`). In practice that means a Qualcomm modem on a
mainline kernel with working WWAN/ModemManager integration, kernel ESP, and
PipeWire. The verified device/carrier combinations are in the README.

## 2. Repository layout

| Path | What it is |
|---|---|
| `project.cpp` | The canonical build description, for [Crafter Build](https://forgejo.catcrafts.net/Catcrafts/Crafter.Build). Chooses the product by `--product=`. |
| `interfaces/Imsd.cppm` | The primary module interface of `imsd-core`: `export import`s the seven partitions below. |
| `interfaces/Imsd-Util.cppm` | `imsd::util` — SIP tokens, IPv6 host/port formatting, hex/base64, MD5 (RFC 1321), the fixed protected ports `PortUc`/`PortUs` (45061/45062). |
| `interfaces/Imsd-Sip.cppm` | `imsd::sip` — SIP as a text layer plus TCP framing: header extraction, status codes, the granted lifetime, `ResponseOf` transaction matching. Not a full RFC 3261 parser, deliberately. |
| `interfaces/Imsd-Sdp.cppm` | `imsd::sdp` — SDP offer/answer: the AMR-WB + AMR + telephone-event offer (optional IR.92 preconditions), the parser the media plane needs, `BuildAnswer` for the terminating side, the `CODECS` override. |
| `interfaces/Imsd-Aka.cppm` | `imsd::aka` — USIM AKA glue: the AKAv1-MD5 digest (RFC 3310), IMPI/IMPU/realm from IMSI + MCC/MNC, parsers for `qmicli --uim-get-card-status` and `mmcli` output, the AUTHENTICATE APDU and its response. |
| `interfaces/Imsd-Ipsec.cppm` | `imsd::ipsec` — the two ESP SA pairs (TS 33.203) as `ip xfrm` argv lists, the Security-Server mechanism selection, and the reader that reconstructs SPIs/ports from `ip xfrm state`/`policy` text for a warm resume. |
| `interfaces/Imsd-Messages.cppm` | `imsd::msg` — every SIP request and response imsd sends, built from an explicit `Context` and per-request variables so the builders are pure functions; byte-pinned by `tests/Messages`. Also `IsEmergency`. |
| `interfaces/Imsd-Engine.cppm` | `imsd::engine` — the decisions as pure reducers: `CallMachine` (one call's dialog, returning `Action`s for the shell) and `RegRecovery` (the registration recovery policy, §7.4). |
| `interfaces/ImsdBus.cppm` | `imsd::bus` — the polkit gate in front of call control, shared by imsd and imsd-ofonod. GLib, so outside `imsd-core`. |
| `implementations/main.cpp` | The `imsd` daemon shell: GLib main loop, GDBus service, sockets, subprocesses, the registration sequence and the run loop that pumps the core. |
| `implementations/media.cpp` | `imsd-media`, the per-call RTP leg. Pure std + POSIX, dlopen's the AMR codecs. |
| `implementations/dialerd.cpp` | `imsd-dialerd`, the Plasma Mobile session-bus backend (`org.kde.telephony.*`). |
| `implementations/ofonod.cpp` | `imsd-ofonod`, the GNOME Calls system-bus backend (`org.ofono`). |
| `tests/<Suite>/main.cpp` | The seven unit-test suites: `Util`, `Aka`, `Ipsec`, `Messages`, `Engine`, `Sip`, `Sdp`. |
| `packaging/` | systemd units, the bring-up and upgrade scripts, D-Bus and polkit policies, presets, the gschema override, `APKBUILD.binary` and the CI helpers (§9). |
| `.forgejo/workflows/package.yml` | CI: builds the aarch64 apks on every push to `main` and publishes them to the Forgejo Alpine registry (§9.4). |
| `bin/`, `build/` | crafter-build's outputs and intermediates. Ignored. |

Also ignored by `.gitignore`, and never to be committed: `*.raw`, `*.rtp`,
`*.stats`, `imsreg.state`, `imsd-*.tar.gz` — captures and runtime state carry
the subscriber's IMSI/MSISDN and network addresses.

## 3. Build products

Six products come out of the tree. C++26 modules throughout, `import std;`
in every source but the re-exporting `Imsd.cppm`, clang + libc++ + lld.

| Product | Kind | Built from | Links | Notes |
|---|---|---|---|---|
| `imsd-core` | static library | the `Imsd` module and its seven partitions | — | Pure `import std` C++, no GLib, no I/O side effects: every message, digest, SA command and call-state decision is here and unit-testable on any dev box without a bus, a modem or a phone. |
| `imsd` | executable | `implementations/main.cpp` | `imsd-core`, `imsd-bus`, gio-2.0, pthread | The daemon. Default product. |
| `imsd-bus` | static library | `interfaces/ImsdBus.cppm` | gio-2.0 | The polkit gate (`net.catcrafts.IMS1.Voice`) imsd and imsd-ofonod both link. A translation daemon that runs as root on the system bus reaches imsd as root, which polkit always allows, so it gates its own callers through this or it is a bypass. |
| `imsd-media` | executable | `implementations/media.cpp` | `-ldl -lpthread` | The RTP/AMR data plane, spawned once per call. No GLib, no core. A separate process for crash isolation and for the far-end-hangup exit-code contract (§7.6). `-- --product=media`. |
| `imsd-dialerd` | executable | `implementations/dialerd.cpp` | gio-2.0 | Plasma Mobile integration: a session daemon owning `org.kde.telephony.{CallUtils,DeviceUtils,UssdUtils}`, bridging to imsd. GDBus translation only. A separate process because those are per-user session-bus names a root system daemon cannot own. `-- --product=dialerd`. |
| `imsd-ofonod` | executable | `implementations/ofonod.cpp` | `imsd-bus`, gio-2.0 | GNOME Calls integration: a system daemon owning `org.ofono`, presenting imsd as an oFono modem. GDBus translation plus the polkit gate on its own callers. `-- --product=ofonod`. |

The rule behind the split: **`imsd-core` stays GLib-free and free of I/O so
that every decision in it has a unit test.** New protocol logic goes into a
partition with a test. The shell (`main.cpp`) holds protocol decisions no
test reaches — some older than 0.3.10, some added by 0.3.10 or 0.3.11: the reg-event
subscription schedule (§7.4: its retry
ladder, Retry-After clamp, Timer F, renewal point and RFC 6665 reasons); the
registration sequence and its transactions (`AwaitFinal`/`AwaitInitialFinal`:
which message is a REGISTER's final, the dispatch of requests arriving
meanwhile and the routing of our reg-event SUBSCRIBE's final read there, the
12 s wait restarted on a provisional); the reregistration's headers and its
answer to a challenge (`Reregister`: the stored challenge answer, or the empty
digest for a pre-0.3.11 state file, the new SPIs, `PortUcNext`; a 401 thrown
as a refusal carrying its status); the
inputs to `RegRecovery` — `SipFlow::Connect`'s split of failures into local
refusals and network silence, and the catch sites that sort a failed refresh
(the keepalive's, `ReconnectCycle`'s) and a failed fresh attempt
(`FreshRecover`, `DeferBringUp`) into Refused, Silent or Throttled with the
status, the Retry-After and `saReplaced_`, the bring-up resume's catch (a
refusal's Retry-After fed as a hold, the old registration written off after
any failed resume), the `OnLink` read that writes the old registration off
when its address has left the netdev (`FreshRecover` after a failed move,
`AddressGone`), and `PacketServiceUp`'s read of
ModemManager before a silent cycle is counted; the shell's reaction to a NOTIFY
terminating our binding (the flow closed at once, or kept under a call and
closed when the call ends, a reconnect cycle's or the bring-up resume's wait
ended by it — the reducer's verdict is pinned, the reaction is not); and the
holds file's codec (`PersistHolds`/`LoadHolds`: aging by boot seconds, the
pdn/boot match, defaults for absent keys — proven on the phone only, captures
45- and 70- of 2026-09-25-polkit-voice). For the schedule, a reducer next to
`RegRecovery`, with a Subscription-State parser in `Imsd:Sip` pinned by
`tests/Sip`, is the follow-up that moves it; a pure format/parse pair in
`Imsd:Engine` is the holds file's.

## 4. Building

### 4.1 Native, with crafter-build

crafter-build is the Crafter.Build Flatpak. It carries its own clang, lld and
libc++, and its SDK (org.freedesktop.Sdk, from Flathub) has the gio-2.0
headers, so the host needs Flatpak and nothing else:

```sh
flatpak remote-add --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak remote-add --if-not-exists catcrafts https://flatpak.catcrafts.net/catcrafts.flatpakrepo
flatpak install catcrafts net.catcrafts.CrafterBuild//stable
mkdir -p ~/.local/bin
printf '#!/bin/sh\nexec flatpak run net.catcrafts.CrafterBuild//stable "$@"\n' > ~/.local/bin/crafter-build
chmod +x ~/.local/bin/crafter-build
```

`crafter-build` is a script and not an alias so that the name also resolves in
a shell that has not defined the alias (a fresh `bash -c`, a CI step);
`~/.local/bin` has to be on `PATH` (any directory on it will do). The
sandbox does not see the host's `/tmp`: keep the checkout under `/home`, or
grant its directory with `flatpak run --filesystem=<dir>`.

```sh
crafter-build                       # bin/imsd-<target>-<march>-<mtune>-<id>/imsd
crafter-build -- --product=media    # bin/imsd-media-…/imsd-media
crafter-build -- --product=dialerd  # bin/imsd-dialerd-…/imsd-dialerd
crafter-build -- --product=ofonod   # bin/imsd-ofonod-…/imsd-ofonod
crafter-build test                  # the seven unit-test suites
crafter-build lint                  # the house-style rules (§4.2, §6)
```

Output directories under `bin/` are named `<product>-<target>-<march>-<mtune>-<id>`;
the `./bin/imsd-*/imsd` globs in §8.3 rely on that.

A host-native binary built this way links the Flatpak's libc++ through an
rpath into `/app/lib`: run it inside the sandbox (`flatpak run --command=sh
net.catcrafts.CrafterBuild//stable`, with `--socket=session-bus` for §8.3's
daemons — the sandbox's own session bus is a filter that refuses their names;
`crafter-build test` runs the suites there
anyway). A sysroot
build (§4.3) gets no such rpath — it links the sysroot's libc++.

### 4.2 The lint rules, a dependency of project.cpp

`project.cpp` imports the house-style rules (§6) from their own repository:

```cpp
#pragma crafter dependency "https://forgejo.catcrafts.net/Catcrafts/Crafter.LintRules.git" commit "<sha>"
import Crafter.LintRules;
```

crafter-build clones that repository — and updates the clone on later loads —
and builds it for the host whenever it loads `project.cpp`: every build does
this, CI's included, not only `lint`. The commit is pinned, so a change to
the rules reaches imsd when the pin moves. `LintRules::AddRules` registers
the rules on each product's configuration.

```sh
crafter-build lint            # report every finding; exit 1 on any
crafter-build lint 'naming'   # one rule (the argument is a glob)
crafter-build format          # apply the transform rules in place
```

Six rules read the AST (`naming`, `no-char-pointer`, `const-local`,
`constexpr-constant`, `fixed-width-types`, `single-declaration`) and need the
module PCMs: build the four programs and the tests first. With crafter-build
2.1.0 they still do not run on 2 of the 18 files, `implementations/main.cpp`
among them (`'type_traits' file not found`).

### 4.3 Cross-compiling for a phone (aarch64 Alpine / postmarketOS)

```sh
packaging/make-sysroot.sh                  # one-time: Alpine aarch64 sysroot from the CDN (edge)
crafter-build -- --target=aarch64-alpine-linux-musl \
    --sysroot=$HOME/.cache/imsd/sysroot-aarch64-alpine \
    --march=armv8.6-a+fp16fml+aes+sha3+sm4 --mtune=cortex-a720
# `crafter-build test` with the same flags fails: its runner is qemu-aarch64,
# which the Flatpak's sandbox does not have. Test host-native (§4.1).
```

`make-sysroot.sh` needs no root, qemu or device: it downloads
`apk-tools-static` and the Alpine keys and populates a minimal rootfs (musl,
libc++ with its std module source, glib and their headers, compiler-rt,
libunwind, gcc for its
`crtbeginS.o`/libgcc, which clang's `*-alpine-linux-musl` driver links
against). In a cross build `project.cpp` does not ask the host's pkg-config
(it would answer for the wrong architecture); it adds `-I=/usr/include/glib-2.0`
and `-I=/usr/lib/glib-2.0/include`, which clang resolves inside the sysroot.
crafter-build compiles the std module against the sysroot's libc++ headers
from the sysroot's own source — Alpine's `llvm-runtimes`, the same release
as its libc++, which `make-sysroot.sh` installs (`libc++-dev` ships none);
run over an existing sysroot the script upgrades its packages (`apk add
--upgrade`).
Without that source it falls back to the toolchain's source for the same libc++
major release, then to the sources Crafter.Build carries for other
releases. This is the flow CI runs (§9.4).

## 5. Tests

`crafter-build test` runs the seven suites. Each suite
is a plain `main()` with a `Check(bool, message)` counter: failures print
`FAIL: …`, success prints `<Suite>: all tests passed`, and the exit status is
the failure count for six suites — non-zero short of a multiple of 256 — and
1 for `Engine`. `crafter-build test` ending in `7 passed`, on every
commit of a range (§11), is the project's definition of green.

What the suites pin:

- `Util` — tokens, IPv6 detection and host/port formatting, hex/base64, MD5.
- `Aka` — the AKAv1-MD5 digest, identity derivation, the `qmicli`/`mmcli`
  text parsers, the AUTHENTICATE APDU and its response parser.
- `Ipsec` — the exact `ip xfrm` argv lists for the SA pairs, the
  Security-Server mechanism selection, the SA reader.
- `Messages` — every outgoing SIP message byte-for-byte, cross-generated from
  the Python reference stack imsd replaced.
- `Sip` — the text layer's edge cases (empty header values, line anchoring,
  keepalive discards), `ResponseOf`, refused finals with reason headers,
  reginfo documents. A change that flips one of those tests is a behaviour
  change, not a cleanup.
- `Sdp` — the offer's exact bytes, the parser, `BuildAnswer` at the offer's
  own payload types and octet-align mode, the `CODECS` override.
- `Engine` — the call state machine driven through recorded network dialogs
  (reliable 18x/PRACK, 2xx ACK + media start, far-end hangup, CANCEL/answer
  races, busy/error finals, the terminating side's 100/180/200/486/487/488,
  G.711-only and AMR-NB gateway offers, ring timeout, BYE both ways, the
  emergency chain) and `RegRecovery` (thresholds, backoffs, the throttle
  schedule, Retry-After, which waits a Dial may cut short, when the old
  registration counts as gone, the holds a restart takes over, a binding the
  network terminated).

Rules:

- A decision in `imsd-core` gets a test in the suite that owns the partition.
  Fixtures are recorded network traffic where any exists, inline fixtures
  only where no recording exists.
- Behaviour that lives in the shell (`main.cpp`: timers, the reg-event
  subscription schedule, the audio-session resolution, the hand-over scripts)
  is proven on a phone where a bench can reach it; the commit message says
  in one sentence that it was tested, and the arms no bench has reached are
  named here. The known gaps: the reg-event subscription schedule (the
  arming and KPN's
  `noresource` closure have run on hardware, the 60 s retry only on a
  pre-fix build that renewed that closure; the retry on this code, the
  renewal firing, Timer F, the doubling, a SUBSCRIBE `Retry-After`, the
  renew arm and the back-off's survival across a refresh have run nowhere —
  §3, §7.4); a `Retry-After` on a REGISTER has run in the field (one
  report, a screenshot of 2026-10-08, O2, 0.3.10: a refresh's Retry-After
  set the next step's time — `in 7 min`, then `in 13 min` — and the
  refusals counted; the wait's length went unmeasured, the journal lines
  on the screenshot carry no timestamps; KPN's refusals, all 403, carry
  none) and a `Dial` bringing a recovery step
  forward (imsd-ofonod refuses the `Dial` itself and imsd-dialerd forwards
  it) is pinned in `tests/Engine` only; a 401 to a reregistration (the
  refreshes KPN accepted were never challenged — consecutive CSeqs on every
  bench — and O2 has not run this shape) and
  the escalation by one refused refresh that carries a `Retry-After` — the
  escalation to a fresh registration is pinned in `tests/Engine` only; a
  refused initial or fresh
  REGISTER (403, 5xx) at bring-up or in recovery (KPN has only throttled or
  stayed silent — pinned in `tests/Engine` only); the write-off of the old
  registration by a recovery's fresh attempt that fails past the SA setup
  (such attempts are on record only as a bring-up's fatal exit, which does
  not pass that arm — pinned in `tests/Engine` only); the escalation by
  two refused refreshes that carry no `Retry-After` (KPN refuses a refresh
  only with the 403 that says the registration is gone — pinned in
  `tests/Engine` only); of the throttled-attempt chain in recovery (a
  throttled fresh attempt, the old SA retried once meanwhile with the stored
  binding's Contact and refused 403, the deferred attempt accepted at the
  hold's end — run 2026-10-03 on the series as it then stood) the deferred
  attempt made by the
  same process, since a restart inside the hold made it from the holds file
  instead (the same process made it on 2026-09-24, on the build before the
  policy's rework); the address moves
  writing `old_gone` to the holds file (the move benches predate that
  field); a SUBSCRIBE final read inside a REGISTER wait (KPN answers within
  the second); the shell's handling of a NOTIFY that terminates our own
  binding (§3; every captured termination is a previous contact's O2
  `deactivated`, and a deregistration during a call cannot be staged);
  `SipFlow::Connect`'s errno arms — socket() failing, a UDP connect()
  error, a bind() refused while the address is still on the link — which
  no bench reaches without fault injection (§3); a coverage gap on the air
  (no bench stages a radio gap without an RF shield) — the packet-service
  read's rule has run through a fake `mmcli` only (2026-10-03: `searching`
  and `detached` not counted, `connecting` with `unknown` counted), its
  other states, ModemManager not answering and its second site (a refresh
  unanswered over a reconnected flow) not at all, and `tests/Engine` pin
  nothing of it; the REGISTER wait's diverting arms — a datagram skipped on
  the init port, a response to another transaction ignored, a provisional
  restarting the wait, a request (an INVITE among them) dispatched and
  answered under the wait; an incoming call answered from a session's
  dialer since the polkit gate — imsd-dialerd's `Accept` by the active
  session, or GNOME Calls' `Answer` through imsd-ofonod's gate (imsd's own
  `Accept` gate has run: refused for an account without a seat session and
  for nobody, accepted for root, 2026-10-04); a refused
  final carrying reason headers (KPN's 403s carry none — the rendering is pinned
  in `tests/Sip` only); an `AUDIO_USER` pin in imsd's own environment that
  names no account (the logind fallback) or an account without a PipeWire
  socket (a silent call) — a pin naming an account with one ran, 2026-10-04.
  `DetectLocal`'s three-tier address ranking for a run without `LOCAL`
  (§10.1) ran through a fake `ip` (2026-10-05: the phone's listing of the
  netdev and two cut-down ones).
- `imsd-media --selftest` round-trips the RFC 4867 payload packing (both
  formats, both AMR codecs) and the G.711 tables. `MIC_SRC=<file>` (raw s16
  PCM at the codec rate, paced in real time) and `PCM_DUMP=1` are test seams
  for driving the leg against a synthetic RTP peer without a phone.
- `imsd --session` and `imsd-ofonod --session` own their names on the session
  bus for development without root, polkit or a phone (§8.3).

## 6. Code style and lint

The rules are the Catcrafts house style,
[Crafter.LintRules](https://forgejo.catcrafts.net/Catcrafts/Crafter.LintRules),
pulled in by `project.cpp` (§4.2); that repository's README lists the
seventeen of them. `// lint-disable-file <rule>` (by convention on line 4,
after the SPDX header and a blank line) and `// lint-disable-next-line <rule>`
suppress a rule where a file needs it; nearly every source file disables
`fixed-width-types`.

The tree was written under an earlier, project-local copy of the rules and
does not pass the shared ones yet: `crafter-build lint` reports 72 findings
(2026-10-06) — `naming` 44 (constants and statics are PascalCase here, the
shared rule wants camelCase), `const-local` 10 (a rule the copy did not
have), `single-declaration` 2 (not reported under the copy), `format-concat`
13 and `wrap-join` 3 (as under the copy). CI does not run lint.

Beyond the rules:

- Comments explain constraints the code cannot show — a network's observed
  behaviour, a spec clause, the reason a shape was chosen — and name their
  evidence (a dated bench, a capture) when they rest on one. They do not
  narrate what the code does.
- Log lines are the operator's interface (§7.8). A new state the daemon can
  be in gets a journal line that names it; existing lines are grepped for by
  scripts (`imsd-handover.sh` reads a pre-0.3.10 daemon's `bring-up deferred: …
  next fresh registration in N min`), so changing one is an interface change.
- GLib only in `main.cpp`, `dialerd.cpp`, `ofonod.cpp` and `ImsdBus.cppm`.
  `media.cpp` is std + POSIX. The partitions are std only.

## 7. How the daemon works

### 7.1 Processes and buses

`imsd` runs as root on the system bus as `net.catcrafts.IMS1` (object
`/net/catcrafts/IMS1`, interface `net.catcrafts.IMS1`; the frozen ABI, §8).
The bus policy `packaging/net.catcrafts.IMS1.conf` lets any local account
reach it; placing, answering and ending a call is polkit's
`net.catcrafts.IMS1.Voice` (`packaging/net.catcrafts.IMS1.policy`, shaped like
ModemManager's Voice action: the active local session and root — not an ssh
login, an inactive session or a system service account). Reads (`GetStatus`,
`GetCalls`) stay open, as ModemManager's are to every local account: the call
signals reach them all anyway, the status (the address, the P-CSCF, the
phone number) travels in no signal.

Two threads: a dedicated engine thread runs the SIP loop (blocking framed
receive on the protected client flow, the protected-port listeners for
terminating requests, media reaping, the keepalive refresh); the GLib main
loop owns the bus. D-Bus method calls push commands to the engine queue;
engine events reach the bus through `g_idle_add`, where the calls table and
the status snapshot live (main thread only).

Per answered call imsd forks `imsd-media` (`IMSD_MEDIA`, default next to the
`imsd` binary, else `/usr/libexec/imsd-media`) with the addresses, ports and
payload type as arguments and the codec and audio knobs in its environment
(§7.6, §10.2).

The desktop daemons are translation layers: `imsd-dialerd` runs as the
session user (autostarted from `/etc/xdg/autostart/imsd-dialerd.desktop` in
place of plasma-dialer's `modem-daemon`, whose autostart the package hides
through `/etc/skel/.config/autostart/org.kde.modem.daemon.desktop`);
`imsd-ofonod` runs as root on the system bus (`imsd-ofonod.service`,
`Conflicts=ofono.service`). Both consume the frozen ABI and add nothing to it.

### 7.2 Bring-up

`imsd.service` runs two `ExecStartPre=` steps before the daemon:

1. `/usr/libexec/ims-pdn-up.sh` brings the `ims` PDN up through ModemManager
   (§7.3) and writes `/run/imsd.env` with `DEV=` (the connected netdev) and
   `LOCAL=` (the PDN address ModemManager assigned).
2. `/usr/libexec/imsd-handover.sh` (`ExecStartPre=-`, failure-tolerant) hands
   a pre-0.3.10 daemon's throttle wait over (§7.5).

The unit then reads `EnvironmentFile=-/run/imsd.env` and
`EnvironmentFile=-/etc/imsd.env` (the admin's file wins) and starts
`/usr/bin/imsd`. systemd reads those files anew for every command it spawns,
so each `ExecStartPre=` already sees the values the previous one wrote.
`TimeoutStartSec=900` covers the bring-up's worst case (a 2-minute modem wait,
a 5-minute registration gate, connect retries). `Restart=on-failure`,
`RestartSec=120`; `KillMode=mixed` with `TimeoutStopSec=10`; `StateDirectory=imsd`
(`/var/lib/imsd`).

The daemon's own bring-up (`BringUp()` in `main.cpp`):

1. `PCSCF` must be set (no P-CSCF discovery from the PDN's PCO yet).
2. `qmicli -d qrtr://0 --uim-get-card-status` finds the ready USIM (slot, AID);
   `mmcli -m any` gives the primary SIM path and the IMEI (for the
   `+sip.instance` URN); `mmcli -i <sim>` gives IMSI and MCC/MNC, from which
   the IMPI/IMPU/realm are derived. `mmcli --location-get` fills
   `P-Access-Network-Info`.
3. **Warm resume** when an SA pair is already in the kernel and the state
   file (`STATE_FILE`, default `/var/lib/imsd/imsreg.state`) describes the
   registration: reconnect through the existing SA and send a refresh
   re-REGISTER (`RESUME via existing SA`, then `REGISTERED (resumed +
   true-refreshed)`). This avoids the network's fresh-SA throttle. `RESUME=0`
   forces a fresh registration. A resume the network refuses is not fatal: the
   fresh registration replaces its SAs.
4. **Fresh registration** otherwise (`FRESH registration`): an unprotected
   REGISTER on `PCSCF_PORT` (UDP, always), the 401 with the AKA challenge and
   the `Security-Server` list, the AKA run over a UIM logical channel
   (`--uim-open-logical-channel`, `--uim-send-apdu`), the SA pair installed
   with `ip xfrm`, the protected REGISTER over the negotiated transport
   (§10.1, `SIP_TRANSPORT`), `REGISTERED (fresh)`. Then the reg-event
   SUBSCRIBE (§7.4).
5. A keepalive re-REGISTER refresh at half the granted lifetime, within
   120–1800 s (`REFRESH_INTERVAL`), only while idle. The refresh of a
   registration this process made fresh is a reregistration as TS 24.229
   5.1.1.4.2 has it: the same Call-ID and Contact, CSeq+1, the
   Authorization of the last successful authentication verbatim (the 401's
   nonce, our cnonce and response, kept in the state file), a
   Security-Client with new SPIs and a new client port (`PortUcNext`,
   45063) for the SA pairs a re-authentication would set up — the server
   port stays (TS 33.203 7.4.0) — and the Security-Verify (the 401's list).
   A registration resumed from the kernel's SAs sends the same
   Authorization and Security-Client, but its Security-Verify — on the
   resume's re-REGISTER and on every keepalive after it, until its next
   fresh registration — is the one entry rebuilt from the SA
   (`Imsd:Ipsec`), not the 401's list, as before 0.3.11. A 401 to a
   reregistration asks for re-authentication over temporary SAs on the
   proposed ports, which imsd does not implement: it is a refusal and the
   recovery is a fresh registration (§7.4). Every refresh of the pre-0.3.11
   shape on record from O2 UK (an initial registration's empty digest, the
   in-use SPIs) was refused with 503 (§13).

The state file is JSON: `callid`, `ftag`, `cseq`, `spi_uc`, `spi_us`,
`expiry`, `route`, `ppi`, `aor`, `contact_user`, `transport`, and since
0.3.11 `auth_nonce`, `auth_cnonce`, `auth_response` (a registration resumed
from an older file has none: its reregistrations — the resume's and the
keepalives', across restarts — carry the empty digest, as before 0.3.11,
until its next fresh registration, and each such resume logs it).

A failed fresh bring-up exits non-zero and systemd retries after
`RestartSec` — except a bring-up the network refused or throttled, which
waits in process on the refusal schedule (§7.4) with nothing to reconnect
over, then tries again (`bring-up deferred: …`). A registered daemon whose
address leaves the netdev with no PDN address there to move to also exits
(`engine fatal: … is no longer on qmapmux0.0 — exiting for systemd
restart`) — not at once: the loss is
noticed at the next recovery step, the refresh (every 1800 s on KPN's
7200 s grant), whose
re-REGISTER gets no reply and whose reconnect cannot bind the address (dev
phone 2026-10-02, the ims bearer disconnected under a registered daemon:
14 min from the disconnect to the exit). After `RestartSec` (120 s)
`ims-pdn-up.sh` runs again and imsd registers on the PDN's address (the
same bench: a new prefix, `REGISTERED (fresh)` six seconds into the new
start).

### 7.3 The ims PDN (`packaging/ims-pdn-up.sh`)

Finds or creates the `ims` bearer through ModemManager, connects it and
configures the muxed netdev. Idempotent; a non-zero exit fails the unit.

- Waits up to 120 s for a modem, then gates the connect attempts on network
  registration (`IMS_REG_TIMEOUT`, default 300 s): some carriers attach ~2 min
  after boot.
- Never creates or connects a bearer while the modem is `disabled` — its
  radio is off (airplane mode): a modem still disabled after the wait, or
  disabled when an attempt is due, makes the script exit non-zero (`modem …
  disabled … (radio off): not connecting`) and systemd retries. A bearer
  connect on a disabled modem fails with `no-service` and leaves
  ModemManager's state `enabled` with the modem in low power; switching the
  radio back on then no longer brings it up (dev phone 2026-10-06: not
  registered after 150 s, against 5 s without the attempt; a ModemManager
  disable/enable recovered it).
- Connects by **3GPP profile index** (`IMS_PROFILE_ID`, default: looked up by
  APN in `qmicli --wds-get-profile-list=3gpp`), not by APN string: a
  profile-indexed request is exempt from the modem's attach-family forcing
  that refuses an APN-string IPv6 request when the attach PDN was granted
  IPv4-only (Odido, O2 UK, Swisscom). An empty profile list is the modem still
  coming up; a list without the APN is final only once the modem is
  registered (the list can be partial while it enables). A leftover
  APN-string bearer is never reused and is deleted when connecting by index.
- Sets `accept_ra_defrtr=0` on the netdev before link-up: the PDN's router
  advertisement must never make the ims PDN the phone's IPv6 default route (on
  IPv4-only wifi every AAAA-first client would send its SYNs into the IMS
  walled garden and hang). The RA's prefix and MTU still apply. An RA default
  route already present (an imsd restart) is flushed.
- Sets the PDN MTU ModemManager reports on the link (1280 on the FP6), adds
  the address, waits out DAD, and installs a host route to `PCSCF`
  (`dev <netdev>`, no gateway — a raw-IP netdev without header_ops). The PDN
  is IPv6-only, so `PCSCF` must be an IPv6 address; anything else on the IMS
  network (its DNS servers, a traceroute target) needs its own host route
  (`ip -6 route add <addr> dev qmapmux0.0`). imsd installs a host route per
  media peer itself.
- Opens imsd's protected ports 45061–45062 (TCP and UDP) on `qmapmux*` in
  nftables when a `inet filter` table exists (pmOS's default INPUT chain drops
  inbound on `qmapmux*`, which silently killed every network-initiated
  request after ESP decapsulation). Best-effort.
- Writes `/run/imsd.env`: `DEV=<netdev>`, `LOCAL=<PDN address>`. imsd
  registers on the address the network assigned, not on the kernel's
  temporary SLAAC address from the RA, which the netdev lists first and
  retires after about two days (the SAs and the Contact would go with it).

Its own settings (read from the environment, else from `/etc/imsd.env` for
manual runs): `IMS_APN` (default `ims`), `IMS_IP_TYPE` (`ipv6`),
`IMS_PROFILE_ID` (empty = look up; `none` = connect by APN string),
`IMS_REG_TIMEOUT` (300), `PCSCF`.

### 7.4 Registration recovery (`RegRecovery`, `Imsd-Engine.cppm`)

What the daemon does when the registration stops working after bring-up, as
a pure reducer the shell feeds with the clock and the network's answers. Two
kinds of failure reach it: a **refusal** (the network answered a refresh with
a final other than 200 — a 401, re-authentication, included — or a fresh
registration's REGISTER with a final other than the expected one: from 300
up on the unprotected one, whose 401 is the challenge, anything but 200 on
the protected one) and **silence** (the flow died
and reconnecting through the SA, or the refresh over it, got no answer).
Neither is retried for long on the same registration (TS 24.229 5.1.1.4 and
RFC 5626 have the UE register anew). Before this reducer existed the daemon
looped reconnect + re-REGISTER on one Call-ID and SA forever (O2 UK,
2026-09-24: 21 × 503 over 70 min, then 33 unanswered reconnect cycles over
2 h 39 min).

The policy, with its constants:

| Step | Rule |
|---|---|
| Reconnect | Through the existing SA, then refresh. Back-off 5 s → 5 min (`ReconnectDelayMin`/`Max`). |
| Escalate to a fresh registration | After one refused refresh whose `Retry-After` is at least a second (the network's considered answer; a `Retry-After` of 0 or one that does not parse counts as none; O2 2026-10-08: the refresh refused 503 with Retry-After 414 s, the refresh after that wait 503 with Retry-After 772 s) or a 401 (re-authentication, which imsd answers with a fresh registration rather than over temporary SAs), after two refused refreshes without one (`RefusalsBeforeFresh`), or two reconnect cycles that end without an answer (`SilentCyclesBeforeFresh`) while the modem reports packet service (`PacketServiceUp`: ModemManager's registration and packet-service state, as `ims-pdn-up.sh` reads them). A cycle that fails during a coverage gap is the phone's silence, not the network's: not counted, retried on the local-refusal pace, so a radio gap across a refresh does not escalate into a fresh registration, which KPN refuses while the registration is live and drops it for (§13). New Call-ID, new challenge, new SA pair (`registering fresh (recovery attempt N)`). |
| A refused or throttled fresh attempt | Waits on the **throttle schedule**: 21 min, growing ×1.5 to 60 (`ThrottleMinutesMin`/`Max`). KPN refuses a fresh SA pair on an address whose registration is live (§13), and drops that registration the moment an initial REGISTER arrives. |
| A silent fresh attempt | Backs off 30 s → 10 min (`FreshDelayMin`/`Max`). |
| `Retry-After` | Always wins over a shorter wait, honoured up to an hour (`RetryAfterMax`, the schedule's longest wait); a longer one is cut to an hour and the refusal heard again then. On a refused fresh attempt it replaces the schedule's wait, shorter or longer, never below 30 s (`FreshDelayMin`). |
| A 403 | A refusal like the others: never hammered (the schedule), never abandoned (a SIM that is unbarred registers within the hour). |
| The old registration is gone | A 403 once a fresh registration is due, a fresh attempt that got past the SA setup, a binding the network terminated (reg-event), an older address that left the netdev, or a bring-up whose resume failed: no reconnect over the old SA from then on. Otherwise the existing SA is still retried on the reconnect back-off while a fresh registration is deferred. |
| A `Dial` while unregistered | Brings the next recovery step forward (the reconnect, or the fresh attempt once it is due) — never a wait the network imposed (a `Retry-After`, or the schedule after a refused or throttled fresh attempt). imsd-ofonod refuses a `Dial` then and imsd-dialerd lists no modem. |
| Success | Resets everything. |

The network's waits are **holds**, and a restart of imsd (an upgrade restarts
it) does not cut them short: `PersistHolds` writes them to the holds file
(`HOLDS_FILE`, default `/run/imsd.holds`) as JSON — `pdn` (the PDN's own
address), `boot` (the boot id), `written_at` (seconds of uptime), `hold`,
`fresh_hold`, `throttle_minutes` (the schedule's place), `old_gone` (the old
registration is gone: a 403 once a fresh registration is due, an attempt past
the SA setup, a binding the network terminated, an older address that left
the netdev, and every bring-up that goes on to register fresh — its resume
failed or there was nothing to resume) — and `LoadHolds` takes them
over at the next start for the same `pdn` and boot id, waiting them out
before the resume (a refresh's `Retry-After`) or before the fresh
registration (a fresh attempt's schedule or `Retry-After`, a terminated
binding's wait); with `old_gone` set the start does not resume over the old SAs at
all, it waits the fresh hold out and registers fresh — also after a start
during an outage, whose resume and fresh attempt both fail without an answer:
the next start then registers fresh while the network still holds the old
registration, which KPN refuses (§13; dev phone 2026-10-04: 21 min where
0.3.9's retried resume would have recovered at once). A file without the key
reads as not gone; the 0.3.9 hand-over (`imsd-handover.sh`) writes the key as
true, since the wait it hands over follows a throttled initial REGISTER, at
which KPN drops the old registration (§13). A reboot or another PDN address
starts clean. The schedule's place is kept
even after the holds run out, so a restart does not start it over at 21 min.
When nothing is held the file is removed — when it is a regular file;
`/dev/null` switches the knob off and anything that is not a regular file is
emptied, never unlinked. The recovery policy's waits and the holds are
measured on the boot clock (`CLOCK_BOOTTIME`), so a suspend counts; the
keepalive refresh gate, the CRLF keepalive and the reg-event retry and
renewal timers run on the steady clock and count awake seconds only.

The **reg-event subscription** (RFC 3680; the network's own view of the
binding) is one live dialog per registration, subscribed anew ahead of the
lifetime the S-CSCF granted. Refused or unanswered, it is retried after 60 s
doubling to 10 min (a `Retry-After` honoured up to an hour; a registration
refresh neither brings a scheduled retry forward nor starts its back-off
over — the back-off resets when a subscription starts afresh or is
accepted). When the S-CSCF ends it
for a reason that allows renewal (RFC 6665: `deactivated`, `timeout`,
`giveup`, `probation`) it is renewed; after any other reason — `noresource`,
`rejected`, a reason-less end — the next registration refresh subscribes anew. An
earlier subscription's NOTIFY is still read for the binding's state, but its
end is not ours to renew (KPN closes a registration's previous subscription
with `noresource` when a new one replaces it). A NOTIFY that **terminates the
binding** triggers a fresh registration — at once, or after the network's
`retry-after` (up to an hour); after the call, when one is up (one exception:
a REGISTER wait that still gets its 200 after such a NOTIFY, a call having
kept the flow, leaves that registration standing). After
`rejected` and `unregistered` it waits on the refusal schedule, a wait no
`Dial` cuts short: the binding is gone, so nothing is retried meanwhile. One
termination counts once, however many NOTIFYs report it (a repeated
`probation` report may extend its hold from its own `retry-after`). The schedule is
shell code in `main.cpp`, not a reducer (§3); its hardware coverage per arm
is in §5.

A registration resumed on an older address (a pre-0.3.10 temporary one)
moves to the PDN's own address in place: with its next fresh registration, or
when the older address leaves the netdev first, which takes that registration
with it.

### 7.5 Upgrades and the 0.3.9 hand-over

An apk upgrade (or an `apk add` of a package whose daemon an `apk del` left
running) restarts `imsd` and `imsd-ofonod` once the transaction is done: the
packages' `post-upgrade`/`post-install` scripts mark the units
`needs-restart` (`systemctl set-property … Markers=+needs-restart`, after a
`daemon-reload` when the unit is not-found because its file was just
reinstalled) and pmOS's systemd trigger runs `enqueue-marked`. A call in
progress ends. The restart resumes the registration over its existing SAs,
or goes on waiting out a hold the network had set (§7.4). A stopped unit
keeps no marker and `enqueue-marked` leaves it stopped (dev phone, systemd
262~rc3, 2026-10-03).

A daemon older than 0.3.10 kept its bring-up throttle wait in memory only, so
the restart registered fresh at once instead of waiting the schedule out.
For that one hop: `imsd.post-upgrade` records the running
daemon's `InvocationID` in `/run/imsd.handover` (when no holds file exists);
at the next start `imsd-handover.sh` reads that finished invocation's last
bring-up line from the journal and, when it is the throttle's (`bring-up
deferred: fresh-SA throttle active (spi-s=0) — … next fresh registration in
N min`; the old daemon's other deferrals, after a 401 it could not use, are
not handed over), ages it on the monotonic clock (`__MONOTONIC_TIMESTAMP` against
`/proc/timer_list`; the phone's wall clock can read 1970 until its first NTP
sync, and the monotonic clock stops in a suspend, so the wait can only come
out long, never short of the network's window), and writes what is left to
the holds file as 0.3.10 keeps it — for the address this start registers on
when that is the address the wait was earned on (the old daemon logged its
`LOCAL=` once at start); on another address — the normal hop, 0.3.9 on the
kernel's temporary address and 0.3.10 on the PDN's own, which holds no
registration (§13) — only the verdict that the old registration is gone is
written and the new start registers fresh at once (dev phone 2026-10-03:
accepted, registered 16 s after 0.3.9's refused attempt) —
`LOCAL` as systemd hands it to the unit's commands (the `/etc/imsd.env` pin
if there is one, else what `ims-pdn-up.sh` just wrote), unquoted the way imsd
itself gets it. Reading at the start rather than at the upgrade means a wait
that ended in between — the old daemon making another attempt — is
read as it ended. When the old address cannot be read the wait is handed
over anyway; when the wait cannot be timed, or this start has no `LOCAL`, the
journal says it was not handed over.

### 7.6 Calls and the media plane

`CallMachine` owns one call's SIP dialog, outgoing or incoming, and never
touches a socket, a clock or a subprocess: each event returns `Action`s for
the shell (send this on the client flow, answer that request on the channel
it came in on, start/stop the media leg, emit this D-Bus state change, arm
this deadline). States and reasons are the frozen D-Bus vocabulary:
`dialing` / `ringing` / `incoming` / `active` / `terminated`, with reasons
`outgoing` / `incoming` / `accepted` / `local-hangup` / `remote-hangup` /
`refused-or-busy` / `error`.

- Outgoing: INVITE with the SDP offer (AMR-WB + AMR + telephone-event by
  default; `CODECS` restricts and orders; `PRECOND=1` adds IR.92
  preconditions), reliable 18x with PRACK, 2xx ACK and media start, CANCEL
  before answer, BYE after. Busy and error finals end the call with
  `refused-or-busy` or `error`.
- Incoming: an INVITE on a protected-port listener is validated before it is
  announced to the UI; 100 and 180 (reliable when the caller requires
  100rel); `Accept` sends the 200 with the SDP answer at the offer's own
  payload types and octet-align mode; a remote CANCEL gets 200 + 487; an
  unusable offer 488 naming what was offered; ring timeout 480. A landline
  gateway's G.711-only or AMR-NB offer rings and is answered at its own
  payload types.
- Emergency: a number `IsEmergency` classifies (112, 911, plus
  `EMERGENCY_NUMBERS`; SIM `EF_ECC` is not read yet) is tried as
  `urn:service:sos` first, with one fallback to a plain INVITE of the digits;
  a plain INVITE answered `380 Alternative Service` with an emergency body is
  re-attempted as `urn:service:sos`.
- `SendDtmf` is a stub (RFC 4733 not implemented).
- Incoming calls reach imsd only when the network's T-ADS routes the call to
  the IMS domain; the REGISTER Contact advertises the MMTEL ICSI and
  `+sip.instance` for that. When T-ADS still picks CS, no INVITE arrives —
  a network/modem-integration matter, not a daemon defect.

`imsd-media` is one process per call: `imsd-media <local-ip> <rtp-port>
<remote-ip> <remote-port> <pt> <secs> <out-base>`. It binds the advertised
RTP port (`RTP_PORT`, default 50004), sends uplink frames from `pw-record`
through the encoder, captures the downlink, and with `PLAY=1` reconstructs
the media clock from RTP timestamps so `pw-play` stays real-time-paced
through far-end DTX silence. Codecs: AMR-WB (16 kHz, the default), AMR
narrowband and G.711 PCMA/PCMU (8 kHz, what a PSTN gateway offers); AMR
frames ride RFC 4867 payloads, octet-aligned or bandwidth-efficient
(`OCTET_ALIGN`, mirrored from the SDP). The AMR codecs are dlopen'd
(`libvo-amrwbenc` + `libopencore-amrwb` for WB, `libopencore-amrnb` for NB);
G.711 is a table. **Exit code 3 means the downlink dried up** for
`MEDIA_TIMEOUT` seconds (default 6.0) — the far-end-hangup signal, which on
carriers whose BYE never reaches our SAs is the reliable teardown trigger.
Per-call stats land in `OUT_DIR` (default the state dir); `RTP_DUMP=1`
captures the raw downlink next to them.

**Audio session.** Call audio goes through the PipeWire of one account.
imsd resolves it for each call from logind: the user session active on
seat0 (`loginctl show-seat seat0`; a greeter does not count) when it has a
`/run/user/<uid>/pipewire-0` socket, and passes it to `imsd-media` as
`AUDIO_USER`. `AUDIO_USER` set in imsd's own environment pins an account
instead; one that is not an account falls back to logind. imsd as packaged
(root, on the system bus) logs the resolution at start and when a call's
media starts (`audio session: …`), `imsd-media` once per call. An `imsd-media` started without the variable at all (by hand, or
by an imsd older than 0.3.10 while an upgrade replaces it) tries the
account `user`, its old default.

### 7.7 Runtime files

| Path | Written by | Content |
|---|---|---|
| `/etc/imsd.env` | the admin | configuration (§10); `PCSCF=` is required. The apk adds a drop-in with `ConditionPathExists=/etc/imsd.env` (§9.2), so an unconfigured phone does not boot into a failing unit |
| `/run/imsd.env` | `ims-pdn-up.sh` | `DEV=`, `LOCAL=` of the connected PDN |
| `/run/imsd.holds` (`HOLDS_FILE`) | imsd, `imsd-handover.sh` | the network's holds and the schedule's place (§7.4) |
| `/run/imsd.handover` | `imsd.post-upgrade` | the InvocationID of the daemon an upgrade replaces, written when no holds file exists (§7.5); consumed by `imsd-handover.sh` |
| `/run/imsd-ofonod.started` | `imsd-ofono-restart-calls.sh` | marker: imsd-ofonod has started once since boot |
| `/var/lib/imsd/imsreg.state` (`STATE_FILE`) | imsd | the registration context for a warm resume |
| `/var/lib/imsd/*.stats`, `*.rtp`, `*.raw` | imsd, imsd-media | per-call media stats, raw RTP (`RTP_DUMP`), SIP dumps (`DUMP_SIP`; mode 0600 — they carry the IMSI/MSISDN) |
| `$XDG_STATE_HOME/imsd/` (else `~/.local/state/imsd/`), `$XDG_RUNTIME_DIR/imsd.holds` (else `/tmp/imsd.holds`) | imsd | the same files for a non-root (`--session`) run |

### 7.8 Journal vocabulary

Lines the operator (and the scripts) look for, all prefixed `imsd: `:

- `LOCAL=… P-CSCF=…`, `SIP transport: …`, `SIM slot=… IMSI=… domain=…` at start.
- `RESUME via existing SA (cseq N)`, `REGISTERED (resumed + true-refreshed)`,
  `FRESH registration`, `Security-Server: …`, `REGISTERED (fresh)`.
- `bring-up deferred: …; next: fresh registration in N s` — one line; `in N
  min` from a minute (the pre-0.3.10 form `bring-up deferred: … next fresh
  registration in N min` is what `imsd-handover.sh` reads from an older
  daemon's journal), `a hold the network set before this start stands (…)`,
  `the refusal schedule stands at N min from before this start: …`.
- `…; next: fresh registration in N s` (`in N min` from a minute, `now`),
  `registering fresh (recovery attempt N)`,
  `fresh registration deferred/refused/failed: …`, `reconnected + re-registered`,
  `recovered by a fresh registration`.
- `the registration's SAs are on X, not the PDN's own address Y: resuming
  there until the next fresh registration`, `the fresh registration moves
  from X to the PDN's own address Y`, `back on X: the old registration stands
  until the next fresh attempt`, `X has left DEV: the registration on it is
  gone; staying on Y; next: …`, `X is no longer on DEV: the registration on
  it is gone; on the PDN's own address Y now; next: …`.
- `engine fatal: … — exiting for systemd restart`.
- `reg-event SUBSCRIBE sent`, and the binding's state from NOTIFY.
- `audio session: …` at start and at each call's media start; `CODECS override active: …`.
- A refused REGISTER or SUBSCRIBE is logged with its status line and
  any `Retry-After` / `Warning` / `Reason` / `Server` headers;
  `re-REGISTER challenged: 401 …; re-authentication asked, the next
  registration is a fresh one` for a challenged refresh; `state file holds
  no challenge answer (pre-0.3.11): the refresh carries an empty digest`
  at a resume of a registration made before 0.3.11 — every reregistration
  of it carries the empty digest until its next fresh registration, and
  the line repeats at each later resume of it.
- `connect: no answer from the P-CSCF N times (…)` when the protected
  connect series ends unanswered.

From the scripts: `ims-pdn-up: …`, `imsd-handover: …`,
`imsd-ofono-restart-calls: …`.

## 8. D-Bus ABI and desktop integration

### 8.1 `net.catcrafts.IMS1` (frozen)

Object `/net/catcrafts/IMS1`, interface `net.catcrafts.IMS1`. The ABI is
frozen — the two translation daemons depend on it, and a new desktop needs a
new translation daemon, not a change here.

| Member | Signature |
|---|---|
| `Dial(number)` | `s → s` (callUni) |
| `HangUp(callUni)` | `s` |
| `Accept(callUni)` | `s` |
| `SendDtmf(callUni, tones)` | `ss` |
| `GetCalls()` | `→ aa{sv}` |
| `GetStatus()` | `→ a{sv}`: `registered` (b), `local` (s), `pcscf` (s), `expiry` (x), `cseq` (x), `ppi` (s) |
| `CallAdded` | `s a{sv}` |
| `CallStateChanged` | `s s s` (callUni, state, reason) |
| `CallDeleted` | `s` |
| `RegistrationChanged` | `b` |

`Dial`, `HangUp`, `Accept` and `SendDtmf` are gated by polkit (§7.1); a
refused caller gets `org.freedesktop.DBus.Error.AccessDenied`, and polkit not
answering is logged with the sender. A translation daemon that runs as root
on the system bus must gate its own callers through `imsd-bus`, or it is a
bypass of `net.catcrafts.IMS1.Voice`.

### 8.2 Plasma Mobile (`imsd-dialerd`)

A drop-in for plasma-dialer's `modem-daemon`, owning the same session-bus
names and objects (`org.kde.telephony.CallUtils` at
`/org/kde/telephony/CallUtils/tel/mm`, likewise `DeviceUtils` and
`UssdUtils`; the `/tel/mm` suffix is hard-coded in plasma-dialer's QML plugin
and kde-telephony-daemon — keep it). plasma-dialer, kde-telephony-daemon and
spacebar run unmodified. ABI details verified against plasma-dialer 6.7.3 are
in the header of `implementations/dialerd.cpp` (`CallData` marshalled as a
struct wrapping `a{sv}`, struct-wrapped int32 enums in MMCallState numbering,
an empty callUni meaning the active call, `callsChanged` with the full vector
on every add/delete). It lists no modem while imsd is unregistered.

### 8.3 GNOME Calls, Phosh, GNOME Mobile (`imsd-ofonod`)

Presents imsd as an oFono modem: `org.ofono` with a `Manager` at `/`, one
`Modem` + `VoiceCallManager` at `/imsd`, and a `VoiceCall` object per live
call — what GNOME Calls' bundled `ofono` provider plugin drives; Calls, Phosh
and GNOME Shell run unmodified. `org.ofono` is the seam because it is the
only one a third party can implement: `org.gnome.Calls.Call` is what Calls
*exports* so a shell can observe and control a call that already exists
(Accept, Hangup, SendDtmf, Silence — no Dial), and Phosh consumes it from
Calls — its call UI needs nothing extra once Calls has a working origin;
Calls' own backends are libpeas provider plugins (`mm`, `ofono`, `sip`,
`dummy`) against private headers no distro installs.
Consumer-shaped details verified against calls 50.0 are in the header of
`implementations/ofonod.cpp`. Verified on a Fairphone 6 (postmarketOS,
dbus-broker) with GNOME Calls 50.0: outgoing and incoming calls with audio
both ways, answered and hung up through `org.gnome.Calls.Call` — the surface
Phosh drives. The answered calls predate the polkit gate (§5).

Setup: `apk add imsd-ofono` enables `imsd-ofonod.service` by preset and ships
a gschema override setting `org.gnome.Calls autoload-plugins` to `['ofono']`
(the per-user equivalent is `gsettings set org.gnome.Calls autoload-plugins
"['ofono']"`). Drop `mm` rather than adding `ofono` alongside it: the
ModemManager provider would offer the modem's CS voice path as a second
origin, and on a phone whose voice path is imsd that origin carries no audio.
Never run it beside a real `ofonod` (one name, one owner;
the unit has `Conflicts=ofono.service` and the daemon exits if it loses the
name). Running `imsd-dialerd` at the same time is fine (different bus,
different consumer). One dbus-broker
detail is load-bearing and lives in `packaging/org.ofono.conf`: dbus-broker
checks a broadcast against the sender's send policy for every name the
receiver owns, so root (imsd) must be allowed `send_destination="org.ofono"`
or imsd's call signals never reach the daemon.

Known gaps, all inherited rather than introduced:

- USSD is refused with `org.ofono.Error.NotSupported` and not advertised in
  the modem's `Interfaces`: the IMS stack has no USSD path.
- No hold, multiparty, transfer or call waiting (`org.ofono.Error.NotImplemented`):
  imsd is a single-call stack.
- Per-call caller-ID withholding is refused rather than silently ignored:
  imsd cannot honour a CLIR request, and failing is safer than dialling with
  the caller ID exposed.
- Phosh's emergency-call screen lists nothing: `imsd-ofonod` publishes
  `org.ofono.VoiceCallManager.EmergencyNumbers` (112, 911, plus
  `EMERGENCY_NUMBERS`), oFono's standard place for it, but Calls' ofono
  provider never reads it: it makes no `GetProperties` call on the VoiceCallManager
  and returns NULL for the origin's `emergency-numbers`, which is what
  `org.gnome.Calls.EmergencyCalls.GetEmergencyContacts` is built from (the
  ModemManager provider fills its equivalent via
  `mm_sim_dup_emergency_numbers()`). An upstream fix would close this.
  Dialling an emergency number from the dialpad is unaffected — that reaches
  imsd's own classifier.
- DTMF is forwarded to imsd, where `SendDtmf` is a stub — the same
  limitation Plasma has.
- Restarting `imsd-ofonod` restarts GNOME Calls with it: when the modem's
  `VoiceCallManager` interface goes away and comes back, Calls 50.0 adds a
  new origin but leaves the old origin's `CallAdded` handler connected, so
  after one such cycle every call is added twice, and after a few the stale handler runs on
  freed memory and Calls dies with SIGBUS in `g_hash_table_lookup` (measured
  on the FP6). Calls-side; an upstream fix belongs in
  `plugins/provider/ofono/calls-ofono-provider.c`. To keep imsd's own
  restarts and registration drops from triggering it, the
  `VoiceCallManager` interface is listed permanently (`Online`
  tracks registration; `Dial` fails with "imsd is not registered" / "imsd is
  not running"), and only this daemon's own restart cycles the origin — after
  which Calls dials nothing until it restarts, so the unit's `ExecStartPost`
  (`imsd-ofono-restart-calls.sh`) restarts the Calls daemon of every user
  running one. The first start since boot leaves a running Calls alone.

Development without a phone or root (crafter-build's binaries run inside the
sandbox: §4.1):

```sh
./bin/imsd-*/imsd --session &            # imsd's ABI on the session bus
./bin/imsd-ofonod-*/imsd-ofonod --session
busctl --user call org.ofono / org.ofono.Manager GetModems
```

## 9. Packaging, CI and releases

### 9.1 The APKBUILD

`packaging/APKBUILD.binary` is **the** packaging since 2026-09-02: it wraps
a crafter-build binary cross-compiled per §4.3 (bundled by
`packaging/make-bin-tarball.sh` together with the units, scripts and
policies) into the apks CI publishes. `pkgver` is taken from the `Version`
constant in `implementations/main.cpp` by `build-package.sh`.
Subpackages: `imsd-ofono` (the GNOME Calls backend, opt-in) and
`imsd-systemd` (the unit files; `install_if` systemd — ordered after
`imsd-ofono` because `systemd()` amoves all of `usr/lib/systemd/system`).
There is no from-source recipe: a build needs crafter-build (§4.1).

It declares `provides="81voltd=$pkgver-r$pkgrel"`: this satisfies
`soc-qcom-modem`'s dependency and excludes the real package, because 81voltd
serves the modem's own ims-PDN requests, races imsd for the PDN and flaps it
with a new prefix every ~2.5 min — two IMS stacks cannot share one PDN.
`depends` lists `modemmanager`, the AMR codec libraries, `pipewire-tools` and
`polkit` (the gate fails closed without it). There is no OpenRC service: the
unit's sequencing is only tested under systemd.

### 9.2 What the package installs and enables

`imsd.service` is enabled by preset (`80-imsd.preset`, plus the
`multi-user.target.wants` link) and gated by a drop-in
`ConditionPathExists=/etc/imsd.env`, so VoLTE survives reboots without a
manual `systemctl enable` and an unconfigured phone does not boot into a
failing unit. `imsd-ofonod.service` is enabled the same way by `imsd-ofono`.
All of that is the apk's doing (`APKBUILD.binary`'s `package()`).
The main package hides kde-telephony's `modem-daemon` autostart for accounts
created after it is installed (`/etc/skel/.config/autostart/org.kde.modem.daemon.desktop`)
because `imsd-dialerd` owns those names.

### 9.3 Install scripts

`imsd.post-upgrade`, `imsd-ofono.post-upgrade` and `imsd-systemd.post-upgrade`
mark their units `needs-restart` (§7.5); `imsd.post-upgrade` also records the
old invocation for the 0.3.9 hand-over. Each `*.post-install` is a symlink to
its `post-upgrade`: `apk del` leaves the daemon running, so a later `apk add`
is an upgrade for that daemon and apk runs post-install for it. A first
install finds no unit active and marks nothing. `imsd-systemd` marks
`imsd.service` too, since it ships the unit file and apk installs it after
`imsd`.

### 9.4 CI and what a push means

`.forgejo/workflows/package.yml` runs on every push to `main` (and on
dispatch): on the `flatpak` runner label (a privileged runner registered to
this repository — bubblewrap inside the job container needs the privilege), in an
`alpine:edge` container, `packaging/build-package.sh` installs the
Crafter.Build Flatpak (`net.catcrafts.CrafterBuild//stable` from
flatpak.catcrafts.net — the branch Crafter.Build publishes from its release
tags, where `beta` follows every push to its master; the build log names its
commit) and llvm/abuild, builds
the sysroot, cross-compiles the four products, runs the suite natively inside
the Flatpak, wraps the result with `APKBUILD.binary`, and the publish step
uploads the apks to the Forgejo Alpine registry
(`…/api/packages/Catcrafts/alpine/edge/fp6`) with `PACKAGE_TOKEN`. The
registry answers 409 for an already-published version and the step treats
that as "nothing to do", so **a push releases exactly when the `Version`
constant changes**. This is the only producer of the imsd apk: fp6-img
images install the published package from the registry, and
installed phones upgrade with plain `apk upgrade`. **A push to `main` is a
release to real users' phones.** A release is a range of commits ending in a
bump commit titled `imsd X.Y.Z` that changes only the `Version` constant.

## 10. Configuration reference

Everything is environment variables. The packaged unit reads
`/run/imsd.env` (from `ims-pdn-up.sh`) and then `/etc/imsd.env` (the user's,
wins on conflict). The variables are user documentation: the tables and the
minimum `/etc/imsd.env` are in the README.

### 10.1 imsd

The README's first table. The protected client/server ports are fixed at
45061/45062 (`imsd::util::PortUc`/`PortUs`) — changing them in source also
requires updating the firewall rules `ims-pdn-up.sh` installs.

### 10.2 imsd-media (per call, set through imsd's environment)

The README's second table. Two pairs are not configuration and are left out
there:

| Variable | Default | Meaning |
|---|---|---|
| `CODEC`, `OCTET_ALIGN` | *(from the SDP)* | set by imsd from the negotiated answer; not configuration |
| `MIC_SRC`, `PCM_DUMP` | *(unset)* | test seams (§5) |

### 10.3 ims-pdn-up.sh and imsd-ofonod

`IMS_APN`, `IMS_IP_TYPE`, `IMS_PROFILE_ID`, `IMS_REG_TIMEOUT` and `PCSCF` for
the bring-up script (§7.3). `imsd-ofonod` reads `EMERGENCY_NUMBERS` (from
`/etc/imsd.env` through its unit) and `IMSD_BUS=session|system` to override
only the imsd side of its bus choice.

## 11. Making changes

- **Commit messages.** Subject `area: what changes` — the area is the module
  partition, program, script or `packaging`/`ci`/`tests`/`README` the change
  lives in (several joined by commas), the rest a short lowercase clause
  with no trailing period, imperative or declarative. The body says why —
  the field incident or the spec clause — and, where the change alters
  behaviour, in one sentence how it was tested. A version bump is a commit
  titled `imsd X.Y.Z` that changes only the `Version` constant and has no
  body.
- **No AI attribution trailers.** Commits carry no `Assisted-by:`,
  `Co-Authored-By:` or similar lines, whoever or whatever drafted them.
- **Signed commits.** Every commit is GPG-signed with the repository's
  configured identity (`git config user.signingkey`, `commit.gpgsign`). Do
  not override the identity from the environment.
- **One change, one commit; a fix goes into the commit that introduced the
  problem** while a range is unpublished (rebase), never as a follow-up on
  top. Every commit of a range builds and passes the seven suites on its own.
- **Keep `make-bin-tarball.sh`'s file list and `APKBUILD.binary`'s
  `package()` in sync** when a packaged file is added.
- **The README and this file follow the code in the same commit.** A new
  knob gets its row in the README's configuration tables; a new state its journal line in §7.8; a new file
  under `/run` or `/var/lib` its row in §7.7.
- **Compatibility notes are kept where the mechanism lives**, not in a
  changelog the repository does not have: the 0.3.9 hand-over is documented
  in §7.5 because `imsd.post-upgrade` ships in every later package and the
  path stays live for any direct upgrade from an older daemon.
- **Never commit captures or state** (`*.raw`, `*.rtp`, `*.stats`,
  `imsreg.state`): they carry the subscriber's identity.

## 12. Security considerations

- imsd runs as root (xfrm, qmicli). Its attack surface on the bus is the
  frozen ABI; the writes are polkit-gated (§7.1, §8.1). Do not add an
  ungated write member, and do not add a root translation daemon that
  forwards calls without gating its own callers through `imsd-bus`.
- The bus policies deliberately let any local account *reach* both names;
  authorization is polkit's, not the policy file's. Keep it that way: a
  policy that names an account breaks the moment the account is called
  something else.
- SIP dumps and the state file carry the IMSI, the MSISDN, SPIs and
  addresses, and the state file (since 0.3.11) the last challenge answer —
  nonce, cnonce and response. Dumps are mode 0600 under the state dir, off by default
  (`DUMP_SIP=0`), and never leave the phone in a commit.
- The ims PDN is a walled garden: `ims-pdn-up.sh` keeps it from becoming
  the default route and gives the P-CSCF a host route; imsd adds one per
  call for the media peer (§7.3). Nothing else is routed over it.
  A change that routes anything else over it needs a reason and a bench.
- The fixed protected ports (45061/45062) are opened in nftables for
  `qmapmux*` only, by the bring-up script.

## 13. Carrier and platform facts the code relies on

Observed on real networks and encoded in the policy; cite them when touching
the related code.

- **KPN's fresh-SA throttle** — the `bring-up deferred: fresh-SA throttle
  active (spi-s=0)` case — refuses a fresh SA pair on an address while the
  registration on that address is live, whatever its age or last refresh:
  refused 11 s, 23 min and 2 h 58 min into a registration, 4 and 6.6 min
  after its last refresh (2026-09-19, -20, -24, 2026-10-03). KPN drops that
  registration the moment the initial REGISTER arrives, throttled or not,
  and with it gone accepts a fresh pair there: 5 and 21 min after the
  throttled attempt (2026-10-03). An address that holds no registration is
  accepted while another address's is live (2026-09-26, 2026-10-03), and a
  new prefix's PDN address was accepted 15 s after a throttled attempt on
  the temporary one (the 0.3.9 → 0.3.10 hop, 2026-10-03). An address whose
  registration another address's had replaced was refused 26 s after,
  accepted 141 s and 10 min after (2026-10-03): how soon KPN accepts where a
  registration has just gone is not measured more closely. The schedule's
  21 min is the wait that has never been refused, not a measured minimum;
  imsd resumes rather than registers fresh wherever it can, since on a live
  registration's address the attempt is refused and costs the registration.
- **O2 UK, 2026-09-24:** a refresh refused 21 × 503 over 70 min, then 33
  unanswered reconnect cycles; a fresh registration was accepted in one
  second. The origin of `RegRecovery`. On 2026-10-08, under 0.3.10 (the
  first release to log a Retry-After), a refresh was refused 503 with
  `Retry-After` 414 s and the refresh after that wait 503 with `Retry-After`
  772 s: the origin of the `Retry-After` escalation. Every refresh of the
  pre-0.3.11 shape — an initial registration's empty digest and the in-use
  SPIs in Security-Client — on record from O2 was refused with 503: 21 on
  2026-09-24 under 0.3.9 and the same loop again that afternoon on a
  stationary phone, two on 2026-10-08 under 0.3.10, and the user never saw
  one succeed, while
  the fresh registrations that followed were accepted: the origin of the
  5.1.1.4.2 refresh shape. KPN accepts both shapes without a challenge.
- **KPN closes a registration's previous reg-event subscription with
  `noresource`** when a new one replaces it: that end is not renewed.
- **Modem attach-family forcing** (Odido, O2 UK, Swisscom): an APN-string
  IPv6 bearer request is refused when the attach PDN was granted IPv4-only;
  a profile-indexed request is not.
- **pmOS's default nftables** drops inbound on `qmapmux*`; without the
  protected-port rule every network-initiated request (NOTIFY, MT INVITE)
  dies after ESP decapsulation and MT calls fall back to CS.
- **The kernel's temporary SLAAC address** on the ims netdev is listed first
  and retired after about two days; a registration bound to it dies with it.
- **systemd reads `EnvironmentFile=` per spawned command** (systemd 262~rc3
  on the FP6, 2026-10-03: an `ExecStart` saw what its `ExecStartPre` had
  just written), and unquotes a quoted value — which is why the hand-over script
  takes `LOCAL` from its environment rather than parsing the files itself.
- **GNOME Calls 50.0** crashes after an oFono origin withdraw/re-add cycle
  and dials nothing after `org.ofono` restarts until it restarts itself.
- **dbus-broker** checks broadcasts against the sender's send policy for
  every name the receiver owns (the `org.ofono.conf` detail).
- **The phone's wall clock reads 1970 until the first NTP sync**; anything
  that ages a journal line uses the monotonic or boot clock.
