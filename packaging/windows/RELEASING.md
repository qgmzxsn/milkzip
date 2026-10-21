# Windows releases

The v0.6.0 portable release uses the repository's `Windows offline build`
workflow: Windows Server 2022, MSVC 2022 x64, Qt 6.8.3, the pinned 7-Zip 26.04
source and the vendored LZ4 1.10.0 library. No build tool is required by users.

1. Update CMake, `ARCHIVER_VERSION`, the Android version name, package metadata,
   workflow artifact names and README download links together.
2. Run the Windows workflow on the intended PR head. Require all core, Qt GUI
   and Windows Credential Manager tests, staged engine smoke test and deployed
   GUI startup to pass. The package includes its Qt/MSVC runtime and licenses.
3. Download the workflow artifact and verify ZIP integrity, required files,
   embedded application version and `SHA256SUMS.txt` against the portable ZIP.
4. Merge with the expected PR head. Verify that the merge commit's tree matches
   the tested head, then tag the merge commit `v0.6.0`.
5. Create a draft GitHub Release with these verified assets:
   - `milkzip-0.6.0-windows-x64-portable.zip`
   - `SHA256SUMS.txt` (SHA256 followed by the file name)
6. Publish the release as Latest. Check the public release, tag commit, README
   links and unauthenticated downloads, including a fresh checksum comparison.

GitHub generates source ZIP/tar archives from the tag. They are source snapshots,
not Windows applications. The application and its bundled guide must both report
0.6.0; do not rename an older binary to make a new version's release package.

`scripts/build-windows-native.ps1`, `scripts/package-windows.py` and
`installer.nsi` retain an alternate MinGW/NSIS packaging route. That route needs
the matching Qt/MinGW kit and its notices and is separate from the MSVC portable
release. v0.6.0 does not claim a validated installer, macOS package or Android APK.
