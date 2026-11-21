"""No serial I/O: identity gates and lost-commit behavior before using OTA."""
import hashlib
import importlib.util
from pathlib import Path
import struct
import sys
from types import SimpleNamespace

import pytest

sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
import preserving_ota as ota


def image():
    data = bytearray(256)
    data[0] = 0xE9
    struct.pack_into('<H', data, 12, 18)
    struct.pack_into('<HH', data, 15, 300, 399)
    data[48:58] = b'0.7.104-p4'
    return bytes(data)


def test_image_identity_gates():
    raw = image()
    digest = hashlib.sha256(raw).hexdigest()
    assert ota.validate_image(raw, '0.7.104-p4', digest, 302) == (300, 399)
    for version, sha, revision in [('wrong', digest, 302), ('0.7.104-p4', 'wrong', 302),
                                   ('0.7.104-p4', digest, 103)]:
        with pytest.raises(ValueError):
            ota.validate_image(raw, version, sha, revision)


def test_ack_correlation_matches_actual_firmware():
    ack = {'topic': 'firmware/ack', 'payload': {'transferId': 't', 'phase': 'chunk', 'seq': 2}}
    assert ota.matches_response(ack, 'firmware/chunk', {'transferId': 't', 'seq': 2}, 'firmware/ack', 'rid')
    for p in [{'transferId': 'other', 'seq': 2}, {'transferId': 't', 'seq': 1}]:
        assert not ota.matches_response(ack, 'firmware/chunk', p, 'firmware/ack', 'rid')
    assert not ota.matches_response(ack, 'firmware/commit', {'transferId': 't'}, 'firmware/ack', 'rid')
    assert not ota.matches_response([], 'firmware/query', {}, 'firmware/status', 'rid')


def test_lost_commit_never_repeats_or_aborts(tmp_path):
    raw = image()
    path = tmp_path / 'app.bin'
    path.write_bytes(raw)
    opts = SimpleNamespace(board='b', image=path, version='0.7.104-p4',
                           sha256=hashlib.sha256(raw).hexdigest())

    class Fake:
        def __init__(self):
            self.calls = []

        def request(self, topic, payload, reply, timeout=15):
            self.calls.append(topic)
            if topic == 'diagnostics/query':
                return {'ok': True, 'boardDeviceId': 'b', 'bootCount': 8,
                        'runtime': {'firmware': '0.7.103-p4', 'chipRevision': 302},
                        'memory': {k: 100 for k in ('freeInternalBytes', 'minimumFreeInternalBytes',
                                   'largestInternalBlockBytes', 'freeDmaBytes', 'freePsramBytes')}}
            if topic == 'firmware/query':
                return {'supported': True, 'active': False, 'pendingVerify': False}
            if topic == 'widget/list':
                return {'ok': True, 'complete': True, 'items': []}
            if topic == 'firmware/commit':
                raise TimeoutError('lost commit ack')
            return {'transferId': payload['transferId'], 'ok': True,
                    'phase': topic.split('/')[-1], 'seq': payload.get('seq'),
                    'receivedBytes': len(raw)}

        def send(self, topic, payload):
            self.calls.append(topic)

    link = Fake()
    with pytest.raises(TimeoutError):
        ota.upgrade(link, opts, lambda *args: None)
    assert link.calls.count('firmware/commit') == 1
    assert 'firmware/abort' not in link.calls


@pytest.mark.parametrize('same_version', [False, True])
def test_catalog_migration_failure_keeps_observation_without_reflashing(tmp_path, monkeypatch, same_version):
    target_elf = 'bb' * 32
    raw = image()[:176] + bytes.fromhex(target_elf) + image()[208:]
    path=tmp_path/'app.bin';path.write_bytes(raw)
    opts=SimpleNamespace(board='b',image=path,version='0.7.104-p4',sha256=hashlib.sha256(raw).hexdigest())
    if same_version:
        opts.expected_running_elf = 'aa' * 32
    monkeypatch.setattr(ota.time,'sleep',lambda _:None)

    class Fake:
        def __init__(self):self.calls=[];self.diags=0;self.lists=0
        def request(self,topic,payload,reply,timeout=15):
            self.calls.append(topic)
            if topic=='diagnostics/query':
                self.diags+=1
                new_boot = self.diags > (2 if same_version else 1)
                return {'ok':True,'boardDeviceId':'b','bootCount':2 if new_boot else 1,'faultResetCount':0,
                    'runtime':{'firmware':opts.version if new_boot or same_version else '0.7.103-p4',
                               'chipRevision':302,'imageState':'valid','runningPartition':'ota_1' if new_boot else 'ota_0'},
                    'storage':{'activeAppearanceSlot':1},
                    'memory':{k:100 for k in ('freeInternalBytes','minimumFreeInternalBytes','largestInternalBlockBytes','freeDmaBytes','freePsramBytes')}}
            if topic == 'debug/identity':
                new_boot = self.diags > 2
                return {'engine':'p4-evidence-identity-v1','boardDeviceId':'b','bootId':'new' if new_boot else 'old',
                        'appElfSha256':target_elf if new_boot else opts.expected_running_elf}
            if topic=='firmware/query':
                return {'supported':True,'active':False,'pendingVerify':False,'nextPartition':{'label':'ota_1'}}
            if topic=='widget/list':
                self.lists+=1
                return {'ok':True,'complete':True,'items':[{'id':'original'}]+([{'id':'new-builtin'}] if self.lists>1 else [])}
            return {'transferId':payload['transferId'],'ok':True,'phase':topic.split('/')[-1],
                'seq':payload.get('seq'),'receivedBytes':len(raw),'sha256':opts.sha256,'version':opts.version,
                'targetPartition':'ota_1'}
        def send(self,topic,payload):self.calls.append(topic)

    link=Fake();saved={}
    with pytest.raises(RuntimeError,match='Component catalog changed'):
        ota.upgrade(link,opts,lambda key,value:saved.__setitem__(key,value))
    assert saved['before']['activeAppearanceSlot']==1
    assert saved['after_observed']['componentIds']==['original','new-builtin']
    assert saved['after_observed']['activeAppearanceSlot']==1
    if same_version:
        assert link.diags == 3
        assert saved['after_observed']['appElfSha256'] == target_elf
    assert 'after' not in saved  # A strict failure is never reported as a passed audit.
    assert link.calls.count('firmware/commit')==1
    assert 'firmware/abort' not in link.calls
