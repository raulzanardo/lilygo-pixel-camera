#include "ui_SettingsScreen.h"
#include "ui_HomeScreen.h"
#include "../ui.h"
#include "lvgl.h"

static lv_obj_t *ui_settings_screen = NULL;
static lv_obj_t *ui_settings_flash_switch = NULL;
static lv_obj_t *ui_settings_storage_switch = NULL;
static lv_obj_t *ui_settings_auto_adjust_switch = NULL;
static lv_obj_t *ui_settings_back_btn = NULL;

// Forward declaration
extern void ui_event_FlashSwitch(lv_event_t *e);
extern bool camera_led_open_flag;

static void ui_settings_back_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    lv_scr_load_anim(ui_HomeScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
    ui_resume_camera_timer();
}

static void build_settings_screen()
{
    if (ui_settings_screen)
    {
        return;
    }

    ui_settings_screen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_settings_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(ui_settings_screen, 12, 0);
    lv_obj_set_style_pad_row(ui_settings_screen, 12, 0);
    lv_obj_set_style_pad_column(ui_settings_screen, 12, 0);
    lv_obj_set_flex_flow(ui_settings_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_settings_screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *header_row = lv_obj_create(ui_settings_screen);
    lv_obj_set_width(header_row, LV_PCT(100));
    lv_obj_set_height(header_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header_row, 0, 0);
    lv_obj_set_style_pad_all(header_row, 0, 0);
    lv_obj_set_style_pad_row(header_row, 0, 0);
    lv_obj_set_style_pad_column(header_row, 8, 0);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_settings_back_btn = lv_btn_create(header_row);
    lv_obj_set_size(ui_settings_back_btn, 44, 44);
    lv_obj_set_style_radius(ui_settings_back_btn, 8, 0);
    lv_obj_set_style_bg_color(ui_settings_back_btn, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_t *back_label = lv_label_create(ui_settings_back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(ui_settings_back_btn, ui_settings_back_event, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(header_row);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);

    // flash toggle
    lv_obj_t *flash_row = lv_obj_create(ui_settings_screen);
    lv_obj_set_width(flash_row, LV_PCT(100));
    lv_obj_set_height(flash_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(flash_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(flash_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(flash_row, 0, 0);
    lv_obj_set_style_pad_all(flash_row, 0, 0);
    lv_obj_set_style_pad_row(flash_row, 8, 0);
    lv_obj_set_style_pad_column(flash_row, 8, 0);
    lv_obj_set_flex_flow(flash_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(flash_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *flash_label = lv_label_create(flash_row);
    lv_label_set_text(flash_label, "Flash");

    ui_settings_flash_switch = lv_switch_create(flash_row);
    if (camera_led_open_flag)
    {
        lv_obj_add_state(ui_settings_flash_switch, LV_STATE_CHECKED);
    }
    lv_obj_add_event_cb(ui_settings_flash_switch, ui_event_FlashSwitch, LV_EVENT_ALL, NULL);

    // storage
    lv_obj_t *storage_row = lv_obj_create(ui_settings_screen);
    lv_obj_set_width(storage_row, LV_PCT(100));
    lv_obj_set_height(storage_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(storage_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(storage_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(storage_row, 0, 0);
    lv_obj_set_style_pad_all(storage_row, 0, 0);
    lv_obj_set_style_pad_row(storage_row, 8, 0);
    lv_obj_set_style_pad_column(storage_row, 8, 0);
    lv_obj_set_flex_flow(storage_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(storage_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *storage_label = lv_label_create(storage_row);
    lv_label_set_text(storage_label, "Storage mode");

    ui_settings_storage_switch = lv_switch_create(storage_row);
    lv_obj_add_event_cb(ui_settings_storage_switch, ui_event_StorageSwitch, LV_EVENT_ALL, NULL);


        // auto adjust toggle
    lv_obj_t *auto_adjust_row = lv_obj_create(ui_settings_screen);
    lv_obj_set_width(auto_adjust_row, LV_PCT(100));
    lv_obj_set_height(auto_adjust_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(auto_adjust_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(auto_adjust_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(  auto_adjust_row, 0, 0);
    lv_obj_set_style_pad_all(auto_adjust_row, 0, 0);
    lv_obj_set_style_pad_row(auto_adjust_row, 8, 0);
    lv_obj_set_style_pad_column(auto_adjust_row, 8, 0);
    lv_obj_set_flex_flow(auto_adjust_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(auto_adjust_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER); 

    lv_obj_t *auto_adjust_label = lv_label_create(auto_adjust_row);
    lv_label_set_text(auto_adjust_label, "Auto adjust");

    ui_settings_auto_adjust_switch = lv_switch_create(auto_adjust_row);
}

lv_obj_t *ui_get_settings_screen(void)
{
    return ui_settings_screen;
}

void ui_settings_show(void)
{
    build_settings_screen();
    ui_pause_camera_timer();
    lv_scr_load_anim(ui_settings_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
