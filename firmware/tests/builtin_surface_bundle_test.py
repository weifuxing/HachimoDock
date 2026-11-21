"""OTA remains bounded; complete resource packages remain independently installable."""
import copy
import json
import sys
import zlib
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "firmware/tools"))
from build_builtin_bundle import (
    bootstrap_widget, component_root, compressed_arrays, DATA_SOURCE_PATH, FACTORY_WASM_IDS,
    read_component_wasm, render_bundle, render_data_source,
)
from p4_component_sprites import compile_component_sprites

def test_compressed_bootstrap_roundtrip_and_budget():
    bundle = render_bundle()
    source = DATA_SOURCE_PATH.read_text()
    packed = compressed_arrays(source)
    encoded = packed["pet_p4_builtin_components_zlib"]
    assert zlib.decompress(encoded) == bundle.encode() + b"\0"
    assert len(bundle.encode()) < 160 * 1024
    assert len(encoded) < 32 * 1024
    for item in json.loads(bundle)["components"]:
        if item["widget"].get("hachiWasm"):
            name = "wasm_" + item["id"].replace("-", "_")
            raw = read_component_wasm(component_root(item["id"]), item["widget"])
            if item["id"] in FACTORY_WASM_IDS:
                assert raw is not None and name not in packed
            else:
                assert zlib.decompress(packed[name]) == raw
    assert render_data_source(bundle, packed) == source

def test_bootstrap_preserves_game_and_actions_without_changing_source():
    root = ROOT / "pc/builtin-clawpkgs/snake-turn"
    source = json.loads((root / "runtime/widget.json").read_text())
    before = copy.deepcopy(source)
    widget, external = bootstrap_widget(source)
    assert source == before
    assert external
    assert widget["game"] == source["game"]
    assert widget["transitions"] == source["transitions"]
    assert widget["surface"]["game_view"] == source["surface"]["game_view"]
    assert not widget["surface"].get("images")
    assert not compile_component_sprites(root, widget)
    assert compile_component_sprites(root, source)

def test_full_surface_cache_rejects_stale_declaration():
    import pytest
    root = ROOT / "pc/builtin-clawpkgs/snake-turn"
    source = json.loads((root / "runtime/widget.json").read_text())
    source["surface"]["images"][0]["frame_width"] = 638
    with pytest.raises(ValueError):
        compile_component_sprites(root, source)

def test_every_bootstrap_surface_is_accepted_by_real_device_parser(tmp_path):
    source=tmp_path/'bootstrap.c'
    source.write_text('''#include "pet_p4_surface.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc,char **argv){(void)argc;FILE *f=fopen(argv[1],"rb");char b[16384];size_t n=fread(b,1,sizeof(b)-1,f);b[n]=0;fclose(f);cJSON *j=cJSON_Parse(b);pet_surface_t *s=calloc(1,sizeof(*s));const cJSON*q=cJSON_GetObjectItemCaseSensitive(j,"quota");unsigned cap=cJSON_IsString(q)&&!strcmp(q->valuestring,"p4-quota-v2")?48:cJSON_IsString(q)&&!strcmp(q->valuestring,"p4-quota-v3")?64:32;int ok=pet_surface_parse_profile(cJSON_GetObjectItemCaseSensitive(j,"surface"),s,cap,false);free(s);cJSON_Delete(j);return ok?0:1;}
''')
    vendor=ROOT/'firmware/tests/vendor/cjson';exe=tmp_path/'bootstrap'
    subprocess.run(['cc','-std=c11','-DPET_P4_QUOTA_V3=1','-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1','-DPET_P4_GRAPHICS_V2=1','-DPET_SERVICE_BRIDGE_ENABLED=1','-DPET_HOST_COLLECTION_ENABLED=1','-fsanitize=address,undefined','-I',str(ROOT/'firmware/main'),'-I',str(vendor),str(source),str(ROOT/'firmware/main/pet_p4_surface.c'),str(ROOT/'firmware/main/pet_p4_host_collection.c'),str(ROOT/'firmware/main/pet_p4_collection.c'),str(vendor/'cJSON.c'),'-lm','-o',str(exe)],check=True)
    for package in json.loads(render_bundle())['components']:
        if 'surface' not in package['widget']:continue
        path=tmp_path/(package['id']+'.json');path.write_text(json.dumps(package['widget']))
        assert subprocess.run([str(exe),str(path)]).returncode==0,package['id']
