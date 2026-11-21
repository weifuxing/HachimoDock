#include <assert.h>
#include "pet_p4_lan_press.h"
static const uint8_t a[16]={1}, b[16]={2};
int main(void) {
  pet_lan_press_t q={0};
  assert(!pet_lan_press_push(&q,10,10,0)); /* no receiving desktop */
  assert(pet_lan_press_poll(&q,10,a,0));
  assert(pet_lan_press_push(&q,11,11,0));
  const uint32_t first=q.events[0].seq;
  /* Lost poll response changes neither head nor sequence; not an ACK. */
  assert(pet_lan_press_poll(&q,12,a,0));assert(q.count==1 && q.events[0].seq==first);
  assert(!pet_lan_press_poll(&q,13,b,first));assert(q.count==1);
  assert(!pet_lan_press_poll(&q,14,a,first+1));assert(q.count==1);
  assert(pet_lan_press_poll(&q,15,a,first));assert(q.count==0);
  /* Lost ACK response is idempotent and cannot pop the next physical press. */
  assert(pet_lan_press_push(&q,16,16,2));assert(pet_lan_press_poll(&q,17,a,first));assert(q.count==1);
  for(unsigned i=1;i<PET_LAN_PRESS_CAPACITY;i++)assert(pet_lan_press_push(&q,18+i,18+i,1));
  assert(!pet_lan_press_push(&q,30,30,1));assert(q.count==PET_LAN_PRESS_CAPACITY);
  pet_lan_press_refresh(&q,4000,true);assert(q.count==0);
  assert(pet_lan_press_push(&q,4001,4001,1));
  /* USB/forget/offline invalidation revokes both events and receiver lease. */
  pet_lan_press_refresh(&q,4002,false);assert(!q.lease && !q.count);
  assert(!pet_lan_press_poll(&q,4003,a,first));
  assert(pet_lan_press_poll(&q,4004,b,0));assert(q.count==0);
  assert(pet_lan_press_push(&q,4005,4005,0));assert(q.events[0].seq>first);
  /* Lease expiry cannot deliver previous receiver's keys to another host. */
  assert(pet_lan_press_poll(&q,11000,a,0));assert(!q.count);
  assert(!pet_lan_press_push(&q,11001,8001,0)); /* original long press already 3s old */
  assert(!pet_lan_press_push(&q,11001,11002,0)); /* future timestamp */
  assert(!pet_lan_press_push(&q,11001,10999,0)); /* before new desktop lease */
  assert(pet_lan_press_push(&q,11002,11000,0));
  assert(q.events[0].until==14000); /* never 14002: input dispatch cannot extend TTL */
  pet_lan_press_refresh(&q,14000,true); assert(!q.count);
  q.sequence=UINT32_MAX;assert(!pet_lan_press_push(&q,11001,11001,0));
  assert(!pet_lan_press_poll(&q,11002,(const uint8_t[16]){0},0));
  return 0;
}
