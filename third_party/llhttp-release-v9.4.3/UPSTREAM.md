<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 Cezary Manista -->

# llhttp (vendored)

| | |
|---|---|
| Upstream | https://github.com/nodejs/llhttp |
| Version | 9.4.3 (release artifact, pre-generated C sources) |
| Imported | 2026-10-15 |
| License | MIT (`LICENSE-MIT` in this directory) |
| Local changes | none |

`third_party/llhttp` is a symlink to the versioned directory
`third_party/llhttp-release-v9.4.3/`. This file lives in that directory.

## What is used

Only `src/api.c`, `src/http.c`, `src/llhttp.c` and `include/llhttp.h`. The list
of sources is defined in `third_party/llhttp.cmake`.

## Updating

1. Download the release artifact (not the git source, which needs a Node.js
   toolchain to generate the parser) for the new version from the upstream
   releases page.
2. Unpack it to `third_party/llhttp-release-vX.Y.Z/` and copy this file there.
3. Repoint the symlink: `ln -sfn llhttp-release-vX.Y.Z third_party/llhttp`.
4. If the set of source files changed, update `third_party/llhttp.cmake`.
5. Remove the old versioned directory, update the version and import date in
   this file, in `README.md` (Third-party table) and in `CHANGELOG.md`.
6. Build and run the tests (host), and build a Zephyr sample.
