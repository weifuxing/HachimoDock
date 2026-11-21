#pragma once

#include <stdbool.h>

/* One boot-time probe for the onboard ESP-Hosted SDIO function. */
bool pet_p4_probe_wifi6_board(void);

unsigned pet_p4_board_probe_phase(void);
