#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

  lv_obj_t *ui_get_settings_screen(void);
  void ui_settings_show(void);
  void ui_settings_set_back_btn_enabled(bool enabled);

#ifdef __cplusplus
} /* extern "C" */
#endif
