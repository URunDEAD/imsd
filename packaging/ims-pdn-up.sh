#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts®
# ims-pdn-up.sh — boot bring-up for the `ims` PDN (journal/ims.md s45).
#
# Finds-or-creates the ims bearer via ModemManager, connects it, and
# configures the muxed netdev with the MM-assigned address (the s34 recipe,
# automated) and a host route to the P-CSCF — never a default route (F15
# below). Idempotent — safe to run when the PDN is already up. Runs as
# ExecStartPre of imsd.service, so imsd only starts once the PDN exists; a
# nonzero exit fails the unit and systemd retries per Restart/RestartSec.
#
# Configuration, via the unit's EnvironmentFile /etc/imsd.env (also read
# directly so manual runs behave the same):
#   IMS_APN          ims APN name (default ims)
#   IMS_IP_TYPE      bearer ip-type (default ipv6)
#   IMS_PROFILE_ID   3GPP profile index to connect through (default: looked up
#                    by APN in the modem's profile list; empty/none = connect by
#                    APN string). A profile-indexed call is exempt from the
#                    modem's attach-family forcing that refuses an APN-string
#                    IPv6 request when the attach PDN was granted IPv4-only
#                    (journal/ims 2026-09-16/17 bench: Odido, O2 UK, Swisscom).
#   IMS_REG_TIMEOUT  max seconds to wait for network registration before
#                    connect attempts start counting anyway (default 300)
#   PCSCF            the P-CSCF address imsd registers with (its own
#                    required setting); gets the host route over the ims PDN

log() { echo "ims-pdn-up: $*"; }

kv() { mmcli "$@" -K 2>/dev/null; }

# mmcli's multi-line error text as one journal-friendly line
squash() { printf '%s' "$1" | tr '\n' ' ' | sed 's/  */ /g'; }

modem_state()  { kv -m "$MODEM" | sed -n 's/^modem\.generic\.state *: *//p'; }
packet_state() { kv -m "$MODEM" | sed -n 's/^modem\.3gpp\.packet-service-state *: *//p'; }

bearer_paths() {
    kv -m "$MODEM" | sed -n 's/^modem\.generic\.bearers\.value\[[0-9]*\] *: *//p'
}

# A disabled modem has its radio off (airplane mode): leave it alone. A bearer
# connect on it fails with no-service and leaves ModemManager's state
# `enabled` with the modem still in low power; switching the radio back on
# then no longer brings the modem up (dev phone 2026-10-06).
radio_off() { [ "$(modem_state)" = disabled ]; }

envval() {  # $1 = key — for manual runs; under systemd the vars are inherited
    sed -n "s/^$1=//p" /etc/imsd.env 2>/dev/null | tail -n1 | tr -d '"'
}
IP_TYPE=${IMS_IP_TYPE:-$(envval IMS_IP_TYPE)}
IP_TYPE=${IP_TYPE:-ipv6}
IMS_APN=${IMS_APN:-$(envval IMS_APN)}
IMS_APN=${IMS_APN:-ims}
PROFILE_ID=${IMS_PROFILE_ID:-$(envval IMS_PROFILE_ID)}
REG_TIMEOUT=${IMS_REG_TIMEOUT:-$(envval IMS_REG_TIMEOUT)}
REG_TIMEOUT=${REG_TIMEOUT:-300}

# ---- wait for a modem (MM + modem firmware take a while after boot)
n=0
while :; do
    MODEM=$(mmcli -L 2>/dev/null | sed -n 's,.*/Modem/\([0-9]*\).*,\1,p' | head -n1)
    [ -n "$MODEM" ] && break
    n=$((n + 1))
    [ "$n" -ge 60 ] && { log "no modem after 120 s"; exit 1; }
    sleep 2
done
log "modem $MODEM"

# ---- the ims profile index (WDS profile list), unless configured. The
# lookup itself can fail while ModemManager is still enabling the modem; a
# failed lookup is not "no profile" (the APN-string request it would fall
# back to is the one the modem refuses under an IPv4 attach), so it is
# retried after the registration wait; a list without the APN is final —
# the APN string is used — only once the modem is registered.
LOOKUP=pending
if [ -n "$PROFILE_ID" ]; then
    [ "$PROFILE_ID" = none ] && PROFILE_ID=
    LOOKUP=done
fi
lookup_profile() {  # $1 = modem state, $2 = log suffix; sets PROFILE_ID, LOOKUP=done on a final answer
    [ "$LOOKUP" = done ] && return 0
    if OUT=$(qmicli -d qrtr://0 --wds-get-profile-list=3gpp 2>&1); then
        # a list with no profiles at all is the modem still coming up, not
        # a modem without an ims profile (dev phone cold boot 2026-09-20
        # 16:47: exit 0, no entries, index 2 present a minute later)
        if ! printf '%s\n' "$OUT" | grep -q -E '^[[:space:]]*\[[0-9]+\] 3gpp'; then
            log "ims profile lookup returned no profiles$2"
            return 0
        fi
        PROFILE_ID=$(printf '%s\n' "$OUT" | awk -v apn="$IMS_APN" '
            /^[ \t]*\[[0-9]+\] 3gpp/ { idx = $1; gsub(/[^0-9]/, "", idx) }
            /APN:/ { a = $0; sub(/.*APN: '"'"'/, "", a); sub(/'"'"'.*/, "", a);
                     if (tolower(a) == tolower(apn) && idx != "") { print idx; exit } }')
        if [ -n "$PROFILE_ID" ]; then
            LOOKUP=done
            log "ims profile: index $PROFILE_ID (apn $IMS_APN)"
            return 0
        fi
        # a list without the APN is final only once the modem is registered:
        # while it enables, the list can be partial (dev phone cold boot
        # 2026-09-24 18:18: entries but no ims at `enabling`, ims listed when
        # read minutes later), and on a walled carrier the APN-string fallback
        # that would follow is the request the modem refuses
        case "$1" in
            registered|connecting|connected)
                LOOKUP=done
                log "ims profile: none for apn $IMS_APN, connecting by APN string" ;;
            *)  log "ims profile: not in the list yet$2" ;;
        esac
    else
        log "ims profile lookup failed$2: $(squash "$OUT")"
    fi
}
STATE=$(modem_state)
lookup_profile "$STATE" " (modem state: $STATE)"

# ---- find a connected ims bearer; else find-or-create one and connect it.
# APN names are case-insensitive. A DISCONNECTED bearer is reused only when
# it is the request this run would make (same profile index, or same APN when
# connecting by APN string): a leftover APN-string bearer from a run whose
# lookup failed is the walled request itself, and reconnecting it fails on
# every restart until the modem is reset (field: O2 UK, 2026-09-20).
has() { printf '%s\n' "$INFO" | grep -qi "^bearer\.$1  *: *$2\$"; }
find_ims_bearer() {  # $1 = required bearer.status.connected value; $2 = "relaxed" ignores ip-type
    for B in $(bearer_paths); do
        INFO=$(kv -b "$B") || continue
        has status.connected "$1" || continue
        if [ -n "$PROFILE_ID" ]; then
            if [ "$1" = yes ]; then
                has properties.profile-id "$PROFILE_ID" || has properties.apn "$IMS_APN" || continue
            else
                has properties.profile-id "$PROFILE_ID" || continue
            fi
        else
            has properties.apn "$IMS_APN" || continue
        fi
        [ "$2" = relaxed ] || has properties.ip-type "$IP_TYPE" || continue
        echo "$B"
        return 0
    done
    return 1
}
# leftover disconnected APN-string ims bearers when this run connects by
# profile index: never reused (above), deleted so a later run without an
# index cannot pick one up either
delete_stale_bearers() {
    [ -n "$PROFILE_ID" ] || return 0
    for B in $(bearer_paths); do
        INFO=$(kv -b "$B") || continue
        has status.connected no || continue
        has properties.apn "$IMS_APN" || continue
        has properties.profile-id "$PROFILE_ID" && continue
        OUT=$(mmcli -m "$MODEM" --delete-bearer="$B" 2>&1) &&
        log "deleted stale bearer $B (apn $IMS_APN, no profile index)" ||
        log "could not delete stale bearer $B: $(squash "$OUT")"
    done
}

BEARER=$(find_ims_bearer yes)

# ---- gate the connect attempts on network registration: the 10x10 s window
# below is shorter than some carriers' post-boot attach (measured ~2 min), so
# without this every attempt can fail on no-service and the retries end
# before the network is even attached
if [ -z "$BEARER" ]; then
    waited=0 last=
    while :; do
        STATE=$(modem_state)
        case "$STATE" in registered|connecting|connected) break ;; esac
        [ "$STATE" != "$last" ] && log "waiting for registration (state: ${STATE:-unknown})"
        last=$STATE
        if [ "$waited" -ge "$REG_TIMEOUT" ]; then
            if [ "$STATE" = disabled ]; then
                log "modem still disabled after $REG_TIMEOUT s (radio off): not connecting"
                exit 1
            fi
            log "not registered after $REG_TIMEOUT s — attempting anyway"
            break
        fi
        sleep 5
        waited=$((waited + 5))
    done
    log "modem state: $(modem_state), packet service: $(packet_state)"
    # a lookup with no final answer yet: it failed, or its list lacked the
    # APN while the modem was still coming up
    n=0
    while [ "$LOOKUP" != done ] && [ "$n" -lt 3 ]; do
        n=$((n + 1))
        sleep 5
        STATE=$(modem_state)
        lookup_profile "$STATE" " (retry $n, modem state: $STATE)"
    done
    if [ "$LOOKUP" != done ]; then
        PROFILE_ID=
        log "ims profile: lookup keeps failing, connecting by APN string"
    fi
    BEARER=$(find_ims_bearer yes)   # the index may now match a connected profile-indexed PDN
    [ -n "$BEARER" ] || delete_stale_bearers
fi

n=0
while [ -z "$BEARER" ]; do
    n=$((n + 1))
    [ "$n" -gt 10 ] && { log "bearer connect failed after 10 attempts"; exit 1; }
    radio_off && { log "modem disabled (radio off): not connecting"; exit 1; }
    B=$(find_ims_bearer no)   # reuse a stale disconnected ims bearer of this run's shape
    if [ -z "$B" ]; then
        if [ -n "$PROFILE_ID" ]; then
            SPEC="profile-id=$PROFILE_ID,ip-type=$IP_TYPE"
        else
            SPEC="apn=$IMS_APN,ip-type=$IP_TYPE"
        fi
        OUT=$(mmcli -m "$MODEM" --create-bearer="$SPEC" 2>&1)
        B=$(printf '%s' "$OUT" | sed -n 's,.*\(/org/freedesktop/ModemManager1/Bearer/[0-9]*\).*,\1,p')
        if [ -n "$B" ]; then
            log "created bearer $B ($SPEC)"
        else
            log "create-bearer attempt $n failed: $(squash "$OUT"); retrying in 10 s"
            sleep 10
            continue
        fi
    fi
    if OUT=$(mmcli -b "$B" --connect 2>&1); then
        BEARER=$B
    else
        log "connect attempt $n failed (state: $(modem_state)): $(squash "$OUT"); retrying in 10 s"
        case "$OUT" in *call-already-present*)
            # the PDN is up on a bearer this run did not recognise (ip-type or
            # APN spelling); use it rather than ask the modem for a second one
            if B=$(find_ims_bearer yes relaxed); then
                log "adopting the connected bearer $B"
                BEARER=$B
                continue
            fi ;;
        esac
        sleep 10
    fi
done
log "connected: $BEARER"

# ---- configure the muxed netdev with the MM-assigned address
INFO=$(kv -b "$BEARER")
IFACE=$(echo "$INFO" | sed -n 's/^bearer\.status\.interface  *: *//p')
ADDR=$(echo "$INFO" | sed -n 's/^bearer\.ipv6-config\.address  *: *//p')
PREFIX=$(echo "$INFO" | sed -n 's/^bearer\.ipv6-config\.prefix  *: *//p')
if [ -z "$IFACE" ] || [ -z "$ADDR" ]; then
    log "bearer up but no interface/address in mmcli output"
    exit 1
fi
# ---- the ims PDN must never be the phone's IPv6 default route (F15). The
# PGW's router advertisement offers a default router; on IPv4-only wifi
# that is the phone's only v6 default, so every AAAA-first client (apk,
# browsers) sends its SYN into the IMS walled garden, which drops it
# silently (dev phone 2026-09-20: a 9-min apk hang). imsd only talks to the
# P-CSCF (host route below) and to media peers (imsd installs those host
# routes per call); the RA's prefix and MTU are still applied (its hop
# limit is not — the kernel default 64 stays). Per-netdev, via /proc, and
# before link-up: the router solicitation goes out once the link-local
# address passes DAD, ~1–2 s after link-up, and the RA answers at once.
if ! echo 0 > "/proc/sys/net/ipv6/conf/$IFACE/accept_ra_defrtr" 2>/dev/null; then
    log "could not set accept_ra_defrtr=0 on $IFACE (continuing)"
fi
# the PDN's MTU as the modem reports it (1280 here), on the link itself:
# the P-CSCF host route makes the P-CSCF reachable before the RA's MTU
# option has been processed, and a send in that window would otherwise be
# sized by the link's default MTU. NetworkManager does the same for its
# bearers. An RA MTU no larger than the link's is still applied.
MTU=$(echo "$INFO" | sed -n 's/^bearer\.ipv6-config\.mtu  *: *//p')
case "$MTU" in ''|--|*[!0-9]*) MTU= ;; esac
if [ -n "$MTU" ] && [ "$MTU" -ge 1280 ]; then
    ip link set "$IFACE" mtu "$MTU" || log "could not set mtu $MTU on $IFACE (continuing)"
fi
ip link set "$IFACE" up || exit 1
ip -6 addr replace "$ADDR/${PREFIX:-64}" dev "$IFACE" || exit 1

# wait out IPv6 DAD: binding a tentative address gives EADDRNOTAVAIL
# (first boot attempt cost a 120 s systemd retry exactly this way)
n=0
while ip -6 addr show dev "$IFACE" | grep -q tentative; do
    n=$((n + 1))
    [ "$n" -ge 10 ] && { log "address still tentative after 10 s"; break; }
    sleep 1
done
log "$IFACE up, $ADDR/${PREFIX:-64}, ipv6 mtu $(cat "/proc/sys/net/ipv6/conf/$IFACE/mtu" 2>/dev/null)"

# a default route an earlier RA already installed (the link was up before
# this run: an imsd restart) — accept_ra_defrtr=0 leaves an existing one in
# place until its lifetime runs out. Any ::/0 via this netdev goes, by the
# policy above.
if [ -n "$(ip -6 route show default dev "$IFACE" 2>/dev/null)" ]; then
    ip -6 route flush default dev "$IFACE" && log "dropped the RA default route on $IFACE"
fi
# the P-CSCF is reachable through the ims PDN only: a host route stands in
# for the default. `dev` without a gateway — a raw-IP netdev without
# header_ops, so the kernel resolves no neighbours — the shape imsd uses
# for media peers. The PDN is brought up IPv6-only (the address above), so
# the P-CSCF must be an IPv6 address; anything else on the IMS network
# (its DNS servers, a traceroute target) needs its own host route. Without
# this route imsd cannot reach the P-CSCF, so a failure is the unit's
# failure.
PCSCF=${PCSCF:-$(envval PCSCF)}
case "$PCSCF" in
    '')  log "PCSCF not configured: no P-CSCF host route (imsd needs PCSCF= in /etc/imsd.env)" ;;
    *\[*) log "PCSCF '$PCSCF': write the bare address, as ip prints it (no brackets)"; exit 1 ;;
    *:*) OUT=$(ip -6 route replace "$PCSCF/128" dev "$IFACE" 2>&1) ||
             { log "host route to P-CSCF $PCSCF failed: $(squash "$OUT")"; exit 1; }
         log "host route to P-CSCF $PCSCF dev $IFACE" ;;
    *)   log "PCSCF '$PCSCF' is not an IPv6 address; the ims PDN is brought up IPv6-only"; exit 1 ;;
esac

# ---- hand the connected ims netdev and the PDN's own address to imsd
# (imsd.service reads /run/imsd.env): the daemon carries no baked-in
# interface name and registers on the address the network assigned — not
# on the kernel's temporary SLAAC address from the RA, which the netdev
# lists first and which is deprecated after a day and removed after two
# (ims ledger F18; the SAs and the Contact would go with it)
printf 'DEV=%s\nLOCAL=%s\n' "$IFACE" "$ADDR" > /run/imsd.env

# ---- open the IMS protected ports in the firewall (rung 5b root cause,
# journal/ims.md s56): pmOS's default nftables INPUT chain is policy-drop and
# explicitly drops all inbound on qmapmux*, which silently killed every
# network-initiated request (reg-event NOTIFY, MT INVITE) after ESP decap —
# the P-CSCF's TCP SYNs to the protected server port never reached the
# listener, so terminating delivery failed and MT calls fell back to CS.
# 45061/45062 = imsd's protected client/server ports (kPortUc/kPortUs).
# Best-effort: never fail the unit over a missing/foreign firewall.
if nft list table inet filter >/dev/null 2>&1; then
    if ! nft list chain inet filter input | grep -q imsd-protected-ports; then
        nft insert rule inet filter input iifname "qmapmux*" tcp dport 45061-45062 accept comment '"imsd-protected-ports"' &&
        nft insert rule inet filter input iifname "qmapmux*" udp dport 45061-45062 accept comment '"imsd-protected-ports"' &&
        log "nftables: opened protected ports 45061-45062 on qmapmux*" ||
        log "nftables: rule insert failed (continuing)"
    fi
fi
exit 0
