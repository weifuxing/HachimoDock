"""The public count matches factory defaults; two legacy slots stay reserved."""
from pathlib import Path
import configparser
import subprocess
ROOT=Path(__file__).resolve().parents[1]
def test_catalog_extra_slot_requires_exact_private_complete_parent(tmp_path):
    path=tmp_path/'profile.c';path.write_text('#include "pet_p4_component_events.h"\n_Static_assert(PET_P4_MINIAPP_VISIBLE_MAX==EXPECTED,"visible capacity");\n_Static_assert(PET_CEV_COMPONENTS==EXPECTED+2,"physical capacity");\n')
    complete=['PET_COMPONENT_PROGRAM_ENABLED=1','PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
      'PET_COMPONENT_BACKGROUND_ENABLED=1','PET_COMPONENT_PROGRAM_PARENT_UNIFIED=1']
    for flags,expected in (([],40),(complete,40),(complete+['PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1'],41)):
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',f'-DEXPECTED={expected}',*[f'-D{f}' for f in flags],
          '-I',str(ROOT/'main'),'-c',str(path),'-o',str(tmp_path/'profile.o')],check=True)
    invalid=subprocess.run(['cc','-std=c11','-DEXPECTED=41','-DPET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1',
      '-I',str(ROOT/'main'),'-c',str(path),'-o',str(tmp_path/'bad.o')],capture_output=True,text=True)
    assert invalid.returncode and 'Private acceptance capacity requires' in invalid.stderr
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        flags=config[section].get('build_flags','')
        if section=='env:esp32_p4_program_parent_acceptance':
            assert config[section]['extends']=='env:esp32_p4_program_parent_candidate' and 'PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1' in flags
        else:assert 'PET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1' not in flags
