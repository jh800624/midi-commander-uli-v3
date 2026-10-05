/*
 * switch_router.h
 *
 *  Created on: 8 Jul 2021
 *      Author: D Harvie
 */

#ifndef INC_SWITCH_ROUTER_H_
#define INC_SWITCH_ROUTER_H_

#include <stdint.h>

/* V3 physical layout, counted top-to-bottom and left-to-right:
 * 1 2 3 4 5
 * 6 7 8 9 0 */
typedef enum {
  V3_SW_1 = 0,
  V3_SW_2,
  V3_SW_3,
  V3_SW_4,
  V3_SW_5,
  V3_SW_6,
  V3_SW_7,
  V3_SW_8,
  V3_SW_9,
  V3_SW_0
} v3_switch_index_t;

extern uint8_t switch_current_page;
void handle_switches(void);
void switch_router_sync_inputs(void);
void switch_router_set_settings_mode(uint8_t enabled);
uint8_t switch_router_is_settings_mode(void);
uint8_t switch_router_is_suspended(void);
void sw_led_init(void);
void update_leds_on_bank_change(void);
void set_all_leds(uint8_t state);
void setIsSuspended(uint8_t suspended);

#endif /* INC_SWITCH_ROUTER_H_ */
