#ifndef MENU_DIAG_EXPERIMENT_H
#define MENU_DIAG_EXPERIMENT_H

#include "video/video_pipeline.h"

void menu_diag_experiment_init(void);
void menu_diag_experiment_on_menu_open(void);
void menu_diag_experiment_on_menu_close(void);
void menu_diag_experiment_tick_background(void);

// Arm the resolution-confirmation prompt for this boot (call before the menu
// init): show the keep/revert countdown for new_mode, reverting to previous_mode.
void menu_diag_experiment_arm_res_confirm(video_pipeline_reboot_mode_t new_mode,
                                          video_pipeline_reboot_mode_t previous_mode);

#if NEOPICO_EXP_GENLOCK_DYNAMIC
// Arm the genlock-confirmation prompt for this boot (call before the menu
// init): show the keep/revert countdown for new_enabled, reverting to
// previous_enabled. Mirrors menu_diag_experiment_arm_res_confirm() above.
void menu_diag_experiment_arm_genlock_confirm(bool new_enabled, bool previous_enabled);
#endif

#endif // MENU_DIAG_EXPERIMENT_H
