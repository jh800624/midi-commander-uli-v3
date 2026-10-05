/* Read-only battery monitor derived from the stock firmware measurement path.
 * It never controls the external charger. */
#ifndef INC_BATTERY_H_
#define INC_BATTERY_H_

#include <stdint.h>

void battery_init(void);
void battery_task(void);
uint8_t battery_is_valid(void);
uint16_t battery_get_centivolts(void);
uint8_t battery_get_percent(void);

#endif /* INC_BATTERY_H_ */
