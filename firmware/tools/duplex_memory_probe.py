#!/usr/bin/env python3
"""Opt-in acoustic load probe. Obtain user consent before --allow-audio.

Generates quiet test tones in memory, checks then discards microphone PCM,
and persists bounded sanitized diagnostics only. Not an echo-quality or real PC test.
"""
import argparse
import base64
import json
import math
import os
import re
import select
from pathlib import Path
import struct
import subprocess
import sys
import threading
import time
import uuid

from preserving_ota import Link as OtaLink
from runtime_memory_probe import sanitize


FIELDS = ('active', 'conversation', 'audioAec', 'audioAecPreparing', 'playbackActive',
          'dspFrames', 'dspLastUs', 'dspMaxUs', 'dspP95Us', 'dspP99Us',
          'dspWindowSamples', 'dspOver20msFrames', 'dspAecUs', 'dspNsUs', 'dspVadUs',
          'internalFreeBytes', 'internalLargestBytes', 'captureTxLastUs',
          'captureTxMaxUs', 'captureStackMinBytes', 'streamUnderruns', 'streamBufferedMs')


def write_known_progress(raw, write, stats, clock=time.monotonic, sleep=time.sleep):
    """Bound a nonblocking write, resuming only its confirmed unwritten suffix.

    pyserial's POSIX timed write waits for *next* writability even after os.write
    accepted bytes. A timeout then loses that count. Do not replay that call.
    This probe uses the nonblocking fd directly, without changing the OTA tool.
    """
    start, offset = clock(), 0
    stats.update(frameBytes=len(raw), writtenBytes=0, blockedCalls=0)
    try:
        while offset < len(raw):
            if clock() - start >= 3:
                raise TimeoutError('Bounded probe write deadline')
            part = raw[offset:offset+64]
            try:
                count = write(part)
            except (BlockingIOError, InterruptedError):
                stats['blockedCalls'] += 1
                sleep(.001)
                continue
            if type(count) is not int or not 0 < count <= len(part):
                raise OSError('Invalid probe write progress')
            offset += count
            stats['writtenBytes'] = offset
            if offset < len(raw):
                sleep(.0005)
    finally:
        stats['elapsedMs'] = round((clock() - start) * 1000, 3)


class Link(OtaLink):
    """Probe-only macOS backend; no driver drains or uncertain write retries."""
    def __init__(self, port):
        super().__init__(port)
        self.write_stats = {}
        self.known_progress = sys.platform == 'darwin'
        if self.known_progress:
            import fcntl
            if not fcntl.fcntl(self.s.fileno(), fcntl.F_GETFL) & os.O_NONBLOCK:
                self.s.close()
                raise RuntimeError('Probe requires a nonblocking descriptor')

    def send(self, topic, payload):
        if not self.known_progress:
            return super().send(topic, payload)
        raw = (json.dumps(dict(topic=topic, payload=payload), separators=(',', ':')) + '\n').encode()
        write_known_progress(raw, lambda part: os.write(self.s.fileno(), part), self.write_stats)

    def resync_before_stop(self):
        # A failed frame may be partial. Terminate it before a fresh STOP;
        # never replay its audio bytes or purge incoming stop confirmation.
        if self.known_progress:
            write_known_progress(b'\n', lambda part: os.write(self.s.fileno(), part), {})
        else:
            self.s.write(b'\n')


class ProbeFault(RuntimeError):
    """Only caller-defined constant codes; never device text or PCM."""


class Evidence:
    def __init__(self, board, session, before, seconds):
        self.board, self.session, self.before = board, session, before
        self.sequence = None
        self.ready_seen = False
        self.origin = time.monotonic()
        self.lock = threading.RLock()
        self.report = dict(complete=False, durationSeconds=seconds, cloudUsed=False,
                           pcmSaved=False, frames=0, sequenceGaps=0, rawFrames=0,
                           integrityChecked=0, integrityFailures=0,
                           playChunks=0, audio=[], memory=[], stopped=False,
                           before=before, acousticQualityTested=False,
                           normalPcLoadTested=False,
                           gapDetails=[], receive=dict(bytes=0, lines=0, nonJsonLines=0,
                               malformedJsonLines=0, malformedAudioLines=0,
                               maxReadBytes=0, maxPartialBytes=0, malformedDetails=[]))

    def observe_read(self, count, partial):
        with self.lock:
            r = self.report['receive']
            r['bytes'] += count
            r['maxReadBytes'] = max(r['maxReadBytes'], count)
            r['maxPartialBytes'] = max(r['maxPartialBytes'], partial)

    def observe_line(self, line, error=None):
        # Only structural counts/offsets, never line bytes, strings or payloads.
        with self.lock:
            r = self.report['receive']
            r['lines'] += 1
            if error is None:
                return
            if b'{' not in line:
                r['nonJsonLines'] += 1
                return
            r['malformedJsonLines'] += 1
            audio = line.count(b'"topic":"audio/chunk"')
            r['malformedAudioLines'] += int(audio > 0)
            if len(r['malformedDetails']) < 32:
                r['malformedDetails'].append(dict(
                    elapsedMs=round((time.monotonic()-self.origin)*1000),
                    lineBytes=len(line), audioMarkers=audio,
                    topicMarkers=line.count(b'"topic":'),
                    controlBytes=sum(b < 32 and b not in (9, 13) for b in line),
                    errorOffset=getattr(error, 'pos', getattr(error, 'start', -1)),
                    structure=damaged_structure(line, error)))

    def consume(self, message):
        with self.lock:
            self._consume(message)

    def _current_aec_fault(self, payload):
        # Preparing a new DSP timeline can still report the retired timeline's
        # diagnostic until the capture worker finishes recreating its handles.
        return (payload.get('conversationSessionId') == self.session
                and payload.get('audioAecPreparing') is not True
                and payload.get('aecReason') not in (None, 'none'))

    def _consume(self, message):
        if not isinstance(message, dict) or not isinstance(message.get('payload'), dict):
            return
        p, topic = message['payload'], message.get('topic')
        if topic == 'audio/chunk' and p.get('conversationSessionId') == self.session:
            seq = p.get('seq')
            if type(seq) is not int or seq < 0:
                raise RuntimeError('Invalid microphone frame sequence')
            self.report['frames'] += 1
            self.report['integrityChecked'] += 1
            self.report['integrityFailures'] += int(not valid_pcm_checksum(p))
            self.report['rawFrames'] += int(p.get('aec') is not True)
            if self.sequence is not None and seq != self.sequence + 1:
                self.report['sequenceGaps'] += 1
                if len(self.report['gapDetails']) < 32:
                    self.report['gapDetails'].append(dict(previous=self.sequence, current=seq,
                        elapsedMs=round((time.monotonic()-self.origin)*1000),
                        malformedAudioLines=self.report['receive']['malformedAudioLines']))
            self.sequence = seq  # Neither the encoded nor decoded PCM is retained.
        elif topic == 'audio/status':
            if p.get('boardDeviceId') != self.board:
                raise RuntimeError('Unexpected audio device')
            # The board emits a small bounded set of status messages per query.
            if len(self.report['audio']) >= 4096:
                raise RuntimeError('Audio status budget exceeded')
            self.report['audio'].append({k: p[k] for k in FIELDS
                                         if type(p.get(k)) in (bool, int, float)})
            self.report['stopped'] = all(p.get(k) is False for k in
                                        ('active', 'conversation', 'playbackActive'))
            if self._current_aec_fault(p):
                raise RuntimeError('AEC fault reported')
            if p.get('active') is True:
                ready = p.get('audioAec') is True and p.get('audioAecPreparing') is False
                if self.ready_seen and not ready:
                    raise RuntimeError('AEC readiness lost after warmup')
                self.ready_seen = self.ready_seen or ready
        elif topic == 'diagnostics/status' and p.get('requestId', '').startswith(self.session + '-query-'):
            value = sanitize(p)
            if (p.get('boardDeviceId') != self.board
                    or any(value[k] != self.before[k] for k in ('bootCount', 'faultResetCount'))
                    or value['runtime'].get('buildId') != self.before['runtime'].get('buildId')):
                raise RuntimeError('Device identity or boot changed during test')
            if len(self.report['memory']) >= 1024:
                raise RuntimeError('Memory status budget exceeded')
            self.report['memory'].append(value)
        elif topic in ('audio/error', 'protocol/error'):
            raise ProbeFault('device_' + topic.replace('/', '_'))
        elif topic == 'protocol/ack' and p.get('ok') is False:
            raise ProbeFault('device_protocol_rejected')
        elif topic == 'audio/diagnostic' and p.get('conversationSessionId') == self.session:
            if self._current_aec_fault(p) or p.get('ok') is False:
                raise ProbeFault('device_audio_diagnostic_failed')

    def verify(self):
        with self.lock:
            self._verify()

    def _verify(self):
        r = self.report
        # A 20-ms capture stream should deliver 50 frames/second. Allow startup
        # time, not a partial run or an absent diagnostic field masquerading as 0.
        if (r['frames'] < (r['durationSeconds'] - 5) * 45
                or r['sequenceGaps'] or r['rawFrames']):
            raise RuntimeError('Capture coverage or continuity failed')
        if r['receive']['malformedAudioLines']:
            raise RuntimeError('Malformed audio JSON line received')
        if r['integrityChecked'] != r['frames'] or r['integrityFailures']:
            raise RuntimeError('Capture integrity incomplete or failed')
        ready_at = next((i for i, s in enumerate(r['audio'])
                         if s.get('active') is True and s.get('audioAec') is True
                         and s.get('audioAecPreparing') is False), None)
        # Preparation before the first ready status is expected, but a later
        # restart/preparation is not silently excluded from acceptance.
        active = [] if ready_at is None else [s for s in r['audio'][ready_at:] if s.get('active') is True]
        if (not active or not any(s.get('dspWindowSamples') == 128 for s in active)
                or any(s.get('audioAec') is not True or s.get('audioAecPreparing') is not False for s in active)):
            raise RuntimeError('AEC coverage incomplete')
        for key in ('streamUnderruns', 'dspOver20msFrames'):
            # stream_begin resets its counter; preparation can legitimately
            # expose the previous stopped stream's count. Never exclude any
            # active playback sample, nor DSP samples during preparation.
            samples = [s for s in active if s.get('playbackActive') is True] if key == 'streamUnderruns' else active
            if not samples:
                raise RuntimeError('Playback coverage incomplete')
            values = [s.get(key) for s in samples]
            if any(type(v) is not int or v != 0 for v in values):
                raise RuntimeError('Audio deadline failure or missing counter')
        if len(r['memory']) < r['durationSeconds'] // 10:
            raise RuntimeError('Memory coverage incomplete')
        for sample in r['memory']:
            for key in ('budgetRejected', 'allocationFailures', 'queueFull'):
                value = sample['protocolMemory'].get(key)
                baseline = self.before['protocolMemory'].get(key)
                if type(value) is not int or type(baseline) is not int or value != baseline:
                    raise RuntimeError('Protocol counter changed or missing')


def valid_pcm_checksum(payload):
    """Validate the existing wire checksum without saving or returning PCM."""
    encoded, checksum = payload.get('data'), payload.get('checksum')
    if (type(payload.get('bytes')) is not int or payload['bytes'] != 640
            or not isinstance(encoded, str) or len(encoded) != 856
            or not isinstance(checksum, str) or not re.fullmatch('[0-9a-f]{16}', checksum)):
        return False
    try:
        pcm = base64.b64decode(encoded, validate=True)
    except ValueError:
        return False
    if len(pcm) != 640:
        return False
    value = 0xcbf29ce484222325
    for byte in pcm:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    return f'{value:016x}' == checksum


def damaged_structure(line, error):
    """Bounded shape only: no input letters/digits/base64 or arbitrary errors."""
    offset = getattr(error, 'pos', getattr(error, 'start', 0))
    start = line.find(b'{')
    pos = max(0, start) + max(0, offset)
    # Each alphanumeric and base64 symbol becomes the SAME placeholder. This
    # cannot reconstruct PCM or identifiers even when damaged JSON is unparseable.
    punctuation = b'{}[]:,"\\ \r\t'
    shape = ''.join(chr(c) if c in punctuation else 'x'
                    for c in line[max(0, pos-24):pos+24])
    fields = {key: line.find(('"'+key+'":').encode()) for key in
              ('payload', 'sessionId', 'seq', 'bytes', 'checksum', 'data',
               'aec', 'conversationSessionId', 'vadSpeech')}
    return dict(errorShape=shape, fieldOffsets=fields,
                objectStarts=[m.start() for m in re.finditer(b'{', line)][:8],
                noiseSuppressorInitMarker=b'WebRtcNs_Create:' in line,
                logMarkers=len(re.findall(rb'[IWE] \([0-9]+\)', line)))


def tones():
    result = []
    for chunk in range(20):
        data = bytearray()
        for i in range(1600):
            t = chunk * .1 + i / 16000
            envelope = min(1, (t % 1) / .03, (1 - t % 1) / .03)
            frequency = 440 if t < 1 else 660
            data.extend(struct.pack('<h', int(900 * envelope * math.sin(2 * math.pi * frequency * t))))
        result.append(base64.b64encode(data).decode())
    return result


def read_ready_block(fd, wait=select.select, read=os.read):
    """Read currently available bytes, not a one-byte/FIONREAD polling cycle.

    Readiness wait sleeps only while idle. The nonblocking read returns short
    immediately; it does not wait to fill 8192 bytes or drain a driver queue.
    Same block capacity as the client's default Rust BufReader. No retries of
    received data, filtering, or reconstruction of malformed audio frames.
    """
    try:
        ready, _, _ = wait([fd], [], [], .01)
        if not ready:
            return b''
        data = read(fd, 8192)
        if not data:
            raise OSError('Serial read reached EOF')
        return data
    except (BlockingIOError, InterruptedError):
        return b''


class DuplexReceiver:
    """Drain input independently of blocking serial writes; retain no PCM queue.

    Keep draining after a validation fault so stop replies can still arrive.
    Only one sanitized failure code is retained; exceptions/tracebacks could
    otherwise keep raw microphone messages alive through their stack frames.
    """
    def __init__(self, serial_port, evidence, backend='available'):
        self.serial_port, self.evidence = serial_port, evidence
        self.stop_event = threading.Event()
        self.failure = None
        self.fd = None
        if backend not in ('available', 'block'):
            raise ValueError('Unknown receiver backend')
        if backend == 'block':
            if sys.platform != 'darwin' or not hasattr(serial_port, 'fileno'):
                raise RuntimeError('Block comparison requires macOS serial fd')
            import fcntl
            fd = serial_port.fileno()
            if not fcntl.fcntl(fd, fcntl.F_GETFL) & os.O_NONBLOCK:
                raise RuntimeError('Receiver requires nonblocking descriptor')
            self.fd = fd
        evidence.report['readBackend'] = 'macos-ready-block-read' if self.fd is not None else 'pyserial-available-read'
        self.thread = threading.Thread(target=self._run, name='duplex-diagnostics-rx', daemon=True)

    def start(self):
        self.thread.start()

    def check(self):
        if self.failure:
            raise RuntimeError('Receiver failed: ' + self.failure)

    def close(self):
        self.stop_event.set()
        self.thread.join(timeout=2)
        if self.thread.is_alive():
            raise RuntimeError('Receiver did not stop')

    def _run(self):
        buf = b''
        while not self.stop_event.is_set():
            try:
                chunk = (read_ready_block(self.fd) if self.fd is not None else
                         self.serial_port.read(max(1, min(self.serial_port.in_waiting, 65536))))
                buf += chunk
                self.evidence.observe_read(len(chunk), len(buf))
                chunk = b''
                if len(buf) > 131072:
                    buf = b''
                    raise RuntimeError('Receive buffer budget exceeded')
                while b'\n' in buf:
                    line, buf = buf.split(b'\n', 1)
                    try:
                        message = json.loads(line[line.index(b'{'):])
                    except ValueError as exc:
                        self.evidence.observe_line(line, exc)
                        line = b''
                        continue
                    self.evidence.observe_line(line)
                    try:
                        self.evidence.consume(message)
                    except Exception as exc:
                        if self.failure is None:
                            self.failure = str(exc) if isinstance(exc, ProbeFault) else type(exc).__name__
                    finally:
                        # Deliberately do not retain the last decoded mic frame.
                        message = None
                        line = b''
            except Exception as exc:
                if self.failure is None:
                    self.failure = type(exc).__name__
                self.stop_event.wait(.01)


def run(opts):
    report = dict(complete=False, cloudUsed=False, pcmSaved=False, stopped=False)
    link, started, evidence, receiver, origin = None, False, None, None, None
    session = 'duplex-probe-' + uuid.uuid4().hex
    play_session = session + '-play'

    try:
        if getattr(opts, 'rust_transport_library', None):
            from rust_serial_probe import RustLink
            link = RustLink(opts.port, opts.rust_transport_library)
        else:
            link = Link(opts.port)
        before = link.request('diagnostics/query', {}, 'diagnostics/status')
        if (before.get('boardDeviceId') != opts.board
                or before.get('runtime', {}).get('firmware') != opts.version
                or before['runtime'].get('audioPlaybackActive') is not False
                or before['runtime'].get('screenPage') != 'main'):
            raise RuntimeError('Unexpected device/version or device not idle')
        evidence = Evidence(opts.board, session, sanitize(before), opts.seconds)
        report = evidence.report
        report['writeBackend'] = getattr(link, 'write_backend',
            'macos-nonblocking-known-progress' if getattr(link, 'known_progress', False) else 'pyserial-timed')
        link.s.timeout = .01
        receiver = DuplexReceiver(link.s, evidence, getattr(opts, 'reader_backend', 'available'))
        if getattr(opts, 'rust_transport_library', None):
            report['readBackend'] = 'rust-production-port-python-framing'
        receiver.start()
        link.send('audio/query', {})
        deadline = time.monotonic() + 4
        while not report['audio'] and time.monotonic() < deadline:
            receiver.check()
            time.sleep(.005)
        if not report['stopped']:
            raise RuntimeError('Existing capture/playback or unknown state; not starting')
        pattern = tones()
        started = True  # Always stop even if the start reply is lost.
        report['stopped'] = False
        link.send('audio/conversation', dict(enabled=True, halfDuplex=False, sessionId=session))
        origin = time.monotonic()
        next_status = next_diag = next_progress = origin
        next_play = None
        while time.monotonic() - origin < opts.seconds:
            receiver.check()
            now = time.monotonic()
            if now >= next_status:
                link.send('audio/query', {})
                next_status = now + 2
            if now >= next_diag:
                link.send('diagnostics/query', dict(requestId=session + '-query-' + uuid.uuid4().hex))
                next_diag = now + 5
            if next_play is None and now - origin >= 5:
                if not any(s.get('active') and s.get('audioAec')
                           and not s.get('audioAecPreparing') for s in report['audio']):
                    raise RuntimeError('AEC capture did not become ready')
                link.send('audio/play_begin', dict(sessionId=play_session))
                next_play = now - .2
            if next_play is not None and now >= next_play:
                seq = report['playChunks']
                link.send('audio/play_chunk', dict(sessionId=play_session, seq=seq,
                          bytes=3200, data=pattern[seq % len(pattern)]))
                report['playChunks'] += 1
                next_play += .1
            receiver.check()
            time.sleep(.001)
            if now >= next_progress:
                print(json.dumps(dict(elapsedSeconds=round(now-origin),
                      frames=report['frames'], sequenceGaps=report['sequenceGaps'],
                      rawFrames=report['rawFrames'], playChunks=report['playChunks'])), flush=True)
                next_progress = now + 30
        report['elapsedSeconds'] = time.monotonic() - origin
        receiver.check()
        evidence.verify()
        report['complete'] = True
    except BaseException as exc:
        report['failureType'] = type(exc).__name__
        if origin is not None:
            report['elapsedSeconds'] = time.monotonic() - origin
        if link and hasattr(link, 'write_stats'):
            report['lastWrite'] = dict(link.write_stats)
        raise
    finally:
        try:
            if link and started:
                report['stopped'] = False
                if report.get('failureType') and hasattr(link, 'resync_before_stop'):
                    try:
                        link.resync_before_stop()
                    except Exception:
                        report['stopResyncFailure'] = True
                # Independently attempt both stop commands if one write fails.
                for topic, payload in (
                    ('audio/play_flush', dict(sessionId=play_session)),
                    ('audio/conversation', dict(enabled=False, sessionId=session)),
                    ('audio/query', {})):
                    try:
                        link.send(topic, payload)
                    except Exception:
                        report['stopWriteFailure'] = True
                deadline = time.monotonic() + 5
                while not report['stopped'] and time.monotonic() < deadline:
                    time.sleep(.01)  # Reader keeps draining even after faults.
        finally:
            if receiver:
                try:
                    receiver.close()
                except Exception:
                    report['receiverStopFailure'] = True
                    report['complete'] = False
                if receiver.failure:
                    report['receiverFailure'] = receiver.failure
                    report['complete'] = False
            if link:
                if hasattr(link, 'write_totals'):
                    report['hostWrite'] = dict(link.write_totals)
            if started and not report['stopped']:
                report['complete'] = False
            # Preserve sanitized evidence before a potentially blocked driver
            # close. The main report is only written after release succeeds.
            checkpoint = opts.output.with_suffix('.preclose.json')
            with checkpoint.open('x') as file:
                json.dump(dict(report, portReleased=False), file, indent=2, allow_nan=False)
                file.write('\n')
            if link:
                link.s.close()
            report['portReleased'] = True
            with opts.output.open('x') as file:
                json.dump(report, file, indent=2, allow_nan=False)
                file.write('\n')
    if not report['complete']:
        raise RuntimeError('Stop not confirmed; inspect device before continuing')
    print('Duplex test complete; stop confirmed and port released', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--board', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--seconds', type=int, default=120)
    parser.add_argument('--reader-backend', choices=('available', 'block'), default='available',
                        help='Explicit comparison only; block reading has NOT fixed WCH frame damage')
    parser.add_argument('--allow-audio', action='store_true')
    parser.add_argument('--rust-transport-library', type=Path,
                        help='Opt-in in-tree serial-probe dylib; not the complete PC UI/service stack')
    opts = parser.parse_args()
    if opts.rust_transport_library and (sys.platform != 'darwin'
            or opts.reader_backend != 'available' or not opts.rust_transport_library.is_file()):
        parser.error('Rust comparison requires macOS, existing library and available reader')
    if not opts.allow_audio or not 30 <= opts.seconds <= 1800:
        parser.error('Explicit audio consent flag and 30..1800 seconds required')
    if opts.output.exists() or opts.output.with_suffix('.preclose.json').exists() or not opts.output.parent.is_dir():
        parser.error('Use a new report in an existing directory')
    busy = subprocess.run(['lsof', opts.port], capture_output=True, text=True)
    if busy.returncode != 1 or busy.stderr.strip():
        parser.error('Port occupied or check failed')
    run(opts)


if __name__ == '__main__':
    main()
