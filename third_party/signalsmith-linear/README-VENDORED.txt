Signalsmith Linear (FFT and STFT), vendored for HOT-STEP LINK as the dependency
of ../signalsmith-stretch.

Upstream:  https://github.com/Signalsmith-Audio/linear
Tag:       0.6.4 (the tag pinned by the CMakeLists.txt of signalsmith-stretch 1.4.0)
Commit:    de55e6a50ffcf6f8f43f649692d94691c7025151 (2026-09-25)
Taken:     2026-10-08
License:   MIT, see LICENSE.txt

Files taken, byte-identical to upstream (third_party/.gitattributes turns line
ending conversion off):
  stft.h
  fft.h
  LICENSE.txt

Not taken: linear.h, approx.h, platform/, include/, tests/, CMakeLists.txt,
README.md, SUPPORT.txt. The platform/ headers are only included under the
SIGNALSMITH_USE_* defines; HOT-STEP LINK defines none of them, so the plain
portable FFT is used.
To update: see docs/DEVELOPING.md, "Vendored libraries".
