LZ4 fixtures produced with the upstream LZ4 1.10.0 command-line encoder.
Decoded payload: `b'MilkZip LZ4 linked-block fixture\n' * 8192`.

- linked.lz4: `lz4 -BD -B4 -BX --content-size source linked.lz4`.
  Includes linked 64 KiB blocks, block checksums, a content checksum and size.
- legacy.lz4: `lz4 -l source legacy.lz4`.

Fixtures are independent encoder output. Tests need no installed LZ4 program.
