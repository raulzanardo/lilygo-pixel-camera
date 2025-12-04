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
#include <Preferences.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
extern "C"
{
#include <extra/libs/png/lv_png.h>
#include <extra/libs/png/lodepng.h>
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
static const uint16_t screenWidth = 480;
static const uint16_t screenHeight = 222;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * screenHeight / 6];

TFT_eSPI tft = TFT_eSPI(); /* TFT instance */

camera_config_t config;

static const int user_button_pins[BOARD_USER_BTN_NUM] = BOARD_USER_BUTTON;
static bool user_button_last_pressed[BOARD_USER_BTN_NUM] = {false};
static bool led_flash_active = false;
static uint32_t led_flash_until = 0;
static constexpr uint8_t LED_FLASH_DUTY = 200;
static constexpr uint32_t LED_FLASH_DURATION_MS = 150;
static bool sd_initialized = false;
static Preferences photo_prefs;
static const char *PHOTO_PREF_NAMESPACE = "gallery";
static const char *PHOTO_PREF_KEY = "last_photo";
static uint32_t photo_counter = 1;
static PNGENC png_encoder;
static File png_file_handle;
static lv_obj_t *gallery_screen = nullptr;
static lv_obj_t *gallery_list = nullptr;
static lv_obj_t *gallery_preview_screen = nullptr;
static lv_obj_t *gallery_preview_img = nullptr;
static lv_obj_t *gallery_preview_label = nullptr;
static lv_obj_t *gallery_preview_back_btn = nullptr;
static lv_img_dsc_t gallery_img_dsc;
static std::vector<uint8_t> gallery_img_buffer;
static std::string gallery_img_path;
static bool sd_fs_registered = false;

static bool ensure_sd_initialized();
static void register_sd_fs_driver();
static void *sd_fs_open_cb(lv_fs_drv_t *drv, const char *path, lv_fs_mode_t mode);
static lv_fs_res_t sd_fs_close_cb(lv_fs_drv_t *drv, void *file_p);
static lv_fs_res_t sd_fs_read_cb(lv_fs_drv_t *drv, void *file_p, void *buf, uint32_t btr, uint32_t *br);
static lv_fs_res_t sd_fs_seek_cb(lv_fs_drv_t *drv, void *file_p, uint32_t pos, lv_fs_whence_t whence);
static lv_fs_res_t sd_fs_tell_cb(lv_fs_drv_t *drv, void *file_p, uint32_t *pos);

static void *png_file_open_cb(const char *filename)
{
    png_file_handle = SD.open(filename, "w+r");
    if (!png_file_handle)
    {
        Serial.printf("Failed to open %s for PNG write\n", filename);
        return nullptr;
    }
    Serial.printf("Opening %s for PNG write\n", filename);
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

static long extract_photo_index(const String &name)
{
    int start = name.indexOf("photo_");
    if (start < 0)
    {
        return -1;
    }
    start += 6;
    int end = name.indexOf('.', start);
    if (end < 0)
    {
        end = name.length();
    }
    String number = name.substring(start, end);
    return number.toInt();
}

static bool list_captured_photos(std::vector<String> &out_names)
{
    if (!ensure_sd_initialized())
    {
        return false;
    }

    File root = SD.open("/");
    if (!root)
    {
        return false;
    }

    File entry = root.openNextFile();
    while (entry)
    {
        if (!entry.isDirectory())
        {
            String name = entry.name();
            if (name.endsWith(".png") || name.endsWith(".PNG"))
            {
                out_names.push_back(name);
            }
        }
        File next = root.openNextFile();
        entry.close();
        entry = next;
    }
    root.close();

    std::sort(out_names.begin(), out_names.end(), [](const String &a, const String &b) {
        long ia = extract_photo_index(a);
        long ib = extract_photo_index(b);
        if (ia == ib)
        {
            return a > b;
        }
        return ia > ib;
    });
    return true;
}

static void show_gallery_screen();

static void back_to_camera_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    ui_resume_camera_timer();
    lv_disp_load_scr(ui_Screen1);
}

static void gallery_button_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    Serial.println("Gallery button tapped");
    show_gallery_screen();
}

static void back_to_gallery_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (gallery_screen)
    {
        lv_disp_load_scr(gallery_screen);
    }
}

static bool load_png_to_dsc(const char *path)
{
    File file = SD.open(path, FILE_READ);
    if (!file)
    {
        Serial.printf("Gallery failed to open %s\n", path);
        return false;
    }

    const size_t size = file.size();
    Serial.printf("Reading PNG file %s (%u bytes)\n", path, size);
    uint8_t *file_data = (uint8_t *)lodepng_malloc(size);
    if (!file_data)
    {
        Serial.println("Failed to allocate file buffer");
        file.close();
        return false;
    }
    size_t read = file.read(file_data, size);
    file.close();
    if (read != size)
    {
        Serial.println("Gallery read mismatch");
        lodepng_free(file_data);
        return false;
    }

    unsigned char *decoded = nullptr;
    unsigned width = 0;
    unsigned height = 0;
    unsigned error = lodepng_decode32(&decoded, &width, &height, file_data, size);
    lodepng_free(file_data);
    if (error || decoded == nullptr)
    {
        Serial.printf("PNG decode error %u: %s\n", error, lodepng_error_text(error));
        if (decoded)
        {
            lodepng_free(decoded);
        }
        return false;
    }
    Serial.printf("PNG decoded: %u x %u\n", width, height);

    // Convert RGBA to LVGL color format
    const size_t pixel_count = (size_t)width * height;
    gallery_img_buffer.resize(pixel_count * sizeof(lv_color_t));
    lv_color_t *dst = reinterpret_cast<lv_color_t *>(gallery_img_buffer.data());
    for (size_t i = 0; i < pixel_count; ++i)
    {
        uint8_t r = decoded[i * 4 + 0];
        uint8_t g = decoded[i * 4 + 1];
        uint8_t b = decoded[i * 4 + 2];
        dst[i] = lv_color_make(r, g, b);
    }
    lodepng_free(decoded);

    gallery_img_dsc.header.always_zero = 0;
    gallery_img_dsc.header.w = (uint16_t)width;
    gallery_img_dsc.header.h = (uint16_t)height;
    gallery_img_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    gallery_img_dsc.data = gallery_img_buffer.data();
    gallery_img_dsc.data_size = gallery_img_buffer.size();
    return true;
}

static void show_photo_preview(const char *filename)
{
    Serial.printf("Opening photo %s\n", filename);
    if (!gallery_preview_screen)
    {
        gallery_preview_screen = lv_obj_create(NULL);
        lv_obj_clear_flag(gallery_preview_screen, LV_OBJ_FLAG_SCROLLABLE);

        gallery_preview_label = lv_label_create(gallery_preview_screen);
        lv_obj_align(gallery_preview_label, LV_ALIGN_TOP_MID, 0, 8);

        gallery_preview_img = lv_img_create(gallery_preview_screen);
        lv_obj_align(gallery_preview_img, LV_ALIGN_CENTER, 0, 10);

        gallery_preview_back_btn = lv_btn_create(gallery_preview_screen);
        lv_obj_set_size(gallery_preview_back_btn, 100, 36);
        lv_obj_align(gallery_preview_back_btn, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_t *label = lv_label_create(gallery_preview_back_btn);
        lv_label_set_text(label, "Back");
        lv_obj_center(label);
        lv_obj_add_event_cb(gallery_preview_back_btn, back_to_gallery_cb, LV_EVENT_CLICKED, NULL);
    }

    gallery_img_path = "/";
    gallery_img_path += filename;
    lv_label_set_text_fmt(gallery_preview_label, "%s", filename);

    bool loaded = load_png_to_dsc(gallery_img_path.c_str());
    if (loaded)
    {
        lv_img_set_src(gallery_preview_img, &gallery_img_dsc);
        lv_obj_clear_flag(gallery_preview_img, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_label_set_text_fmt(gallery_preview_label, "Failed: %s", filename);
        lv_obj_add_flag(gallery_preview_img, LV_OBJ_FLAG_HIDDEN);
    }

    lv_disp_load_scr(gallery_preview_screen);
    ui_pause_camera_timer();
}

static void gallery_item_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    lv_obj_t *btn = lv_event_get_target(e);
    const char *name = lv_list_get_btn_text(gallery_list, btn);
    if (!name)
    {
        return;
    }
    Serial.printf("Gallery item tapped: %s\n", name);
    show_photo_preview(name);
}

static void populate_gallery_list()
{
    if (!gallery_list)
    {
        return;
    }
    lv_obj_clean(gallery_list);

    std::vector<String> photos;
    bool ok = list_captured_photos(photos);
    if (!ok)
    {
        lv_obj_t *label = lv_label_create(gallery_list);
        lv_label_set_text(label, "Failed to read SD card");
        return;
    }

    if (photos.empty())
    {
        lv_obj_t *label = lv_label_create(gallery_list);
        lv_label_set_text(label, "No photos found");
        return;
    }

    for (const String &name : photos)
    {
        lv_obj_t *btn = lv_list_add_btn(gallery_list, LV_SYMBOL_FILE, name.c_str());
        lv_obj_add_event_cb(btn, gallery_item_event_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void show_gallery_screen()
{
    if (!gallery_screen)
    {
        Serial.println("Creating gallery screen");
        gallery_screen = lv_obj_create(NULL);
        lv_obj_clear_flag(gallery_screen, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *title = lv_label_create(gallery_screen);
        lv_label_set_text(title, "Gallery");
        lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);

        gallery_list = lv_list_create(gallery_screen);
        lv_obj_set_size(gallery_list, 440, 150);
        lv_obj_align(gallery_list, LV_ALIGN_CENTER, 0, 0);

        lv_obj_t *back_btn = lv_btn_create(gallery_screen);
        lv_obj_set_size(back_btn, 100, 36);
        lv_obj_align(back_btn, LV_ALIGN_BOTTOM_MID, 0, -12);
        lv_obj_t *label = lv_label_create(back_btn);
        lv_label_set_text(label, "Back");
        lv_obj_center(label);
        lv_obj_add_event_cb(back_btn, back_to_camera_cb, LV_EVENT_CLICKED, NULL);
    }

    Serial.println("Populating gallery list");
    populate_gallery_list();
    lv_disp_load_scr(gallery_screen);
    Serial.println("Gallery screen shown");
    ui_pause_camera_timer();
}

static bool ensure_sd_initialized()
{
    if (sd_initialized)
    {
        return true;
    }

    if (!SD.begin(BOARD_SD_CS))
    {
        Serial.println("Failed to mount SD card");
        return false;
    }

    sd_initialized = true;
    register_sd_fs_driver();
    Serial.println("SD card ready");
    return true;
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
    const size_t pixel_count = static_cast<size_t>(width) * height;

    std::vector<uint16_t> working(pixel_count);
    uint16_t *src = reinterpret_cast<uint16_t *>(frame->buf);

    bool rotate = ui_get_camera_rotation();
    if (rotate)
    {
        out_w = height;
        out_h = width;
        for (size_t y = 0; y < height; ++y)
        {
            for (size_t x = 0; x < width; ++x)
            {
                size_t src_idx = y * width + x;
                size_t dst_idx = x * height + (height - 1 - y);
                working[dst_idx] = src[src_idx];
            }
        }
    }
    else
    {
        out_w = width;
        out_h = height;
        memcpy(working.data(), src, pixel_count * sizeof(uint16_t));
    }

    camera_fb_t temp_frame = *frame;
    temp_frame.buf = reinterpret_cast<uint8_t *>(working.data());
    temp_frame.width = out_w;
    temp_frame.height = out_h;

    switch (ui_get_filter_mode())
    {
    case 1:
        applyPixelate(&temp_frame, 8, false);
        break;
    case 2:
        applyColorPalette(working.data(), out_w, out_h, PALETTE_CYBERPUNK, PALETTE_CYBERPUNK_SIZE, 1, 2, 2);
        break;
    case 3:
        applyEdgeDetection(&temp_frame, 1);
        break;
    default:
        break;
    }

    rgb565_out = std::move(working);
    return true;
}

static bool encode_rgb565_png(const char *path, const uint16_t *pixels, uint16_t width, uint16_t height)
{
    SD.remove(path);

    int rc = png_encoder.open(path, png_file_open_cb, png_file_close_cb, png_file_read_cb, png_file_write_cb, png_file_seek_cb);
    if (rc != PNG_SUCCESS)
    {
        Serial.println("PNG open failed");
        return false;
    }

    rc = png_encoder.encodeBegin(width, height, PNG_PIXEL_TRUECOLOR, 24, nullptr, 3);
    if (rc != PNG_SUCCESS)
    {
        Serial.printf("encodeBegin failed: %d\n", rc);
        png_encoder.close();
        return false;
    }

    std::vector<uint8_t> temp_line(width * 3);
    for (uint16_t y = 0; y < height; ++y)
    {
        const uint16_t *row = pixels + y * width;
        rc = png_encoder.addRGB565Line(const_cast<uint16_t *>(row), temp_line.data(), true);
        if (rc != PNG_SUCCESS)
        {
            Serial.printf("addLine failed at row %u: %d\n", y, rc);
            png_encoder.close();
            return false;
        }
    }

    int32_t written = png_encoder.close();
    if (written <= 0)
    {
        Serial.println("PNG close failed");
        return false;
    }

    File verify = SD.open(path, FILE_READ);
    if (verify)
    {
        size_t verify_size = verify.size();
        uint8_t header[16] = {0};
        verify.read(header, sizeof(header));
        verify.close();
        Serial.printf("PNG saved (%u bytes). Header: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                      static_cast<unsigned int>(verify_size),
                      header[0], header[1], header[2], header[3], header[4], header[5], header[6], header[7]);
    }
    else
    {
        Serial.println("Failed to reopen PNG for verification");
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
        Serial.println("Failed to process frame before saving");
        return false;
    }

    bool ok = encode_rgb565_png(path, processed_pixels.data(), out_w, out_h);
    if (ok)
    {
        Serial.printf("Saved photo to %s (%u x %u)\n", path, out_w, out_h);
        photo_prefs.putUInt(PHOTO_PREF_KEY, current_index);
        photo_counter = current_index + 1;
    }
    return ok;
}

static void capture_photo_with_flash()
{
    bool flash_active = trigger_led_flash();

    if (flash_active)
    {
        delay(60);
    }

    camera_fb_t *frame = esp_camera_fb_get();
    if (!frame)
    {
        Serial.println("Failed to capture frame");
        return;
    }

    if (!save_frame_as_png(frame))
    {
        Serial.println("Failed to save captured frame");
    }

    esp_camera_fb_return(frame);
}

static void handle_user_buttons()
{
    for (size_t i = 0; i < BOARD_USER_BTN_NUM; ++i)
    {
        bool pressed = digitalRead(user_button_pins[i]) == LOW;
        if (pressed && !user_button_last_pressed[i])
        {
            capture_photo_with_flash();
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
    config.frame_size = FRAMESIZE_240X240;
    config.jpeg_quality = 0;
    config.fb_count = 1;

    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK)
    {
        Serial.println("camera init error!");
    }
    sensor_t *s = esp_camera_sensor_get();

    if (s)
    {
        Serial.print("camera id:");
        Serial.println(s->id.PID);
        camera_sensor_info_t *sinfo = esp_camera_sensor_get_info(&(s->id));
        if (sinfo)
        {
            Serial.print("camera model:");
            Serial.println(sinfo->name);
        }
        if (s->id.PID == GC0308_PID)
        {
            s->set_vflip(s, 0); // This can't flip the picture vertically. Watch out!
            s->set_hmirror(s, 0);
            //s->set_contrast(s, 0);
            //s->set_gain_ctrl(s, 1);

         
        }
        else
        {
            Serial.println("Camera ID error!");
        }
    }

    // Initialize Camera LED
    // Adjust the LED duty cycle to save power and heat.
    // If you directly set the LED to HIGH, the heat brought by the LED will be huge,
    // and the current consumption will also be huge.
    ledcSetup(LEDC_WHITE_CH, 1000, 8);
    ledcAttachPin(CAMERA_WHITH_LED, LEDC_WHITE_CH);
    ledcWrite(LEDC_WHITE_CH, 0);
    // ledcWrite(LEDC_WHITE_CH, 20);
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

        // Serial.print("Data x ");
        // Serial.println(touch_last_x);

        // Serial.print("Data y ");
        // Serial.println(touch_last_y);

        // Update the label with the touch coordinates
        // update_touch_coordinates(touch_last_x, touch_last_y);
    }
    else
    {
        data->state = LV_INDEV_STATE_REL;
    }
}

void setup()
{
    delay(2000);
    Serial.begin(115200); /* prepare for possible serial debug */

    String LVGL_Arduino = "Hello Arduino! ";
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

    Serial.println(LVGL_Arduino);
    Serial.println("I am LVGL_Arduino");

    camera_init();
    init_user_buttons();

    lv_init();
    lv_png_init();

    tft.begin();        /* TFT init */
    tft.setRotation(1); /* Landscape orientation, flipped */

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

    lv_obj_t *gallery_btn = ui_get_gallery_button();
    if (gallery_btn)
    {
        lv_obj_add_event_cb(gallery_btn, gallery_button_event_cb, LV_EVENT_CLICKED, NULL);
    }

    ensure_sd_initialized();

    if (photo_prefs.begin(PHOTO_PREF_NAMESPACE, false))
    {
        uint32_t last_saved = photo_prefs.getUInt(PHOTO_PREF_KEY, 0);
        photo_counter = last_saved + 1;
    }

    // start_camera_stream();
    // init_camera_module();

    //
    Serial.println("Setup done");
}

void loop()
{
    handle_user_buttons();
    update_led_flash();
    lv_task_handler();
    delay(2);
}
