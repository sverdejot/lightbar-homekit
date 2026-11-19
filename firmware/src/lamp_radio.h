// SPDX-License-Identifier: MIT
/*
 * Xiaomi Mi Computer Monitor Light Bar (MJGJD01YL) proprietary 2.4 GHz TX,
 * sent from the nRF52840 radio inside MPSL timeslots so it coexists with
 * Thread and Bluetooth.
 *
 * Protocol: https://github.com/lamperez/xiaomi-lightbar-nrf24
 */
#ifndef LAMP_RADIO_H
#define LAMP_RADIO_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LAMP_CMD_TOGGLE 0x01
#define LAMP_CMD_COLOR  0x02 /* opt: signed step, + cooler / - warmer */
#define LAMP_CMD_BRIGHT 0x04 /* opt: signed step, + brighter / - dimmer */
#define LAMP_CMD_RESET  0x06

/* Queue one lamp command (non-blocking). Returns 0 or -ENOMEM if queue full. */
int lamp_send(uint8_t cmd, uint8_t opt);

void lamp_set_remote_id(uint32_t id);
uint32_t lamp_get_remote_id(void);

/* Number of timeslot groups per command (each group = all channels x 2 reps). */
void lamp_set_groups(uint8_t groups);
uint8_t lamp_get_groups(void);

#ifdef __cplusplus
}
#endif

#endif /* LAMP_RADIO_H */
