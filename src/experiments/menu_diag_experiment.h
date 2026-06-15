#ifndef MENU_DIAG_EXPERIMENT_H
#define MENU_DIAG_EXPERIMENT_H

#if NEOPICO_OSD_RES_CONFIRM
#include "video/video_pipeline.h"
#endif

void menu_diag_experiment_init(void);
void menu_diag_experiment_on_menu_open(void);
void menu_diag_experiment_on_menu_close(void);
void menu_diag_experiment_tick_background(void);

#if NEOPICO_OSD_RES_CONFIRM
// Arm the resolution-confirmation prompt for this boot (call before the menu
// init): show the keep/revert countdown for new_mode, reverting to previous_mode.
void menu_diag_experiment_arm_res_confirm(video_pipeline_reboot_mode_t new_mode,
                                          video_pipeline_reboot_mode_t previous_mode);
#endif

#endif // MENU_DIAG_EXPERIMENT_H
