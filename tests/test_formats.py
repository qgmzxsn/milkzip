"""Content detection, real LZ4 fixtures, recursion and independent failures."""
import bz2
import ctypes as C
import gzip
import io
import lzma
import os
from pathlib import Path
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zipfile

LIB, ENGINE = map(lambda p: str(Path(p).resolve()), sys.argv[1:3])
SEVEN = str(Path(sys.argv[3]).resolve()) if len(sys.argv) > 3 else None
del sys.argv[1:]
lib = C.CDLL(LIB)
CHOOSE = C.CFUNCTYPE(C.c_int, C.POINTER(C.c_char_p), C.c_uint32, C.c_void_p)
PASSWORD = C.CFUNCTYPE(C.c_int, C.c_char_p, C.c_int, C.c_void_p, C.c_uint32, C.c_void_p)
ACCEPTED = C.CFUNCTYPE(None, C.c_char_p, C.c_void_p)
PROGRESS = C.CFUNCTYPE(C.c_int, C.c_uint64, C.c_uint64, C.c_void_p)

class Options(C.Structure):
    _fields_ = [('depth', C.c_uint32), ('bytes', C.c_uint64), ('keep', C.c_int),
                ('all', C.c_int), ('choose', CHOOSE), ('password', PASSWORD),
                ('accepted', ACCEPTED), ('user', C.c_void_p)]

lib.archiver_extract_nested_ex.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p,
    C.POINTER(C.c_char_p), C.c_uint32, C.POINTER(Options), PROGRESS, C.c_void_p,
    C.c_char_p, C.c_uint32]
lib.archiver_open.argtypes = [C.c_char_p, C.c_char_p, C.c_char_p, C.POINTER(C.c_void_p)]
lib.archiver_test.argtypes = [C.c_void_p, C.c_char_p]
lib.archiver_close.argtypes = [C.c_void_p]
FIXTURES = Path(__file__).parent / 'fixtures'
PAYLOAD = b'MilkZip format probe\n'
LZ4_PAYLOAD = b'MilkZip LZ4 linked-block fixture\n' * 8192

def zipped(members):
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        for name, data in members.items(): z.writestr(name, data)
    return out.getvalue()

def tarred(members):
    out = io.BytesIO()
    with tarfile.open(fileobj=out, mode='w') as t:
        for name, data in members.items():
            entry = tarfile.TarInfo(name); entry.size = len(data)
            t.addfile(entry, io.BytesIO(data))
    return out.getvalue()

def lz4_raw(data):
    # Standard independent-block header. The block is explicitly uncompressed.
    assert len(data) <= 64 << 10
    return bytes.fromhex('04224d18604082') + struct.pack('<I', len(data) | 0x80000000) + data + b'\0' * 4

class FormatTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.index = 0

    def extract(self, name, data, limit=64 << 30, progress=None):
        self.index += 1
        case = self.root / str(self.index); case.mkdir()
        archive = case / name; archive.write_bytes(data)
        dest = case / 'out'; dest.mkdir()
        options = Options(32, limit, 0, 1, CHOOSE(), PASSWORD(), ACCEPTED(), None)
        message = C.create_string_buffer(32768)
        callback = PROGRESS(progress) if progress else PROGRESS()
        code = lib.archiver_extract_nested_ex(os.fsencode(archive), os.fsencode(ENGINE), os.fsencode(dest),
            None, 0, C.byref(options), callback, None, message, len(message))
        return code, message.value.decode('utf-8'), dest

    def assert_payload(self, dest, payload=PAYLOAD):
        self.assertTrue(any(p.is_file() and p.read_bytes() == payload for p in dest.rglob('*')),
                        [str(p.relative_to(dest)) for p in dest.rglob('*')])

    def test_nsis_executable_and_nested_renamed_container(self):
        data = (FIXTURES / 'nsis.exe').read_bytes()
        for name, payload in [('setup.exe', data), ('renamed.txt', data),
                              ('outer.zip', zipped({'setup.exe': data}))]:
            code, message, out = self.extract(name, payload)
            self.assertEqual(code, 0, message)
            self.assert_payload(out, b'MilkZip NSIS fixture')

    def test_zip_unknown_and_incorrect_suffixes(self):
        for name in ['archive.txt', 'archive.bin', 'archive', 'archive.7z', 'archive.lz4']:
            with self.subTest(name=name):
                code, message, out = self.extract(name, zipped({'hello.txt': PAYLOAD}))
                self.assertEqual(code, 0, message); self.assert_payload(out)

    def test_streams_and_tar_by_content(self):
        frame = bytes.fromhex('28b52ffd0000') + ((len(PAYLOAD) << 3) | 1).to_bytes(3, 'little') + PAYLOAD
        cases = [gzip.compress(PAYLOAD), bz2.compress(PAYLOAD), lzma.compress(PAYLOAD),
                 lzma.compress(PAYLOAD, format=lzma.FORMAT_ALONE), frame,
                 tarred({'hello.txt': PAYLOAD})]
        for i, data in enumerate(cases):
            for name in ['unknown.txt', 'unknown', 'wrong.zip']:
                with self.subTest(format=i, name=name):
                    code, message, out = self.extract(name, data)
                    self.assertEqual(code, 0, message); self.assert_payload(out)

    def test_nested_renamed_archives(self):
        for name, data in [('inner.txt', zipped({'hello.txt': PAYLOAD})),
                           ('inner.bin', gzip.compress(PAYLOAD)),
                           ('inner', tarred({'hello.txt': PAYLOAD}))]:
            with self.subTest(name=name):
                code, message, out = self.extract('outer.zip', zipped({name: data}))
                self.assertEqual(code, 0, message); self.assert_payload(out)
                self.assertFalse((out / name).exists())

    def test_incorrect_compound_suffix_and_prefixed_payload(self):
        for name, data in [('wrong.tar.gz', gzip.compress(PAYLOAD)),
                           ('wrong.tar.bz2', bz2.compress(PAYLOAD)),
                           ('wrong.tar.xz', lzma.compress(PAYLOAD))]:
            code, message, out = self.extract(name, data)
            self.assertEqual(code, 0, message); self.assert_payload(out)
        payload = b'MZ' + b'\0' * 128 + zipped({'hello.txt': PAYLOAD})
        for name in ['wrong.7z', 'wrong.gz', 'wrong.lz4']:
            code, message, out = self.extract(name, payload)
            self.assertEqual(code, 0, message); self.assert_payload(out)
        code, message, out = self.extract('empty.txt', zipped({}))
        self.assertEqual(code, 0, message); self.assertEqual(list(out.iterdir()), [])

    def test_lz4_linked_checksums_and_legacy(self):
        for filename in ['linked.lz4', 'legacy.lz4']:
            for name in ['data.lz4', 'data.txt', 'data', '中文😀.lz4']:
                with self.subTest(fixture=filename, name=name):
                    code, message, out = self.extract(name, (FIXTURES / filename).read_bytes())
                    self.assertEqual(code, 0, message); self.assert_payload(out, LZ4_PAYLOAD)

    def test_lz4_concat_skippable_and_nested_tar(self):
        skip = struct.pack('<II', 0x184D2A53, 5) + b'notes'
        data = skip + lz4_raw(b'A') + skip + lz4_raw(b'B')
        code, message, out = self.extract('data.lz4', data)
        self.assertEqual(code, 0, message); self.assert_payload(out, b'AB')
        for name in ['bundle.tar.lz4', 'bundle.txt']:
            code, message, out = self.extract(name, lz4_raw(tarred({'hello.txt': PAYLOAD})))
            self.assertEqual(code, 0, message); self.assert_payload(out)
        data = (FIXTURES / 'legacy.lz4').read_bytes() + lz4_raw(PAYLOAD)
        code, message, out = self.extract('concat.lz4', data)
        self.assertEqual(code, 0, message); self.assert_payload(out, LZ4_PAYLOAD + PAYLOAD)
        code, message, out = self.extract('linux.lz4', (FIXTURES / 'legacy.lz4').read_bytes() + b'\0' * 4)
        self.assertEqual(code, 0, message); self.assert_payload(out, LZ4_PAYLOAD)

    def test_lz4_corruption_test_and_cleanup(self):
        original = (FIXTURES / 'linked.lz4').read_bytes()
        for data in [original[:-1], original[:20], original[:-1] + bytes([original[-1] ^ 1]),
                     original + b'garbage', bytes.fromhex('02214c18ffffffff')]:
            with self.subTest(length=len(data)):
                code, message, out = self.extract('data.lz4', data)
                self.assertEqual(code, 3, message)
                self.assertEqual(list(out.iterdir()), [])
        path = self.root / 'test.lz4'; path.write_bytes(original)
        handle = C.c_void_p()
        self.assertEqual(lib.archiver_open(os.fsencode(path), os.fsencode(ENGINE), None, C.byref(handle)), 0)
        try: self.assertEqual(lib.archiver_test(handle, None), 0)
        finally: lib.archiver_close(handle)

    def test_lz4_runtime_limit_and_cancellation(self):
        data = (FIXTURES / 'linked.lz4').read_bytes()
        code, message, out = self.extract('data.lz4', data, limit=1024)
        self.assertEqual(code, 12, message); self.assertEqual(list(out.iterdir()), [])
        code, message, out = self.extract('data.lz4', data, progress=lambda done, total, user: done > 0)
        self.assertEqual(code, 9, message); self.assertEqual(list(out.iterdir()), [])

    def test_document_containers_are_preserved_but_can_be_opened_explicitly(self):
        doc = zipped({'word/document.xml': PAYLOAD, 'embedded.zip': zipped({'secret.txt': PAYLOAD})})
        for name in ['document.docx', 'document.bin']:
            code, message, out = self.extract('outer.zip', zipped({name: doc}))
            self.assertEqual(code, 0, message); self.assertEqual((out / name).read_bytes(), doc)
            self.assertFalse((out / 'word').exists()); self.assertIn('kept container', message)
        code, message, out = self.extract('document.docx', doc)
        self.assertEqual(code, 0, message); self.assertEqual((out / 'word/document.xml').read_bytes(), PAYLOAD)

    def test_failed_branch_does_not_block_good_branch(self):
        for names in [('a-broken.zip', 'z-good.zip'), ('z-broken.zip', 'a-good.zip')]:
            bad, good = names
            code, message, out = self.extract('outer.zip', zipped({bad: b'not zip', good: zipped({'hello.txt': PAYLOAD})}))
            self.assertEqual(code, 13, message)
            self.assert_payload(out); self.assertTrue((out / bad).exists())
            self.assertIn('failed [format]', message); self.assertIn('extracted:', message)
        code, message, out = self.extract('outer.zip', zipped({
            'part.7z.002': b'no first volume', 'good.zip': zipped({'hello.txt': PAYLOAD})}))
        self.assertEqual(code, 13, message); self.assert_payload(out)
        self.assertIn('missing-volume', message)

    def test_ordinary_text_remains_a_leaf(self):
        code, message, out = self.extract('note.txt', b'plain text')
        self.assertEqual(code, 8, message); self.assertEqual(list(out.iterdir()), [])
        code, message, out = self.extract('outer.zip', zipped({'note.txt': b'plain text'}))
        self.assertEqual(code, 0, message); self.assertEqual((out / 'note.txt').read_bytes(), b'plain text')

    def test_failures_remain_visible_after_a_long_success_report(self):
        members = {f'{i:03d}-' + 'x' * 100 + '.zip': zipped({'hello.txt': PAYLOAD}) for i in range(140)}
        members['z-failure.zip'] = b'not zip'
        code, message, out = self.extract('outer.zip', zipped(members))
        self.assertEqual(code, 13, message)
        self.assertIn('z-failure.zip', message[:1024])
        self.assertTrue((out / 'z-failure.zip').exists())

    @unittest.skipUnless(SEVEN, '7-Zip fixture encoder unavailable')
    def test_large_appended_archive_matches_top_level_detection(self):
        source = self.root / 'large.bin'; source.write_bytes(b'x' * (6 << 20))
        archive = self.root / 'large.7z'
        subprocess.run([SEVEN, 'a', '-t7z', '-mx=0', str(archive), str(source)], check=True, capture_output=True)
        media = b'\x00\x00\x00\x18ftypisom' + b'\0' * 500 + archive.read_bytes()
        for name, data in [('video.mp4', media), ('video.txt', media),
                           ('outer.zip', zipped({'inner.mp4': media}))]:
            code, message, out = self.extract(name, data)
            self.assertEqual(code, 0, message); self.assert_payload(out, source.read_bytes())

if __name__ == '__main__': unittest.main()
