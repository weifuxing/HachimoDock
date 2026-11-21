"""Execute the real capability block under each shipping wireless profile."""
from pathlib import Path
import subprocess

ROOT=Path(__file__).resolve().parents[1]


def test_keyboard_capability_requires_compiled_nimble_and_wifi6_hardware(tmp_path):
    source=(ROOT/'main/pet_p4_protocol.c').read_text()
    key=source.index('"bluetoothKeyboardConfig"')
    start=source.rindex('#if ',0,key)
    end=source.index('#endif',key)+len('#endif')
    block=source[start:end]
    harness=tmp_path/'capability.c'
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <string.h>
static bool hardware;static int radio,fields,configuration,extended;
static bool pet_p4_is_wifi6_hardware(void){return hardware;}
static int pet_ble_shortcut_available(void){return radio;}
static void cJSON_AddStringToObject(void *o,const char *k,const char *v){
  (void)o;(void)v;fields++;configuration+=!strcmp(k,"bluetoothKeyboardConfig");
}
static void cJSON_AddBoolToObject(void *o,const char *k,bool v){
  (void)o;assert(v);extended+=!strcmp(k,"bluetoothKeyboardExtendedChords");
}
static void emit(void){void *capabilities=0;(void)capabilities;
'''+block+r'''
}
int main(void){
  for(unsigned board=0;board<2;board++)for(int state=-1;state<=1;state++){
    hardware=board;radio=state;fields=configuration=extended=0;emit();
    bool supported=PET_P4_SYSTEM_WIFI&&CONFIG_BT_NIMBLE_ENABLED&&hardware;
    assert(configuration==(int)supported);
    assert(extended==(int)supported);
    assert(fields==(supported?(radio>0?3:1):0));
  }
}
''')
    for wifi,nimble in [(0,0),(1,0),(1,1)]:
        binary=tmp_path/f'profile-{wifi}-{nimble}'
        subprocess.run(['cc','-std=c11','-fsanitize=address,undefined',
                        f'-DPET_P4_SYSTEM_WIFI={wifi}',f'-DCONFIG_BT_NIMBLE_ENABLED={nimble}',
                        str(harness),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
