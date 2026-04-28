#include "ui_SettingsScreen.h"
#include "ui_HomeScreen.h"
#include "../ui.h"
#include "lvgl.h"
#include "../../../include/utilities.h"
#include <cstdio>
#include <cstring>

extern bool sd_format_card();

static lv_obj_t *ui_settings_screen = NULL;
static lv_obj_t *ui_settings_flash_switch = NULL;
static lv_obj_t *ui_settings_storage_switch = NULL;
static lv_obj_t *ui_settings_auto_adjust_switch = NULL;
static lv_obj_t *ui_settings_auto_levels_switch = NULL;
static lv_obj_t *ui_settings_screenshot_switch = NULL;
static lv_obj_t *ui_settings_easy_mode_switch = NULL;
static lv_obj_t *ui_settings_back_btn = NULL;
static lv_obj_t *ui_settings_format_sd_btn = NULL;

// Forward declaration
extern void ui_event_FlashSwitch(lv_event_t *e);
extern bool camera_led_open_flag;

static void ui_settings_auto_levels_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }
    lv_obj_t *target = lv_event_get_target(e);
    if (!target)
    {
        return;
    }
    bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    ui_set_auto_levels_enabled(enabled);
}

static void ui_settings_auto_adjust_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *target = lv_event_get_target(e);
    if (!target)
    {
        return;
    }

    bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    ui_set_auto_adjust_enabled(enabled);
}

static void ui_settings_screenshot_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *target = lv_event_get_target(e);
    if (!target)
    {
        return;
    }

    bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    ui_set_screenshot_mode_enabled(enabled);
}

static void ui_settings_easy_mode_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *target = lv_event_get_target(e);
    if (!target)
    {
        return;
    }

    bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    ui_set_easy_mode_enabled(enabled);
}

static void ui_settings_format_sd_confirm_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *mbox = lv_event_get_current_target(e);
    const char *btn_text = lv_msgbox_get_active_btn_text(mbox);

    if (btn_text && strcmp(btn_text, "Format") == 0)
    {
        lv_msgbox_close(mbox);

        // Show a full-screen modal overlay with spinner and status text
        lv_obj_t *overlay = lv_obj_create(lv_layer_top());
        lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(overlay, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(overlay, LV_OPA_70, 0);
        lv_obj_set_style_border_width(overlay, 0, 0);
        lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(overlay, 16, 0);

        lv_obj_t *spinner = lv_spinner_create(overlay, 1000, 60);
        lv_obj_set_size(spinner, 60, 60);

        lv_obj_t *status_label = lv_label_create(overlay);
        lv_label_set_text(status_label, "Formatting...");
        lv_obj_set_style_text_color(status_label, lv_color_white(), 0);

        // Force a display refresh so the overlay appears before the blocking operation
        lv_refr_now(NULL);

        bool ok = sd_format_card();

        // Update status, remove spinner
        lv_obj_del(spinner);
        if (ok)
        {
            lv_label_set_text(status_label, "Format complete!");
            lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_GREEN), 0);
        }
        else
        {
            lv_label_set_text(status_label, "Format failed!");
            lv_obj_set_style_text_color(status_label, lv_palette_main(LV_PALETTE_RED), 0);
        }

        lv_refr_now(NULL);

        // Auto-close the overlay after 1.5 seconds
        lv_timer_t *close_timer = lv_timer_create(
            [](lv_timer_t *t)
            {
                lv_obj_t *ov = static_cast<lv_obj_t *>(t->user_data);
                lv_obj_del(ov);
                lv_timer_del(t);
            },
            1500, overlay);
        (void)close_timer;
    }
    else
    {
        lv_msgbox_close(mbox);
    }
}

static void ui_settings_format_sd_event(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    static const char *btn_txts[] = {"Format", "Cancel", ""};
    lv_obj_t *mbox = lv_msgbox_create(
        NULL,
        "Format SD Card",
        "This will delete ALL files on the SD card.\nThis action cannot be undone!",
        btn_txts,
        false);
    lv_obj_add_event_cb(mbox, ui_settings_format_sd_confirm_event, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_center(mbox);
}

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
    lv_obj_set_size(ui_settings_flash_switch, 60, 40);
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
    lv_obj_set_size(ui_settings_storage_switch, 60, 40);

    // auto adjust toggle
    lv_obj_t *auto_adjust_row = lv_obj_create(ui_settings_screen);
    lv_obj_set_width(auto_adjust_row, LV_PCT(100));
    lv_obj_set_height(auto_adjust_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(auto_adjust_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(auto_adjust_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(auto_adjust_row, 0, 0);
    lv_obj_set_style_pad_all(auto_adjust_row, 0, 0);
    lv_obj_set_style_pad_row(auto_adjust_row, 8, 0);
    lv_obj_set_style_pad_column(auto_adjust_row, 8, 0);
    lv_obj_set_flex_flow(auto_adjust_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(auto_adjust_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *auto_adjust_label = lv_label_create(auto_adjust_row);
    lv_label_set_text(auto_adjust_label, "Auto adjust");

    ui_settings_auto_adjust_switch = lv_switch_create(auto_adjust_row);
    if (ui_get_auto_adjust_enabled())
    {
        lv_obj_add_state(ui_settings_auto_adjust_switch, LV_STATE_CHECKED);
    }
    lv_obj_set_size(ui_settings_auto_adjust_switch, 60, 40);
    lv_obj_add_event_cb(ui_settings_auto_adjust_switch, ui_settings_auto_adjust_event, LV_EVENT_ALL, NULL);

    // auto levels toggle (palette brightness normalization)
    lv_obj_t *auto_levels_row = lv_obj_create(ui_settings_screen);
    lv_obj_set_width(auto_levels_row, LV_PCT(100));
    lv_obj_set_height(auto_levels_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(auto_levels_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(auto_levels_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(auto_levels_row, 0, 0);
    lv_obj_set_style_pad_all(auto_levels_row, 0, 0);
    lv_obj_set_style_pad_row(auto_levels_row, 8, 0);
    lv_obj_set_style_pad_column(auto_levels_row, 8, 0);
    lv_obj_set_flex_flow(auto_levels_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(auto_levels_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *auto_levels_label = lv_label_create(auto_levels_row);
    lv_label_set_text(auto_levels_label, "Auto Dithering");

    ui_settings_auto_levels_switch = lv_switch_create(auto_levels_row);
    if (ui_get_auto_levels_enabled())
    {
        lv_obj_add_state(ui_settings_auto_levels_switch, LV_STATE_CHECKED);
    }
    lv_obj_set_size(ui_settings_auto_levels_switch, 60, 40);
    lv_obj_add_event_cb(ui_settings_auto_levels_switch, ui_settings_auto_levels_event, LV_EVENT_ALL, NULL);

    // screenshot mode toggle
    lv_obj_t *screenshot_row = lv_obj_create(ui_settings_screen);
    lv_obj_set_width(screenshot_row, LV_PCT(100));
    lv_obj_set_height(screenshot_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(screenshot_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(screenshot_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(screenshot_row, 0, 0);
    lv_obj_set_style_pad_all(screenshot_row, 0, 0);
    lv_obj_set_style_pad_row(screenshot_row, 8, 0);
    lv_obj_set_style_pad_column(screenshot_row, 8, 0);
    lv_obj_set_flex_flow(screenshot_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(screenshot_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *screenshot_label = lv_label_create(screenshot_row);
    lv_label_set_text(screenshot_label, "Screenshot mode");

    ui_settings_screenshot_switch = lv_switch_create(screenshot_row);
    if (ui_get_screenshot_mode_enabled())
    {
        lv_obj_add_state(ui_settings_screenshot_switch, LV_STATE_CHECKED);
    }
    lv_obj_set_size(ui_settings_screenshot_switch, 60, 40);
    lv_obj_add_event_cb(ui_settings_screenshot_switch, ui_settings_screenshot_event, LV_EVENT_ALL, NULL);

    // easy mode toggle
    lv_obj_t *easy_mode_row = lv_obj_create(ui_settings_screen);
    lv_obj_set_width(easy_mode_row, LV_PCT(100));
    lv_obj_set_height(easy_mode_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(easy_mode_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(easy_mode_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(easy_mode_row, 0, 0);
    lv_obj_set_style_pad_all(easy_mode_row, 0, 0);
    lv_obj_set_style_pad_row(easy_mode_row, 8, 0);
    lv_obj_set_style_pad_column(easy_mode_row, 8, 0);
    lv_obj_set_flex_flow(easy_mode_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(easy_mode_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *easy_mode_label = lv_label_create(easy_mode_row);
    lv_label_set_text(easy_mode_label, "Easy mode");

    ui_settings_easy_mode_switch = lv_switch_create(easy_mode_row);
    if (ui_get_easy_mode_enabled())
    {
        lv_obj_add_state(ui_settings_easy_mode_switch, LV_STATE_CHECKED);
    }
    lv_obj_set_size(ui_settings_easy_mode_switch, 60, 40);
    lv_obj_add_event_cb(ui_settings_easy_mode_switch, ui_settings_easy_mode_event, LV_EVENT_ALL, NULL);

    // format SD card button
    lv_obj_t *format_sd_row = lv_obj_create(ui_settings_screen);
    lv_obj_set_width(format_sd_row, LV_PCT(100));
    lv_obj_set_height(format_sd_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(format_sd_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(format_sd_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(format_sd_row, 0, 0);
    lv_obj_set_style_pad_all(format_sd_row, 0, 0);
    lv_obj_set_style_pad_row(format_sd_row, 8, 0);
    lv_obj_set_style_pad_column(format_sd_row, 8, 0);
    lv_obj_set_flex_flow(format_sd_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(format_sd_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *format_sd_label = lv_label_create(format_sd_row);
    lv_label_set_text(format_sd_label, "Format SD Card");

    ui_settings_format_sd_btn = lv_btn_create(format_sd_row);
    lv_obj_set_height(ui_settings_format_sd_btn, 40);
    lv_obj_set_style_radius(ui_settings_format_sd_btn, 8, 0);
    lv_obj_set_style_bg_color(ui_settings_format_sd_btn, lv_palette_main(LV_PALETTE_RED), 0);
    lv_obj_t *format_btn_label = lv_label_create(ui_settings_format_sd_btn);
    lv_label_set_text(format_btn_label, "Format");
    lv_obj_center(format_btn_label);
    lv_obj_add_event_cb(ui_settings_format_sd_btn, ui_settings_format_sd_event, LV_EVENT_CLICKED, NULL);

    // Add flexible spacer to push version label to bottom
    lv_obj_t *spacer = lv_obj_create(ui_settings_screen);
    lv_obj_set_size(spacer, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(spacer, 1);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);
    lv_obj_clear_flag(spacer, LV_OBJ_FLAG_SCROLLABLE);
}

static void show_version_label()
{
    if (!ui_settings_screen)
    {
        return;
    }

    // Create version label in the flex container (after spacer)
    static lv_obj_t *version_label = NULL;
    if (!version_label)
    {
        version_label = lv_label_create(ui_settings_screen);

        // Convert __DATE__ (format "Mmm dd yyyy") to YYYYMMDD
        const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char compile_date[9];
        int month = 0;
        char mon_str[4];
        int day, year;
        sscanf(__DATE__, "%s %d %d", mon_str, &day, &year);
        for (int i = 0; i < 12; i++)
        {
            if (strcmp(mon_str, months[i]) == 0)
            {
                month = i + 1;
                break;
            }
        }
        snprintf(compile_date, sizeof(compile_date), "%04d%02d%02d", year, month, day);

        lv_label_set_text_fmt(version_label, "Version: %s (%s)", FIRMWARE_VERSION, compile_date);
        lv_obj_set_style_text_color(version_label, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_text_font(version_label, &lv_font_montserrat_12, 0);
        lv_obj_set_width(version_label, LV_PCT(100));
        lv_obj_set_style_text_align(version_label, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_bottom(version_label, 8, 0);
    }
}

lv_obj_t *ui_get_settings_screen(void)
{
    return ui_settings_screen;
}

void ui_settings_set_back_btn_enabled(bool enabled)
{
    if (!ui_settings_back_btn)
        return;
    if (enabled)
    {
        lv_obj_clear_state(ui_settings_back_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(ui_settings_back_btn, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_bg_opa(ui_settings_back_btn, LV_OPA_COVER, 0);
    }
    else
    {
        lv_obj_add_state(ui_settings_back_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(ui_settings_back_btn, lv_palette_main(LV_PALETTE_GREY), 0);
        lv_obj_set_style_bg_opa(ui_settings_back_btn, LV_OPA_40, 0);
    }
}

void ui_settings_show(void)
{
    build_settings_screen();
    show_version_label();
    ui_pause_camera_timer();
    lv_scr_load_anim(ui_settings_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
