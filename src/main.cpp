#include <lvgl.h>
#include <TFT_eSPI.h>
#include <ui.h>
#include "touch.h"
#include "utilities.h"
#include "esp_camera.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <vector>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
/* Change to your screen resolution */
static const uint16_t screenWidth  = 480;
static const uint16_t screenHeight = 222;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[ screenWidth * screenHeight / 6 ];

TFT_eSPI tft = TFT_eSPI(); /* TFT instance */

static const uint16_t cameraWidth  = 240;
static const uint16_t cameraHeight = 176;
static const uint32_t cameraTimerPeriodMs = 100;
static const uint32_t lvglTickPeriodMs    = 10;
static lv_img_dsc_t camera_img_dsc;
static uint8_t *camera_img_buf      = nullptr;
static bool camera_img_bound        = false;
static lv_timer_t *camera_timer     = nullptr;
static volatile bool camera_frame_pending = false;
static bool camera_processing             = false;
static TaskHandle_t cameraTaskHandle       = nullptr;

static bool init_camera_module();
static bool ensure_camera_buffer();
static void camera_timer_cb(lv_timer_t *timer);
static void start_camera_stream();
static void fix_rgb565_byte_order(size_t byte_count);
static void process_camera_frame();
static void camera_task(void *param);
static void camera_image_invalidate_async(void *param);

#if LV_USE_LOG != 0
/* Serial debugging */
void my_print(const char * buf)
{
    Serial.printf(buf);
    Serial.flush();
}

#endif
static std::vector<uint16_t> rotationBuffer;

static bool init_camera_module()
{
    camera_config_t config = {};
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
    config.xclk_freq_hz = 20000000; // XCLK_FREQ_HZ;
    config.pixel_format = PIXFORMAT_RGB565;
    config.frame_size = FRAMESIZE_HQVGA;
    config.jpeg_quality = 0;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    config.grab_mode = CAMERA_GRAB_LATEST;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        Serial.printf("Camera init failed: 0x%x\n", err);
        return false;
    }

    return true;
}

static bool ensure_camera_buffer()
{
    if (camera_img_buf) {
        return true;
    }

    const size_t buffer_size = cameraWidth * cameraHeight * 2;
    camera_img_buf = static_cast<uint8_t *>(heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!camera_img_buf) {
        camera_img_buf = static_cast<uint8_t *>(heap_caps_malloc(buffer_size, MALLOC_CAP_8BIT));
    }
    if (!camera_img_buf) {
        camera_img_buf = static_cast<uint8_t *>(malloc(buffer_size));
    }

    if (!camera_img_buf) {
        Serial.println("Camera buffer allocation failed");
        return false;
    }

    camera_img_dsc.header.always_zero = 0;
    camera_img_dsc.header.w           = cameraWidth;
    camera_img_dsc.header.h           = cameraHeight;
    camera_img_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    camera_img_dsc.data_size          = buffer_size;
    camera_img_dsc.data               = camera_img_buf;

    return true;
}

static void camera_timer_cb(lv_timer_t *timer)
{
    LV_UNUSED(timer);
    camera_frame_pending = true;
    if (cameraTaskHandle) {
        xTaskNotifyGive(cameraTaskHandle);
    }
}

static void start_camera_stream()
{
    if (!init_camera_module()) {
        return;
    }

    if (!ensure_camera_buffer()) {
        return;
    }

    if (camera_timer) {
        lv_timer_del(camera_timer);
        camera_timer = nullptr;
    }

   camera_timer = lv_timer_create(camera_timer_cb, cameraTimerPeriodMs, NULL);

    if (!cameraTaskHandle) {
        xTaskCreatePinnedToCore(camera_task, "camera_task", 4096, nullptr, 5, &cameraTaskHandle, 1);
    }
}

static void fix_rgb565_byte_order(size_t byte_count)
{
    if (!camera_img_buf) {
        return;
    }

    uint16_t *pixels = reinterpret_cast<uint16_t *>(camera_img_buf);
    const size_t px_count = byte_count / 2;

    for (size_t i = 0; i < px_count; ++i) {
        uint16_t color = pixels[i];
        pixels[i]      = static_cast<uint16_t>((color >> 8) | (color << 8));
    }
}

static void process_camera_frame()
{
    if (camera_processing || !camera_frame_pending) {
        return;
    }

    camera_processing     = true;
    camera_frame_pending  = false;

    if (!camera_img_buf) {
        camera_processing = false;
        return;
    }

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        Serial.println("Camera capture failed");
        camera_processing = false;
        return;
    }

    size_t copy_len = camera_img_dsc.data_size;
    if (fb->len < copy_len) {
        copy_len = fb->len;
    }

    memcpy(camera_img_buf, fb->buf, copy_len);
    fix_rgb565_byte_order(copy_len);
    if (copy_len < camera_img_dsc.data_size) {
        memset(camera_img_buf + copy_len, 0, camera_img_dsc.data_size - copy_len);
    }

    esp_camera_fb_return(fb);

    lv_async_call(camera_image_invalidate_async, NULL);
    camera_processing = false;
}

static void camera_image_invalidate_async(void *param)
{
    LV_UNUSED(param);

    if (ui_Image1 && !camera_img_bound) {
        lv_obj_set_size(ui_Image1, camera_img_dsc.header.w, camera_img_dsc.header.h);
        lv_img_set_src(ui_Image1, &camera_img_dsc);
        camera_img_bound = true;
    }

    if (camera_img_bound) {
        lv_obj_invalidate(ui_Image1);
    }
}

static void camera_task(void *param)
{
    LV_UNUSED(param);

    while (true) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        process_camera_frame();
    }
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
    if (touch_touched()) {
        data->state = LV_INDEV_STATE_PR;
        data->point.x = touch_last_x;
        data->point.y = touch_last_y;

        // Serial.print("Data x ");
        // Serial.println(touch_last_x);

        // Serial.print("Data y ");
        // Serial.println(touch_last_y);

        // Update the label with the touch coordinates
        //update_touch_coordinates(touch_last_x, touch_last_y);
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}


void setup()
{
    Serial.begin(115200); /* prepare for possible serial debug */

    String LVGL_Arduino = "Hello Arduino! ";
    LVGL_Arduino += String('V') + lv_version_major() + "." + lv_version_minor() + "." + lv_version_patch();

    Serial.println(LVGL_Arduino);
    Serial.println("I am LVGL_Arduino");

    lv_init();

#if LV_USE_LOG != 0
    lv_log_register_print_cb(my_print); /* register print function for debugging */
#endif

    tft.begin();          /* TFT init */
    tft.setRotation(3);   /* Landscape orientation, flipped */

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

    //start_camera_stream();
    //init_camera_module();
    Serial.println("Setup done");
}

void loop()
{
    lv_timer_handler(); /* let the GUI do its work */
    delay(lvglTickPeriodMs);
}
