#include <assert.h>
#include "pet_p4_center_layout.h"
int main(void) {
  const int settings_x = PET_CENTER_SETTINGS_X;
  const int settings_y = PET_CENTER_HEADER_TOP;
  assert(pet_center_settings_hit(settings_x, settings_y));
  assert(pet_center_settings_hit(settings_x + PET_CENTER_SETTINGS_W - 1,
    settings_y + PET_CENTER_HEADER_H - 1));
  assert(!pet_center_settings_hit(PET_CENTER_COUNT_X, settings_y));
  assert(!pet_center_settings_hit(settings_x, PET_CENTER_TAB_TOP));
  assert(pet_center_tab_hit(settings_x, settings_y) == -1);
  assert(pet_center_hit(settings_x, settings_y, 0, 4) == -1);
  for(int tab=0;tab<PET_CENTER_FILTER_COUNT;++tab){
    const int x=PET_CENTER_LEFT+tab*PET_CENTER_TAB_STEP;
    assert(pet_center_tab_hit(x,PET_CENTER_TAB_TOP)==tab);
    assert(pet_center_tab_hit(x+PET_CENTER_TAB_W-1,PET_CENTER_TAB_TOP+PET_CENTER_TAB_H-1)==tab);
    assert(pet_center_tab_hit(x+PET_CENTER_TAB_W,PET_CENTER_TAB_TOP)==-1);
    assert(pet_center_tab_hit(x,PET_CENTER_TOP)==-1);
  }
  assert(pet_center_matches(PET_CENTER_ALL,PET_CENTER_MEDIA));
  assert(!pet_center_matches(PET_CENTER_GAMES,PET_CENTER_TOOLS));
  assert(pet_center_matches(PET_CENTER_MEDIA,PET_CENTER_MEDIA));
  for(size_t count=0;count<=32;count++)for(size_t selected=0;selected<(count?count:1);selected++) {
    size_t start=selected/4*4;
    for(int slot=0;slot<4;slot++) {
      int x=24+(slot%2)*304,y=PET_CENTER_TOP+(slot/2)*PET_CENTER_STEP_Y;
      int expected=start+slot<count?(int)start+slot:-1;
      assert(pet_center_hit(x+10,y+10,selected,count)==expected);
      assert(pet_center_hit(x+PET_CENTER_CARD_W-1,y+PET_CENTER_CARD_H-1,selected,count)==expected);
      assert(pet_center_hit(x+PET_CENTER_CARD_W,y+PET_CENTER_CARD_H,selected,count)==-1);
    }
    assert(pet_center_hit(320,420,selected,count)==-1);
    size_t next=pet_center_page(selected,count,1),prev=pet_center_page(selected,count,-1);
    assert(!count||(next<count&&prev<count));
    assert(pet_center_page(next,count,-1)==start);
  }
  assert(pet_center_page(24,25,1)==0);
  assert(pet_center_page(0,25,-1)==24);
}
