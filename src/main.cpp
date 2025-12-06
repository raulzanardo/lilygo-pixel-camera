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
#include <XPowersLib.h>
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
PowersSY6970 PMU;
camera_config_t config;

static const int user_button_pins[BOARD_USER_BTN_NUM] = BOARD_USER_BUTTON;
static bool user_button_last_pressed[BOARD_USER_BTN_NUM] = {false};
static bool led_flash_active = false;
static uint32_t led_flash_until = 0;
static constexpr uint8_t LED_FLASH_DUTY = 200;
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

// Exported for gallery screen module
bool gallery_ensure_sd_initialized()
{
    return ensure_sd_initialized();
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
    {
        int palette_size = 0;
        const uint32_t *palette = nullptr;
        ui_get_palette(&palette, &palette_size);
        if (!palette || palette_size <= 0)
        {
            palette = PALETTE_CYBERPUNK;
            palette_size = PALETTE_CYBERPUNK_SIZE;
        }
        applyColorPalette(working.data(), out_w, out_h, palette, palette_size, 1, 2, 2);
    }
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
    if (SD.exists(path))
    {
        SD.remove(path);
    }

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

    if (SD.exists(path))
    {
        SD.remove(path);
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
        delay(120);
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

    // Ensure flash is turned off after capture completes
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
        }
        else if (s->id.PID == OV3660_PID)
        {
            s->set_hmirror(s, 1);
            //s->set_agc_gain(s, 30);


            // s->set_lenc(s, 1);

            // s->set_vflip(s, 0);

            //  s->set_aec2(s, 1);
            //  s->set_dcw(s, 1);
            //  s->set_bpc(s, 1);
            //  s->set_wpc(s, 1);
            // s->set_aec_value(s, 0);
            // s->set_ae_level(s, 2);

             //s->set_exposure_ctrl(s, 0);
            // s->set_whitebal(s, 0);
            // s->set_awb_gain(s, 0);
            // s->set_special_effect(s, 3);
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
    // Minimal setup: leave charge defaults, disable status LED, light measurement
    PMU.disableStatLed();
    return true;
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
