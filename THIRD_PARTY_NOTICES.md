# Third-party notices

Nocturne uses Qt 6 and other third-party libraries, distributed as separate DLLs.
The Windows package includes supplier license texts in `licenses/`, a file-to-package
inventory in `THIRD_PARTY_SOURCES.json`, and exact-version source archive links in
`THIRD_PARTY_SOURCES.md`.

Qt Core, GUI, Widgets and SQL are used under the GNU Lesser General Public License
version 3. Qt is copyright The Qt Company Ltd. and its contributors. Qt's bundled
third-party components retain their own notices and terms.

The distributed libraries are dynamically loaded. You may replace them with
compatible modified versions and reverse engineer the combined work as needed
to debug modifications to those libraries, as provided by their licenses.
Nocturne does not require a vendor signature on replacement DLLs.

The MSYS2 source archives linked next to this release provide the corresponding
sources, distribution patches and PKGBUILD build recipes for the exact installed
package versions. The archives are served without a charge at
https://repo.msys2.org/mingw/sources/ . Source access does not require installing
or running Nocturne.

Other included libraries include Brotli, bzip2, double-conversion, FreeType,
GCC runtime libraries, GNU gettext and libiconv, GLib, Graphite2, HarfBuzz,
ICU, libb2, libjpeg-turbo, libpng, winpthreads, MD4C, PCRE2, SQLite, zlib and
Zstandard. Their original supplier texts are included in the package.
FreeType is used under the FreeType License; Zstandard under its BSD license.
GCC runtime exception texts are included alongside the GCC licenses.

The package-level license declarations in the JSON inventory may also describe
supplier tools or documentation that are not part of this application package.
They do not mean that every listed license applies to every shipped DLL.

These notices describe third-party components; they do not assign a new license
to Nocturne's own source code or branding.

Native formula rendering also statically includes MicroTeX (MIT, copyright
2020 Nano Michael), revision `0e3707f6dafebb121d98b53c64364d16fefe481d`, and
TinyXML-2 10.0.0 (zlib license), revision `321ea883b7190d4e85cae5512a12e5eaa8f8731f`.
Pinned source URLs and SHA-256 digests are recorded in `scripts/MathRuntime.cmake`.
MicroTeX's Qt adapter is adapted for Qt 6, embedded resources and stable outline
glyph drawing; no external TeX engine is used. Its unmodified bundled math fonts
retain the OFL, Knuth and dsrom notices included under `licenses/MicroTeX/licences`.
Original library licenses are included under `licenses/MicroTeX` and
`licenses/tinyxml2`.

Source: [MicroTeX](https://github.com/NanoMichael/MicroTeX/tree/0e3707f6dafebb121d98b53c64364d16fefe481d),
[TinyXML-2](https://github.com/leethomason/tinyxml2/tree/321ea883b7190d4e85cae5512a12e5eaa8f8731f).

Optional offline speech support downloads sherpa-onnx 1.13.8 (Apache-2.0) and its
supplier-provided ONNX Runtime (MIT), together with the referenced Zipformer
Chinese model. These binaries and model weights are not bundled in the base
application package. The installer pins the two supplier archives by SHA-256;
the original runtime license texts are shipped under `voice-licenses/` and
copied into the installed component's `licenses/` directory.
See [sherpa-onnx](https://github.com/k2-fsa/sherpa-onnx/releases/tag/v1.13.8),
[ONNX Runtime](https://github.com/microsoft/onnxruntime), and
[the model documentation](https://k2-fsa.github.io/sherpa/onnx/pretrained_models/online-ctc/zipformer-ctc-models.html#sherpa-onnx-streaming-zipformer-small-ctc-zh-int8-2025-04-01-chinese).
Windows audio capture and decoding use operating-system APIs; the operating
system components themselves are not redistributed by Nocturne.
