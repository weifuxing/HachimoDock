"""Exercise the native digest-covered manifest and catalog field parser."""
from pathlib import Path
import subprocess


def test_component_metadata_is_bounded_and_matches_the_staged_identity(tmp_path):
    firmware = Path(__file__).resolve().parents[1]
    vendor = firmware / "tests" / "vendor" / "cjson"
    source = tmp_path / "metadata.c"
    source.write_text(r'''
#include "pet_p4_component_metadata.h"
#include <assert.h>

int main(void) {
  const char manifest[] = "{\"id\":\"wasm-piano\",\"version\":\"1.1.0\",\"kind\":\"tool\"}";
  pet_p4_component_metadata_t meta = pet_p4_component_metadata_read(
    manifest, sizeof(manifest) - 1, "wasm-piano");
  assert(!strcmp(meta.version, "1.1.0"));
  assert(!strcmp(meta.kind, "tool"));

  meta = pet_p4_component_metadata_read(manifest, sizeof(manifest) - 1, "other");
  assert(!meta.version[0] && !meta.kind[0]);
  meta = pet_p4_component_metadata_read(manifest, sizeof(manifest) - 2, "wasm-piano");
  assert(!meta.version[0]);
  meta = pet_p4_component_metadata_fields(NULL, NULL);
  assert(!meta.version[0] && !meta.kind[0]);

  cJSON *version = cJSON_CreateString("123456789012345678901234567890123456789012345678");
  cJSON *kind = cJSON_CreateString("application");
  meta = pet_p4_component_metadata_fields(version, kind);
  assert(!meta.version[0] && !meta.kind[0]);
  cJSON_Delete(version);
  cJSON_Delete(kind);
  return 0;
}
''')
    executable = tmp_path / "metadata"
    subprocess.run(
        ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(vendor),
         "-I", str(firmware / "main"), str(source), str(vendor / "cJSON.c"),
         "-lm", "-o", str(executable)],
        check=True,
    )
    subprocess.run([str(executable)], check=True)
