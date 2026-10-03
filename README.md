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

Call-capable, both directions: outgoing calls place, answer, carry full-duplex
AMR-WB audio and tear down; incoming calls ring, answer and carry media the
same way, driven end-to-end from a stock dialer over a frozen D-Bus ABI.
Not there yet: DTMF (`SendDtmf` is a stub), hold/multiparty/call waiting,
USSD, P-CSCF discovery (the address is configured by hand).

Verified by community members to be fully working:

| Device | OS | Carrier |
|---|---|---|
| The Fairphone (Gen. 6) | postmarketOS, Linux 7.1.2 | KPN NL |
| The Fairphone (Gen. 6) | postmarketOS, Linux 7.1.2 | Telekom Deutschland GER |
| The Fairphone (Gen. 6) | postmarketOS, Linux 7.1.2 | Phonero |
| The Fairphone (Gen. 6) | postmarketOS, Linux 7.1.2 | Telia Norge |

## What it needs

imsd talks to the modem through ModemManager plus `qmicli` over QRTR, installs
its security associations with `ip xfrm` and plays audio through PipeWire, so
in practice: a Qualcomm modem on a mainline kernel with working
WWAN/ModemManager integration, kernel ESP, PipeWire, systemd and polkit — and
your carrier's P-CSCF address.

## Install and configure

On a Catcrafts Fairphone 6 image imsd is installed from the project's apk
registry and upgrades with `apk upgrade` (an upgrade restarts the daemon; a
call in progress ends). Elsewhere, build it (below), `make install`, and
enable the units yourself — `systemctl enable imsd`, and `systemctl enable
--now imsd-ofonod` for GNOME Calls: imsd's preset, the
`multi-user.target.wants` link and the `/etc/imsd.env` condition below come
with the apk, not with `make install`.

Configuration is environment variables in `/etc/imsd.env`; the one required
setting is the P-CSCF:

```sh
# your carrier's P-CSCF address — from a stock-firmware capture or the
# carrier's IMS documentation
PCSCF=2001:db8::105
```

The apk's `imsd.service` is enabled by preset and starts at boot once that
file exists (the condition is checked when the unit starts, so after writing
the file run `systemctl start imsd`). It first brings the `ims` PDN up through ModemManager, then registers:
resuming an existing registration when it can, registering fresh otherwise,
and waiting out whatever the network tells it to wait out. Check on it with:

```sh
journalctl -u imsd -f
busctl --system call net.catcrafts.IMS1 /net/catcrafts/IMS1 net.catcrafts.IMS1 GetStatus
```

Every variable, what the daemon does when the registration breaks, and
what the journal lines mean are in [AGENTS.md](AGENTS.md) (§10, §7.4, §7.8).

## Dialers

imsd itself is desktop-agnostic: everything a dialer needs is on the system
bus as `net.catcrafts.IMS1`. A small translation daemon per desktop speaks
the backend protocol its dialer already consumes, so dialer, shell and the
call-history/contacts stack run unmodified.

| Desktop | Daemon | Package | Owns | Consumed by |
|---|---|---|---|---|
| Plasma Mobile | `imsd-dialerd` | `imsd` | `org.kde.telephony.*` (session bus) | plasma-dialer + kde-telephony-daemon, replacing `modem-daemon` |
| Phosh / GNOME Mobile | `imsd-ofonod` | `imsd-ofono` | `org.ofono` (system bus) | GNOME Calls' `ofono` provider plugin |

Plasma Mobile needs nothing beyond the `imsd` package: `imsd-dialerd` is
autostarted in the session in place of plasma-dialer's `modem-daemon`, whose
own autostart must be off — the package hides it for accounts created after
the install (`/etc/skel`); an existing account hides it itself with a
`Hidden=true` copy of `org.kde.modem.daemon.desktop` in `~/.config/autostart`.

For Phosh and GNOME Mobile, `apk add imsd-ofono` enables `imsd-ofonod` and
points GNOME Calls at its `ofono` provider (a gschema override; the per-user
equivalent is `gsettings set org.gnome.Calls autoload-plugins "['ofono']"`).
Do not run it alongside a real `ofonod`. Known gaps — USSD, hold and
multiparty, caller-ID withholding, the empty emergency-call screen, and why a
restart of `imsd-ofonod` restarts GNOME Calls — are listed in
[AGENTS.md §8.3](AGENTS.md).

Call control is polkit's `net.catcrafts.IMS1.Voice`: the active local
session and root may place, answer and end calls; an ssh login may not.

## Building

[Crafter Build](https://forgejo.catcrafts.net/Catcrafts/Crafter.Build) plus
clang/libc++/lld and the gio-2.0 headers:

```sh
crafter-build          # the daemon; -- --product=media|dialerd|ofonod for the others
crafter-build test     # the seven unit-test suites
make && make check     # the same, without crafter-build (distro packaging)
```

Cross-compiling for a phone, the Makefile's requirements, the package layout
and the CI that publishes the apks are in [AGENTS.md](AGENTS.md) (§4, §9).

## Documentation

[AGENTS.md](AGENTS.md) is the complete reference — for coding agents, and
for anyone who wants to know how imsd works or change it: the build products
and the rule that keeps the engine core free of GLib and I/O, the test
suites, the lint rules, the registration and recovery behaviour, the ims PDN
bring-up, upgrades, the frozen D-Bus ABI, packaging and release mechanics,
the full configuration reference, and the carrier behaviours the code was
shaped by.

## License

GPL-3.0-only — see [LICENSE](LICENSE).

## Copyright

Copyright (C) 2026 Catcrafts®
catcrafts.net
