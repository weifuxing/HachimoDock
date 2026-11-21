/* Native renderer metadata only. Neither a texture nor a guest capability.
 * A captured plan drives BOTH actual common drawing and exact matching, so a
 * mutable state observation cannot retroactively authorize an old overlay. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  int32_t touch_x,touch_y;
  uint8_t banner;
  bool touch,boot;
} pet_logical_common_plan_t;
typedef struct {pet_logical_common_plan_t plan;bool valid;} pet_logical_retention_t;
enum {
  PET_CONNECTION_NONE, PET_CONNECTION_OFFLINE, PET_CONNECTION_USB, PET_CONNECTION_WIFI
};
static inline uint8_t pet_connection_banner(bool usb, bool wifi) {
  return usb ? PET_CONNECTION_USB : wifi ? PET_CONNECTION_WIFI : PET_CONNECTION_OFFLINE;
}
static inline pet_logical_common_plan_t pet_logical_common_plan(bool state_present,
    bool touch,int32_t x,int32_t y,uint8_t banner,bool boot){
  pet_logical_common_plan_t p={.boot=boot};
  if(!state_present)return p;
  p.touch=touch;if(touch){p.touch_x=x;p.touch_y=y;}
  p.banner=banner;
  return p;
}
static inline bool pet_logical_common_equal(pet_logical_common_plan_t a,pet_logical_common_plan_t b){
  return a.banner==b.banner&&a.touch==b.touch&&a.boot==b.boot
    &&(!a.touch||(a.touch_x==b.touch_x&&a.touch_y==b.touch_y));
}
static inline bool pet_logical_common_empty(pet_logical_common_plan_t p){return !p.banner&&!p.touch&&!p.boot;}
/* Consume the previous successful owner BEFORE anything can fail or clear the
 * logical buffer. A later failed rotation/transfer must not retain authority. */
static inline bool pet_logical_retention_begin(pet_logical_retention_t *s,
    pet_logical_common_plan_t p,bool previous_native_valid,bool previous_app,bool current_app){
  bool ready=s->valid&&previous_native_valid&&previous_app&&current_app&&pet_logical_common_equal(s->plan,p);
  s->valid=false;return ready;
}
static inline void pet_logical_retention_finish(pet_logical_retention_t *s,
    pet_logical_common_plan_t p,bool complete_eligible_frame){
  s->valid=complete_eligible_frame;if(s->valid)s->plan=p;
}
_Static_assert(sizeof(pet_logical_common_plan_t)<=12&&sizeof(pet_logical_retention_t)<=16,"bounded native logical metadata");
