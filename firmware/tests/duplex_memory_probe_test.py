import importlib.util
import base64
import json
from pathlib import Path
from types import SimpleNamespace
import sys
import queue
import threading
import time

import pytest

TOOLS = Path(__file__).parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
spec = importlib.util.spec_from_file_location('duplex_probe', TOOLS / 'duplex_memory_probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def baseline():
    return dict(boardFingerprint='sanitized', bootCount=3, faultResetCount=0,
                runtime=dict(buildId='candidate'),
                protocolMemory=dict(budgetRejected=0, allocationFailures=0, queueFull=0))


def evidence():
    return probe.Evidence('test-board', 'test-session', baseline(), 30)


def message(topic, **payload):
    return dict(topic=topic, payload=payload)


def test_pcm_and_session_are_not_retained():
    e = evidence()
    for seq in range(3):
        e.consume(message('audio/chunk', conversationSessionId='test-session',
                          seq=seq, aec=True, data='PRIVATE-MICROPHONE-PCM'))
    e.consume(message('audio/chunk', conversationSessionId='other', seq=9, aec=False))
    assert e.report['frames'] == 3
    assert e.report['sequenceGaps'] == e.report['rawFrames'] == 0
    assert 'PRIVATE' not in json.dumps(e.report)
    assert 'test-session' not in json.dumps(e.report)


def test_gaps_and_raw_capture_are_not_hidden():
    e = evidence()
    for seq in (1, 3, 3):
        e.consume(message('audio/chunk', conversationSessionId='test-session', seq=seq, aec=False))
    assert e.report['sequenceGaps'] == 2
    assert e.report['rawFrames'] == 3


def test_transport_evidence_is_bounded_numeric_and_never_retains_private_line():
    e = evidence()
    line = b'{"topic":"audio/chunk","payload":{"data":"PRIVATE-PCM"}\x00'
    for _ in range(40):
        e.observe_line(line, ValueError('PRIVATE ERROR'))
    e.observe_line(b'PRIVATE BOOT LOG', ValueError())
    e.observe_read(64, 100)
    for seq in range(0, 80, 2):
        e.consume(message('audio/chunk', conversationSessionId='test-session', seq=seq, aec=True))
    r=e.report
    assert r['receive']['malformedAudioLines'] == 40
    assert r['receive']['nonJsonLines'] == 1
    assert len(r['receive']['malformedDetails']) == len(r['gapDetails']) == 32
    assert r['receive']['malformedDetails'][0]['controlBytes'] == 1
    assert r['gapDetails'][0]['previous'] == 0
    assert r['gapDetails'][0]['current'] == 2
    assert 'PRIVATE' not in json.dumps(r)


def test_ready_block_reader_returns_short_reads_without_waiting_to_fill():
    calls=[]
    def read(fd, size):
        calls.append((fd, size))
        return b'fragment'
    assert probe.read_ready_block(7, wait=lambda *args: ([7], [], []), read=read) == b'fragment'
    assert calls == [(7, 8192)]


def test_ready_block_reader_idle_and_races_do_not_discard_data_or_spin():
    def never_read(*args):
        raise AssertionError('must not read without readiness')
    assert probe.read_ready_block(7, wait=lambda *args: ([], [], []), read=never_read) == b''
    for error in (BlockingIOError(), InterruptedError()):
        calls=[]
        def transient(*args):
            calls.append(1)
            raise error
        assert probe.read_ready_block(7, wait=lambda *args: ([7], [], []), read=transient) == b''
        assert len(calls) == 1
    with pytest.raises(OSError, match='EOF'):
        probe.read_ready_block(7, wait=lambda *args: ([7], [], []), read=lambda *args: b'')


def test_stop_requires_capture_conversation_and_playback_false():
    e = evidence()
    e.consume(message('audio/status', boardDeviceId='test-board', active=False, conversation=False))
    assert not e.report['stopped']
    e.consume(message('audio/status', boardDeviceId='test-board', active=False,
                      conversation=False, playbackActive=True))
    assert not e.report['stopped']
    e.consume(message('audio/status', boardDeviceId='test-board', active=False,
                      conversation=False, playbackActive=False, unknownText='private'))
    assert e.report['stopped']
    assert 'private' not in json.dumps(e.report)


@pytest.mark.parametrize('topic,payload', [
    ('audio/status', dict(boardDeviceId='wrong')),
    ('audio/status', dict(boardDeviceId='test-board', conversationSessionId='test-session', aecReason='timeout')),
    ('audio/error', {}), ('protocol/error', {}), ('protocol/ack', dict(ok=False)),
    ('audio/diagnostic', dict(conversationSessionId='test-session', ok=False)),
    ('audio/chunk', dict(conversationSessionId='test-session', seq=True)),
])
def test_faults_fail_closed(topic, payload):
    with pytest.raises(RuntimeError):
        evidence().consume(message(topic, **payload))


def test_preparation_does_not_attribute_a_retired_aec_fault_to_new_capture():
    e = evidence()
    active = dict(boardDeviceId='test-board', active=True, conversation=True,
                  aecReason='reference_overflow')
    e.consume(message('audio/status', **active, conversationSessionId='old-session'))
    e.consume(message('audio/status', **active, conversationSessionId='test-session',
                      audioAecPreparing=True))
    with pytest.raises(RuntimeError, match='AEC fault'):
        e.consume(message('audio/status', **active, conversationSessionId='test-session',
                          audioAecPreparing=False))


def valid_evidence():
    e = evidence()
    e.report['frames'] = 1500
    e.report['integrityChecked'] = 1500
    e.report['audio'] = [dict(active=True, audioAec=True, audioAecPreparing=False, playbackActive=True, dspWindowSamples=128,
                              streamUnderruns=0, dspOver20msFrames=0)]
    e.report['memory'] = [baseline() for _ in range(6)]
    return e


def test_verify_accepts_complete_numeric_evidence():
    valid_evidence().verify()


def test_verify_rejects_malformed_audio_even_without_sequence_gap():
    e = valid_evidence()
    e.observe_line(b'{"topic":"audio/chunk"', ValueError())
    assert e.report['sequenceGaps'] == 0
    with pytest.raises(RuntimeError, match='Malformed audio JSON'):
        e.verify()


def test_pcm_checksum_checks_real_bytes_without_retaining_them():
    pcm = bytes(640)
    value = 0xcbf29ce484222325
    for byte in pcm:
        value = ((value ^ byte) * 0x100000001b3) & 0xffffffffffffffff
    p = dict(bytes=640, data=base64.b64encode(pcm).decode(), checksum=f'{value:016x}')
    assert probe.valid_pcm_checksum(p)
    for patch in ({'bytes': True}, {'data': '!'*856}, {'checksum': '0'*16},
                  {'data': base64.b64encode(bytes(639)).decode()}, {'checksum': None}):
        assert not probe.valid_pcm_checksum(dict(p, **patch))


@pytest.mark.parametrize('patch', [{'integrityChecked': 1499}, {'integrityFailures': 1}])
def test_integrity_failure_or_missing_coverage_cannot_pass(patch):
    e = valid_evidence()
    e.report.update(patch)
    with pytest.raises(RuntimeError, match='integrity'):
        e.verify()


def test_damaged_structure_never_retains_arbitrary_text_or_pcm():
    line = b'{"data":"PRIVATE_PCM123+/=",SECRET:INVALID}'
    result = probe.damaged_structure(line, SimpleNamespace(pos=27))
    assert set(result['errorShape']) <= set('{}[]:,"\\ \r\tx')
    assert result['fieldOffsets']['data'] == 1
    assert 'PRIVATE' not in json.dumps(result) and 'SECRET' not in json.dumps(result)


def test_block_reader_is_explicit_and_never_silently_substituted():
    e = evidence()
    with pytest.raises(ValueError):
        probe.DuplexReceiver(SimpleNamespace(), e, 'unknown')
    with pytest.raises(RuntimeError):
        probe.DuplexReceiver(SimpleNamespace(), e, 'block')


def test_initial_preparation_is_expected_but_later_readiness_loss_fails():
    e = valid_evidence()
    e.report['audio'].insert(0, dict(active=True, audioAec=False, audioAecPreparing=True))
    e.verify()
    e.consume(message('audio/status', boardDeviceId='test-board', active=True,
                      audioAec=True, audioAecPreparing=False))
    with pytest.raises(RuntimeError):
        e.consume(message('audio/status', boardDeviceId='test-board', active=True,
                          audioAec=False, audioAecPreparing=True))


@pytest.mark.parametrize('key', ['streamUnderruns', 'dspOver20msFrames'])
def test_missing_audio_counters_cannot_pass(key):
    e = valid_evidence()
    del e.report['audio'][0][key]
    with pytest.raises(RuntimeError):
        e.verify()


def test_absent_diagnostics_or_protocol_failure_cannot_pass():
    e = valid_evidence()
    e.report['memory'] = []
    with pytest.raises(RuntimeError):
        e.verify()
    e = valid_evidence()
    e.report['memory'][-1]['protocolMemory']['queueFull'] = 1
    with pytest.raises(RuntimeError):
        e.verify()


def test_wrong_board_never_starts_or_stops_audio(monkeypatch, tmp_path):
    sent = []
    class FakeLink:
        def __init__(self, port):
            self.s = SimpleNamespace(close=lambda: sent.append('closed'))
        def request(self, *args):
            return dict(boardDeviceId='wrong')
        def send(self, *args):
            sent.append(args)
    monkeypatch.setattr(probe, 'Link', FakeLink)
    output = tmp_path / 'report.json'
    with pytest.raises(RuntimeError):
        probe.run(SimpleNamespace(port='unused', board='test-board', version='candidate',
                                  seconds=30, output=output))
    assert sent == ['closed']
    assert json.loads(output.read_text())['complete'] is False


def test_capture_stop_is_attempted_even_if_playback_flush_write_fails(monkeypatch, tmp_path):
    sent = []
    class FakeLink:
        def __init__(self, port):
            self.started = self.stopped = False
            self.s = SimpleNamespace(close=lambda: sent.append('closed'), in_waiting=1,
                                     read=self.read)
        def request(self, *args):
            return dict(ok=True, boardDeviceId='test-board', bootCount=3, faultResetCount=0,
                        runtime=dict(firmware='candidate', buildId='candidate',
                                     audioPlaybackActive=False, screenPage='main'),
                        memory=dict.fromkeys(('freeInternalBytes', 'minimumFreeInternalBytes',
                                              'largestInternalBlockBytes', 'freeDmaBytes',
                                              'freePsramBytes'), 100),
                        protocolMemory=baseline()['protocolMemory'])
        def send(self, topic, payload):
            sent.append((topic, payload))
            if topic == 'audio/play_flush':
                raise OSError('simulated transport failure')
            if topic == 'audio/conversation':
                self.started = True
                self.stopped = payload['enabled'] is False
        def resync_before_stop(self):
            sent.append('resync-attempted')
            raise OSError('resync failure must not suppress stop attempts')
        def read(self, size):
            if self.started and not self.stopped:
                raise RuntimeError('simulated capture failure')
            return (json.dumps(message('audio/status', boardDeviceId='test-board',
                                       active=False, conversation=False, playbackActive=False)) + '\n').encode()
    monkeypatch.setattr(probe, 'Link', FakeLink)
    output = tmp_path / 'report.json'
    with pytest.raises(RuntimeError):
        probe.run(SimpleNamespace(port='unused', board='test-board', version='candidate',
                                  seconds=30, output=output))
    assert any(isinstance(s, tuple) and s[0] == 'audio/conversation'
               and s[1]['enabled'] is False for s in sent)
    assert sent[-1] == 'closed'
    report = json.loads(output.read_text())
    assert report['stopped'] is True
    assert report['complete'] is False
    assert report['stopWriteFailure'] is True
    assert report['stopResyncFailure'] is True
    assert 'resync-attempted' in sent


def test_evidence_survives_driver_close_failure(monkeypatch, tmp_path):
    output = tmp_path / 'close-failure.json'
    def close():
        checkpoint = json.loads(output.with_suffix('.preclose.json').read_text())
        assert checkpoint['portReleased'] is False
        assert checkpoint['complete'] is False
        assert checkpoint['failureType'] == 'RuntimeError'
        raise OSError('synthetic close failure')
    class FakeLink:
        def __init__(self, port):
            self.s = SimpleNamespace(close=close)
        def request(self, *args):
            return dict(boardDeviceId='wrong')
    monkeypatch.setattr(probe, 'Link', FakeLink)
    with pytest.raises(OSError):
        probe.run(SimpleNamespace(port='unused', board='test-board', version='candidate',
                                  seconds=30, output=output))
    assert not output.exists()  # Never claim the port was released.


def test_tone_is_bounded_and_generated_without_audio_io():
    import base64
    import struct
    pattern = probe.tones()
    assert len(pattern) == 20
    for chunk in pattern:
        data = base64.b64decode(chunk)
        assert len(data) == 3200
        assert max(abs(v) for v in struct.unpack('<1600h', data)) <= 900


class IncomingPort:
    def __init__(self):
        self.incoming = queue.Queue()
        self.read_seen = threading.Event()
    @property
    def in_waiting(self):
        return 1
    def read(self, size):
        try:
            result = self.incoming.get(timeout=.01)
            self.read_seen.set()
            return result
        except queue.Empty:
            return b''
    def feed(self, value):
        self.incoming.put((json.dumps(value) + '\n').encode())
    def write(self, data):
        # Model the host write waiting for input drain. A single-threaded
        # read-after-write pump cannot satisfy this condition.
        assert self.read_seen.wait(timeout=1)
        return len(data)


def wait_for(condition):
    deadline = time.monotonic() + 1
    while not condition() and time.monotonic() < deadline:
        time.sleep(.005)
    assert condition()


def test_reader_drains_while_writer_is_waiting():
    port, e = IncomingPort(), evidence()
    receiver = probe.DuplexReceiver(port, e)
    receiver.start()
    try:
        port.feed(message('audio/chunk', conversationSessionId='test-session',
                          seq=0, aec=True, data='PRIVATE-PCM'))
        assert port.write(b'output') == 6
        wait_for(lambda: e.report['frames'] == 1)
        receiver.check()
        assert 'PRIVATE' not in json.dumps(e.report)
    finally:
        receiver.close()
    assert not receiver.thread.is_alive()


@pytest.mark.parametrize('size', [1, 3, 64, 510, 1020, 8192])
def test_serial_fragmentation_does_not_drop_or_duplicate_frames(size):
    port, e = IncomingPort(), evidence()
    stream = b''.join((json.dumps(message('audio/chunk', conversationSessionId='test-session',
                         seq=i, aec=True, data='PRIVATE-PCM'))+'\n').encode() for i in range(30))
    for offset in range(0, len(stream), size):
        port.incoming.put(stream[offset:offset+size])
    receiver = probe.DuplexReceiver(port, e)
    receiver.start()
    try:
        wait_for(lambda: e.report['frames'] == 30)
        assert e.report['sequenceGaps'] == 0
        assert e.report['receive']['malformedAudioLines'] == 0
        assert e.report['receive']['bytes'] == len(stream)
        assert 'PRIVATE' not in json.dumps(e.report)
        receiver.check()
    finally:
        receiver.close()


def test_reader_keeps_accepting_stop_confirmation_after_fault():
    port, e = IncomingPort(), evidence()
    receiver = probe.DuplexReceiver(port, e)
    receiver.start()
    try:
        port.feed(message('audio/error', detail='PRIVATE-ERROR-DATA'))
        wait_for(lambda: receiver.failure is not None)
        with pytest.raises(RuntimeError, match='Receiver failed: device_audio_error'):
            receiver.check()
        port.feed(message('audio/status', boardDeviceId='test-board', active=False,
                          conversation=False, playbackActive=False))
        wait_for(lambda: e.report['stopped'])
        assert receiver.failure == 'device_audio_error'
    finally:
        receiver.close()


def test_receive_overflow_fails_closed_and_does_not_prevent_stop_reply():
    port, e = IncomingPort(), evidence()
    receiver = probe.DuplexReceiver(port, e)
    receiver.start()
    try:
        for _ in range(3):
            port.incoming.put(b'x' * 65536)
        wait_for(lambda: receiver.failure is not None)
        port.feed(message('audio/status', boardDeviceId='test-board', active=False,
                          conversation=False, playbackActive=False))
        wait_for(lambda: e.report['stopped'])
    finally:
        receiver.close()


def test_previous_stream_counter_is_not_current_playback_but_live_underrun_fails():
    e = valid_evidence()
    e.report['audio'].insert(0, dict(active=True, audioAec=True, audioAecPreparing=False,
                                   playbackActive=False, streamUnderruns=100, dspOver20msFrames=0))
    e.verify()
    e.report['audio'][-1]['streamUnderruns'] = 1
    with pytest.raises(RuntimeError):
        e.verify()


def test_playback_never_observed_cannot_pass():
    e = valid_evidence()
    e.report['audio'][0]['playbackActive'] = False
    with pytest.raises(RuntimeError):
        e.verify()


def test_known_progress_writer_resumes_only_suffix_and_does_not_poll_after_success():
    received, stats, attempts = bytearray(), {}, []
    def write(part):
        attempts.append(1)
        if len(attempts) in (2, 4):
            raise BlockingIOError()
        count = min(7, len(part))
        received.extend(part[:count])
        return count
    raw = bytes(range(200))
    probe.write_known_progress(raw, write, stats, sleep=lambda _: None)
    assert received == raw
    assert stats['writtenBytes'] == len(raw)
    assert stats['blockedCalls'] == 2
    assert set(stats) == {'frameBytes', 'writtenBytes', 'blockedCalls', 'elapsedMs'}


def test_known_progress_writer_deadline_is_bounded_without_replaying_prefix():
    elapsed, received, stats = [0.0], bytearray(), {}
    def write(part):
        if not received:
            received.extend(part[:3])
            return 3
        raise BlockingIOError()
    def sleep(seconds):
        elapsed[0] += seconds
    with pytest.raises(TimeoutError):
        probe.write_known_progress(b'example', write, stats, clock=lambda: elapsed[0], sleep=sleep)
    assert received == b'exa'
    assert stats['writtenBytes'] == 3
    assert 3000 <= stats['elapsedMs'] < 3010


@pytest.mark.parametrize('count', [0, -1, 1000, True])
def test_known_progress_writer_invalid_counts_fail(count):
    with pytest.raises(OSError):
        probe.write_known_progress(b'abc', lambda _: count, {})
