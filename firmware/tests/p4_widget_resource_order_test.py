"""Validate resource ordering with the production descriptor and checksum code."""
from p4_phase2_transaction_test import COMMON, compile_c, production
from p4_render_trace_test import function
import subprocess


def test_sorted_wire_resources_keep_the_declared_flash_layout(tmp_path):
    code = production("pet_p4_miniapp.c")
    fixture = COMMON + r"""
#define PET_P4_KEYFRAME_BG 1
#define PET_SURFACE_KEYFRAMES 4
#define PET_P4_MINIAPP_SPRITE_ID_MAX 16
#define PET_P4_MINIAPP_SPRITE_DECLARED_MAX 8
#define PET_P4_MINIAPP_SPRITE_MAX 11
#define PET_WTRACE_CHECKSUM 0
#define PET_WIDGET_TRACE_SCOPE(name, type) ((void)0)
typedef struct {
  char id[16];
  uint16_t frame_width, frame_height;
  uint8_t frames, fps;
  uint32_t data_offset, data_length;
} pet_p4_miniapp_sprite_t;
typedef struct {
  uint8_t count;
  size_t data_length, wasm_size;
  pet_p4_miniapp_sprite_t items[11];
  uint8_t pixels[24];
} pet_p4_miniapp_sprite_pack_t;
static bool pet_json_source_bounded_text(const char *text) { return strlen(text) < 2000; }
"""
    start = code.index("static uint32_t sprite_pack_checksum(const pet_p4_miniapp_sprite_pack_t *pack) {")
    fixture += code[start:code.index("static bool parse_checksum_hex", start)]
    fixture += "\n".join(function(code, name) for name in (
        "json_string", "json_int_range", "sprite_pack_find", "order_widget_resources"
    ))
    fixture += r"""
static pet_p4_miniapp_sprite_t make(const char *id, unsigned offset) {
  pet_p4_miniapp_sprite_t item = {.data_offset=offset, .data_length=1, .frame_width=64,
    .frame_height=32, .frames=1, .fps=0};
  snprintf(item.id,sizeof(item.id),"%s",id);
  return item;
}
static void check_flash_order(const char *json, const char **order, unsigned count,
                             pet_p4_miniapp_sprite_pack_t pack) {
  pet_p4_miniapp_sprite_pack_t original=pack, persisted={.count=count,.data_length=count};
  assert(order_widget_resources(json,&pack));
  assert(!memcmp(pack.pixels,original.pixels,sizeof(pack.pixels)));
  for(unsigned i=0;i<count;i++){
    int before=sprite_pack_find(&original,order[i]);
    assert(before>=0 && !strcmp(pack.items[i].id,order[i]));
    assert(pack.items[i].data_offset==original.items[before].data_offset);
    persisted.items[i]=pack.items[i];
    persisted.items[i].data_offset=i;
    persisted.pixels[i]=pack.pixels[pack.items[i].data_offset];
  }
  assert(sprite_pack_checksum(&pack)==sprite_pack_checksum(&persisted));
  assert(order_widget_resources(json,&pack));
  assert(sprite_pack_checksum(&pack)==sprite_pack_checksum(&persisted));
}
int main(void) {
  pet_p4_miniapp_sprite_pack_t pack={.count=4,.data_length=4,.pixels={16,44,73,90}};
  pack.items[0]=make("bg",0);pack.items[1]=make("envelope",1);
  pack.items[2]=make("gold",2);pack.items[3]=make("pink",3);
  const char *order[]={"bg","gold","pink","envelope"};
  check_flash_order("{\"surface\":{\"images\":[{\"id\":\"bg\"},{\"id\":\"gold\"},{\"id\":\"pink\"},{\"id\":\"envelope\"}]}}",order,4,pack);
  const pet_p4_miniapp_sprite_pack_t unchanged=pack;
  assert(!order_widget_resources("{\"surface\":{\"images\":[{\"id\":\"bg\"},{\"id\":\"gold\"},{\"id\":\"gold\"},{\"id\":\"envelope\"}]}}",&pack));
  assert(!memcmp(&pack,&unchanged,sizeof(pack)));
  pack.items[1]=make("bg.2",1);pack.items[2]=make("bg.3",2);pack.items[3]=make("icon",3);
  const char *animated[]={"bg","icon","bg.2","bg.3"};
  check_flash_order("{\"surface\":{\"images\":[{\"id\":\"bg\",\"encoding\":\"jpeg\",\"frames\":3},{\"id\":\"icon\"}]}}",animated,4,pack);
  pack.count=pack.data_length=2;pack.items[0]=make("a",0);pack.items[1]=make("z",1);
  const char *scene[]={"z","a"};
  check_flash_order("{\"scene\":{\"sprites\":[{\"id\":\"z\"},{\"id\":\"a\"}]}}",scene,2,pack);
  return 0;
}
"""
    executable = compile_c(tmp_path, fixture, "resource-order")
    subprocess.run([str(executable)], check=True, timeout=10)
