#!/bin/sh -eu
# SPDX-License-Identifier: GPL-3.0-only
# SPDX-FileCopyrightText: Copyright (C) 2026 Catcrafts®
# CI package build: produce the imsd apk(s) for aarch64 from THIS checkout,
# using packaging/aport/ (the canonical aport) and pmbootstrap's cross
# chroots. Runs in CI inside an Alpine container on the privileged "pmos"
# runner (qemu-user binfmt on the host for the aarch64 chroots); also
# runnable in any Alpine environment with the same privileges
# (IMSD_NO_CROSSDIRECT=1 for hosts where crossdirect's /native bridge
# breaks — the build then runs qemu-only, slower, identical output).
#
# Built packages land in ~build/.local/var/pmbootstrap/packages/*/aarch64/;
# the workflow's publish step uploads the imsd*.apk ones to the Forgejo
# Alpine registry.

set -eu

PMAPORTS_REPO=https://gitlab.postmarketos.org/postmarketOS/pmaports.git
SRC=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)

# pmbootstrap refuses to run as root: install deps, then re-exec as a build
# user with passwordless sudo (pmbootstrap escalates itself where needed).
if [ "$(id -u)" = 0 ]; then
	apk add -q git sudo python3 py3-pip multipath-tools util-linux tar
	# pmbootstrap pinned from git: Alpine's package is older. The pmOS
	# gitlab hiccups under crawler load; a failure costs a retry, not the run
	for _i in 1 2 3; do
		pip install -q --break-system-packages \
			git+https://gitlab.postmarketos.org/postmarketOS/pmbootstrap.git@3.11.1 \
			&& break
		if [ "$_i" = 3 ]; then
			echo "pmbootstrap pip install failed after 3 attempts" >&2
			exit 1
		fi
		echo "pmbootstrap pip install failed (attempt $_i/3), retrying in 15s..." >&2
		sleep 15
	done
	# containers cannot modprobe; make pmbootstrap's 'sudo modprobe' a no-op
	# (/usr/local/sbin precedes /sbin in sudo's secure_path)
	mkdir -p /usr/local/sbin
	printf '#!/bin/sh\nexit 0\n' > /usr/local/sbin/modprobe
	chmod +x /usr/local/sbin/modprobe
	id build >/dev/null 2>&1 || adduser -D build
	echo 'build ALL=(ALL) NOPASSWD: ALL' > /etc/sudoers.d/build
	# su scrubs the environment — carry the knobs that matter across it
	exec su build -c "IMSD_NO_CROSSDIRECT='${IMSD_NO_CROSSDIRECT:-}' sh -eu '$SRC/packaging/build-package.sh'"
fi

# the CI checkout is root-owned; let git read it as the build user
git config --global --add safe.directory "$SRC"

# git hosts occasionally hiccup; a clone failure costs a retry, not the run
clone_retry() { # clone_retry <dest> <git clone args...>
	_dest=$1; shift
	for _i in 1 2 3; do
		rm -rf "$_dest"
		git clone "$@" "$_dest" && return 0
		echo "git clone $_dest failed (attempt $_i/3), retrying in 10s..." >&2
		sleep 10
	done
	echo "git clone $_dest failed after 3 attempts" >&2
	return 1
}

retry() { # retry <description> <cmd...>
	_desc=$1; shift
	for _i in 1 2 3; do
		"$@" && return 0
		echo "$_desc failed (attempt $_i/3), retrying in 30s..." >&2
		sleep 30
	done
	echo "$_desc failed after 3 attempts" >&2
	return 1
}

# pmbootstrap swallows its subcommands' stderr into its own log; surface it
# whenever this script dies so failures are diagnosable from CI output alone
trap 'rc=$?; if [ $rc -ne 0 ]; then
	echo "=== build-package.sh failed (exit $rc); pmbootstrap log tail ==="
	tail -60 "$HOME/.local/var/pmbootstrap/log.txt" 2>/dev/null || true
fi' EXIT

# --- pmaports with our aport dropped in; source tarball from this checkout
# (the Forgejo instance serves no source archives, so git-archive it)
WORK=${IMSD_PKG_WORK:-$HOME/imsd-pkg-work}
rm -rf "$WORK"
mkdir -p "$WORK"
clone_retry "$WORK/pmaports" -q --depth=1 "$PMAPORTS_REPO"
mkdir -p "$WORK/pmaports/modem"
cp -r "$SRC/packaging/aport" "$WORK/pmaports/modem/imsd"
COMMIT=$(git -C "$SRC" rev-parse --short HEAD)
git -C "$SRC" archive --prefix=imsd/ \
	-o "$WORK/pmaports/modem/imsd/imsd-$COMMIT.tar.gz" HEAD
sed -i "s/^_commit=.*/_commit=\"$COMMIT\"/" "$WORK/pmaports/modem/imsd/APKBUILD"

# --- configure pmbootstrap (config written directly; 'init' is interactive)
WORKDIR="$HOME/.local/var/pmbootstrap"
mkdir -p "$WORKDIR/cache_git"
python3 -c "import pmb.config; print(pmb.config.work_version)" > "$WORKDIR/version"
mkdir -p "$HOME/.config"
cat > "$HOME/.config/pmbootstrap_v3.cfg" <<EOF
[pmbootstrap]
aports = $WORK/pmaports
device = fairphone-fp6
ui = console
systemd = always
EOF

NOCROSS=${IMSD_NO_CROSSDIRECT:+--no-cross}

pmbootstrap checksum imsd
retry "build imsd" pmbootstrap $NOCROSS build --arch aarch64 imsd

echo "=== built packages ==="
ls -la "$WORKDIR"/packages/*/aarch64/imsd*.apk
