# Contributing to multimaster

Thanks for your interest! multimaster is a small C++20 LAN gossip-mesh library.
Contributions — bug reports, fixes, features, docs — are welcome.

## Reporting bugs

Open a GitHub issue with your distribution, `cmake --version`, compiler version,
whether crypto (libsodium) was enabled, and a minimal reproduction if you can.
Networking bugs are much easier to fix with a `docs/`-style scenario description
(number of peers, who dials whom, what you observed).

## Building & testing

```sh
sudo apt install build-essential cmake pkg-config libsodium-dev
cmake -S . -B build -DMULTIMASTER_BUILD_TESTS=ON -DMULTIMASTER_BUILD_EXAMPLES=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.20 and a C++20 compiler. Some tests exercise real UDP
multicast / loopback sockets; run them on a machine where loopback networking is
available.

## Coding style

- **clang-format, Google style** — the `.clang-format` at the repo root is
  authoritative. Run `clang-format -i` on the files you touch.
- Keep pure-reformatting changes in their **own commit** (see
  `.git-blame-ignore-revs`).
- Warnings are treated seriously (`-Wall -Wextra -Wpedantic`); keep the build
  warning-clean.

## Design

The internals — components, threading model, wire protocol, gossip routing and
self-healing — are documented in [`docs/architecture.md`](docs/architecture.md)
and [`docs/how-the-mesh-works.md`](docs/how-the-mesh-works.md). Read them before
changing the protocol or routing.

## Licensing of contributions

multimaster is **LGPL-3.0-or-later**. By contributing you agree to license your
work under those terms. Add an SPDX header to new source files:

```cpp
// SPDX-License-Identifier: LGPL-3.0-or-later
// SPDX-FileCopyrightText: <year> <your name>
```

Please sign off your commits (`git commit -s`) per the
[Developer Certificate of Origin](https://developercertificate.org/).

## Pull requests

- Branch off `master`; keep PRs focused; make sure CI (build + tests) is green.
