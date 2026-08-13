#ifndef INC_CHARGE_DIAGNOSTIC_H_
#define INC_CHARGE_DIAGNOSTIC_H_

/* Factory-derived charging state machine.  Every terminal/error path releases
 * active-low PC10 high before drawing the result screen. */
void charge_mode_run(void);

#endif /* INC_CHARGE_DIAGNOSTIC_H_ */
