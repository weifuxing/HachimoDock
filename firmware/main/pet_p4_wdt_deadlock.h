#ifndef PET_P4_WDT_DEADLOCK_H
#define PET_P4_WDT_DEADLOCK_H

typedef enum {
  PET_DEADLOCK_MAIN,
  PET_DEADLOCK_USB_RX,
  PET_DEADLOCK_UART_RX,
  PET_DEADLOCK_INPUT,
  PET_DEADLOCK_PLAYBACK,
  PET_DEADLOCK_TASK_COUNT
} pet_p4_deadlock_task_t;

void pet_p4_wdt_deadlock_start(void);
void pet_p4_wdt_deadlock_feed(pet_p4_deadlock_task_t task);

#endif
