#pragma once
#include <stdbool.h>
#include <stdint.h>


// Persistent slot indices. The stick uses short/long press and four directions.
#define PET_BLE_GESTURE_COUNT 20
#if PET_P4_SYSTEM_WIFI
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#if CONFIG_BT_NIMBLE_ENABLED
void ble_budget_probe(void);
void pet_ble_shortcut_open_pair(void);
void pet_ble_shortcut_close_pair(void);
void pet_ble_shortcut_forget(void);
int pet_ble_shortcut_available(void);
int pet_ble_shortcut_paired(void);
bool pet_ble_shortcut_pairing(void);
unsigned pet_ble_shortcut_code(void);
bool pet_ble_shortcut_restore_requested(void);
int pet_ble_shortcut_radio_error(void);
unsigned pet_ble_shortcut_report_counter(unsigned which);
unsigned pet_ble_shortcut_advertising(void);
int pet_ble_shortcut_directed_result(void);
unsigned pet_ble_shortcut_link_counter(unsigned which);
void pet_ble_keyboard_load(void);
bool pet_ble_keyboard_enabled(void);
bool pet_ble_keyboard_connected(void);
bool pet_ble_keyboard_set_mode(bool enabled);
bool pet_ble_keyboard_set_chord(unsigned slot, uint16_t chord);
uint16_t pet_ble_keyboard_chord(unsigned slot);
bool pet_ble_keyboard_send(uint16_t chord);
#else
static inline void ble_budget_probe(void) { }
static inline void pet_ble_shortcut_open_pair(void) { }
static inline void pet_ble_shortcut_close_pair(void) { }
static inline void pet_ble_shortcut_forget(void) { }
static inline int pet_ble_shortcut_available(void) { return 0; }
static inline int pet_ble_shortcut_paired(void) { return 0; }
static inline bool pet_ble_shortcut_pairing(void) { return false; }
static inline unsigned pet_ble_shortcut_code(void) { return 0; }
static inline bool pet_ble_shortcut_restore_requested(void) { return false; }
static inline int pet_ble_shortcut_radio_error(void) { return 0; }
static inline unsigned pet_ble_shortcut_report_counter(unsigned which) { (void)which; return 0; }
static inline unsigned pet_ble_shortcut_advertising(void) { return 0; }
static inline int pet_ble_shortcut_directed_result(void) { return 0; }
static inline unsigned pet_ble_shortcut_link_counter(unsigned which) { (void)which; return 0; }
static inline void pet_ble_keyboard_load(void) { }
static inline bool pet_ble_keyboard_enabled(void) { return false; }
static inline bool pet_ble_keyboard_connected(void) { return false; }
static inline bool pet_ble_keyboard_set_mode(bool enabled) { (void)enabled; return false; }
static inline bool pet_ble_keyboard_set_chord(unsigned slot, uint16_t chord) { (void)slot; (void)chord; return false; }
static inline uint16_t pet_ble_keyboard_chord(unsigned slot) { (void)slot; return 0; }
static inline bool pet_ble_keyboard_send(uint16_t chord) { (void)chord; return false; }
#endif
#endif
