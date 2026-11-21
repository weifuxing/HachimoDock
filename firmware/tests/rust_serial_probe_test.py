"""In-memory adapter checks. No serial open, microphone or library loading."""
import ctypes
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
from rust_serial_probe import RustPort


class FakeLibrary:
    def __init__(self, chunks):
        self.chunks = list(chunks)
        self.frames = []
        self.closed = 0
        self.result = 0

    def probe_read(self, buffer, cap):
        raw = self.chunks.pop(0) if self.chunks else b''
        assert len(raw) <= cap
        ctypes.memmove(buffer, raw, len(raw))
        return len(raw)

    def probe_send(self, raw, size):
        self.frames.append(raw[:size])
        return self.result

    def probe_stats(self, values, cap):
        values[:] = [len(self.frames[-1]), 0, 1200, int(self.result != 0)]
        return 0

    def probe_close(self):
        self.closed += 1


def port(chunks):
    p = RustPort.__new__(RustPort)
    p.lib = FakeLibrary(chunks)
    p.closed = False
    p.pending = b''
    p.buffer = ctypes.create_string_buffer(8192)
    p.timeout = .01
    return p


def test_coalesced_and_split_frames_are_not_lost():
    p = port([b'first', b'\nsecond\n'])
    assert p.read_until(b'\n', 100) == b'first\n'
    assert p.read(3) == b'sec'
    assert p.read(100) == b'ond\n'
    p.close()
    p.close()
    assert p.lib.closed == 1 and p.pending == b'' and p.buffer is None
    with pytest.raises(OSError):
        p.read(1)


def test_error_does_not_replay_frame_or_hide_progress():
    p = port([])
    p.lib.result = -2
    stats = {}
    with pytest.raises(OSError):
        p.send_frame(b'private-frame', stats)
    assert len(p.lib.frames) == 1
    assert stats == dict(frameBytes=13, writtenBytes=13, retries=0, elapsedMs=1.2, failed=True)
    assert 'private' not in str(stats)
    p.close()
