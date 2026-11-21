import copy
import importlib.util
from pathlib import Path

import pytest

spec = importlib.util.spec_from_file_location('summary', Path(__file__).parents[1] / 'tools/summarize_memory_probe.py')
summary = importlib.util.module_from_spec(spec)
spec.loader.exec_module(summary)


def report():
    samples = []
    for i in range(3):
        samples.append({'boardFingerprint': 'hashed', 'bootCount': 1, 'faultResetCount': 0,
                        'runtime': {'buildId': 'test', 'uptimeMs': 1000 + i*1000, 'screenPage': 'main', 'audioPlaybackActive': False},
                        'memory': {'freeInternalBytes': 100, 'freePsramBytes': 1000-i*100},
                        'protocolMemory': {'budgetRejected': 0, 'allocationFailures': 0, 'queueFull': 0},
                        'renderCache': {'streamBytes': 100+i*100, 'workStreamBytes': 200,
                                        'streamSlots': 2, 'stableDuringMemorySample': True}})
    return {'complete': True, 'samples': samples}


def test_partial_cache_normalization_does_not_claim_total_leak_acceptance():
    result = summary.summarize(report())
    assert result['spanMs'] == 2000
    assert result['partialCacheNormalizedFreePsramBytes']['min'] == 1300
    assert result['partialCacheNormalizedFreePsramBytes']['max'] == 1300
    assert result['memory']['freePsramBytes']['lastMinusFirst'] == -200
    assert result['protocolCounters']['queueFull']['increase'] == 0
    assert result['playbackStateMissingSamples'] == 0
    assert 'not-proof' in result['acceptance']


def test_unstable_cache_sample_still_counts_in_internal_minimum():
    data = report()
    data['samples'][1]['renderCache']['stableDuringMemorySample'] = False
    data['samples'][1]['memory']['freeInternalBytes'] = 10
    result = summary.summarize(data)
    assert result['memory']['freeInternalBytes']['min'] == 10
    assert result['cacheSamplesExcluded']['unstable'] == 1
    assert result['partialCacheNormalizedFreePsramBytes']['samples'] == 2


def test_missing_diagnostics_are_unknown_not_zero():
    data = report()
    for sample in data['samples']:
        del sample['renderCache']['workStreamBytes']
        del sample['protocolMemory']['queueFull']
        del sample['runtime']['audioPlaybackActive']
    result = summary.summarize(data)
    assert result['partialCacheNormalizedFreePsramBytes']['samples'] == 0
    assert result['cacheSamplesExcluded']['missingFields'] == 3
    assert result['protocolCounters']['queueFull']['increase'] is None
    assert result['memory']['largestInternalBlockBytes']['samples'] == 0
    assert result['playbackStateMissingSamples'] == 3


@pytest.mark.parametrize('change', ['reboot', 'counter-reset', 'backward-time', 'incomplete'])
def test_invalid_evidence_is_not_silently_accepted(change):
    data = report()
    if change == 'reboot':
        data['samples'][2]['bootCount'] = 2
    elif change == 'counter-reset':
        data['samples'][0]['protocolMemory']['queueFull'] = 1
    elif change == 'backward-time':
        data['samples'][1]['runtime']['uptimeMs'] = 1
    else:
        data['complete'] = False
    result = summary.summarize(data)
    if change in ('reboot', 'backward-time'):
        assert result['spanMs'] is None
    elif change == 'counter-reset':
        assert result['protocolCounters']['queueFull']['increase'] is None
    else:
        assert not result['sourceComplete']


def test_invalid_input_and_nonfinite_values():
    for value in ({}, {'samples': []}, {'samples': [None]}, {'samples': [{'memory': None}]}):
        with pytest.raises(ValueError):
            summary.summarize(value)
    assert summary.series([True, float('nan'), float('inf'), -1, None]) == {'samples': 0, 'missing': 5}
    data = report()
    before = copy.deepcopy(data)
    summary.summarize(data)
    assert data == before
