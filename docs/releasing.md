# Releasing msga

Releases are built, tested and published by GitHub Actions on the public repo,
`punarinta/make-slack-great-again`. The workflow is `.github/workflows/release.yml`.
Nothing runs on `master`. The private dev repo has no CI.

## The flow

1. Sync the mirror (`scripts/sync-to-public.sh`) and
   commit to the mirror's `master` as usual.
2. On GitHub, open a pull request from `master` into `release`. **Its description becomes the
   release notes.**
   - The PR runs the full pipeline without publishing:
     - Linux static (musl) with tests and an X11 smoke launch
     - Windows (mingw) with a Wine smoke launch
     - FreeBSD and OpenBSD (cross-built)
     - macOS arm64 with tests and the DMG, built on macOS 26; the same build and tests run
       on macOS 15 as a check for older systems
     - macOS x86_64 (Intel Macs) with tests and its own DMG, built on `macos-15-intel`.
       GitHub keeps that runner "until August 2027" (actions/runner-images#13045); after
       that the Intel build has to be cross-compiled on an arm64 runner
     - a Flatpak build and lint
   - A red check means don't merge. Fix it on `master` and sync again; the PR re-runs by itself.
3. Merge the PR (a merge commit). If nothing landed on `master` since the PR's checks, the
   release is the exact code the PR tested: its binaries are published at once, no builds run
   again. Otherwise the builds run on `release` first. Either way, it then publishes:
   - a GitHub Release tagged with the date (`2026-11-18`; a second one that day is
     `2026-11-18-2`), with all binaries and their `.manifest` files
   - the Homebrew cask in `punarinta/homebrew-msga`, rendered from `packaging/homebrew/msga.rb.in`
   - msga's own Flatpak repository, signed, at `https://msga.app/flatpak/` (job `flatpak-site`,
     a GitHub Pages site at `flatpak.msga.app` that nginx proxies). Users install with
     `flatpak install --user https://msga.app/flatpak/com.nisdos.msga.flatpakref`, then
     `flatpak update` follows the releases.

Debug symbols (`.debug`, `.dSYM`) are kept as workflow artifacts named `symbols-<os>-<tag>`
(`symbols-macos-x86_64-<tag>` for the Intel Mac build) for 90 days. They're never release assets.

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
| `FLATPAK_SIGNING_KEY` | secret | the armored secret GPG key that signs the Flatpak repository | repository not published (warning) |

No app keys are involved. Builds carry no Slack or GIPHY credentials; users add their own.

## Trying it without publishing

Actions → Release → **Run workflow** on any branch:
- **build**: the PR checks only. Nothing is published.
- **draft**: also creates a **draft** release. Drafts are invisible to the public and have no tag
  until published, so `latest` and msga.app are untouched. The cask goes to the tap's `ci-test`
  branch, which `brew` never reads. The Flatpak repository isn't published. Delete the draft and the `ci-test` branch
  afterwards.

## Adding the Intel Mac to a published release

**Run workflow**, mode **backfill**, builds the macOS x86_64 DMG from the latest release's tag, as
that release's version, on `macos-15-intel` with the full tests. It then adds the DMG and its
manifest to that release and renders the cask for both Macs. Nothing else in the release changes,
and an asset that's already there is never replaced: the run refuses a release that has the DMG.
The tag predates the Intel entry in the updater's asset map, so `src/app/update/updater.cpp` and its
test come from the run's own commit; the job fails if they differ from the tag in more than
x86_64 lines. It exists for release 48, published before the Intel build; every release since
builds the Intel DMG itself.

## Cost

Standard GitHub-hosted runners are free for public repos, macOS included.

## The Flatpak repository

msga publishes its own Flatpak repository instead of going through Flathub, whose policy wants a
hand-written manifest and no AI-assisted packaging. `packaging/flatpak/publish-repo.sh` turns the
CI's Flatpak build into the site: the app on the `stable` branch, commits and summary signed with
`FLATPAK_SIGNING_KEY`, static deltas, `com.nisdos.msga.flatpakref` and `msga.flatpakrepo`. The
runtime comes from Flathub (`RuntimeRepo`).

**Run workflow**, mode **flatpak**, republishes it from the latest release tag without a new
release (e.g. after changing the key or the site).

The signing key never changes in normal use: users' remotes carry its public part, so a new key
means everyone re-adds the repository. Keep the backup of its secret part safe.

## Versions

Nobody types a version. Each run takes the last release's version (from its `.manifest`) and adds
one; every build writes that into `version.cmake` before compiling, the Flatpak's metainfo gets a
matching `<release>`, and once the release is out the publish job commits the number to `master`
(`version N`). `sync-to-public.sh` doesn't copy `version.cmake`, so the dev repo's value never
overwrites it; there it only names local builds. The **flatpak** mode rebuilds the last release
under its own version.
