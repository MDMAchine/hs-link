# Third-party notices

HOT-STEP LINK is licensed under AGPL-3.0-or-later (see `LICENSE`). It is built with JUCE and includes the libraries below as source in `third_party/`. Each folder holds the upstream license file and a `README-VENDORED.txt` with the exact commit and the files taken.

## Signalsmith Stretch

- Use: offline time stretch of a take (`source/TakeStretch.cpp`).
- Upstream: https://github.com/Signalsmith-Audio/signalsmith-stretch, tag 1.4.0, commit a670068d9aeb64913331d5cc29337b19a457a7df.
- License: MIT. Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd.
- Full text: `third_party/signalsmith-stretch/LICENSE.txt`.

## Signalsmith Linear

- Use: FFT and STFT under Signalsmith Stretch.
- Upstream: https://github.com/Signalsmith-Audio/linear, tag 0.6.4, commit de55e6a50ffcf6f8f43f649692d94691c7025151.
- License: MIT. Copyright (c) 2025 Signalsmith Audio.
- Full text: `third_party/signalsmith-linear/LICENSE.txt`.

## JUCE

- Not included in this repository; found at build time (see `CMakeLists.txt`). Used under its AGPLv3 option. https://github.com/juce-framework/JUCE

## clap-juce-extensions

- Use: CLAP wrapper for the plugin (build time only, fetched by CMake).
- Upstream: https://github.com/free-audio/clap-juce-extensions, commit 55525c9858d4b25687be7759a5e0f70eccef218e, plus `cmake/patches/clap-juce-extensions-state.patch`.
- License: MIT (clap-juce-extensions and the bundled CLAP SDK and clap-helpers are MIT). Compatible with AGPL-3.0-or-later.

## Cascadia Mono (logo glyphs)

- Use: the logo text is converted to outlines from Cascadia Mono (`tools/make_logo.py`); no font file is shipped.
- Copyright (c) 2020 Microsoft Corporation (from the font name table, version 2102.025).
- License: SIL Open Font License 1.1. https://openfontlicense.org
