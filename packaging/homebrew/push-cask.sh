#!/usr/bin/env bash
# Renders msga.rb.in into punarinta/homebrew-msga as Casks/msga.rb and pushes
# it, for the release workflow (.github/workflows/release.yml): the publish
# job after a release, the backfill job after adding a Mac to one.
#
# Usage: push-cask.sh <version> <tag> <arm64 manifest> <x86_64 manifest> <branch> [message]
# Env:   TOKEN  a token that can push to punarinta/homebrew-msga
set -euo pipefail

[[ $# -ge 5 ]] || { echo "usage: $0 <version> <tag> <arm64 manifest> <x86_64 manifest> <branch> [message]" >&2; exit 2; }
version="$1" tag="$2" arm_manifest="$3" intel_manifest="$4" branch="$5"
message="${6:-msga ${version}}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

arm="$(jq -r .sha256 "$arm_manifest")"
intel="$(jq -r .sha256 "$intel_manifest")"
for s in "$arm" "$intel"; do
    [[ "$s" =~ ^[0-9a-f]{64}$ ]] || { echo "::error::no sha256 in $arm_manifest or $intel_manifest"; exit 1; }
done

tap="$(mktemp -d)/tap"
git clone -q "https://x-access-token:${TOKEN}@github.com/punarinta/homebrew-msga.git" "$tap"
git -C "$tap" checkout -q -B "$branch"
sed -e "s/@VERSION@/${version}/g" -e "s/@TAG@/${tag}/g" \
    -e "s/@SHA256_ARM64@/${arm}/g" -e "s/@SHA256_X86_64@/${intel}/g" \
    "${here}/msga.rb.in" >"${tap}/Casks/msga.rb"
if grep -n '@[A-Z0-9_]*@' "${tap}/Casks/msga.rb"; then
    echo "::error::placeholder left in the cask (above)"
    exit 1
fi
if git -C "$tap" diff --quiet; then
    echo "Cask already current."
    exit 0
fi
git -C "$tap" commit -q -am "$message"
git -C "$tap" push -q --force-with-lease origin "$branch"
