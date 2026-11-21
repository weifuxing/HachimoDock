/* Shared drawing/hit-test geometry for the native component launcher. */
#pragma once
#include <stddef.h>
#include <stdbool.h>

typedef enum {
  PET_CENTER_ALL, PET_CENTER_GAMES, PET_CENTER_TOOLS, PET_CENTER_MEDIA, PET_CENTER_FILTER_COUNT
} pet_center_filter_t;

static inline bool pet_center_matches(pet_center_filter_t filter, pet_center_filter_t category) {
  return filter == PET_CENTER_ALL || filter == category;
}

enum { PET_CENTER_COLUMNS=2, PET_CENTER_PAGE_SIZE=4,
  PET_CENTER_LEFT=24, PET_CENTER_TOP=123, PET_CENTER_STEP_X=304,
  PET_CENTER_STEP_Y=144, PET_CENTER_CARD_W=288, PET_CENTER_CARD_H=132,
  PET_CENTER_TAB_TOP=76, PET_CENTER_TAB_H=36, PET_CENTER_TAB_STEP=150, PET_CENTER_TAB_W=142,
  PET_CENTER_HEADER_TOP=22, PET_CENTER_HEADER_H=30,
  PET_CENTER_COUNT_X=474, PET_CENTER_COUNT_W=142,
  PET_CENTER_SETTINGS_W=96, PET_CENTER_SETTINGS_X=PET_CENTER_COUNT_X-10-PET_CENTER_SETTINGS_W,
  PET_CENTER_SETTINGS_TOP=14, PET_CENTER_SETTINGS_H=46, PET_CENTER_SETTINGS_SLOP=6 };

static inline bool pet_center_settings_hit(int x, int y) {
  return x >= PET_CENTER_SETTINGS_X - PET_CENTER_SETTINGS_SLOP &&
         x < PET_CENTER_SETTINGS_X + PET_CENTER_SETTINGS_W + PET_CENTER_SETTINGS_SLOP &&
         y >= PET_CENTER_SETTINGS_TOP - PET_CENTER_SETTINGS_SLOP &&
         y < PET_CENTER_SETTINGS_TOP + PET_CENTER_SETTINGS_H + PET_CENTER_SETTINGS_SLOP;
}

static inline int pet_center_tab_hit(int x,int y) {
  if (x<PET_CENTER_LEFT || y<PET_CENTER_TAB_TOP || y>=PET_CENTER_TAB_TOP+PET_CENTER_TAB_H) return -1;
  int relative=x-PET_CENTER_LEFT;
  int tab=relative/PET_CENTER_TAB_STEP;
  return tab<PET_CENTER_FILTER_COUNT && relative%PET_CENTER_TAB_STEP<PET_CENTER_TAB_W ? tab : -1;
}
static inline int pet_center_hit(int x,int y,size_t selected,size_t count) {
  for(int slot=0;slot<PET_CENTER_PAGE_SIZE;slot++) {
    int left=PET_CENTER_LEFT+(slot%PET_CENTER_COLUMNS)*PET_CENTER_STEP_X;
    int top=PET_CENTER_TOP+(slot/PET_CENTER_COLUMNS)*PET_CENTER_STEP_Y;
    size_t index=(selected/PET_CENTER_PAGE_SIZE)*PET_CENTER_PAGE_SIZE+(size_t)slot;
    if(index<count&&x>=left&&x<left+PET_CENTER_CARD_W&&y>=top&&y<top+PET_CENTER_CARD_H)return (int)index;
  }
  return -1;
}
static inline size_t pet_center_page(size_t selected,size_t count,int direction) {
  if(!count)return 0;
  size_t pages=(count+PET_CENTER_PAGE_SIZE-1)/PET_CENTER_PAGE_SIZE;
  size_t page=selected/PET_CENTER_PAGE_SIZE;
  page=direction>0?(page+1)%pages:(page+pages-1)%pages;
  return page*PET_CENTER_PAGE_SIZE;
}
