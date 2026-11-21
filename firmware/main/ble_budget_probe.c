/* WiFi6 lab candidate. HID keyboard reports are accepted only from the
 * physical-action path and only sent to an authenticated, bonded peer. */
#include "pet_p4_ble_shortcut.h"
#include "esp_hosted.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_sm.h"
#include "host/ble_store.h"
#include "services/gap/ble_svc_gap.h"
#include "services/bas/ble_svc_bas.h"
#include "services/gatt/ble_svc_gatt.h"
#include "services/dis/ble_svc_dis.h"
#include "services/hid/ble_svc_hid.h"
#include "services/sps/ble_svc_sps.h"
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define NO_PEER UINT16_MAX
#define PAIR_WINDOW_MS 60000
#define KEY_RELEASE_MS 45
#define HID_INPUT 0x2a4d
#define HID_BOOT_INPUT 0x2a22
static const char name[] = "HachimiDock";
static const uint8_t report_map[] = {
  0x05,0x01,0x09,0x06,0xa1,0x01,0x85,0x01,0x05,0x07,0x19,0xe0,0x29,0xe7,0x15,0,0x25,1,
  0x75,1,0x95,8,0x81,2,0x95,1,0x75,8,0x81,1,0x95,6,0x75,8,0x15,0,0x25,0x65,0x19,0,
  0x29,0x65,0x81,0,
  0x05,0x08,0x19,1,0x29,5,0x15,0,0x25,1,0x75,1,0x95,5,0x91,2,0x75,3,0x95,1,0x91,1,0xc0
};
static struct ble_npl_callout poll;
static _Atomic int available, paired, ready, radio_error;
static atomic_uint advertising;
#define CHORD_QUEUE_SIZE 16u
static uint16_t chord_queue[CHORD_QUEUE_SIZE];
static atomic_uint chord_read, chord_write;

static void clear_chord_queue(void) {
  atomic_store(&chord_read, atomic_load(&chord_write));
}

static uint16_t next_chord(void) {
  const unsigned read = atomic_load(&chord_read);
  if (read == atomic_load(&chord_write)) return 0;
  const uint16_t chord = chord_queue[read % CHORD_QUEUE_SIZE];
  atomic_store(&chord_read, read + 1);
  return chord;
}
static atomic_int directed_result;
static atomic_uint connects, disconnect_reason, security_result, address_hash;
static uint32_t fast_adv_until;
static bool fast_adv, security_requested, neutral_pending;
unsigned pet_ble_shortcut_link_counter(unsigned which) {
  switch (which) {
    case 0: return atomic_load(&connects);
    case 1: return atomic_load(&disconnect_reason);
    case 2: return atomic_load(&security_result);
    case 3: return atomic_load(&address_hash);
    default: return 0;
  }
}
static atomic_bool forget;
static _Atomic uint32_t pair_deadline, displayed_code, sent_press, sent_release, send_error;
unsigned pet_ble_shortcut_report_counter(unsigned which) {
  if (which==0) return atomic_load(&sent_press);
  if (which==1) return atomic_load(&sent_release);
  return atomic_load(&send_error);
}
static uint16_t peer = NO_PEER, input_handle, boot_handle, protocol_handle;
static ble_addr_t bonded_identity;
static bool bonded_identity_valid;
static uint32_t peer_since;
static bool input_subscribed, boot_subscribed, suspended, release_due, adv_open;
static uint8_t protocol_mode = 1, local_address;
static uint16_t release_handle;
static uint32_t release_at;

extern void ble_store_config_init(void);

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time()/1000); }

static bool pairing_open(void) {
  uint32_t deadline = atomic_load(&pair_deadline);
  return deadline && (int32_t)(deadline-now_ms()) > 0;
}

void pet_ble_shortcut_open_pair(void) {
  atomic_store(&pair_deadline,now_ms()+PAIR_WINDOW_MS);
}

void pet_ble_shortcut_close_pair(void) {
  atomic_store(&pair_deadline,0);
  atomic_store(&displayed_code,0);
}

void pet_ble_shortcut_forget(void) { atomic_store(&forget,true); }

int pet_ble_shortcut_radio_error(void) { return atomic_load(&radio_error); }

int pet_ble_shortcut_available(void) { return atomic_load(&available); }
int pet_ble_shortcut_directed_result(void) { return atomic_load(&directed_result); }
unsigned pet_ble_shortcut_advertising(void) { return atomic_load(&advertising); }
int pet_ble_shortcut_paired(void) { return atomic_load(&paired); }
bool pet_ble_shortcut_pairing(void) { return pairing_open(); }
unsigned pet_ble_shortcut_code(void) { return pairing_open() ? atomic_load(&displayed_code) : 0; }

static bool secure(uint16_t handle) {
  struct ble_gap_conn_desc d;
  return !ble_gap_conn_find(handle,&d) && d.sec_state.encrypted &&
    d.sec_state.authenticated && d.sec_state.bonded;
}

typedef struct {
  uint8_t enabled;
  uint8_t reserved;
  uint16_t keys[PET_BLE_GESTURE_COUNT];
} keyboard_config_t;

static keyboard_config_t keyboard = {
  // USB HID keyboard usages: Ctrl+C, Ctrl+V and Ctrl+A.
  .keys = { [0] = 0x0106, [3] = 0x0119, [6] = 0x0104 },
};
static atomic_bool keyboard_enabled;

void pet_ble_keyboard_load(void) {
  nvs_handle_t nvs;
  keyboard_config_t saved = {0};
  size_t size = sizeof(saved);
  if (nvs_open("pet_ble", NVS_READONLY, &nvs) == ESP_OK) {
    if (nvs_get_blob(nvs, "layout_v1", &saved, &size) == ESP_OK &&
        size == sizeof(saved) && saved.enabled <= 1) keyboard = saved;
    nvs_close(nvs);
  }
  atomic_store(&keyboard_enabled, keyboard.enabled);
}

bool pet_ble_shortcut_restore_requested(void) { return pet_ble_keyboard_enabled(); }
bool pet_ble_keyboard_enabled(void) { return atomic_load(&keyboard_enabled); }
bool pet_ble_keyboard_connected(void) { return pet_ble_keyboard_enabled() && atomic_load(&ready); }

static bool save_keyboard(const keyboard_config_t *next) {
  nvs_handle_t nvs;
  if (nvs_open("pet_ble", NVS_READWRITE, &nvs) != ESP_OK) return false;
  const bool saved = nvs_set_blob(nvs, "layout_v1", next, sizeof(*next)) == ESP_OK &&
                     nvs_commit(nvs) == ESP_OK;
  nvs_close(nvs);
  if (saved) keyboard = *next;
  return saved;
}

bool pet_ble_keyboard_set_mode(bool enabled) {
  if (keyboard.enabled != enabled) {
    keyboard_config_t next = keyboard;
    next.enabled = enabled;
    if (!save_keyboard(&next)) return false;
  }
  atomic_store(&keyboard_enabled, enabled);
  if (enabled && !pet_ble_shortcut_paired()) pet_ble_shortcut_open_pair();
  if (!enabled) pet_ble_shortcut_close_pair();
  return true;
}

static bool keyboard_slot_valid(unsigned slot) {
  return slot < PET_BLE_GESTURE_COUNT && slot != 10 && (slot < 12 || (slot % 2) == 0);
}

bool pet_ble_keyboard_set_chord(unsigned slot, uint16_t chord) {
  if (!keyboard_slot_valid(slot)) return false;
  const unsigned key = chord & 0xff;
  // The high byte is the HID modifier bitmap. Usage zero sends modifiers only.
  if (key && (key < 4 || key > 0x65)) return false;
  if (keyboard.keys[slot] == chord) return true;
  keyboard_config_t next = keyboard;
  next.keys[slot] = chord;
  return save_keyboard(&next);
}

uint16_t pet_ble_keyboard_chord(unsigned slot) {
  return keyboard_slot_valid(slot) ? keyboard.keys[slot] : 0;
}

static bool trusted_peer(void) {
  struct ble_gap_conn_desc d;
  return bonded_identity_valid && !ble_gap_conn_find(peer,&d)
    && !ble_addr_cmp(&d.peer_id_addr,&bonded_identity);
}

static void update_ready(void) {
  const bool can_send=pet_ble_keyboard_enabled() && peer != NO_PEER && !suspended && trusted_peer() && secure(peer) &&
    (protocol_mode ? input_subscribed : boot_subscribed);
  if (!atomic_exchange(&ready,can_send) && can_send) neutral_pending=true;
}

bool pet_ble_keyboard_send(uint16_t chord) {
  if (!chord || !pet_ble_keyboard_connected()) return false;
  const unsigned write = atomic_load(&chord_write);
  if (write - atomic_load(&chord_read) >= CHORD_QUEUE_SIZE) {
    atomic_fetch_add(&send_error, 1);
    return false;
  }
  chord_queue[write % CHORD_QUEUE_SIZE] = chord;
  atomic_store(&chord_write, write + 1);
  return true;
}

static bool read_gatt(uint16_t handle, void *data, size_t size) {
  struct os_mbuf *value=NULL;
  if (!handle || ble_att_svr_read_local(handle,&value)) return false;
  const bool ok=OS_MBUF_PKTLEN(value)==size && !os_mbuf_copydata(value,0,size,data);
  os_mbuf_free_chain(value);
  return ok;
}

static void register_gatt(struct ble_gatt_register_ctxt *ctxt, void *arg) {
  (void)arg;
  if (ctxt->op==BLE_GATT_REGISTER_OP_CHR) {
    const uint16_t uuid=ble_uuid_u16(ctxt->chr.chr_def->uuid);
    if (uuid==HID_BOOT_INPUT) boot_handle=ctxt->chr.val_handle;
    if (uuid==BLE_SVC_HID_CHR_UUID16_PROTOCOL_MODE) protocol_handle=ctxt->chr.val_handle;
  } else if (ctxt->op==BLE_GATT_REGISTER_OP_DSC &&
             ble_uuid_u16(ctxt->dsc.dsc_def->uuid)==BLE_SVC_HID_DSC_UUID16_RPT_REF) {
    uint8_t report[2];
    if (ctxt->dsc.dsc_def->arg && read_gatt(ctxt->dsc.handle,report,sizeof(report)) &&
        report[0]==1 && report[1]==BLE_SVC_HID_RPT_TYPE_INPUT)
      input_handle=*(const uint16_t*)ctxt->dsc.dsc_def->arg;
  }
}

// ESP-IDF's HID generator adds an External Report Reference even when the
// report map uses no external characteristic. Keep its services and access
// handlers, but omit that descriptor from this keyboard's report map.
int __real_ble_gatts_add_svcs(const struct ble_gatt_svc_def *services);
int __wrap_ble_gatts_add_svcs(const struct ble_gatt_svc_def *services) {
  const struct ble_gatt_svc_def *svc=services;
  if (svc->type==0 || ble_uuid_u16(svc->uuid)!=0x1812)
    return __real_ble_gatts_add_svcs(services);

  static struct ble_gatt_svc_def keyboard[2];
  static struct ble_gatt_chr_def characteristics[12];
  if (svc[1].type!=0) return BLE_HS_EINVAL;

  size_t count=0;
  for (const struct ble_gatt_chr_def *chr=svc->characteristics; chr->uuid; ++chr) {
    if (count+1>=sizeof(characteristics)/sizeof(characteristics[0]))
      return BLE_HS_ENOMEM;
    characteristics[count]=*chr;
    if (ble_uuid_u16(chr->uuid)==BLE_SVC_HID_CHR_UUID16_REPORT_MAP)
      characteristics[count].descriptors=NULL;
    ++count;
  }
  characteristics[count]=(struct ble_gatt_chr_def){0};
  keyboard[0]=*svc;
  keyboard[0].characteristics=characteristics;
  return __real_ble_gatts_add_svcs(keyboard);
}

static int register_keyboard(void) {
  static const char pnp[]={2,0x3a,0x30,0x40,0x40,0x01,0x01};
  struct ble_svc_hid_params hid={
    .proto_mode_present=1,.proto_mode=BLE_SVC_HID_PROTO_MODE_REPORT,
    .kbd_inp_present=1,.kbd_out_present=1,.ctrl_pt=1,.hid_info=0x02000111,
    // The keyboard report uses only HID characteristics. Battery is independent.
    .report_map_len=sizeof(report_map),
    .rpts_len=2,.rpts={{.id=1,.type=BLE_SVC_HID_RPT_TYPE_INPUT,.len=8},
                     {.id=1,.type=BLE_SVC_HID_RPT_TYPE_OUTPUT,.len=1}}
  };
  memcpy(hid.report_map,report_map,sizeof(report_map));
  ble_svc_gap_init();ble_svc_gatt_init();ble_svc_sps_init(0,0);ble_svc_bas_init();ble_svc_dis_init();
  if (ble_svc_dis_pnp_id_set(pnp) || ble_svc_gap_device_appearance_set(0x03c1) ||
      ble_svc_gap_device_name_set(name) || ble_svc_hid_add(hid)) return -1;
  ble_svc_hid_init();
  ble_hs_cfg.gatts_register_cb=register_gatt;
  return 0;
}

static int gap_event(struct ble_gap_event *event, void *arg) {
  (void)arg;
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status==0) {
        peer=event->connect.conn_handle;
        peer_since=now_ms();
        atomic_fetch_add(&connects,1);
        security_requested=false;
        if (pairing_open()) {
          atomic_store(&security_result,ble_gap_security_initiate(peer));
          security_requested=true;
        }
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      atomic_store(&disconnect_reason,event->disconnect.reason);
      fast_adv_until=now_ms()+30000;
      neutral_pending=false;
      peer=NO_PEER; input_subscribed=false; boot_subscribed=false; suspended=false;
      release_due=false; protocol_mode=1;
      clear_chord_queue();
      atomic_store(&displayed_code,0);
      update_ready();
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      atomic_store(&security_result,event->enc_change.status);
      if (peer!=NO_PEER && !event->enc_change.status && secure(peer) &&
          (pairing_open() || trusted_peer())) {
        struct ble_gap_conn_desc d;
        if (pairing_open() && !ble_gap_conn_find(peer,&d)) {
          bonded_identity=d.peer_id_addr;
          bonded_identity_valid=true;
        }
        atomic_store(&paired,1);
        pet_ble_shortcut_close_pair();
        update_ready();
      } else if (peer!=NO_PEER) {
        (void)ble_gap_terminate(peer,BLE_ERR_REM_USER_CONN_TERM);
      }
      break;
    case BLE_GAP_EVENT_SUBSCRIBE:
      if (event->subscribe.attr_handle==input_handle) input_subscribed=event->subscribe.cur_notify;
      if (event->subscribe.attr_handle==boot_handle) boot_subscribed=event->subscribe.cur_notify;
      update_ready();
      break;
    case BLE_GAP_EVENT_PASSKEY_ACTION:
      if (pairing_open() && !atomic_load(&paired) && event->passkey.params.action==BLE_SM_IOACT_DISP) {
        struct ble_sm_io passkey={.action=BLE_SM_IOACT_DISP,.passkey=100000+esp_random()%900000};
        if (!ble_sm_inject_io(event->passkey.conn_handle,&passkey)) {
          atomic_store(&displayed_code,passkey.passkey);
          return 0;
        }
      }
      (void)ble_gap_terminate(event->passkey.conn_handle,BLE_ERR_REM_USER_CONN_TERM);
      break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
      return BLE_GAP_REPEAT_PAIRING_IGNORE;
  }
  return 0;
}

static bool send_report(uint16_t handle, const uint8_t data[8]) {
  struct os_mbuf *m=ble_hs_mbuf_from_flat(data,8);
  if (m && !ble_gatts_notify_custom(peer,handle,m)) return true;
  atomic_fetch_add(&send_error,1);
  return false;
}

static void tick(struct ble_npl_event *event) {
  (void)event;
  if (atomic_exchange(&forget,false)) {
    pet_ble_shortcut_close_pair();
    atomic_store(&ready,0);
    clear_chord_queue();
    if (peer!=NO_PEER) (void)ble_gap_terminate(peer,BLE_ERR_REM_USER_CONN_TERM);
    ble_addr_t bonded;
    int count=0;
    if (!ble_store_util_bonded_peers(&bonded,&count,1) && count==1 &&
        !ble_store_util_delete_peer(&bonded)) {
      atomic_store(&paired,0);
      bonded_identity_valid=false;
    }
    if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
  }
  if (!pet_ble_keyboard_enabled()) {
    atomic_store(&ready,0);
    clear_chord_queue();
    if (ble_gap_adv_active()) (void)ble_gap_adv_stop();
    if (peer!=NO_PEER) {
      if (release_due) (void)send_report(release_handle,(const uint8_t[8]){0});
      release_due=false;
      (void)ble_gap_terminate(peer,BLE_ERR_REM_USER_CONN_TERM);
    }
    atomic_store(&advertising,0);
    ble_npl_callout_reset(&poll,ble_npl_time_ms_to_ticks32(25));
    return;
  }
  const bool open=pairing_open() && !atomic_load(&paired);
  if (peer!=NO_PEER && !open && !secure(peer) && !security_requested &&
      (int32_t)(now_ms()-peer_since)>800) {
    security_requested=true;
    atomic_store(&security_result,ble_gap_security_initiate(peer));
  }
  if (peer!=NO_PEER && !open && (!secure(peer) || !trusted_peer()) &&
      (int32_t)(now_ms()-peer_since)>12000)
    (void)ble_gap_terminate(peer,BLE_ERR_REM_USER_CONN_TERM);
  if (peer==NO_PEER) {
    const bool fast=open || (int32_t)(fast_adv_until-now_ms())>0;
    if (ble_gap_adv_active() && (adv_open!=open || fast_adv!=fast)) (void)ble_gap_adv_stop();
    if (!ble_gap_adv_active()) {
      // Let peers with rotating private addresses reach encryption. Access
      // stays gated by the persisted identity and authenticated bond.
      if (open || bonded_identity_valid) {
        const struct ble_gap_adv_params params={.conn_mode=BLE_GAP_CONN_MODE_UND,
          .disc_mode=BLE_GAP_DISC_MODE_GEN,.itvl_min=fast?48:1600,.itvl_max=fast?48:1600};
        const int rc=ble_gap_adv_start(local_address,NULL,BLE_HS_FOREVER,&params,gap_event,NULL);
        atomic_store(&directed_result,rc ? -rc : 1);
        if (!rc) {adv_open=open;fast_adv=fast;}
      }
    }
  } else if (release_due) {
    if ((int32_t)(now_ms()-release_at)>=0 && send_report(release_handle,(const uint8_t[8]){0})) {
      release_due=false;
      atomic_fetch_add(&sent_release,1);
    }
    if ((int32_t)(now_ms()-release_at)>500)
      (void)ble_gap_terminate(peer,BLE_ERR_REM_USER_CONN_TERM);
  } else {
    uint8_t mode;
    if (read_gatt(protocol_handle,&mode,1) && mode<=1) protocol_mode=mode;
    update_ready();
    if (neutral_pending && atomic_load(&ready) &&
        send_report(protocol_mode ? input_handle : boot_handle,(const uint8_t[8]){0}))
      neutral_pending=false;
    int command = atomic_load(&ready) ? next_chord() : 0;
    if (command && atomic_load(&ready) && secure(peer)) {
      const uint8_t data[8]={(uint8_t)(command >> 8),0,(uint8_t)command};
      release_handle=protocol_mode ? input_handle : boot_handle;
      if (send_report(release_handle,data)) {
        atomic_fetch_add(&sent_press,1);
        release_due=true;
        release_at=now_ms()+KEY_RELEASE_MS;
      }
    }
  }
  atomic_store(&advertising,ble_gap_adv_active());
  ble_npl_callout_reset(&poll,ble_npl_time_ms_to_ticks32(25));
}

static void reset(int reason) {
  (void)reason;
  peer=NO_PEER;
  input_subscribed=false; boot_subscribed=false; suspended=false;
  release_due=false; protocol_mode=1;
  clear_chord_queue();
  atomic_store(&ready,0);
  atomic_store(&available,0);
  ble_npl_callout_stop(&poll);
}

static void synced(void) {
  static ble_uuid16_t uuid=BLE_UUID16_INIT(0x1812);
  struct ble_hs_adv_fields fields={
    .flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP,
    .name=(const uint8_t*)name,.name_len=sizeof(name)-1,.name_is_complete=1,
    .appearance=0x03c1,.appearance_is_present=1,
    .uuids16=&uuid,.num_uuids16=1,.uuids16_is_complete=1
  };
  int count=0;
  if (ble_hs_id_infer_auto(0,&local_address) || ble_gap_adv_set_fields(&fields)) {
    atomic_store(&available,-1);
    return;
  }
  uint8_t address[6];
  uint32_t digest=2166136261u;
  if (!ble_hs_id_copy_addr(local_address,address,NULL)) {
    for (unsigned i=0;i<6;++i) digest=(digest ^ address[i])*16777619u;
    atomic_store(&address_hash,digest);
  }
  fast_adv_until=now_ms()+30000;
  bonded_identity_valid=!ble_store_util_bonded_peers(&bonded_identity,&count,1) && count==1;
  atomic_store(&paired,bonded_identity_valid);
  atomic_store(&radio_error,0);
  atomic_store(&available,1);
  ble_npl_callout_reset(&poll,0);
}

static void host(void *arg) {
  (void)arg;
  nimble_port_run();
  nimble_port_freertos_deinit();
}

void ble_budget_probe(void) {
  int rc=esp_hosted_bt_controller_init();
  if (rc==ESP_OK) rc=esp_hosted_bt_controller_enable();
  // Some factory C6 firmware enables the controller during boot and predates
  // the explicit Hosted init RPC. A successful NimBLE sync proves that path.
  atomic_store(&radio_error,rc);
  const int host_result=nimble_port_init();
  if (host_result!=ESP_OK) { atomic_store(&radio_error,host_result); atomic_store(&available,-1); return; }
  if (register_keyboard()) {
    atomic_store(&available,-1); return;
  }
  ble_hs_cfg.sync_cb=synced;
  ble_hs_cfg.reset_cb=reset;
  ble_hs_cfg.sm_bonding=1; ble_hs_cfg.sm_mitm=1; ble_hs_cfg.sm_sc=1;
  ble_hs_cfg.sm_io_cap=BLE_SM_IO_CAP_DISP_ONLY;
  ble_hs_cfg.sm_our_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.sm_their_key_dist=BLE_SM_PAIR_KEY_DIST_ENC|BLE_SM_PAIR_KEY_DIST_ID;
  ble_hs_cfg.store_status_cb=ble_store_util_status_rr;
  ble_store_config_init();
  ble_npl_callout_init(&poll,nimble_port_get_dflt_eventq(),tick,NULL);
  nimble_port_freertos_init(host);
}
