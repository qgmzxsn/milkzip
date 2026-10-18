# Windows releases

The supported release toolchain for 0.5.1 is Qt 6.8.3 and its matching MinGW-w64
13.1 POSIX/SEH kit, with the official 7-Zip 26.04 x64 `7z.dll`.

Keep build dependencies outside Git. The native script downloads the pinned
7-Zip interface source if missing and validates its SHA-256. It requires CMake,
Windows tar, and the supplied Qt/compiler folders. Python/JDK are only developer
test tools, not end-user runtime dependencies.

```powershell
./scripts/build-windows-native.ps1 -QtDir C:/deps/Qt `
  -CompilerDir C:/deps/mingw1310_64 -SevenZipDll C:/deps/7zip/7z.dll

py scripts/package-windows.py --build-dir build-windows-qt `
  --qt-dir C:/deps/Qt --compiler-dir C:/deps/mingw1310_64 `
  --sevenzip-dll C:/deps/7zip/7z.dll --output-dir C:/releases/0.5.1 `
  --makensis C:/deps/nsis/makensis.exe
```

Before packaging, retain the license collection under `LICENSES/Qt`. The package
script copies these notices, the matching toolchain license files, Qt SPDX
documents, and the independent runtime DLLs. The stage directory must be new.

The script produces:

- `milkzip-0.5.1-windows-x64-setup.exe`: per-user installation, Start menu
  shortcuts and an uninstall entry, without administrator access.
- `milkzip-0.5.1-windows-x64-portable.zip`: extract the entire folder and run
  `milkzip.exe`.
- `milkzip-0.5.1-source.zip`: current source, including uncommitted edits;
  no build outputs, compiler, downloaded 7-Zip source or private Git metadata.
- `milkzip-0.5.1-SHA256SUMS.txt`: binary/source asset hashes.

Installer testing can use `/S /NOINTEGRATION /D=C:\test-directory`. This installs
files without Start menu or registry integration. The normal interactive
installer does include integration. Uninstall removes only package-owned files;
it never recursively deletes the installation directory or extracted user files.

On GitHub, keep the source in the repository, tag the verified commit, then create
a Release and attach these binaries, source ZIP and hashes. GitHub also generates
`Source code (zip)`/`Source code (tar.gz)` from the tag automatically. Those are
source snapshots, not compiled applications. The build and package scripts do
not upload anything automatically. Publish only after verifying that the source
ZIP matches the tagged commit and the SHA-256 manifest matches all three assets.

For 0.5.1, use tag `v0.5.1` and attach the four files listed above to
<https://github.com/qgmzxsn/milkzip/releases/tag/v0.5.1>.
