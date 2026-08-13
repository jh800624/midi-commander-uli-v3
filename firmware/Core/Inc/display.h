/*
 * display.h
 *
 *  Created on: 8 Jul 2021
 *      Author: D Harvie
 */

#ifndef INC_DISPLAY_H_
#define INC_DISPLAY_H_

void display_init(void);
void display_task(void);
void display_setConfigName(void);
void display_setProfileName(uint8_t profile);
void display_performance_key(uint8_t key, uint8_t mode, uint8_t number,
                             uint8_t value, uint8_t profile);
void display_performance_expression(uint8_t exp1, uint8_t exp2);
void display_performance_battery(uint8_t percent, uint16_t centivolts,
                                 uint8_t valid);
void display_show_settings(uint8_t profile, uint8_t selected_row);

#endif /* INC_DISPLAY_H_ */
