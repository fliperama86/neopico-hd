#ifndef SELFTEST_H
#define SELFTEST_H

// Load PIO program and claim DMA channel (call from Core 0, before core1 launch)
void selftest_init(void);

// Start PIO+DMA sampling (call when selftest screen opens)
void selftest_start(void);

// Stop PIO+DMA sampling (call when selftest screen closes)
void selftest_stop(void);

// Reset state and draw static layout (call when OSD becomes visible)
void selftest_reset(void);

// Check update flag and render one icon phase if needed (~2.5us max)
void selftest_render(void);

#endif // SELFTEST_H
