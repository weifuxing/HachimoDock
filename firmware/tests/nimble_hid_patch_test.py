"""Verify the actual pinned SDK fix without changing the installed packages."""
import importlib.util
import os
from pathlib import Path
import subprocess
import pytest

ROOT=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('hid_patch',ROOT/'tools/patch_nimble_hid.py')
patch=importlib.util.module_from_spec(spec);spec.loader.exec_module(patch)


def test_rejects_unknown_sdk():
    with pytest.raises(ValueError,match='Unreviewed'):
        patch.transform(b'unreviewed HID implementation')


@pytest.mark.parametrize('family',['V1','V3'])
def test_pinned_hid_source_and_security_flags(family,tmp_path):
    sdk=os.environ.get(f'PET_P4_{family}_IDF_PATH')
    if not sdk:
        pytest.skip('Pinned SDK path not configured')
    source=Path(sdk)/patch.REL
    original=source.read_bytes()
    fixed=patch.transform(original)
    if family=='V1':
        assert fixed==original.replace(patch.BEFORE.encode(),patch.AFTER.encode())
        assert len(fixed)==len(original)+4
    else:
        assert fixed==original
    assert patch.BEFORE.encode() not in fixed
    # Execute the corrected production expression for both supported security levels.
    text=fixed.decode();start=text.index('    write_flags = BLE_GATT_CHR_F_WRITE |')
    end=text.index('0;',start)+2
    body=text[start:end]
    test=tmp_path/'flags.c'
    test.write_text('''#include <assert.h>
#define MYNEWT_VAL(x) (x)
#define BLE_GATT_CHR_F_WRITE 1
#define BLE_GATT_CHR_F_WRITE_ENC 2
#define BLE_GATT_CHR_F_WRITE_AUTHEN 4
int main(void){unsigned write_flags;
'''+body+'''
assert(write_flags==(BLE_SM_LVL==3?7:3));}
''')
    for level in [2,3]:
        exe=tmp_path/f'flags-{level}'
        subprocess.run(['cc','-Werror',f'-DBLE_SM_LVL={level}',str(test),'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True)
    assert source.read_bytes()==original
