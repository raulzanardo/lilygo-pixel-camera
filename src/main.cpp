#define DISABLE_ALL_LIBRARY_WARNINGS

#include <Arduino.h>
#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>
#include "touch.h"
#include "utilities.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include <FS.h>
#include <SD.h>
#include <cstring>
#include <vector>
#include <string>
#include <algorithm>
#include <PNGenc.h>
#include <cstdlib>
#include <Wire.h>
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#undef IS_BIT_SET
#include <XPowersLib.h>
#include "esp_wifi.h"
#include "esp_bt.h"

extern "C"
{
#include <extra/libs/png/lv_png.h>
#include <extra/libs/png/lodepng.h>
#include <extra/others/snapshot/lv_snapshot.h>
}
#include "filter.h"
#include "palettes.h"

extern "C" void *lodepng_malloc(size_t size)
{
    void *ptr = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ptr)
    {
        ptr = heap_caps_malloc(size, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
    }
    return ptr;
}

extern "C" void *lodepng_realloc(void *ptr, size_t new_size)
{
    void *new_ptr = heap_caps_realloc(ptr, new_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!new_ptr)
    {
        new_ptr = heap_caps_realloc(ptr, new_size, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
    }
    return new_ptr;
}

extern "C" void lodepng_free(void *ptr)
{
    heap_caps_free(ptr);
}
/* Change to your screen resolution */
static const uint16_t screenWidth = 222;
static const uint16_t screenHeight = 480;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * screenHeight / 6];

TFT_eSPI tft = TFT_eSPI(); /* TFT instance */
PowersSY6970 PMU;
camera_config_t config;

static const int user_button_pins[BOARD_USER_BTN_NUM] = BOARD_USER_BUTTON;
static bool user_button_last_pressed[BOARD_USER_BTN_NUM] = {false};
static bool led_flash_active = false;
static uint32_t led_flash_until = 0;
static constexpr uint8_t LED_FLASH_DUTY = 255;
static constexpr uint32_t LED_FLASH_DURATION_MS = 200;
static bool sd_initialized = false;
static bool pmu_ready = false;
static Preferences photo_prefs;
static const char *PHOTO_PREF_NAMESPACE = "gallery";
static const char *PHOTO_PREF_KEY = "last_photo";
static uint32_t photo_counter = 1;
static PNGENC png_encoder;
static File png_file_handle;
static bool sd_fs_registered = false;

static bool ensure_sd_initialized();
static void register_sd_fs_driver();
static void *sd_fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode);
static lv_fs_res_t sd_fs_close_cb(lv_fs_drv_t *drv, void *file_p);
static lv_fs_res_t sd_fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br);
static lv_fs_res_t sd_fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence);
static lv_fs_res_t sd_fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos);
static void ensure_flash_power(bool enable);
static bool ensure_pmu_ready();

static void *png_file_open_cb(const char *filename)
{
    png_file_handle = SD.open(filename, "w+r");
    if (!png_file_handle)
    {
        return nullptr;
    }
    return &png_file_handle;
}

static void png_file_close_cb(PNGFILE *pFile)
{
    if (png_file_handle)
    {
        png_file_handle.flush();
        png_file_handle.close();
    }
}

static int32_t png_file_read_cb(PNGFILE *pFile, uint8_t *buffer, int32_t length)
{
    File *f = static_cast<File *>(pFile->fHandle);
    if (!f)
    {
        return 0;
    }
    int32_t bytes = f->read(buffer, length);
    pFile->iPos = f->position();
    return bytes;
}

static int32_t png_file_write_cb(PNGFILE *pFile, uint8_t *buffer, int32_t length)
{
    File *f = static_cast<File *>(pFile->fHandle);
    if (!f)
    {
        return 0;
    }
    int32_t written = f->write(buffer, length);
    pFile->iPos += written;
    return written;
}

static int32_t png_file_seek_cb(PNGFILE *pFile, int32_t position)
{
    File *f = static_cast<File *>(pFile->fHandle);
    if (!f || !f->seek(position))
    {
        return -1;
    }
    pFile->iPos = f->position();
    return pFile->iPos;
}

static bool ensure_sd_initialized()
{
    if (sd_initialized)
    {
        return true;
    }

    if (!SD.begin(BOARD_SD_CS))
    {
        return false;
    }

    sd_initialized = true;
    register_sd_fs_driver();
    return true;
}

// Exported for gallery screen module
bool gallery_ensure_sd_initialized()
{
    return ensure_sd_initialized();
}

// Exported for UI status bar - get SD card free space in MB
uint32_t ui_get_sd_free_mb()
{
    if (!ensure_sd_initialized())
    {
        return 0;
    }
    uint64_t total = SD.totalBytes();
    uint64_t used = SD.usedBytes();
    uint64_t free = (total > used) ? (total - used) : 0;
    return static_cast<uint32_t>(free / (1024 * 1024));
}

// Delete all files and directories from the SD card root
static void sd_delete_root_contents()
{
    File root = SD.open("/");
    if (!root)
    {
        return;
    }

    std::vector<String> files;
    std::vector<String> dirs;

    File entry = root.openNextFile();
    while (entry)
    {
        String name = String("/") + entry.name();
        if (entry.isDirectory())
        {
            dirs.push_back(name);
        }
        else
        {
            files.push_back(name);
        }
        File next = root.openNextFile();
        entry.close();
        entry = next;
    }
    root.close();

    for (auto &f : files)
    {
        SD.remove(f.c_str());
    }
    for (auto &d : dirs)
    {
        SD.rmdir(d.c_str());
    }
}

// Exported for settings screen - format SD card by deleting all content
bool sd_format_card()
{
    if (!ensure_sd_initialized())
    {
        return false;
    }

    sd_delete_root_contents();

    // Reset photo counter and saved preferences
    photo_prefs.putUInt(PHOTO_PREF_KEY, 0);
    photo_counter = 1;
    return true;
}

// Exported for UI status bar - get battery voltage in mV
uint16_t ui_get_battery_voltage()
{
    if (!ensure_pmu_ready())
    {
        return 0;
    }
    PMU.enableMeasure();
    return PMU.getBattVoltage();
}

// Exported for UI status bar - check if charging
bool ui_is_charging()
{
    if (!ensure_pmu_ready())
    {
        return false;
    }
    auto status = PMU.chargeStatus();
    return (status == PowersSY6970::CHARGE_STATE_PRE_CHARGE ||
            status == PowersSY6970::CHARGE_STATE_FAST_CHARGE);
}

// Exported for UI status bar - check if USB is connected
bool ui_is_usb_connected()
{
    if (!ensure_pmu_ready())
    {
        return false;
    }
    auto bus = PMU.getBusStatus();
    return (bus != PowersSY6970::BUS_STATE_NOINPUT && bus != PowersSY6970::BUS_STATE_OTG);
}

static void register_sd_fs_driver()
{
    if (sd_fs_registered)
    {
        return;
    }

    lv_fs_drv_t drv;
    lv_fs_drv_init(&drv);
    drv.letter = 'S';
    drv.cache_size = 0;
    drv.open_cb = sd_fs_open_cb;
    drv.close_cb = sd_fs_close_cb;
    drv.read_cb = sd_fs_read_cb;
    drv.seek_cb = sd_fs_seek_cb;
    drv.tell_cb = sd_fs_tell_cb;
    lv_fs_drv_register(&drv);
    sd_fs_registered = true;
}

static void *sd_fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode)
{
    LV_UNUSED(drv);
    const char *arduino_mode = (mode == LV_FS_MODE_WR) ? "w+" : "r";
    File *file = new File();
    String arduino_path = path;
    if (!arduino_path.startsWith("/"))
    {
        arduino_path = "/" + arduino_path;
    }
    *file = SD.open(arduino_path.c_str(), arduino_mode);
    if (!file || !*file)
    {
        delete file;
        return nullptr;
    }
    return file;
}

static lv_fs_res_t sd_fs_close_cb(lv_fs_drv_t *drv, void *file_p)
{
    LV_UNUSED(drv);
    File *file = static_cast<File *>(file_p);
    if (!file)
    {
        return LV_FS_RES_OK;
    }
    file->close();
    delete file;
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br)
{
    LV_UNUSED(drv);
    File *file = static_cast<File *>(file_p);
    if (!file)
    {
        return LV_FS_RES_NOT_EX;
    }
    size_t read_bytes = file->read(static_cast<uint8_t *>(buf), btr);
    if (br)
    {
        *br = read_bytes;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence)
{
    LV_UNUSED(drv);
    File *file = static_cast<File *>(file_p);
    if (!file)
    {
        return LV_FS_RES_NOT_EX;
    }
    uint32_t target = pos;
    if (whence == LV_FS_SEEK_CUR)
    {
        target = file->position() + pos;
    }
    else if (whence == LV_FS_SEEK_END)
    {
        target = file->size() + pos;
    }
    if (!file->seek(target))
    {
        return LV_FS_RES_FS_ERR;
    }
    return LV_FS_RES_OK;
}

static lv_fs_res_t sd_fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos)
{
    LV_UNUSED(drv);
    File *file = static_cast<File *>(file_p);
    if (!file || !pos)
    {
        return LV_FS_RES_NOT_EX;
    }
    *pos = file->position();
    return LV_FS_RES_OK;
}

static bool trigger_led_flash()
{
    if (!ui_is_flash_enabled())
    {
        return false;
    }
    ensure_flash_power(true);
    ledcWrite(LEDC_WHITE_CH, LED_FLASH_DUTY);
    led_flash_active = true;
    led_flash_until = millis() + LED_FLASH_DURATION_MS;
    return true;
}

static void update_led_flash()
{
    if (led_flash_active && (millis() >= led_flash_until))
    {
        ledcWrite(LEDC_WHITE_CH, 0);
        led_flash_active = false;
        ensure_flash_power(false);
    }
}

static void ensure_flash_power(bool enable)
{
    if (enable)
    {
        if (!ensure_pmu_ready())
        {
            return;
        }
        PMU.enableOTG();
    }
    else if (pmu_ready)
    {
        PMU.disableOTG();
    }
}

static void init_user_buttons()
{
    for (size_t i = 0; i < BOARD_USER_BTN_NUM; ++i)
    {
        pinMode(user_button_pins[i], INPUT_PULLUP);
        user_button_last_pressed[i] = digitalRead(user_button_pins[i]) == LOW;
    }
}

static bool rotate_and_filter_frame(camera_fb_t *frame, std::vector<uint16_t> &rgb565_out, uint16_t &out_w, uint16_t &out_h)
{
    const uint16_t width = frame->width;
    const uint16_t height = frame->height;
    size_t pixel_count = static_cast<size_t>(width) * height;

    std::vector<uint16_t> working(pixel_count);
    uint16_t *src = reinterpret_cast<uint16_t *>(frame->buf);

    int zoom_level = ui_get_zoom_level();
    if (zoom_level > 0)
    {
        int crop_width, crop_height;

        if (zoom_level == 1)
        {
            crop_width = width / 2;
            crop_height = height / 2;
        }
        else
        {
            crop_width = width / 4;
            crop_height = height / 4;
        }

        int start_x = (width - crop_width) / 2;
        int start_y = (height - crop_height) / 2;

        for (int y = 0; y < height; y++)
        {
            for (int x = 0; x < width; x++)
            {
                int src_x = start_x + (x * crop_width / width);
                int src_y = start_y + (y * crop_height / height);

                if (src_x >= width)
                    src_x = width - 1;
                if (src_y >= height)
                    src_y = height - 1;

                working[y * width + x] = src[src_y * width + src_x];
            }
        }
    }
    else
    {
        memcpy(working.data(), src, pixel_count * sizeof(uint16_t));
    }

    out_w = width;
    out_h = height;

    camera_fb_t temp_frame = *frame;
    temp_frame.buf = reinterpret_cast<uint8_t *>(working.data());
    temp_frame.width = out_w;
    temp_frame.height = out_h;

    if (ui_get_auto_adjust_enabled())
    {
        applyAutoAdjust(&temp_frame);
    }

    switch (ui_get_filter_mode())
    {
    case 1:
        applyPixelate(&temp_frame, 8, false);
        break;
    case 2:
        applyDithering(&temp_frame, ui_get_dither_bits(), ui_get_dither_bits(), ui_get_dither_bits(), ui_get_dither_grayscale(), ui_get_dither_algorithm(), ui_get_dither_bayer_size());
        break;
    case 3:
    {
        int palette_size = 0;
        const uint32_t *palette = nullptr;
        ui_get_palette(&palette, &palette_size);
        if (!palette || palette_size <= 0)
        {
            palette = PALETTE_CYBERPUNK;
            palette_size = PALETTE_CYBERPUNK_SIZE;
        }
        applyColorPalette(working.data(), out_w, out_h, palette, palette_size, ui_get_dither_type(), ui_get_pixel_size(), 2, ui_get_auto_levels_enabled());
    }
    break;
    case 4:
        applyEdgeDetection(&temp_frame, 1);
        break;
    case 5:
        applyCRT(&temp_frame, ui_get_pixel_size());
        break;
    case 6:
        applyMultipleExposure(&temp_frame, ui_get_multi_exposure_frames(), ui_get_multi_exposure_blend_mode());
        if (ui_get_multi_exposure_palette_enabled())
        {
            int palette_size = 0;
            const uint32_t *palette = nullptr;
            ui_get_palette(&palette, &palette_size);
            if (!palette || palette_size <= 0)
            {
                palette = PALETTE_CYBERPUNK;
                palette_size = PALETTE_CYBERPUNK_SIZE;
            }
            applyColorPalette(working.data(), out_w, out_h, palette, palette_size, ui_get_dither_type(), ui_get_pixel_size(), 2, ui_get_auto_levels_enabled());
        }
        break;
    default:
        break;
    }

    rgb565_out = std::move(working);
    return true;
}

static bool encode_rgb565_png(const char *path, const uint16_t *pixels, uint16_t width, uint16_t height)
{
    if (SD.exists(path))
    {
        SD.remove(path);
    }

    int rc = png_encoder.open(path, png_file_open_cb, png_file_close_cb, png_file_read_cb, png_file_write_cb, png_file_seek_cb);
    if (rc != PNG_SUCCESS)
    {
        return false;
    }

    // Scale up the image by 2x (each pixel becomes 2x2)
    uint16_t scaled_width = width * 2;
    uint16_t scaled_height = height * 2;

    rc = png_encoder.encodeBegin(scaled_width, scaled_height, PNG_PIXEL_TRUECOLOR, 24, nullptr, 3);
    if (rc != PNG_SUCCESS)
    {
        png_encoder.close();
        return false;
    }

    std::vector<uint16_t> scaled_line(scaled_width);
    std::vector<uint8_t> temp_line(scaled_width * 3);

    for (uint16_t y = 0; y < height; ++y)
    {
        const uint16_t *row = pixels + y * width;

        // Create a scaled line: each pixel becomes 2 pixels horizontally
        for (uint16_t x = 0; x < width; ++x)
        {
            scaled_line[x * 2] = row[x];
            scaled_line[x * 2 + 1] = row[x];
        }

        // Write the same line twice (for vertical scaling)
        rc = png_encoder.addRGB565Line(scaled_line.data(), temp_line.data(), true);
        if (rc != PNG_SUCCESS)
        {
            png_encoder.close();
            return false;
        }

        rc = png_encoder.addRGB565Line(scaled_line.data(), temp_line.data(), true);
        if (rc != PNG_SUCCESS)
        {
            png_encoder.close();
            return false;
        }
    }

    int32_t written = png_encoder.close();
    if (written <= 0)
    {
        return false;
    }

    return true;
}

static bool save_frame_as_png(camera_fb_t *frame)
{
    if (!ensure_sd_initialized())
    {
        return false;
    }

    char path[32];
    uint32_t current_index = photo_counter;
    snprintf(path, sizeof(path), "/photo_%lu.png", static_cast<unsigned long>(current_index));

    std::vector<uint16_t> processed_pixels;
    uint16_t out_w = frame->width;
    uint16_t out_h = frame->height;
    if (!rotate_and_filter_frame(frame, processed_pixels, out_w, out_h))
    {
        return false;
    }

    if (SD.exists(path))
    {
        SD.remove(path);
    }

    bool ok = encode_rgb565_png(path, processed_pixels.data(), out_w, out_h);
    if (ok)
    {
        photo_prefs.putUInt(PHOTO_PREF_KEY, current_index);
        photo_counter = current_index + 1;
        ui_show_photo_overlay("Photo saved");
    }
    return ok;
}

static bool save_screenshot_as_bmp(const char *filename, lv_img_dsc_t *img_dsc, void *buf)
{
    if (!ensure_sd_initialized())
    {
        return false;
    }

    File file = SD.open(filename, FILE_WRITE);
    if (!file)
    {
        return false;
    }

    uint32_t width = img_dsc->header.w;
    uint32_t height = img_dsc->header.h;
    uint32_t row_size = ((width * 3 + 3) / 4) * 4;
    uint32_t image_size = row_size * height;
    uint32_t file_size = 54 + image_size;

    uint8_t bmp_header[54] = {
        'B', 'M',
        (uint8_t)(file_size), (uint8_t)(file_size >> 8), (uint8_t)(file_size >> 16), (uint8_t)(file_size >> 24),
        0, 0, 0, 0,
        54, 0, 0, 0,
        40, 0, 0, 0,
        (uint8_t)(width), (uint8_t)(width >> 8), (uint8_t)(width >> 16), (uint8_t)(width >> 24),
        (uint8_t)(-height), (uint8_t)((-height) >> 8), (uint8_t)((-height) >> 16), (uint8_t)((-height) >> 24),
        1, 0,
        24, 0,
        0, 0, 0, 0,
        (uint8_t)(image_size), (uint8_t)(image_size >> 8), (uint8_t)(image_size >> 16), (uint8_t)(image_size >> 24),
        0x13, 0x0B, 0, 0,
        0x13, 0x0B, 0, 0,
        0, 0, 0, 0,
        0, 0, 0, 0};

    file.write(bmp_header, 54);

    uint16_t *pixel_data = (uint16_t *)buf;
    uint8_t *row = (uint8_t *)malloc(row_size);
    if (!row)
    {
        file.close();
        return false;
    }

    for (uint32_t y = 0; y < height; y++)
    {
        memset(row, 0, row_size);
        for (uint32_t x = 0; x < width; x++)
        {
            uint16_t rgb565 = pixel_data[y * width + x];
            uint8_t r = ((rgb565 >> 11) & 0x1F) << 3;
            uint8_t g = ((rgb565 >> 5) & 0x3F) << 2;
            uint8_t b = (rgb565 & 0x1F) << 3;
            row[x * 3 + 0] = b;
            row[x * 3 + 1] = g;
            row[x * 3 + 2] = r;
        }
        file.write(row, row_size);
    }

    free(row);
    file.close();
    return true;
}

static void capture_screenshot()
{
    if (!ensure_sd_initialized())
    {
        ui_show_photo_overlay("SD card error");
        return;
    }

    lv_obj_t *scr = lv_scr_act();
    if (!scr)
    {
        return;
    }

    uint32_t buf_size = lv_snapshot_buf_size_needed(scr, LV_IMG_CF_TRUE_COLOR);

    void *screenshot_buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!screenshot_buf)
    {
        screenshot_buf = heap_caps_malloc(buf_size, MALLOC_CAP_DEFAULT | MALLOC_CAP_8BIT);
    }

    if (!screenshot_buf)
    {
        ui_show_photo_overlay("Memory error");
        return;
    }

    lv_img_dsc_t img_dsc;
    lv_res_t res = lv_snapshot_take_to_buf(scr, LV_IMG_CF_TRUE_COLOR, &img_dsc, screenshot_buf, buf_size);

    if (res != LV_RES_OK)
    {
        heap_caps_free(screenshot_buf);
        ui_show_photo_overlay("Capture failed");
        return;
    }

    if (photo_prefs.begin(PHOTO_PREF_NAMESPACE, false))
    {
        uint32_t last_saved = photo_prefs.getUInt(PHOTO_PREF_KEY, 0);
        photo_counter = last_saved + 1;
    }

    char filename[64];
    snprintf(filename, sizeof(filename), "/screenshot_%lu.bmp", photo_counter);

    bool saved = save_screenshot_as_bmp(filename, &img_dsc, screenshot_buf);
    heap_caps_free(screenshot_buf);

    if (saved)
    {
        photo_prefs.putUInt(PHOTO_PREF_KEY, photo_counter);
        photo_counter++;
        ui_show_photo_overlay("Screenshot saved");
    }
    else
    {
        ui_show_photo_overlay("Save failed");
    }
}

static void capture_photo_with_flash()
{
    bool flash_active = trigger_led_flash();

    if (flash_active)
    {
        delay(50);

        for (int i = 0; i < 3; i++)
        {
            camera_fb_t *temp = esp_camera_fb_get();
            if (temp)
            {
                esp_camera_fb_return(temp);
            }
            delay(50);
        }
    }

    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame)
    {
        if (led_flash_active)
        {
            ledcWrite(LEDC_WHITE_CH, 0);
            led_flash_active = false;
            ensure_flash_power(false);
        }
        return;
    }

    if (!save_frame_as_png(frame))
    {
    }

    esp_camera_fb_return(frame);

    if (led_flash_active)
    {
        ledcWrite(LEDC_WHITE_CH, 0);
        led_flash_active = false;
        ensure_flash_power(false);
    }
}

static void handle_user_buttons()
{
    for (size_t i = 0; i < BOARD_USER_BTN_NUM; ++i)
    {
        bool pressed = digitalRead(user_button_pins[i]) == LOW;
        if (pressed && !user_button_last_pressed[i])
        {
            if (i == 1 && ui_get_screenshot_mode_enabled())
            {
                capture_screenshot();
            }
            else
            {
                capture_photo_with_flash();
            }
        }
        user_button_last_pressed[i] = pressed;
    }
}

void camera_init(void)
{
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = CAMERA_PIN_Y2;
    config.pin_d1 = CAMERA_PIN_Y3;
    config.pin_d2 = CAMERA_PIN_Y4;
    config.pin_d3 = CAMERA_PIN_Y5;
    config.pin_d4 = CAMERA_PIN_Y6;
    config.pin_d5 = CAMERA_PIN_Y7;
    config.pin_d6 = CAMERA_PIN_Y8;
    config.pin_d7 = CAMERA_PIN_Y9;
    config.pin_xclk = CAMERA_PIN_XCLK;
    config.pin_pclk = CAMERA_PIN_PCLK;
    config.pin_vsync = CAMERA_PIN_VSYNC;
    config.pin_href = CAMERA_PIN_HREF;
    config.pin_sccb_sda = CAMERA_PIN_SIOD;
    config.pin_sccb_scl = CAMERA_PIN_SIOC;
    config.pin_pwdn = CAMERA_PIN_PWDN;
    config.pin_reset = CAMERA_PIN_RESET;
    config.xclk_freq_hz = XCLK_FREQ_HZ;

    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_HQVGA; // HQVGA (240x176) for best FPS
    config.jpeg_quality = 0;
    config.fb_count = 2;

    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
    }
    sensor_t *s = esp_camera_sensor_get();

    if (s)
    {
        if (s->id.PID == GC0308_PID)
        {
            s->set_vflip(s, 0); // This can't flip the picture vertically. Watch out!
            s->set_hmirror(s, 0);
        }
        else if (s->id.PID == OV3660_PID)
        {
            // ── 1. Fix high-quality frame-capture bug ──────────────────────
            // At quality < ~10 the OV3660 can stall. These two register writes
            // switch the pixel clock to manual mode and prevent the lock-up.
            s->set_reg(s, 0x3824, 0x1f, 0x04); // PCLK divider – manual mode
            s->set_reg(s, 0x460c, 0x02, 0x02); // VFIFO control – enable manual PCLK
            delay(100);                        // let the sensor settle

            // ── 3. Automatic White Balance ─────────────────────────────────
            s->set_whitebal(s, 1); // enable AWB
            s->set_awb_gain(s, 1); // enable AWB gain
            s->set_wb_mode(s, 0);  // 0=Auto

            // ── 4. Bad Pixel Correction & noise reduction ──────────────────
            s->set_bpc(s, 1);     // bad pixel correction ON
            s->set_wpc(s, 1);     // white pixel correction ON
            s->set_dcw(s, 1);     // downsize/crop weighting ON
            s->set_raw_gma(s, 1); // raw gamma ON (more natural tones)

            // ── 5. Exposure & gain (UI-controlled) ────────────────────────
            bool aec2_enabled = ui_get_aec2_enabled();
            s->set_aec2(s, aec2_enabled ? 1 : 0); // AEC DSP

            bool exposure_ctrl = ui_get_exposure_ctrl_enabled();
            s->set_exposure_ctrl(s, exposure_ctrl ? 1 : 0);
            if (!exposure_ctrl)
            {
                s->set_aec_value(s, ui_get_aec_value());
            }

            bool gain_ctrl = ui_get_gain_ctrl_enabled();
            s->set_gain_ctrl(s, gain_ctrl ? 1 : 0);
            if (!gain_ctrl)
            {
                s->set_agc_gain(s, ui_get_agc_gain());
            }

            // ── 6. Sharpness, saturation & contrast ───────────────────────
            bool dark_mode = ui_get_dark_mode_enabled();
            s->set_sharpness(s, 1);                  // mild sharpening  (-2…+2)
            s->set_saturation(s, dark_mode ? 1 : 0); // colour saturation (-2…+2)
            s->set_contrast(s, dark_mode ? 1 : 0);   // contrast          (-2…+2)
            s->set_brightness(s, dark_mode ? 1 : 0); // brightness        (-2…+2)
            s->set_ae_level(s, dark_mode ? 2 : 0);   // exposure bias     (-2…+2)

            // ── 7. Lens distortion correction ─────────────────────────────
            s->set_lenc(s, 1); // lens correction ON

            // ── 8. Orientation ────────────────────────────────────────────
            s->set_hmirror(s, 0);
            s->set_vflip(s, 1);
        }
        else
        {
        }
    }

    // Initialize Camera LED
    // Adjust the LED duty cycle to save power and heat.
    // If you directly set the LED to HIGH, the heat brought by the LED will be huge,
    // and the current consumption will also be huge.
    ledcSetup(LEDC_WHITE_CH, 1000, 8);
    ledcAttachPin(CAMERA_WHITH_LED, LEDC_WHITE_CH);
    ledcWrite(LEDC_WHITE_CH, 0);
}

/* Display flushing */
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p)
{
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();

    lv_disp_flush_ready(disp);
}

// Touchpad reading function
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    if (touch_touched())
    {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touch_last_x;
        data->point.y = touch_last_y;
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

static bool ensure_pmu_ready()
{
    if (pmu_ready)
    {
        return true;
    }

    bool hasPMU = PMU.init(Wire, BOARD_I2C_SDA, BOARD_I2C_SCL, SY6970_SLAVE_ADDRESS);
    if (!hasPMU)
    {
        return false;
    }
    pmu_ready = true;
    PMU.enableStatLed();
    PMU.setChargeTargetVoltage(4352);
    PMU.setPrechargeCurr(64);
    PMU.setChargerConstantCurr(320);
    PMU.enableMeasure();
    return true;
}

void setup()
{
    // Disable WiFi and Bluetooth to save power and reduce RF noise
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    pinMode(BOARD_TFT_BL, OUTPUT);
    digitalWrite(BOARD_TFT_BL, LOW); // Backlight OFF

    camera_init();
    init_user_buttons();

    lv_init();
    lv_png_init();

    tft.begin();               /* TFT init */
    tft.setRotation(0);        /* Landscape orientation, flipped */
    tft.fillScreen(TFT_BLACK); // Clear the screen to black
    digitalWrite(BOARD_TFT_BL, HIGH);

    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * screenHeight / 10);

    /* Initialize the display */
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    /* Change the following line to your display resolution */
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    /* Initialize the (dummy) input device driver */
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);

    touch_init(screenWidth, screenHeight, tft.getRotation());

    ui_init();

    ensure_sd_initialized();

    if (photo_prefs.begin(PHOTO_PREF_NAMESPACE, false))
    {
        uint32_t last_saved = photo_prefs.getUInt(PHOTO_PREF_KEY, 0);
        photo_counter = last_saved + 1;
    }
}

void loop()
{
    handle_user_buttons();
    update_led_flash();
    lv_task_handler();
}
