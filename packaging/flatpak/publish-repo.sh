#!/usr/bin/env bash
# Turns a flatpak-builder repo (build-local.sh's build-flatpak/repo) into the
# signed Flatpak repository msga publishes itself (not Flathub), as a static
# site: the app's ref copied to the `stable` branch and GPG-signed, the summary
# signed with static deltas, and the two files users start from:
#
#   com.nisdos.msga.flatpakref  flatpak install <url>/com.nisdos.msga.flatpakref
#   msga.flatpakrepo          flatpak remote-add --if-not-exists msga <url>/msga.flatpakrepo
#
# The shared runtime (org.freedesktop.Platform) comes from Flathub: installing
# from the .flatpakref adds Flathub when it's missing (its RuntimeRepo); after
# a remote-add of the .flatpakrepo, Flathub has to be added by hand.
#
# Usage:
#   packaging/flatpak/publish-repo.sh <src-repo> <out-dir> <base-url>
#
#   <src-repo>  the flatpak-builder repo (has app/com.nisdos.msga/<arch>/master)
#   <out-dir>   the site to make (emptied first); serve it at <base-url>
#   <base-url>  where the site is served, e.g. https://msga.app/flatpak
#
# Env: GPG_KEY_ID (required), GPG_HOMEDIR (default: gpg's own) — the signing
#      key, its secret part in that keyring.
# Needs: flatpak, ostree, gpg.
set -euo pipefail

SRC="${1:?usage: publish-repo.sh <src-repo> <out-dir> <base-url>}"
OUT="${2:?usage: publish-repo.sh <src-repo> <out-dir> <base-url>}"
BASE="${3:?usage: publish-repo.sh <src-repo> <out-dir> <base-url>}"
BASE="${BASE%/}"
: "${GPG_KEY_ID:?set GPG_KEY_ID to the signing key}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

APP=com.nisdos.msga
ARCH="$(flatpak --default-arch)"
SIGN=(--gpg-sign="$GPG_KEY_ID")
GPG=(gpg)
if [ -n "${GPG_HOMEDIR:-}" ]; then
    SIGN+=(--gpg-homedir="$GPG_HOMEDIR")
    GPG+=(--homedir "$GPG_HOMEDIR")
fi

rm -rf "$OUT"
mkdir -p "$OUT"
ostree init --mode=archive-z2 --repo="$OUT/repo"

# Only the app itself (not its .Debug extension), on the branch users follow.
flatpak build-commit-from --src-repo="$SRC" --src-ref="app/$APP/$ARCH/master" \
    "${SIGN[@]}" --no-update-summary "$OUT/repo" "app/$APP/$ARCH/stable"
flatpak build-update-repo "${SIGN[@]}" --generate-static-deltas --prune \
    --title="msga" --default-branch=stable "$OUT/repo"

KEY="$("${GPG[@]}" --export "$GPG_KEY_ID" | base64 -w0)"
[ -n "$KEY" ] || { echo "publish-repo.sh: no public key for $GPG_KEY_ID" >&2; exit 1; }
install -m 644 "$ROOT/gfx/icon_256.png" "$OUT/msga.png"

cat >"$OUT/msga.flatpakrepo" <<EOF
[Flatpak Repo]
Title=msga
Url=$BASE/repo/
Homepage=https://msga.app/
Comment=A fast native client for Slack
Icon=$BASE/msga.png
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
GPGKey=$KEY
EOF

cat >"$OUT/$APP.flatpakref" <<EOF
[Flatpak Ref]
Name=$APP
Branch=stable
Title=msga
Url=$BASE/repo/
SuggestRemoteName=msga
IsRuntime=false
Homepage=https://msga.app/
Icon=$BASE/msga.png
RuntimeRepo=https://dl.flathub.org/repo/flathub.flatpakrepo
GPGKey=$KEY
EOF

cat >"$OUT/index.html" <<EOF
<!doctype html>
<meta charset="utf-8">
<title>msga Flatpak repository</title>
<h1>msga Flatpak repository</h1>
<p>Install msga, and get its updates with <code>flatpak update</code>:</p>
<pre>flatpak install --user $BASE/$APP.flatpakref</pre>
<p>or add the repository yourself (msga runs on Flathub's freedesktop runtime):</p>
<pre>flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo
flatpak remote-add --user --if-not-exists msga $BASE/msga.flatpakrepo
flatpak install --user msga $APP</pre>
<p>More at <a href="https://msga.app/">msga.app</a>.</p>
EOF

echo "Site in $OUT for $BASE:"
ostree refs --repo="$OUT/repo"
du -sh "$OUT"
