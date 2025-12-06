#include "ui_GalleryScreen.h"
#include "ui_HomeScreen.h"
#include "../ui.h"
#include "lvgl.h"
#include <Arduino.h>
#include <FS.h>
#include <SD.h>
#include <vector>
#include <string>
#include <algorithm>
#include "../../../../src/utilities.h"

extern "C"
{
#include <extra/libs/png/lodepng.h>
}

// lodepng memory hooks (defined in main.cpp)
extern "C" void *lodepng_malloc(size_t size);
extern "C" void lodepng_free(void *ptr);

static lv_obj_t *gallery_screen = nullptr;
static lv_obj_t *gallery_list = nullptr;
static lv_obj_t *gallery_preview_screen = nullptr;
static lv_obj_t *gallery_preview_img = nullptr;
static lv_obj_t *gallery_preview_label = nullptr;
static lv_obj_t *gallery_preview_back_btn = nullptr;
static lv_obj_t *gallery_preview_delete_btn = nullptr;
static lv_img_dsc_t gallery_img_dsc;
static std::vector<uint8_t> gallery_img_buffer;
static std::string gallery_img_path;
static std::string gallery_current_photo_name;
static int gallery_page = 0;
static constexpr size_t GALLERY_PAGE_SIZE = 20;
static lv_obj_t *gallery_page_label = nullptr;
static lv_obj_t *gallery_prev_btn = nullptr;
static lv_obj_t *gallery_next_btn = nullptr;
static std::vector<String> gallery_photo_cache;

// SD helpers - defined externally
extern bool gallery_ensure_sd_initialized();

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
    if (!gallery_ensure_sd_initialized())
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

    std::sort(out_names.begin(), out_names.end(), [](const String &a, const String &b)
              {
        long ia = extract_photo_index(a);
        long ib = extract_photo_index(b);
        if (ia == ib)
        {
          return a > b;
        }
        return ia > ib; });
    return true;
}

static bool refresh_gallery_cache()
{
    gallery_photo_cache.clear();
    return list_captured_photos(gallery_photo_cache);
}

static void set_btn_enabled(lv_obj_t *btn, bool enabled)
{
    if (!btn)
    {
        return;
    }
    if (enabled)
    {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
    }
}

static void update_gallery_nav(size_t total_photos)
{
    if (!gallery_page_label || !gallery_prev_btn || !gallery_next_btn)
    {
        return;
    }

    size_t max_page = (total_photos == 0) ? 0 : (total_photos - 1) / GALLERY_PAGE_SIZE;
    if (gallery_page > static_cast<int>(max_page))
    {
        gallery_page = static_cast<int>(max_page);
    }
    set_btn_enabled(gallery_prev_btn, gallery_page > 0);
    set_btn_enabled(gallery_next_btn, static_cast<size_t>(gallery_page) < max_page);
    lv_label_set_text_fmt(gallery_page_label, "Page %d / %d", gallery_page + 1, static_cast<int>(max_page + 1));
}

static void populate_gallery_list();
static void show_photo_preview(const char *filename);
static void delete_photo_cb(lv_event_t *e);

static void back_to_camera_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    lv_scr_load_anim(ui_HomeScreen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
    ui_resume_camera_timer();
}

static void back_to_gallery_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    if (gallery_screen)
    {
        lv_scr_load_anim(gallery_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
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
        lv_obj_set_size(gallery_preview_back_btn, 44, 44);
        lv_obj_align(gallery_preview_back_btn, LV_ALIGN_TOP_LEFT, 8, 8);
        lv_obj_set_style_radius(gallery_preview_back_btn, 8, 0);
        lv_obj_t *label = lv_label_create(gallery_preview_back_btn);
        lv_label_set_text(label, LV_SYMBOL_LEFT);
        lv_obj_center(label);
        lv_obj_add_event_cb(gallery_preview_back_btn, back_to_gallery_cb, LV_EVENT_CLICKED, NULL);

        gallery_preview_delete_btn = lv_btn_create(gallery_preview_screen);
        lv_obj_set_size(gallery_preview_delete_btn, 100, 36);
        lv_obj_align(gallery_preview_delete_btn, LV_ALIGN_TOP_RIGHT, -12, 8);
        lv_obj_t *delete_label = lv_label_create(gallery_preview_delete_btn);
        lv_label_set_text(delete_label, "Delete");
        lv_obj_center(delete_label);
        lv_obj_add_event_cb(gallery_preview_delete_btn, delete_photo_cb, LV_EVENT_CLICKED, NULL);
    }

    gallery_img_path = "/";
    gallery_img_path += filename;
    gallery_current_photo_name = filename;
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

    lv_scr_load_anim(gallery_preview_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}

static void gallery_item_event_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }
    lv_obj_t *btn = lv_event_get_target(e);
    const char *name = (const char *)lv_obj_get_user_data(btn);
    if (!name)
    {
        return;
    }
    Serial.printf("Gallery item tapped: %s\n", name);
    show_photo_preview(name);
}

static void gallery_prev_page_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    if (gallery_page > 0)
    {
        gallery_page--;
        populate_gallery_list();
    }
}

static void gallery_next_page_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    size_t total = gallery_photo_cache.size();
    size_t max_page = (total == 0) ? 0 : (total - 1) / GALLERY_PAGE_SIZE;
    if (static_cast<size_t>(gallery_page) < max_page)
    {
        gallery_page++;
        populate_gallery_list();
    }
}

static void populate_gallery_list()
{
    if (!gallery_list)
    {
        return;
    }
    lv_obj_clean(gallery_list);

    if (!refresh_gallery_cache())
    {
        return;
    }

    size_t total = gallery_photo_cache.size();
    if (total == 0)
    {
        lv_obj_t *label = lv_label_create(gallery_list);
        lv_label_set_text(label, "No photos found");
        update_gallery_nav(total);
        return;
    }

    size_t start = static_cast<size_t>(gallery_page) * GALLERY_PAGE_SIZE;
    if (start >= total)
    {
        gallery_page = static_cast<int>(total / GALLERY_PAGE_SIZE);
        if (gallery_page < 0)
        {
            gallery_page = 0;
        }
        start = static_cast<size_t>(gallery_page) * GALLERY_PAGE_SIZE;
    }
    size_t end = std::min(total, start + GALLERY_PAGE_SIZE);

    for (size_t i = start; i < end; ++i)
    {
        const String &name = gallery_photo_cache[i];
        // Create a button with icon and text for horizontal layout
        lv_obj_t *btn = lv_btn_create(gallery_list);
        lv_obj_set_size(btn, 120, 100);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_all(btn, 4, 0);

        lv_obj_t *icon = lv_label_create(btn);
        lv_label_set_text(icon, LV_SYMBOL_IMAGE);
        lv_obj_set_style_text_font(icon, &lv_font_montserrat_14, 0);

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, name.c_str());
        lv_obj_set_style_text_font(label, &lv_font_montserrat_12, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, 110);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);

        // Store name as user data for event callback
        lv_obj_set_user_data(btn, (void *)gallery_photo_cache[i].c_str());
        lv_obj_add_event_cb(btn, gallery_item_event_cb, LV_EVENT_CLICKED, NULL);
    }

    update_gallery_nav(total);
}

static void delete_photo_cb(lv_event_t *e)
{
    if (lv_event_get_code(e) != LV_EVENT_CLICKED)
    {
        return;
    }

    if (gallery_current_photo_name.empty())
    {
        Serial.println("No photo selected for deletion");
        return;
    }

    if (!gallery_ensure_sd_initialized())
    {
        Serial.println("SD not ready, cannot delete photo");
        return;
    }

    std::string path = "/" + gallery_current_photo_name;
    if (SD.remove(path.c_str()))
    {
        Serial.printf("Deleted photo %s\n", path.c_str());
        gallery_current_photo_name.clear();
        populate_gallery_list();
        if (gallery_screen)
        {
            lv_scr_load_anim(gallery_screen, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 200, 0, false);
        }
    }
    else
    {
        Serial.printf("Failed to delete photo %s\n", path.c_str());
        lv_label_set_text_fmt(gallery_preview_label, "Delete failed: %s", path.c_str());
    }
}

static void build_gallery_screen()
{
    if (gallery_screen)
    {
        return;
    }

    Serial.println("Creating gallery screen");
    gallery_screen = lv_obj_create(NULL);
    lv_obj_set_size(gallery_screen, 480, 222);
    lv_obj_clear_flag(gallery_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(gallery_screen, 8, 0);
    lv_obj_set_style_pad_row(gallery_screen, 4, 0);
    lv_obj_set_flex_flow(gallery_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(gallery_screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Header row with back button and title
    lv_obj_t *header_row = lv_obj_create(gallery_screen);
    lv_obj_set_width(header_row, LV_PCT(100));
    lv_obj_set_height(header_row, 36);
    lv_obj_clear_flag(header_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(header_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header_row, 0, 0);
    lv_obj_set_style_pad_all(header_row, 0, 0);
    lv_obj_set_style_pad_column(header_row, 8, 0);
    lv_obj_set_flex_flow(header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *back_btn = lv_btn_create(header_row);
    lv_obj_set_size(back_btn, 36, 36);
    lv_obj_set_style_radius(back_btn, 8, 0);
    lv_obj_set_style_bg_color(back_btn, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_t *back_label = lv_label_create(back_btn);
    lv_label_set_text(back_label, LV_SYMBOL_LEFT);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back_btn, back_to_camera_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(header_row);
    lv_label_set_text(title, "Gallery");
    lv_obj_set_style_text_font(title, LV_FONT_DEFAULT, 0);

    // Gallery list - horizontal scrolling
    gallery_list = lv_obj_create(gallery_screen);
    lv_obj_set_size(gallery_list, LV_PCT(100), 130);
    lv_obj_set_flex_flow(gallery_list, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(gallery_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(gallery_list, 4, 0);
    lv_obj_set_style_pad_column(gallery_list, 8, 0);
    lv_obj_set_scroll_dir(gallery_list, LV_DIR_HOR);
    lv_obj_set_scroll_snap_x(gallery_list, LV_SCROLL_SNAP_START);

    // Pagination controls
    lv_obj_t *nav_row = lv_obj_create(gallery_screen);
    lv_obj_set_width(nav_row, LV_PCT(100));
    lv_obj_set_height(nav_row, 28);
    lv_obj_clear_flag(nav_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_opa(nav_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(nav_row, 0, 0);
    lv_obj_set_style_pad_all(nav_row, 0, 0);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    gallery_prev_btn = lv_btn_create(nav_row);
    lv_obj_set_size(gallery_prev_btn, 80, 24);
    lv_obj_t *prev_label = lv_label_create(gallery_prev_btn);
    lv_label_set_text(prev_label, "< Prev");
    lv_obj_center(prev_label);
    lv_obj_add_event_cb(gallery_prev_btn, gallery_prev_page_cb, LV_EVENT_CLICKED, NULL);

    gallery_page_label = lv_label_create(nav_row);
    lv_label_set_text(gallery_page_label, "Page 1 / 1");

    gallery_next_btn = lv_btn_create(nav_row);
    lv_obj_set_size(gallery_next_btn, 80, 24);
    lv_obj_t *next_label = lv_label_create(gallery_next_btn);
    lv_label_set_text(next_label, "Next >");
    lv_obj_center(next_label);
    lv_obj_add_event_cb(gallery_next_btn, gallery_next_page_cb, LV_EVENT_CLICKED, NULL);
}

lv_obj_t *ui_get_gallery_screen(void)
{
    return gallery_screen;
}

void ui_gallery_show(void)
{
    build_gallery_screen();
    Serial.println("Populating gallery list");
    populate_gallery_list();
    ui_pause_camera_timer();
    lv_scr_load_anim(gallery_screen, LV_SCR_LOAD_ANIM_MOVE_LEFT, 200, 0, false);
}
