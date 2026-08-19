# gpui-cpp-dist

The single-file build of [gpui-cpp](https://github.com/kjk/gpui-cpp): `gpui.h` and `gpui.cpp`,
amalgamated from that repo's `src/**` plus the vendored md4c by its
`cmd/build-dist.ts`. Nothing here is written by hand, so issues and pull
requests belong in the source repo.

## Use it

Drop both files into your tree, `#include "gpui.h"` where you need the API,
and compile `gpui.cpp` as one more source file. It is C++20, and the platform
halves are already inside it behind `GPUI_OS_*` guards, so the same pair
builds on all three:

- **Windows** — `cl /std:c++20 /EHsc /utf-8 /DUNICODE /D_UNICODE`, static CRT;
  links against the Win32, Direct2D and DirectWrite import libraries.
- **Linux** — `g++ -std=c++20` with `pkg-config --cflags --libs x11 cairo pangocairo`.
- **macOS** — `clang++ -std=c++20 -x objective-c++` with the Cocoa, CoreText and
  IOKit frameworks. The file is Objective-C++ because the mac half is.

No other dependencies, no build system, no STL containers.

## This copy

Amalgamated from gpui-cpp [`a91248de04f1`](https://github.com/kjk/gpui-cpp/commit/a91248de04f13e7b5fd2f29e96f120a3c72e9d45) — cmd: the amalgam is published to its own repo, built and proven before it goes

[What has changed in gpui-cpp since](https://github.com/kjk/gpui-cpp/compare/a91248de04f13e7b5fd2f29e96f120a3c72e9d45...main)
shows every commit this copy is behind by; if that page is empty, it is current.
