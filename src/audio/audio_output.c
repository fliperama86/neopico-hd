/**
 * HDMI Audio Output Module
 * Handles writing processed audio samples to DVI/HDMI audio ring buffer
 */

#include "audio_output.h"
#include "audio_buffer.h"
#include "dvi.h"
#include "audio_ring.h"

static struct dvi_inst *g_dvi = NULL;

void audio_output_init(struct dvi_inst *dvi, uint32_t sample_rate) {
    g_dvi = dvi;

    // Configure HDMI audio
    // Note: Audio buffer is set by main.c via dvi_audio_sample_buffer_set()
    dvi_set_audio_freq(dvi, sample_rate, 25200, 6144);
}

void audio_output_write(const ap_sample_t *samples, uint32_t count) {
    if (!g_dvi || count == 0) return;

    // Check available space in DVI audio ring
    int space = get_write_size(&g_dvi->audio_ring, false);
    if (space == 0) return;

    // Limit to available space
    uint32_t to_write = (count < (uint32_t)space) ? count : (uint32_t)space;

    // Write to HDMI audio ring
    audio_sample_t *audio_ptr = get_write_pointer(&g_dvi->audio_ring);
    for (uint32_t i = 0; i < to_write; i++) {
        audio_ptr[i].channels[0] = samples[i].left;
        audio_ptr[i].channels[1] = samples[i].right;
    }
    increase_write_pointer(&g_dvi->audio_ring, to_write);
}
