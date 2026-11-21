#!/usr/bin/env python3
"""Opt-in silent device smoke: page/cache churn and serial reconnect, no audio.

Does not replace a normal Pet Manager load test or acoustic AEC acceptance.
Restores the original page, never activates/replaces individual components.
"""
import argparse
import json
from pathlib import Path
import subprocess
import time

from preserving_ota import Link
from runtime_memory_probe import sanitize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--board', required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', type=Path, required=True)
    opts = parser.parse_args()
    if opts.output.exists():
        parser.error('Use a new output file')
    busy = subprocess.run(['lsof', opts.port], capture_output=True, text=True)
    if busy.returncode == 0 or busy.returncode not in (0, 1) or busy.stderr.strip():
        parser.error('Port occupied or check failed')
    report = {'complete': False, 'audioTested': False, 'normalPcLoadTested': False,
              'psramLeakAcceptance': 'requires-cache-normalized-measurement',
              'pageCycles': [], 'reconnections': []}
    link = None
    original_page = None
    identity = None

    def diag():
        nonlocal identity
        raw = link.request('diagnostics/query', {}, 'diagnostics/status')
        if raw.get('boardDeviceId') != opts.board or raw['runtime']['firmware'] != opts.version:
            raise RuntimeError('Unexpected board/version')
        value = sanitize(raw)
        current = (value['bootCount'], value['faultResetCount'])
        if identity is not None and identity != current:
            raise RuntimeError('Device restarted during smoke test')
        identity = current
        return value

    try:
        link = Link(opts.port)
        before = diag()
        report['before'] = before
        original_page = before['runtime']['screenPage']
        # Re-entering an arbitrary active app could resume user audio/state.
        if original_page != 'main':
            raise RuntimeError('Return to the pet home page before this silent test')
        for _ in range(20):
            cycle = {}
            for page in ('components', 'main'):
                link.request('control/screen-page', {'page': page}, 'protocol/ack')
                time.sleep(.5)
                value = diag()
                if value['runtime']['screenPage'] != page:
                    raise RuntimeError('Page change not reflected in diagnostics')
                cycle[page] = value
            report['pageCycles'].append(cycle)
        link.s.close()
        link = None
        for _ in range(20):
            link = Link(opts.port)
            report['reconnections'].append(diag())
            link.s.close()
            link = None
            time.sleep(.2)
        homes = [c['main']['memory']['freeInternalBytes'] for c in report['pageCycles']]
        psram = [c['main']['memory']['freePsramBytes'] for c in report['pageCycles']]
        report['homeMemorySpreadBytes'] = max(homes) - min(homes)
        report['homePsramSpreadBytes'] = max(psram) - min(psram)
        # Pet animation rotates a two-slot PSRAM video cache, so endpoint heap
        # deltas alone are NOT a PSRAM leak test. Keep that acceptance pending;
        # do not raise the threshold until a run happens to pass. Internal RAM
        # should not have this cache churn; still flag a > 4 KiB decline there.
        if homes[-1] < homes[0] - 4096:
            raise RuntimeError('Home-page internal free memory declined during cycling')
        for value in [c[p] for c in report['pageCycles'] for p in ('main', 'components')] + report['reconnections']:
            for key in ('budgetRejected', 'allocationFailures', 'queueFull'):
                if key not in value['protocolMemory'] or key not in before['protocolMemory']:
                    raise RuntimeError('Missing protocol counters; cannot verify smoke test')
                if value['protocolMemory'].get(key) != before['protocolMemory'].get(key):
                    raise RuntimeError('Protocol failure counter increased')
        report['complete'] = True
    except Exception as exc:
        report['failureType'] = type(exc).__name__
        raise
    finally:
        if link is not None:
            try:
                if original_page == 'main':
                    link.request('control/screen-page', {'page': 'main'}, 'protocol/ack')
            finally:
                link.s.close()
        opts.output.parent.mkdir(parents=True, exist_ok=True)
        with opts.output.open('x') as file:
            json.dump(report, file, indent=2)
            file.write('\n')
    print(json.dumps({k: v for k, v in report.items()
                      if k not in ('before', 'pageCycles', 'reconnections')}, indent=2))
    print('Page cycles: 20; reconnects: 20; port released', flush=True)


if __name__ == '__main__':
    main()
