"""Backport two HID security-level-3 OR operators into a build-local SDK copy."""
import argparse
import hashlib
from pathlib import Path
REL = Path('components/bt/host/nimble/nimble/nimble/host/services/hid/src/ble_svc_hid.c')
V1 = '544b2682e2dd861b388e5d2c6c7fbae1395dd9296efccfd425e8d4778a51ebb5'
V3 = 'ea668acbedea693222841bd7b33e87a8c367869247fb8bb383d3ec097e34045f'
BEFORE = 'BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_WRITE_AUTHEN\n#endif\n                  0;'
AFTER = BEFORE.replace('AUTHEN\n', 'AUTHEN |\n')

def transform(data):
    digest = hashlib.sha256(data).hexdigest()
    if digest == V3:
        return data
    if digest != V1:
        raise ValueError('Unreviewed NimBLE HID source')
    text = data.decode()
    if text.count(BEFORE) != 2:
        raise ValueError('HID security flag anchors changed')
    return text.replace(BEFORE, AFTER).encode()

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk-dir', type=Path, required=True)
    parser.add_argument('--build-dir', type=Path, required=True)
    args = parser.parse_args()
    source = args.sdk_dir / REL
    data = source.read_bytes()
    fixed = transform(data)
    if fixed == data:
        print(source)
    else:
        output = args.build_dir / 'pet_p4_nimble_hid.c'
        if args.sdk_dir.resolve() in output.resolve().parents or output.is_symlink():
            raise ValueError('Output must be a separate build file')
        output.write_bytes(b'#line 1 "nimble/ble_svc_hid.c"\n' + fixed)
        print(output)
