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

