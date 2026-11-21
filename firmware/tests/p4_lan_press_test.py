"""Bounded LAN key queue and wire/producer invariants; no device or network."""
from pathlib import Path
import json
import re
import subprocess
ROOT = Path(__file__).resolve().parents[1]

def test_queue_lifecycle(tmp_path):
    exe = tmp_path / 'lan-press'
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I', str(ROOT / 'main'), str(ROOT / 'tests/p4_lan_press_test.c'), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)

def test_status_current_old_peer_and_future_version_capacity():
    source = (ROOT / 'main/pet_p4_lan.c').read_text()
    section = source.split('const bool built =', 1)[1].split('const bool encoded =', 1)[0]
    version = re.search(r'set\(PROJECT_VER "([^"]+)"\)', (ROOT / 'CMakeLists.txt').read_text())[1] + '-wifi6'
    fields = {}
    for kind, name in re.findall(r'cJSON_Add(String|Bool|Number)ToObject\(status, "([^"]+)"', section):
        fields[name] = False if kind == 'Bool' else (4294967295 if kind == 'Number' else
            {'firmware':version, 'boardDeviceId':'p4-0123456789ab', 'bootId':'0123456789abcdef'}[name])
    fields['rtcKey'] = 7
    # Production socket error encodes 0x20000|uint16_t errno; other exits are smaller.
    fields['wirelessRealtimeLastExit'] = 0x2ffff
    # Fixed true capabilities cannot become false in this exact schema.
    for name in re.findall(r'cJSON_AddBoolToObject\(status, "([^"]+)", true\)', section):
        fields[name] = True
    wire = json.dumps(fields, separators=(',', ':')).encode()
    assert len(wire) <= 512  # old PC response cap, exact current public version
    fields['firmware'] = 'v' * 31  # maximum NUL-terminated ESP app descriptor version
    assert '!extended || (cJSON_AddNumberToObject(status, "ac"' in source
    assert 'cJSON_AddBoolToObject(status, "wd", true)' in source
    fields['ac'] = 16 * 1024 * 1024
    fields['wd'] = True
    future = json.dumps(fields, separators=(',', ':')).encode()
    assert len(future) + 6 <= 576  # includes conservative cJSON print slack
    assert '#define LAN_STATUS_MAX 576' in source
    pc = (ROOT.parent / 'pc/src-tauri/src/lan_agent.rs').read_text()
    assert 'const DEVICE_STATUS_MAX: usize = 576;' in pc

def test_legacy_probe_and_producer_routes_preserved():
    lan = (ROOT / 'main/pet_p4_lan.c').read_text()
    assert 'case 0: case LAN_OTA_MODE: if (size) return;' in lan
    assert 'case LAN_STATUS_MODE: if (size > 1) return;' in lan
    assert 'case LAN_PRESS_MODE: if (size != 36) return;' in lan
    assert 'client_seen_ms = now;' in lan
    assert 'request_generation == generation && paired' in lan
    assert '!atomic_load(&usb_client_active)' in lan
    assert 'memcmp(plain, pet_p4_boot_identity(), 16)' in lan
    source = (ROOT / 'main/pet_p4_input.c').read_text()
    producer = source.split('static void send_input_event(', 1)[1].split('/* R5 DSH', 1)[0]
    assert '#if PET_P4_SYSTEM_WIFI' in producer
    assert 'pet_p4_lan_realtime_press(state, event->ts_ms,' in producer
    assert producer.index('pet_p4_lan_realtime_press(state,') < producer.index('cJSON_CreateObject()')
    assert '    return;\n  }\n#endif' in producer
    assert 'pet_p4_lan_realtime_press' not in source.split('static void send_input_event(',1)[0]
    protocol = (ROOT / 'main/pet_p4_protocol.c').read_text()
    assert protocol.count('if (state->desktop_device_id[0]) pet_p4_lan_usb_active();') == 3
