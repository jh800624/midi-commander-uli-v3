/**
  ******************************************************************************
  * @file    usbd_midi.c
  ******************************************************************************

    (CC at)2016 by D.F.Mac. @TripArts Music

*/ 

/* Includes ------------------------------------------------------------------*/
#include "usbd_midi.h"
#include "usbd_desc.h"
#include "stm32f1xx_hal_conf.h"
#include "usbd_ctlreq.h"
#include "stm32f1xx_hal.h"

static uint8_t  USBD_MIDI_Init (USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t  USBD_MIDI_DeInit (USBD_HandleTypeDef *pdev, uint8_t cfgidx);
static uint8_t  USBD_MIDI_DataIn (USBD_HandleTypeDef *pdev, uint8_t epnum);
static uint8_t  USBD_MIDI_DataOut (USBD_HandleTypeDef *pdev, uint8_t epnum);
static void USBD_MIDI_StartNextPacket(void);

static uint8_t  *USBD_MIDI_GetCfgDesc (uint16_t *length);

USBD_HandleTypeDef *pInstance = NULL; 

uint32_t APP_Rx_ptr_in  = 0;
uint32_t APP_Rx_ptr_out = 0;
uint32_t APP_Rx_length  = 0;
volatile uint8_t  USB_Tx_State = 0;

/* A USB IN endpoint has only one hardware transfer slot.  Footswitch MIDI
 * must not be discarded just because the host has not acknowledged the prior
 * packet yet, so retain complete USB-MIDI endpoint packets in FIFO order. */
#define MIDI_TX_QUEUE_DEPTH  (32U)
#define MIDI_TX_QUEUE_BYTES  (MIDI_DATA_IN_PACKET_SIZE)
typedef struct {
  uint8_t data[MIDI_TX_QUEUE_BYTES];
  uint8_t length;
} midi_tx_queue_entry_t;
static midi_tx_queue_entry_t midi_tx_queue[MIDI_TX_QUEUE_DEPTH];
static volatile uint8_t midi_tx_queue_head;
static volatile uint8_t midi_tx_queue_tail;
static volatile uint8_t midi_tx_queue_count;

__ALIGN_BEGIN uint8_t USB_Rx_Buffer[MIDI_DATA_OUT_PACKET_SIZE] __ALIGN_END ;
__ALIGN_BEGIN uint8_t APP_Rx_Buffer[APP_RX_DATA_SIZE] __ALIGN_END ;


/* USB MIDI interface class callbacks structure */
USBD_ClassTypeDef  USBD_MIDI = 
{
  USBD_MIDI_Init,
  USBD_MIDI_DeInit,
  NULL,
  NULL,
  NULL,
  USBD_MIDI_DataIn,
  USBD_MIDI_DataOut,
  NULL,
  NULL,
  NULL,
  NULL,// HS
  USBD_MIDI_GetCfgDesc,// FS
  NULL,// OTHER SPEED
  NULL,// DEVICE_QUALIFIER
};

/* USB MIDI device Configuration Descriptor */
__ALIGN_BEGIN uint8_t USBD_MIDI_CfgDesc[USB_MIDI_CONFIG_DESC_SIZ] __ALIGN_END =
{
  // configuration descriptor
  0x09, 0x02, 0x65, 0x00, 0x02, 0x01, 0x00, 0x80, 0x31,

  // The Audio Interface Collection
  0x09, 0x04, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x00, // Standard AC Interface Descriptor
  0x09, 0x24, 0x01, 0x00, 0x01, 0x09, 0x00, 0x01, 0x01, // Class-specific AC Interface Descriptor
  0x09, 0x04, 0x01, 0x00, 0x02, 0x01, 0x03, 0x00, 0x00, // MIDIStreaming Interface Descriptors
  0x07, 0x24, 0x01, 0x00, 0x01, 0x41, 0x00,             // Class-Specific MS Interface Header Descriptor

  // MIDI IN JACKS
  0x06, 0x24, 0x02, 0x01, 0x01, 0x00,
  0x06, 0x24, 0x02, 0x02, 0x02, 0x00,

  // MIDI OUT JACKS
  0x09, 0x24, 0x03, 0x01, 0x03, 0x01, 0x02, 0x01, 0x00,
  0x09, 0x24, 0x03, 0x02, 0x06, 0x01, 0x01, 0x01, 0x00,

  // OUT endpoint descriptor
  0x09, 0x05, MIDI_OUT_EP, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
  0x05, 0x25, 0x01, 0x01, 0x01,

  // IN endpoint descriptor
  0x09, 0x05, MIDI_IN_EP, 0x02, 0x40, 0x00, 0x00, 0x00, 0x00,
  0x05, 0x25, 0x01, 0x01, 0x03,
};

static uint8_t USBD_MIDI_Init(USBD_HandleTypeDef *pdev, uint8_t cfgidx){
  pInstance = pdev;
  USB_Tx_State = 0U;
  midi_tx_queue_head = 0U;
  midi_tx_queue_tail = 0U;
  midi_tx_queue_count = 0U;
  USBD_LL_OpenEP(pdev,MIDI_IN_EP,USBD_EP_TYPE_BULK,MIDI_DATA_IN_PACKET_SIZE);
  USBD_LL_OpenEP(pdev,MIDI_OUT_EP,USBD_EP_TYPE_BULK,MIDI_DATA_OUT_PACKET_SIZE);

  pdev->ep_out[MIDI_OUT_EP & 0xFU].is_used = 1U;
  pdev->ep_in[MIDI_IN_EP & 0xFU].is_used = 1U;

  USBD_LL_PrepareReceive(pdev,MIDI_OUT_EP,(uint8_t*)(USB_Rx_Buffer),MIDI_DATA_OUT_PACKET_SIZE);
  return 0;
}

static uint8_t USBD_MIDI_DeInit (USBD_HandleTypeDef *pdev, uint8_t cfgidx){
  pInstance = NULL;
  USB_Tx_State = 0U;
  midi_tx_queue_head = 0U;
  midi_tx_queue_tail = 0U;
  midi_tx_queue_count = 0U;
  USBD_LL_CloseEP(pdev,MIDI_IN_EP);
  USBD_LL_CloseEP(pdev,MIDI_OUT_EP);
  return 0;
}

static uint8_t USBD_MIDI_DataIn (USBD_HandleTypeDef *pdev, uint8_t epnum){

  if (USB_Tx_State == 1){
    USB_Tx_State = 0;
  }
  USBD_MIDI_StartNextPacket();
  return USBD_OK;
}

static uint8_t  USBD_MIDI_DataOut (USBD_HandleTypeDef *pdev, uint8_t epnum)
{      
  uint16_t USB_Rx_Cnt;

  USBD_MIDI_ItfTypeDef *pmidi;
  pmidi = (USBD_MIDI_ItfTypeDef *)(pdev->pUserData);

  USB_Rx_Cnt = ((PCD_HandleTypeDef*)pdev->pData)->OUT_ep[epnum].xfer_count;

  pmidi->pIf_MidiRx(USB_Rx_Buffer, USB_Rx_Cnt);

  USBD_LL_PrepareReceive(pdev,MIDI_OUT_EP,USB_Rx_Buffer,MIDI_DATA_OUT_PACKET_SIZE);
  return USBD_OK;
}



static void USBD_MIDI_StartNextPacket(void){
  if (pInstance == NULL || pInstance->dev_state != USBD_STATE_CONFIGURED ||
      USB_Tx_State != 0U || midi_tx_queue_count == 0U)
    return;

  const uint8_t slot = midi_tx_queue_tail;
  USB_Tx_State = 1U;
  if (USBD_LL_Transmit(pInstance, MIDI_IN_EP, midi_tx_queue[slot].data,
                       midi_tx_queue[slot].length) == USBD_OK) {
    midi_tx_queue_tail = (uint8_t)((slot + 1U) % MIDI_TX_QUEUE_DEPTH);
    midi_tx_queue_count--;
  } else {
    USB_Tx_State = 0U;
  }
}

uint8_t USBD_MIDI_SendPacket(uint8_t *buffer, uint8_t len){
	/* MIDI must keep working on the DIN output when USB is unplugged.  The old
	 * code dereferenced a NULL pInstance before enumeration and spun forever if
	 * an IN transfer stopped completing during suspend/disconnect. */
	if (pInstance == NULL || pInstance->dev_state != USBD_STATE_CONFIGURED)
		return USBD_FAIL;
	if (buffer == NULL || len == 0U || len > MIDI_TX_QUEUE_BYTES)
		return USBD_FAIL;

  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  if (midi_tx_queue_count >= MIDI_TX_QUEUE_DEPTH) {
    if (primask == 0U) __enable_irq();
		return USBD_BUSY;
  }

	const uint8_t slot = midi_tx_queue_head;
	memcpy(midi_tx_queue[slot].data, buffer, len);
	midi_tx_queue[slot].length = len;
	midi_tx_queue_head = (uint8_t)((slot + 1U) % MIDI_TX_QUEUE_DEPTH);
	midi_tx_queue_count++;
	USBD_MIDI_StartNextPacket();
  if (primask == 0U) __enable_irq();
	return USBD_OK;
}

uint8_t USBD_MIDI_IsTxIdle(void){
	return USB_Tx_State == 0U && midi_tx_queue_count == 0U;
}

void USBD_MIDI_TxTask(void){
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  USBD_MIDI_StartNextPacket();
  if (primask == 0U) __enable_irq();
}

uint8_t USBD_MIDI_BeginMaintenance(void){
	/* Bulk OUT NAK makes the host retry instead of losing a real-time packet
	 * while single-bank Flash temporarily stalls instruction fetch. */
	if (pInstance == NULL || pInstance->dev_state != USBD_STATE_CONFIGURED)
		return 1U;
	if (pInstance->pData == NULL || USB_Tx_State != 0U)
		return 0U;
	PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pInstance->pData;
	const uint32_t primask = __get_PRIMASK();
	__disable_irq();
	PCD_SET_EP_RX_STATUS(hpcd->Instance, MIDI_OUT_EP & 0x7FU, USB_EP_RX_NAK);
	if (primask == 0U) __enable_irq();
	return 1U;
}

void USBD_MIDI_EndMaintenance(void){
	if (pInstance != NULL && pInstance->dev_state == USBD_STATE_CONFIGURED)
		(void)USBD_LL_PrepareReceive(pInstance, MIDI_OUT_EP, USB_Rx_Buffer,
				MIDI_DATA_OUT_PACKET_SIZE);
}

void USBD_MIDI_NotifyLinkDown(void){
	/* An IN transfer that was active when VBUS/traffic disappeared will never
	 * receive DataIn completion. Clear only the software busy latch; subsequent
	 * sends remain rejected by dev_state until USB is configured again. */
	USB_Tx_State = 0U;
	midi_tx_queue_head = 0U;
	midi_tx_queue_tail = 0U;
	midi_tx_queue_count = 0U;
}

static uint8_t *USBD_MIDI_GetCfgDesc (uint16_t *length){
  *length = sizeof (USBD_MIDI_CfgDesc);
  return USBD_MIDI_CfgDesc;
}

uint8_t USBD_MIDI_RegisterInterface(USBD_HandleTypeDef *pdev, USBD_MIDI_ItfTypeDef *fops)
{
  uint8_t ret = USBD_FAIL;
  
  if(fops != NULL){
    pdev->pUserData= fops;
    ret = USBD_OK;    
  }
  
  return ret;
}
