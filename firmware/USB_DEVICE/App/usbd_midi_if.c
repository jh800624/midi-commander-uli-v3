/*
 *
 * EEPROM Routines taken in part form Github - nimaltd/ee24
 */

#include "usbd_midi_if.h"
#include "stm32f1xx_hal.h"
#include "midi_defines.h"
#include "flash_midi_settings.h"
#include "midi_cmds.h"
#include <string.h>

extern I2C_HandleTypeDef hi2c1;


#define SYSEX_MAX_LENGTH 64
uint8_t sysex_rx_buffer[SYSEX_MAX_LENGTH];
uint8_t sysex_rx_counter = 0;
uint8_t sysex_tx_assembly_buffer[SYSEX_MAX_LENGTH+32];

uint8_t midi_msg_tx_buffer[SYSEX_MAX_LENGTH];

typedef struct {
	uint8_t start;
	uint8_t manuf_id;
	uint8_t msg_cmd;
	uint8_t start_parameters;

} MIDI_sysex_head_TypeDef;

USBD_MIDI_ItfTypeDef USBD_Interface_fops_FS =
{
  MIDI_DataRx,
  MIDI_DataTx
};

void abort_sysex_message(void){
	sysex_rx_counter = 0;
}

static uint8_t append_sysex_bytes(const uint8_t *data, uint8_t count)
{
	if (count > (uint8_t)(SYSEX_MAX_LENGTH - sysex_rx_counter)) {
		abort_sysex_message();
		return 0U;
	}
	memcpy(sysex_rx_buffer + sysex_rx_counter, data, count);
	sysex_rx_counter += count;
	return 1U;
}

void sysex_send_message(uint8_t* buffer, uint8_t length){
	uint8_t *buff_ptr = buffer;
	uint8_t *assembly_ptr = sysex_tx_assembly_buffer;

	while(buff_ptr < length + buffer){
		uint8_t data_to_go = length + buffer - buff_ptr;

		if(data_to_go > 3){
			assembly_ptr[0] = CIN_SYSEX_STARTS_OR_CONTINUES;
			memcpy(assembly_ptr+1, buff_ptr, 3);
			buff_ptr += 3;
			assembly_ptr += 4;
		} else if (data_to_go == 3) {
			assembly_ptr[0] = CIN_SYSEX_ENDS_WITH_FOLLOWING_THREE_BYTES;
			memcpy(assembly_ptr+1, buff_ptr, 3);
			buff_ptr += 3;
			assembly_ptr += 4;
		} else if (data_to_go == 2) {
			assembly_ptr[0] = CIN_SYSEX_ENDS_WITH_FOLLOWING_TWO_BYTES;
			memcpy(assembly_ptr+1, buff_ptr, 2);
			buff_ptr += 2;
			assembly_ptr += 3;
			*assembly_ptr++ = 0xFF;
		} else if (data_to_go == 1) {
			assembly_ptr[0] = CIN_SYSEX_ENDS_WITH_FOLLOWING_SINGLE_BYTE;
			memcpy(assembly_ptr+1, buff_ptr, 1);
			buff_ptr += 1;
			assembly_ptr += 2;
			*assembly_ptr++ = 0xFF;
			*assembly_ptr++ = 0xFF;
		}
	}

	MIDI_DataTx(sysex_tx_assembly_buffer, assembly_ptr - sysex_tx_assembly_buffer);
}


void sysex_erase_settings(uint8_t* data_packet_start){
	if(data_packet_start[0] != 0x42 || data_packet_start[1] != 0x24){
		return;
	}

	flash_settings_erase();

	midi_msg_tx_buffer[0] = SYSEX_START;
	midi_msg_tx_buffer[1] = MIDI_MANUF_ID;
	midi_msg_tx_buffer[2] = SYSEX_RSP_ERASE_FLASH;
	midi_msg_tx_buffer[3] = SYSEX_END;
	sysex_send_message(midi_msg_tx_buffer, 4);
}

void sysex_write_flash(uint8_t* data_packet_start){
	uint32_t flash_byte_offset = ( (data_packet_start[0] << 7) | data_packet_start[1]) * 16;

	uint8_t reassembled_array[16];
	data_packet_start += 2;
	for (int i=0; i<16; i++){
		reassembled_array[i] = data_packet_start[2*i] << 4 | data_packet_start[2*i + 1];
	}

	flash_settings_write(reassembled_array, flash_byte_offset);

	midi_msg_tx_buffer[0] = SYSEX_START;
	midi_msg_tx_buffer[1] = MIDI_MANUF_ID;
	midi_msg_tx_buffer[2] = SYSEX_RSP_WRITE_FLASH;
	midi_msg_tx_buffer[3] = SYSEX_END;
	sysex_send_message(midi_msg_tx_buffer, 4);

}

void process_sysex_message(void){
	// Check start and end bytes
	if(sysex_rx_counter < 4U || sysex_rx_buffer[0] != SYSEX_START ||
				sysex_rx_buffer[sysex_rx_counter -1] != SYSEX_END){
		abort_sysex_message();
		return;
	}

	MIDI_sysex_head_TypeDef *pSysexHead = (MIDI_sysex_head_TypeDef*)sysex_rx_buffer;

	if(pSysexHead->manuf_id != MIDI_MANUF_ID){
		abort_sysex_message();
		return;
	}

	switch(pSysexHead->msg_cmd){
	/* v3 owns its configuration store.  Do not let the factory editor erase
	 * or write it through its old raw-page SysEx protocol. */
	case SYSEX_CMD_ERASE_FLASH:
	case SYSEX_CMD_WRITE_FLASH:
		break;
	case SYSEX_CMD_RESET:
		if (sysex_rx_counter == 4U) NVIC_SystemReset();
		break;
	default:
		break;
	}

	sysex_rx_counter = 0;
}

uint16_t MIDI_DataRx(uint8_t *msg, uint16_t length)
{
	if (msg == NULL) return USBD_FAIL;
	midiCmd_note_transport_activity();

	/* USB-MIDI event packets are always four bytes, regardless of how many MIDI
	 * bytes their CIN contains.  Advancing by 2/3 bytes desynchronised the old
	 * parser, and an unbounded SysEx stream could overwrite adjacent RAM. */
	uint16_t processed_data_cnt = 0U;
	while ((uint16_t)(processed_data_cnt + 4U) <= length) {
		uint8_t usb_msg_cin = msg[processed_data_cnt] & 0x0FU;

		if(sysex_rx_counter != 0){
			if(usb_msg_cin != CIN_SYSEX_STARTS_OR_CONTINUES &&
					usb_msg_cin != CIN_SYSEX_ENDS_WITH_FOLLOWING_SINGLE_BYTE &&
					usb_msg_cin != CIN_SYSEX_ENDS_WITH_FOLLOWING_TWO_BYTES &&
					usb_msg_cin != CIN_SYSEX_ENDS_WITH_FOLLOWING_THREE_BYTES){
				abort_sysex_message();
			}
		}

		switch(usb_msg_cin){
		case CIN_SYSEX_STARTS_OR_CONTINUES:
			append_sysex_bytes(msg + processed_data_cnt + 1U, 3U);
			break;
		case CIN_SYSEX_ENDS_WITH_FOLLOWING_SINGLE_BYTE:
			if (append_sysex_bytes(msg + processed_data_cnt + 1U, 1U))
				process_sysex_message();
			break;
		case CIN_SYSEX_ENDS_WITH_FOLLOWING_TWO_BYTES:
			if (append_sysex_bytes(msg + processed_data_cnt + 1U, 2U))
				process_sysex_message();
			break;
		case CIN_SYSEX_ENDS_WITH_FOLLOWING_THREE_BYTES:
			if (append_sysex_bytes(msg + processed_data_cnt + 1U, 3U))
				process_sysex_message();
			break;

		case CIN_SINGLE_BYTE: {
			/* The v3 2-profile x 10-key x 1-command model is independent of
			 * transport forwarding.  Restore USB-to-DIN system real-time bytes,
			 * including Continue. Deliberately do not forward Active Sensing or
			 * System Reset: they were not part of the original transport feature. */
			const uint8_t realtime = msg[processed_data_cnt + 1U];
			if (realtime == MIDI_REALTIME_CLOCK ||
				realtime == MIDI_REALTIME_START ||
				realtime == MIDI_REALTIME_CONTINUE ||
				realtime == MIDI_REALTIME_STOP)
				midiCmd_send_byte_serial(realtime);
			break;
		}

		default:
			if (sysex_rx_counter != 0U) abort_sysex_message();
			break;

		}
		processed_data_cnt += 4U;
	}

	return USBD_OK;
}

uint16_t MIDI_DataTx(uint8_t *msg, uint16_t length)
{
  if (length > UINT8_MAX) return USBD_FAIL;
  midiCmd_note_transport_activity();
  return USBD_MIDI_SendPacket(msg, (uint8_t)length);
}
