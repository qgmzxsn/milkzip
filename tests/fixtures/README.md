LZ4 fixtures produced with the upstream LZ4 1.10.0 command-line encoder.
Decoded payload: `b'MilkZip LZ4 linked-block fixture\n' * 8192`.

- linked.lz4: `lz4 -BD -B4 -BX --content-size source linked.lz4`.
  Includes linked 64 KiB blocks, block checksums, a content checksum and size.
- legacy.lz4: `lz4 -l source legacy.lz4`.

Fixtures are independent encoder output. Tests need no installed LZ4 program.

NSIS regression: `nsis.exe` is a generated NSIS 3.13 fixture containing only
`nsis-payload.txt` with the bytes `MilkZip NSIS fixture`. Built with makensis,
`SetCompressor /SOLID lzma`, a single Section with SetOutPath $INSTDIR and File.
The NSIS stub and compression notices are retained in LICENSE.nsis.txt.
The fixture is parsed as data and never executed. Android test assets carry the
same fixture plus a 32-layer 7z chain and an MZ-prefixed copy, and a 3 MB BCJ2
payload (500000 repetitions of e8 05 00 00 00 90) to exercise threaded codecs.
