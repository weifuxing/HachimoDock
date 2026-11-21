"""Real C allocation/queue paths with fault injection, not hardware acceptance."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def execute(tmp_path, code):
    source = tmp_path / 'test.c'
    source.write_text(code)
    exe = tmp_path / 'test'
    subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra',
                    '-Werror', '-fsanitize=address,undefined', '-I', str(ROOT/'main'),
                    str(source), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


def test_budget_and_latency_boundaries(tmp_path):
    execute(tmp_path, r'''
#include <assert.h>
#include <limits.h>
#include "pet_p4_protocol_budget.h"
#include "pet_p4_latency_window.h"
int main(void) {
  pet_protocol_budget_t b={0};
  assert(!pet_protocol_budget_acquire(&b,0));
  assert(!pet_protocol_budget_acquire(&b,SIZE_MAX));
  assert(pet_protocol_budget_acquire(&b,PET_PROTOCOL_PENDING_BYTES-PET_PROTOCOL_SMALL_RESERVE));
  assert(!pet_protocol_budget_acquire(&b,4096));
  for(int i=0;i<8;i++)assert(pet_protocol_budget_acquire(&b,2048));
  assert(!pet_protocol_budget_acquire(&b,1));
  assert(b.bytes==PET_PROTOCOL_PENDING_BYTES&&b.peak_bytes==b.bytes);
  pet_protocol_budget_release(&b,b.bytes);assert(b.bytes==0);
  assert(pet_protocol_budget_acquire(&b,4096));pet_protocol_budget_release(&b,4096);
  b.rejected=UINT32_MAX;assert(!pet_protocol_budget_acquire(&b,SIZE_MAX));
  assert(b.rejected==UINT32_MAX);
  assert(pet_protocol_duration(UINT64_MAX)==UINT32_MAX);
  pet_latency_window_t w={0},copy;uint32_t p95,p99;
  pet_latency_percentiles(&w,&p95,&p99);assert(!p95&&!p99);
  for(unsigned i=1;i<=100;i++)pet_latency_record(&w,i);
  copy=w;pet_latency_percentiles(&copy,&p95,&p99);assert(p95==95&&p99==99);
  assert(w.next==100&&w.count==100); // sorting a snapshot never changes recorder
  for(int i=0;i<128;i++)pet_latency_record(&w,20000);
  copy=w;pet_latency_percentiles(&copy,&p95,&p99);assert(p95==20000&&p99==20000);
  assert(w.count==128);pet_latency_record(&w,UINT64_MAX);
  copy=w;pet_latency_percentiles(&copy,&p95,&p99);assert(p99==20000);
}
''')


def test_current_queue_admission_allocation_and_queue_failure(tmp_path):
    source = (ROOT/'main/pet_p4_main.c').read_text()
    start = source.index('static bool enqueue_protocol_line(')
    end = source.index('\n#if defined(__GNUC__)', start)
    function = source[start:end]
    execute(tmp_path, r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "pet_p4_protocol_budget.h"
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define pdMS_TO_TICKS(x) (x)
#define pdTRUE 1
#define portENTER_CRITICAL(x) ((void)(x))
#define portEXIT_CRITICAL(x) ((void)(x))
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef struct {char *line;size_t charged_bytes;uint64_t enqueued_us;
  pet_p4_send_line_fn send_line;void *reply_ctx;} pet_p4_protocol_request_t;
typedef struct {char *line;} pet_p4_query_reply_t;
static pet_protocol_budget_t g_protocol_budget;
static int g_protocol_budget_lock,g_protocol_queue=1,g_state_mutex,g_state;
static atomic_bool g_storage_query_window;
static pet_p4_protocol_request_t queued;
static bool fail_alloc,fail_queue;
static unsigned allocations,errors;
static int64_t esp_timer_get_time(void){return 123;}
static void *heap_caps_malloc(size_t n,unsigned caps){
  assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));++allocations;
  return fail_alloc?NULL:malloc(n);
}
static int xSemaphoreTake(int m,int t){(void)m;(void)t;return 0;}
static void xSemaphoreGive(int m){(void)m;}
static bool pet_p4_handle_storage_query(int *s,const char *l,pet_p4_query_reply_t *r){
  (void)s;(void)l;(void)r;assert(0);return false;
}
static void cJSON_free(void *p){free(p);}
static int xQueueSend(int q,const void *r,int t){
  (void)q;(void)t;if(fail_queue)return 0;queued=*(const pet_p4_protocol_request_t*)r;return pdTRUE;
}
static void reply(const char *line,void *ctx){
  assert(ctx==(void*)7);assert(strstr(line,"command_queue_busy"));++errors;
}
''' + function + r'''
int main(void){
  assert(enqueue_protocol_line("hello",reply,(void*)7,NULL));
  assert(g_protocol_budget.bytes==6&&queued.charged_bytes==6);
  assert(queued.enqueued_us==123&&!strcmp(queued.line,"hello"));
  free(queued.line);pet_protocol_budget_release(&g_protocol_budget,queued.charged_bytes);
  fail_alloc=true;assert(!enqueue_protocol_line("a",reply,(void*)7,NULL));
  assert(!g_protocol_budget.bytes&&g_protocol_budget.allocation_failures==1);
  fail_alloc=false;fail_queue=true;
  assert(!enqueue_protocol_line("b",reply,(void*)7,NULL));
  assert(!g_protocol_budget.bytes&&g_protocol_budget.queue_full==1);
  g_protocol_budget.bytes=PET_PROTOCOL_PENDING_BYTES;
  unsigned before=allocations;
  assert(!enqueue_protocol_line("c",reply,(void*)7,NULL));
  assert(allocations==before&&g_protocol_budget.rejected==1&&errors==3);
  assert(!enqueue_protocol_line(NULL,reply,(void*)7,NULL));
}
''')


def test_boot_trace_is_bounded_and_first_observation_is_immutable(tmp_path):
    execute(tmp_path, r'''
#include <assert.h>
#include "pet_p4_boot_memory.h"
int main(void) {
  pet_boot_memory_trace_t trace = {0};
  assert(sizeof(trace) <= 256);
  for (unsigned i=0; i<PET_MEMORY_BOOT_STAGE_COUNT; ++i) {
    pet_boot_memory_sample_t s={i,100+i,90+i,80+i,1000+i};
    assert(pet_boot_memory_record(&trace,(pet_boot_stage_t)i,s));
    s.free_internal=0;
    assert(!pet_boot_memory_record(&trace,(pet_boot_stage_t)i,s));
    assert(trace.samples[i].free_internal==100+i);
  }
  pet_boot_memory_sample_t s={0};
  assert(!pet_boot_memory_record(&trace,(pet_boot_stage_t)-1,s));
  assert(!pet_boot_memory_record(&trace,PET_MEMORY_BOOT_STAGE_COUNT,s));
  assert(trace.recorded==((1U<<PET_MEMORY_BOOT_STAGE_COUNT)-1));
}
''')
