#!/usr/bin/env python3
"""MP4/image disguise: renamed archives and archives glued after media. No rar writer is committed."""

import io
import os
import struct
import subprocess
import sys
import tempfile
import zipfile

RESULT_UNSUPPORTED = 8


def run(cli, engine, args):
    proc = subprocess.run(
        [cli, *args, "--engine", engine],
        text=True,
        encoding="utf-8",
        capture_output=True,
        check=False,
    )
    return proc.returncode, proc.stdout, proc.stderr


def fail(message):
    print("FAIL:", message, file=sys.stderr)
    return 1


def mp4_bytes(padding):
    brand = b"isom" + b"\x00\x00\x00\x00" + b"isom"
    ftyp = struct.pack(">I", 8 + len(brand)) + b"ftyp" + brand
    mdat = struct.pack(">I", 8 + len(padding)) + b"mdat" + padding
    return ftyp + mdat


def make_zip(path, name, payload):
    with zipfile.ZipFile(path, "w") as zf:
        zf.writestr(name, payload)


def main():
    if len(sys.argv) != 3:
        print("usage: test_disguise.py <archiver-cli> <7z.so>", file=sys.stderr)
        return 2
    cli, engine = sys.argv[1:]
    errors = 0
    payload = b"phase-1\n"
    with tempfile.TemporaryDirectory() as tmp:
        zip_path = os.path.join(tmp, "real.zip")
        make_zip(zip_path, "dir/hello.txt", payload)
        zip_bytes = open(zip_path, "rb").read()

        renamed = os.path.join(tmp, "renamed.mp4")
        open(renamed, "wb").write(zip_bytes)
        code, out, err = run(cli, engine, ["list", renamed])
        if code != 0 or "dir/hello.txt" not in out:
            print(code, out, err, file=sys.stderr)
            errors += fail("zip renamed to mp4")
        else:
            print("ok zip renamed to mp4")
        dest = os.path.join(tmp, "out-renamed")
        os.makedirs(dest)
        code, out, err = run(cli, engine, ["extract", renamed, dest])
        got = os.path.join(dest, "dir", "hello.txt")
        if code != 0 or not os.path.isfile(got) or open(got, "rb").read() != payload:
            print(code, out, err, file=sys.stderr)
            errors += fail("extract zip renamed to mp4")
        else:
            print("ok extract zip renamed to mp4")

        # Larger than 7-Zip's header search window, with a fake local-header
        # signature in the video bytes and a real zip only at the end.
        padding = b"PK\x03\x04" + b"\x00" * (5 * 1024 * 1024)
        appended = os.path.join(tmp, "video.mp4")
        open(appended, "wb").write(mp4_bytes(padding) + zip_bytes)
        code, out, err = run(cli, engine, ["list", appended])
        if code != 0 or "dir/hello.txt" not in out:
            print(code, out, err, file=sys.stderr)
            errors += fail("zip appended after large mp4")
        else:
            print("ok zip appended after large mp4")
        dest = os.path.join(tmp, "out-appended")
        os.makedirs(dest)
        code, out, err = run(cli, engine, ["extract", appended, dest])
        got = os.path.join(dest, "dir", "hello.txt")
        if code != 0 or not os.path.isfile(got) or open(got, "rb").read() != payload:
            print(code, out, err, file=sys.stderr)
            errors += fail("extract zip appended after large mp4")
        else:
            print("ok extract zip appended after large mp4")

        plain = os.path.join(tmp, "plain.mp4")
        open(plain, "wb").write(mp4_bytes(b"\x37\x7A\xBC\xAF\x27\x1C" + b"\x00" * 4096))
        code, out, err = run(cli, engine, ["list", plain])
        if code != RESULT_UNSUPPORTED or "dir/hello.txt" in out:
            print(code, out, err, file=sys.stderr)
            errors += fail("plain mp4 must not be an archive")
        else:
            print("ok plain mp4 rejected")

        # JPEG end marker, then a zip. The marker is not enough by itself.
        picture = os.path.join(tmp, "photo.jpg")
        open(picture, "wb").write(b"\xff\xd8" + b"\x00" * 64 + b"\xff\xd9" + zip_bytes)
        code, out, err = run(cli, engine, ["list", picture])
        if code != 0 or "dir/hello.txt" not in out:
            print(code, out, err, file=sys.stderr)
            errors += fail("zip after jpeg end")
        else:
            print("ok zip after jpeg end")

        # ZIP64 needs the ZIP64 end record to locate the start when the archive
        # is prefixed or followed by media data.
        def zip64_bytes(data):
            buffer = io.BytesIO()
            with zipfile.ZipFile(buffer, "w") as zf:
                zf.writestr("dir/hello.txt", data)
            raw = bytearray(buffer.getvalue())
            eocd = raw.rfind(b"PK\x05\x06")
            cd_size, cd_offset = struct.unpack_from("<II", raw, eocd + 12)
            zip64_end = struct.pack("<IQHHIIQQQQ", 0x06064B50, 44, 45, 45, 0, 0, 1, 1,
                                    cd_size, cd_offset)
            locator = struct.pack("<IIQI", 0x07064B50, 0, eocd, 1)
            classic = struct.pack("<IHHHHIIH", 0x06054B50, 0, 0, 0xFFFF, 0xFFFF,
                                  0xFFFFFFFF, 0xFFFFFFFF, 0)
            return bytes(raw[:eocd]) + zip64_end + locator + classic

        prefix = mp4_bytes(b"\x00" * (256 * 1024))
        suffix = b"MOOVTRAILER" * 1000
        # A real PE stub is not needed to exercise the reader; the executable
        # path is only scanned for an embedded archive and is never run.
        exe_stub = b"MZ" + b"\x00" * 4094
        containers = {
            "zip64.mp4": zip64_bytes(payload),
            "zip64-prefixed.mp4": prefix + zip64_bytes(payload),
            "zip64-trailing.mp4": prefix + zip64_bytes(payload) + suffix,
            "zip-prefixed.mp4": prefix + zip_bytes,
            "zip-trailing.mp4": prefix + zip_bytes + suffix,
            "sfx.exe": exe_stub + zip_bytes,
            "sfx-trailing.exe": exe_stub + zip_bytes + suffix,
            "custom.zipsc": zip_bytes,
        }
        for name, content in containers.items():
            path = os.path.join(tmp, name)
            open(path, "wb").write(content)
            code, out, err = run(cli, engine, ["list", path])
            if code != 0 or "dir/hello.txt" not in out:
                print(code, out, err, file=sys.stderr)
                errors += fail(name + " list")
                continue
            dest = os.path.join(tmp, "out-" + name)
            os.makedirs(dest)
            code, out, err = run(cli, engine, ["extract", path, dest])
            got = os.path.join(dest, "dir", "hello.txt")
            if code != 0 or not os.path.isfile(got) or open(got, "rb").read() != payload:
                print(code, out, err, file=sys.stderr)
                errors += fail(name + " extract")
            else:
                print("ok", name)

        seven = None
        try:
            import py7zr

            seven = os.path.join(tmp, "real.7z")
            note = os.path.join(tmp, "note.txt")
            open(note, "wb").write(payload)
            with py7zr.SevenZipFile(seven, "w") as zf:
                zf.write(note, "dir/hello.txt")
        except Exception as exc:
            print("skip 7z disguise:", exc)
        if seven and os.path.isfile(seven):
            seven_bytes = open(seven, "rb").read()
            renamed7 = os.path.join(tmp, "renamed7.mp4")
            open(renamed7, "wb").write(seven_bytes)
            code, out, err = run(cli, engine, ["list", renamed7])
            if code != 0 or "dir/hello.txt" not in out:
                print(code, out, err, file=sys.stderr)
                errors += fail("7z renamed to mp4")
            else:
                print("ok 7z renamed to mp4")
            glued = os.path.join(tmp, "video7.mp4")
            open(glued, "wb").write(mp4_bytes(b"\x00" * (5 * 1024 * 1024)) + seven_bytes)
            code, out, err = run(cli, engine, ["list", glued])
            if code != 0 or "dir/hello.txt" not in out:
                print(code, out, err, file=sys.stderr)
                errors += fail("7z appended after large mp4")
            else:
                print("ok 7z appended after large mp4")

        rar_bin = "/tmp/rar/rar"
        if not os.path.isfile(rar_bin):
            print("skip rar renamed to mp4: no rar tool")
        else:
            note = os.path.join(tmp, "note.txt")
            open(note, "wb").write(payload)
            rar_path = os.path.join(tmp, "real.rar")
            subprocess.check_call([rar_bin, "a", "-ma5", "-ep", rar_path, note], stdout=subprocess.DEVNULL)
            renamed_rar = os.path.join(tmp, "renamed.rar.mp4")
            open(renamed_rar, "wb").write(open(rar_path, "rb").read())
            code, out, err = run(cli, engine, ["list", renamed_rar])
            if code != 0 or "note.txt" not in out:
                print(code, out, err, file=sys.stderr)
                errors += fail("rar renamed to mp4")
            else:
                print("ok rar renamed to mp4")
            dest = os.path.join(tmp, "out-rar")
            os.makedirs(dest)
            code, out, err = run(cli, engine, ["extract", renamed_rar, dest])
            got = os.path.join(dest, "note.txt")
            if code != 0 or not os.path.isfile(got) or open(got, "rb").read() != payload:
                print(code, out, err, file=sys.stderr)
                errors += fail("extract rar renamed to mp4")
            else:
                print("ok extract rar renamed to mp4")

    if errors:
        print(errors, "failed")
        return 1
    print("disguise checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
