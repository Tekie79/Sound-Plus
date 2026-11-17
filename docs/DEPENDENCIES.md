# Dependency record

| Dependency | Version used | Source and terms |
| --- | --- | --- |
| JUCE | 8.0.6 | Official [tagged source](https://github.com/juce-framework/JUCE/tree/8.0.6). Modules are dual licensed AGPLv3 / commercial JUCE. No commercial license is selected or purchased by this build. Preserve `vendor/JUCE-8.0.6/LICENSE.md` and module notices; choose the appropriate distribution license before shipping. |
| SQLite | macOS system library; SDK headers 3.51.0 on the tested Mac | System `libsqlite3`, FTS5 checked by the test suite. SQLite is [public domain](https://sqlite.org/copyright.html). Linked dynamically to the OS. |
| CMake | 3.31.6 | Official Python wheel from PyPI; BSD 3-Clause. Build tool only. |
| Ninja | 1.11.1.3 Python distribution | PyPI wheel; Apache 2.0. Build tool only. |
| Apple Clang / SDK | Apple Clang 21.0.0; macOS SDK 26.5 | Installed Xcode Command Line Tools. System Core Audio, CoreServices, Foundation and AppKit frameworks. |
| Generated test recordings | Repository generator | Deterministic tones generated from sine waves. No third-party media or model weights. |

JUCE source archive SHA-256:

```text
95752f8e8b37d802b4c10182bc757de7d88f0c8899e879afa8798589be306a11
```

JUCE's bundled codec and graphics dependencies retain their own notices, including FLAC, Ogg Vorbis, libpng, JPEG, HarfBuzz and SheenBidi. The upstream license inventory is in the downloaded source's `LICENSE.md`. Plugin hosting, external model runtimes and downloaded AI checkpoints are absent from this milestone's feature set. A full redistribution audit and packaged notice set remain part of OPS-05/OPS-06 qualification.
