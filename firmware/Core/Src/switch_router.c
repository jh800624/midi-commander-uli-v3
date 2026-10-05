/*
 * switch_router.c
 *
 *  Created on: 8 Jul 2021
 *      Author: D Harvie
 */
#include "main.h"
#include "switch_router.h"
#include "midi_defines.h"
#include "midi_cmds.h"
#include "flash_midi_settings.h"
#include "display.h"
#include "usbd_hid_custom.h"
#include "tempo_clock.h"

void update_leds_on_bank_change(void);

/*
 * Creating some constant arrays for the switches that can be scanned and handled
 * in a loop to simplify code and reduce duplication;
 */
typedef struct {
	GPIO_TypeDef *sw_gpio_port;
	uint16_t sw_gpio_pin;
	volatile uint16_t *pSwChangeState;
	GPIO_TypeDef *led_gpio_port;
	uint16_t led_gpio_pin;
	uint8_t switch_toggle_state; // Each bit will be a flag for pages 0-7
	uint8_t led_cmd_toggle;
} sw_t;

typedef struct {
	uint32_t systick_timout;
	uint8_t *pRomCmd;
} delayed_cmd_t;

#define SW_PORTA_MASK (SW_1_Pin | SW_2_Pin | SW_E_Pin | SW_D_Pin | SW_C_Pin)
#define SW_PORTB_MASK (SW_A_Pin | SW_3_Pin | SW_4_Pin | SW_5_Pin)
#define SW_PORTC_MASK (SW_B_Pin)

uint16_t port_A_previous_state = SW_PORTA_MASK; // All pins will be high un-pressed
volatile uint16_t port_A_switches_changed = 0;
uint16_t port_B_previous_state = SW_PORTB_MASK; // All pins will be high un-pressed
volatile uint16_t port_B_switches_changed = 0;
uint16_t port_C_previous_state = SW_PORTC_MASK; // All pins will be high un-pressed
volatile uint16_t port_C_switches_changed = 0;

volatile uint8_t debounce_counter = 0;
// Flag to indicate if USB is suspended. If so, we shouldn't update LEDs or read switches in the main loop
// because the main loop might keep running even if USB is suspended (if low_power_enable is 0)
static volatile uint8_t is_app_suspended = 0;
static uint8_t settings_mode = 0;
static uint8_t settings_row = 0;
static uint16_t active_press_mask = 0U;

extern uint8_t f_sys_config_complete;

/* Compatibility name retained while the display/settings code is migrated.
 * In v3 it is a CUS profile index only: 0 = CUS-1, 1 = CUS-2. */
uint8_t switch_current_page = 0;
sw_t a_sw_obj[] = {
		/* V3 layout: 1 2 3 4 5 / 6 7 8 9 0.
		 * Factory labels: 1 2 3 4/CHG / A B C D/SET. */
		{ .sw_gpio_port = SW_1_GPIO_Port, .sw_gpio_pin = SW_1_Pin, .pSwChangeState = &port_A_switches_changed, .led_gpio_port = LED_1_GPIO_Port, .led_gpio_pin = LED_1_Pin, .switch_toggle_state = 0},
		{ .sw_gpio_port = SW_2_GPIO_Port, .sw_gpio_pin = SW_2_Pin, .pSwChangeState = &port_A_switches_changed, .led_gpio_port = LED_2_GPIO_Port, .led_gpio_pin = LED_2_Pin, .switch_toggle_state = 0},
		{ .sw_gpio_port = SW_3_GPIO_Port, .sw_gpio_pin = SW_3_Pin, .pSwChangeState = &port_B_switches_changed, .led_gpio_port = LED_3_GPIO_Port, .led_gpio_pin = LED_3_Pin, .switch_toggle_state = 0},
		{ .sw_gpio_port = SW_4_GPIO_Port, .sw_gpio_pin = SW_4_Pin, .pSwChangeState = &port_B_switches_changed, .led_gpio_port = LED_4_GPIO_Port, .led_gpio_pin = LED_4_Pin, .switch_toggle_state = 0},
		{ .sw_gpio_port = SW_5_GPIO_Port, .sw_gpio_pin = SW_5_Pin, .pSwChangeState = &port_B_switches_changed, .led_gpio_port = LED_5_GPIO_Port, .led_gpio_pin = LED_5_Pin, .switch_toggle_state = 0},
		{ .sw_gpio_port = SW_A_GPIO_Port, .sw_gpio_pin = SW_A_Pin, .pSwChangeState = &port_B_switches_changed, .led_gpio_port = LED_A_GPIO_Port, .led_gpio_pin = LED_A_Pin, .switch_toggle_state = 0},
		{ .sw_gpio_port = SW_B_GPIO_Port, .sw_gpio_pin = SW_B_Pin, .pSwChangeState = &port_C_switches_changed, .led_gpio_port = LED_B_GPIO_Port, .led_gpio_pin = LED_B_Pin, .switch_toggle_state = 0},
		{ .sw_gpio_port = SW_C_GPIO_Port, .sw_gpio_pin = SW_C_Pin, .pSwChangeState = &port_A_switches_changed, .led_gpio_port = LED_C_GPIO_Port, .led_gpio_pin = LED_C_Pin, .switch_toggle_state = 0},
		{ .sw_gpio_port = SW_D_GPIO_Port, .sw_gpio_pin = SW_D_Pin, .pSwChangeState = &port_A_switches_changed, .led_gpio_port = LED_D_GPIO_Port, .led_gpio_pin = LED_D_Pin, .switch_toggle_state = 0},
		{ .sw_gpio_port = SW_E_GPIO_Port, .sw_gpio_pin = SW_E_Pin, .pSwChangeState = &port_A_switches_changed, .led_gpio_port = LED_E_GPIO_Port, .led_gpio_pin = LED_E_Pin, .switch_toggle_state = 0}
};

#define MAX_DELAYED_CMDS (32)

delayed_cmd_t delayed_cmds[MAX_DELAYED_CMDS];


/*
 * This scans the switch ports each ms for changes in the systick interrupt context.
 * We can't use port interrupts, as some of the pins are on non-interrupt pins.
 *
 * Switch changes are then handled in the main loop.
 */
void sw_scan(void){

	if(!f_sys_config_complete){
		return;
	}

	if(debounce_counter){
		debounce_counter--;
		return;
	}

	/* PORTA input pins */
	uint16_t current_port_A = GPIOA->IDR & SW_PORTA_MASK;
	port_A_switches_changed |= current_port_A  ^ port_A_previous_state;
	port_A_previous_state = current_port_A;

	/* PORTB input pins */
	uint16_t current_port_B = GPIOB->IDR & SW_PORTB_MASK;
	port_B_switches_changed |= current_port_B  ^ port_B_previous_state;
	port_B_previous_state = current_port_B;

	/* PORTC input pins */
	uint16_t current_port_C = GPIOC->IDR & SW_PORTC_MASK;
	port_C_switches_changed |= current_port_C  ^ port_C_previous_state;
	port_C_previous_state = current_port_C;

	if(port_A_switches_changed | port_B_switches_changed | port_C_switches_changed){
		debounce_counter = 10; // 10ms debounce delay
		return;
	}

}

static inline uint8_t get_sw_toggle_state(sw_t *sw){
	return sw->switch_toggle_state & (1 << switch_current_page);
}

static inline void toggle_sw_state(sw_t *sw){
	sw->switch_toggle_state ^= (1 << switch_current_page);
}


static uint8_t key_press_count[256] = {0};
static uint8_t mod_press_count[8] = {0};

static void update_keyboard_state(uint8_t mod_byte, uint8_t key_code, uint8_t is_pressed) {
    // Modifiers
    for(int i=0; i<8; i++) {
        if((mod_byte >> i) & 1) {
            if(is_pressed) {
                if(mod_press_count[i] < 255) mod_press_count[i]++;
            } else {
                if(mod_press_count[i] > 0) mod_press_count[i]--;
            }
        }
    }
    
    // Key Code
    if(key_code != 0) {
       if(is_pressed) {
            if(key_press_count[key_code] < 255) key_press_count[key_code]++;
       } else {
            if(key_press_count[key_code] > 0) key_press_count[key_code]--;
       }
    }
    
    // Build Report
    uint8_t report[8] = {0};
    
    // Mod Byte
    for(int i=0; i<8; i++) {
        if(mod_press_count[i] > 0) report[0] |= (1<<i);
    }
    
    // Keys (max 6)
    int k = 0;
    for(int i=0; i<256 && k < 6; i++) {
        if(key_press_count[i] > 0) {
            report[2+k] = i;
            k++;
        }
    }
    
    HID_SendReport_FS(report, 8);
}

uint8_t get_button_led_mode(uint8_t sw){ (void)sw; return 0; }

// Helper to determine LED state based on Mode and Press state
// pressed: 1 if button is held down (physically)
// mode: 0=Normal, 1=Reverse, 2=AlwaysOn
uint8_t calculate_led_state(uint8_t pressed, uint8_t mode){
	uint32_t tick = HAL_GetTick();
	uint8_t blink_on = ((tick % 200) < 100) ? 1 : 0; // 50% duty, 200ms period
	
	if(mode == 2){ // AlwaysOn (Blink on press)
		if(pressed) return blink_on; // Blink when pressed
		else return 1; // Always ON when released
	} else if (mode == 1){ // Reverse (Inverted)
		return !pressed;
	} else { // Normal
		return pressed;
	}
}

void sw_led_init(void){
	// In v3 only a CC TAG=ON setting toggles a footswitch LED/state.
	for(int page=0; page<V3_PROFILE_COUNT; page++){
		for(int sw=0; sw<V3_SWITCHES_PER_PROFILE; sw++){
			const v3_key_setting_t *key = &v3_settings_profile(page)->key[sw];
			if (key->mode == V3_KEY_CC && key->toggle) a_sw_obj[sw].led_cmd_toggle |= (1 << page);
			else a_sw_obj[sw].led_cmd_toggle &= ~(1 << page);
		}
	}

	// Init all delayed cmds to off
	for(int i=0; i<MAX_DELAYED_CMDS; i++){
		delayed_cmds[i].systick_timout = UINT32_MAX;
	}

	// Init all the LEDs based on current page (0) and settings
	update_leds_on_bank_change();
}


/*
 * Functions for the command duration, i.e. delay until switching a cmd/note/pb off.
 */
int get_available_delayed_cmd_slot(void){
	for(int i=0; i<MAX_DELAYED_CMDS; i++){
		if(delayed_cmds[i].systick_timout == UINT32_MAX){
			return i;
		}
	}

	return -1;
}

void handle_delayed_cmds(void){
	for(int i=0; i<MAX_DELAYED_CMDS; i++){
		if(delayed_cmds[i].systick_timout < HAL_GetTick()){
			uint8_t* pRom = delayed_cmds[i].pRomCmd;
			switch(*pRom & 0xF0){
			case CMD_PB_NIBBLE:
				midiCmd_send_pb_command_from_rom(pRom, MIDI_CONTROL_OFF);
			case CMD_NOTE_NIBBLE:
				midiCmd_send_note_command_from_rom(pRom, MIDI_CONTROL_OFF);
				break;
			case CMD_KEY_NIBBLE:
				update_keyboard_state(pRom[1], pRom[2], 0); // Release
				break;
			default:
				break;
			}
			delayed_cmds[i].systick_timout = UINT32_MAX;
		}
	}
}

// Sets the cmd point to switch off and the delay timeout into the delayed cmds table
void set_cmd_duration_delay(uint8_t *pRom){
	int slot = get_available_delayed_cmd_slot();
	if(slot >= 0){
		delayed_cmds[slot].pRomCmd = pRom;
		delayed_cmds[slot].systick_timout = HAL_GetTick() + midiCmd_get_delay(pRom);
	}
}

void handle_cmd_sw_down(uint8_t *pRom, uint8_t toggleState){
	/*
	 * Assume success by default since this was the behavior before adding this status.
	 */
	int8_t status = 0;

	switch(*pRom & 0xF0){
	case CMD_PC_NIBBLE:
		status = midiCmd_send_pc_command_from_rom(pRom);
		break;
	case CMD_CC_NIBBLE:
		if(midiCmd_get_cmd_toggle(pRom)){
			status = midiCmd_send_cc_command_from_rom(pRom, toggleState);
		} else {
			// Not toggling, so set command and either set a duration or not
			status = midiCmd_send_cc_command_from_rom(pRom, MIDI_CONTROL_ON);
		}
		break;
	case CMD_PB_NIBBLE:
		if(midiCmd_get_cmd_toggle(pRom)){
			status = midiCmd_send_pb_command_from_rom(pRom, toggleState);
		} else {
			// Not toggling, so set command and either set a duration or not
			status = midiCmd_send_pb_command_from_rom(pRom, MIDI_CONTROL_ON);
			if(midiCmd_get_delay(pRom) != 0){
				set_cmd_duration_delay(pRom);
			}
		}
		break;
	case CMD_NOTE_NIBBLE:
		if(midiCmd_get_cmd_toggle(pRom)){
			status = midiCmd_send_note_command_from_rom(pRom, toggleState);
		} else {
			// Not toggling, so set command and either set a duration or not
			status = midiCmd_send_note_command_from_rom(pRom, MIDI_CONTROL_ON);
			if(midiCmd_get_delay(pRom) != 0){
				set_cmd_duration_delay(pRom);
			}
		}
		break;
	case CMD_KEY_NIBBLE:
    {
        // Key Mode Logic
        // Mode is stored in the lower nibble of Byte 0
        uint8_t key_mode = pRom[0] & 0x0F;
        uint32_t delay_val = midiCmd_get_delay(pRom);

		if(midiCmd_get_cmd_toggle(pRom)){
			// Toggle Logic (Valid for Normal Mode 0 only mostly?)
			update_keyboard_state(pRom[1], pRom[2], toggleState);
		} else {
            // Momentary / Manual Logic based on Mode
            if (key_mode == 1) { // Down Only
                if (delay_val > 0) HAL_Delay(delay_val); // Blocking Pre-Delay
                update_keyboard_state(pRom[1], pRom[2], 1); // Press
            } 
            else if (key_mode == 2) { // Up Only
                if (delay_val > 0) HAL_Delay(delay_val); // Blocking Pre-Delay
                update_keyboard_state(pRom[1], pRom[2], 0); // Release
            }
            else { // Mode 0: Normal Momentary (Pulse)
                // Immediate Press
			    update_keyboard_state(pRom[1], pRom[2], 1); 
                // Auto Release after Duration
			    if(delay_val != 0){
				    set_cmd_duration_delay(pRom);
			    }
            }
		}
    }
		break;
	case CMD_START_NIBBLE:
		status = midiCmd_send_start_command();
		break;
	case CMD_STOP_NIBBLE:
		status = midiCmd_send_stop_command();
		break;
	case CMD_TAP_TEMPO_NIBBLE:
		tempoClock_tap();
		break;
	default:
		break;
	}

	if (status == ERROR_BUFFERS_FULL) {
		Error("Buffers full");
 	}
}

void handle_cmd_sw_up(uint8_t *pRom, uint8_t toggleState){
	/*
	 * Assume success by default since this was the behavior before adding this status.
	 */
	int8_t status = 0;

	switch(*pRom & 0xF0){
	case CMD_PC_NIBBLE:
		break;
	case CMD_CC_NIBBLE:
		if(!midiCmd_get_cmd_toggle(pRom)){
			status = midiCmd_send_cc_command_from_rom(pRom, MIDI_CONTROL_OFF);
		}
		break;
	case CMD_PB_NIBBLE:
		if(!midiCmd_get_cmd_toggle(pRom)) {
			if(midiCmd_get_delay(pRom) == 0) {
				// No cmd duration delay, so immediately release
				status = midiCmd_send_pb_command_from_rom(pRom, MIDI_CONTROL_OFF);
			}
		}
		break;
	case CMD_NOTE_NIBBLE:
		if(!midiCmd_get_cmd_toggle(pRom)) {
			if(midiCmd_get_delay(pRom) == 0) {
				// No cmd duration delay, so immediately release
				status = midiCmd_send_note_command_from_rom(pRom, MIDI_CONTROL_OFF);
			}
		}
		break;
	case CMD_KEY_NIBBLE:
		if(!midiCmd_get_cmd_toggle(pRom)) {
			if(midiCmd_get_delay(pRom) == 0) {
				// No cmd duration delay, so immediately release
				update_keyboard_state(pRom[1], pRom[2], 0); // Release
			}
		}
		break;
	case CMD_START_NIBBLE:
		break;
	case CMD_STOP_NIBBLE:
		break;
	case CMD_TAP_TEMPO_NIBBLE:
		break;
	default:
		break;
	}

	if (status == ERROR_BUFFERS_FULL) {
		Error("Buffers full");
 	}
}

void set_led(uint8_t sw_no, uint8_t state){
	GPIO_PinState pinState = (state) ? GPIO_PIN_RESET : GPIO_PIN_SET;
	HAL_GPIO_WritePin(a_sw_obj[sw_no].led_gpio_port, a_sw_obj[sw_no].led_gpio_pin, pinState);

}

void update_leds_on_bank_change(void){
	for(int i=0; i<V3_SWITCHES_PER_PROFILE; i++){
		if(a_sw_obj[i].led_cmd_toggle & (1<<switch_current_page)){
			uint8_t mode = get_button_led_mode(i);
			uint8_t active = get_sw_toggle_state(&a_sw_obj[i]);
			uint8_t state = calculate_led_state(active, mode);
			set_led(i, state ? SET : RESET);
		} else {
			// Update based on mode (assuming released state)
			uint8_t mode = get_button_led_mode(i);
			// For update, we assume Not Pressed. 
			// If AlwaysOn -> ON. If Reverse -> ON. Normal -> OFF.
			uint8_t state = calculate_led_state(0, mode);
			set_led(i, state ? SET : RESET);
		}
	}
}

void switch_router_sync_inputs(void)
{
	/* Boot-selection switches may still be held when scanning is enabled.  Use
	 * the current levels as the baseline so boot gestures do not emit MIDI. */
	port_A_previous_state = GPIOA->IDR & SW_PORTA_MASK;
	port_B_previous_state = GPIOB->IDR & SW_PORTB_MASK;
	port_C_previous_state = GPIOC->IDR & SW_PORTC_MASK;
	port_A_switches_changed = 0U;
	port_B_switches_changed = 0U;
	port_C_switches_changed = 0U;
	active_press_mask = 0U;
	debounce_counter = 0U;
}

static void handle_v3_key_down(uint8_t sw, uint8_t toggle_state)
{
	const v3_key_setting_t *key = &v3_settings_profile(switch_current_page)->key[sw];
	int8_t status = 0;
	uint8_t display_number = key->number;
	uint8_t display_value = 0U;
	if (key->mode == V3_KEY_CC) {
		display_value = key->toggle ? (toggle_state ? 127U : 0U) : 127U;
		status = midiCmd_send_cc(0, key->number, display_value);
	} else if (key->mode == V3_KEY_PC) {
		status = midiCmd_send_pc(0, key->number);
	} else if (key->mode == V3_KEY_MIDI_CLOCK) {
		tempoClock_tap();
		display_number = (uint8_t)tempoClock_bpm();
		display_value = tempoClock_is_running();
	}
	display_performance_key(sw == V3_SW_0 ? 0U : sw + 1U, key->mode,
	                        display_number, display_value,
	                        switch_current_page);
	/* A transiently full DIN queue may drop this event, but must never turn a
	 * busy MIDI burst into a device-wide fatal error. */
	(void)status;
}

static void handle_settings_press(uint8_t sw)
{
	if (sw == V3_SW_2) { switch_current_page = 0; settings_row = 0; }
	else if (sw == V3_SW_3) { switch_current_page = 1; settings_row = 0; }
	else if (sw == V3_SW_5 && settings_row > 0) settings_row--;
	else if (sw == V3_SW_0 && settings_row < 31) settings_row++;
	else if (sw == V3_SW_7) v3_settings_change(switch_current_page, settings_row, -1);
	else if (sw == V3_SW_8) v3_settings_change(switch_current_page, settings_row, 1);
	display_show_settings(switch_current_page, settings_row);
}

void switch_router_set_settings_mode(uint8_t enabled)
{
	settings_mode = enabled;
	settings_row = 0;
	if (enabled) display_show_settings(switch_current_page, settings_row);
}

uint8_t switch_router_is_settings_mode(void)
{
	return settings_mode;
}

void handle_switches(void){
	if(is_app_suspended) return;

	// The Command switches
	for(int i=0; i<V3_SWITCHES_PER_PROFILE; i++){
		if(*a_sw_obj[i].pSwChangeState & a_sw_obj[i].sw_gpio_pin){
				*a_sw_obj[i].pSwChangeState &= ~a_sw_obj[i].sw_gpio_pin;

				if(!HAL_GPIO_ReadPin(a_sw_obj[i].sw_gpio_port, a_sw_obj[i].sw_gpio_pin)){
					if (settings_mode) { handle_settings_press(i); continue; }
					// Switch Down
					active_press_mask |= (uint16_t)(1U << i);
					toggle_sw_state(&a_sw_obj[i]);

					// Either toggle the LED, or set it if not toggling
					if(a_sw_obj[i].led_cmd_toggle & (1<<switch_current_page)){
						uint8_t mode = get_button_led_mode(i);
						uint8_t active = get_sw_toggle_state(&a_sw_obj[i]);
						uint8_t state = calculate_led_state(active, mode);
						set_led(i, state ? SET : RESET);
					} else {
						// Momentary Logic
						uint8_t mode = get_button_led_mode(i);
						uint8_t state = calculate_led_state(1, mode); // Pressed = 1
						set_led(i, state ? SET : RESET);
					}

					handle_v3_key_down(i, get_sw_toggle_state(&a_sw_obj[i]));
				}else {
					// Switch up
					uint8_t had_press = (active_press_mask & (uint16_t)(1U << i)) != 0U;
					active_press_mask &= (uint16_t)~(1U << i);
					// Clear the LED if it's not toggling.
					if(!(a_sw_obj[i].led_cmd_toggle & (1<<switch_current_page))){
						// Momentary Logic
						uint8_t mode = get_button_led_mode(i);
						uint8_t state = calculate_led_state(0, mode); // Pressed = 0
						set_led(i, state ? SET : RESET);
					}

					/* TAG OFF is a one-shot CC: press sends 127 and release sends
					 * no MIDI. TAG ON already alternates 127/0 on successive presses. */
					(void)had_press;

				}
			}

	}

	handle_delayed_cmds();
	
	// Continuous Blink Update Loop for AlwaysOn Buttons
	for(int i=0; i<V3_SWITCHES_PER_PROFILE; i++){
		uint8_t mode = get_button_led_mode(i);
		if(mode == 2){ // AlwaysOn (Blink)
			uint8_t is_active = 0;
			if(a_sw_obj[i].led_cmd_toggle & (1<<switch_current_page)){
				// Toggle Mode: Active if toggle state is ON
				is_active = get_sw_toggle_state(&a_sw_obj[i]);
			} else {
				// Momentary Mode: Active if physically pressed
				if(!HAL_GPIO_ReadPin(a_sw_obj[i].sw_gpio_port, a_sw_obj[i].sw_gpio_pin)){
					is_active = 1;
				}
			}
			uint8_t state = calculate_led_state(is_active, mode);
			set_led(i, state ? SET : RESET);
		}
	}
	
}


void set_all_leds(uint8_t state){
	for(int i=0; i<V3_SWITCHES_PER_PROFILE; i++){
		set_led(i, state);
	}
}

void setIsSuspended(uint8_t suspended){
	is_app_suspended = suspended;
	if(suspended){
		set_all_leds(0);
	}
}

uint8_t switch_router_is_suspended(void)
{
	return is_app_suspended;
}
