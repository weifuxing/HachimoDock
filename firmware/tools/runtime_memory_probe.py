#!/usr/bin/env python3
"""Read-only, bounded diagnostics sampling. No bind/reset/audio/flash commands.

An idle sample is NOT an AEC, playback or Wi-Fi acceptance test. Raw protocol
traffic and session data are deliberately excluded from the resulting report.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import time
import uuid


RUNTIME_FIELDS = (
    'firmware', 'buildId', 'gitSha', 'buildDirty', 'chipRevision', 'idfVersion',
    'runningPartition', 'imageState', 'uptimeMs', 'taskCount', 'screenPage',
    'lcdDsiLanes', 'lcdDsiLaneMbps', 'lcdPixelClockMHz', 'renderTargetIntervalMs',
    'renderLastUs', 'renderP95Us', 'renderMaxUs', 'renderDecodeUs',
    'renderWindowSamples', 'renderSubmittedFrames', 'renderUnchangedFrames',
    'inputDroppedEvents', 'touchDroppedEvents', 'audioReady',
    'audioPlaybackReady', 'audioPlaybackActive', 'miniappActive',
)
BOOT_STAGES = ('entry', 'storage', 'catalog', 'builtins', 'manifest', 'transport',
               'display', 'audio', 'first_frame', 'rx_tasks', 'afe', 'ready')
BOOT_FIELDS = ('uptimeMs', 'freeInternalBytes', 'minimumFreeInternalBytes',
               'largestInternalBlockBytes', 'freePsramBytes')


def optional_memory_details(payload):
    """Additive fields remain absent on older firmware, never synthetic zeroes."""
    result = {}
    stages = payload.get('bootMemory')
    if isinstance(stages, list):
        seen = set()
        clean = []
        for item in stages[:len(BOOT_STAGES)]:
            if not isinstance(item, dict):
                continue
            stage = item.get('stage')
            if (stage not in BOOT_STAGES or stage in seen
                    or any(type(item.get(k)) is not int or item[k] < 0 for k in BOOT_FIELDS)):
                continue
            seen.add(stage)
            clean.append(dict(stage=stage, **{k: item[k] for k in BOOT_FIELDS}))
        result['bootMemory'] = clean
    cache = payload.get('renderCache')
    if isinstance(cache, dict):
        keys = ('streamBytes', 'streamSlots', 'generation')
        if (all(type(cache.get(k)) is int and cache[k] >= 0 for k in keys)
                and type(cache.get('stableDuringMemorySample')) is bool):
            result['renderCache'] = {k: cache[k] for k in (*keys, 'stableDuringMemorySample')}
            if type(cache.get('workStreamBytes')) is int and cache['workStreamBytes'] >= 0:
                result['renderCache']['workStreamBytes'] = cache['workStreamBytes']
    return result


def sanitize(payload):
    board = payload.get('boardDeviceId')
    runtime = payload.get('runtime', {})
    memory = payload.get('memory', {})
    required = ('freeInternalBytes', 'minimumFreeInternalBytes',
                'largestInternalBlockBytes', 'freeDmaBytes', 'freePsramBytes')
    if (payload.get('ok') is not True or not isinstance(board, str) or not board
            or not isinstance(runtime, dict) or not isinstance(memory, dict)
            or not runtime.get('firmware') or not isinstance(payload.get('bootCount'), int)
            or any(type(memory.get(k)) not in (int, float) or memory[k] < 0 for k in required)):
        raise ValueError('Incomplete device diagnostics; cannot accept as a baseline')
    numeric = lambda obj: {k: v for k, v in obj.items()
                           if type(v) in (int, float) and v >= 0}
    return {
        'boardFingerprint': hashlib.sha256(board.encode()).hexdigest()[:16],
        'bootCount': payload['bootCount'],
        'faultResetCount': payload.get('faultResetCount'),
        'runtime': {k: runtime[k] for k in RUNTIME_FIELDS if k in runtime},
        'memory': numeric(memory),
        'protocolMemory': numeric(payload.get('protocolMemory', {})),
        **optional_memory_details(payload),
    }


def query(port, timeout=8, include_boot=False):
    request_id = 'memory-probe-' + uuid.uuid4().hex
    request = {'topic': 'diagnostics/query', 'payload': {'requestId': request_id}}
    if include_boot:
        request['payload']['includeBootMemory'] = True
    start = time.monotonic()
    port.write(('\n' + json.dumps(request) + '\n').encode())
    while time.monotonic() - start < timeout:
        line = port.read_until(b'\n', 65536)
        if not line.endswith(b'\n'):
            continue
        try:
            msg = json.loads(line[line.index(b'{'):])
            payload = msg.get('payload', {})
            if msg.get('topic') != 'diagnostics/status' or payload.get('requestId') != request_id:
                continue
            result = sanitize(payload)
            result['roundTripMs'] = round((time.monotonic() - start) * 1000, 3)
            return result
        except (ValueError, TypeError, AttributeError):
            # Bad/stale traffic must not produce a successful sample.
            continue
    raise TimeoutError('No complete, correlated diagnostics response; no device changes made')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--samples', type=int, default=5)
    parser.add_argument('--interval', type=float, default=1)
    parser.add_argument('--progress', action='store_true',
                        help='Print bounded numeric progress every 30 seconds')
    opts = parser.parse_args()
    if not 1 <= opts.samples <= 300 or not 1 <= opts.interval <= 60:
        parser.error('samples must be 1..300; interval must be 1..60 seconds')
    if opts.output.exists():
        parser.error('Output already exists; use a new baseline filename')
    occupied = subprocess.run(['lsof', opts.port], capture_output=True, text=True)
    if occupied.returncode == 0 or occupied.returncode not in (0, 1) or occupied.stderr.strip():
        parser.error('Port occupied or occupancy check failed; disconnect Pet Manager first')
    import serial

    report = {'scenario': 'read-only-diagnostics', 'complete': False,
              'acceptance': 'not-an-audio-or-pc-concurrency-test', 'samples': []}
    try:
        # Match the shipped client's steady line state, without a reset pulse.
        # Serial/JTAG may not deliver replies when DTR is left deasserted.
        port = serial.Serial(port=None, baudrate=4_000_000, timeout=.2,
                             write_timeout=2, exclusive=True)
        port.port = opts.port
        port.dtr = sys.platform != 'win32'
        port.rts = sys.platform != 'win32'
        port.open()
        with port:
            identity = None
            last_progress = 0.0
            for index in range(opts.samples):
                sample = query(port, include_boot=index == 0)
                current = (sample['boardFingerprint'], sample['bootCount'],
                           sample['runtime'].get('buildId'))
                if identity is not None and identity != current:
                    raise RuntimeError('Device identity/boot/build changed during sampling')
                identity = current
                report['samples'].append(sample)
                now = time.monotonic()
                if opts.progress and (index == 0 or now - last_progress >= 30
                                      or index + 1 == opts.samples):
                    print(json.dumps({'sample': index + 1, 'total': opts.samples,
                                      'uptimeMs': sample['runtime'].get('uptimeMs'),
                                      'freeInternalBytes': sample['memory']['freeInternalBytes'],
                                      'freePsramBytes': sample['memory']['freePsramBytes'],
                                      'renderCache': sample.get('renderCache'),
                                      'faultResetCount': sample.get('faultResetCount')}), flush=True)
                    last_progress = now
                if index + 1 < opts.samples:
                    time.sleep(opts.interval)
            report['complete'] = True
    except Exception as exc:
        # Do not store exception text which may contain local/remote addresses.
        report['failureType'] = type(exc).__name__
        raise
    finally:
        opts.output.parent.mkdir(parents=True, exist_ok=True)
        with opts.output.open('x') as file:
            json.dump(report, file, ensure_ascii=False, indent=2)
            file.write('\n')
    print(json.dumps(report, ensure_ascii=False, indent=2))


if __name__ == '__main__':
    main()
