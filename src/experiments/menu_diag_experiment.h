#ifndef MENU_DIAG_EXPERIMENT_H
#define MENU_DIAG_EXPERIMENT_H

#include "video/video_pipeline.h"

void menu_diag_experiment_init(void);
void menu_diag_experiment_on_menu_open(void);
void menu_diag_experiment_on_menu_close(void);
void menu_diag_experiment_tick_background(void);

// Arm the combined keep/revert confirmation prompt for this boot (call before
// the menu init): show the countdown for new_mode (already active, per the
// Apply that rebooted into it), reverting to revert_resolution/revert_genlock
// on BACK/B/SELECT or timeout. Unconditional (not guarded by
// NEOPICO_EXP_GENLOCK_DYNAMIC): a batched Video-screen Apply can change
// Resolution and/or Refresh (genlock) together, so a single revert record
// covers both -- revert_resolution comes from watchdog scratch
// (video_pipeline_take_pending_confirmation()), revert_genlock from flash
// (settings.h neopico_settings_t.pending_revert_genlock).
void menu_diag_experiment_arm_revert_confirm(video_pipeline_reboot_mode_t new_mode,
                                             video_pipeline_reboot_mode_t revert_resolution, bool revert_genlock);

#endif // MENU_DIAG_EXPERIMENT_H
