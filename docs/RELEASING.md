<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# Releasing uchttp

This document describes how uchttp is versioned and released.

## Versioning

uchttp follows [Semantic Versioning 2.0.0](https://semver.org/):

| Part   | Bump when…                                                        |
|--------|-------------------------------------------------------------------|
| MAJOR  | you make an incompatible (breaking) API change                    |
| MINOR  | you add functionality in a backward-compatible way                |
| PATCH  | you make a backward-compatible bug fix                            |

While the project is in the `0.x` series the public API is **unstable**:
breaking changes may ship in a MINOR release. `1.0.0` will mark the first
API-stable release.

### Single source of truth

The version lives in **one** place:

```cmake
project(uchttp VERSION X.Y.Z LANGUAGES C CXX)
```

in the top-level `CMakeLists.txt`. From it, CMake generates
`uchttp_version.h` (from `include/uchttp_version.h.in`) into the build
directory, exposing:

```c
UCHTTP_VERSION_MAJOR
UCHTTP_VERSION_MINOR
UCHTTP_VERSION_PATCH
UCHTTP_VERSION_STRING          // "X.Y.Z"
UCHTTP_VERSION                 // integer, for #if comparisons
UCHTTP_VERSION_ENCODE(a,b,c)
```

The git tag (`vX.Y.Z`) must always equal the CMake version; the Release
workflow fails the build if they diverge.

## Branching model

Lightweight trunk-based development:

- `master` is always releasable (CI green).
- Work happens on feature branches merged via pull request.
- A release is just an annotated tag on `master` — there is no long-lived
  `develop` branch.
- Maintenance branches (`release/0.x`) are only created if/when an older line
  needs backported fixes.

## Changelog

`CHANGELOG.md` uses the [Keep a Changelog](https://keepachangelog.com/)
format. Day-to-day, add entries under `## [Unreleased]`. At release time those
entries move under the new version heading.

## Cutting a release

The helper script `scripts/release.sh` automates the mechanical steps and
validates consistency. **It never pushes** — you stay in control of the remote.

1. Make sure `master` is green and your working tree is clean.

2. Move the `[Unreleased]` notes in `CHANGELOG.md` under a new heading and
   update the comparison links at the bottom:

   ```markdown
   ## [0.2.0] - 2026-12-01
   ...moved entries...

   [Unreleased]: https://github.com/cmanista/uchttp/compare/v0.2.0...HEAD
   [0.2.0]: https://github.com/cmanista/uchttp/compare/v0.1.0...v0.2.0
   ```

3. Preview the version bump without touching git:

   ```bash
   scripts/release.sh 0.2.0 --dry-run
   ```

4. Prepare the release commit and annotated tag:

   ```bash
   scripts/release.sh 0.2.0
   ```

   This bumps `CMakeLists.txt`, commits `release: v0.2.0`, and creates the
   annotated tag `v0.2.0`.

5. Review, then push (pushing the tag triggers the Release workflow):

   ```bash
   git show v0.2.0
   git push origin master
   git push origin v0.2.0
   ```

To undo before pushing:

```bash
git tag -d v0.2.0
git reset --hard HEAD~1
```

## Automation (GitHub Actions)

| Workflow              | Trigger                | What it does                                              |
|-----------------------|------------------------|-----------------------------------------------------------|
| `.github/workflows/ci.yml`      | push/PR to `master`    | Build + test with GCC and Clang, Debug and Release        |
| `.github/workflows/release.yml` | push of a `v*` tag     | Verify tag == CMake version, build, test, create a source tarball (`uchttp-X.Y.Z.tar.gz` + `.sha256`), and publish a GitHub Release with notes pulled from `CHANGELOG.md` |

Releases in the `0.x` series are marked as **pre-release** automatically.

> Note: the Zephyr (`native_sim`) build is intentionally not part of CI yet;
> it can be added later once the Zephyr SDK setup is wired in.
