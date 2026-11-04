#!/usr/bin/env python3
"""Real-engine progress/cancellation tests through the public C API."""
import ctypes as c
from pathlib import Path
import sys
import tempfile
import unittest
import zipfile

LIBRARY = Path(sys.argv[1]).resolve()
ENGINE = str(Path(sys.argv[2]).resolve()).encode('utf-8')
del sys.argv[1:]
API = c.CDLL(str(LIBRARY))
CALLBACK = c.CFUNCTYPE(c.c_int, c.c_uint64, c.c_uint64, c.c_void_p)
API.archiver_open.argtypes = [c.c_char_p, c.c_char_p, c.c_char_p, c.POINTER(c.c_void_p)]
API.archiver_open.restype = c.c_int
API.archiver_extract_with_progress.argtypes = [c.c_void_p, c.c_char_p, c.c_char_p, CALLBACK, c.c_void_p]
API.archiver_extract_with_progress.restype = c.c_int
API.archiver_test_with_progress.argtypes = [c.c_void_p, c.c_char_p, CALLBACK, c.c_void_p]
API.archiver_test_with_progress.restype = c.c_int
API.archiver_close.argtypes = [c.c_void_p]


class ProgressTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='progress-', dir=LIBRARY.parent)
        self.base = Path(self.temp.name).resolve()
        assert self.base.is_relative_to(LIBRARY.parent)
        self.addCleanup(self.temp.cleanup)
        self.payload = b'progress contents\n' * 131072
        archive = self.base / 'plain.zip'
        with zipfile.ZipFile(archive, 'w') as zf:
            zf.writestr('hello.txt', self.payload)
        self.handle = c.c_void_p()
        self.assertEqual(API.archiver_open(str(archive).encode(), ENGINE, None, c.byref(self.handle)), 0)
        self.addCleanup(API.archiver_close, self.handle)

    def extract(self, name, cancel_at=None):
        events = []

        @CALLBACK
        def progress(done, total, _):
            events.append((done, total))
            return int(cancel_at is not None and done >= cancel_at)

        dest = self.base / name
        result = API.archiver_extract_with_progress(self.handle, str(dest).encode(), None, progress, None)
        return result, events, dest

    def test_integrity_progress_and_cancellation(self):
        events = []
        @CALLBACK
        def observe(done, total, _):
            events.append((done, total))
            return 0
        self.assertEqual(API.archiver_test_with_progress(self.handle, None, observe, None), 0)
        self.assertTrue(any(done > 0 for done, _ in events))
        @CALLBACK
        def cancel(done, total, _):
            return 1
        self.assertEqual(API.archiver_test_with_progress(self.handle, None, cancel, None), 9)
        self.assertEqual(API.archiver_test_with_progress(self.handle, None, observe, None), 0)
        self.assertFalse((self.base / 'hello.txt').exists())

    def test_progress_success(self):
        result, events, dest = self.extract('out')
        self.assertEqual(result, 0)
        self.assertEqual((dest / 'hello.txt').read_bytes(), self.payload)
        self.assertTrue(events)
        self.assertEqual(events[-1], (len(self.payload), len(self.payload)))
        self.assertEqual([x[0] for x in events], sorted(x[0] for x in events))

    def test_cancel_before_writes(self):
        result, events, dest = self.extract('cancelled', cancel_at=0)
        self.assertEqual(result, 9)
        self.assertFalse((dest / 'hello.txt').exists())
        self.assertEqual(events[0][0], 0)

    def test_cancel_mid_file_and_retry(self):
        result, events, dest = self.extract('out', cancel_at=65536)
        self.assertEqual(result, 9)
        self.assertTrue(any(x[0] > 0 for x in events))
        self.assertFalse((dest / 'hello.txt').exists())
        result, _, dest = self.extract('out')
        self.assertEqual(result, 0)
        self.assertEqual((dest / 'hello.txt').read_bytes(), self.payload)


if __name__ == '__main__':
    unittest.main(verbosity=2)
