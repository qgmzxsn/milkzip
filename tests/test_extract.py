#!/usr/bin/env python3
"""Create tiny original archives and check the phase 1 core. No sample password is stored."""

import os
import secrets
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import zipfile
import zlib

RESULT = {
    "ok": 0,
    "bad-argument": 1,
    "io": 2,
    "format": 3,
    "engine": 4,
    "password-required": 5,
    "password-rejected": 6,
    "path": 7,
    "unsupported": 8,
}


def crc_table():
    table = []
    for n in range(256):
        c = n
        for _ in range(8):
            c = (c >> 1) ^ 0xEDB88320 if c & 1 else c >> 1
        table.append(c & 0xFFFFFFFF)
    return table


TABLE = crc_table()


def crc32_byte(crc, byte):
    return (TABLE[(crc ^ byte) & 0xFF] ^ (crc >> 8)) & 0xFFFFFFFF


class ZipCrypto:
    def __init__(self, password):
        self.key0 = 305419896
        self.key1 = 591751049
        self.key2 = 878082192
        for byte in password:
            self.update(byte)

    def update(self, byte):
        self.key0 = crc32_byte(self.key0, byte)
        self.key1 = (self.key1 + (self.key0 & 0xFF)) & 0xFFFFFFFF
        self.key1 = (self.key1 * 134775813 + 1) & 0xFFFFFFFF
        self.key2 = crc32_byte(self.key2, (self.key1 >> 24) & 0xFF)

    def stream(self, data):
        out = bytearray()
        for byte in data:
            temp = (self.key2 | 2) & 0xFFFF
            cipher = byte ^ (((temp * (temp ^ 1)) >> 8) & 0xFF)
            self.update(byte)
            out.append(cipher)
        return bytes(out)


def write_encrypted_zip(path, member_name, data, password):
    crc = zlib.crc32(data) & 0xFFFFFFFF
    crypto = ZipCrypto(password)
    header = secrets.token_bytes(11) + bytes([(crc >> 24) & 0xFF])
    encrypted = crypto.stream(header + data)
    name = member_name.encode("utf-8")
    flags = 1
    local = struct.pack(
        "<IHHHHHIIIHH",
        0x04034B50,
        20,
        flags,
        0,
        0,
        0,
        crc,
        len(encrypted),
        len(data),
        len(name),
        0,
    ) + name
    central = struct.pack(
        "<IHHHHHHIIIHHHHHII",
        0x02014B50,
        20,
        20,
        flags,
        0,
        0,
        0,
        crc,
        len(encrypted),
        len(data),
        len(name),
        0,
        0,
        0,
        0,
        0,
        0,
    ) + name
    eocd = struct.pack(
        "<IHHHHIIH",
        0x06054B50,
        0,
        0,
        1,
        1,
        len(central),
        len(local) + len(encrypted),
        0,
    )
    with open(path, "wb") as handle:
        handle.write(local + encrypted + central + eocd)


def run(cli, args, password=None):
    proc = subprocess.run(
        [cli, *args],
        input=(password + "\n") if password is not None else None,
        text=True,
        capture_output=True,
        check=False,
    )
    return proc.returncode, proc.stdout, proc.stderr


def fail(message):
    print("FAIL:", message, file=sys.stderr)
    return 1


def main():
    if len(sys.argv) != 4:
        print("usage: test_extract.py <archiver-cli> <libarchiver.so> <7z.so>", file=sys.stderr)
        return 2
    cli, library, engine = sys.argv[1:]
    needed = subprocess.check_output(["readelf", "-d", library], text=True)
    if "7z.so" in needed:
        return fail("libarchiver.so is linked to 7z.so; it must dlopen it")
    symbols = subprocess.check_output(["nm", "-D", engine], text=True)
    if "CreateObject" not in symbols:
        return fail("7z.so does not export CreateObject")

    errors = 0
    with tempfile.TemporaryDirectory() as tmp:
        hello = os.path.join(tmp, "hello.txt")
        with open(hello, "w", encoding="utf-8") as handle:
            handle.write("phase-1\n")

        cases = []
        zip_path = os.path.join(tmp, "plain.zip")
        with zipfile.ZipFile(zip_path, "w") as zf:
            zf.write(hello, "dir/hello.txt")
        cases.append(("zip", zip_path))

        tar_path = os.path.join(tmp, "plain.tar")
        with tarfile.open(tar_path, "w") as tf:
            tf.add(hello, "dir/hello.txt")
        cases.append(("tar", tar_path))

        for mode, name in (("w:gz", "a.tar.gz"), ("w:bz2", "a.tar.bz2"), ("w:xz", "a.tar.xz")):
            path = os.path.join(tmp, name)
            with tarfile.open(path, mode) as tf:
                tf.add(hello, "dir/hello.txt")
            cases.append((name, path))

        zst_cli = None
        for candidate in ("zstd", "/usr/bin/zstd"):
            if subprocess.call(["sh", "-c", f"command -v {candidate}"], stdout=subprocess.DEVNULL) == 0:
                zst_cli = candidate
                break
        if zst_cli:
            zst_path = os.path.join(tmp, "a.tar.zst")
            subprocess.check_call(
                f"tar -C {tmp} -cf - hello.txt | {zst_cli} -q -o {zst_path}",
                shell=True,
            )
            # The tar above stores hello.txt at the root. Rebuild with a directory name.
            os.remove(zst_path)
            with tarfile.open(os.path.join(tmp, "inner.tar"), "w") as tf:
                tf.add(hello, "dir/hello.txt")
            subprocess.check_call([zst_cli, "-q", "-f", os.path.join(tmp, "inner.tar"), "-o", zst_path])
            cases.append(("tar.zst", zst_path))
        else:
            print("skip tar.zst: zstd is not installed")

        seven = None
        try:
            import py7zr
            seven = os.path.join(tmp, "plain.7z")
            with py7zr.SevenZipFile(seven, "w") as zf:
                zf.write(hello, "dir/hello.txt")
            cases.append(("7z", seven))
        except Exception as exc:
            print("skip 7z:", exc)

        for label, path in cases:
            dest = os.path.join(tmp, "out-" + label.replace(".", "-"))
            os.makedirs(dest)
            code, out, err = run(cli, ["extract", path, dest, "--engine", engine])
            text = os.path.join(dest, "dir", "hello.txt")
            if label == "tar.zst" and not os.path.exists(os.path.join(dest, "dir", "hello.txt")):
                text = os.path.join(dest, "hello.txt")
            if code != 0 or not os.path.exists(text):
                print(label, code, out, err, file=sys.stderr)
                errors += fail(label + " extract")
            elif open(text, encoding="utf-8").read() != "phase-1\n":
                errors += fail(label + " contents")
            else:
                print("ok", label)

        renamed = os.path.join(tmp, "notes.dat")
        with open(zip_path, "rb") as src, open(renamed, "wb") as dst:
            dst.write(src.read())
        code, out, err = run(cli, ["list", renamed, "--engine", engine])
        if code != 0:
            print(code, out, err, file=sys.stderr)
            errors += fail("ZIP with an unknown extension should be detected by content")
        else:
            print("ok wrong-extension detected")

        split = os.path.join(tmp, "vol.7z.001")
        with open(zip_path, "rb") as src, open(split, "wb") as dst:
            dst.write(src.read())
        code, out, err = run(cli, ["list", split, "--engine", engine])
        if code != 0:
            print(code, out, err, file=sys.stderr)
            errors += fail("numeric volume names should preserve content detection")
        else:
            print("ok misnamed numeric volume detected")

        slip = os.path.join(tmp, "slip.zip")
        with zipfile.ZipFile(slip, "w") as zf:
            zf.writestr("../outside.txt", "nope")
            zf.writestr("keep.txt", "yes")
        dest = os.path.join(tmp, "slip-out")
        os.makedirs(dest)
        code, out, err = run(cli, ["extract", slip, dest, "--engine", engine])
        outside = os.path.join(tmp, "outside.txt")
        if code != RESULT["path"] or os.path.exists(outside) or os.path.exists(os.path.join(dest, "keep.txt")):
            print(code, out, err, file=sys.stderr)
            errors += fail("zip slip")
        else:
            print("ok zip slip rejected")

        tar_slip = os.path.join(tmp, "slip.tar")
        with tarfile.open(tar_slip, "w") as tf:
            info = tarfile.TarInfo("sub/escape")
            info.type = tarfile.SYMTYPE
            info.linkname = "../../outside.txt"
            tf.addfile(info)
            data = b"inside\n"
            info = tarfile.TarInfo("sub/ok.txt")
            info.size = len(data)
            tf.addfile(info, fileobj=__import__("io").BytesIO(data))
        dest = os.path.join(tmp, "tar-slip-out")
        os.makedirs(dest)
        code, out, err = run(cli, ["extract", tar_slip, dest, "--engine", engine])
        if code != RESULT["path"] or os.path.lexists(os.path.join(dest, "sub", "escape")):
            print(code, out, err, file=sys.stderr)
            errors += fail("tar symlink escape")
        else:
            print("ok tar symlink escape rejected")

        safe_tar = os.path.join(tmp, "safe-link.tar")
        with tarfile.open(safe_tar, "w") as tf:
            data = b"inside\n"
            info = tarfile.TarInfo("sub/ok.txt")
            info.size = len(data)
            tf.addfile(info, fileobj=__import__("io").BytesIO(data))
            info = tarfile.TarInfo("sub/link")
            info.type = tarfile.SYMTYPE
            info.linkname = "ok.txt"
            tf.addfile(info)
        dest = os.path.join(tmp, "safe-link-out")
        os.makedirs(dest)
        code, out, err = run(cli, ["extract", safe_tar, dest, "--engine", engine])
        link = os.path.join(dest, "sub", "link")
        if code != 0 or not os.path.islink(link) or os.readlink(link) != "ok.txt":
            print(code, out, err, os.path.lexists(link), file=sys.stderr)
            errors += fail("safe tar symlink")
        else:
            print("ok safe tar symlink")

        marker = os.path.join(tmp, "was-executed")
        script_tar = os.path.join(tmp, "script.tar")
        payload = b"#!/bin/sh\necho pwned > " + marker.encode() + b"\n"
        with tarfile.open(script_tar, "w") as tf:
            info = tarfile.TarInfo("payload.sh")
            info.size = len(payload)
            info.mode = 0o755
            tf.addfile(info, fileobj=__import__("io").BytesIO(payload))
        dest = os.path.join(tmp, "script-out")
        os.makedirs(dest)
        code, out, err = run(cli, ["extract", script_tar, dest, "--engine", engine])
        extracted = os.path.join(dest, "payload.sh")
        mode = os.stat(extracted).st_mode if os.path.exists(extracted) else 0
        if code != 0 or os.path.exists(marker) or mode & stat.S_IXUSR:
            print(code, out, err, oct(mode), file=sys.stderr)
            errors += fail("extracted program was executable or ran")
        else:
            print("ok did not run extracted file")

        password = secrets.token_hex(16)
        enc = os.path.join(tmp, "enc.zip")
        write_encrypted_zip(enc, "secret.txt", b"hidden\n", password.encode())
        code, out, err = run(cli, ["list", enc, "--engine", engine])
        if code != RESULT["password-required"]:
            # Traditional ZIP often opens without a password and asks at extract time.
            if code != 0:
                print("encrypted list", code, out, err, file=sys.stderr)
                errors += fail("encrypted zip list")
            else:
                print("ok encrypted zip lists before the password")
        else:
            print("ok encrypted zip asks at open")
        if password in out or password in err:
            errors += fail("password leaked in list output")
        dest = os.path.join(tmp, "enc-out")
        os.makedirs(dest)
        code, out, err = run(cli, ["extract", enc, dest, "--engine", engine], password=password)
        if code != 0 or open(os.path.join(dest, "secret.txt"), "rb").read() != b"hidden\n":
            print(code, out, err, file=sys.stderr)
            errors += fail("encrypted zip extract")
        elif password in out or password in err:
            errors += fail("password leaked in extract output")
        else:
            print("ok encrypted zip")
        dest = os.path.join(tmp, "enc-bad")
        os.makedirs(dest)
        wrong = secrets.token_hex(16)
        code, out, err = run(cli, ["extract", enc, dest, "--engine", engine], password=wrong)
        if code == 0:
            errors += fail("wrong password was accepted")
        elif wrong in out or wrong in err:
            errors += fail("wrong password was printed")
        else:
            print("ok wrong password rejected", code)

        if seven is not None:
            import py7zr
            enc7 = os.path.join(tmp, "enc.7z")
            pw7 = secrets.token_hex(16)
            with py7zr.SevenZipFile(enc7, "w", password=pw7) as zf:
                zf.write(hello, "dir/hello.txt")
            code, out, err = run(cli, ["list", enc7, "--engine", engine])
            if code not in (0, RESULT["password-required"]) or pw7 in out or pw7 in err:
                print(code, out, err, file=sys.stderr)
                errors += fail("7z encrypted list")
            else:
                print("ok 7z encrypted list", code)
            dest = os.path.join(tmp, "enc7-need")
            os.makedirs(dest)
            code, out, err = run(cli, ["extract", enc7, dest, "--engine", engine])
            if code != RESULT["password-required"] or pw7 in out or pw7 in err:
                print(code, out, err, file=sys.stderr)
                errors += fail("7z should ask for a password at extract")
            else:
                print("ok 7z password required")
            dest = os.path.join(tmp, "enc7-out")
            os.makedirs(dest)
            code, out, err = run(cli, ["extract", enc7, dest, "--engine", engine], password=pw7)
            got = os.path.join(dest, "dir", "hello.txt")
            if code != 0 or not os.path.exists(got) or pw7 in out or pw7 in err:
                print(code, out, err, file=sys.stderr)
                errors += fail("7z encrypted extract")
            else:
                print("ok 7z encrypted extract")

    if errors:
        print(errors, "failed")
        return 1
    print("extract checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
