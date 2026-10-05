/*
 * Local tap-tempo MIDI Clock generator.
 *
 * The engine begins clocking after two valid taps and sends F8 at 24 PPQN to
 * both USB MIDI and DIN MIDI OUT. It deliberately does not emit Start/Stop:
 * a tempo controller must never start transport on a connected looper/DAW
 * merely because its tempo was tapped.
 */
#ifndef INC_TEMPO_CLOCK_H_
#define INC_TEMPO_CLOCK_H_

#include <stdint.h>

void tempoClock_init(void);
void tempoClock_tap(void);
void tempoClock_task(void);
uint16_t tempoClock_bpm(void);
uint8_t tempoClock_is_running(void);

#endif /* INC_TEMPO_CLOCK_H_ */
