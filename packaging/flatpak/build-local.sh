#!/usr/bin/env bash
# Builds the Flatpak from this checkout (uncommitted changes included) and
# installs it for the current user:
#
#   packaging/flatpak/build-local.sh [--run] [--lint]
#
# Needs flatpak-builder (or the org.flatpak.Builder app, installed on demand)
# and the org.freedesktop.Sdk//26.08 runtime (installed on demand). Both go in
# --user from Flathub. --run starts msga afterwards; --lint runs Flathub's
# linter on the manifest and the built repo.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
WORK="$ROOT/build-flatpak"
RUN=0
LINT=0
for a in "$@"; do
    case "$a" in
        --run) RUN=1 ;;
        --lint) LINT=1 ;;
        *) echo "usage: $0 [--run] [--lint]" >&2; exit 2 ;;
    esac
done

# Everything goes in --user, which needs Flathub as a user remote too (a
# system-wide one isn't seen there).
flatpak remote-add --user --if-not-exists flathub https://dl.flathub.org/repo/flathub.flatpakrepo

builder_app() {
    flatpak info org.flatpak.Builder >/dev/null 2>&1 ||
        flatpak install --user -y flathub org.flatpak.Builder
}
if command -v flatpak-builder >/dev/null 2>&1; then
    BUILDER=(flatpak-builder)
else
    builder_app
    BUILDER=(flatpak run org.flatpak.Builder)
fi

# The manifest with this checkout as its source in place of the git one.
# Only what the build reads is copied: the top-level entries outside KEEP
# (build trees, the website, release output, …) are skipped.
KEEP=" CMakeLists.txt version.cmake LICENSE THIRD_PARTY_LICENSES src gfx sfx packaging "
SKIP=""
for entry in "$ROOT"/* "$ROOT"/.[!.]*; do
    [ -e "$entry" ] || continue
    name="$(basename "$entry")"
    case "$KEEP" in *" $name "*) continue ;; esac
    SKIP+="          - ${name}"$'\n'
done
mkdir -p "$WORK"
MANIFEST="$WORK/com.nisdos.msga.yml"
awk -v root="$ROOT" -v skip="$SKIP" '
    /^      - type: git$/ {
        print "      - type: dir"; print "        path: " root; print "        skip:"; printf "%s", skip
        drop = 1; next
    }
    drop && /^      - type: / { drop = 0 }
    drop { next }
    { print }
' "$HERE/com.nisdos.msga.yml" > "$MANIFEST"
grep -q 'type: dir' "$MANIFEST" || { echo "build-local.sh: no git source to replace" >&2; exit 1; }

# --disable-rofiles-fuse: its FUSE helper fails inside the Builder app.
"${BUILDER[@]}" --user --install --install-deps-from=flathub --force-clean --disable-rofiles-fuse \
    --state-dir="$WORK/state" --repo="$WORK/repo" \
    "$WORK/app" "$MANIFEST"

# Lint errors fail the build (warnings don't). lint-exceptions.json excuses
# only what a local build can't satisfy: Flathub's own build mirrors the
# screenshots, which a local repo never has.
if [ "$LINT" = 1 ]; then
    builder_app
    LINTER=(flatpak run --command=flatpak-builder-lint org.flatpak.Builder
        --exceptions --user-exceptions "$HERE/lint-exceptions.json")
    "${LINTER[@]}" manifest "$HERE/com.nisdos.msga.yml"
    "${LINTER[@]}" repo "$WORK/repo"
fi
if [ "$RUN" = 1 ]; then
    flatpak run com.nisdos.msga
fi
