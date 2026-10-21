#!/usr/bin/env python3
"""Portable runtime regressions using an actual 7-Zip shared library.

usage: test_regressions.py <archiver-cli> <engine> [7z-command]
All archives and links are generated inside a temporary test directory.
"""

import io
import os
from pathlib import Path
import secrets
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

from test_extract import write_encrypted_zip

CLI = Path(sys.argv[1]).resolve()
ENGINE = Path(sys.argv[2]).resolve()
SEVEN = Path(sys.argv[3]).resolve() if len(sys.argv) > 3 else None
del sys.argv[1:]


class ExtractionRegressions(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="regression-", dir=CLI.parent)
        self.base = Path(self.temp.name).resolve()
        assert self.base.is_relative_to(CLI.parent)
        self.addCleanup(self.temp.cleanup)

    def run_cli(self, command, archive, dest=None, password=None):
        args = [str(CLI), command, str(archive)]
        if dest is not None:
            args.append(str(dest))
        result = subprocess.run(
            [*args, "--engine", str(ENGINE)],
            input=b"" if password is None else (password + "\n").encode("utf-8"),
            capture_output=True, timeout=30,
        )
        if password:
            self.assertNotIn(password.encode("utf-8"), result.stdout + result.stderr)
        return result

    def expect_code(self, result, code):
        self.assertEqual(result.returncode, code, (result.stdout, result.stderr))

    def zip(self, name="plain.zip", members=None):
        path = self.base / name
        with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as archive:
            for member, data in (members or {"sub/hello.txt": b"hello\n"}).items():
                archive.writestr(member, data)
        return path

    def test_zip_list_and_extract(self):
        data = secrets.token_bytes(1024 * 1024)
        archive = self.zip(members={"sub/hello.txt": data, "empty.txt": b""})
        for _ in range(3):
            result = self.run_cli("list", archive)
            self.expect_code(result, 0)
            self.assertIn(b"sub/hello.txt", result.stdout)
        dest = self.base / "out"
        self.expect_code(self.run_cli("extract", archive, dest), 0)
        self.assertEqual((dest / "sub/hello.txt").read_bytes(), data)
        self.assertEqual((dest / "empty.txt").read_bytes(), b"")

    def test_tar_and_compressed_tar(self):
        for mode, suffix in (("w", ".tar"), ("w:gz", ".tar.gz"),
                             ("w:bz2", ".tar.bz2"), ("w:xz", ".tar.xz")):
            with self.subTest(suffix=suffix):
                archive = self.base / ("archive" + suffix)
                with tarfile.open(archive, mode) as tf:
                    item = tarfile.TarInfo("sub/hello.txt")
                    item.size = 6
                    tf.addfile(item, io.BytesIO(b"hello\n"))
                dest = self.base / ("out" + suffix.replace(".", "-"))
                self.expect_code(self.run_cli("extract", archive, dest), 0)
                self.assertEqual((dest / "sub/hello.txt").read_bytes(), b"hello\n")

    def test_existing_file_preserved(self):
        archive = self.zip()
        dest = self.base / "out"
        (dest / "sub").mkdir(parents=True)
        existing = dest / "sub/hello.txt"
        existing.write_bytes(b"original user data")
        self.expect_code(self.run_cli("extract", archive, dest), 2)
        self.assertEqual(existing.read_bytes(), b"original user data")

    def test_existing_hardlink_preserved(self):
        outside = self.base / "outside.txt"
        outside.write_bytes(b"original user data")
        dest = self.base / "out"
        (dest / "sub").mkdir(parents=True)
        os.link(outside, dest / "sub/hello.txt")
        self.expect_code(self.run_cli("extract", self.zip(), dest), 2)
        self.assertEqual(outside.read_bytes(), b"original user data")

    def test_wrong_password_cleanup_and_retry(self):
        archive = self.base / "encrypted.zip"
        password = secrets.token_hex(16)
        write_encrypted_zip(archive, "secret.txt", b"secret contents", password.encode())
        dest = self.base / "out"
        self.expect_code(self.run_cli("extract", archive, dest), 5)
        self.expect_code(self.run_cli("extract", archive, dest, "wrong"), 6)
        self.assertFalse((dest / "secret.txt").exists())
        self.expect_code(self.run_cli("extract", archive, dest, password), 0)
        self.assertEqual((dest / "secret.txt").read_bytes(), b"secret contents")

    def test_crc_failure_removes_incomplete_file(self):
        archive = self.base / "corrupt.zip"
        with zipfile.ZipFile(archive, "w", compression=zipfile.ZIP_STORED) as zf:
            zf.writestr("hello.txt", b"original contents")
        raw = bytearray(archive.read_bytes())
        name_len, extra_len = struct.unpack_from("<HH", raw, 26)
        raw[30 + name_len + extra_len] ^= 1
        archive.write_bytes(raw)
        dest = self.base / "out"
        self.expect_code(self.run_cli("extract", archive, dest), 3)
        self.assertFalse((dest / "hello.txt").exists())

    def test_zip_slip_rejected_before_writes(self):
        archive = self.zip(members={"keep.txt": b"ok", "../outside.txt": b"escape"})
        dest = self.base / "out"
        self.expect_code(self.run_cli("extract", archive, dest), 7)
        self.assertFalse((self.base / "outside.txt").exists())
        self.assertFalse((dest / "keep.txt").exists())

    def test_renamed_files_detected_and_missing_volumes_reported(self):
        for name in ("notes.dat", "opened-archive"):
            with self.subTest(name=name):
                self.expect_code(self.run_cli("list", self.zip(name)), 0)
        for name in ("part.7z.001", "x.part01.rar"):
            with self.subTest(name=name):
                self.expect_code(self.run_cli("list", self.zip(name)), 0)

    def test_unicode_archive_and_destination(self):
        archive = self.zip("压缩包😀.zip", {"目录😀/文件🦊.txt": b"unicode"})
        dest = self.base / "输出😀"
        self.expect_code(self.run_cli("extract", archive, dest), 0)
        self.assertEqual((dest / "目录😀/文件🦊.txt").read_bytes(), b"unicode")

    @unittest.skipUnless(os.name == "nt", "Windows junction regression")
    def test_windows_junction_refused(self):
        import _winapi
        dest = self.base / "out"
        outside = self.base / "outside"
        dest.mkdir()
        outside.mkdir()
        link = dest / "sub"
        _winapi.CreateJunction(str(outside), str(link))
        try:
            self.expect_code(self.run_cli("extract", self.zip(), dest), 7)
            self.expect_code(self.run_cli("extract", self.zip(), link), 7)
            self.assertEqual(list(outside.iterdir()), [])
        finally:
            link.rmdir()

    @unittest.skipUnless(os.name == "nt", "Windows path aliases")
    def test_windows_aliases_rejected(self):
        for name in ("sub/.. /outside.txt", "NUL.txt", "dir/file.", "COM1"):
            with self.subTest(name=name):
                archive = self.zip(members={name: b"unsafe"})
                self.expect_code(self.run_cli("extract", archive, self.base / "out"), 7)

    @unittest.skipUnless(SEVEN, "pass a 7z command to test 7z encryption")
    def test_7z_and_header_encrypted_password(self):
        source = self.base / "hello.txt"
        source.write_bytes(b"seven zip")
        for encrypted in (False, True):
            archive = self.base / ("encrypted.7z" if encrypted else "plain.7z")
            password = secrets.token_hex(16) if encrypted else None
            args = [str(SEVEN), "a", "-t7z", str(archive), str(source)]
            if encrypted:
                args += ["-p" + password, "-mhe=on"]
            subprocess.run(args, capture_output=True, check=True, timeout=30)
            if encrypted:
                self.expect_code(self.run_cli("list", archive), 5)
                self.expect_code(self.run_cli("list", archive, password="wrong"), 6)
            dest = self.base / ("encrypted-out" if encrypted else "plain-out")
            self.expect_code(self.run_cli("extract", archive, dest, password), 0)
            self.assertEqual((dest / "hello.txt").read_bytes(), source.read_bytes())

    def test_tar_zstd(self):
        tar = self.base / "inner.tar"
        with tarfile.open(tar, "w") as tf:
            item = tarfile.TarInfo("sub/hello.txt")
            item.size = 5
            tf.addfile(item, io.BytesIO(b"hello"))
        archive = self.base / "archive.tar.zst"
        # A small RFC 8878 frame with a single raw block needs no external
        # zstd encoder (7-Zip supports decoding, but not creating this format).
        data = tar.read_bytes()
        assert len(data) < 128 * 1024
        frame = b"\x28\xb5\x2f\xfd\xa0" + struct.pack("<I", len(data))
        frame += ((len(data) << 3) | 1).to_bytes(3, "little") + data
        archive.write_bytes(frame)
        dest = self.base / "out"
        self.expect_code(self.run_cli("extract", archive, dest), 0)
        self.assertEqual((dest / "sub/hello.txt").read_bytes(), b"hello")


if __name__ == "__main__":
    unittest.main(verbosity=2)
