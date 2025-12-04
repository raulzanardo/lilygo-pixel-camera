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
#include <PNGenc.h>
#include <cstdlib>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
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
static uint32_t photo_counter = 0;
static PNGENC png_encoder;
static File png_file_handle;

static void *png_file_open_cb(const char *filename)
{
    png_file_handle = SD.open(filename, FILE_WRITE);
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
        Serial.println("Failed to mount SD card");
        return false;
    }

    sd_initialized = true;
    Serial.println("SD card ready");
    return true;
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

static bool save_frame_as_png(camera_fb_t *frame)
{
    if (!ensure_sd_initialized())
    {
        return false;
    }

    char path[32];
    snprintf(path, sizeof(path), "/photo_%lu.png", static_cast<unsigned long>(photo_counter++));

    int rc = png_encoder.open(path, png_file_open_cb, png_file_close_cb, png_file_read_cb, png_file_write_cb, png_file_seek_cb);
    if (rc != PNG_SUCCESS)
    {
        Serial.println("PNG open failed");
        return false;
    }

    rc = png_encoder.encodeBegin(frame->width, frame->height, PNG_PIXEL_TRUECOLOR, 24, nullptr, 3);
    if (rc != PNG_SUCCESS)
    {
        Serial.printf("encodeBegin failed: %d\n", rc);
        png_encoder.close();
        return false;
    }

    std::vector<uint8_t> temp_line(frame->width * 3);
    const uint16_t *src = reinterpret_cast<const uint16_t *>(frame->buf);
    for (int y = 0; y < frame->height; ++y)
    {
        const uint16_t *row = src + y * frame->width;
        for (int x = 0; x < frame->width; ++x)
        {
            uint16_t px = row[x];
            uint8_t r5 = (px >> 11) & 0x1F;
            uint8_t g6 = (px >> 5) & 0x3F;
            uint8_t b5 = px & 0x1F;
            temp_line[x * 3 + 0] = (r5 << 3) | (r5 >> 2);
            temp_line[x * 3 + 1] = (g6 << 2) | (g6 >> 4);
            temp_line[x * 3 + 2] = (b5 << 3) | (b5 >> 2);
        }

        rc = png_encoder.addLine(temp_line.data());
        if (rc != PNG_SUCCESS)
        {
            Serial.printf("addLine failed at row %d: %d\n", y, rc);
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

    Serial.printf("Saved photo to %s (%u x %u)\n", path, frame->width, frame->height);
    return true;
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
