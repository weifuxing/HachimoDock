"""Opt-in adapter for the in-tree production Rust serial transport test library.

No device actions on import. Raw bytes stay in memory and are never logged.
The caller owns identity checks, acoustic consent, stop confirmation and close.
"""
import ctypes
import json
import time
from preserving_ota import Link as OtaLink


class RustPort:
    def __init__(self, port, library):
        self.lib = ctypes.CDLL(str(library))
        self.lib.probe_open.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        self.lib.probe_open.restype = ctypes.c_int
        self.lib.probe_read.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        self.lib.probe_read.restype = ctypes.c_ssize_t
        self.lib.probe_send.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        self.lib.probe_send.restype = ctypes.c_int
        self.lib.probe_stats.argtypes = [ctypes.POINTER(ctypes.c_uint64), ctypes.c_size_t]
        self.lib.probe_stats.restype = ctypes.c_int
        self.lib.probe_close.argtypes = []
        self.lib.probe_close.restype = None
        encoded = port.encode()
        if self.lib.probe_open(encoded, len(encoded)) != 0:
            raise OSError('Production Rust serial open failed')
        self.closed = False
        self.pending = b''
        self.buffer = ctypes.create_string_buffer(8192)
        self.timeout = .15

    @property
    def in_waiting(self):
        # Capacity hint, not kernel availability. Production Read itself waits
        # at most its configured 100 ms and returns any short read immediately.
        return 8192

    def read(self, size):
        if self.closed:
            raise OSError('Rust serial closed')
        if self.pending:
            result, self.pending = self.pending[:size], self.pending[size:]
            return result
        n = self.lib.probe_read(self.buffer, min(size, 8192))
        if n < 0:
            raise OSError('Production Rust serial read failed')
        return self.buffer.raw[:n]

    def read_until(self, delimiter, maximum):
        data, self.pending = self.pending, b''
        end = time.monotonic() + self.timeout
        while delimiter not in data and len(data) < maximum and time.monotonic() < end:
            data += self.read(min(8192, maximum-len(data)))
        if delimiter in data:
            n = data.index(delimiter) + len(delimiter)
            data, self.pending = data[:n], data[n:]
        return data

    def send_frame(self, raw, stats):
        if self.closed:
            raise OSError('Rust serial closed')
        result = self.lib.probe_send(raw, len(raw))
        values = (ctypes.c_uint64 * 4)()
        if self.lib.probe_stats(values, 4) != 0:
            raise OSError('Production Rust serial stats unavailable')
        stats.update(frameBytes=len(raw), writtenBytes=values[0], retries=values[1],
                     elapsedMs=values[2]/1000, failed=bool(values[3]))
        if result != 0:
            raise OSError('Production Rust serial write failed')

    def close(self):
        if not self.closed:
            self.closed = True
            self.pending = b''
            self.buffer = None
            self.lib.probe_close()


class RustLink(OtaLink):
    def __init__(self, port, library):
        self.s = RustPort(port, library)
        self.write_stats = {}
        self.write_totals = dict(frames=0, bytes=0, retries=0, failures=0, maxElapsedMs=0)
        self.write_backend = 'rust-production-live-frame'
        self.known_progress = False
        try:
            self.resync_before_stop()
        except BaseException:
            self.s.close()
            raise

    def send(self, topic, payload):
        raw = (json.dumps(dict(topic=topic, payload=payload), separators=(',', ':'))+'\n').encode()
        try:
            self.s.send_frame(raw, self.write_stats)
        finally:
            t, s = self.write_totals, self.write_stats
            t['frames'] += 1
            t['bytes'] += s.get('writtenBytes', 0)
            t['retries'] += s.get('retries', 0)
            t['failures'] += int(s.get('failed', True))
            t['maxElapsedMs'] = max(t['maxElapsedMs'], s.get('elapsedMs', 0))

    def resync_before_stop(self):
        self.s.send_frame(b'\n', self.write_stats)
