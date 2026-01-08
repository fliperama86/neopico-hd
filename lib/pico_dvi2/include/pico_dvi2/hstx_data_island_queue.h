#ifndef HSTX_DATA_ISLAND_QUEUE_H
#define HSTX_DATA_ISLAND_QUEUE_H

#include <stdint.h>
#include <stdbool.h>
#include "pico_dvi2/hstx_packet.h"

/**
 * Initialize the Data Island queue and scheduler.
 */
void hstx_di_queue_init(void);

/**
 * Push a pre-encoded Data Island into the queue.
 * Returns true if successful, false if the queue is full.
 */
bool hstx_di_queue_push(const hstx_data_island_t *island);

/**
 * Advance the Data Island scheduler by one scanline.
 * Must be called exactly once per scanline in the DMA ISR.
 */
void hstx_di_queue_tick(void);

/**
 * Get the next audio Data Island packet if the scheduler determines it's time.
 * 
 * @return Pointer to 36-word HSTX data island, or NULL if no packet is due.
 */
const uint32_t* hstx_di_queue_get_audio_packet(void);

#endif // HSTX_DATA_ISLAND_QUEUE_H
