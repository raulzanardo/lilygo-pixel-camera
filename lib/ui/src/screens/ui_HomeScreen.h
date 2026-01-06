#ifndef UI_HOMESCREEN_H
#define UI_HOMESCREEN_H

#ifdef __cplusplus
extern "C" {
#endif

// SCREEN: ui_HomeScreen
extern void ui_HomeScreen_screen_init(void);
extern void ui_HomeScreen_screen_destroy(void);
extern lv_obj_t *ui_HomeScreen;
extern void ui_event_Dropdown1( lv_event_t * e);
extern void ui_event_PaletteDropdown( lv_event_t * e);
extern lv_obj_t *ui_FilterDropdown;
extern lv_obj_t *ui_PaletteDropdown;
extern lv_obj_t *ui_Image1;
// CUSTOM VARIABLES
void ui_set_filter_mode(int mode);
void ui_set_palette_index(int idx);
void ui_get_palette(const uint32_t **palette, int *size);
void ui_set_flash_enabled(bool enabled);
bool ui_is_flash_enabled(void);
lv_obj_t *ui_get_gallery_button(void);
int ui_get_filter_mode(void);
int ui_get_dither_type(void);
int ui_get_pixel_size(void);
void ui_show_photo_overlay(const char *text);

void ui_pause_camera_timer(void);
void ui_resume_camera_timer(void);
void ui_event_FlashSwitch(lv_event_t *e);
void ui_event_StorageSwitch(lv_event_t *e);
void ui_event_CameraCanvasTap(lv_event_t *e);
bool ui_get_aec2_enabled(void);
bool ui_get_gain_ctrl_enabled(void);
int ui_get_agc_gain(void);
bool ui_get_exposure_ctrl_enabled(void);
int ui_get_aec_value(void);
bool ui_get_auto_adjust_enabled(void);
void ui_set_auto_adjust_enabled(bool enabled);
int ui_get_zoom_level(void);
void ui_set_zoom_level(int level);
bool ui_get_screenshot_mode_enabled(void);
void ui_set_screenshot_mode_enabled(bool enabled);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif
