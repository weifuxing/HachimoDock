#!/usr/bin/env python3
"""Summarize sanitized probe evidence, without turning idle data into AEC acceptance."""
import argparse
import json
import math
from pathlib import Path


def number(value):
    return type(value) in (int, float) and math.isfinite(value) and value >= 0


def series(values):
    valid = [v for v in values if number(v)]
    if not valid:
        return {'samples': 0, 'missing': len(values)}
    return {'samples': len(valid), 'missing': len(values) - len(valid),
            'min': min(valid), 'max': max(valid), 'first': valid[0],
            'last': valid[-1], 'lastMinusFirst': valid[-1] - valid[0]}


def summarize(report):
    samples = report.get('samples')
    if not isinstance(samples, list) or not samples or not all(isinstance(s, dict) for s in samples):
        raise ValueError('Report needs nonempty sanitized samples')
    for sample in samples:
        for key in ('runtime', 'memory', 'protocolMemory', 'renderCache'):
            if key in sample and not isinstance(sample[key], dict):
                raise ValueError('Malformed sample object')
    identities = [(s.get('boardFingerprint'), s.get('bootCount'),
                   s.get('runtime', {}).get('buildId')) for s in samples]
    identity_known = all(isinstance(i[0], str) and i[0] and type(i[1]) is int
                         and i[1] >= 0 and isinstance(i[2], str) and i[2] for i in identities)
    same_identity = identity_known and all(i == identities[0] for i in identities)
    uptimes = [s.get('runtime', {}).get('uptimeMs') for s in samples]
    increasing = all(number(v) for v in uptimes) and all(a < b for a, b in zip(uptimes, uptimes[1:]))
    normalized, groups = [], {}
    unstable = missing = 0
    for sample in samples:
        cache = sample.get('renderCache', {})
        if cache.get('stableDuringMemorySample') is False:
            unstable += 1
            continue
        free = sample.get('memory', {}).get('freePsramBytes')
        keys = ('streamBytes', 'workStreamBytes', 'streamSlots')
        if (cache.get('stableDuringMemorySample') is not True or not number(free)
                or not all(type(cache.get(k)) is int and cache[k] >= 0 for k in keys)):
            missing += 1
            continue
        normalized.append(free + cache['streamBytes'] + cache['workStreamBytes'])
        key = tuple(cache[k] for k in keys)
        groups.setdefault(key, []).append(free)
    counters = {}
    for key in ('budgetRejected', 'allocationFailures', 'queueFull'):
        values = [s.get('protocolMemory', {}).get(key) for s in samples]
        known = all(type(v) is int and v >= 0 for v in values)
        monotonic = known and all(a <= b for a, b in zip(values, values[1:]))
        counters[key] = {'complete': known, 'monotonic': monotonic,
                         'increase': values[-1] - values[0] if known and monotonic and same_identity else None}
    result = {
        'sourceComplete': report.get('complete') is True,
        'sampleCount': len(samples),
        'sameDeviceBootBuild': bool(same_identity),
        'uptimeStrictlyIncreasing': bool(increasing),
        'spanMs': uptimes[-1] - uptimes[0] if same_identity and increasing else None,
        'faultResetCount': series([s.get('faultResetCount') for s in samples]),
        'memory': {k: series([s.get('memory', {}).get(k) for s in samples]) for k in (
            'freeInternalBytes', 'minimumFreeInternalBytes', 'largestInternalBlockBytes',
            'freeDmaBytes', 'largestDmaBlockBytes', 'freePsramBytes', 'largestPsramBlockBytes')},
        'protocolCounters': counters,
        'renderWindowP95Us': series([s.get('runtime', {}).get('renderP95Us') for s in samples]),
        'queryRoundTripMs': series([s.get('roundTripMs') for s in samples]),
        'partialCacheNormalizedFreePsramBytes': series(normalized),
        'cacheSamplesExcluded': {'unstable': unstable, 'missingFields': missing},
        'sameCacheGroups': [dict(zip(('streamBytes', 'workStreamBytes', 'streamSlots'), k),
                                freePsramBytes=series(v)) for k, v in sorted(groups.items())],
        'observedPlaybackActiveSamples': sum(s.get('runtime', {}).get('audioPlaybackActive') is True for s in samples),
        'playbackStateMissingSamples': sum(type(s.get('runtime', {}).get('audioPlaybackActive')) is not bool for s in samples),
        'observedNonHomeSamples': sum(s.get('runtime', {}).get('screenPage') not in (None, 'main') for s in samples),
        'screenPageMissingSamples': sum(not isinstance(s.get('runtime', {}).get('screenPage'), str) for s in samples),
        'acceptance': 'idle-observation-only; not-proof-of-no-leak-or-audio-or-real-PC-load-acceptance',
    }
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('report', type=Path)
    parser.add_argument('--output', type=Path)
    opts = parser.parse_args()
    result = summarize(json.loads(opts.report.read_text()))
    encoded = json.dumps(result, indent=2, allow_nan=False) + '\n'
    if opts.output:
        with opts.output.open('x') as file:
            file.write(encoded)
    print(encoded, end='')


if __name__ == '__main__':
    main()
