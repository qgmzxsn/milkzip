# Third-party components

Original MilkZip code is MIT licensed; see `LICENSE`. The application icon
(`assets/milkzip-mascot.jpg` and the generated `assets/milkzip.ico`) is provided
by the project maintainer and is not covered by an explicit open-source license.

Windows builds use separate, replaceable shared libraries. MinGW
packages and MSVC packages use different compiler runtimes; only
the runtime DLLs actually present in a package apply to that package. No copy of
7-Zip or Qt is statically embedded into the application. Source, version and
license information for the shipped dependencies follows.

| Component | Version | License / source |
| --- | --- | --- |
| LZ4 / xxHash (linked into the core) | LZ4 1.10.0 | BSD 2-Clause. `LICENSES/lz4-license.txt`. Unmodified library sources: https://github.com/lz4/lz4/tree/v1.10.0/lib |
| 7-Zip (`7z.dll`) | 26.04 | LGPL 2.1 with additional unRAR/BSD terms. `LICENSES/7-zip-license.txt`, `LICENSES/lgpl-2.1.txt`, `LICENSES/unrar-license.txt`. Source: https://www.7-zip.org/a/7z2604-src.tar.xz |
| Qt Core, Gui, Widgets, Concurrent, platform/style plugins | 6.8.3 | LGPL 3.0 (see `LICENSES/qt-LGPL-3.0.txt` and `LICENSES/qt-GPL-3.0.txt`). Corresponding source: https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qtbase-everywhere-src-6.8.3.tar.xz |
| Qt Chinese translation (MinGW release packages) | 6.8.3 | Qt license texts above. Source: https://download.qt.io/archive/qt/6.8/6.8.3/submodules/qttranslations-everywhere-src-6.8.3.tar.xz |
| GCC runtime (MinGW builds: `libstdc++-6.dll`, `libgcc_s_seh-1.dll`) | 13.1.0 | GPL 3.0 with GCC Runtime Library Exception. License texts under `LICENSES/MinGW/gcc`. Source: https://ftp.gnu.org/gnu/gcc/gcc-13.1.0/gcc-13.1.0.tar.xz |
| MinGW-w64 / winpthreads (MinGW builds) | 11.0.1 | Notices under `LICENSES/MinGW/mingw-w64` and `LICENSES/MinGW/winpthreads`. Toolchain/source information: https://download.qt.io/online/qtsdkrepository/windows_x86/desktop/tools_mingw1310/qt.tools.win64_mingw1310/ |

MSVC packages instead include Microsoft's Visual C++ runtime
DLLs (`msvcp140*.dll`, `vcruntime140*.dll`, and companion runtime DLLs). Their
versions and Microsoft copyright notices are recorded in the DLL version
resources. These Microsoft components are not licensed under MIT, LGPL or the
GCC exception. MSVC packages do not include Qt translation catalogs or the
MinGW/GCC runtime. Qt dialog labels explicitly provided by the app remain Chinese.

Packages include Qt's license texts for its bundled third-party components under
`LICENSES/Qt`; the MinGW release packaging script additionally copies Qt's SPDX notices. These include the
copyright and license notices from the corresponding Qt source tree. The
build/deployment helper tools themselves are not shipped in the app package.

Users may replace the independent Qt/7-Zip DLLs with compatible builds, including
modified builds, and may debug those modifications. The application imposes no
restriction on reverse engineering for debugging modifications of LGPL components.
No proprietary runtime service, login, activation or network access is required.
