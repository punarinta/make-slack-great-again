# Releasing msga

Releases are built, tested and published by GitHub Actions on the public repo,
`punarinta/make-slack-great-again`. The workflow is `.github/workflows/release.yml`.
Nothing runs on `master`. The private dev repo has no CI.

## The flow

1. Bump `MSGA_VERSION` in `version.cmake`, sync the mirror (`scripts/sync-to-public.sh`) and
   commit to the mirror's `master` as usual.
2. On GitHub, open a pull request from `master` into `release`. **Its description becomes the
   release notes.**
   - The PR runs the full pipeline without publishing:
     - Linux static (musl) with tests and an X11 smoke launch
     - Windows (mingw) with a Wine smoke launch
     - FreeBSD and OpenBSD (cross-built)
     - macOS arm64 with tests and the DMG
     - a Flatpak build and lint
   - A red check means don't merge. Fix it on `master` and sync again; the PR re-runs by itself.
   - If the version wasn't bumped past the last release, that's a red check too.
3. Merge the PR. The same builds run again on `release`, then publish:
   - a GitHub Release tagged with the date (`2026-11-18`; a second one that day is
     `2026-11-18-2`), with all binaries and their `.manifest` files
   - the Homebrew cask in `punarinta/homebrew-msga`, rendered from `packaging/homebrew/msga.rb.in`
   - a version-bump PR on `flathub/com.nisdos.msga`, which Flathub then builds from the tag

Debug symbols (`.debug`, `.dSYM`) are kept as workflow artifacts named `symbols-<os>-<tag>`
for 90 days. They're never release assets.

## msga.app downloads

`website/msga.app.conf` forwards `msga.app/download/*` to the newest release:
- binaries get a 302 redirect
- manifests are relayed, because the site's script needs a CORS header that GitHub doesn't send

So the URLs used by the site, the in-app updater and old installs never change, and a release
needs no site deploy. `website/deploy.sh` is only for changes to the site's own content.

## Secrets and variables

Set these in the mirror under Settings → Secrets and variables → Actions:

| Name | Kind | What | Without it |
|---|---|---|---|
| `HOMEBREW_TAP_TOKEN` | secret | fine-grained PAT: `punarinta/homebrew-msga` only, Contents read/write | cask not updated (warning) |
| `FLATHUB_TOKEN` | secret | PAT with write access to `flathub/com.nisdos.msga` | Flathub not updated (warning) |
| `FLATHUB_AUTOMERGE` | variable | `true`: auto-merge the Flathub PR once its test build passes | merge the Flathub PR by hand |

No app keys are involved. Builds carry no Slack or GIPHY credentials; users add their own.

## Trying it without publishing

Actions → Release → **Run workflow** on any branch:
- **build**: the PR checks only. Nothing is published.
- **draft**: also creates a **draft** release. Drafts are invisible to the public and have no tag
  until published, so `latest` and msga.app are untouched. The cask goes to the tap's `ci-test`
  branch, which `brew` never reads. Flathub is skipped. Delete the draft and the `ci-test` branch
  afterwards.

## Cost

Standard GitHub-hosted runners are free for public repos, macOS included.
