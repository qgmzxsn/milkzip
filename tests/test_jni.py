#!/usr/bin/env python3
"""Exercise the real JNI boundary on a host JVM (no Android device required)."""
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import tempfile
import zipfile

from test_extract import write_encrypted_zip

java, javac, library, jni, engine = sys.argv[1:6]
seven = sys.argv[6] if len(sys.argv) > 6 else None
library, jni, engine = (Path(p).resolve() for p in (library, jni, engine))
with tempfile.TemporaryDirectory(prefix="jni-", dir=library.parent) as directory:
    base = Path(directory).resolve()
    assert base.is_relative_to(library.parent)
    source = Path(__file__).parent / "java/app/localarchiver/ArchiverNative.java"
    subprocess.run([javac, "-encoding", "UTF-8", "-d", str(base), str(source)], check=True)
    archive = base / "压缩😀.zip"
    with zipfile.ZipFile(archive, "w") as zf:
        zf.writestr("目录😀/文件🦊.txt", "unicode")
    damaged = base / "damaged.zip"
    with zipfile.ZipFile(damaged, "w") as zf:
        zf.writestr("hello.txt", "hello")
    raw = bytearray(damaged.read_bytes())
    name_len, extra_len = struct.unpack_from("<HH", raw, 26)
    raw[30 + name_len + extra_len] ^= 1
    damaged.write_bytes(raw)
    password = secrets.token_hex(16)
    suffix = ".7z" if seven else ".zip"
    encrypted = base / ("加密🔑" + suffix)
    if seven:
        password += "密码🔑"
        source = base / "secret.txt"
        source.write_bytes(b"secret")
        subprocess.run([seven, "a", "-t7z", "-p" + password, "-mhe=on", str(encrypted),
                        str(source)], check=True, capture_output=True, timeout=30)
    else:
        # Legacy ZipCrypto has platform-dependent non-ASCII password encodings.
        write_encrypted_zip(encrypted, "secret.txt", b"secret", password.encode("ascii"))
    if seven:
        machine = base / "machine.bin"
        machine.write_bytes((b"\xe8\x05\x00\x00\x00\x90" * 500000))
        subprocess.run([seven, "a", "-t7z", "-m0=BCJ2", "-m1=LZMA2", "-m2=LZMA2", "-m3=LZMA2",
            "-mb0:1", "-mb0s1:2", "-mb0s2:3", str(base / "threaded.7z"), str(machine)],
            check=True, capture_output=True, timeout=60)
    subprocess.run(
        [java, "-Xcheck:jni", "-cp", str(base), "app.localarchiver.ArchiverNative",
         str(library), str(jni), str(engine), str(base), suffix],
        input=password.encode("utf-8"), check=True, timeout=30,
    )
