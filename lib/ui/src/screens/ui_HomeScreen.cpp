#include <Arduino.h>
#include <stdio.h>
#include <stdlib.h>
#include <esp_camera.h>
#include <Preferences.h>
#include <USB.h>
#include <USBMSC.h>
#include <FS.h>
#include <SD.h>
#include "lvgl.h"
#include "../ui.h"
#include "ui.h"
#include "ui_SettingsScreen.h"
#include "ui_GalleryScreen.h"
#include "../../../src/utilities.h"
#include "../../../../include/filter.h"
#include "../../../../include/palettes.h"

#define EYE_COLOR_INACTIVE lv_color_white()
#define EYE_COLOR_ACTIVE lv_palette_main(LV_PALETTE_GREEN)

extern uint32_t ui_get_sd_free_mb();
extern uint16_t ui_get_battery_voltage();
extern bool ui_is_charging();
extern bool ui_is_usb_connected();

USBMSC msc;
SemaphoreHandle_t cam_mutex;

typedef enum
{
    CAMERA_FILTER_NONE = 0,
    CAMERA_FILTER_PIXELATE,
    CAMERA_FILTER_DITHER,
    CAMERA_FILTER_EDGE,
    CAMERA_FILTER_CRT
} camera_filter_t;

typedef struct
{
    const uint32_t *palette;
    int size;
} palette_option_t;

void ui_event_FlashSwitch(lv_event_t *e);
void ui_event_StorageSwitch(lv_event_t *e);
void ui_event_CameraCanvasTap(lv_event_t *e);

lv_obj_t *ui_HomeScreen = NULL;
lv_obj_t *ui_FilterDropdown = NULL;
lv_obj_t *ui_PaletteDropdown = NULL;
lv_obj_t *ui_Image1 = NULL;
lv_obj_t *ui_fps_label = NULL;
lv_obj_t *ui_camera_canvas = NULL;
lv_timer_t *camera_timer = NULL;
bool camera_get_photo_flag = false;
bool camera_led_open_flag = true;
lv_obj_t *ui_flash_switch = NULL;
lv_obj_t *ui_gallery_button = NULL;
lv_obj_t *ui_settings_button = NULL;
lv_obj_t *ui_camera_settings_button = NULL;

static lv_obj_t *ui_status_sd_label = NULL;
static lv_obj_t *ui_status_batt_label = NULL;
static lv_obj_t *ui_camera_settings_icon = NULL;
static lv_obj_t *ui_filter_column = NULL;
static lv_obj_t *ui_camera_settings_column = NULL;
static lv_obj_t *ui_DitherDropdown = NULL;
static lv_obj_t *ui_PixelSizeDropdown = NULL;
static lv_obj_t *ui_photo_overlay_label = NULL;
static lv_obj_t *ui_zoom_label = NULL;
static lv_obj_t *ui_agc_gain_slider = NULL;
static lv_obj_t *ui_aec_value_slider = NULL;

static lv_timer_t *status_timer = NULL;
static lv_timer_t *photo_overlay_timer = NULL;

static camera_filter_t current_filter = CAMERA_FILTER_NONE;
static bool camera_settings_visible = false;
static int current_dithering = 0;
static int current_pixel_size = 1;
static int current_zoom_level = 0;
static int current_palette_index = 0;

static Preferences ui_prefs;
static bool ui_prefs_ready = false;
static const char *UI_PREF_NAMESPACE = "ui_state";
static const char *UI_PREF_FILTER_KEY = "filter_mode";
static const char *UI_PREF_PALETTE_KEY = "palette_idx";
static const char *UI_PREF_FLASH_KEY = "flash_enabled";
static const char *UI_PREF_AEC2_KEY = "aec2_enabled";
static const char *UI_PREF_GAIN_CTRL_KEY = "gain_ctrl";
static const char *UI_PREF_AGC_GAIN_KEY = "agc_gain";
static const char *UI_PREF_EXPOSURE_CTRL_KEY = "exp_ctrl";
static const char *UI_PREF_AEC_VALUE_KEY = "aec_value";
static const char *UI_PREF_DITHER_KEY = "dither_type";
static const char *UI_PREF_PIXEL_SIZE_KEY = "pixel_size";
static const char *UI_PREF_AUTO_ADJUST_KEY = "auto_adjust";
static const char *UI_PREF_ZOOM_LEVEL_KEY = "zoom_level";
static const char *UI_PREF_SCREENSHOT_KEY = "screenshot_mode";

static uint8_t *camera_canvas_buf = NULL;
static size_t camera_canvas_buf_size = 0;

static const palette_option_t kPaletteOptions[] = {
    {PALETTE_SUNSET, PALETTE_SUNSET_SIZE},
    {PALETTE_YELLOW_BROWN, PALETTE_YELLOW_BROWN_SIZE},
    {PALETTE_GRAYSCALE, PALETTE_GRAYSCALE_SIZE},
    {PALETTE_GAMEBOY, PALETTE_GAMEBOY_SIZE},
    {PALETTE_CYBERPUNK, PALETTE_CYBERPUNK_SIZE},
    {PALETTE_AUTUMN, PALETTE_AUTUMN_SIZE},
    {PALETTE_OCEAN, PALETTE_OCEAN_SIZE},
    {PALETTE_DESERT, PALETTE_DESERT_SIZE},
    {PALETTE_SAKURA, PALETTE_SAKURA_SIZE},
    {PALETTE_MINT, PALETTE_MINT_SIZE},
    {PALETTE_FIRE, PALETTE_FIRE_SIZE},
    {PALETTE_ARCTIC, PALETTE_ARCTIC_SIZE},
    {PALETTE_SEPIA, PALETTE_SEPIA_SIZE},
    {PALETTE_NEON, PALETTE_NEON_SIZE},
    {PALETTE_BW, PALETTE_BW_SIZE},
    {PALETTE_4COLOR, PALETTE_4COLOR_SIZE},
    {PALETTE_16COLOR, PALETTE_16COLOR_SIZE},
    {PALETTE_FRESTA, PALETTE_FRESTA_SIZE},
};
static const uint8_t kPaletteOptionCount = sizeof(kPaletteOptions) / sizeof(kPaletteOptions[0]);

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize)
{
    uint32_t secSize = SD.sectorSize();
    if (!secSize)
    {
        return false; // disk error
    }
    log_v("Write lba: %ld\toffset: %ld\tbufsize: %ld", lba, offset, bufsize);
    for (int x = 0; x < bufsize / secSize; x++)
    {
        uint8_t blkbuffer[secSize];
        memcpy(blkbuffer, (uint8_t *)buffer + secSize * x, secSize);
        if (!SD.writeRAW(blkbuffer, lba + x))
        {
            return false;
        }
    }
    return bufsize;
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize)
{
    uint32_t secSize = SD.sectorSize();
    if (!secSize)
    {
        return false; // disk error
    }
    log_v("Read lba: %ld\toffset: %ld\tbufsize: %ld\tsector: %lu", lba, offset, bufsize, secSize);
    for (int x = 0; x < bufsize / secSize; x++)
    {
        if (!SD.readRAW((uint8_t *)buffer + (x * secSize), lba + x))
        {
            return false; // outside of volume boundary
        }
    }
    return bufsize;
}

static bool onStartStop(uint8_t power_condition, bool start, bool load_eject)
{
    log_i("Start/Stop power: %u\tstart: %d\teject: %d", power_condition, start, load_eject);
    return true;
}

static void usbEventCallback(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == ARDUINO_USB_EVENTS)
    {
        arduino_usb_event_data_t *data = (arduino_usb_event_data_t *)event_data;
        switch (event_id)
        {
        case ARDUINO_USB_STARTED_EVENT:
            Serial.println("USB PLUGGED");
            break;
        case ARDUINO_USB_STOPPED_EVENT:
            Serial.println("USB UNPLUGGED");
            break;
        case ARDUINO_USB_SUSPEND_EVENT:
            Serial.printf("USB SUSPENDED: remote_wakeup_en: %u\n", data->suspend.remote_wakeup_en);
            break;
        case ARDUINO_USB_RESUME_EVENT:
            Serial.println("USB RESUMED");
            break;

        default:
            break;
        }
    }
}

static void status_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    // Update SD card free space
    uint32_t sd_free = ui_get_sd_free_mb();
    if (sd_free > 0)
    {
        if (sd_free >= 1024)
        {
            lv_label_set_text_fmt(ui_status_sd_label, LV_SYMBOL_SD_CARD " %.1fGB", sd_free / 1024.0f);
        }
        else
        {
            lv_label_set_text_fmt(ui_status_sd_label, LV_SYMBOL_SD_CARD " %luMB", (unsigned long)sd_free);
        }
    }
    else
    {
        lv_label_set_text(ui_status_sd_label, LV_SYMBOL_SD_CARD " --");
    }

    // Update battery status
    uint16_t batt_mv = ui_get_battery_voltage();
    bool charging = ui_is_charging();
    bool usb = ui_is_usb_connected();

    // Calculate approximate percentage (3.3V = 0%, 4.2V = 100%)
    int pct = 0;
    if (batt_mv > 3300)
    {
        pct = (batt_mv - 3300) * 100 / 900;
        if (pct > 100)
            pct = 100;
        if (pct < 0)
            pct = 0;
    }

    const char *icon = LV_SYMBOL_BATTERY_FULL;
    if (charging)
    {
        icon = LV_SYMBOL_CHARGE;
    }
    else if (pct <= 10)
    {
        icon = LV_SYMBOL_BATTERY_EMPTY;
    }
    else if (pct <= 30)
    {
        icon = LV_SYMBOL_BATTERY_1;
    }
    else if (pct <= 60)
    {
        icon = LV_SYMBOL_BATTERY_2;
    }
    else if (pct <= 80)
    {
        icon = LV_SYMBOL_BATTERY_3;
    }

    if (usb && !charging)
    {
        lv_label_set_text_fmt(ui_status_batt_label, "%s %d%% " LV_SYMBOL_USB, icon, pct);
    }
    else
    {
        lv_label_set_text_fmt(ui_status_batt_label, "%s %d%%", icon, pct);
    }
}


static void photo_overlay_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    if (photo_overlay_timer)
    {
        lv_timer_del(photo_overlay_timer);
        photo_overlay_timer = NULL;
    }
    if (ui_photo_overlay_label)
    {
        lv_label_set_text(ui_photo_overlay_label, "");
        lv_obj_add_flag(ui_photo_overlay_label, LV_OBJ_FLAG_HIDDEN);
    }
}

void ui_show_photo_overlay(const char *text)
{
    if (!ui_photo_overlay_label)
    {
        return;
    }

    if (photo_overlay_timer)
    {
        lv_timer_del(photo_overlay_timer);
        photo_overlay_timer = NULL;
    }

    lv_label_set_text(ui_photo_overlay_label, text ? text : "");
    lv_obj_clear_flag(ui_photo_overlay_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(ui_photo_overlay_label, LV_ALIGN_TOP_MID, 0, 4);
    photo_overlay_timer = lv_timer_create(photo_overlay_timer_cb, 1200, NULL);
}


void camera_set_safe(void (*fn)(sensor_t *s))
{
    // Pause streaming to avoid SCCB bus contention
    ui_pause_camera_timer();

    // Wait for current timer callback to finish
    vTaskDelay(pdMS_TO_TICKS(100));

    // Drain all queued frame buffers from DMA
    camera_fb_t *fb;
    while ((fb = esp_camera_fb_get()) != NULL)
    {
        esp_camera_fb_return(fb);
    }

    // OV3660 needs quiet time after DMA stops
    vTaskDelay(pdMS_TO_TICKS(200));

    sensor_t *s = esp_camera_sensor_get();
    if (s)
    {
        fn(s);
    }

    vTaskDelay(pdMS_TO_TICKS(50));

    ui_resume_camera_timer();
}


static inline int clamp_palette_index(int idx)
{
    if (idx < 0 || idx >= kPaletteOptionCount)
    {
        return 0;
    }
    return idx;
}

static inline int clamp_dither_type(int v)
{
    if (v < 0 || v > 2)
    {
        return 0;
    }
    return v;
}

static inline int clamp_pixel_size(int v)
{
    switch (v)
    {
    case 1:
    case 2:
    case 4:
    case 8:
        return v;
    default:
        return 1;
    }
}

static inline int pixel_size_to_index(int v)
{
    switch (v)
    {
    case 1:
        return 0;
    case 2:
        return 1;
    case 4:
        return 2;
    case 8:
        return 3;
    default:
        return 0;
    }
}

static const uint32_t *get_current_palette(int &out_size)
{
    current_palette_index = clamp_palette_index(current_palette_index);
    out_size = kPaletteOptions[current_palette_index].size;
    return kPaletteOptions[current_palette_index].palette;
}


static inline uint16_t swap_rgb565_bytes(uint16_t px)
{
    return (px >> 8) | (px << 8);
}

static bool ensure_camera_canvas_buffer(size_t bytes)
{
    if (camera_canvas_buf_size >= bytes)
    {
        return true;
    }

    uint8_t *new_buf = (uint8_t *)realloc(camera_canvas_buf, bytes);
    if (!new_buf)
    {
        return false;
    }

    camera_canvas_buf = new_buf;
    camera_canvas_buf_size = bytes;
    return true;
}

static void rotate_frame_90_clockwise(uint16_t *dst, const uint16_t *src, size_t width, size_t height)
{
    for (size_t y = 0; y < height; y++)
    {
        for (size_t x = 0; x < width; x++)
        {
            size_t src_idx = y * width + x;
            size_t dst_idx = x * height + (height - 1 - y);
            dst[dst_idx] = swap_rgb565_bytes(src[src_idx]);
        }
    }
}

static void copy_frame(uint16_t *dst, const uint16_t *src, size_t pixel_count)
{
    for (size_t i = 0; i < pixel_count; i++)
    {
        dst[i] = swap_rgb565_bytes(src[i]);
    }
}

static void px_swap(uint8_t *a, uint8_t *b)
{
    uint8_t c = *a;
    *a = *b;
    *b = c;
}

static void apply_selected_filter(camera_fb_t *frame)
{
    if (ui_get_auto_adjust_enabled())
    {
        applyAutoAdjust(frame);
    }

    switch (current_filter)
    {
    case CAMERA_FILTER_PIXELATE:
        applyPixelate(frame, current_pixel_size, false);
        break;
    case CAMERA_FILTER_DITHER:
    {
        int palette_size = 0;
        const uint32_t *palette = get_current_palette(palette_size);
        applyColorPalette((uint16_t *)frame->buf, frame->width, frame->height, palette, palette_size, current_dithering, current_pixel_size, 2);
    }
    break;
    case CAMERA_FILTER_EDGE:
        applyEdgeDetection(frame, 1);
        break;
    case CAMERA_FILTER_CRT:
        applyCRT(frame, current_pixel_size);
        break;
    case CAMERA_FILTER_NONE:
    default:
        break;
    }
}

void ui_set_filter_mode(int mode)
{
    if (mode < CAMERA_FILTER_NONE || mode > CAMERA_FILTER_CRT)
    {
        mode = CAMERA_FILTER_NONE;
    }
    current_filter = (camera_filter_t)mode;
    if (ui_prefs_ready)
    {
        ui_prefs.putInt(UI_PREF_FILTER_KEY, current_filter);
    }
}

int ui_get_filter_mode(void)
{
    return static_cast<int>(current_filter);
}

int ui_get_dither_type(void)
{
    return current_dithering;
}

int ui_get_pixel_size(void)
{
    return current_pixel_size;
}

void ui_get_palette(const uint32_t **palette, int *size)
{
    if (!palette || !size)
    {
        return;
    }
    *size = 0;
    *palette = get_current_palette(*size);
}

void ui_set_palette_index(int idx)
{
    current_palette_index = clamp_palette_index(idx);
    if (ui_prefs_ready)
    {
        ui_prefs.putInt(UI_PREF_PALETTE_KEY, current_palette_index);
    }
}

void ui_set_flash_enabled(bool enabled)
{
    camera_led_open_flag = enabled;
    if (ui_flash_switch)
    {
        if (enabled)
        {
            lv_obj_add_state(ui_flash_switch, LV_STATE_CHECKED);
        }
        else
        {
            lv_obj_clear_state(ui_flash_switch, LV_STATE_CHECKED);
        }
    }
    if (ui_prefs_ready)
    {
        ui_prefs.putBool(UI_PREF_FLASH_KEY, enabled);
    }
}

bool ui_is_flash_enabled(void)
{
    return camera_led_open_flag;
}


void ui_pause_camera_timer(void)
{
    if (camera_timer)
    {
        lv_timer_pause(camera_timer);
    }
}

void ui_resume_camera_timer(void)
{
    if (camera_timer)
    {
        lv_timer_resume(camera_timer);
    }
}

bool ui_get_aec2_enabled(void)
{
    Preferences prefs;
    if (prefs.begin(UI_PREF_NAMESPACE, true))
    {
        bool val = prefs.getBool(UI_PREF_AEC2_KEY, false);
        prefs.end();
        return val;
    }
    return false;
}

bool ui_get_gain_ctrl_enabled(void)
{
    Preferences prefs;
    if (prefs.begin(UI_PREF_NAMESPACE, true))
    {
        bool val = prefs.getBool(UI_PREF_GAIN_CTRL_KEY, true); // Default enabled (AGC on)
        prefs.end();
        return val;
    }
    return true;
}

int ui_get_agc_gain(void)
{
    Preferences prefs;
    if (prefs.begin(UI_PREF_NAMESPACE, true))
    {
        int val = prefs.getInt(UI_PREF_AGC_GAIN_KEY, 15);
        prefs.end();
        return val;
    }
    return 15;
}

bool ui_get_exposure_ctrl_enabled(void)
{
    Preferences prefs;
    if (prefs.begin(UI_PREF_NAMESPACE, true))
    {
        bool val = prefs.getBool(UI_PREF_EXPOSURE_CTRL_KEY, true); // Default enabled (AEC on)
        prefs.end();
        return val;
    }
    return true;
}

int ui_get_aec_value(void)
{
    Preferences prefs;
    if (prefs.begin(UI_PREF_NAMESPACE, true))
    {
        int val = prefs.getInt(UI_PREF_AEC_VALUE_KEY, 800);
        prefs.end();
        return val;
    }
    return 800;
}

bool ui_get_auto_adjust_enabled(void)
{
    Preferences prefs;
    if (prefs.begin(UI_PREF_NAMESPACE, true))
    {
        bool val = prefs.getBool(UI_PREF_AUTO_ADJUST_KEY, false);
        prefs.end();
        return val;
    }
    return false;
}

void ui_set_auto_adjust_enabled(bool enabled)
{
    if (ui_prefs_ready)
    {
        ui_prefs.putBool(UI_PREF_AUTO_ADJUST_KEY, enabled);
    }
}

int ui_get_zoom_level(void)
{
    return current_zoom_level;
}

void ui_set_zoom_level(int level)
{
    if (level < 0 || level > 2)
    {
        level = 0;
    }
    
    current_zoom_level = level;
    
    if (ui_zoom_label)
    {
        const char* zoom_text = "";
        switch (level)
        {
            case 0: zoom_text = "1x"; break;
            case 1: zoom_text = "2x"; break;
            case 2: zoom_text = "4x"; break;
        }
        lv_label_set_text(ui_zoom_label, zoom_text);
    }
    
    if (ui_prefs_ready)
    {
        ui_prefs.putInt(UI_PREF_ZOOM_LEVEL_KEY, current_zoom_level);
    }
}

bool ui_get_screenshot_mode_enabled(void)
{
    Preferences prefs;
    if (prefs.begin(UI_PREF_NAMESPACE, true))
    {
        bool enabled = prefs.getBool(UI_PREF_SCREENSHOT_KEY, false);
        prefs.end();
        return enabled;
    }
    return false;
}

void ui_set_screenshot_mode_enabled(bool enabled)
{
    if (ui_prefs_ready)
    {
        ui_prefs.putBool(UI_PREF_SCREENSHOT_KEY, enabled);
    }
}

static void camera_video_play(lv_timer_t *t)
{
    static uint32_t last_fps_tick = 0;
    static uint16_t frame_counter = 0;

    camera_fb_t *frame = esp_camera_fb_get();
    if (frame)
    {

        apply_selected_filter(frame);

        if (!ensure_camera_canvas_buffer(frame->len))
        {
            esp_camera_fb_return(frame);
            return;
        }

        uint16_t *dst_pixels = (uint16_t *)camera_canvas_buf;
        const uint16_t *src_pixels = (const uint16_t *)frame->buf;
        
        int target_width = 240;
        int target_height = 176;
        
        if (current_zoom_level == 0)
        {
            size_t pixel_count = frame->len / 2;
            copy_frame(dst_pixels, src_pixels, pixel_count);
        }
        else
        {
            int crop_width, crop_height;
            
            if (current_zoom_level == 1)
            {
                crop_width = target_width / 2;
                crop_height = target_height / 2;
            }
            else
            {
                crop_width = target_width / 4;
                crop_height = target_height / 4;
            }
            
            int start_x = (frame->width - crop_width) / 2;
            int start_y = (frame->height - crop_height) / 2;
            
            for (int y = 0; y < target_height; y++)
            {
                for (int x = 0; x < target_width; x++)
                {
                    int src_x = start_x + (x * crop_width / target_width);
                    int src_y = start_y + (y * crop_height / target_height);
                    
                    if (src_x >= frame->width) src_x = frame->width - 1;
                    if (src_y >= frame->height) src_y = frame->height - 1;
                    
                    int src_idx = src_y * frame->width + src_x;
                    int dst_idx = y * target_width + x;
                    dst_pixels[dst_idx] = swap_rgb565_bytes(src_pixels[src_idx]);
                }
            }
        }
        
        lv_canvas_set_buffer(ui_camera_canvas, camera_canvas_buf, target_width, target_height, LV_IMG_CF_TRUE_COLOR);

        if (camera_get_photo_flag)
        {
            camera_get_photo_flag = false;
        }
        frame_counter++;

        uint32_t now = lv_tick_get();
        if (now - last_fps_tick >= 1000 && ui_fps_label)
        {
            uint32_t elapsed = now - last_fps_tick;
            uint32_t fps = (frame_counter * 1000) / (elapsed ? elapsed : 1);
            lv_label_set_text_fmt(ui_fps_label, "%lu FPS", static_cast<unsigned long>(fps));
            last_fps_tick = now;
            frame_counter = 0;
        }

        esp_camera_fb_return(frame);
    }
}

void ui_event_CameraCanvasTap(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);
    if (event_code != LV_EVENT_CLICKED)
    {
        return;
    }
    
    int next_zoom = (current_zoom_level + 1) % 3;
    ui_set_zoom_level(next_zoom);
    
    char zoom_text[8];
    snprintf(zoom_text, sizeof(zoom_text), "%s", next_zoom == 0 ? "1x" : (next_zoom == 1 ? "2x" : "4x"));
    ui_show_photo_overlay(zoom_text);
}

static void ui_event_SettingsButton(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    ui_settings_show();
}

static void ui_event_CameraSettingsButton(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
        return;

    camera_settings_visible = !camera_settings_visible;

    if (camera_settings_visible)
    {
        lv_obj_add_flag(ui_filter_column, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(ui_camera_settings_column, LV_OBJ_FLAG_HIDDEN);

        /* Eye active color */
        lv_obj_set_style_text_color(ui_camera_settings_icon,
                                    EYE_COLOR_ACTIVE, 0);
    }
    else
    {
        lv_obj_clear_flag(ui_filter_column, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(ui_camera_settings_column, LV_OBJ_FLAG_HIDDEN);

        /* Eye inactive color */
        lv_obj_set_style_text_color(ui_camera_settings_icon,
                                    EYE_COLOR_INACTIVE, 0);
    }
}
static bool aec2_pending_value = false;

static void set_aec2_callback(sensor_t *s)
{
    if (s)
    {
        s->set_aec2(s, aec2_pending_value ? 1 : 0);
    }
}

static void ui_event_AEC2Switch(lv_event_t *e)
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
    if (ui_prefs_ready)
    {
        ui_prefs.putBool(UI_PREF_AEC2_KEY, enabled);
    }
    ESP.restart();
}

static void ui_event_GainCtrlSwitch(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
        return;
    lv_obj_t *target = lv_event_get_target(e);
    if (!target)
        return;
    bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    if (ui_prefs_ready)
    {
        ui_prefs.putBool(UI_PREF_GAIN_CTRL_KEY, enabled);
    }
    if (ui_agc_gain_slider)
    {
        if (enabled)
            lv_obj_add_state(ui_agc_gain_slider, LV_STATE_DISABLED);
        else
            lv_obj_clear_state(ui_agc_gain_slider, LV_STATE_DISABLED);
    }
    ESP.restart();
}

static void ui_event_AgcGainSlider(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
        return;
    // Don't restart if AGC is enabled (slider is just for display)
    if (ui_prefs_ready && ui_prefs.getBool(UI_PREF_GAIN_CTRL_KEY, true))
        return;
    lv_obj_t *target = lv_event_get_target(e);
    if (!target)
        return;
    int value = lv_slider_get_value(target);
    if (ui_prefs_ready)
    {
        ui_prefs.putInt(UI_PREF_AGC_GAIN_KEY, value);
    }
    ESP.restart();
}

static void ui_event_ExposureCtrlSwitch(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
        return;
    lv_obj_t *target = lv_event_get_target(e);
    if (!target)
        return;
    bool enabled = lv_obj_has_state(target, LV_STATE_CHECKED);
    if (ui_prefs_ready)
    {
        ui_prefs.putBool(UI_PREF_EXPOSURE_CTRL_KEY, enabled);
    }
    if (ui_aec_value_slider)
    {
        if (enabled)
            lv_obj_add_state(ui_aec_value_slider, LV_STATE_DISABLED);
        else
            lv_obj_clear_state(ui_aec_value_slider, LV_STATE_DISABLED);
    }
    ESP.restart();
}

static void ui_event_AecValueSlider(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
        return;
    // Don't restart if AEC is enabled (slider is just for display)
    if (ui_prefs_ready && ui_prefs.getBool(UI_PREF_EXPOSURE_CTRL_KEY, true))
        return;
    lv_obj_t *target = lv_event_get_target(e);
    if (!target)
        return;
    int value = lv_slider_get_value(target);
    if (ui_prefs_ready)
    {
        ui_prefs.putInt(UI_PREF_AEC_VALUE_KEY, value);
    }
    ESP.restart();
}

static void ui_event_GalleryButton(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    ui_gallery_show();
}

// event funtions
void ui_event_Dropdown1(lv_event_t *e)
{
    lv_event_code_t event_code = lv_event_get_code(e);

    if (event_code == LV_EVENT_VALUE_CHANGED)
    {
        changeState(e);
    }
}

void ui_event_PaletteDropdown(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }

    lv_obj_t *dropdown = lv_event_get_target(e);
    if (!dropdown)
    {
        return;
    }

    uint16_t selected_value = lv_dropdown_get_selected(dropdown);
    ui_set_palette_index(static_cast<int>(selected_value));
}

static void ui_event_DitherDropdown(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }
    lv_obj_t *dropdown = lv_event_get_target(e);
    if (!dropdown)
    {
        return;
    }
    int selected = static_cast<int>(lv_dropdown_get_selected(dropdown));
    current_dithering = clamp_dither_type(selected);
    if (ui_prefs_ready)
    {
        ui_prefs.putInt(UI_PREF_DITHER_KEY, current_dithering);
    }
}

static void ui_event_PixelSizeDropdown(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_VALUE_CHANGED)
    {
        return;
    }
    lv_obj_t *dropdown = lv_event_get_target(e);
    if (!dropdown)
    {
        return;
    }
    uint16_t sel = lv_dropdown_get_selected(dropdown);
    // Options order must match creation list
    const int kPixelOptions[] = {1, 2, 4, 8};
    if (sel >= (sizeof(kPixelOptions) / sizeof(kPixelOptions[0])))
        sel = 0;
    current_pixel_size = clamp_pixel_size(kPixelOptions[sel]);
    if (ui_prefs_ready)
    {
        ui_prefs.putInt(UI_PREF_PIXEL_SIZE_KEY, current_pixel_size);
    }
}

void ui_event_FlashSwitch(lv_event_t *e)
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
    ui_set_flash_enabled(enabled);
}

void ui_event_StorageSwitch(lv_event_t *e)
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

    if (enabled)
    {
        msc.vendorID("ESP32");
        msc.productID("USB_MSC");
        msc.productRevision("1.0");
        msc.onRead(onRead);
        msc.onWrite(onWrite);
        msc.onStartStop(onStartStop);
        msc.mediaPresent(true);
        msc.begin(SD.numSectors(), SD.sectorSize());
        USB.begin();
        USB.onEvent(usbEventCallback);
    }
    else
    {
        ESP.restart();
    }
}

// build funtions

void ui_HomeScreen_screen_init(void)
{
    if (cam_mutex == NULL)
    {
        cam_mutex = xSemaphoreCreateMutex();
    }

    ui_HomeScreen = lv_obj_create(NULL);
    lv_obj_clear_flag(ui_HomeScreen, LV_OBJ_FLAG_SCROLLABLE); /// Flags

    if (!ui_prefs_ready)
    {
        ui_prefs_ready = ui_prefs.begin(UI_PREF_NAMESPACE, false);
        if (ui_prefs_ready)
        {
            int stored_filter = ui_prefs.getInt(UI_PREF_FILTER_KEY, current_filter);
            current_filter = (camera_filter_t)stored_filter;
            current_palette_index = clamp_palette_index(ui_prefs.getInt(UI_PREF_PALETTE_KEY, current_palette_index));
            current_dithering = clamp_dither_type(ui_prefs.getInt(UI_PREF_DITHER_KEY, current_dithering));
            current_pixel_size = clamp_pixel_size(ui_prefs.getInt(UI_PREF_PIXEL_SIZE_KEY, current_pixel_size));
            camera_led_open_flag = ui_prefs.getBool(UI_PREF_FLASH_KEY, camera_led_open_flag);
            current_zoom_level = ui_prefs.getInt(UI_PREF_ZOOM_LEVEL_KEY, 0); // Default to 1x zoom
        }
    }

    // Status bar row (SD card + battery)
    lv_obj_t *ui_status_row = lv_obj_create(ui_HomeScreen);
    lv_obj_set_width(ui_status_row, LV_PCT(100));
    lv_obj_set_height(ui_status_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(ui_status_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_status_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_status_row, 0, 0);
    lv_obj_set_style_pad_all(ui_status_row, 0, 0);
    lv_obj_set_style_pad_column(ui_status_row, 8, 0);
    lv_obj_set_style_pad_row(ui_status_row, 8, 0);
    lv_obj_set_flex_flow(ui_status_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_status_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    ui_camera_canvas = lv_canvas_create(ui_HomeScreen);
    lv_obj_set_width(ui_camera_canvas, 222);
    lv_obj_set_height(ui_camera_canvas, 176);
    lv_obj_set_x(ui_camera_canvas, 0);
    lv_obj_set_y(ui_camera_canvas, 14);
    lv_obj_set_align(ui_camera_canvas, LV_ALIGN_TOP_LEFT);
    lv_obj_add_flag(ui_camera_canvas, LV_OBJ_FLAG_CLICKABLE);    /// Make it clickable
    lv_obj_clear_flag(ui_camera_canvas, LV_OBJ_FLAG_SCROLLABLE); /// Flags
    lv_obj_add_event_cb(ui_camera_canvas, ui_event_CameraCanvasTap, LV_EVENT_CLICKED, NULL);

    // Overlay label for photo taken feedback
    ui_photo_overlay_label = lv_label_create(ui_camera_canvas);
    lv_label_set_text(ui_photo_overlay_label, "");
    lv_obj_set_style_bg_opa(ui_photo_overlay_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(ui_photo_overlay_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui_photo_overlay_label, &lv_font_montserrat_18, 0);
    lv_obj_align(ui_photo_overlay_label, LV_ALIGN_TOP_MID, 0, 4);
    lv_obj_add_flag(ui_photo_overlay_label, LV_OBJ_FLAG_HIDDEN);

    ui_fps_label = lv_label_create(ui_camera_canvas);
    lv_label_set_text(ui_fps_label, "-- FPS");
    lv_obj_set_style_bg_opa(ui_fps_label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_text_color(ui_fps_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui_fps_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_pad_all(ui_fps_label, 4, 0);
    lv_obj_set_style_radius(ui_fps_label, 4, 0);
    lv_obj_align(ui_fps_label, LV_ALIGN_BOTTOM_LEFT, 6, -4);
    
    // Zoom level indicator
    ui_zoom_label = lv_label_create(ui_camera_canvas);
    lv_obj_set_style_bg_color(ui_zoom_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ui_zoom_label, LV_OPA_50, 0);
    lv_obj_set_style_text_color(ui_zoom_label, lv_color_white(), 0);
    lv_obj_set_style_text_font(ui_zoom_label, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(ui_zoom_label, 4, 0);
    lv_obj_set_style_radius(ui_zoom_label, 4, 0);
    lv_obj_align(ui_zoom_label, LV_ALIGN_BOTTOM_RIGHT, -6, -4);
    
    // Set initial zoom indicator text based on saved preference
    const char* zoom_text = "1x";
    switch (current_zoom_level) {
        case 0: zoom_text = "1x"; break;
        case 1: zoom_text = "2x"; break;
        case 2: zoom_text = "4x"; break;
    }
    lv_label_set_text(ui_zoom_label, zoom_text);

    camera_timer = lv_timer_create(camera_video_play, 50, NULL);
    lv_timer_ready(camera_timer);

    lv_obj_t *ui_bottom_panel = lv_obj_create(ui_HomeScreen);
    lv_obj_set_size(ui_bottom_panel, 222, 480 - 176 - 14);
    lv_obj_set_x(ui_bottom_panel, 0);
    lv_obj_set_y(ui_bottom_panel, 176 + 14);
    lv_obj_set_align(ui_bottom_panel, LV_ALIGN_TOP_LEFT);
    lv_obj_clear_flag(ui_bottom_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_bottom_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_bottom_panel, 0, 0);
    lv_obj_set_style_pad_all(ui_bottom_panel, 8, 0);
    lv_obj_set_style_pad_row(ui_bottom_panel, 8, 0);
    lv_obj_set_flex_flow(ui_bottom_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(ui_bottom_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);

    ui_status_sd_label = lv_label_create(ui_status_row);
    lv_label_set_text(ui_status_sd_label, LV_SYMBOL_SD_CARD " --");
    lv_obj_set_style_text_font(ui_status_sd_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui_status_sd_label, lv_color_white(), 0);

    ui_status_batt_label = lv_label_create(ui_status_row);
    lv_label_set_text(ui_status_batt_label, LV_SYMBOL_BATTERY_FULL " --");
    lv_obj_set_style_text_font(ui_status_batt_label, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(ui_status_batt_label, lv_color_white(), 0);

    ui_filter_column = lv_obj_create(ui_bottom_panel);
    lv_obj_set_width(ui_filter_column, LV_PCT(100));
    lv_obj_set_height(ui_filter_column, LV_SIZE_CONTENT);
    lv_obj_clear_flag(ui_filter_column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_filter_column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_filter_column, 0, 0);

    /* Padding */
    lv_obj_set_style_pad_all(ui_filter_column, 0, 0);
    lv_obj_set_style_pad_row(ui_filter_column, 8, 0); // vertical spacing between items

    /* Flex column */
    lv_obj_set_flex_flow(ui_filter_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        ui_filter_column,
        LV_FLEX_ALIGN_START, // main axis (top)
        LV_FLEX_ALIGN_START, // cross axis (left)
        LV_FLEX_ALIGN_CENTER);

    /* Filter dropdown */
    ui_FilterDropdown = lv_dropdown_create(ui_filter_column);
    lv_obj_set_width(ui_FilterDropdown, LV_PCT(100));
    lv_obj_set_height(ui_FilterDropdown, 42);
    lv_obj_add_flag(ui_FilterDropdown, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(ui_FilterDropdown, ui_event_Dropdown1, LV_EVENT_ALL, NULL);
    lv_obj_set_style_pad_ver(ui_FilterDropdown, 10, LV_PART_MAIN);

    lv_dropdown_set_options_static(
        ui_FilterDropdown,
        "No filter\nPixelate\nDithering\nEdge detect\nCRT");
    lv_dropdown_set_selected(ui_FilterDropdown, current_filter);

    /* Palette dropdown */
    ui_PaletteDropdown = lv_dropdown_create(ui_filter_column);
    lv_obj_set_width(ui_PaletteDropdown, LV_PCT(100));
    lv_obj_set_height(ui_PaletteDropdown, 42);
    lv_obj_add_flag(ui_PaletteDropdown, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(ui_PaletteDropdown, ui_event_PaletteDropdown, LV_EVENT_ALL, NULL);
    lv_obj_set_style_pad_ver(ui_PaletteDropdown, 10, LV_PART_MAIN);

    lv_dropdown_set_options_static(
        ui_PaletteDropdown,
        "Sunset\n"
        "Yellow/Brown\n"
        "Grayscale\n"
        "Game Boy\n"
        "Cyberpunk\n"
        "Autumn\n"
        "Ocean\n"
        "Desert\n"
        "Sakura\n"
        "Mint\n"
        "Fire\n"
        "Arctic\n"
        "Sepia\n"
        "Neon\n"
        "Black & White\n"
        "CGA 4-color\n"
        "VGA 16-color\n"
        "Fresta");
    lv_dropdown_set_selected(ui_PaletteDropdown, current_palette_index);

    /* Dithering dropdown */
    ui_DitherDropdown = lv_dropdown_create(ui_filter_column);
    lv_obj_set_width(ui_DitherDropdown, LV_PCT(100));
    lv_obj_set_height(ui_DitherDropdown, 42);
    lv_obj_add_flag(ui_DitherDropdown, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(ui_DitherDropdown, ui_event_DitherDropdown, LV_EVENT_ALL, NULL);
    lv_obj_set_style_pad_ver(ui_DitherDropdown, 10, LV_PART_MAIN);
    lv_dropdown_set_options_static(
        ui_DitherDropdown,
        "Off\n"
        "Floyd-Steinberg\n"
        "Bayer");
    lv_dropdown_set_selected(ui_DitherDropdown, current_dithering);

    /* Pixel size dropdown */
    ui_PixelSizeDropdown = lv_dropdown_create(ui_filter_column);
    lv_obj_set_width(ui_PixelSizeDropdown, LV_PCT(100));
    lv_obj_set_height(ui_PixelSizeDropdown, 42);
    lv_obj_add_flag(ui_PixelSizeDropdown, LV_OBJ_FLAG_SCROLL_ON_FOCUS);
    lv_obj_add_event_cb(ui_PixelSizeDropdown, ui_event_PixelSizeDropdown, LV_EVENT_ALL, NULL);
    lv_obj_set_style_pad_ver(ui_PixelSizeDropdown, 10, LV_PART_MAIN);
    lv_dropdown_set_options_static(
        ui_PixelSizeDropdown,
        "1\n"
        "2x2\n"
        "4x4\n"
        "8x8");
    lv_dropdown_set_selected(ui_PixelSizeDropdown, pixel_size_to_index(current_pixel_size));

    /* Camera settings column (hidden by default) */
    ui_camera_settings_column = lv_obj_create(ui_bottom_panel);
    lv_obj_set_width(ui_camera_settings_column, LV_PCT(100));
    lv_obj_set_height(ui_camera_settings_column, LV_SIZE_CONTENT);
    lv_obj_clear_flag(ui_camera_settings_column, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_camera_settings_column, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_camera_settings_column, 0, 0);

    lv_obj_set_style_pad_all(ui_camera_settings_column, 0, 0);
    lv_obj_set_style_pad_row(ui_camera_settings_column, 8, 0);

    lv_obj_set_flex_flow(ui_camera_settings_column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(
        ui_camera_settings_column,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_START,
        LV_FLEX_ALIGN_CENTER);

    /* Example camera settings */
    lv_obj_t *camera_setting_flip_row = lv_obj_create(ui_camera_settings_column);
    lv_obj_set_width(camera_setting_flip_row, LV_PCT(100));
    lv_obj_set_height(camera_setting_flip_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(camera_setting_flip_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(camera_setting_flip_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(camera_setting_flip_row, 0, 0);
    lv_obj_set_style_pad_all(camera_setting_flip_row, 0, 0);
    lv_obj_set_style_pad_row(camera_setting_flip_row, 8, 0);
    lv_obj_set_style_pad_column(camera_setting_flip_row, 8, 0);
    lv_obj_set_flex_flow(camera_setting_flip_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(camera_setting_flip_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *setting_label = lv_label_create(camera_setting_flip_row);
    lv_label_set_text(setting_label, "AEC2");

    lv_obj_t *ui_settings_switch = lv_switch_create(camera_setting_flip_row);
    lv_obj_add_event_cb(ui_settings_switch, ui_event_AEC2Switch, LV_EVENT_ALL, NULL);
    // Initialize switch state from preferences
    if (ui_prefs_ready && ui_prefs.getBool(UI_PREF_AEC2_KEY, false))
    {
        lv_obj_add_state(ui_settings_switch, LV_STATE_CHECKED);
    }

    // --- Gain Control (AGC) Switch ---
    lv_obj_t *gain_ctrl_row = lv_obj_create(ui_camera_settings_column);
    lv_obj_set_width(gain_ctrl_row, LV_PCT(100));
    lv_obj_set_height(gain_ctrl_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(gain_ctrl_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(gain_ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(gain_ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(gain_ctrl_row, 0, 0);
    lv_obj_set_flex_flow(gain_ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gain_ctrl_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *gain_ctrl_label = lv_label_create(gain_ctrl_row);
    lv_label_set_text(gain_ctrl_label, "AGC");

    lv_obj_t *gain_ctrl_switch = lv_switch_create(gain_ctrl_row);
    lv_obj_add_event_cb(gain_ctrl_switch, ui_event_GainCtrlSwitch, LV_EVENT_ALL, NULL);
    if (ui_prefs_ready && ui_prefs.getBool(UI_PREF_GAIN_CTRL_KEY, true))
    {
        lv_obj_add_state(gain_ctrl_switch, LV_STATE_CHECKED);
    }

    // --- AGC Gain Slider ---
    lv_obj_t *agc_gain_row = lv_obj_create(ui_camera_settings_column);
    lv_obj_set_width(agc_gain_row, LV_PCT(100));
    lv_obj_set_height(agc_gain_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(agc_gain_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(agc_gain_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(agc_gain_row, 0, 0);
    lv_obj_set_style_pad_all(agc_gain_row, 0, 0);
    lv_obj_set_flex_flow(agc_gain_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(agc_gain_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *agc_gain_label = lv_label_create(agc_gain_row);
    lv_label_set_text(agc_gain_label, "Gain");

    ui_agc_gain_slider = lv_slider_create(agc_gain_row);
    lv_obj_t *agc_gain_slider = ui_agc_gain_slider;
    lv_obj_set_width(agc_gain_slider, 100);
    lv_slider_set_range(agc_gain_slider, 0, 30);
    lv_slider_set_value(agc_gain_slider, ui_prefs_ready ? ui_prefs.getInt(UI_PREF_AGC_GAIN_KEY, 15) : 15, LV_ANIM_OFF);
    lv_obj_add_event_cb(agc_gain_slider, ui_event_AgcGainSlider, LV_EVENT_ALL, NULL);
    // Greyed-out style when disabled
    lv_obj_set_style_bg_color(agc_gain_slider, lv_color_hex(0x888888), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(agc_gain_slider, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(agc_gain_slider, lv_color_hex(0x888888), LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(agc_gain_slider, LV_OPA_60, LV_PART_KNOB | LV_STATE_DISABLED);
    // Disable slider if AGC is enabled
    if (ui_prefs_ready && ui_prefs.getBool(UI_PREF_GAIN_CTRL_KEY, true))
    {
        lv_obj_add_state(agc_gain_slider, LV_STATE_DISABLED);
    }

    // --- Exposure Control (AEC) Switch ---
    lv_obj_t *exp_ctrl_row = lv_obj_create(ui_camera_settings_column);
    lv_obj_set_width(exp_ctrl_row, LV_PCT(100));
    lv_obj_set_height(exp_ctrl_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(exp_ctrl_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(exp_ctrl_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(exp_ctrl_row, 0, 0);
    lv_obj_set_style_pad_all(exp_ctrl_row, 0, 0);
    lv_obj_set_flex_flow(exp_ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(exp_ctrl_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *exp_ctrl_label = lv_label_create(exp_ctrl_row);
    lv_label_set_text(exp_ctrl_label, "AEC");

    lv_obj_t *exp_ctrl_switch = lv_switch_create(exp_ctrl_row);
    lv_obj_add_event_cb(exp_ctrl_switch, ui_event_ExposureCtrlSwitch, LV_EVENT_ALL, NULL);
    if (ui_prefs_ready && ui_prefs.getBool(UI_PREF_EXPOSURE_CTRL_KEY, true))
    {
        lv_obj_add_state(exp_ctrl_switch, LV_STATE_CHECKED);
    }

    // --- AEC Value Slider ---
    lv_obj_t *aec_value_row = lv_obj_create(ui_camera_settings_column);
    lv_obj_set_width(aec_value_row, LV_PCT(100));
    lv_obj_set_height(aec_value_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(aec_value_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(aec_value_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(aec_value_row, 0, 0);
    lv_obj_set_style_pad_all(aec_value_row, 0, 0);
    lv_obj_set_flex_flow(aec_value_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(aec_value_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *aec_value_label = lv_label_create(aec_value_row);
    lv_label_set_text(aec_value_label, "Exp");

    ui_aec_value_slider = lv_slider_create(aec_value_row);
    lv_obj_t *aec_value_slider = ui_aec_value_slider;
    lv_obj_set_width(aec_value_slider, 100);
    lv_slider_set_range(aec_value_slider, 0, 1200);
    lv_slider_set_value(aec_value_slider, ui_prefs_ready ? ui_prefs.getInt(UI_PREF_AEC_VALUE_KEY, 800) : 800, LV_ANIM_OFF);
    lv_obj_add_event_cb(aec_value_slider, ui_event_AecValueSlider, LV_EVENT_ALL, NULL);
    // Greyed-out style when disabled
    lv_obj_set_style_bg_color(aec_value_slider, lv_color_hex(0x888888), LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(aec_value_slider, LV_OPA_40, LV_PART_MAIN | LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(aec_value_slider, lv_color_hex(0x888888), LV_PART_KNOB | LV_STATE_DISABLED);
    lv_obj_set_style_bg_opa(aec_value_slider, LV_OPA_60, LV_PART_KNOB | LV_STATE_DISABLED);
    // Disable slider if AEC is enabled
    if (ui_prefs_ready && ui_prefs.getBool(UI_PREF_EXPOSURE_CTRL_KEY, true))
    {
        lv_obj_add_state(aec_value_slider, LV_STATE_DISABLED);
    }

    /* Hide initially */
    lv_obj_add_flag(ui_camera_settings_column, LV_OBJ_FLAG_HIDDEN);

    // Spacer to push navigation buttons to the bottom of the right panel
    lv_obj_t *ui_nav_spacer = lv_obj_create(ui_bottom_panel);
    lv_obj_clear_flag(ui_nav_spacer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_nav_spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_nav_spacer, 0, 0);
    lv_obj_set_width(ui_nav_spacer, LV_PCT(100));
    lv_obj_set_height(ui_nav_spacer, 0);
    lv_obj_set_flex_grow(ui_nav_spacer, 1);

    lv_obj_t *ui_nav_row = lv_obj_create(ui_bottom_panel);
    lv_obj_set_width(ui_nav_row, LV_PCT(100));
    lv_obj_set_height(ui_nav_row, LV_SIZE_CONTENT);
    lv_obj_clear_flag(ui_nav_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(ui_nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(ui_nav_row, 0, 0);
    lv_obj_set_style_pad_all(ui_nav_row, 0, 0);
    lv_obj_set_style_pad_column(ui_nav_row, 12, 0);
    lv_obj_set_flex_flow(ui_nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ui_nav_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // Left: Gallery
    ui_gallery_button = lv_btn_create(ui_nav_row);
    lv_obj_set_size(ui_gallery_button, 64, 64);
    lv_obj_set_style_bg_color(ui_gallery_button, lv_palette_main(LV_PALETTE_BLUE), 0);
    lv_obj_set_style_bg_opa(ui_gallery_button, LV_OPA_80, 0);
    lv_obj_set_style_radius(ui_gallery_button, 8, 0);
    lv_obj_set_style_text_color(ui_gallery_button, lv_color_white(), 0);
    lv_obj_t *gallery_label = lv_label_create(ui_gallery_button);
    lv_label_set_text(gallery_label, LV_SYMBOL_IMAGE);
    lv_obj_center(gallery_label);
    lv_obj_set_style_text_font(gallery_label, &lv_font_montserrat_28, 0);
    lv_obj_add_event_cb(ui_gallery_button, ui_event_GalleryButton, LV_EVENT_CLICKED, NULL);

    // Center: Camera settings
    ui_camera_settings_button = lv_btn_create(ui_nav_row);
    lv_obj_set_size(ui_camera_settings_button, 64, 64);
    lv_obj_set_style_bg_color(ui_camera_settings_button, lv_palette_main(LV_PALETTE_BROWN), 0);
    lv_obj_set_style_bg_opa(ui_camera_settings_button, LV_OPA_80, 0);
    lv_obj_set_style_radius(ui_camera_settings_button, 8, 0);
    lv_obj_set_style_text_color(ui_camera_settings_button, lv_color_white(), 0);
    lv_obj_add_event_cb(ui_camera_settings_button, ui_event_CameraSettingsButton, LV_EVENT_CLICKED, NULL);

    ui_camera_settings_icon = lv_label_create(ui_camera_settings_button);
    lv_label_set_text(ui_camera_settings_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_font(ui_camera_settings_icon,
                               &lv_font_montserrat_28, 0);
    lv_obj_center(ui_camera_settings_icon);

    // Right: Settings
    ui_settings_button = lv_btn_create(ui_nav_row);
    lv_obj_set_size(ui_settings_button, 64, 64);
    lv_obj_set_style_bg_color(ui_settings_button, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_bg_opa(ui_settings_button, LV_OPA_80, 0);
    lv_obj_set_style_radius(ui_settings_button, 8, 0);
    lv_obj_set_style_text_color(ui_settings_button, lv_color_white(), 0);
    lv_obj_t *settings_label = lv_label_create(ui_settings_button);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(settings_label, &lv_font_montserrat_28, 0);

    lv_obj_center(settings_label);
    lv_obj_add_event_cb(ui_settings_button, ui_event_SettingsButton, LV_EVENT_CLICKED, NULL);

    // Status bar update timer (every 2 seconds)
    status_timer = lv_timer_create(status_timer_cb, 2000, NULL);
    lv_timer_ready(status_timer);
}

void ui_HomeScreen_screen_destroy(void)
{
    if (status_timer)
    {
        lv_timer_del(status_timer);
        status_timer = NULL;
    }
    if (ui_HomeScreen)
        lv_obj_del(ui_HomeScreen);

    // NULL screen variables
    ui_HomeScreen = NULL;
    ui_FilterDropdown = NULL;
    ui_Image1 = NULL;
    ui_status_sd_label = NULL;
    ui_status_batt_label = NULL;
}

lv_obj_t *ui_get_gallery_button(void)
{
    return ui_gallery_button;
}
