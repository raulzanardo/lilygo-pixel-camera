#pragma once

#include "lvgl.h"

#ifdef __cplusplus
extern "C"
{
#endif

  lv_obj_t *ui_get_gallery_screen(void);
  void ui_gallery_show(void);
  void ui_gallery_show_last_photo(void);

#ifdef __cplusplus
} /* extern "C" */
#endif
