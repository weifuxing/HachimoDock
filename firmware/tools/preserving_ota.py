#!/usr/bin/env python3
"""Explicitly authorized application-only OTA. Never erase user partitions.

Requires exact board, image hash and version. No automatic commit retry, ROM
reset, binding, component installation, microphone or playback commands.
"""
import argparse
import base64
import hashlib
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import time
import uuid

from runtime_memory_probe import sanitize


def validate_image(image, version, digest, revision):
    if len(image) < 256 or image[0] != 0xE9 or struct.unpack_from('<H', image, 12)[0] != 18:
        raise ValueError('Not an ESP32-P4 application image')
    low, high = struct.unpack_from('<HH', image, 15)
    if not low <= revision <= high:
        raise ValueError('Image and actual chip revision do not match')
    if image[48:80].split(b'\0')[0].decode() != version:
        raise ValueError('Unexpected image version')
    if hashlib.sha256(image).hexdigest() != digest:
        raise ValueError('Unexpected image hash')
    return low, high


def matches_response(message, topic, payload, reply, request_id):
    if not isinstance(message, dict) or message.get('topic') != reply:
        return False
    p = message.get('payload')
    if not isinstance(p, dict):
        return False
    if reply == 'firmware/ack':
        # Existing firmware ACKs use transferId/phase/seq, not requestId.
        phase = topic.split('/')[-1]
        return (p.get('transferId') == payload.get('transferId')
                and p.get('phase') == phase
                and (phase != 'chunk' or p.get('seq') == payload.get('seq')))
    return p.get('requestId') == request_id


class Link:
    def __init__(self, port):
        import serial
        self.s = serial.Serial(port=None, baudrate=4_000_000, timeout=.15,
                               write_timeout=3, exclusive=True)
        self.s.port = port
        self.s.dtr = self.s.rts = sys.platform != 'win32'
        self.s.open()
        self.s.write(b'\n')

    def send(self, topic, payload):
        raw = (json.dumps({'topic': topic, 'payload': payload}, separators=(',', ':')) + '\n').encode()
        for start in range(0, len(raw), 64):
            self.s.write(raw[start:start+64])
            time.sleep(.0005)

    def request(self, topic, payload, reply, timeout=15):
        rid = 'mem-ota-' + uuid.uuid4().hex
        self.send(topic, dict(payload, requestId=rid))
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            line = self.s.read_until(b'\n', 65536)
            if not line.endswith(b'\n'):
                continue
            try:
                msg = json.loads(line[line.index(b'{'):])
            except (ValueError, TypeError):
                continue
            if not matches_response(msg, topic, payload, reply, rid):
                continue
            p = msg['payload']
            if p.get('ok') is False:
                raise RuntimeError('Device rejected ' + topic)
            return p
        raise TimeoutError(topic)


def upgrade(link, opts, save):
    before = link.request('diagnostics/query', {'includeBootMemory': True}, 'diagnostics/status')
    if before.get('boardDeviceId') != opts.board:
        raise ValueError('Unexpected board; no update started')
    image = opts.image.read_bytes()
    low, high = validate_image(image, opts.version, opts.sha256, before['runtime']['chipRevision'])
    expected_running_elf = getattr(opts, 'expected_running_elf', None)
    initial_identity = None
    target_elf = None
    if expected_running_elf is not None:
        if not re.fullmatch(r'[0-9a-f]{64}', expected_running_elf):
            raise ValueError('Expected running ELF must be a lowercase SHA-256 digest')
        # ESP image header + segment header + esp_app_desc_t app_elf_sha256 offset.
        target_elf = image[176:208].hex()
        initial_identity = link.request('debug/identity', {}, 'debug/identity')
        if (initial_identity.get('engine') != 'p4-evidence-identity-v1'
                or initial_identity.get('boardDeviceId') != opts.board
                or not initial_identity.get('bootId')
                or initial_identity.get('appElfSha256') != expected_running_elf):
            raise ValueError('Running ELF or board identity changed; no update started')
        if target_elf == '0' * 64 or target_elf == expected_running_elf:
            raise ValueError('Same firmware image or missing candidate ELF identity')
    elif before['runtime']['firmware'] == opts.version:
        raise ValueError('Same version requires an explicit running ELF digest')
    status = link.request('firmware/query', {}, 'firmware/status')
    if not status.get('supported') or status.get('active') or status.get('pendingVerify'):
        raise ValueError('OTA unavailable/busy/pending verification')
    original_partition = before['runtime'].get('runningPartition')
    target_partition = status.get('nextPartition', {}).get('label')
    if initial_identity and (target_partition not in ('ota_0', 'ota_1') or target_partition == original_partition):
        raise ValueError('Inactive application partition could not be verified')
    catalog = link.request('widget/list', {}, 'widget/inventory')
    if not catalog.get('complete') or not catalog.get('ok'):
        raise ValueError('Incomplete inventory; no update started')
    ids = [v['id'] for v in catalog['items']]
    save('before', {'diagnostics': sanitize(before), 'componentIds': ids,
                    'activeAppearanceSlot': before.get('storage', {}).get('activeAppearanceSlot'),
                    **({'appElfSha256': expected_running_elf} if initial_identity else {})})
    tid = 'memory-ota-' + uuid.uuid4().hex
    commit_sent = False
    try:
        ack = link.request('firmware/begin', {'transferId': tid, 'size': len(image),
            'sha256': opts.sha256, 'chipRevisionMin': low, 'chipRevisionMax': high}, 'firmware/ack', 30)
        if ack.get('transferId') != tid or ack.get('phase') != 'begin' or ack.get('ok') is not True:
            raise RuntimeError('Invalid begin acknowledgement')
        for seq, offset in enumerate(range(0, len(image), 4092)):
            part = image[offset:offset+4092]
            payload = {'transferId': tid, 'seq': seq, 'decodedSize': len(part),
                       'data': base64.b64encode(part).decode()}
            for attempt in range(3):
                try:
                    ack = link.request('firmware/chunk', payload, 'firmware/ack')
                    break
                except TimeoutError:
                    if attempt == 2:
                        raise
            if (ack.get('transferId') != tid or ack.get('phase') != 'chunk'
                    or ack.get('ok') is not True or ack.get('seq') != seq
                    or ack.get('receivedBytes') != offset + len(part)):
                raise RuntimeError('Invalid chunk acknowledgement')
            if seq % 100 == 0:
                print(f'OTA {offset + len(part)}/{len(image)}', flush=True)
        # From here on, never abort/retry: an absent response may already mean
        # the new boot partition was committed. Inspect boot state instead.
        commit_sent = True
        ack = link.request('firmware/commit', {'transferId': tid}, 'firmware/ack', 30)
        if (ack.get('transferId') != tid or ack.get('phase') != 'commit'
                or ack.get('ok') is not True or ack.get('sha256') != opts.sha256
                or ack.get('version') != opts.version
                or (target_partition and ack.get('targetPartition') != target_partition)):
            raise RuntimeError('Commit confirmation invalid; inspect before retrying')
        save('commit', {'version': ack['version'], 'sha256': ack['sha256'],
                        'targetPartition': ack.get('targetPartition')})
    finally:
        if not commit_sent:
            # Firmware verifies transferId; never abort someone else's transfer.
            link.send('firmware/abort', {'transferId': tid})
    print('Commit confirmed; checking new boot', flush=True)
    end = time.monotonic() + 75
    while time.monotonic() < end:
        time.sleep(1)
        try:
            after = link.request('diagnostics/query', {'includeBootMemory': True}, 'diagnostics/status', 5)
        except TimeoutError:
            continue
        if after.get('boardDeviceId') != opts.board:
            raise RuntimeError('Board changed after update')
        r = after['runtime']
        if (r.get('firmware') != opts.version or r.get('imageState') != 'valid'
                or r.get('runningPartition') == original_partition
                or (target_partition and r.get('runningPartition') != target_partition)):
            continue
        if initial_identity:
            try:
                identity = link.request('debug/identity', {}, 'debug/identity', 5)
            except TimeoutError:
                continue
            if (identity.get('engine') != 'p4-evidence-identity-v1'
                    or identity.get('boardDeviceId') != opts.board
                    or identity.get('appElfSha256') != target_elf
                    or not identity.get('bootId')
                    or identity['bootId'] == initial_identity['bootId']):
                raise RuntimeError('New boot image identity did not match the committed candidate')
        break
    else:
        raise RuntimeError('New boot not confirmed; inspect, do not repeat update')
    inventory = link.request('widget/list', {}, 'widget/inventory')
    if not inventory.get('complete') or not inventory.get('ok'):
        raise RuntimeError('Post-update inventory incomplete')
    # Persist what the device actually reports BEFORE strict assertions. A
    # catalog migration must remain inspectable without repeating a committed OTA.
    save('after_observed', {'diagnostics': sanitize(after),
         'componentIds': [v['id'] for v in inventory['items']],
         'activeAppearanceSlot': after.get('storage', {}).get('activeAppearanceSlot'),
         **({'appElfSha256': target_elf} if initial_identity else {})})
    if ids != [v['id'] for v in inventory['items']]:
        raise RuntimeError('Component catalog changed')
    if after['storage']['activeAppearanceSlot'] != before['storage']['activeAppearanceSlot']:
        raise RuntimeError('Appearance slot changed')
    if after['faultResetCount'] != before['faultResetCount']:
        raise RuntimeError('New fault reset detected')
    save('after', {'diagnostics': sanitize(after), 'componentIds': ids,
                  'appearanceSlotUnchanged': True, 'componentCatalogUnchanged': True,
                  **({'appElfSha256': target_elf} if initial_identity else {})})
    print(json.dumps({'firmware': r['firmware'], 'imageState': r['imageState'],
                      'componentCount': len(ids), 'faultResetCount': after['faultResetCount']}), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('port', 'board', 'version', 'sha256'):
        parser.add_argument('--' + name, required=True)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--expected-running-elf',
                        help='Verify the old and new ELF digests, including for a same-version build')
    parser.add_argument('--output-dir', type=Path, required=True)
    opts = parser.parse_args()
    occupied = subprocess.run(['lsof', opts.port], capture_output=True, text=True)
    if occupied.returncode == 0 or occupied.returncode not in (0, 1) or occupied.stderr.strip():
        parser.error('Port occupied or occupancy check failed')
    opts.output_dir.mkdir(parents=True, exist_ok=False)

    def save(name, value):
        with (opts.output_dir / (name + '.json')).open('x') as file:
            json.dump(value, file, indent=2)
            file.write('\n')

    link = Link(opts.port)
    try:
        upgrade(link, opts, save)
    finally:
        link.s.close()


if __name__ == '__main__':
    main()
