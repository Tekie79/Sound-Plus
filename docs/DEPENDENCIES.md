# Dependency record

| Dependency | Version used | Source and terms |
| --- | --- | --- |
| JUCE | 8.0.6 | Official [tagged source](https://github.com/juce-framework/JUCE/tree/8.0.6). Modules are dual licensed AGPLv3 / commercial JUCE. No commercial license is selected or purchased by this build. Preserve `vendor/JUCE-8.0.6/LICENSE.md` and module notices; choose the appropriate distribution license before shipping. |
| SQLite | macOS system library; SDK headers 3.51.0 on the tested Mac | System `libsqlite3`, FTS5 checked by the test suite. SQLite is [public domain](https://sqlite.org/copyright.html). Linked dynamically to the OS. |
| CMake | 3.31.6 | Official Python wheel from PyPI; BSD 3-Clause. Build tool only. |
| Ninja | 1.11.1.3 Python distribution | PyPI wheel; Apache 2.0. Build tool only. |
| Apple Clang / SDK | Apple Clang 21.0.0; macOS SDK 26.5 | Installed Xcode Command Line Tools. System Core Audio, CoreServices, Foundation and AppKit frameworks. |
| ONNX Runtime | 1.22.1, official Microsoft native osx-arm64 binary | [Microsoft.ML.OnnxRuntime NuGet package](https://www.nuget.org/packages/Microsoft.ML.OnnxRuntime/1.22.1), MIT plus packaged third-party notices. Downloaded and verified by `scripts/fetch_runtime.py`; bundled only in the AI helper's runtime path. Upstream source commit `89746dc19a0a1ae59ebf4b16df9acab8f99f3925`. Requires macOS 13.3+. |
| Paired CLAP model | `laion/clap-htsat-unfused`, quantized Xenova ONNX conversion at `c28f2883575e590e04d3146ff0713c2448d691ba` | [LAION checkpoint card](https://huggingface.co/laion/clap-htsat-unfused) declares Apache-2.0. [Conversion repository](https://huggingface.co/Xenova/clap-htsat-unfused) supplies paired encoders, configs and tokenizer. Downloaded separately on request, not embedded in the app. Exact URLs, sizes and SHA-256 in `resources/clap-model.json`. |
| CLAP upstream code license | CC0-1.0 | [LAION-AI/CLAP license](https://github.com/LAION-AI/CLAP/blob/1fd4c37df5ffbfcfbad5415c170bc66cf94c9994/LICENSE). This is distinct from the checkpoint card's Apache-2.0 declaration. Both notices retained. |
| Reference preprocessing/tokenizer tools | Transformers 4.46.3 `audio_utils`, tokenizers 0.22.2, NumPy 2.3.5 | Optional build-time parity checks only; Apache-2.0 / Apache-2.0 / BSD-3-Clause respectively. No Python packages run inside or ship with the app. Native BPE, mel features and resampling implemented in C++. |
| Generated test recordings | Repository generators | Deterministic tones for transport and integrity tests. Not used to claim semantic relevance. |
| Real evaluation recordings | ESC-50 subset, revision `33c8ce9eb2cf0b1c2f8bcf322eb349b6be34dbb6` | [ESC-50](https://github.com/karolpiczak/ESC-50) compilation CC BY-NC 3.0. Forty attributed excerpts plus three explicitly derived composites, evaluation only, never bundled with the application. Manifest retains original filenames, Freesound IDs/links, download URLs and hashes outside the indexed media directory. |

JUCE source archive SHA-256:

```text
95752f8e8b37d802b4c10182bc757de7d88f0c8899e879afa8798589be306a11
```

The ONNX Runtime archive SHA-256 is `7b091e012dcfef2f9cc965f2252041101b1840c748ffbc1c18ba047bd6508b1c`; the bundled native library is `36a46aeb65eee6618daded125577192514d058d0cce6adc4892bc1bcc77cb9be` (33,548,864 bytes). Its MIT license, third-party notices and model-source notices are copied into `SoundPlus.app/Contents/Resources/Licenses`.

JUCE's bundled codec and graphics dependencies retain their own notices, including FLAC, Ogg Vorbis, libpng, JPEG, HarfBuzz and SheenBidi. The upstream license inventory is in the downloaded source's `LICENSE.md`. A full redistribution audit, distribution license choice, signing and notarization remain OPS-05/OPS-06 qualification work. Plugin hosting is absent.
