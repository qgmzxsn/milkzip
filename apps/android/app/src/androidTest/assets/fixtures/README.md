These fixtures are instrumentation test data, excluded from the application APK.

- `inner.zip`, `renamed.txt`, `partial.zip`, `bad-path.zip`, `documents.zip`,
  `carrier.mp4`, `payload.*`: generated specifically for MilkZip tests. Payloads
  include Unicode names, nested branches, an unsafe path and application containers.
- `secret.7z`: 7-Zip 26.04, encrypted headers, password `安卓 密码` (test only).
- `split.7z.001` through `.003`: 7-Zip 26.04, STORE, 400-byte volumes; deterministic
  pseudorandom test payload.
- `linked.lz4`, `legacy.lz4`: independent upstream LZ4 CLI encoder output, copied
  from `tests/fixtures/`; see that directory's README for exact options.
- `rar5.rar`: decoded UU fixture `test_read_format_rar5_multiple_files.rar.uu`
  from https://github.com/libarchive/libarchive/tree/master/libarchive/test.
  It exercises the bundled 7-Zip RAR5 reader; libarchive itself is not linked or
  packaged. Its attribution and license are in `libarchive-COPYING.txt`.

GZip fixture assets carry an extra `.fixture` suffix because AAPT otherwise
inflates `.gz` assets during APK packaging. Tests restore the original names.
