import importlib.util
import json
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location(
    'memory_probe', Path(__file__).parents[1] / 'tools/runtime_memory_probe.py')
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def payload():
    return {'ok': True, 'boardDeviceId': 'test-board', 'bootCount': 1,
            'runtime': {'firmware': 'test', 'currentSessionId': 'private',
                        'sessionQueueIds': ['private'], 'renderP95Us': 123},
            'memory': dict.fromkeys(('freeInternalBytes', 'minimumFreeInternalBytes',
                                    'largestInternalBlockBytes', 'freeDmaBytes',
                                    'freePsramBytes'), 100)}


def test_sanitize_excludes_sessions_and_raw_identity():
    result = probe.sanitize(payload())
    assert result['runtime'] == {'firmware': 'test', 'renderP95Us': 123}
    assert len(result['boardFingerprint']) == 16
    assert 'private' not in json.dumps(result)
    assert 'test-board' not in json.dumps(result)
    assert result['protocolMemory'] == {}  # Older firmware is valid, not zero metrics.


def test_sanitize_preserves_display_profile():
    value = payload()
    profile = dict(lcdDsiLanes=2, lcdDsiLaneMbps=500, lcdPixelClockMHz=24,
                   renderTargetIntervalMs=33)
    value['runtime'].update(profile)
    assert probe.sanitize(value)['runtime'] == dict(
        firmware='test', renderP95Us=123, **profile)


@pytest.mark.parametrize('field', ['boardDeviceId', 'bootCount', 'memory', 'runtime'])
def test_missing_baseline_rejected(field):
    value = payload()
    del value[field]
    with pytest.raises(ValueError):
        probe.sanitize(value)


def test_query_ignores_stale_and_non_json():
    class Port:
        def write(self, data):
            request_id = json.loads(data)['payload']['requestId']
            current = payload()
            current['requestId'] = request_id
            stale = dict(current, requestId='old')
            self.lines = [b'log\n', b'{truncated',
                          (json.dumps({'topic': 'diagnostics/status', 'payload': stale}) + '\n').encode(),
                          (json.dumps({'topic': 'diagnostics/status', 'payload': current}) + '\n').encode()]

        def read_until(self, *args):
            return self.lines.pop(0)

    assert probe.query(Port())['runtime']['firmware'] == 'test'


def test_timeout_is_not_a_pass():
    class SilentPort:
        def write(self, data):
            assert json.loads(data)['topic'] == 'diagnostics/query'

    with pytest.raises(TimeoutError):
        probe.query(SilentPort(), timeout=0)


def test_new_memory_fields_are_bounded_and_private_fields_are_excluded():
    stage = dict(stage='entry', **dict.fromkeys(probe.BOOT_FIELDS, 123))
    value = payload()
    value['bootMemory'] = [dict(stage, path='private'), stage,
                           dict(stage, stage='unknown'), dict(stage, stage='afe', uptimeMs=True)]
    value['renderCache'] = dict(streamBytes=1000, streamSlots=2, generation=6,
                                stableDuringMemorySample=False, path='private')
    result = probe.sanitize(value)
    assert result['bootMemory'] == [stage]
    assert result['renderCache'] == dict(streamBytes=1000, streamSlots=2, generation=6,
                                         stableDuringMemorySample=False)
    assert 'private' not in json.dumps(result)


def test_incomplete_cache_does_not_become_a_stable_sample():
    value = payload()
    value['renderCache'] = dict(streamBytes=12, streamSlots=1, generation=1)
    assert 'renderCache' not in probe.sanitize(value)
    assert 'bootMemory' not in probe.sanitize(value)


@pytest.mark.parametrize('capacity', [None, -1, True, '123', 0, 12345])
def test_work_stream_capacity_is_optional_and_strictly_numeric(capacity):
    value = payload()
    value['renderCache'] = dict(streamBytes=12, streamSlots=1, generation=2,
                               stableDuringMemorySample=True,
                               workStreamBytes=capacity)
    result = probe.sanitize(value)['renderCache']
    if type(capacity) is int and capacity >= 0:
        assert result['workStreamBytes'] == capacity
    else:
        assert 'workStreamBytes' not in result


@pytest.mark.parametrize('include_boot', [False, True])
def test_boot_history_is_explicitly_requested_only(include_boot):
    class Port:
        def write(self, data):
            self.request = json.loads(data)['payload']
            current = dict(payload(), requestId=self.request['requestId'])
            self.response = (json.dumps({'topic': 'diagnostics/status', 'payload': current}) + '\n').encode()

        def read_until(self, *args):
            return self.response

    port = Port()
    probe.query(port, include_boot=include_boot)
    assert ('includeBootMemory' in port.request) is include_boot
