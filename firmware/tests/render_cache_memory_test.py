"""Allocation accounting, overlap/ABA rejection and production call-site bounds."""
from pathlib import Path
import subprocess
from p4_catalog_membership_reboot_test import function

ROOT = Path(__file__).resolve().parents[1]


def test_cache_accounting_excludes_inflight_and_failed_realloc(tmp_path):
    source = tmp_path/'counter.c'
    source.write_text(r'''
#include "pet_p4_render_cache_memory.h"
#include <assert.h>
int main(void) {
  pet_render_cache_memory_t s={0}, before=s;
  assert(pet_render_cache_memory_stable(before,s));
  pet_render_cache_memory_begin(&s);
  assert(!pet_render_cache_memory_stable(s,s));
  pet_render_cache_memory_end(&s,1024,1,UINT32_MAX);
  assert(s.stream_bytes==1024&&s.stream_slots==1&&!s.mutations);
  assert(!pet_render_cache_memory_stable(before,s));
  before=s;
  // Two concurrent allocations: completion of one cannot declare stability.
  pet_render_cache_memory_begin(&s);pet_render_cache_memory_begin(&s);
  pet_render_cache_memory_end(&s,2048,1,UINT32_MAX);
  assert(!pet_render_cache_memory_stable(s,s));
  pet_render_cache_memory_end(&s,0,0,4096);
  assert(s.stream_bytes==3072&&s.stream_slots==2&&s.work_stream_bytes==4096);
  assert(!pet_render_cache_memory_stable(before,s));
  before=s;
  // Failed growth keeps the old allocation, but invalidates overlapping samples.
  pet_render_cache_memory_begin(&s);pet_render_cache_memory_end(&s,0,0,4096);
  assert(s.work_stream_bytes==4096&&!pet_render_cache_memory_stable(before,s));
  before=s;
  // Free/reallocate of equal size is an ABA change, not a stable sample.
  pet_render_cache_memory_begin(&s);pet_render_cache_memory_end(&s,-1024,-1,UINT32_MAX);
  pet_render_cache_memory_begin(&s);pet_render_cache_memory_end(&s,1024,1,UINT32_MAX);
  assert(s.stream_bytes==before.stream_bytes&&!pet_render_cache_memory_stable(before,s));
  assert(pet_render_cache_memory_stable(s,s));
}
''')
    binary=tmp_path/'counter'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-I',str(ROOT/'main'),str(source),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)


def test_production_accounting_surrounds_only_heap_mutations():
    code=(ROOT/'main/pet_p4_renderer.c').read_text()
    for name, operation in [('asset_cache_release','heap_caps_free(previous.bytes)'),
                            ('prepare_asset_entry','heap_caps_malloc(job.size')]:
        body=function(code,name)
        assert body.index('cache_memory_begin();') < body.index(operation) < body.index('cache_memory_end(')
    realloc=code[code.index('  if (g_h264_work_capacity < stream_size) {'):]
    assert realloc.index('cache_memory_begin();') < realloc.index('heap_caps_realloc(') < realloc.index('cache_memory_end(') < realloc.index('if (!resized)')
    assert code.count('cache_memory_begin();') == code.count('    cache_memory_end(') == 3
    for name in ['cache_memory_begin','cache_memory_end','pet_p4_renderer_cache_memory']:
        body=function(code,name)
        assert all(op not in body for op in ['heap_caps_', 'malloc(', 'free(', 'vTaskDelay(', 'fopen('])
    diagnostic=(ROOT/'main/pet_p4_diagnostics.c').read_text()
    assert diagnostic.index('cache_before =') < diagnostic.index('"freePsramBytes", heap_caps_get_free_size') < diagnostic.index('cache_after =')
    assert 'pet_render_cache_memory_stable(cache_before, cache_after)' in diagnostic
