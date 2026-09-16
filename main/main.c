#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "avatar_store.h"
#include "ble_remote.h"
#include "music_input.h"
#include "launcher.h"
#include "screenshot_bridge.h"
#include "ui_debug.h"
#include "voice_input.h"
#include "wallpaper_input.h"
#include "wifi_setup.h"
#include "companion_apps.h"
#include "notification_center.h"
#include "star_game.h"
#include "english_app.h"
#include "library_app.h"
#include "kids_apps.h"
#include "story_app.h"
#include "app_views.h"
#include "content_assets.h"
#include "service_config.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_spiffs.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "nvs.h"
#include "nvs_flash.h"

#define WALLPAPER_WIDTH 466
#define WALLPAPER_HEIGHT 466
#define WALLPAPER_STRIDE (WALLPAPER_WIDTH * 2)
#define WALLPAPER_BYTES (WALLPAPER_STRIDE * WALLPAPER_HEIGHT)
#define WIFI_READY_BIT BIT0
#define TIME_VALID_AFTER 1704067200L
#define CHECK_INTERVAL_MS 60000
#define ANIMATION_INTERVAL_MS 100
#define STAR_COUNT 28
#define AURORA_BAND_COUNT 8
#define HOME_BUTTON_GPIO GPIO_NUM_0
#define HOME_BUTTON_LONG_PRESS_MS 1200
#define AXP2101_ADDRESS 0x34
#define AXP2101_INTEN2 0x41
#define AXP2101_INTSTS2 0x49
#define AXP2101_PKEY_LONG_MASK BIT2
#define AXP2101_PKEY_SHORT_MASK BIT3
#define WALLPAPER_PATH "/wallpaper/current.rgb565"
#define WALLPAPER_TEMP_PATH "/wallpaper/current.tmp"
#define WEATHER_RESPONSE_BYTES 2048
#define WEATHER_REFRESH_MS (30 * 60 * 1000)
#define WEATHER_RETRY_MS 60000
#define QUOTA_RESPONSE_BYTES (128 * 1024)
#define QUOTA_MAX_AGENTS 8
#define QUOTA_REFRESH_MS (5 * 60 * 1000)
#define QUOTA_RETRY_MS 60000
#define CODEX_RESET_RESPONSE_BYTES 4096
#define CODEX_RESET_REFRESH_MS (5 * 60 * 1000)
#define CODEX_RESET_RETRY_MS 60000
#define WIFI_SCAN_MAX_RESULTS 4
#define APP_SWIPE_DISTANCE 72
#define APP_OVERVIEW_SWIPE_DISTANCE 58
#define APP_OVERVIEW_EDGE_Y 340

extern const uint8_t default_wallpaper_start[] asm("_binary_default_rgb565_start");
extern const uint8_t default_wallpaper_end[] asm("_binary_default_rgb565_end");
extern const uint8_t device_ca_start[] asm("_binary_device_ca_pem_start");
extern const uint8_t device_client_cert_start[] asm("_binary_device_client_pem_start");
extern const uint8_t device_client_key_start[] asm("_binary_device_client_key_start");

static const char *TAG = "round_clock";
static EventGroupHandle_t wifi_events;
static lv_obj_t *wallpaper_image;
static lv_obj_t *time_label;
static lv_obj_t *seconds_label;
static lv_obj_t *date_label;
static lv_obj_t *desktop_view;
static lv_obj_t *clock_view;
static lv_obj_t *weather_view;
static lv_obj_t *settings_view;
static lv_obj_t *settings_list;
static lv_obj_t *desktop_app_grid;

static lv_obj_t *wifi_view;
static lv_obj_t *remote_view;
static lv_obj_t *music_view;
static lv_obj_t *radio_view;
static lv_obj_t *wallpaper_studio_view;
static lv_obj_t *bluetooth_view;
static lv_obj_t *avatar_view;
static lv_obj_t *app_overview_view;
static lv_obj_t *app_overview_grid;
static lv_obj_t *quota_view;
static lv_obj_t *quota_list;
static lv_obj_t *quota_total_label;
static lv_obj_t *quota_update_label;
static lv_obj_t *quota_agent_name_labels[QUOTA_MAX_AGENTS];
static lv_obj_t *quota_agent_usage_labels[QUOTA_MAX_AGENTS];
static lv_obj_t *quota_agent_detail_labels[QUOTA_MAX_AGENTS];
static lv_obj_t *quota_agent_cards[QUOTA_MAX_AGENTS];
static lv_obj_t *codex_reset_view;
static lv_obj_t *codex_reset_state_label;
static lv_obj_t *codex_reset_time_label;
static lv_obj_t *codex_reset_summary_label;
static lv_obj_t *codex_reset_update_label;
static lv_obj_t *codex_reset_age_label;
static lv_obj_t *codex_reset_odds_label;
static lv_obj_t *codex_reset_meta_label;
static time_t codex_reset_forecast_expires;
static time_t codex_reset_event_epoch;
static time_t codex_reset_age_minute;
static uint32_t codex_reset_sync_count;
static lv_obj_t *avatar_image;
static lv_obj_t *avatar_status_label;
static lv_obj_t *avatar_sync_label;
static lv_obj_t *avatar_reaction_label;
static lv_obj_t *desktop_time_label;
static lv_obj_t *weather_city_label;
static lv_obj_t *weather_temp_label;
static lv_obj_t *weather_condition_label;
static lv_obj_t *weather_range_label;
static lv_obj_t *weather_humidity_label;
static lv_obj_t *weather_wind_label;
static lv_obj_t *weather_update_label;
static lv_obj_t *settings_wifi_label;
static lv_obj_t *settings_brightness_label;
static lv_obj_t *settings_volume_label;
static lv_obj_t *settings_notice_label;
static lv_obj_t *wifi_status_label;
static lv_obj_t *wifi_network_list;
static lv_obj_t *wifi_password_panel;
static lv_obj_t *wifi_selected_label;
static lv_obj_t *wifi_password_input;
static lv_obj_t *wifi_keyboard;
static lv_obj_t *wifi_setup_panel, *wifi_setup_qr, *wifi_setup_message;
static bool wifi_setup_was_active;
static lv_obj_t *remote_status_label;
static lv_obj_t *music_status_label;
static lv_obj_t *radio_status_label;
static lv_obj_t *wallpaper_studio_status_label;
static lv_obj_t *wallpaper_record_button;
static lv_obj_t *wallpaper_record_label;
static lv_obj_t *wallpaper_finish_button;
static lv_obj_t *wallpaper_finish_label;
static lv_obj_t *bluetooth_status_label;
static lv_image_dsc_t default_wallpaper;
static lv_image_dsc_t downloaded_wallpaper[2];
static lv_image_dsc_t avatar_image_descriptor;
static uint8_t *download_buffers[2];
static uint8_t *avatar_buffer;
static uint8_t *avatar_original;
static avatar_region_t avatar_regions[3];
static uint32_t avatar_face_started, avatar_idle_blink;
static int avatar_mood;
static bool load_avatar_slot_locked(uint8_t slot);
static int active_download_buffer;
static bool have_downloaded_wallpaper;
static bool showing_downloaded_wallpaper;
static volatile bool force_update_requested;
static TaskHandle_t wallpaper_refresh_task_handle;

typedef struct {
    char city[24];
    float temperature;
    float apparent;
    float high;
    float low;
    float wind;
    int humidity;
    int code;
} weather_data_t;

typedef struct {
    char name[32];
    uint64_t tokens;
    char quota_detail[80];
} quota_agent_t;

typedef struct {
    char generated_at[32];
    uint64_t total_tokens;
    uint8_t agent_count;
    quota_agent_t agents[QUOTA_MAX_AGENTS];
} quota_data_t;

typedef struct {
    char signal_id[32];
    char event_label[80];
    char event_time[48];
    char event_age[96];
    char forecast_meta[120];
    char details[1600];
    char update_label[100];
    bool forecast_available;
    bool remember_signal;
    int probability24;
    int probability48;
    time_t forecast_expires;
    time_t event_epoch;
    bool active;
    bool stale;
} codex_reset_data_t;

static volatile app_view_t current_app_view = APP_VIEW_CLOCK;
static volatile int requested_app_view = -1;
static volatile int requested_screen_power = -1;
static volatile bool screen_on = true;
static volatile bool weather_refresh_requested = true;
static volatile bool weather_data_pending;
static volatile bool weather_error_pending;
static portMUX_TYPE weather_data_mux = portMUX_INITIALIZER_UNLOCKED;
static weather_data_t pending_weather_data;
static volatile bool quota_refresh_requested = true;
static volatile bool quota_data_pending;
static volatile bool quota_error_pending;
static portMUX_TYPE quota_data_mux = portMUX_INITIALIZER_UNLOCKED;
static quota_data_t pending_quota_data;
static volatile bool codex_reset_refresh_requested = true;
static volatile bool codex_reset_data_pending;
static volatile bool codex_reset_error_pending;
static portMUX_TYPE codex_reset_data_mux = portMUX_INITIALIZER_UNLOCKED;
static codex_reset_data_t pending_codex_reset_data;
static bool touch_long_pressed;
static lv_point_t touch_start;
static lv_point_t avatar_touch_start;
static uint8_t current_avatar_slot = 0xFF;
static uint32_t avatar_revision_seen;
static uint8_t avatar_reaction_index;
static uint8_t avatar_reaction_seconds;
static uint8_t avatar_cycle_seconds;
static uint32_t animation_frame;
static i2c_master_dev_handle_t axp2101;
static uint8_t display_brightness = 100;
static uint8_t audio_volume = 70;
static bool use_chinese = true;
static bool use_ironman_theme;
static uint32_t voice_animation_frame;
static wifi_ap_record_t scanned_networks[WIFI_SCAN_MAX_RESULTS];
static uint16_t scanned_network_count;
static char selected_wifi_ssid[sizeof(((wifi_config_t *)0)->sta.ssid)];
static const char *radio_prompt_prefix =
    "AI radio station: Night sky focus. Create an instrumental radio track with a clear mood.";
static bool radio_channel_selected;
static int64_t radio_channel_feedback_until;
static lv_obj_t *radio_station_buttons[3];
static lv_obj_t *record_controls[4], *finish_controls[4];
static const char *radio_station_prompts[3] = {
    "AI radio station: Night sky focus. Create an instrumental radio track with a calm forward pulse.",
    "AI radio station: Aurora sleep. Create an instrumental ambient radio track, slow, spacious and warm.",
    "AI radio station: Energy core. Create an instrumental electronic radio track with an optimistic beat.",
};

static const app_view_t desktop_launcher_targets[] = {
    APP_VIEW_CLOCK, APP_VIEW_WEATHER, APP_VIEW_AVATAR, APP_VIEW_REMOTE,
    APP_VIEW_SETTINGS, APP_VIEW_QUOTA, APP_VIEW_CODEX_RESET, APP_VIEW_MUSIC,
    APP_VIEW_RADIO, APP_VIEW_WALLPAPER_STUDIO, APP_VIEW_AGENTS, APP_VIEW_NOTES,
    APP_VIEW_STARS,
    APP_VIEW_ENGLISH,
    APP_VIEW_LIBRARY,
    APP_VIEW_PET, APP_VIEW_STORY, APP_VIEW_FLIP,
};


static const char *desktop_launcher_english_names[] = {
    "CLOCK", "WEATHER", "AVATAR", "VOICE", "SETTINGS", "USAGE", "RESET", "MUSIC", "RADIO", "WALLPAPER", "AGENTS", "NOTES", "LITTLE STARS", "PICTURE WORDS", "MY CREATIONS", "LITTLE PET", "STORIES", "MEMORY CARDS",
};
static const char *desktop_launcher_chinese_names[] = {
    "时钟", "天气", "头像", "联网语音", "设置", "用量", "重置预警", "音乐创作", "AI电台", "动态壁纸", "工作台", "随手记", "点点小星星", "看图英语", "作品相册", "电子宠物", "互动故事屋", "记忆翻牌",
};


static void show_view_locked(app_view_t view);
static const char *localized(const char *english, const char *chinese);
static void wifi_refresh_status_locked(void);
static void remote_refresh_status_locked(void);
static void music_refresh_status_locked(void);
static void radio_refresh_status_locked(void);
static void wallpaper_studio_refresh_status_locked(void);
static void bluetooth_refresh_status_locked(void);
static void wifi_setup_refresh_locked(void);
LV_FONT_DECLARE(round_clock_cn_16);
LV_FONT_DECLARE(round_clock_cn_20);
LV_FONT_DECLARE(round_clock_cn_28);

typedef struct {
    lv_obj_t *object;
    int16_t base_y;
    uint8_t phase;
    uint8_t speed;
} star_t;

typedef struct {
    lv_obj_t *object;
    int16_t base_y;
    uint8_t phase;
} aurora_band_t;

#define VOICE_WAVE_BAR_COUNT 9
typedef struct {
    lv_obj_t *bars[VOICE_WAVE_BAR_COUNT];
    int16_t center_y;
    uint32_t color;
} voice_wave_t;

static star_t stars[STAR_COUNT];
static aurora_band_t aurora_bands[AURORA_BAND_COUNT];
static voice_wave_t remote_voice_wave;
static voice_wave_t music_voice_wave;
static voice_wave_t radio_voice_wave;
static voice_wave_t wallpaper_voice_wave;

/* The clock keeps its own face. Other apps use the light system material: a
 * neutral canvas, translucent white controls, and familiar blue interaction. */
static void apply_apple_material_tree(lv_obj_t *object)
{
    uint32_t index = 0;
    lv_obj_t *child;
    while ((child = lv_obj_get_child(object, index++)) != NULL) {
        int32_t width = lv_obj_get_width(child);
        int32_t height = lv_obj_get_height(child);
        if (lv_obj_check_type(child, &lv_button_class) && width < 400 && height < 400) {
            lv_obj_set_style_bg_color(child, lv_color_white(), 0);
            lv_obj_set_style_bg_grad_dir(child, LV_GRAD_DIR_NONE, 0);
            lv_obj_set_style_bg_opa(child, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(child, 1, 0);
            lv_obj_set_style_border_color(child, lv_color_hex(0xD1D1D6), 0);
            lv_obj_set_style_border_opa(child, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(child,
                                    abs(width - height) < 10 ? LV_RADIUS_CIRCLE : 22, 0);
            lv_obj_set_style_shadow_width(child, 0, 0);
            lv_obj_set_style_shadow_color(child, lv_color_hex(0x8E8E93), 0);
            lv_obj_set_style_shadow_opa(child, LV_OPA_20, 0);
        }
        if (lv_obj_check_type(child, &lv_label_class)) {
            lv_obj_set_style_text_color(child,
                                        lv_obj_check_type(lv_obj_get_parent(child),
                                                          &lv_button_class)
                                            ? lv_color_hex(0x007AFF) : lv_color_hex(0x1C1C1E), 0);
        }
        apply_apple_material_tree(child);
    }
}

static void apply_apple_app_style(lv_obj_t *view)
{
    lv_obj_set_style_bg_color(view, lv_color_hex(0xF4F5F8), 0);
    lv_obj_set_style_bg_grad_color(view, lv_color_hex(0xE5E5EA), 0);
    lv_obj_set_style_bg_opa(view, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_grad_dir(view, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_border_width(view, 0, 0);
    apply_apple_material_tree(view);
}

static void apply_home_icon_tints(lv_obj_t *grid)
{
    unsigned app = 0;
    for (uint32_t i = 0; i < lv_obj_get_child_count(grid); ++i) {
        lv_obj_t *child = lv_obj_get_child(grid, i);
        if (lv_obj_check_type(child, &lv_button_class)) launcher_decorate_icon(child, app++);
    }
}

static void time_sync_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Network time synchronized: %lld", (long long)tv->tv_sec);
}

static bool time_is_valid(time_t value)
{
    return value >= TIME_VALID_AFTER;
}

static int current_day_key(void)
{
    time_t now;
    struct tm local;
    time(&now);
    if (!time_is_valid(now) || localtime_r(&now, &local) == NULL) {
        return 0;
    }
    return (local.tm_year + 1900) * 1000 + local.tm_yday + 1;
}

static void set_status(const char *text)
{
    ESP_LOGI(TAG, "%s", text);
}

static void show_wallpaper_locked(bool downloaded)
{
    if (downloaded && have_downloaded_wallpaper) {
        lv_image_set_src(wallpaper_image, &downloaded_wallpaper[active_download_buffer]);
        showing_downloaded_wallpaper = true;
    } else {
        lv_image_set_src(wallpaper_image, &default_wallpaper);
        showing_downloaded_wallpaper = false;
    }
    lv_obj_invalidate(wallpaper_image);
}

static void show_wallpaper(bool downloaded)
{
    if (bsp_display_lock(-1) != ESP_OK) {
        return;
    }
    show_wallpaper_locked(downloaded);
    bsp_display_unlock();
}

static const char *weather_condition(int code)
{
    if (code == 0) {
        return use_chinese ? "晴" : "CLEAR";
    }
    if (code >= 1 && code <= 3) {
        return use_chinese ? "多云" : "CLOUDY";
    }
    if (code == 45 || code == 48) {
        return use_chinese ? "雾" : "FOG";
    }
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) {
        return use_chinese ? "有雨" : "RAIN";
    }
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) {
        return use_chinese ? "有雪" : "SNOW";
    }
    if (code >= 95) {
        return use_chinese ? "雷暴" : "STORM";
    }
    return use_chinese ? "天气" : "WEATHER";
}

static void apply_pending_weather_locked(void)
{
    if (!weather_data_pending || weather_temp_label == NULL) {
        return;
    }

    weather_data_t weather;
    taskENTER_CRITICAL(&weather_data_mux);
    weather = pending_weather_data;
    weather_data_pending = false;
    taskEXIT_CRITICAL(&weather_data_mux);

    char text[64];
    const char *city = use_chinese && strcasecmp(weather.city, "Beijing") == 0
                           ? "北京" : weather.city;
    lv_label_set_text(weather_city_label, city);
    snprintf(text, sizeof(text), "%.1f°", weather.temperature);
    lv_label_set_text(weather_temp_label, text);
    if (use_chinese) {
        snprintf(text, sizeof(text), "%s  体感 %.0f度",
                 weather_condition(weather.code), weather.apparent);
    } else {
        snprintf(text, sizeof(text), "%s  FEELS %.0f C",
                 weather_condition(weather.code), weather.apparent);
    }
    lv_label_set_text(weather_condition_label, text);
    if (use_chinese) {
        snprintf(text, sizeof(text), "最高 %.0f度   最低 %.0f度", weather.high, weather.low);
    } else {
        snprintf(text, sizeof(text), "H %.0f C   L %.0f C", weather.high, weather.low);
    }
    lv_label_set_text(weather_range_label, text);
    snprintf(text, sizeof(text), use_chinese ? "湿度\n%d%%" : "HUMIDITY\n%d%%",
             weather.humidity);
    lv_label_set_text(weather_humidity_label, text);
    snprintf(text, sizeof(text), use_chinese ? "风速\n%.0f km/h" : "WIND\n%.0f km/h",
             weather.wind);
    lv_label_set_text(weather_wind_label, text);
    lv_label_set_text(weather_update_label, use_chinese ? "刚刚更新" : "UPDATED NOW");
}

static void format_compact_tokens(uint64_t value, char *buffer, size_t size)
{
    if (value >= 1000000000ULL) {
        snprintf(buffer, size, "%.1fB", value / 1000000000.0);
    } else if (value >= 1000000ULL) {
        snprintf(buffer, size, "%.1fM", value / 1000000.0);
    } else if (value >= 1000ULL) {
        snprintf(buffer, size, "%.1fK", value / 1000.0);
    } else {
        snprintf(buffer, size, "%llu", (unsigned long long)value);
    }
}

static void apply_pending_quota_locked(void)
{
    if (quota_view == NULL) {
        return;
    }
    if (quota_error_pending) {
        quota_error_pending = false;
        if (!quota_data_pending) {
            lv_label_set_text(quota_update_label,
                              use_chinese ? "同步失败" : "SYNC FAILED");
        }
    }
    if (!quota_data_pending) {
        return;
    }

    quota_data_t quota;
    taskENTER_CRITICAL(&quota_data_mux);
    quota = pending_quota_data;
    quota_data_pending = false;
    taskEXIT_CRITICAL(&quota_data_mux);

    char text[64];
    format_compact_tokens(quota.total_tokens, text, sizeof(text));
    lv_label_set_text(quota_total_label, text);
    snprintf(text, sizeof(text), "LLMQuota %.10s %.5s UTC",
             quota.generated_at, strlen(quota.generated_at) > 11 ? quota.generated_at + 11 : "--:--");
    lv_label_set_text(quota_update_label, text);
    for (size_t i = 0; i < QUOTA_MAX_AGENTS; ++i) {
        bool visible = i < quota.agent_count;
        if (!visible) {
            lv_obj_add_flag(quota_agent_cards[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(quota_agent_cards[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(quota_agent_name_labels[i], quota.agents[i].name);
        char tokens[24];
        format_compact_tokens(quota.agents[i].tokens, tokens, sizeof(tokens));
        snprintf(text, sizeof(text), use_chinese ? "30天 %s" : "30D %s", tokens);
        lv_label_set_text(quota_agent_detail_labels[i], quota.agents[i].quota_detail);

        lv_label_set_text(quota_agent_usage_labels[i], text);
    }
}

static void load_last_codex_reset_id(char *buffer, size_t size)
{
    buffer[0] = '\0';
    nvs_handle_t handle;
    if (nvs_open("alerts", NVS_READONLY, &handle) == ESP_OK) {
        size_t required = size;
        if (nvs_get_str(handle, "codex_reset", buffer, &required) != ESP_OK) {
            buffer[0] = '\0';
        }
        nvs_close(handle);
    }
}

static void save_last_codex_reset_id(const char *signal_id)
{
    nvs_handle_t handle;
    if (nvs_open("alerts", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_str(handle, "codex_reset", signal_id);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void reset_hide_forecast(const char *reason)
{
    codex_reset_forecast_expires = 0;
    lv_label_set_text(codex_reset_odds_label, "--          --");
    lv_label_set_text(codex_reset_meta_label, reason);
}

static void apply_pending_codex_reset_locked(void)
{
    if (codex_reset_view == NULL) return;
    time_t now = time(NULL);
    if (codex_reset_event_epoch > 0 && now >= codex_reset_event_epoch &&
        now / 60 != codex_reset_age_minute) {
        codex_reset_age_minute = now / 60;
        int age = (int)(now - codex_reset_event_epoch);
        if (age >= 86400) {
            lv_label_set_text_fmt(codex_reset_age_label, "%d天%d小时前 · 历史记录", age / 86400, age % 86400 / 3600);
        } else {
            lv_label_set_text_fmt(codex_reset_age_label, "%d小时%d分钟前 · 非当前发生", age / 3600, age % 3600 / 60);
        }
    }
    if (codex_reset_forecast_expires &&
        (time(NULL) < 1700000000 || time(NULL) > codex_reset_forecast_expires)) {
        reset_hide_forecast("预测已过期，请刷新");
    }
    if (codex_reset_error_pending) {
        codex_reset_error_pending = false;
        if (!codex_reset_data_pending) {
            lv_label_set_text(codex_reset_update_label, "同步失败 · 保留历史时间");
            reset_hide_forecast("同步失败，暂停显示预测");
        }
    }
    if (!codex_reset_data_pending) return;

    codex_reset_data_t *data = heap_caps_malloc(sizeof(*data), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) return;
    taskENTER_CRITICAL(&codex_reset_data_mux);
    *data = pending_codex_reset_data;
    ++codex_reset_sync_count;
    codex_reset_data_pending = false;
    taskEXIT_CRITICAL(&codex_reset_data_mux);

    char previous_id[32];
    load_last_codex_reset_id(previous_id, sizeof(previous_id));
    bool first_signal = previous_id[0] == '\0';
    bool changed = data->signal_id[0] != '\0' &&
                   strcmp(previous_id, data->signal_id) != 0;
    bool should_alert = changed && !first_signal && data->active && !data->stale;
    if (changed && (first_signal || (!data->stale && data->remember_signal))) save_last_codex_reset_id(data->signal_id);
    lv_label_set_text(codex_reset_state_label, data->event_label);
    lv_label_set_text(codex_reset_time_label, data->event_time);
    lv_label_set_text(codex_reset_age_label, data->event_age);
    codex_reset_event_epoch = data->event_epoch;
    codex_reset_age_minute = 0;
    lv_label_set_text(codex_reset_summary_label, data->details);
    lv_label_set_text(codex_reset_update_label, data->update_label);
    if (data->forecast_available && time(NULL) >= 1700000000 &&
        time(NULL) <= data->forecast_expires) {
        codex_reset_forecast_expires = data->forecast_expires;
        lv_label_set_text_fmt(codex_reset_odds_label, "%d%%          %d%%",
                              data->probability24, data->probability48);
        lv_label_set_text(codex_reset_meta_label, data->forecast_meta);
    } else {
        reset_hide_forecast("预测不可用或已过期");
    }

    if (should_alert) {
        char notice_key[64]; snprintf(notice_key,sizeof(notice_key),"codex-reset:%s",data->signal_id);
        notification_post(notice_key, "发现新的重置信息，点此查看摘要", APP_VIEW_CODEX_RESET, NOTICE_DONE);
        if (!screen_on) {
            bsp_display_backlight_on();
            bsp_display_brightness_set(display_brightness);
            screen_on = true;
        }
        show_view_locked(APP_VIEW_CODEX_RESET);
        ESP_LOGW(TAG, "New Codex reset signal: %s", data->signal_id);
    }
    free(data);
}

static void load_system_settings(void)
{
    nvs_handle_t handle;
    uint8_t saved_brightness = 0;
    uint8_t saved_volume = 0;
    uint8_t saved_language = 1;
    uint8_t saved_theme = 0;
    if (nvs_open("system", NVS_READONLY, &handle) == ESP_OK) {
        if (nvs_get_u8(handle, "brightness", &saved_brightness) == ESP_OK &&
            saved_brightness >= 10 && saved_brightness <= 100) {
            display_brightness = saved_brightness;
        }
        if (nvs_get_u8(handle, "volume", &saved_volume) == ESP_OK && saved_volume <= 100) {
            audio_volume = saved_volume;
        }
        if (nvs_get_u8(handle, "language_v2", &saved_language) == ESP_OK) {
            use_chinese = saved_language != 0;
        }
        if (nvs_get_u8(handle, "theme", &saved_theme) == ESP_OK) {
            use_ironman_theme = saved_theme != 0;
        }
        nvs_close(handle);
    }
}

static void save_system_settings(void)
{
    nvs_handle_t handle;
    if (nvs_open("system", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "brightness", display_brightness);
        nvs_set_u8(handle, "volume", audio_volume);
        nvs_set_u8(handle, "language_v2", use_chinese ? 1 : 0);
        nvs_set_u8(handle, "theme", use_ironman_theme ? 1 : 0);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static const char *localized(const char *english, const char *chinese)
{
    return use_chinese ? chinese : english;
}

static const lv_font_t *localized_font(const lv_font_t *english)
{
    return use_chinese ? &round_clock_cn_20 : english;
}

static const lv_font_t *localized_title_font(const lv_font_t *english)
{
    return use_chinese ? &round_clock_cn_28 : english;
}

static const char *localized_remote_status(const char *status)
{
    if (!use_chinese) {
        return status;
    }
    static const struct {
        const char *english;
        const char *chinese;
    } translations[] = {
        {"BLUETOOTH OFF", "蓝牙未启动"},
        {"BLUETOOTH READY", "蓝牙已就绪"},
        {"BLUETOOTH START FAILED", "蓝牙启动失败"},
        {"BLUETOOTH UNAVAILABLE", "蓝牙不可用"},
        {"PAIRING OPEN 5 MINUTES", "请在电脑上连接"},
        {"PAIRING CLOSED", "配对已关闭"},
        {"DEVICE CONNECTED - VERIFYING", "蓝牙已连接，正在验证"},
        {"DEVICE DISCONNECTED", "蓝牙已断开"},
        {"DEVICE CONNECTED - SECURED", "蓝牙已安全连接"},
        {"PAIRING FAILED", "配对失败"},
        {"MICROPHONE FAILED", "麦克风异常"},
        {"RECORDING TOO SHORT", "录音时间太短"},
        {"SENDING AUDIO TO MAC...", "正在发送录音"},
        {"RECOGNIZING ON MAC...", "正在本地识别"},
        {"TEXT ENTERED - OFFLINE", "离线文字已输入"},
        {"VOICE INPUT FAILED", "语音输入失败"},
        {"RECORDING - TAP CONFIRM", "正在录音，点击确认"},
        {"FINISHING RECORDING...", "正在结束录音"},
        {"READY FOR ONLINE VOICE", "联网语音已就绪"},
        {"RECORDING - TAP STOP", "正在录音，点击停止"},
        {"RECOGNIZING ONLINE...", "正在联网识别"},
        {"ONLINE VOICE FAILED", "联网语音失败"},
        {"ONLINE VOICE RECOGNIZED", "联网语音已识别"},
        {"READY FOR MUSIC IDEA", "说出音乐想法"},
        {"RECORDING MUSIC IDEA - TAP STOP", "正在录音，点击停止"},
        {"RECOGNIZING MUSIC IDEA...", "正在识别想法"},
        {"CREATING MUSIC...", "正在生成音乐"},
        {"MUSIC READY - TAP PLAY", "音乐已生成，点击试听"},
        {"PLAYING MUSIC...", "正在试听"},
        {"MUSIC CREATION FAILED", "音乐生成失败"},
        {"MUSIC PLAYBACK FAILED", "音乐试听失败"},
        {"MUSIC IDEA FAILED", "音乐想法失败"},
        {"READY FOR WALLPAPER IDEA", "说出壁纸画面"},
        {"RECORDING WALLPAPER IDEA - TAP STOP", "正在录音，点击停止"},
        {"RECOGNIZING WALLPAPER IDEA...", "正在识别画面"},
        {"CREATING WALLPAPER...", "正在生成壁纸"},
        {"WALLPAPER READY - UPDATING", "壁纸已生成，正在更新"},
        {"CURRENT WALLPAPER SAVED ON DEVICE", "壁纸已保存"},
        {"WALLPAPER SAVED ON DEVICE", "壁纸已保存"},
        {"WALLPAPER UPDATE RETRYING", "正在更新"},
        {"WALLPAPER CREATION FAILED", "壁纸生成失败"},
        {"WALLPAPER IDEA FAILED", "壁纸想法失败"},
        {"PHOTO READY - OPEN AVATAR", "大头贴已上传"},
        {"PHOTO SAVE FAILED", "照片保存失败"},
        {"RECEIVING PHOTO FROM MAC...", "正在接收照片"},
    };
    for (size_t i = 0; i < sizeof(translations) / sizeof(translations[0]); ++i) {
        if (strcmp(status, translations[i].english) == 0) {
            return translations[i].chinese;
        }
    }
    if (strncmp(status, "PAIR CODE ", 10) == 0) {
        static char pairing_code[32];
        snprintf(pairing_code, sizeof(pairing_code), "配对码 %s", status + 10);
        return pairing_code;
    }
    return status;
}

static lv_color_t theme_background_top(void)
{
    return lv_color_hex(use_ironman_theme ? 0x160507 : 0x07111F);
}

static lv_color_t theme_background_bottom(void)
{
    return lv_color_hex(use_ironman_theme ? 0x160507 : 0x12335B);
}

static lv_color_t theme_card(void)
{
    return lv_color_hex(use_ironman_theme ? 0x41100D : 0x123F64);
}

static lv_color_t theme_accent(void)
{
    return lv_color_hex(use_ironman_theme ? 0xFFB000 : 0x4DBEE3);
}

static void refresh_settings_locked(void)
{
    if (settings_brightness_label != NULL) {
        lv_label_set_text_fmt(settings_brightness_label, "%u%%", display_brightness);
    }
    if (settings_volume_label != NULL) {
        lv_label_set_text_fmt(settings_volume_label, "%u%%", audio_volume);
    }
}

static void update_record_controls(unsigned app, const char *status, bool recording)
{
    static const uint32_t colors[] = {0x147DF5, 0xA44BC4, 0x596DEB, 0x129EAF};
    bool busy = strstr(status, "RECOGNIZING") || strstr(status, "CREATING") ||
                strstr(status, "PLAYING") || strstr(status, "FINISHING");
    bool ready = !recording && !busy && !voice_input_is_recording() &&
                 strcmp(voice_input_status(), "RECOGNIZING ONLINE...") != 0;
    if (app == 3 && wallpaper_input_has_pending_update()) ready = false;
    lv_obj_t *buttons[] = {record_controls[app], finish_controls[app]};
    bool enabled[] = {ready, recording};
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *button = buttons[i];
        if (!button) continue;
        if (enabled[i]) lv_obj_remove_state(button, LV_STATE_DISABLED);
        else lv_obj_add_state(button, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(button, lv_color_hex(enabled[i] ? colors[app] : 0xE4E6ED), 0);
        lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(button, 0, 0);
        lv_obj_t *label = lv_obj_get_child(button, 0);
        if (label) lv_obj_set_style_text_color(label, lv_color_hex(enabled[i] ? 0xFFFFFF : 0x737988), 0);
    }
}

static void remote_refresh_status_locked(void)
{
    update_record_controls(0, voice_input_status(), voice_input_is_recording());
    if (remote_status_label != NULL) {
        lv_label_set_text(remote_status_label,
                          localized_remote_status(voice_input_status()));
    }
}

static void music_refresh_status_locked(void)
{
    update_record_controls(1, music_input_status(), music_input_is_recording() && voice_input_is_recording());
    if (music_status_label != NULL) {
        lv_label_set_text(music_status_label,
                          localized_remote_status(music_input_status()));
    }
}

static void radio_refresh_status_locked(void)
{
    update_record_controls(2, music_input_status(), music_input_is_recording() && voice_input_is_recording());
    if (radio_status_label != NULL) {
        const char *status = music_input_status();
        bool selected_feedback = esp_timer_get_time() < radio_channel_feedback_until &&
                                 (strcmp(status, "MUSIC READY - TAP PLAY") == 0 ||
                                  strcmp(status, "MUSIC IDEA FAILED") == 0);
        if (strcmp(status, "READY FOR MUSIC IDEA") == 0 || selected_feedback) {
            lv_label_set_text(radio_status_label,
                              localized("PICK A CHANNEL, THEN SPEAK",
                                        radio_channel_selected ? "频道已选择，说出感受" : "选择频道，再说感受"));
        } else {
            lv_label_set_text(radio_status_label, localized_remote_status(music_input_status()));
        }
    }
}

static void wallpaper_studio_refresh_status_locked(void)
{
    if (wallpaper_studio_status_label != NULL) {
        lv_label_set_text(wallpaper_studio_status_label,
                          localized_remote_status(wallpaper_input_status()));
    }
    if (wallpaper_record_button != NULL && wallpaper_record_label != NULL) {
        bool recording = strcmp(wallpaper_input_status(),
                                "RECORDING WALLPAPER IDEA - TAP STOP") == 0;
        bool creating = strcmp(wallpaper_input_status(),
                               "RECOGNIZING WALLPAPER IDEA...") == 0 ||
                        strcmp(wallpaper_input_status(), "CREATING WALLPAPER...") == 0;
        lv_obj_set_style_bg_color(wallpaper_record_button,
                                  recording ? lv_color_hex(0x007AFF) : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(wallpaper_record_button,
                                recording ? LV_OPA_COVER : LV_OPA_80, 0);
        lv_obj_set_style_border_color(wallpaper_record_button,
                                      recording ? lv_color_hex(0x007AFF)
                                                : lv_color_hex(0xD1D1D6), 0);
        lv_obj_set_style_text_color(wallpaper_record_label,
                                    recording ? lv_color_white() : lv_color_hex(0x007AFF), 0);
        lv_label_set_text(wallpaper_record_label,
                          recording ? localized("RECORDING\nTAP STOP", "正在录音\n点击停止")
                                    : localized("SPEAK\nSCENE", "描述画面"));
        if (wallpaper_finish_button != NULL && wallpaper_finish_label != NULL) {
            lv_obj_set_style_bg_color(wallpaper_finish_button,
                                      creating ? lv_color_hex(0x007AFF) : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(wallpaper_finish_button,
                                    creating ? LV_OPA_COVER : LV_OPA_80, 0);
            lv_obj_set_style_border_color(wallpaper_finish_button,
                                          creating ? lv_color_hex(0x007AFF)
                                                   : lv_color_hex(0xD1D1D6), 0);
            lv_obj_set_style_text_color(wallpaper_finish_label,
                                        creating ? lv_color_white() : lv_color_hex(0x007AFF), 0);
            lv_label_set_text(wallpaper_finish_label,
                              creating ? localized("CREATING...", "正在生成")
                                       : localized("STOP\nCREATE", "生成壁纸"));
        }
    }
    update_record_controls(3, wallpaper_input_status(),
                           strcmp(wallpaper_input_status(), "RECORDING WALLPAPER IDEA - TAP STOP") == 0 && voice_input_is_recording());
}

static void bluetooth_refresh_status_locked(void)
{
    if (bluetooth_status_label != NULL) {
        lv_label_set_text(bluetooth_status_label,
                          localized_remote_status(vibe_remote_status()));
    }
}

static void set_display_brightness(uint8_t brightness)
{
    display_brightness = brightness;
    if (bsp_display_brightness_set(display_brightness) != ESP_OK) {
        ESP_LOGW(TAG, "Display brightness update failed");
    }
    save_system_settings();
    refresh_settings_locked();
}

static void clock_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    int requested_power = requested_screen_power;
    if (requested_power >= 0) {
        requested_screen_power = -1;
        if (requested_power) {
            bsp_display_backlight_on();
            bsp_display_brightness_set(display_brightness);
        } else {
            bsp_display_backlight_off();
        }
        screen_on = requested_power != 0;
        star_game_set_active(screen_on && current_app_view == APP_VIEW_STARS);
        english_app_set_active(screen_on && current_app_view == APP_VIEW_ENGLISH);
        library_app_set_active(screen_on && current_app_view == APP_VIEW_LIBRARY);
        kids_apps_activate(screen_on?(current_app_view==APP_VIEW_PET?0:current_app_view==APP_VIEW_FLIP?1:-1):-1);
        story_app_set_active(screen_on && current_app_view==APP_VIEW_STORY);
        ESP_LOGI(TAG, "Screen switched: %s", screen_on ? "on" : "off");
    }

    int requested_view = requested_app_view;
    if (requested_view >= 0) {
        requested_app_view = -1;
        show_view_locked((app_view_t)requested_view);
        const char *name = requested_view == APP_VIEW_CLOCK ? "clock" :
                           requested_view == APP_VIEW_WEATHER ? "weather" :
                           requested_view == APP_VIEW_SETTINGS ? "settings" :
                           requested_view == APP_VIEW_REMOTE ? "vibe remote" :
                           requested_view == APP_VIEW_BLUETOOTH ? "bluetooth" :
                           requested_view == APP_VIEW_QUOTA ? "quota" :
                           requested_view == APP_VIEW_CODEX_RESET ? "codex reset" :
                           requested_view == APP_VIEW_AVATAR ? "avatar" : "desktop";
        ESP_LOGI(TAG, "View switched: %s", name);
    }

    apply_pending_weather_locked();
    apply_pending_quota_locked();
    apply_pending_codex_reset_locked();
    wifi_setup_refresh_locked();
    companion_apps_tick(screen_on);
    notification_center_tick();
    if (current_app_view == APP_VIEW_REMOTE) remote_refresh_status_locked();
    if (current_app_view == APP_VIEW_MUSIC) music_refresh_status_locked();
    if (current_app_view == APP_VIEW_RADIO) radio_refresh_status_locked();
    if (current_app_view == APP_VIEW_WALLPAPER_STUDIO) wallpaper_studio_refresh_status_locked();
    if (current_app_view == APP_VIEW_BLUETOOTH) bluetooth_refresh_status_locked();
    if (wallpaper_input_take_reload_request()) {
        force_update_requested = true;
        if (wallpaper_refresh_task_handle != NULL) {
            xTaskNotifyGive(wallpaper_refresh_task_handle);
        }
    }
    uint32_t avatar_revision = avatar_store_revision();
    if (avatar_revision != avatar_revision_seen && avatar_image != NULL) {
        uint8_t latest = avatar_store_latest_slot();
        if (latest < AVATAR_MAX_SLOTS) load_avatar_slot_locked(latest);
        else { current_avatar_slot=0xff; memset(avatar_regions,0,sizeof(avatar_regions)); lv_obj_add_flag(avatar_image,LV_OBJ_FLAG_HIDDEN); }
        avatar_revision_seen = avatar_revision;
    }
    if (avatar_reaction_seconds > 0 && --avatar_reaction_seconds == 0 &&
        avatar_reaction_label != NULL) {
        lv_obj_add_flag(avatar_reaction_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(avatar_sync_label, LV_OBJ_FLAG_HIDDEN);
    }
    if (current_app_view == APP_VIEW_AVATAR && avatar_store_count() > 1) {
        if (++avatar_cycle_seconds >= 12) {
            avatar_cycle_seconds = 0;
            int next = avatar_store_adjacent_slot(current_avatar_slot, 1);
            if (next >= 0) load_avatar_slot_locked((uint8_t)next);
        }
    } else {
        avatar_cycle_seconds = 0;
    }
    if(current_app_view==APP_VIEW_AVATAR && avatar_status_label) {
        char text[120];
        if(current_avatar_slot<AVATAR_MAX_SLOTS) snprintf(text,sizeof(text),"头像 %u · %s",current_avatar_slot+1,avatar_regions[0].w?"点脸互动":"旧照片 · 重新上传可眨眼");
        else snprintf(text,sizeof(text),"请从电脑助手上传头像");
        lv_label_set_text(avatar_status_label,text);
        lv_label_set_text(avatar_sync_label,avatar_store_sync_status());
    }
    if (weather_error_pending && weather_update_label != NULL) {
        weather_error_pending = false;
        lv_label_set_text(weather_update_label,
                          use_chinese ? "更新失败，点击重试" : "UPDATE FAILED - TAP AGAIN");
    }
    refresh_settings_locked();

    time_t now;
    struct tm local;
    char time_text[8];
    char seconds_text[8];
    char date_text[48];

    time(&now);
    if (!time_is_valid(now) || localtime_r(&now, &local) == NULL) {
        lv_label_set_text(time_label, "--:--");
        lv_label_set_text(seconds_label, "--");
        lv_label_set_text(date_label, "----  --  --");
        if (desktop_time_label != NULL) {
            lv_label_set_text(desktop_time_label, "--:--");
        }
        return;
    }

    strftime(time_text, sizeof(time_text), "%H:%M", &local);
    strftime(seconds_text, sizeof(seconds_text), ":%S", &local);
    if (use_chinese) {
        static const char *weekdays[] = {"日", "一", "二", "三", "四", "五", "六"};
        snprintf(date_text, sizeof(date_text), "%04d年%02d月%02d日  星期%s",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                 weekdays[local.tm_wday]);
    } else {
        strftime(date_text, sizeof(date_text), "%Y  %m  %d   %a", &local);
    }
    lv_label_set_text(time_label, time_text);
    lv_label_set_text(seconds_label, seconds_text);
    lv_label_set_text(date_label, date_text);
    if (desktop_time_label != NULL) {
        lv_label_set_text(desktop_time_label, time_text);
    }
}

static void show_view_locked(app_view_t view)
{
    notification_center_close();
    lv_obj_add_flag(desktop_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(clock_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(weather_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(settings_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wifi_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(remote_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(music_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(radio_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(wallpaper_studio_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(bluetooth_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(avatar_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(quota_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(codex_reset_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(app_overview_view, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(companion_apps_view(0), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(companion_apps_view(1), LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(star_game_view(), LV_OBJ_FLAG_HIDDEN);
    star_game_set_active(false);
    lv_obj_add_flag(english_app_view(), LV_OBJ_FLAG_HIDDEN);
    english_app_set_active(false);
    lv_obj_add_flag(library_app_view(), LV_OBJ_FLAG_HIDDEN);
    library_app_set_active(false);
    for(unsigned i=0;i<2;i++)lv_obj_add_flag(kids_apps_view(i),LV_OBJ_FLAG_HIDDEN);
    kids_apps_activate(-1);
    lv_obj_add_flag(story_app_view(),LV_OBJ_FLAG_HIDDEN);story_app_set_active(false);

    lv_obj_t *selected = view == APP_VIEW_CLOCK ? clock_view :
                         view == APP_VIEW_WEATHER ? weather_view :
                         view == APP_VIEW_SETTINGS ? settings_view :
                         view == APP_VIEW_WIFI ? wifi_view :
                         view == APP_VIEW_REMOTE ? remote_view :
                         view == APP_VIEW_MUSIC ? music_view :
                         view == APP_VIEW_RADIO ? radio_view :
                         view == APP_VIEW_WALLPAPER_STUDIO ? wallpaper_studio_view :
                         view == APP_VIEW_BLUETOOTH ? bluetooth_view : desktop_view;
    if (view == APP_VIEW_AVATAR) {
        selected = avatar_view;
    } else if (view == APP_VIEW_OVERVIEW) {
        selected = app_overview_view;
    } else if (view == APP_VIEW_QUOTA) {
        selected = quota_view;
    } else if (view == APP_VIEW_CODEX_RESET) {
        selected = codex_reset_view;
    } else if (view == APP_VIEW_AGENTS) {
        selected = companion_apps_view(0);
        companion_apps_activate(0);
    } else if (view == APP_VIEW_NOTES) {
        selected = companion_apps_view(1);
        companion_apps_activate(1);
    } else if (view == APP_VIEW_STARS) {
        selected = star_game_view();
        star_game_set_active(screen_on);
    } else if (view == APP_VIEW_ENGLISH) {
        selected = english_app_view();
        english_app_set_active(screen_on);
    } else if (view == APP_VIEW_LIBRARY) {
        selected=library_app_view();library_app_set_active(screen_on);
    } else if (view == APP_VIEW_PET || view==APP_VIEW_FLIP) {
        int index=view==APP_VIEW_PET?0:1;selected=kids_apps_view(index);kids_apps_activate(screen_on?index:-1);
    } else if (view == APP_VIEW_STORY) {
        selected=story_app_view();story_app_set_active(screen_on);
    }
    lv_obj_remove_flag(selected, LV_OBJ_FLAG_HIDDEN);
    current_app_view = view;
    if (view == APP_VIEW_DESKTOP && desktop_time_label != NULL && time_label != NULL) {
        lv_label_set_text(desktop_time_label, lv_label_get_text(time_label));
    } else if (view == APP_VIEW_SETTINGS) {
        refresh_settings_locked();
    } else if (view == APP_VIEW_WIFI) {
        wifi_refresh_status_locked();
    } else if (view == APP_VIEW_REMOTE) {
        remote_refresh_status_locked();
    } else if (view == APP_VIEW_MUSIC) {
        music_refresh_status_locked();
    } else if (view == APP_VIEW_RADIO) {
        radio_refresh_status_locked();
    } else if (view == APP_VIEW_WALLPAPER_STUDIO) {
        wallpaper_studio_refresh_status_locked();
    } else if (view == APP_VIEW_BLUETOOTH) {
        bluetooth_refresh_status_locked();
    } else if (view == APP_VIEW_QUOTA) {
        quota_refresh_requested = true;
    } else if (view == APP_VIEW_CODEX_RESET) {
        codex_reset_refresh_requested = true;
    }
}

esp_err_t ui_debug_show_view(uint8_t view)
{
    if (view >= APP_VIEW_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (bsp_display_lock(5000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    show_view_locked((app_view_t)view);
    bsp_display_unlock();
    return ESP_OK;
}

/* USB-only test input runs through LVGL hit testing and gesture dispatch. */
static lv_indev_t *test_pointer;
static lv_indev_data_t test_pointer_data;
static int64_t launcher_render_start;
static uint32_t launcher_render_count, launcher_render_total_us, launcher_render_max_us;
static void render_metrics(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        launcher_render_start = current_app_view == APP_VIEW_DESKTOP ? esp_timer_get_time() : 0;
    } else if (lv_event_get_code(event) == LV_EVENT_RENDER_READY && launcher_render_start) {
        uint32_t elapsed = esp_timer_get_time() - launcher_render_start;
        ++launcher_render_count;
        launcher_render_total_us += elapsed;
        if (elapsed > launcher_render_max_us) launcher_render_max_us = elapsed;
        launcher_render_start = 0;
    }
}
static void test_pointer_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = test_pointer_data.point;
    data->state = test_pointer_data.state;
}
esp_err_t ui_debug_pointer(int x, int y, bool pressed)
{
    if (x < 0 || x >= 466 || y < 0 || y >= 466) return ESP_ERR_INVALID_ARG;
    if (bsp_display_lock(5000) != ESP_OK) return ESP_ERR_TIMEOUT;
    if (!test_pointer) {
        test_pointer = lv_indev_create();
        lv_indev_set_type(test_pointer, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(test_pointer, test_pointer_read);
        lv_indev_set_mode(test_pointer, LV_INDEV_MODE_EVENT);
        lv_indev_set_scroll_limit(test_pointer, 24);
    }
    test_pointer_data.point = (lv_point_t){x, y};
    test_pointer_data.state = pressed ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
    lv_indev_read(test_pointer);
    bsp_display_unlock();
    return ESP_OK;
}
static void debug_labels(lv_obj_t *obj, cJSON *labels)
{
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_check_type(obj, &lv_label_class)) {
        lv_area_t a; lv_obj_get_coords(obj, &a);
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "text", lv_label_get_text(obj));
        cJSON_AddNumberToObject(item, "x", a.x1); cJSON_AddNumberToObject(item, "y", a.y1);
        cJSON_AddNumberToObject(item, "w", lv_area_get_width(&a));
        cJSON_AddNumberToObject(item, "h", lv_area_get_height(&a));
        const unsigned char *p = (const unsigned char *)lv_label_get_text(obj);
        unsigned missing = 0;
        while (*p) {
            uint32_t cp = *p++;
            int continuation = (cp & 0xE0) == 0xC0 ? 1 : (cp & 0xF0) == 0xE0 ? 2 : (cp & 0xF8) == 0xF0 ? 3 : 0;
            if (continuation) cp &= (1U << (6 - continuation)) - 1;
            while (continuation-- > 0 && *p) cp = (cp << 6) | (*p++ & 0x3F);
            if (cp < 32) continue;
            lv_font_glyph_dsc_t glyph;
            if (!lv_font_get_glyph_dsc(lv_obj_get_style_text_font(obj, 0), &glyph, cp, 0) || glyph.is_placeholder) ++missing;
        }
        cJSON_AddNumberToObject(item, "missing_glyphs", missing);
        cJSON_AddItemToArray(labels, item);
    }
    for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) debug_labels(lv_obj_get_child(obj, i), labels);
}
esp_err_t ui_debug_state(char *buffer, size_t capacity)
{
    if (bsp_display_lock(5000) != ESP_OK) return ESP_ERR_TIMEOUT;
    lv_obj_update_layout(lv_screen_active());
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "view", current_app_view);
    cJSON_AddStringToObject(root, "build", __DATE__ " " __TIME__);
    cJSON_AddNumberToObject(root, "launcher_render_count", launcher_render_count);
    cJSON_AddNumberToObject(root, "launcher_render_mean_us", launcher_render_count ? launcher_render_total_us / launcher_render_count : 0);
    cJSON_AddNumberToObject(root, "launcher_render_max_us", launcher_render_max_us);
    cJSON_AddNumberToObject(root, "free_internal", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "reset_sync_count", codex_reset_sync_count);
    char reset_cursor[32];
    load_last_codex_reset_id(reset_cursor, sizeof(reset_cursor));
    cJSON_AddStringToObject(root, "reset_cursor", reset_cursor);
    cJSON_AddNumberToObject(root, "minimum_internal", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(root, "selected", launcher_selected());
    cJSON_AddBoolToObject(root, "animating", launcher_animating());
    cJSON_AddNumberToObject(root, "quota_scroll", lv_obj_get_scroll_y(quota_list));
    cJSON_AddNumberToObject(root, "settings_scroll", lv_obj_get_scroll_y(settings_list));
    cJSON_AddStringToObject(root, "quota_total", lv_label_get_text(quota_total_label));
    cJSON_AddStringToObject(root, "wallpaper_status", wallpaper_input_status());
    cJSON_AddStringToObject(root, "music_status", music_input_status());
    wifi_setup_state_t setup; wifi_setup_state(&setup);
    cJSON_AddBoolToObject(root, "wifi_setup_active", setup.active);
    cJSON_AddBoolToObject(root, "wifi_setup_testing", setup.testing);
    cJSON_AddNumberToObject(root, "wifi_saved_networks", setup.saved_count);
    cJSON_AddStringToObject(root, "wifi_setup_status", setup.status);
    cJSON_AddBoolToObject(root, "wifi_connected", wifi_events && (xEventGroupGetBits(wifi_events) & WIFI_READY_BIT));
    cJSON_AddNumberToObject(root, "free_psram", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    cJSON_AddNumberToObject(root, "avatar_count", avatar_store_count());
    cJSON_AddNumberToObject(root, "avatar_slot", current_avatar_slot);
    cJSON_AddBoolToObject(root, "avatar_face_ready", avatar_regions[0].w!=0);
    cJSON_AddNumberToObject(root, "avatar_mood", avatar_mood);
    cJSON_AddNumberToObject(root, "avatar_reactions", avatar_reaction_index);
    cJSON_AddStringToObject(root, "avatar_sync", avatar_store_sync_status());
    companion_apps_debug(root);
    star_game_debug(root);
    english_app_debug(root);
    library_app_debug(root);
    kids_apps_debug(root);story_app_debug(root);
    cJSON *labels = cJSON_AddArrayToObject(root, "labels");
    debug_labels(lv_screen_active(), labels);
    bool ok = cJSON_PrintPreallocated(root, buffer, capacity, false);
    cJSON_Delete(root);
    bsp_display_unlock();
    return ok ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t ui_debug_start_pairing(void)
{
    esp_err_t result = vibe_remote_start_pairing();
    if (bsp_display_lock(5000) == ESP_OK) {
        remote_refresh_status_locked();
        bsp_display_unlock();
    }
    return result;
}

esp_err_t ui_debug_scroll_settings(bool to_bottom)
{
    if (settings_list == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bsp_display_lock(5000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    show_view_locked(APP_VIEW_SETTINGS);
    lv_obj_scroll_to_y(settings_list, to_bottom ? 10000 : 0, LV_ANIM_OFF);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_scroll_overview(bool to_bottom)
{
    if (app_overview_grid == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bsp_display_lock(5000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    show_view_locked(APP_VIEW_OVERVIEW);
    lv_obj_scroll_to_y(app_overview_grid, to_bottom ? 10000 : 0, LV_ANIM_OFF);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_scroll_desktop(bool to_bottom)
{
    if (desktop_app_grid == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bsp_display_lock(5000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    show_view_locked(APP_VIEW_DESKTOP);
    lv_obj_scroll_to_y(desktop_app_grid, to_bottom ? 10000 : 0, LV_ANIM_OFF);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_scroll_quota(bool to_bottom)
{
    if (quota_list == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (bsp_display_lock(5000) != ESP_OK) {
        return ESP_ERR_TIMEOUT;
    }
    show_view_locked(APP_VIEW_QUOTA);
    lv_obj_scroll_to_y(quota_list, to_bottom ? 10000 : 0, LV_ANIM_OFF);
    bsp_display_unlock();
    return ESP_OK;
}

esp_err_t ui_debug_arm_codex_reset_alert(void)
{
    save_last_codex_reset_id("usb-test-previous-signal");
    codex_reset_refresh_requested = true;
    return ESP_OK;
}

esp_err_t ui_debug_play_music(void)
{
    return music_input_play();
}

static void show_view(app_view_t view)
{
    requested_app_view = view;
}

static app_view_t adjacent_app_view(app_view_t view, int direction)
{
    static const app_view_t order[] = {
        APP_VIEW_DESKTOP, APP_VIEW_CLOCK, APP_VIEW_WEATHER,
        APP_VIEW_AVATAR, APP_VIEW_REMOTE, APP_VIEW_QUOTA,
        APP_VIEW_CODEX_RESET, APP_VIEW_MUSIC, APP_VIEW_RADIO,
        APP_VIEW_WALLPAPER_STUDIO, APP_VIEW_AGENTS, APP_VIEW_NOTES, APP_VIEW_STARS, APP_VIEW_ENGLISH, APP_VIEW_LIBRARY, APP_VIEW_PET, APP_VIEW_STORY, APP_VIEW_FLIP, APP_VIEW_SETTINGS,
    };
    app_view_t selected = (view == APP_VIEW_WIFI || view == APP_VIEW_BLUETOOTH)
                              ? APP_VIEW_SETTINGS : view;
    size_t index = 0;
    for (; index < sizeof(order) / sizeof(order[0]); ++index) {
        if (order[index] == selected) {
            break;
        }
    }
    if (index == sizeof(order) / sizeof(order[0])) {
        return APP_VIEW_DESKTOP;
    }
    int next = (int)index + direction;
    if (next < 0) {
        next += sizeof(order) / sizeof(order[0]);
    }
    return order[next % (int)(sizeof(order) / sizeof(order[0]))];
}

static void launcher_open(unsigned index)
{
    if (screen_on && index < sizeof(desktop_launcher_targets) / sizeof(desktop_launcher_targets[0]))
        show_view_locked(desktop_launcher_targets[index]);
}
static void launcher_overview(void)
{
    if (screen_on) show_view_locked(APP_VIEW_OVERVIEW);
}

static void companion_home(void) { show_view_locked(APP_VIEW_DESKTOP); }
static void open_library_story(void) {show_view_locked(APP_VIEW_STORY);}
static void notification_open_app(unsigned view) { show_view_locked((app_view_t)view); }

static void app_gesture_event_cb(lv_event_t *event)
{
    if (!screen_on) {
        return;
    }

    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(lv_indev_active(), &touch_start);
        touch_long_pressed = false;
    } else if (code == LV_EVENT_LONG_PRESSED) {
        touch_long_pressed = true;
        if (current_app_view == APP_VIEW_CLOCK) {
            force_update_requested = true;
            ESP_LOGI(TAG, "Touch long press: wallpaper update requested");
        }
    } else if (code == LV_EVENT_RELEASED && !touch_long_pressed) {
        lv_point_t release;
        lv_indev_get_point(lv_indev_active(), &release);
        int delta_x = release.x - touch_start.x;
        int delta_y = release.y - touch_start.y;
        if (current_app_view == APP_VIEW_OVERVIEW && delta_y > APP_OVERVIEW_SWIPE_DISTANCE) {
            show_view_locked(APP_VIEW_DESKTOP);
            return;
        }
        if (touch_start.y > APP_OVERVIEW_EDGE_Y && delta_y < -APP_OVERVIEW_SWIPE_DISTANCE) {
            show_view_locked(APP_VIEW_OVERVIEW);
            return;
        }


        if (current_app_view != APP_VIEW_OVERVIEW &&
            (delta_x > APP_SWIPE_DISTANCE || delta_x < -APP_SWIPE_DISTANCE)) {
            show_view_locked(adjacent_app_view(current_app_view,
                                                delta_x < 0 ? 1 : -1));
            return;
        }
        if (current_app_view == APP_VIEW_CLOCK) {
            show_wallpaper_locked(!showing_downloaded_wallpaper);
            ESP_LOGI(TAG, "Touch tap: %s wallpaper",
                     showing_downloaded_wallpaper ? "downloaded" : "built-in");
        }
    }
}

static void create_gesture_layer(lv_obj_t *parent)
{
    lv_obj_t *layer = lv_obj_create(parent);
    lv_obj_set_size(layer, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(layer);
    lv_obj_set_style_bg_opa(layer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(layer, 0, 0);
    lv_obj_set_style_pad_all(layer, 0, 0);
    lv_obj_add_flag(layer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(layer, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(layer, app_gesture_event_cb, LV_EVENT_ALL, NULL);
}

static void create_voice_wave(lv_obj_t *parent, voice_wave_t *wave,
                              int16_t center_y, uint32_t color)
{
    static const int16_t idle_heights[VOICE_WAVE_BAR_COUNT] = {8, 12, 16, 10, 18, 10, 16, 12, 8};
    const int16_t gap = 7;
    const int16_t bar_width = 7;
    int16_t total_width = VOICE_WAVE_BAR_COUNT * bar_width +
                          (VOICE_WAVE_BAR_COUNT - 1) * gap;
    wave->center_y = center_y;
    wave->color = color;
    for (int i = 0; i < VOICE_WAVE_BAR_COUNT; ++i) {
        lv_obj_t *bar = lv_obj_create(parent);
        lv_obj_set_size(bar, bar_width, idle_heights[i]);
        lv_obj_set_pos(bar, (WALLPAPER_WIDTH - total_width) / 2 + i * (bar_width + gap),
                       center_y - idle_heights[i] / 2);
        lv_obj_set_style_radius(bar, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(bar, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(color), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_40, 0);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        wave->bars[i] = bar;
    }
}

static void update_voice_wave(voice_wave_t *wave, bool recording, uint8_t level)
{
    static const uint8_t idle_heights[VOICE_WAVE_BAR_COUNT] = {8, 12, 16, 10, 18, 10, 16, 12, 8};
    static const uint8_t wave_shape[][VOICE_WAVE_BAR_COUNT] = {
        {18, 42, 68, 36, 96, 48, 72, 38, 18},
        {32, 72, 30, 88, 54, 100, 40, 68, 28},
        {54, 26, 82, 44, 100, 34, 86, 30, 52},
        {24, 78, 42, 96, 34, 72, 90, 38, 24},
    };
    if (wave->bars[0] == NULL) {
        return;
    }
    size_t frame = (voice_animation_frame / 2) %
                   (sizeof(wave_shape) / sizeof(wave_shape[0]));
    for (int i = 0; i < VOICE_WAVE_BAR_COUNT; ++i) {
        int16_t height = recording ? 6 + (level * wave_shape[frame][i]) / 180
                                   : idle_heights[i];
        lv_obj_set_height(wave->bars[i], height);
        lv_obj_set_y(wave->bars[i], wave->center_y - height / 2);
        lv_obj_set_style_bg_opa(wave->bars[i], recording ? LV_OPA_COVER : LV_OPA_40, 0);
    }
}

static void voice_wave_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!screen_on) return;
    voice_wave_t *wave = current_app_view == APP_VIEW_REMOTE ? &remote_voice_wave :
                         current_app_view == APP_VIEW_MUSIC ? &music_voice_wave :
                         current_app_view == APP_VIEW_RADIO ? &radio_voice_wave :
                         current_app_view == APP_VIEW_WALLPAPER_STUDIO ? &wallpaper_voice_wave : NULL;
    if (wave == NULL) return;
    ++voice_animation_frame;
    update_voice_wave(wave, voice_input_is_recording(), voice_input_level());
}

static uint8_t triangle_wave(uint8_t phase, uint8_t period)
{
    uint8_t position = phase % period;
    return position < period / 2 ? position : period - 1 - position;
}

static void ambient_animation_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!screen_on || current_app_view != APP_VIEW_CLOCK) {
        return;
    }
    ++animation_frame;

    for (int i = 0; i < STAR_COUNT; ++i) {
        uint8_t phase = (stars[i].phase + animation_frame * stars[i].speed) & 31;
        uint8_t pulse = triangle_wave(phase, 32);
        lv_obj_set_style_bg_opa(stars[i].object, 55 + pulse * 12, 0);
        if ((animation_frame & 3) == 0) {
            lv_obj_set_y(stars[i].object, stars[i].base_y + pulse / 6);
        }
    }

    for (int i = 0; i < AURORA_BAND_COUNT; ++i) {
        uint8_t phase = (aurora_bands[i].phase + animation_frame) & 63;
        uint8_t flow = triangle_wave(phase, 64);
        lv_obj_set_y(aurora_bands[i].object, aurora_bands[i].base_y + (int)flow / 2 - 8);
        lv_obj_set_style_bg_opa(aurora_bands[i].object, 8 + flow / 2, 0);
    }
}

static void create_ambient_animation(lv_obj_t *screen)
{
    static const int16_t band_x[AURORA_BAND_COUNT] = {54, 94, 137, 181, 235, 287, 333, 378};
    static const int16_t band_y[AURORA_BAND_COUNT] = {88, 55, 74, 42, 48, 68, 50, 91};
    static const int16_t band_w[AURORA_BAND_COUNT] = {16, 24, 14, 27, 22, 16, 25, 15};
    static const int16_t band_h[AURORA_BAND_COUNT] = {88, 132, 104, 148, 142, 108, 128, 82};
    static const uint32_t band_color[AURORA_BAND_COUNT] = {
        0x45E7FF, 0x57FFC8, 0x9A6BFF, 0x49DBFF,
        0x8A63FF, 0x55FFD4, 0x6DA8FF, 0xA46BFF,
    };

    for (int i = 0; i < AURORA_BAND_COUNT; ++i) {
        lv_obj_t *band = lv_obj_create(screen);
        lv_obj_set_pos(band, band_x[i], band_y[i]);
        lv_obj_set_size(band, band_w[i], band_h[i]);
        lv_obj_set_style_radius(band, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(band, 0, 0);
        lv_obj_set_style_bg_color(band, lv_color_hex(band_color[i]), 0);
        lv_obj_set_style_bg_grad_color(band, lv_color_hex(0x07142B), 0);
        lv_obj_set_style_bg_grad_dir(band, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_opa(band, 14, 0);
        lv_obj_remove_flag(band, LV_OBJ_FLAG_SCROLLABLE);
        aurora_bands[i] = (aurora_band_t) {
            .object = band,
            .base_y = band_y[i],
            .phase = i * 7,
        };
    }

    uint32_t seed = 0x175C2026;
    for (int i = 0; i < STAR_COUNT; ++i) {
        int16_t x;
        int16_t y;
        do {
            seed = seed * 1664525U + 1013904223U;
            x = 24 + seed % 418;
            seed = seed * 1664525U + 1013904223U;
            y = 24 + seed % 418;
            int32_t dx = x - WALLPAPER_WIDTH / 2;
            int32_t dy = y - WALLPAPER_HEIGHT / 2;
            if (dx * dx + dy * dy > 210 * 210 ||
                (x > 55 && x < 411 && y > 125 && y < 341)) {
                x = -1;
            }
        } while (x < 0);

        uint8_t size = 2 + (seed >> 28) % 3;
        lv_obj_t *star = lv_obj_create(screen);
        lv_obj_set_pos(star, x, y);
        lv_obj_set_size(star, size, size);
        lv_obj_set_style_radius(star, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_border_width(star, 0, 0);
        lv_obj_set_style_bg_color(star, lv_color_hex(i % 4 == 0 ? 0x82E8FF : 0xFFFFFF), 0);
        lv_obj_set_style_bg_opa(star, 150, 0);
        lv_obj_remove_flag(star, LV_OBJ_FLAG_SCROLLABLE);
        stars[i] = (star_t) {
            .object = star,
            .base_y = y,
            .phase = (seed >> 8) & 31,
            .speed = 1 + (seed >> 16) % 3,
        };
    }

    lv_timer_create(ambient_animation_cb, ANIMATION_INTERVAL_MS, NULL);
}

static void overview_app_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    app_view_t view = (app_view_t)(intptr_t)lv_event_get_user_data(event);
    show_view_locked(view);
    if (view == APP_VIEW_WEATHER) {
        weather_refresh_requested = true;
    } else if (view == APP_VIEW_AVATAR) {
        avatar_cycle_seconds = 0;
    } else if (view == APP_VIEW_QUOTA) {
        quota_refresh_requested = true;
    } else if (view == APP_VIEW_CODEX_RESET) {
        codex_reset_refresh_requested = true;
    }
}

static void overview_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
    }
}

static bool load_avatar_slot_locked(uint8_t slot)
{
    if (avatar_store_load_face(slot, avatar_original, avatar_regions) != ESP_OK) {
        return false;
    }
    memcpy(avatar_buffer,avatar_original,AVATAR_BYTES);
    avatar_face_started=0; avatar_idle_blink=lv_tick_get()+4500;
    current_avatar_slot = slot;
    lv_image_set_src(avatar_image, &avatar_image_descriptor);
    lv_obj_remove_flag(avatar_image, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(avatar_status_label, localized("TAP ME", "点点我"));
    lv_obj_invalidate(avatar_image);
    avatar_cycle_seconds = 0;
    return true;
}

static void avatar_face_tick(lv_timer_t *timer)
{
    (void)timer;
    if(!screen_on || current_app_view!=APP_VIEW_AVATAR || !avatar_regions[0].w) return;
    uint32_t now=lv_tick_get();
    if(!avatar_face_started && (int32_t)(now-avatar_idle_blink)>=0) {
        avatar_mood=0; avatar_face_started=now; avatar_idle_blink=now+4500;
    }
    if(!avatar_face_started) return;
    uint32_t age=now-avatar_face_started;
    uint32_t duration=(avatar_mood==0 || avatar_mood==3)?480:1800;
    float amount=0;
    if(age<duration) {
        float t=(float)age/duration;
        amount=t<0.25f?t*4:t>0.75f?(1-t)*4:1;
    } else avatar_face_started=0;
    avatar_face_render((uint16_t *)avatar_original,(uint16_t *)avatar_buffer,avatar_regions,avatar_mood,amount);
    lv_obj_invalidate(avatar_image);
}

static void avatar_scale_anim(void *object, int32_t value)
{
    lv_image_set_scale((lv_obj_t *)object, value);
}

static void avatar_rotation_anim(void *object, int32_t value)
{
    lv_image_set_rotation((lv_obj_t *)object, value);
}

static void avatar_react_locked(void)
{
    static const char *english_responses[] = {
        "BLINK!", "SMILE!", "KISS!", "WINK!",
    };
    static const char *chinese_responses[] = {
        "眨眨眼", "笑一个", "亲亲你", "调皮一下",
    };
    static const uint32_t colors[] = {
        0x245FC4, 0xAC2361, 0x875100, 0x176438,
    };
    size_t response = avatar_reaction_index++ %
                      (sizeof(english_responses) / sizeof(english_responses[0]));
    lv_label_set_text(avatar_reaction_label,
                      use_chinese ? chinese_responses[response] : english_responses[response]);
    lv_obj_set_style_text_color(avatar_reaction_label,
                                lv_color_hex(colors[response]), 0);
    lv_obj_remove_flag(avatar_reaction_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(avatar_sync_label, LV_OBJ_FLAG_HIDDEN);
    avatar_reaction_seconds = 3;
    avatar_cycle_seconds = 0;
    avatar_mood=response%4; avatar_face_started=lv_tick_get();
    avatar_idle_blink=avatar_face_started+6000;

    lv_anim_delete(avatar_image, avatar_scale_anim);
    lv_anim_t scale;
    lv_anim_init(&scale);
    lv_anim_set_var(&scale, avatar_image);
    lv_anim_set_exec_cb(&scale, avatar_scale_anim);
    lv_anim_set_values(&scale, 256, 270);
    lv_anim_set_duration(&scale, 150);
    lv_anim_set_reverse_duration(&scale, 220);
    lv_anim_start(&scale);

    lv_anim_delete(avatar_image, avatar_rotation_anim);
    lv_anim_t rotation;
    lv_anim_init(&rotation);
    lv_anim_set_var(&rotation, avatar_image);
    lv_anim_set_exec_cb(&rotation, avatar_rotation_anim);
    lv_anim_set_values(&rotation, response % 2 == 0 ? -45 : 45, 0);
    lv_anim_set_duration(&rotation, 240);
    lv_anim_start(&rotation);
}

static void avatar_event_cb(lv_event_t *event)
{
    if (!screen_on || current_avatar_slot >= AVATAR_MAX_SLOTS) {
        return;
    }
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(lv_indev_active(), &avatar_touch_start);
    } else if (code == LV_EVENT_RELEASED) {
        lv_point_t release;
        lv_indev_get_point(lv_indev_active(), &release);
        int delta_x = release.x - avatar_touch_start.x;
        if ((delta_x > 60 || delta_x < -60) && avatar_store_count() > 1) {
            int next = avatar_store_adjacent_slot(current_avatar_slot,
                                                  delta_x < 0 ? 1 : -1);
            if (next >= 0) {
                load_avatar_slot_locked((uint8_t)next);
            }
        } else {
            avatar_react_locked();
        }
    }
}

static void avatar_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
    }
}

static void clock_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
        ESP_LOGI(TAG, "Clock: desktop opened");
    }
}

static void weather_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
        ESP_LOGI(TAG, "Weather: desktop opened");
    }
}

static void weather_refresh_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        weather_refresh_requested = true;
        lv_label_set_text(weather_update_label,
                          use_chinese ? "正在更新" : "UPDATING...");
        ESP_LOGI(TAG, "Weather: refresh requested");
    }
}

static void quota_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
    }
}

static void quota_refresh_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        quota_refresh_requested = true;
        lv_label_set_text(quota_update_label,
                          use_chinese ? "正在更新" : "UPDATING...");
    }
}

static void codex_reset_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
    }
}

static void codex_reset_refresh_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        codex_reset_refresh_requested = true;
        lv_label_set_text(codex_reset_update_label,
                          use_chinese ? "正在更新" : "UPDATING...");
    }
}

static void settings_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
        ESP_LOGI(TAG, "Settings: desktop opened");
    }
}

static void remote_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
    }
}

static void bluetooth_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_SETTINGS);
    }
}

static void bluetooth_action_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    esp_err_t result = vibe_remote_start_pairing();
    bluetooth_refresh_status_locked();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth start failed: %s", esp_err_to_name(result));
    }
}

static void remote_action_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }

    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    esp_err_t result = ESP_OK;
    if (action == 1) {
        result = voice_input_start();
    } else if (action == 2) {
        if (voice_input_is_recording()) {
            result = voice_input_finish();
        } else {
            result = ESP_ERR_INVALID_STATE;
        }
    }
    remote_refresh_status_locked();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Vibe remote action %d failed: %s", (int)action,
                 esp_err_to_name(result));
    }
}

static void music_action_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    esp_err_t result = action == 1 ? music_input_start() :
                       action == 2 ? music_input_finish() : music_input_play();
    music_refresh_status_locked();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Music action %d failed: %s", (int)action,
                 esp_err_to_name(result));
    }
}

static void radio_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
    }
}

static void radio_station_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        uint8_t selected = (uint8_t)(intptr_t)lv_event_get_user_data(event);
        if (selected >= 3) {
            return;
        }
        radio_prompt_prefix = radio_station_prompts[selected];
        radio_channel_selected = true;
        radio_channel_feedback_until = esp_timer_get_time() + 3000000;
        for (size_t i = 0; i < 3; ++i) {
            lv_obj_t *button = radio_station_buttons[i];
            if (button == NULL) {
                continue;
            }
            bool active = i == selected;
            lv_obj_set_style_bg_color(button,
                                       active ? lv_color_hex(0x007AFF) : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(button, active ? LV_OPA_COVER : LV_OPA_80, 0);
            lv_obj_set_style_border_color(button,
                                           active ? lv_color_hex(0x007AFF) : lv_color_hex(0xD1D1D6), 0);
            lv_obj_set_style_text_color(lv_obj_get_child(button, 0),
                                         active ? lv_color_white() : lv_color_hex(0x007AFF), 0);
        }
        radio_refresh_status_locked();
    }
}

static void radio_action_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    esp_err_t result = action == 1 ? music_input_start_with_prefix(radio_prompt_prefix) :
                       action == 2 ? music_input_finish() : music_input_play();
    radio_refresh_status_locked();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Radio action %d failed: %s", (int)action, esp_err_to_name(result));
    }
}

static void wallpaper_studio_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
    }
}

static void wallpaper_studio_action_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t action = (intptr_t)lv_event_get_user_data(event);
    esp_err_t result = action == 1 ? wallpaper_input_start() : wallpaper_input_finish();
    wallpaper_studio_refresh_status_locked();
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "Wallpaper studio action %d failed: %s", (int)action,
                 esp_err_to_name(result));
    }
}

static void settings_wifi_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_WIFI);
        ESP_LOGI(TAG, "Settings: Wi-Fi configuration opened");
    }
}

static void settings_bluetooth_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_BLUETOOTH);
        ESP_LOGI(TAG, "Settings: Bluetooth opened");
    }
}

static void settings_brightness_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    int delta = (int)(intptr_t)lv_event_get_user_data(event);
    int brightness = display_brightness + delta;
    if (brightness < 10) {
        brightness = 10;
    } else if (brightness > 100) {
        brightness = 100;
    }
    set_display_brightness((uint8_t)brightness);
    ESP_LOGI(TAG, "Settings: brightness %d%%", brightness);
}

static bool set_audio_volume(uint8_t volume)
{
    audio_volume = volume;
    music_input_set_volume(volume);
    save_system_settings();
    refresh_settings_locked();
    return true;
}

static void settings_volume_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    int volume = audio_volume + (int)(intptr_t)lv_event_get_user_data(event);
    if (volume < 0) {
        volume = 0;
    } else if (volume > 100) {
        volume = 100;
    }
    if (set_audio_volume((uint8_t)volume)) {
        ESP_LOGI(TAG, "Settings: volume %d%%", volume);
    }
}

static void settings_restart_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    esp_restart();
}

static void settings_preference_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t preference = (intptr_t)lv_event_get_user_data(event);
    if (preference == 1) {
        use_chinese = !use_chinese;
        ESP_LOGI(TAG, "Settings: language changed to %s", use_chinese ? "Chinese" : "English");
    } else if (preference == 2) {
        use_ironman_theme = !use_ironman_theme;
        ESP_LOGI(TAG, "Settings: theme changed to %s", use_ironman_theme ? "iron man" : "aurora");
    }
    save_system_settings();
    if (settings_notice_label != NULL) {
        lv_label_set_text(settings_notice_label, localized("SAVED — RESTARTING...", "已保存，正在重启..."));
    }
    lv_timer_t *timer = lv_timer_create(settings_restart_timer_cb, 700, NULL);
    lv_timer_set_repeat_count(timer, 1);
}

static void wifi_refresh_status_locked(void)
{
    if (wifi_status_label == NULL) {
        return;
    }
    if (wifi_events == NULL) {
        lv_label_set_text(wifi_status_label, localized("WI-FI NOT AVAILABLE", "Wi-Fi 不可用"));
        return;
    }
    wifi_ap_record_t access_point = {0};
    if ((xEventGroupGetBits(wifi_events) & WIFI_READY_BIT) != 0 &&
        esp_wifi_sta_get_ap_info(&access_point) == ESP_OK) {
        lv_label_set_text_fmt(wifi_status_label, "%s: %s", localized("CONNECTED", "已连接"),
                              (const char *)access_point.ssid);
    } else {
        lv_label_set_text(wifi_status_label, localized("SELECT A NETWORK", "选择网络"));
    }
}

static void wifi_hide_password_panel(void)
{
    if (wifi_password_panel != NULL) {
        lv_obj_add_flag(wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
    }
    if (wifi_keyboard != NULL) {
        lv_keyboard_set_textarea(wifi_keyboard, NULL);
    }
}

static void wifi_connect_selected(void)
{
    if (selected_wifi_ssid[0] == '\0' || wifi_password_input == NULL || wifi_events == NULL) {
        return;
    }

    esp_err_t result = wifi_setup_connect(selected_wifi_ssid, lv_textarea_get_text(wifi_password_input));
    if (result == ESP_OK) {
        lv_label_set_text(wifi_status_label, localized("CONNECTING...", "正在连接..."));
        wifi_hide_password_panel();
        ESP_LOGI(TAG, "Wi-Fi connection requested");
    } else {
        lv_label_set_text(wifi_selected_label, localized("CONNECTION FAILED", "连接失败"));
        ESP_LOGE(TAG, "Wi-Fi configuration failed: %s", esp_err_to_name(result));
    }
}

static void wifi_keyboard_event_cb(lv_event_t *event)
{
    lv_event_code_t code = lv_event_get_code(event);
    if (code == LV_EVENT_READY) {
        wifi_connect_selected();
    } else if (code == LV_EVENT_CANCEL) {
        wifi_hide_password_panel();
    }
}

static void wifi_network_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED) {
        return;
    }
    intptr_t index = (intptr_t)lv_event_get_user_data(event);
    if (index < 0 || index >= scanned_network_count) {
        return;
    }
    strlcpy(selected_wifi_ssid, (const char *)scanned_networks[index].ssid,
            sizeof(selected_wifi_ssid));
    lv_label_set_text_fmt(wifi_selected_label, "%s\n%s", localized("PASSWORD FOR", "输入密码"),
                          selected_wifi_ssid);
    lv_textarea_set_text(wifi_password_input, "");
    lv_obj_remove_flag(wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(wifi_keyboard, wifi_password_input);
    lv_obj_add_state(wifi_password_input, LV_STATE_FOCUSED);
}

static void wifi_scan_event_cb(lv_event_t *event)
{
    if (!screen_on || lv_event_get_code(event) != LV_EVENT_CLICKED || wifi_events == NULL) {
        return;
    }
    lv_label_set_text(wifi_status_label, localized("SCANNING...", "正在扫描..."));
    lv_obj_clean(wifi_network_list);
    scanned_network_count = 0;

    esp_err_t result = esp_wifi_scan_start(NULL, true);
    uint16_t available = 0;
    if (result == ESP_OK) {
        result = esp_wifi_scan_get_ap_num(&available);
    }
    if (result == ESP_OK && available > 0) {
        scanned_network_count = available > WIFI_SCAN_MAX_RESULTS ? WIFI_SCAN_MAX_RESULTS : available;
        result = esp_wifi_scan_get_ap_records(&scanned_network_count, scanned_networks);
    }
    if (result != ESP_OK || scanned_network_count == 0) {
        lv_label_set_text(wifi_status_label, localized("NO NETWORKS FOUND", "未找到网络"));
        return;
    }

    for (uint16_t i = 0; i < scanned_network_count; ++i) {
        lv_obj_t *network = lv_button_create(wifi_network_list);
        lv_obj_set_size(network, 324, 40);
        lv_obj_set_pos(network, 8, i * 43);
        lv_obj_set_style_radius(network, 14, 0);
        lv_obj_set_style_bg_color(network, theme_card(), 0);
        lv_obj_set_style_border_width(network, 1, 0);
        lv_obj_set_style_border_color(network, theme_accent(), 0);
        lv_obj_add_event_cb(network, wifi_network_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *ssid = lv_label_create(network);
        lv_label_set_text(ssid, (const char *)scanned_networks[i].ssid);
        lv_label_set_long_mode(ssid, LV_LABEL_LONG_DOT);
        lv_obj_set_width(ssid, 220);
        lv_obj_set_style_text_font(ssid, localized_font(&lv_font_montserrat_14), 0);
        lv_obj_align(ssid, LV_ALIGN_LEFT_MID, 14, 0);

        lv_obj_t *signal = lv_label_create(network);
        lv_label_set_text_fmt(signal, "%d", scanned_networks[i].rssi);
        lv_obj_set_style_text_font(signal, &lv_font_montserrat_12, 0);
        lv_obj_align(signal, LV_ALIGN_RIGHT_MID, -14, 0);
    }
    lv_label_set_text(wifi_status_label, localized("SELECT A NETWORK", "选择网络"));
}

static void wifi_back_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        wifi_hide_password_panel();
        show_view_locked(APP_VIEW_SETTINGS);
    }
}

static void wifi_cancel_password_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) == LV_EVENT_CLICKED) {
        wifi_hide_password_panel();
        wifi_refresh_status_locked();
    }
}

static void wifi_setup_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;
    if (lv_event_get_user_data(event)) {
        wifi_setup_close();
        lv_obj_add_flag(wifi_setup_panel, LV_OBJ_FLAG_HIDDEN);
        wifi_refresh_status_locked();
    } else {
        lv_label_set_text(wifi_setup_message, localized("STARTING...", "正在开启热点..."));
        lv_obj_add_flag(wifi_setup_qr, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(wifi_setup_panel, LV_OBJ_FLAG_HIDDEN);
        if (wifi_setup_open() != ESP_OK)
            lv_label_set_text(wifi_setup_message, localized("PLEASE RETRY", "启动失败，请重试"));
    }
}

static void wifi_setup_refresh_locked(void)
{
    static bool was_testing;
    if (!wifi_setup_panel) return;
    wifi_setup_state_t s; wifi_setup_state(&s);
    if (s.active && !wifi_setup_was_active) {
        char qr[128];
        snprintf(qr,sizeof(qr),"WIFI:T:WPA;S:%s;P:%s;;",s.ssid,s.password);
        lv_qrcode_update(wifi_setup_qr,qr,strlen(qr));
        lv_obj_remove_flag(wifi_setup_qr,LV_OBJ_FLAG_HIDDEN);
    }
    if (!lv_obj_has_flag(wifi_setup_panel,LV_OBJ_FLAG_HIDDEN)) {
        lv_label_set_text(wifi_setup_message,s.status);
        if (wifi_setup_was_active && !s.active) {
            lv_obj_add_flag(wifi_setup_panel,LV_OBJ_FLAG_HIDDEN);
            wifi_refresh_status_locked();
        }
    }
    if (s.testing || was_testing) lv_label_set_text(wifi_status_label, s.status);
    was_testing=s.testing;
    wifi_setup_was_active=s.active;
}

static void create_weather(lv_obj_t *screen)
{
    weather_view = lv_obj_create(screen);
    lv_obj_set_size(weather_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(weather_view);
    lv_obj_set_style_bg_color(weather_view, lv_color_hex(0x03121F), 0);
    lv_obj_set_style_bg_grad_color(weather_view, lv_color_hex(0x123D5B), 0);
    lv_obj_set_style_bg_grad_dir(weather_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(weather_view, 0, 0);
    lv_obj_set_style_radius(weather_view, 0, 0);
    lv_obj_set_style_pad_all(weather_view, 0, 0);
    lv_obj_remove_flag(weather_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(weather_view);

    lv_obj_t *glow = lv_obj_create(weather_view);
    lv_obj_set_size(glow, 250, 250);
    lv_obj_align(glow, LV_ALIGN_TOP_MID, 0, -88);
    lv_obj_set_style_radius(glow, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(glow, lv_color_hex(0x4EDBFF), 0);
    lv_obj_set_style_bg_opa(glow, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(glow, 0, 0);
    lv_obj_remove_flag(glow, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    weather_city_label = lv_label_create(weather_view);
    lv_label_set_text(weather_city_label, localized("BEIJING", "北京"));
    lv_obj_set_style_text_font(weather_city_label,
                               localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(weather_city_label, lv_color_hex(0xF2F2F7), 0);
    lv_obj_align(weather_city_label, LV_ALIGN_TOP_MID, 0, 45);

    weather_temp_label = lv_label_create(weather_view);
    lv_label_set_text(weather_temp_label, "--.-°");
    lv_obj_set_style_text_font(weather_temp_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(weather_temp_label, lv_color_white(), 0);
    lv_obj_align(weather_temp_label, LV_ALIGN_TOP_MID, 0, 82);

    weather_condition_label = lv_label_create(weather_view);
    lv_label_set_text(weather_condition_label, localized("CONNECTING...", "正在连接"));
    lv_obj_set_style_text_font(weather_condition_label,
                               localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(weather_condition_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(weather_condition_label, LV_ALIGN_TOP_MID, 0, 145);

    weather_range_label = lv_label_create(weather_view);
    lv_label_set_text(weather_range_label,
                      localized("H -- C   L -- C", "最高 --度   最低 --度"));
    lv_obj_set_style_text_font(weather_range_label,
                               localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(weather_range_label, lv_color_hex(0xAEAEB2), 0);
    lv_obj_align(weather_range_label, LV_ALIGN_TOP_MID, 0, 177);

    lv_obj_t *info_panel = lv_obj_create(weather_view);
    lv_obj_set_size(info_panel, 300, 90);
    lv_obj_align(info_panel, LV_ALIGN_CENTER, 0, 75);
    lv_obj_set_style_radius(info_panel, 28, 0);
    lv_obj_set_style_bg_color(info_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(info_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(info_panel, 0, 0);
    lv_obj_set_style_pad_all(info_panel, 0, 0);
    lv_obj_remove_flag(info_panel, LV_OBJ_FLAG_SCROLLABLE);

    weather_humidity_label = lv_label_create(info_panel);
    lv_label_set_text(weather_humidity_label, localized("HUMIDITY\n--%", "湿度\n--%"));
    lv_obj_set_style_text_align(weather_humidity_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(weather_humidity_label,
                               localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(weather_humidity_label, lv_color_hex(0xF2F2F7), 0);
    lv_obj_align(weather_humidity_label, LV_ALIGN_LEFT_MID, 35, 0);

    weather_wind_label = lv_label_create(info_panel);
    lv_label_set_text(weather_wind_label, localized("WIND\n-- km/h", "风速\n-- km/h"));
    lv_obj_set_style_text_align(weather_wind_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(weather_wind_label,
                               localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(weather_wind_label, lv_color_hex(0xF2F2F7), 0);
    lv_obj_align(weather_wind_label, LV_ALIGN_RIGHT_MID, -35, 0);

    lv_obj_t *refresh_button = lv_button_create(weather_view);
    lv_obj_set_size(refresh_button, 44, 44);
    lv_obj_align(refresh_button, LV_ALIGN_BOTTOM_MID, -66, -22);
    lv_obj_set_style_radius(refresh_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(refresh_button, lv_color_hex(0x174A62), 0);
    lv_obj_set_style_bg_opa(refresh_button, LV_OPA_70, 0);
    lv_obj_add_event_cb(refresh_button, weather_refresh_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *refresh_icon = lv_label_create(refresh_button);
    lv_label_set_text(refresh_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(refresh_icon, lv_color_hex(0xD8F5FF), 0);
    lv_obj_center(refresh_icon);

    weather_update_label = lv_label_create(weather_view);
    lv_label_set_text(weather_update_label,
                      localized("WAITING FOR WI-FI", "等待无线网络"));
    lv_obj_set_style_text_font(weather_update_label,
                               localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(weather_update_label, lv_color_hex(0x75AFC2), 0);
    lv_obj_align(weather_update_label, LV_ALIGN_BOTTOM_MID, 0, -82);

    lv_obj_t *home_button = lv_button_create(weather_view);
    lv_obj_set_size(home_button, 52, 52);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 0, -18);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x071524), 0);
    lv_obj_set_style_bg_opa(home_button, LV_OPA_70, 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, lv_color_hex(0x72E7FF), 0);
    lv_obj_add_event_cb(home_button, weather_home_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, lv_color_hex(0xD9F7FF), 0);
    lv_obj_center(home_icon);
}

static void create_settings(lv_obj_t *screen)
{
    settings_view = lv_obj_create(screen);
    lv_obj_set_size(settings_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(settings_view);
    lv_obj_set_style_bg_color(settings_view, theme_background_top(), 0);
    lv_obj_set_style_bg_grad_color(settings_view, theme_background_bottom(), 0);
    lv_obj_set_style_bg_grad_dir(settings_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(settings_view, 0, 0);
    lv_obj_set_style_radius(settings_view, 0, 0);
    lv_obj_set_style_pad_all(settings_view, 0, 0);
    lv_obj_remove_flag(settings_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(settings_view);

    lv_obj_t *title = lv_label_create(settings_view);
    lv_label_set_text(title, localized("SETTINGS", "设置"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE7F8FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    settings_notice_label = lv_label_create(settings_view);
    lv_label_set_text(settings_notice_label, localized("TAP A ROW TO CHANGE", "点击更改"));
    lv_obj_set_width(settings_notice_label, 330);
    lv_label_set_long_mode(settings_notice_label, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(settings_notice_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(settings_notice_label, localized_font(&lv_font_montserrat_12), 0);
    lv_obj_set_style_text_color(settings_notice_label, lv_color_hex(0x9DE9FF), 0);
    lv_obj_set_style_text_font(settings_notice_label, &round_clock_cn_16, 0);
    lv_obj_align(settings_notice_label, LV_ALIGN_TOP_MID, 0, 56);

    settings_list = lv_obj_create(settings_view);
    lv_obj_set_size(settings_list, 374, 298);
    lv_obj_align(settings_list, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_set_style_bg_opa(settings_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(settings_list, 0, 0);
    lv_obj_set_style_radius(settings_list, 0, 0);
    lv_obj_set_style_pad_all(settings_list, 0, 0);
    lv_obj_set_scroll_dir(settings_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(settings_list, LV_SCROLLBAR_MODE_ON);
    lv_obj_add_flag(settings_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(settings_list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_width(settings_list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(settings_list, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(settings_list, theme_accent(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(settings_list, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(settings_list, 2, LV_PART_SCROLLBAR);

    lv_obj_t *wifi_button = lv_button_create(settings_list);
    lv_obj_set_size(wifi_button, 340, 62);
    lv_obj_align(wifi_button, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_radius(wifi_button, 20, 0);
    lv_obj_set_style_bg_color(wifi_button, theme_card(), 0);
    lv_obj_set_style_bg_opa(wifi_button, LV_OPA_90, 0);
    lv_obj_set_style_border_width(wifi_button, 1, 0);
    lv_obj_set_style_border_color(wifi_button, theme_accent(), 0);
    lv_obj_add_event_cb(wifi_button, settings_wifi_event_cb, LV_EVENT_CLICKED, NULL);

    settings_wifi_label = lv_label_create(wifi_button);
    lv_label_set_text(settings_wifi_label, localized("WI-FI SETTINGS", "无线网络"));
    lv_label_set_long_mode(settings_wifi_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(settings_wifi_label, 250);
    lv_obj_set_style_text_font(settings_wifi_label, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(settings_wifi_label, lv_color_white(), 0);
    lv_obj_align(settings_wifi_label, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *wifi_action = lv_label_create(wifi_button);
    lv_label_set_text(wifi_action, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(wifi_action, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(wifi_action, lv_color_hex(0xD9F7FF), 0);
    lv_obj_align(wifi_action, LV_ALIGN_RIGHT_MID, -18, 0);

    lv_obj_t *bluetooth_button = lv_button_create(settings_list);
    lv_obj_set_size(bluetooth_button, 340, 62);
    lv_obj_align(bluetooth_button, LV_ALIGN_TOP_MID, 0, 74);
    lv_obj_set_style_radius(bluetooth_button, 20, 0);
    lv_obj_set_style_bg_color(bluetooth_button, lv_color_hex(0x123C78), 0);
    lv_obj_set_style_bg_opa(bluetooth_button, LV_OPA_90, 0);
    lv_obj_set_style_border_width(bluetooth_button, 1, 0);
    lv_obj_set_style_border_color(bluetooth_button, lv_color_hex(0x74C7FF), 0);
    lv_obj_add_event_cb(bluetooth_button, settings_bluetooth_event_cb,
                        LV_EVENT_CLICKED, NULL);

    lv_obj_t *bluetooth_icon = lv_label_create(bluetooth_button);
    lv_label_set_text(bluetooth_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(bluetooth_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bluetooth_icon, lv_color_hex(0x74C7FF), 0);
    lv_obj_align(bluetooth_icon, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *bluetooth_label = lv_label_create(bluetooth_button);
    lv_label_set_text(bluetooth_label, localized("BLUETOOTH", "蓝牙"));
    lv_obj_set_style_text_font(bluetooth_label,
                               localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(bluetooth_label, lv_color_white(), 0);
    lv_obj_align(bluetooth_label, LV_ALIGN_LEFT_MID, 52, 0);

    lv_obj_t *bluetooth_action = lv_label_create(bluetooth_button);
    lv_label_set_text(bluetooth_action, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(bluetooth_action, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(bluetooth_action, lv_color_hex(0xD9F7FF), 0);
    lv_obj_align(bluetooth_action, LV_ALIGN_RIGHT_MID, -18, 0);

    lv_obj_t *display_card = lv_obj_create(settings_list);
    lv_obj_set_size(display_card, 340, 62);
    lv_obj_align(display_card, LV_ALIGN_TOP_MID, 0, 148);
    lv_obj_set_style_radius(display_card, 20, 0);
    lv_obj_set_style_bg_color(display_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(display_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(display_card, 1, 0);
    lv_obj_set_style_border_color(display_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(display_card, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(display_card, 0, 0);
    lv_obj_remove_flag(display_card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *display_title = lv_label_create(display_card);
    lv_label_set_text(display_title, localized("DISPLAY", "屏幕亮度"));
    lv_obj_set_style_text_font(display_title, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(display_title, lv_color_white(), 0);
    lv_obj_align(display_title, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *minus_button = lv_button_create(display_card);
    lv_obj_set_size(minus_button, 42, 42);
    lv_obj_align(minus_button, LV_ALIGN_RIGHT_MID, -124, 0);
    lv_obj_set_style_radius(minus_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(minus_button, lv_color_hex(use_ironman_theme ? 0x7A2714 : 0x263C68), 0);
    lv_obj_add_event_cb(minus_button, settings_brightness_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)-10);
    lv_obj_t *minus_label = lv_label_create(minus_button);
    lv_label_set_text(minus_label, "-");
    lv_obj_set_style_text_font(minus_label, &lv_font_montserrat_20, 0);
    lv_obj_center(minus_label);

    settings_brightness_label = lv_label_create(display_card);
    lv_label_set_text(settings_brightness_label, "100%");
    lv_obj_set_style_text_font(settings_brightness_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(settings_brightness_label, lv_color_hex(0xF2F2F7), 0);
    lv_obj_align(settings_brightness_label, LV_ALIGN_RIGHT_MID, -72, 0);

    lv_obj_t *plus_button = lv_button_create(display_card);
    lv_obj_set_size(plus_button, 42, 42);
    lv_obj_align(plus_button, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_radius(plus_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(plus_button, lv_color_hex(use_ironman_theme ? 0xB73417 : 0x3E5BC0), 0);
    lv_obj_add_event_cb(plus_button, settings_brightness_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)10);
    lv_obj_t *plus_label = lv_label_create(plus_button);
    lv_label_set_text(plus_label, "+");
    lv_obj_set_style_text_font(plus_label, &lv_font_montserrat_20, 0);
    lv_obj_center(plus_label);

    lv_obj_t *sound_card = lv_obj_create(settings_list);
    lv_obj_set_size(sound_card, 340, 62);
    lv_obj_align(sound_card, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_set_style_radius(sound_card, 20, 0);
    lv_obj_set_style_bg_color(sound_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sound_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sound_card, 1, 0);
    lv_obj_set_style_border_color(sound_card, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_opa(sound_card, LV_OPA_20, 0);
    lv_obj_set_style_pad_all(sound_card, 0, 0);
    lv_obj_remove_flag(sound_card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *sound_label = lv_label_create(sound_card);
    lv_label_set_text(sound_label, localized("SOUND", "声音"));
    lv_obj_set_style_text_font(sound_label, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(sound_label, lv_color_white(), 0);
    lv_obj_align(sound_label, LV_ALIGN_LEFT_MID, 18, 0);

    lv_obj_t *volume_down = lv_button_create(sound_card);
    lv_obj_set_size(volume_down, 42, 42);
    lv_obj_align(volume_down, LV_ALIGN_RIGHT_MID, -124, 0);
    lv_obj_set_style_radius(volume_down, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(volume_down, lv_color_hex(use_ironman_theme ? 0x7A2714 : 0x263C68), 0);
    lv_obj_add_event_cb(volume_down, settings_volume_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-10);
    lv_obj_t *volume_down_label = lv_label_create(volume_down);
    lv_label_set_text(volume_down_label, "-");
    lv_obj_set_style_text_font(volume_down_label, &lv_font_montserrat_20, 0);
    lv_obj_center(volume_down_label);

    settings_volume_label = lv_label_create(sound_card);
    lv_label_set_text(settings_volume_label, "70%");
    lv_obj_set_style_text_font(settings_volume_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(settings_volume_label, lv_color_hex(0xF2F2F7), 0);
    lv_obj_align(settings_volume_label, LV_ALIGN_RIGHT_MID, -72, 0);

    lv_obj_t *volume_up = lv_button_create(sound_card);
    lv_obj_set_size(volume_up, 42, 42);
    lv_obj_align(volume_up, LV_ALIGN_RIGHT_MID, -14, 0);
    lv_obj_set_style_radius(volume_up, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(volume_up, lv_color_hex(use_ironman_theme ? 0xB73417 : 0x3E5BC0), 0);
    lv_obj_add_event_cb(volume_up, settings_volume_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)10);
    lv_obj_t *volume_up_label = lv_label_create(volume_up);
    lv_label_set_text(volume_up_label, "+");
    lv_obj_set_style_text_font(volume_up_label, &lv_font_montserrat_20, 0);
    lv_obj_center(volume_up_label);

    lv_obj_t *language_button = lv_button_create(settings_list);
    lv_obj_set_size(language_button, 340, 62);
    lv_obj_align(language_button, LV_ALIGN_TOP_MID, 0, 296);
    lv_obj_set_style_radius(language_button, 20, 0);
    lv_obj_set_style_bg_color(language_button, theme_card(), 0);
    lv_obj_set_style_bg_opa(language_button, LV_OPA_90, 0);
    lv_obj_set_style_border_width(language_button, 1, 0);
    lv_obj_set_style_border_color(language_button, theme_accent(), 0);
    lv_obj_add_event_cb(language_button, settings_preference_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *language_label = lv_label_create(language_button);
    lv_label_set_text(language_label, localized("LANGUAGE", "语言"));
    lv_obj_set_style_text_font(language_label, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(language_label, lv_color_hex(0xD9F7FF), 0);
    lv_obj_align(language_label, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t *language_value = lv_label_create(language_button);
    lv_label_set_text(language_value, use_chinese ? "中文" : "English");
    lv_obj_set_style_text_font(language_value, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_align(language_value, LV_ALIGN_RIGHT_MID, -18, 0);

    lv_obj_t *theme_button = lv_button_create(settings_list);
    lv_obj_set_size(theme_button, 340, 62);
    lv_obj_align(theme_button, LV_ALIGN_TOP_MID, 0, 370);
    lv_obj_set_style_radius(theme_button, 20, 0);
    lv_obj_set_style_bg_color(theme_button, theme_card(), 0);
    lv_obj_set_style_bg_opa(theme_button, LV_OPA_90, 0);
    lv_obj_set_style_border_width(theme_button, 1, 0);
    lv_obj_set_style_border_color(theme_button, theme_accent(), 0);
    lv_obj_add_event_cb(theme_button, settings_preference_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    lv_obj_t *theme_label = lv_label_create(theme_button);
    lv_label_set_text(theme_label, localized("THEME", "主题"));
    lv_obj_set_style_text_font(theme_label, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(theme_label, lv_color_hex(0xD9F7FF), 0);
    lv_obj_align(theme_label, LV_ALIGN_LEFT_MID, 18, 0);
    lv_obj_t *theme_value = lv_label_create(theme_button);
    lv_label_set_text(theme_value, localized(use_ironman_theme ? "IRON MAN" : "AURORA",
                                             use_ironman_theme ? "钢铁侠" : "极光"));
    lv_obj_set_style_text_font(theme_value, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_align(theme_value, LV_ALIGN_RIGHT_MID, -18, 0);

    lv_obj_t *home_button = lv_button_create(settings_view);
    lv_obj_set_size(home_button, 52, 52);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x071524), 0);
    lv_obj_set_style_bg_opa(home_button, LV_OPA_80, 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, lv_color_hex(0x72E7FF), 0);
    lv_obj_add_event_cb(home_button, settings_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, lv_color_hex(0xD9F7FF), 0);
    lv_obj_center(home_icon);
}

static void create_wifi(lv_obj_t *screen)
{
    wifi_view = lv_obj_create(screen);
    lv_obj_set_size(wifi_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(wifi_view);
    lv_obj_set_style_bg_color(wifi_view, theme_background_top(), 0);
    lv_obj_set_style_bg_grad_color(wifi_view, theme_background_bottom(), 0);
    lv_obj_set_style_bg_grad_dir(wifi_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(wifi_view, 0, 0);
    lv_obj_set_style_radius(wifi_view, 0, 0);
    lv_obj_set_style_pad_all(wifi_view, 0, 0);
    lv_obj_remove_flag(wifi_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(wifi_view);

    lv_obj_t *title = lv_label_create(wifi_view);
    lv_label_set_text(title, localized("WI-FI", "无线网络"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE7F8FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 32);

    wifi_status_label = lv_label_create(wifi_view);
    lv_label_set_text(wifi_status_label, localized("SELECT A NETWORK", "选择网络"));
    lv_label_set_long_mode(wifi_status_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(wifi_status_label, 300);
    lv_obj_set_style_text_align(wifi_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wifi_status_label, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(wifi_status_label, lv_color_hex(0xBCEFFF), 0);
    lv_obj_align(wifi_status_label, LV_ALIGN_TOP_MID, 0, 66);

    lv_obj_t *scan_button = lv_button_create(wifi_view);
    lv_obj_set_size(scan_button, 210, 42);
    lv_obj_align(scan_button, LV_ALIGN_TOP_MID, 0, 98);
    lv_obj_set_style_radius(scan_button, 18, 0);
    lv_obj_set_style_bg_color(scan_button, theme_card(), 0);
    lv_obj_set_style_border_width(scan_button, 1, 0);
    lv_obj_set_style_border_color(scan_button, theme_accent(), 0);
    lv_obj_add_event_cb(scan_button, wifi_scan_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *scan_label = lv_label_create(scan_button);
    lv_label_set_text(scan_label, localized("SCAN NETWORKS", "扫描网络"));
    lv_obj_set_style_text_font(scan_label, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_center(scan_label);

    wifi_network_list = lv_obj_create(wifi_view);
    lv_obj_set_size(wifi_network_list, 340, 180);
    lv_obj_align(wifi_network_list, LV_ALIGN_TOP_MID, 0, 154);
    lv_obj_set_style_bg_opa(wifi_network_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(wifi_network_list, 0, 0);
    lv_obj_set_style_pad_all(wifi_network_list, 0, 0);
    lv_obj_remove_flag(wifi_network_list, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back_button = lv_button_create(wifi_view);
    lv_obj_set_size(back_button, 130, 44);
    lv_obj_align(back_button, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_radius(back_button, 18, 0);
    lv_obj_set_style_bg_color(back_button, lv_color_hex(use_ironman_theme ? 0x49120F : 0x132743), 0);
    lv_obj_set_style_border_width(back_button, 1, 0);
    lv_obj_set_style_border_color(back_button, theme_accent(), 0);
    lv_obj_add_event_cb(back_button, wifi_back_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(back_button);
    lv_label_set_text(back_label, localized("BACK", "返回"));
    lv_obj_set_style_text_font(back_label, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_center(back_label);

    wifi_password_panel = lv_obj_create(wifi_view);
    lv_obj_set_size(wifi_password_panel, 466, 466);
    lv_obj_center(wifi_password_panel);
    lv_obj_set_style_radius(wifi_password_panel, 0, 0);
    lv_obj_set_style_bg_color(wifi_password_panel, lv_color_hex(0xF4F5F8), 0);
    lv_obj_set_style_bg_opa(wifi_password_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(wifi_password_panel, 0, 0);
    lv_obj_set_style_border_color(wifi_password_panel, theme_accent(), 0);
    lv_obj_set_style_pad_all(wifi_password_panel, 0, 0);
    lv_obj_remove_flag(wifi_password_panel, LV_OBJ_FLAG_SCROLLABLE);

    wifi_selected_label = lv_label_create(wifi_password_panel);
    lv_label_set_text(wifi_selected_label, localized("PASSWORD", "输入密码"));
    lv_obj_set_style_text_align(wifi_selected_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wifi_selected_label, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(wifi_selected_label, lv_color_hex(0xE7F8FF), 0);
    lv_obj_set_width(wifi_selected_label, 300);
    lv_label_set_long_mode(wifi_selected_label, LV_LABEL_LONG_DOT);
    lv_obj_align(wifi_selected_label, LV_ALIGN_TOP_MID, 0, 52);

    wifi_password_input = lv_textarea_create(wifi_password_panel);
    lv_obj_set_size(wifi_password_input, 330, 42);
    lv_obj_align(wifi_password_input, LV_ALIGN_TOP_MID, 0, 122);
    lv_textarea_set_one_line(wifi_password_input, true);
    lv_textarea_set_password_mode(wifi_password_input, true);
    lv_textarea_set_placeholder_text(wifi_password_input, localized("PASSWORD", "密码"));
    lv_obj_set_style_text_font(wifi_password_input, localized_font(&lv_font_montserrat_14), 0);

    wifi_keyboard = lv_keyboard_create(wifi_password_panel);
    lv_obj_set_size(wifi_keyboard, 390, 166);
    lv_obj_align(wifi_keyboard, LV_ALIGN_TOP_MID, 0, 190);
    lv_obj_set_style_text_font(wifi_keyboard, &lv_font_montserrat_12, 0);
    lv_obj_add_event_cb(wifi_keyboard, wifi_keyboard_event_cb, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(wifi_keyboard, wifi_keyboard_event_cb, LV_EVENT_CANCEL, NULL);

    lv_obj_t *cancel = lv_button_create(wifi_password_panel);
    lv_obj_set_size(cancel, 130, 44);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_add_event_cb(cancel, wifi_cancel_password_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_text = lv_label_create(cancel);
    lv_label_set_text(cancel_text, localized("BACK", "返回"));
    lv_obj_set_style_text_font(cancel_text, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_center(cancel_text);

    lv_obj_add_flag(wifi_password_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *quick = lv_button_create(wifi_view);
    lv_obj_set_size(quick,210,44);
    lv_obj_align(quick,LV_ALIGN_TOP_MID,0,344);
    lv_obj_set_style_bg_color(quick,lv_color_hex(0x147DF5),0);
    lv_obj_set_style_radius(quick,22,0);
    lv_obj_add_event_cb(quick,wifi_setup_event_cb,LV_EVENT_CLICKED,NULL);
    lv_obj_t *quick_label=lv_label_create(quick);
    lv_label_set_text(quick_label,localized("PHONE SETUP","手机快捷配网"));
    lv_obj_set_style_text_font(quick_label,localized_font(&lv_font_montserrat_16),0);
    lv_obj_center(quick_label);
    /* Password overlay must remain above the quick-setup entry. */
    lv_obj_move_foreground(wifi_password_panel);

    wifi_setup_panel=lv_obj_create(wifi_view);
    lv_obj_remove_style_all(wifi_setup_panel);
    lv_obj_set_size(wifi_setup_panel,466,466);
    lv_obj_set_style_bg_color(wifi_setup_panel,lv_color_hex(0xF4F5F8),0);
    lv_obj_set_style_bg_opa(wifi_setup_panel,255,0);
    lv_obj_remove_flag(wifi_setup_panel,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *setup_title=lv_label_create(wifi_setup_panel);
    lv_label_set_text(setup_title,localized("PHONE SETUP","手机配网"));
    lv_obj_set_style_text_font(setup_title,localized_title_font(&lv_font_montserrat_20),0);
    lv_obj_align(setup_title,LV_ALIGN_TOP_MID,0,38);
    wifi_setup_qr=lv_qrcode_create(wifi_setup_panel);
    lv_qrcode_set_size(wifi_setup_qr,196);
    lv_qrcode_set_dark_color(wifi_setup_qr,lv_color_hex(0x101723));
    lv_qrcode_set_light_color(wifi_setup_qr,lv_color_white());
    lv_obj_set_style_border_color(wifi_setup_qr,lv_color_white(),0);
    lv_obj_set_style_border_width(wifi_setup_qr,12,0);
    lv_obj_align(wifi_setup_qr,LV_ALIGN_TOP_MID,0,93);
    wifi_setup_message=lv_label_create(wifi_setup_panel);
    lv_obj_set_width(wifi_setup_message,330);
    lv_obj_set_style_text_align(wifi_setup_message,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_font(wifi_setup_message,localized_font(&lv_font_montserrat_16),0);
    lv_obj_align(wifi_setup_message,LV_ALIGN_TOP_MID,0,309);
    lv_label_set_text(wifi_setup_message,"");
    lv_obj_t *url=lv_label_create(wifi_setup_panel);
    lv_label_set_text(url,"http://192.168.4.1");
    lv_obj_set_style_text_font(url,&lv_font_montserrat_16,0);
    lv_obj_align(url,LV_ALIGN_TOP_MID,0,365);
    lv_obj_t *stop=lv_button_create(wifi_setup_panel);
    lv_obj_set_size(stop,150,44); lv_obj_align(stop,LV_ALIGN_BOTTOM_MID,0,-24);
    lv_obj_set_style_radius(stop,22,0);
    lv_obj_add_event_cb(stop,wifi_setup_event_cb,LV_EVENT_CLICKED,(void *)1);
    lv_obj_t *stop_label=lv_label_create(stop);
    lv_label_set_text(stop_label,localized("CLOSE HOTSPOT","关闭热点"));
    lv_obj_set_style_text_font(stop_label,localized_font(&lv_font_montserrat_16),0);
    lv_obj_center(stop_label);
    lv_obj_add_flag(wifi_setup_panel,LV_OBJ_FLAG_HIDDEN);
}

static void create_quota(lv_obj_t *screen)
{


    quota_view = lv_obj_create(screen);
    lv_obj_set_size(quota_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(quota_view);
    lv_obj_set_style_bg_color(quota_view, lv_color_hex(0x050816), 0);
    lv_obj_set_style_bg_grad_color(quota_view, lv_color_hex(0x182A52), 0);
    lv_obj_set_style_bg_grad_dir(quota_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(quota_view, 0, 0);
    lv_obj_set_style_radius(quota_view, 0, 0);
    lv_obj_set_style_pad_all(quota_view, 0, 0);
    lv_obj_remove_flag(quota_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(quota_view);

    lv_obj_t *title = lv_label_create(quota_view);
    lv_label_set_text(title, localized("TOKEN USAGE", "用量"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xDDEBFF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    quota_total_label = lv_label_create(quota_view);
    lv_label_set_text(quota_total_label, "--");
    lv_obj_set_style_text_font(quota_total_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(quota_total_label, lv_color_hex(0xF2F2F7), 0);
    lv_obj_align(quota_total_label, LV_ALIGN_TOP_MID, 0, 60);

    lv_obj_t *total_caption = lv_label_create(quota_view);
    lv_label_set_text(total_caption, localized("30 DAY TOKENS", "30天用量"));
    lv_obj_set_style_text_font(total_caption,
                               localized_font(&lv_font_montserrat_12), 0);
    lv_obj_set_style_text_color(total_caption, lv_color_hex(0x8E8E93), 0);
    lv_obj_align(total_caption, LV_ALIGN_TOP_MID, 0, 110);

    quota_update_label = lv_label_create(quota_view);
    lv_label_set_text(quota_update_label,
                      localized("WAITING FOR DATA", "等待数据"));
    lv_obj_set_style_text_font(quota_update_label,
                               localized_font(&lv_font_montserrat_12), 0);
    lv_obj_set_style_text_color(quota_update_label, lv_color_hex(0x8E8E93), 0);
    lv_obj_set_style_text_font(quota_update_label, &lv_font_montserrat_12, 0);
    lv_obj_align(quota_update_label, LV_ALIGN_TOP_MID, 0, 139);

    lv_obj_t *refresh_button = lv_button_create(quota_view);
    lv_obj_set_size(refresh_button, 42, 42);
    lv_obj_align(refresh_button, LV_ALIGN_BOTTOM_MID, -65, -20);
    lv_obj_set_style_radius(refresh_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(refresh_button, lv_color_hex(0x173866), 0);
    lv_obj_set_style_bg_opa(refresh_button, LV_OPA_80, 0);
    lv_obj_add_event_cb(refresh_button, quota_refresh_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_icon = lv_label_create(refresh_button);
    lv_label_set_text(refresh_icon, LV_SYMBOL_REFRESH);
    lv_obj_set_style_text_color(refresh_icon, lv_color_hex(0xDDEBFF), 0);
    lv_obj_center(refresh_icon);

    quota_list = lv_obj_create(quota_view);
    lv_obj_set_size(quota_list, 356, 228);
    lv_obj_align(quota_list, LV_ALIGN_TOP_MID, 0, 164);
    lv_obj_set_style_bg_opa(quota_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(quota_list, 0, 0);
    lv_obj_set_style_pad_all(quota_list, 0, 0);
    lv_obj_set_scroll_dir(quota_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(quota_list, LV_SCROLLBAR_MODE_ON);
    lv_obj_add_flag(quota_list, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(quota_list, LV_OBJ_FLAG_SCROLL_ELASTIC |
                                   LV_OBJ_FLAG_OVERFLOW_VISIBLE);
    lv_obj_set_style_width(quota_list, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(quota_list, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(quota_list, lv_color_hex(0x636366), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(quota_list, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_clip_corner(quota_list, true, 0);

    for (size_t i = 0; i < QUOTA_MAX_AGENTS; ++i) {
        lv_obj_t *card = lv_obj_create(quota_list);
        quota_agent_cards[i] = card;
        lv_obj_set_size(card, 326, 100);
        lv_obj_set_pos(card, 10, i * 110);
        lv_obj_set_style_radius(card, 18, 0);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_pad_all(card, 0, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        quota_agent_name_labels[i] = lv_label_create(card);
        lv_label_set_text(quota_agent_name_labels[i], "--");
        lv_label_set_long_mode(quota_agent_name_labels[i], LV_LABEL_LONG_DOT);
        lv_obj_set_width(quota_agent_name_labels[i], 270);
        lv_obj_set_style_text_font(quota_agent_name_labels[i],
                                   &round_clock_cn_20, 0);
        lv_obj_set_style_text_color(quota_agent_name_labels[i], lv_color_white(), 0);
        lv_obj_align(quota_agent_name_labels[i], LV_ALIGN_TOP_LEFT, 16, 10);

        quota_agent_usage_labels[i] = lv_label_create(card);
        lv_label_set_text(quota_agent_usage_labels[i], "30D --");
        lv_obj_set_style_text_font(quota_agent_usage_labels[i],
                                   &round_clock_cn_20, 0);
        lv_obj_set_style_text_color(quota_agent_usage_labels[i],
                                    lv_color_hex(0x8E8E93), 0);
        lv_obj_align(quota_agent_usage_labels[i], LV_ALIGN_TOP_LEFT, 16, 39);
        quota_agent_detail_labels[i] = lv_label_create(card);
        lv_label_set_text(quota_agent_detail_labels[i], "--");
        lv_obj_set_style_text_font(quota_agent_detail_labels[i], &round_clock_cn_16, 0);
        lv_obj_align(quota_agent_detail_labels[i], LV_ALIGN_TOP_LEFT, 16, 72);
        lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);
    }



    lv_obj_t *home_button = lv_button_create(quota_view);
    lv_obj_set_size(home_button, 48, 48);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x071524), 0);
    lv_obj_set_style_bg_opa(home_button, LV_OPA_90, 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, lv_color_hex(0x72E7FF), 0);
    lv_obj_add_event_cb(home_button, quota_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, lv_color_hex(0xD9F7FF), 0);
    lv_obj_center(home_icon);
}

/* A vertical document: key facts above the fold, evidence below it. */
static lv_obj_t *reset_text(lv_obj_t *parent, const char *text, int y, const lv_font_t *font)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, 318);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, 0, y);
    return label;
}

static void create_codex_reset(lv_obj_t *screen)
{
    codex_reset_view = lv_obj_create(screen);
    lv_obj_set_size(codex_reset_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(codex_reset_view);
    lv_obj_set_style_bg_color(codex_reset_view, lv_color_hex(0xE8F3FF), 0);
    lv_obj_set_style_border_width(codex_reset_view, 0, 0);
    lv_obj_set_style_radius(codex_reset_view, 0, 0);
    lv_obj_set_style_pad_all(codex_reset_view, 0, 0);
    lv_obj_remove_flag(codex_reset_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(codex_reset_view);
    lv_obj_t *title = reset_text(codex_reset_view, "重置雷达", 0, &round_clock_cn_28);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 38);

    lv_obj_t *scroll = lv_obj_create(codex_reset_view);
    lv_obj_set_size(scroll, 346, 298);
    lv_obj_align(scroll, LV_ALIGN_TOP_MID, 0, 88);
    lv_obj_set_style_radius(scroll, 24, 0);
    lv_obj_set_style_bg_color(scroll, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scroll, LV_OPA_80, 0);
    lv_obj_set_style_border_width(scroll, 0, 0);
    lv_obj_set_style_pad_all(scroll, 14, 0);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_remove_flag(scroll, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_CHAIN);
    codex_reset_state_label = reset_text(scroll, "等待重置记录", 0, &round_clock_cn_20);
    codex_reset_time_label = reset_text(scroll, "--", 28, &round_clock_cn_28);
    codex_reset_age_label = reset_text(scroll, "时间未知，不触发提醒", 61, &round_clock_cn_20);
    reset_text(scroll, "未来24小时     48小时内", 108, &round_clock_cn_20);
    codex_reset_odds_label = reset_text(scroll, "--          --", 140, &round_clock_cn_28);
    codex_reset_meta_label = reset_text(scroll, "预测尚未同步", 185, &round_clock_cn_20);
    reset_text(scroll, "上滑查看依据与更新时间", 239, &round_clock_cn_20);
    codex_reset_update_label = reset_text(scroll, "等待来源更新", 289, &round_clock_cn_20);
    codex_reset_summary_label = reset_text(scroll, "暂无预测说明", 350, &round_clock_cn_20);

    lv_obj_t *refresh_button = lv_button_create(codex_reset_view);
    lv_obj_set_size(refresh_button, 44, 44);
    lv_obj_align(refresh_button, LV_ALIGN_BOTTOM_MID, -62, -24);
    lv_obj_set_style_radius(refresh_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(refresh_button, lv_color_hex(0x6E1930), 0);
    lv_obj_add_event_cb(refresh_button, codex_reset_refresh_event_cb,
                        LV_EVENT_CLICKED, NULL);
    lv_obj_t *refresh_icon = lv_label_create(refresh_button);
    lv_label_set_text(refresh_icon, LV_SYMBOL_REFRESH);
    lv_obj_center(refresh_icon);

    lv_obj_t *home_button = lv_button_create(codex_reset_view);
    lv_obj_set_size(home_button, 52, 52);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x260B13), 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, lv_color_hex(0xFF9278), 0);
    lv_obj_add_event_cb(home_button, codex_reset_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, lv_color_white(), 0);
    lv_obj_center(home_icon);
}

static void create_desktop(lv_obj_t *screen)
{
    desktop_view = lv_obj_create(screen);
    lv_obj_set_size(desktop_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(desktop_view);
    lv_obj_set_style_radius(desktop_view, 0, 0);
    lv_obj_set_style_pad_all(desktop_view, 0, 0);
    lv_obj_remove_flag(desktop_view, LV_OBJ_FLAG_SCROLLABLE);
    desktop_time_label = lv_label_create(desktop_view);
    lv_label_set_text(desktop_time_label, "--:--");
    lv_obj_set_style_text_font(desktop_time_label, &lv_font_montserrat_20, 0);
    lv_obj_align(desktop_time_label, LV_ALIGN_TOP_MID, 0, 45);
    launcher_create(desktop_view, localized_title_font(&lv_font_montserrat_20),
                    use_chinese ? desktop_launcher_chinese_names : desktop_launcher_english_names,
                    sizeof(desktop_launcher_targets) / sizeof(desktop_launcher_targets[0]),
                    launcher_open, launcher_overview);
}

static void create_app_overview(lv_obj_t *screen)
{
    const app_view_t *targets = desktop_launcher_targets;
    const char *const *english_names = desktop_launcher_english_names;
    const char *const *chinese_names = desktop_launcher_chinese_names;

    app_overview_view = lv_obj_create(screen);
    lv_obj_set_size(app_overview_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(app_overview_view);
    lv_obj_set_style_bg_color(app_overview_view, theme_background_top(), 0);
    lv_obj_set_style_bg_grad_color(app_overview_view, theme_background_bottom(), 0);
    lv_obj_set_style_bg_grad_dir(app_overview_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(app_overview_view, 0, 0);
    lv_obj_set_style_radius(app_overview_view, 0, 0);
    lv_obj_set_style_pad_all(app_overview_view, 0, 0);
    lv_obj_remove_flag(app_overview_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(app_overview_view);

    lv_obj_t *title = lv_label_create(app_overview_view);
    lv_label_set_text(title, localized("ALL APPS", "所有应用"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF5E7D8), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 18);

    app_overview_grid = lv_obj_create(app_overview_view);
    lv_obj_set_size(app_overview_grid, 374, 320);
    lv_obj_align(app_overview_grid, LV_ALIGN_TOP_MID, 0, 64);
    lv_obj_set_style_bg_opa(app_overview_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(app_overview_grid, 0, 0);
    lv_obj_set_style_radius(app_overview_grid, 0, 0);
    lv_obj_set_style_pad_all(app_overview_grid, 0, 0);
    lv_obj_set_scroll_dir(app_overview_grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(app_overview_grid, LV_SCROLLBAR_MODE_ON);
    lv_obj_add_flag(app_overview_grid, LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_remove_flag(app_overview_grid, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_set_style_width(app_overview_grid, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(app_overview_grid, LV_RADIUS_CIRCLE, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(app_overview_grid, theme_accent(), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(app_overview_grid, LV_OPA_70, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(app_overview_grid, 2, LV_PART_SCROLLBAR);

    const size_t app_count = sizeof(desktop_launcher_targets) / sizeof(desktop_launcher_targets[0]);
    for (size_t i = 0; i < app_count; ++i) {
        int16_t x = (i % 2 == 0) ? 46 : 196;
        if (i + 1 == app_count && app_count % 2 != 0) {
            x = 121;
        }
        int16_t y = (i / 2) * 160;
        lv_obj_t *button = lv_button_create(app_overview_grid);
        lv_obj_set_size(button, 132, 132);
        lv_obj_set_pos(button, x, y);
        lv_obj_set_style_radius(button, 38, 0);
        lv_obj_set_style_bg_color(button, lv_color_hex(0x147DF5), 0);
        lv_obj_set_style_border_width(button, 1, 0);
        lv_obj_set_style_border_color(button, theme_accent(), 0);
        lv_obj_add_event_cb(button, overview_app_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)targets[i]);



        lv_obj_t *name = lv_label_create(app_overview_grid);
        lv_label_set_text(name, localized(english_names[i], chinese_names[i]));
        lv_obj_set_width(name, 140);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(name, localized_font(&lv_font_montserrat_14), 0);
        lv_obj_set_style_text_color(name, lv_color_hex(0xF5E7D8), 0);
        lv_obj_align_to(name, button, LV_ALIGN_OUT_BOTTOM_MID, 0, 7);
    }

    lv_obj_t *home_button = lv_button_create(app_overview_view);
    lv_obj_set_size(home_button, 48, 48);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x071524), 0);
    lv_obj_set_style_bg_opa(home_button, LV_OPA_80, 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, theme_accent(), 0);
    lv_obj_add_event_cb(home_button, overview_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, lv_color_hex(0xD9F7FF), 0);
    lv_obj_center(home_icon);
}

static void create_remote(lv_obj_t *screen)
{
    remote_view = lv_obj_create(screen);
    lv_obj_set_size(remote_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(remote_view);
    lv_obj_set_style_bg_color(remote_view, theme_background_top(), 0);
    lv_obj_set_style_bg_grad_color(remote_view, theme_background_bottom(), 0);
    lv_obj_set_style_bg_grad_dir(remote_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(remote_view, 0, 0);
    lv_obj_set_style_radius(remote_view, 0, 0);
    lv_obj_set_style_pad_all(remote_view, 0, 0);
    lv_obj_remove_flag(remote_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(remote_view);

    lv_obj_t *title = lv_label_create(remote_view);
    lv_label_set_text(title, localized("ONLINE VOICE", "联网语音"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xE7F8FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    remote_status_label = lv_label_create(remote_view);
    lv_label_set_text(remote_status_label,
                      localized_remote_status(voice_input_status()));
    lv_label_set_long_mode(remote_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(remote_status_label, 350, 76);
    lv_obj_set_style_text_align(remote_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(remote_status_label,
                               localized_font(&lv_font_montserrat_12), 0);
    lv_obj_set_style_text_color(remote_status_label, lv_color_hex(0x9DE9FF), 0);
    lv_obj_align(remote_status_label, LV_ALIGN_TOP_MID, 0, 70);

    create_voice_wave(remote_view, &remote_voice_wave, 150, 0x0A84FF);

    lv_obj_t *voice_button = lv_button_create(remote_view);
    record_controls[0] = voice_button;
    lv_obj_set_size(voice_button, 150, 96);
    lv_obj_align(voice_button, LV_ALIGN_TOP_MID, -82, 166);
    lv_obj_set_style_radius(voice_button, 34, 0);
    lv_obj_set_style_bg_color(voice_button, lv_color_hex(use_ironman_theme ? 0xA22814 : 0x167C9E), 0);
    lv_obj_set_style_bg_grad_color(voice_button, lv_color_hex(use_ironman_theme ? 0xF0A420 : 0x23B889), 0);
    lv_obj_set_style_bg_grad_dir(voice_button, LV_GRAD_DIR_VER, 0);
    lv_obj_add_event_cb(voice_button, remote_action_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *voice_label = lv_label_create(voice_button);
    lv_label_set_text(voice_label, localized("START\nRECORDING", "开始录音"));
    lv_obj_set_style_text_align(voice_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(voice_label, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_center(voice_label);

    lv_obj_t *confirm_button = lv_button_create(remote_view);
    finish_controls[0] = confirm_button;
    lv_obj_set_size(confirm_button, 150, 96);
    lv_obj_align(confirm_button, LV_ALIGN_TOP_MID, 82, 166);
    lv_obj_set_style_radius(confirm_button, 34, 0);
    lv_obj_set_style_bg_color(confirm_button, lv_color_hex(use_ironman_theme ? 0x7C1A12 : 0x244980), 0);
    lv_obj_set_style_bg_grad_color(confirm_button, lv_color_hex(use_ironman_theme ? 0xE0611A : 0x526CCE), 0);
    lv_obj_set_style_bg_grad_dir(confirm_button, LV_GRAD_DIR_VER, 0);
    lv_obj_add_event_cb(confirm_button, remote_action_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    lv_obj_t *confirm_label = lv_label_create(confirm_button);
    lv_label_set_text(confirm_label, localized("STOP AND\nRECOGNIZE", "停止并识别"));
    lv_obj_set_style_text_align(confirm_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(confirm_label, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_center(confirm_label);

    lv_obj_t *home_button = lv_button_create(remote_view);
    lv_obj_set_size(home_button, 52, 52);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x071524), 0);
    lv_obj_set_style_bg_opa(home_button, LV_OPA_80, 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, theme_accent(), 0);
    lv_obj_add_event_cb(home_button, remote_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, lv_color_hex(0xD9F7FF), 0);
    lv_obj_center(home_icon);
}

static void music_home_event_cb(lv_event_t *event)
{
    if (screen_on && lv_event_get_code(event) == LV_EVENT_CLICKED) {
        show_view_locked(APP_VIEW_DESKTOP);
    }
}

static void create_music(lv_obj_t *screen)
{
    music_view = lv_obj_create(screen);
    lv_obj_set_size(music_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(music_view);
    lv_obj_set_style_bg_color(music_view, lv_color_hex(0x150828), 0);
    lv_obj_set_style_bg_grad_color(music_view, lv_color_hex(0x4C145D), 0);
    lv_obj_set_style_bg_grad_dir(music_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(music_view, 0, 0);
    lv_obj_set_style_radius(music_view, 0, 0);
    lv_obj_set_style_pad_all(music_view, 0, 0);
    lv_obj_remove_flag(music_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(music_view);

    lv_obj_t *title = lv_label_create(music_view);
    lv_label_set_text(title, localized("MUSIC STUDIO", "音乐创作"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xF8D5FF), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    music_status_label = lv_label_create(music_view);
    lv_label_set_text(music_status_label, localized_remote_status(music_input_status()));
    lv_label_set_long_mode(music_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(music_status_label, 346, 60);
    lv_obj_set_style_text_align(music_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(music_status_label, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(music_status_label, lv_color_hex(0xF0B8FF), 0);
    lv_obj_align(music_status_label, LV_ALIGN_TOP_MID, 0, 78);

    create_voice_wave(music_view, &music_voice_wave, 144, 0xAF52DE);

    lv_obj_t *record_button = lv_button_create(music_view);
    record_controls[1] = record_button;
    lv_obj_set_size(record_button, 150, 96);
    lv_obj_align(record_button, LV_ALIGN_TOP_MID, -82, 160);
    lv_obj_set_style_radius(record_button, 34, 0);
    lv_obj_set_style_bg_color(record_button, lv_color_hex(0x6D28D9), 0);
    lv_obj_set_style_bg_grad_color(record_button, lv_color_hex(0xC026D3), 0);
    lv_obj_set_style_bg_grad_dir(record_button, LV_GRAD_DIR_VER, 0);
    lv_obj_add_event_cb(record_button, music_action_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)1);
    lv_obj_t *record_label = lv_label_create(record_button);
    lv_label_set_text(record_label, localized("SPEAK\nIDEA", "开始录音"));
    lv_obj_set_style_text_align(record_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(record_label, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_center(record_label);

    lv_obj_t *finish_button = lv_button_create(music_view);
    finish_controls[1] = finish_button;
    lv_obj_set_size(finish_button, 150, 96);
    lv_obj_align(finish_button, LV_ALIGN_TOP_MID, 82, 160);
    lv_obj_set_style_radius(finish_button, 34, 0);
    lv_obj_set_style_bg_color(finish_button, lv_color_hex(0x312E81), 0);
    lv_obj_set_style_bg_grad_color(finish_button, lv_color_hex(0x7E22CE), 0);
    lv_obj_set_style_bg_grad_dir(finish_button, LV_GRAD_DIR_VER, 0);
    lv_obj_add_event_cb(finish_button, music_action_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)2);
    lv_obj_t *finish_label = lv_label_create(finish_button);
    lv_label_set_text(finish_label, localized("STOP AND\nCREATE", "生成音乐"));
    lv_obj_set_style_text_align(finish_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(finish_label, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_center(finish_label);

    lv_obj_t *play_button = lv_button_create(music_view);
    lv_obj_set_size(play_button, 70, 70);
    lv_obj_align(play_button, LV_ALIGN_BOTTOM_MID, -62, -26);
    lv_obj_set_style_radius(play_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play_button, lv_color_hex(0xC026D3), 0);
    lv_obj_add_event_cb(play_button, music_action_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)3);
    lv_obj_t *play_icon = lv_label_create(play_button);
    lv_label_set_text(play_icon, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(play_icon, &lv_font_montserrat_20, 0);
    lv_obj_center(play_icon);

    lv_obj_t *home_button = lv_button_create(music_view);
    lv_obj_set_size(home_button, 52, 52);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 44, -35);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x2E1065), 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, lv_color_hex(0xF0ABFC), 0);
    lv_obj_add_event_cb(home_button, music_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_center(home_icon);
}

static void create_radio(lv_obj_t *screen)
{
    static const char *station_names[] = {"星空", "极光", "能量"};
    static const uint32_t station_colors[] = {0x273C8E, 0x0F766E, 0xB45309};

    radio_view = lv_obj_create(screen);
    lv_obj_set_size(radio_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(radio_view);
    lv_obj_set_style_bg_color(radio_view, lv_color_hex(0x07112D), 0);
    lv_obj_set_style_bg_grad_color(radio_view, lv_color_hex(0x1E1B4B), 0);
    lv_obj_set_style_bg_grad_dir(radio_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(radio_view, 0, 0);
    lv_obj_set_style_radius(radio_view, 0, 0);
    lv_obj_set_style_pad_all(radio_view, 0, 0);
    lv_obj_remove_flag(radio_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(radio_view);

    lv_obj_t *title = lv_label_create(radio_view);
    lv_label_set_text(title, localized("AI RADIO", "AI 电台"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xC4B5FD), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 28);

    radio_status_label = lv_label_create(radio_view);
    lv_label_set_text(radio_status_label, localized("PICK A CHANNEL, THEN SPEAK", "选择频道，再说感受"));
    lv_label_set_long_mode(radio_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(radio_status_label, 350, 48);
    lv_obj_set_style_text_align(radio_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(radio_status_label, localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(radio_status_label, lv_color_hex(0xDDD6FE), 0);
    lv_obj_align(radio_status_label, LV_ALIGN_TOP_MID, 0, 70);

    for (int i = 0; i < 3; ++i) {
        lv_obj_t *station = lv_button_create(radio_view);
        lv_obj_set_size(station, 92, 64);
        lv_obj_set_pos(station, 77 + i * 108, 132);
        lv_obj_set_style_radius(station, 23, 0);
        lv_obj_set_style_bg_color(station, lv_color_hex(station_colors[i]), 0);
        lv_obj_set_style_border_width(station, 1, 0);
        lv_obj_set_style_border_color(station, lv_color_hex(0xC4B5FD), 0);
        radio_station_buttons[i] = station;
        lv_obj_add_event_cb(station, radio_station_event_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)i);
        lv_obj_t *name = lv_label_create(station);
        lv_label_set_text(name, localized(i == 0 ? "SKY" : i == 1 ? "AURORA" : "ENERGY",
                                          station_names[i]));
        lv_obj_set_style_text_font(name, localized_font(&lv_font_montserrat_12), 0);
        lv_obj_center(name);
    }

    create_voice_wave(radio_view, &radio_voice_wave, 210, 0x5856D6);

    lv_obj_t *record = lv_button_create(radio_view);
    record_controls[2] = record;
    lv_obj_set_size(record, 138, 88);
    lv_obj_align(record, LV_ALIGN_TOP_MID, -76, 224);
    lv_obj_set_style_radius(record, 30, 0);
    lv_obj_set_style_bg_color(record, lv_color_hex(0x6D28D9), 0);
    lv_obj_add_event_cb(record, radio_action_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_t *record_name = lv_label_create(record);
    lv_label_set_text(record_name, localized("SPEAK\nMOOD", "说出想法"));
    lv_obj_set_style_text_align(record_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(record_name, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_center(record_name);

    lv_obj_t *stop = lv_button_create(radio_view);
    finish_controls[2] = stop;
    lv_obj_set_size(stop, 138, 88);
    lv_obj_align(stop, LV_ALIGN_TOP_MID, 76, 224);
    lv_obj_set_style_radius(stop, 30, 0);
    lv_obj_set_style_bg_color(stop, lv_color_hex(0x312E81), 0);
    lv_obj_add_event_cb(stop, radio_action_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)2);
    lv_obj_t *stop_name = lv_label_create(stop);
    lv_label_set_text(stop_name, localized("STOP\nCREATE", "生成音乐"));
    lv_obj_set_style_text_align(stop_name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(stop_name, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_center(stop_name);

    lv_obj_t *play = lv_button_create(radio_view);
    lv_obj_set_size(play, 62, 62);
    lv_obj_align(play, LV_ALIGN_BOTTOM_MID, -48, -22);
    lv_obj_set_style_radius(play, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(play, lv_color_hex(0x8B5CF6), 0);
    lv_obj_add_event_cb(play, radio_action_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)3);
    lv_obj_t *play_icon = lv_label_create(play);
    lv_label_set_text(play_icon, LV_SYMBOL_PLAY);
    lv_obj_center(play_icon);

    lv_obj_t *home = lv_button_create(radio_view);
    lv_obj_set_size(home, 52, 52);
    lv_obj_align(home, LV_ALIGN_BOTTOM_MID, 44, -27);
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x171033), 0);
    lv_obj_add_event_cb(home, radio_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_center(home_icon);
}

static void create_wallpaper_studio(lv_obj_t *screen)
{
    wallpaper_studio_view = lv_obj_create(screen);
    lv_obj_set_size(wallpaper_studio_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(wallpaper_studio_view);
    lv_obj_set_style_bg_color(wallpaper_studio_view, lv_color_hex(0x041A1F), 0);
    lv_obj_set_style_bg_grad_color(wallpaper_studio_view, lv_color_hex(0x063C55), 0);
    lv_obj_set_style_bg_grad_dir(wallpaper_studio_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(wallpaper_studio_view, 0, 0);
    lv_obj_set_style_radius(wallpaper_studio_view, 0, 0);
    lv_obj_set_style_pad_all(wallpaper_studio_view, 0, 0);
    lv_obj_remove_flag(wallpaper_studio_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(wallpaper_studio_view);

    lv_obj_t *title = lv_label_create(wallpaper_studio_view);
    lv_label_set_text(title, localized("LIVE WALLPAPER", "动态壁纸"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xA5F3FC), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 30);

    wallpaper_studio_status_label = lv_label_create(wallpaper_studio_view);
    lv_label_set_text(wallpaper_studio_status_label,
                      localized_remote_status(wallpaper_input_status()));
    lv_label_set_long_mode(wallpaper_studio_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(wallpaper_studio_status_label, 350, 62);
    lv_obj_set_style_text_align(wallpaper_studio_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wallpaper_studio_status_label,
                               localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(wallpaper_studio_status_label, lv_color_hex(0xCFFAFE), 0);
    lv_obj_align(wallpaper_studio_status_label, LV_ALIGN_TOP_MID, 0, 80);

    lv_obj_t *orb = lv_obj_create(wallpaper_studio_view);
    lv_obj_set_size(orb, 150, 150);
    lv_obj_align(orb, LV_ALIGN_CENTER, 0, -5);
    lv_obj_set_style_radius(orb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(orb, lv_color_hex(0x0A84FF), 0);
    lv_obj_set_style_bg_grad_dir(orb, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(orb, LV_OPA_30, 0);
    lv_obj_set_style_border_width(orb, 1, 0);
    lv_obj_set_style_border_color(orb, lv_color_hex(0x5AC8FA), 0);
    lv_obj_remove_flag(orb, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    create_voice_wave(wallpaper_studio_view, &wallpaper_voice_wave, 232, 0x0A84FF);

    wallpaper_record_button = lv_button_create(wallpaper_studio_view);
    record_controls[3] = wallpaper_record_button;
    lv_obj_set_size(wallpaper_record_button, 138, 86);
    lv_obj_align(wallpaper_record_button, LV_ALIGN_BOTTOM_MID, -76, -50);
    lv_obj_set_style_radius(wallpaper_record_button, 30, 0);
    lv_obj_set_style_bg_color(wallpaper_record_button, lv_color_hex(0x0E7490), 0);
    lv_obj_add_event_cb(wallpaper_record_button, wallpaper_studio_action_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)1);
    wallpaper_record_label = lv_label_create(wallpaper_record_button);
    lv_label_set_text(wallpaper_record_label, localized("SPEAK\nSCENE", "描述画面"));
    lv_obj_set_style_text_align(wallpaper_record_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wallpaper_record_label, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_center(wallpaper_record_label);

    wallpaper_finish_button = lv_button_create(wallpaper_studio_view);
    finish_controls[3] = wallpaper_finish_button;
    lv_obj_set_size(wallpaper_finish_button, 138, 86);
    lv_obj_align(wallpaper_finish_button, LV_ALIGN_BOTTOM_MID, 76, -50);
    lv_obj_set_style_radius(wallpaper_finish_button, 30, 0);
    lv_obj_set_style_bg_color(wallpaper_finish_button, lv_color_hex(0x155E75), 0);
    lv_obj_add_event_cb(wallpaper_finish_button, wallpaper_studio_action_event_cb, LV_EVENT_CLICKED,
                        (void *)(intptr_t)2);
    wallpaper_finish_label = lv_label_create(wallpaper_finish_button);
    lv_label_set_text(wallpaper_finish_label, localized("STOP\nCREATE", "生成壁纸"));
    lv_obj_set_style_text_align(wallpaper_finish_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(wallpaper_finish_label, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_center(wallpaper_finish_label);

    lv_obj_t *home = lv_button_create(wallpaper_studio_view);
    lv_obj_set_size(home, 46, 46);
    lv_obj_align(home, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_set_style_radius(home, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home, lv_color_hex(0x083344), 0);
    lv_obj_add_event_cb(home, wallpaper_studio_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_center(home_icon);
}

static void create_bluetooth(lv_obj_t *screen)
{
    bluetooth_view = lv_obj_create(screen);
    lv_obj_set_size(bluetooth_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(bluetooth_view);
    lv_obj_set_style_bg_color(bluetooth_view, lv_color_hex(0x07152E), 0);
    lv_obj_set_style_bg_grad_color(bluetooth_view, lv_color_hex(0x123C78), 0);
    lv_obj_set_style_bg_grad_dir(bluetooth_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(bluetooth_view, 0, 0);
    lv_obj_set_style_radius(bluetooth_view, 0, 0);
    lv_obj_set_style_pad_all(bluetooth_view, 0, 0);
    lv_obj_remove_flag(bluetooth_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(bluetooth_view);

    lv_obj_t *icon = lv_label_create(bluetooth_view);
    lv_label_set_text(icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x74C7FF), 0);
    lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, 42);

    lv_obj_t *title = lv_label_create(bluetooth_view);
    lv_label_set_text(title, localized("BLUETOOTH", "蓝牙"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_white(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 103);

    bluetooth_status_label = lv_label_create(bluetooth_view);
    lv_label_set_text(bluetooth_status_label,
                      localized_remote_status(vibe_remote_status()));
    lv_label_set_long_mode(bluetooth_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_size(bluetooth_status_label, 340, 60);
    lv_obj_set_style_text_align(bluetooth_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(bluetooth_status_label,
                               localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(bluetooth_status_label, lv_color_hex(0xBCEBFF), 0);
    lv_obj_align(bluetooth_status_label, LV_ALIGN_TOP_MID, 0, 145);

    lv_obj_t *pair_button = lv_button_create(bluetooth_view);
    lv_obj_set_size(pair_button, 280, 72);
    lv_obj_align(pair_button, LV_ALIGN_CENTER, 0, 42);
    lv_obj_set_style_radius(pair_button, 28, 0);
    lv_obj_set_style_bg_color(pair_button, lv_color_hex(0x1266C5), 0);
    lv_obj_set_style_bg_grad_color(pair_button, lv_color_hex(0x13A4D8), 0);
    lv_obj_set_style_bg_grad_dir(pair_button, LV_GRAD_DIR_HOR, 0);
    lv_obj_set_style_border_width(pair_button, 1, 0);
    lv_obj_set_style_border_color(pair_button, lv_color_hex(0xA8E7FF), 0);
    lv_obj_add_event_cb(pair_button, bluetooth_action_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *pair_label = lv_label_create(pair_button);
    lv_label_set_text(pair_label, localized("TURN ON / PAIR", "开启蓝牙 / 配对"));
    lv_obj_set_style_text_font(pair_label, localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(pair_label, lv_color_white(), 0);
    lv_obj_center(pair_label);

    lv_obj_t *note = lv_label_create(bluetooth_view);
    lv_label_set_text(note, localized("SYSTEM SETTING", "系统设置"));
    lv_obj_set_style_text_font(note, localized_font(&lv_font_montserrat_12), 0);
    lv_obj_set_style_text_color(note, lv_color_hex(0x8DB8D8), 0);
    lv_obj_align(note, LV_ALIGN_CENTER, 0, 104);

    lv_obj_t *home_button = lv_button_create(bluetooth_view);
    lv_obj_set_size(home_button, 52, 52);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x071524), 0);
    lv_obj_set_style_bg_opa(home_button, LV_OPA_80, 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, lv_color_hex(0x72E7FF), 0);
    lv_obj_add_event_cb(home_button, bluetooth_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_color(home_icon, lv_color_hex(0xD9F7FF), 0);
    lv_obj_center(home_icon);
}

static void create_avatar(lv_obj_t *screen)
{
    avatar_view = lv_obj_create(screen);
    lv_obj_set_size(avatar_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(avatar_view);
    lv_obj_set_style_bg_color(avatar_view, lv_color_hex(0x08051A), 0);
    lv_obj_set_style_bg_grad_color(avatar_view,
                                   lv_color_hex(use_ironman_theme ? 0x5B130B : 0x32195E), 0);
    lv_obj_set_style_bg_grad_dir(avatar_view, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(avatar_view, 0, 0);
    lv_obj_set_style_radius(avatar_view, 0, 0);
    lv_obj_set_style_pad_all(avatar_view, 0, 0);
    lv_obj_remove_flag(avatar_view, LV_OBJ_FLAG_SCROLLABLE);
    create_gesture_layer(avatar_view);

    lv_obj_t *title = lv_label_create(avatar_view);
    lv_label_set_text(title, localized("MY AVATAR", "互动头像"));
    lv_obj_set_style_text_font(title, localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFE7FA), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 24);

    avatar_status_label = lv_label_create(avatar_view);
    lv_label_set_text(avatar_status_label,
                      localized("UPLOAD FROM MAC", "请从电脑上传"));
    lv_obj_set_style_text_font(avatar_status_label,
                               localized_font(&lv_font_montserrat_14), 0);
    lv_obj_set_style_text_color(avatar_status_label, lv_color_hex(0xA9EFFF), 0);
    lv_obj_align(avatar_status_label, LV_ALIGN_TOP_MID, 0, 55);
    avatar_sync_label=lv_label_create(avatar_view);
    lv_label_set_text(avatar_sync_label,avatar_store_sync_status());
    lv_obj_set_style_text_font(avatar_sync_label,&round_clock_cn_16,0);
    lv_obj_set_style_text_color(avatar_sync_label,lv_color_hex(0x687587),0);
    lv_obj_align(avatar_sync_label,LV_ALIGN_TOP_MID,0,80);

    lv_obj_t *halo = lv_obj_create(avatar_view);
    lv_obj_set_size(halo, 280, 280);
    lv_obj_align(halo, LV_ALIGN_CENTER, 0, 12);
    lv_obj_set_style_radius(halo, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(halo, lv_color_hex(0xFFE3ED), 0);
    lv_obj_set_style_bg_grad_color(halo, lv_color_hex(0x14B8A6), 0);
    lv_obj_set_style_bg_grad_dir(halo, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(halo, LV_OPA_70, 0);
    lv_obj_set_style_border_width(halo, 2, 0);
    lv_obj_set_style_border_color(halo, lv_color_hex(0x85F4FF), 0);
    lv_obj_remove_flag(halo, LV_OBJ_FLAG_SCROLLABLE);

    avatar_image = lv_image_create(avatar_view);
    lv_image_set_src(avatar_image, &avatar_image_descriptor);
    lv_obj_align(avatar_image, LV_ALIGN_CENTER, 0, 12);
    lv_image_set_pivot(avatar_image, AVATAR_WIDTH / 2, AVATAR_HEIGHT / 2);
    lv_image_set_scale(avatar_image, 256);
    lv_obj_add_flag(avatar_image, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(avatar_image, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(avatar_image, avatar_event_cb, LV_EVENT_ALL, NULL);
    lv_timer_create(avatar_face_tick,40,NULL);

    uint8_t latest = avatar_store_latest_slot();
    if (latest < AVATAR_MAX_SLOTS && load_avatar_slot_locked(latest)) {
        avatar_revision_seen = avatar_store_revision();
    } else {
        lv_obj_add_flag(avatar_image, LV_OBJ_FLAG_HIDDEN);
    }

    avatar_reaction_label = lv_label_create(avatar_view);
    lv_label_set_text(avatar_reaction_label, localized("HELLO!", "你好呀"));
    lv_obj_set_style_text_font(avatar_reaction_label,
                               localized_title_font(&lv_font_montserrat_20), 0);
    lv_obj_set_style_text_color(avatar_reaction_label, lv_color_hex(0x72E7FF), 0);
    lv_obj_set_style_bg_color(avatar_reaction_label, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(avatar_reaction_label, LV_OPA_80, 0);
    lv_obj_set_style_pad_hor(avatar_reaction_label, 16, 0);
    lv_obj_set_style_pad_ver(avatar_reaction_label, 8, 0);
    lv_obj_set_style_radius(avatar_reaction_label, 18, 0);
    lv_obj_align(avatar_reaction_label, LV_ALIGN_TOP_MID, 0, 82);
    lv_obj_add_flag(avatar_reaction_label, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *home_button = lv_button_create(avatar_view);
    lv_obj_set_size(home_button, 52, 52);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 0, -14);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x071524), 0);
    lv_obj_set_style_bg_opa(home_button, LV_OPA_80, 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, lv_color_hex(0x72E7FF), 0);
    lv_obj_add_event_cb(home_button, avatar_home_event_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, lv_color_hex(0xD9F7FF), 0);
    lv_obj_center(home_icon);
}

static void create_ui(void)
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    default_wallpaper = (lv_image_dsc_t) {
        .header.magic = LV_IMAGE_HEADER_MAGIC,
        .header.cf = LV_COLOR_FORMAT_RGB565,
        .header.w = WALLPAPER_WIDTH,
        .header.h = WALLPAPER_HEIGHT,
        .header.stride = WALLPAPER_STRIDE,
        .data_size = WALLPAPER_BYTES,
        .data = default_wallpaper_start,
    };
    for (int i = 0; i < 2; ++i) {
        downloaded_wallpaper[i] = (lv_image_dsc_t) {
            .header.magic = LV_IMAGE_HEADER_MAGIC,
            .header.cf = LV_COLOR_FORMAT_RGB565,
            .header.w = WALLPAPER_WIDTH,
            .header.h = WALLPAPER_HEIGHT,
            .header.stride = WALLPAPER_STRIDE,
            .data_size = WALLPAPER_BYTES,
            .data = download_buffers[i],
        };
    }
    avatar_image_descriptor = (lv_image_dsc_t) {
        .header.magic = LV_IMAGE_HEADER_MAGIC,
        .header.cf = LV_COLOR_FORMAT_RGB565A8,
        .header.w = AVATAR_WIDTH,
        .header.h = AVATAR_HEIGHT,
        .header.stride = AVATAR_WIDTH * 2,
        .data_size = AVATAR_BYTES + AVATAR_WIDTH * AVATAR_HEIGHT,
        .data = avatar_buffer,
    };

    clock_view = lv_obj_create(screen);
    lv_obj_set_size(clock_view, WALLPAPER_WIDTH, WALLPAPER_HEIGHT);
    lv_obj_center(clock_view);
    lv_obj_set_style_bg_opa(clock_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(clock_view, 0, 0);
    lv_obj_set_style_radius(clock_view, 0, 0);
    lv_obj_set_style_pad_all(clock_view, 0, 0);
    lv_obj_remove_flag(clock_view, LV_OBJ_FLAG_SCROLLABLE);

    wallpaper_image = lv_image_create(clock_view);
    lv_image_set_src(wallpaper_image, &default_wallpaper);
    lv_obj_center(wallpaper_image);

    create_ambient_animation(clock_view);

    lv_obj_t *panel = lv_obj_create(clock_view);
    lv_obj_set_size(panel, 318, 172);
    lv_obj_center(panel);
    lv_obj_set_style_radius(panel, 72, 0);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x020711), 0);
    lv_obj_set_style_bg_opa(panel, LV_OPA_50, 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x5EDCFF), 0);
    lv_obj_set_style_border_opa(panel, LV_OPA_30, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

    time_label = lv_label_create(panel);
    lv_obj_set_style_text_font(time_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(time_label, lv_color_white(), 0);
    lv_obj_align(time_label, LV_ALIGN_CENTER, -25, -19);

    seconds_label = lv_label_create(panel);
    lv_obj_set_style_text_font(seconds_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(seconds_label, lv_color_hex(0x72E7FF), 0);
    lv_obj_align(seconds_label, LV_ALIGN_CENTER, 101, -8);

    date_label = lv_label_create(panel);
    lv_obj_set_style_text_font(date_label,
                               localized_font(&lv_font_montserrat_16), 0);
    lv_obj_set_style_text_color(date_label, lv_color_hex(0xD9E9FF), 0);
    lv_obj_align(date_label, LV_ALIGN_CENTER, 0, 36);

    create_gesture_layer(clock_view);

    lv_obj_t *home_button = lv_button_create(clock_view);
    lv_obj_set_size(home_button, 48, 48);
    lv_obj_align(home_button, LV_ALIGN_BOTTOM_MID, 0, -24);
    lv_obj_set_style_radius(home_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(home_button, lv_color_hex(0x071524), 0);
    lv_obj_set_style_bg_opa(home_button, LV_OPA_70, 0);
    lv_obj_set_style_border_width(home_button, 1, 0);
    lv_obj_set_style_border_color(home_button, lv_color_hex(0x72E7FF), 0);
    lv_obj_add_event_cb(home_button, clock_home_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *home_icon = lv_label_create(home_button);
    lv_label_set_text(home_icon, LV_SYMBOL_HOME);
    lv_obj_set_style_text_color(home_icon, lv_color_hex(0xD9F7FF), 0);
    lv_obj_center(home_icon);

    create_weather(screen);
    create_desktop(screen);
    create_settings(screen);
    create_wifi(screen);
    create_remote(screen);
    create_music(screen);
    create_radio(screen);
    create_wallpaper_studio(screen);
    create_bluetooth(screen);
    create_avatar(screen);
    create_quota(screen);
    create_codex_reset(screen);
    create_app_overview(screen);
    companion_apps_create(screen, &round_clock_cn_20, &round_clock_cn_28, companion_home);
    star_game_create(screen, &round_clock_cn_20, &round_clock_cn_28, companion_home);
    english_app_create(screen, &round_clock_cn_20, &round_clock_cn_28, companion_home);
    kids_apps_create(screen, &round_clock_cn_20, &round_clock_cn_28, companion_home);
    story_app_create(screen, &round_clock_cn_20, &round_clock_cn_28, companion_home);
    library_app_create(screen, &round_clock_cn_20, &round_clock_cn_28, companion_home, open_library_story);
    notification_center_create(screen, &round_clock_cn_20, &round_clock_cn_16, &round_clock_cn_28, notification_open_app);
    apply_apple_app_style(desktop_view);
    apply_apple_app_style(weather_view);
    apply_apple_app_style(settings_view);
    apply_apple_app_style(wifi_view);
    apply_apple_app_style(remote_view);
    apply_apple_app_style(music_view);
    apply_apple_app_style(radio_view);
    apply_apple_app_style(wallpaper_studio_view);
    apply_apple_app_style(bluetooth_view);
    apply_apple_app_style(avatar_view);
    apply_apple_app_style(quota_view);
    apply_apple_app_style(codex_reset_view);
    apply_apple_app_style(app_overview_view);
    apply_home_icon_tints(app_overview_grid);
    show_view_locked(APP_VIEW_CLOCK);

    lv_indev_t *touch = bsp_display_get_input_dev();
    if (touch != NULL) {
        lv_indev_set_long_press_time(touch, 900);
        lv_indev_set_scroll_limit(touch, 24);
    }

    lv_timer_create(clock_timer_cb, 1000, NULL);
    lv_display_add_event_cb(lv_display_get_default(), render_metrics, LV_EVENT_ALL, NULL);
    lv_timer_create(voice_wave_timer_cb, 120, NULL);
    clock_timer_cb(NULL);
}

static esp_err_t start_wifi(void)
{
    wifi_events = xEventGroupCreate();
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "esp_netif_init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init), TAG, "esp_wifi_init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_FLASH), TAG, "Wi-Fi storage failed");
    wifi_config_t config = {0};
    ESP_RETURN_ON_ERROR(esp_wifi_get_config(WIFI_IF_STA, &config), TAG, "Wi-Fi config read failed");
    if (config.sta.ssid[0] == '\0') {
        strlcpy((char *)config.sta.ssid, CLOCK_WIFI_SSID, sizeof(config.sta.ssid));
        strlcpy((char *)config.sta.password, CLOCK_WIFI_PASSWORD, sizeof(config.sta.password));
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    }

    return wifi_setup_init(wifi_events, &config);
}

static void start_network_time(void)
{
    setenv("TZ", CLOCK_TIMEZONE, 1);
    tzset();
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "ntp.aliyun.com");
    esp_sntp_setservername(1, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_cb);
    esp_sntp_init();
}

static bool read_cached_wallpaper(uint8_t *target)
{
    FILE *file = fopen(WALLPAPER_PATH, "rb");
    if (file == NULL) {
        return false;
    }
    size_t bytes = fread(target, 1, WALLPAPER_BYTES, file);
    int extra = fgetc(file);
    fclose(file);
    return bytes == WALLPAPER_BYTES && extra == EOF;
}

static bool persist_wallpaper(const uint8_t *pixels)
{
    FILE *file = fopen(WALLPAPER_TEMP_PATH, "wb");
    if (file == NULL) {
        ESP_LOGE(TAG, "Cannot open temporary wallpaper: errno=%d", errno);
        return false;
    }
    bool ok = fwrite(pixels, 1, WALLPAPER_BYTES, file) == WALLPAPER_BYTES;
    ok = ok && fflush(file) == 0 && fsync(fileno(file)) == 0;
    ok = fclose(file) == 0 && ok;
    if (!ok) {
        unlink(WALLPAPER_TEMP_PATH);
        return false;
    }
    unlink(WALLPAPER_PATH);
    if (rename(WALLPAPER_TEMP_PATH, WALLPAPER_PATH) != 0) {
        ESP_LOGE(TAG, "Cannot activate wallpaper: errno=%d", errno);
        return false;
    }
    return true;
}

static esp_err_t download_wallpaper(uint8_t *target)
{
    char url[384];
    time_t now;
    struct tm local;
    char date[16] = "current";
    time(&now);
    if (time_is_valid(now) && localtime_r(&now, &local) != NULL) {
        strftime(date, sizeof(date), "%Y-%m-%d", &local);
    }
    const char separator = strchr(CLOCK_WALLPAPER_URL, '?') == NULL ? '?' : '&';
    int written = snprintf(url, sizeof(url),
                           "%s%cwidth=%d&height=%d&format=rgb565le&date=%s",
                           CLOCK_WALLPAPER_URL, separator, WALLPAPER_WIDTH,
                           WALLPAPER_HEIGHT, date);
    if (written < 0 || written >= (int)sizeof(url)) {
        return ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_config_t config = {
        .url = url,
        .timeout_ms = 20000,
        .buffer_size = 4096,
        .user_agent = "Waveshare-Round-Clock/1.0",
        .cert_pem = (const char *)device_ca_start,
        .client_cert_pem = (const char *)device_client_cert_start,
        .client_key_pem = (const char *)device_client_key_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (CLOCK_WALLPAPER_TOKEN[0] != '\0') {
        char auth[192];
        snprintf(auth, sizeof(auth), "Bearer %s", CLOCK_WALLPAPER_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || (content_length >= 0 && content_length != WALLPAPER_BYTES)) {
        ESP_LOGE(TAG, "Wallpaper HTTP status=%d length=%lld", status, content_length);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }

    size_t total = 0;
    while (total < WALLPAPER_BYTES) {
        int count = esp_http_client_read(client, (char *)target + total,
                                         WALLPAPER_BYTES - total);
        if (count <= 0) {
            break;
        }
        total += count;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return total == WALLPAPER_BYTES ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static int load_last_update_day(void)
{
    nvs_handle_t handle;
    int32_t day = 0;
    if (nvs_open("clock", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_i32(handle, "wallpaper_day", &day);
        nvs_close(handle);
    }
    return day;
}

static void save_last_update_day(int day)
{
    nvs_handle_t handle;
    if (nvs_open("clock", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_i32(handle, "wallpaper_day", day);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

static void wallpaper_task(void *arg)
{
    (void)arg;
    int last_update_day = load_last_update_day();

    if (read_cached_wallpaper(download_buffers[active_download_buffer])) {
        have_downloaded_wallpaper = true;
        show_wallpaper(true);
        wallpaper_input_mark_cached_available();
        set_status("CACHED WALLPAPER");
        ESP_LOGI(TAG, "Cached wallpaper loaded (%d bytes)", WALLPAPER_BYTES);
    }

    for (;;) {
        int today = current_day_key();
        bool needs_update = force_update_requested || (today != 0 && today != last_update_day);
        if (needs_update && wifi_events != NULL &&
            (xEventGroupGetBits(wifi_events) & WIFI_READY_BIT) != 0) {
            force_update_requested = false;
            set_status("DOWNLOADING TODAY'S WALLPAPER");
            int next_buffer = have_downloaded_wallpaper ? 1 - active_download_buffer : active_download_buffer;
            esp_err_t download_result = download_wallpaper(download_buffers[next_buffer]);
            if (download_result == ESP_OK && persist_wallpaper(download_buffers[next_buffer])) {
                active_download_buffer = next_buffer;
                have_downloaded_wallpaper = true;
                show_wallpaper(true);
                wallpaper_input_mark_download_result(true);
                if (today != 0) {
                    last_update_day = today;
                    save_last_update_day(today);
                }
                set_status("TODAY'S WALLPAPER");
                ESP_LOGI(TAG, "Today's wallpaper downloaded and cached (%d bytes)", WALLPAPER_BYTES);
            } else {
                bool retry_voice_wallpaper = wallpaper_input_has_pending_update();
                wallpaper_input_mark_download_result(false);
                if (retry_voice_wallpaper) {
                    force_update_requested = true;
                }
                set_status("UPDATE FAILED - USING OLD");
                ESP_LOGE(TAG, "Wallpaper update failed: %s", esp_err_to_name(download_result));
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

static bool json_number(cJSON *object, const char *name, double *value)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, name);
    if (!cJSON_IsNumber(item)) {
        return false;
    }
    *value = item->valuedouble;
    return true;
}

static esp_err_t download_weather(weather_data_t *weather)
{
    char payload[WEATHER_RESPONSE_BYTES];
    esp_http_client_config_t config = {
        .url = CLOCK_WEATHER_URL,
        .timeout_ms = 12000,
        .buffer_size = 1024,
        .user_agent = "Waveshare-Round-Clock/1.0",
        .cert_pem = (const char *)device_ca_start,
        .client_cert_pem = (const char *)device_client_cert_start,
        .client_key_pem = (const char *)device_client_key_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (CLOCK_WALLPAPER_TOKEN[0] != '\0') {
        char auth[192];
        snprintf(auth, sizeof(auth), "Bearer %s", CLOCK_WALLPAPER_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) {
        esp_http_client_cleanup(client);
        return result;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_length >= (int64_t)sizeof(payload)) {
        ESP_LOGE(TAG, "Weather HTTP status=%d length=%lld", status, content_length);
        result = ESP_FAIL;
        goto cleanup;
    }

    int count = esp_http_client_read_response(client, payload, sizeof(payload) - 1);
    if (count <= 0) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    payload[count] = '\0';

    cJSON *root = cJSON_Parse(payload);
    if (root == NULL) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    cJSON *city = cJSON_GetObjectItemCaseSensitive(root, "city");
    cJSON *current = cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON *today = cJSON_GetObjectItemCaseSensitive(root, "today");
    double temperature;
    double apparent;
    double humidity;
    double wind;
    double code;
    double high;
    double low;
    bool valid = cJSON_IsString(city) && cJSON_IsObject(current) && cJSON_IsObject(today) &&
                 json_number(current, "temperature", &temperature) &&
                 json_number(current, "apparent", &apparent) &&
                 json_number(current, "humidity", &humidity) &&
                 json_number(current, "wind", &wind) &&
                 json_number(current, "code", &code) &&
                 json_number(today, "max", &high) &&
                 json_number(today, "min", &low);
    if (valid) {
        memset(weather, 0, sizeof(*weather));
        strlcpy(weather->city, city->valuestring, sizeof(weather->city));
        weather->temperature = (float)temperature;
        weather->apparent = (float)apparent;
        weather->humidity = (int)humidity;
        weather->wind = (float)wind;
        weather->code = (int)code;
        weather->high = (float)high;
        weather->low = (float)low;
        result = ESP_OK;
    } else {
        result = ESP_ERR_INVALID_RESPONSE;
    }
    cJSON_Delete(root);

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return result;
}

static void weather_task(void *arg)
{
    (void)arg;
    TickType_t next_attempt = 0;

    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool due = weather_refresh_requested || next_attempt == 0 ||
                   (int32_t)(now - next_attempt) >= 0;
        bool online = wifi_events != NULL &&
                      (xEventGroupGetBits(wifi_events) & WIFI_READY_BIT) != 0;
        if (due && online) {
            weather_refresh_requested = false;
            weather_data_t weather;
            esp_err_t result = download_weather(&weather);
            if (result == ESP_OK) {
                taskENTER_CRITICAL(&weather_data_mux);
                pending_weather_data = weather;
                weather_data_pending = true;
                taskEXIT_CRITICAL(&weather_data_mux);
                next_attempt = now + pdMS_TO_TICKS(WEATHER_REFRESH_MS);
                ESP_LOGI(TAG, "Weather updated: %s %.1f C", weather.city,
                         weather.temperature);
            } else {
                weather_error_pending = true;
                next_attempt = now + pdMS_TO_TICKS(WEATHER_RETRY_MS);
                ESP_LOGE(TAG, "Weather update failed: %s", esp_err_to_name(result));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t download_quota(quota_data_t *quota)
{
    char *payload = heap_caps_malloc(QUOTA_RESPONSE_BYTES + 1,
                                     MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (payload == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t config = {
        .url = CLOCK_QUOTA_URL,
        .timeout_ms = 15000,
        .buffer_size = 2048,
        .user_agent = "Waveshare-Round-Clock/1.0",
        .cert_pem = (const char *)device_ca_start,
        .client_cert_pem = (const char *)device_client_cert_start,
        .client_key_pem = (const char *)device_client_key_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        heap_caps_free(payload);
        return ESP_ERR_NO_MEM;
    }
    if (CLOCK_WALLPAPER_TOKEN[0] != '\0') {
        char auth[192];
        snprintf(auth, sizeof(auth), "Bearer %s", CLOCK_WALLPAPER_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) {
        goto cleanup;
    }
    int64_t content_length = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200 || content_length <= 0 ||
        content_length > QUOTA_RESPONSE_BYTES) {
        ESP_LOGE(TAG, "Quota HTTP status=%d length=%lld", status, content_length);
        result = ESP_FAIL;
        goto cleanup;
    }
    int count = esp_http_client_read_response(client, payload, QUOTA_RESPONSE_BYTES);
    if (count <= 0 || (content_length >= 0 && count != content_length)) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    payload[count] = '\0';

    cJSON *root = cJSON_ParseWithLength(payload, count);
    if (root == NULL) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    cJSON *generated_at = cJSON_GetObjectItemCaseSensitive(root, "generatedAt");
    cJSON *reports = cJSON_GetObjectItemCaseSensitive(root, "reports");
    if (!cJSON_IsString(generated_at) || !cJSON_IsArray(reports)) {
        cJSON_Delete(root);
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }

    memset(quota, 0, sizeof(*quota));
    strlcpy(quota->generated_at, generated_at->valuestring,
            sizeof(quota->generated_at));
    cJSON *report;
    cJSON_ArrayForEach(report, reports) {
        cJSON *detected = cJSON_GetObjectItemCaseSensitive(report, "detected");
        cJSON *name = cJSON_GetObjectItemCaseSensitive(report, "agentName");
        cJSON *tokens = cJSON_GetObjectItemCaseSensitive(report, "last30dBillableTokens");
        if (!cJSON_IsTrue(detected) || !cJSON_IsString(name) ||
            !cJSON_IsNumber(tokens) || tokens->valuedouble < 0) {
            continue;
        }
        uint64_t token_count = (uint64_t)tokens->valuedouble;
        quota->total_tokens += token_count;
        if (quota->agent_count >= QUOTA_MAX_AGENTS) {
            continue;
        }

        quota_agent_t *agent = &quota->agents[quota->agent_count++];
        strlcpy(agent->name, name->valuestring, sizeof(agent->name));
        agent->tokens = token_count;
        strlcpy(agent->quota_detail, use_chinese ? "额度暂无数据" : "QUOTA UNAVAILABLE", sizeof(agent->quota_detail));
        double tightest_fraction = -1.0;
        cJSON *statuses = cJSON_GetObjectItemCaseSensitive(report, "statuses");
        cJSON *quota_status;
        cJSON_ArrayForEach(quota_status, statuses) {
            cJSON *advisory = cJSON_GetObjectItemCaseSensitive(quota_status, "advisory");
            cJSON *fraction = cJSON_GetObjectItemCaseSensitive(quota_status, "usedFraction");
            cJSON *source = cJSON_GetObjectItemCaseSensitive(quota_status, "sourceKind");
            if (cJSON_IsTrue(advisory) || !cJSON_IsNumber(fraction) ||
                (cJSON_IsString(source) && strcmp(source->valuestring, "unknown") == 0)) {
                continue;
            }
            if (fraction->valuedouble > tightest_fraction) {
                tightest_fraction = fraction->valuedouble;
                cJSON *window = cJSON_GetObjectItemCaseSensitive(quota_status, "label");
                bool estimate = cJSON_IsString(source) && strcmp(source->valuestring, "localEstimate") == 0;
                snprintf(agent->quota_detail, sizeof(agent->quota_detail),
                         use_chinese ? "%s 已用%.1f%% %s" : "%s USED %.1f%% %s",
                         cJSON_IsString(window) ? window->valuestring : "",
                         tightest_fraction * 100.0,
                         estimate ? (use_chinese ? "估算" : "EST.") : "");
            }
        }


    }
    cJSON_Delete(root);
    result = quota->agent_count > 0 ? ESP_OK : ESP_ERR_INVALID_RESPONSE;

cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    heap_caps_free(payload);
    return result;
}

static void quota_task(void *arg)
{
    (void)arg;
    TickType_t next_attempt = 0;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool due = quota_refresh_requested || next_attempt == 0 ||
                   (int32_t)(now - next_attempt) >= 0;
        bool online = wifi_events != NULL &&
                      (xEventGroupGetBits(wifi_events) & WIFI_READY_BIT) != 0;
        if (due && online) {
            quota_refresh_requested = false;
            quota_data_t quota;
            esp_err_t result = download_quota(&quota);
            if (result == ESP_OK) {
                taskENTER_CRITICAL(&quota_data_mux);
                pending_quota_data = quota;
                quota_data_pending = true;
                taskEXIT_CRITICAL(&quota_data_mux);
                next_attempt = now + pdMS_TO_TICKS(QUOTA_REFRESH_MS);
                ESP_LOGI(TAG, "LLMQuota snapshot updated: %u agents, %llu tokens",
                         quota.agent_count, (unsigned long long)quota.total_tokens);
            } else {
                quota_error_pending = true;
                next_attempt = now + pdMS_TO_TICKS(QUOTA_RETRY_MS);
                ESP_LOGE(TAG, "LLMQuota update failed: %s", esp_err_to_name(result));
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static esp_err_t download_codex_reset(codex_reset_data_t *data)
{
    char *payload = heap_caps_malloc(CODEX_RESET_RESPONSE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!payload) return ESP_ERR_NO_MEM;
    esp_http_client_config_t config = {
        .url = CLOCK_CODEX_RESET_URL,
        .timeout_ms = 12000,
        .buffer_size = 1024,
        .user_agent = "Waveshare-Round-Clock/1.0",
        .cert_pem = (const char *)device_ca_start,
        .client_cert_pem = (const char *)device_client_cert_start,
        .client_key_pem = (const char *)device_client_key_start,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) { free(payload); return ESP_ERR_NO_MEM; }
    if (CLOCK_WALLPAPER_TOKEN[0] != '\0') {
        char auth[192];
        snprintf(auth, sizeof(auth), "Bearer %s", CLOCK_WALLPAPER_TOKEN);
        esp_http_client_set_header(client, "Authorization", auth);
    }
    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) goto cleanup;
    int64_t content_length = esp_http_client_fetch_headers(client);
    if (esp_http_client_get_status_code(client) != 200 ||
        content_length >= CODEX_RESET_RESPONSE_BYTES) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    int count = esp_http_client_read_response(client, payload, CODEX_RESET_RESPONSE_BYTES - 1);
    if (count <= 0 || !esp_http_client_is_complete_data_received(client)) {
        result = ESP_ERR_INVALID_RESPONSE;
        goto cleanup;
    }
    payload[count] = '\0';
    cJSON *root = cJSON_Parse(payload);
    if (!root) { result = ESP_ERR_INVALID_RESPONSE; goto cleanup; }
    memset(data, 0, sizeof(*data));
    result = ESP_ERR_INVALID_RESPONSE;
    cJSON *version = cJSON_GetObjectItemCaseSensitive(root, "schemaVersion");
    if (!cJSON_IsNumber(version) || version->valueint != 2) goto parsed;
    struct { const char *key; char *target; size_t capacity; } fields[] = {
        {"signalId", data->signal_id, sizeof(data->signal_id)},
        {"eventLabel", data->event_label, sizeof(data->event_label)},
        {"eventTime", data->event_time, sizeof(data->event_time)},
        {"eventAge", data->event_age, sizeof(data->event_age)},
        {"forecastMeta", data->forecast_meta, sizeof(data->forecast_meta)},
        {"details", data->details, sizeof(data->details)},
        {"updateLabel", data->update_label, sizeof(data->update_label)},
    };
    for (size_t i = 0; i < sizeof(fields)/sizeof(fields[0]); ++i) {
        cJSON *item = cJSON_GetObjectItemCaseSensitive(root, fields[i].key);
        if (!cJSON_IsString(item) || strlen(item->valuestring) >= fields[i].capacity) goto parsed;
        strlcpy(fields[i].target, item->valuestring, fields[i].capacity);
    }
    cJSON *active = cJSON_GetObjectItemCaseSensitive(root, "alertEligible");
    cJSON *stale = cJSON_GetObjectItemCaseSensitive(root, "stale");
    cJSON *available = cJSON_GetObjectItemCaseSensitive(root, "forecastAvailable");
    cJSON *remember = cJSON_GetObjectItemCaseSensitive(root, "rememberSignal");
    cJSON *event_epoch = cJSON_GetObjectItemCaseSensitive(root, "eventEpoch");
    if (cJSON_IsNumber(event_epoch) && event_epoch->valuedouble > 0 &&
        event_epoch->valuedouble <= 4102444800.0) data->event_epoch = (time_t)event_epoch->valuedouble;
    if (!cJSON_IsBool(active) || !cJSON_IsBool(stale) || !cJSON_IsBool(available) || !cJSON_IsBool(remember)) goto parsed;
    data->remember_signal = cJSON_IsTrue(remember);
    data->active = cJSON_IsTrue(active);
    data->stale = cJSON_IsTrue(stale);
    data->forecast_available = cJSON_IsTrue(available);
    if (data->forecast_available) {
        cJSON *p24 = cJSON_GetObjectItemCaseSensitive(root, "probability24");
        cJSON *p48 = cJSON_GetObjectItemCaseSensitive(root, "probability48");
        cJSON *expires = cJSON_GetObjectItemCaseSensitive(root, "forecastExpiresAt");
        if (!cJSON_IsNumber(p24) || !cJSON_IsNumber(p48) || !cJSON_IsNumber(expires) ||
            p24->valuedouble < 0 || p48->valuedouble > 100 ||
            p24->valuedouble > p48->valuedouble ||
            expires->valuedouble < 1700000000 || expires->valuedouble > 4102444800.0) goto parsed;
        data->probability24 = p24->valueint;
        data->probability48 = p48->valueint;
        data->forecast_expires = (time_t)expires->valuedouble;
    }
    result = ESP_OK;
parsed:
    cJSON_Delete(root);
cleanup:
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    free(payload);
    return result;
}

static void codex_reset_task(void *arg)
{
    (void)arg;
    TickType_t next_attempt = 0;
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        bool due = codex_reset_refresh_requested || next_attempt == 0 ||
                   (int32_t)(now - next_attempt) >= 0;
        bool online = wifi_events != NULL &&
                      (xEventGroupGetBits(wifi_events) & WIFI_READY_BIT) != 0;
        if (due && online) {
            codex_reset_refresh_requested = false;
            codex_reset_data_t *data = heap_caps_malloc(sizeof(*data), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            esp_err_t result = data ? download_codex_reset(data) : ESP_ERR_NO_MEM;
            if (result == ESP_OK) {
                taskENTER_CRITICAL(&codex_reset_data_mux);
                pending_codex_reset_data = *data;
                codex_reset_data_pending = true;
                taskEXIT_CRITICAL(&codex_reset_data_mux);
                next_attempt = now + pdMS_TO_TICKS(data->forecast_available && time_is_valid(time(NULL)) ? CODEX_RESET_REFRESH_MS : CODEX_RESET_RETRY_MS);
                ESP_LOGI(TAG, "Codex reset feed updated: %s", data->signal_id);
            } else {
                codex_reset_error_pending = true;
                next_attempt = now + pdMS_TO_TICKS(CODEX_RESET_RETRY_MS);
                ESP_LOGE(TAG, "Codex reset update failed: %s", esp_err_to_name(result));
            }
            free(data);
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void home_button_task(void *arg)
{
    (void)arg;
    bool stable_pressed = false;
    bool long_press_sent = false;
    uint8_t changed_samples = 0;
    TickType_t pressed_at = 0;

    for (;;) {
        int sampled_boot_level = gpio_get_level(HOME_BUTTON_GPIO);
        bool sampled_pressed = sampled_boot_level == 0;
        if (sampled_pressed != stable_pressed) {
            if (++changed_samples >= 3) {
                stable_pressed = sampled_pressed;
                changed_samples = 0;
                if (stable_pressed) {
                    pressed_at = xTaskGetTickCount();
                    long_press_sent = false;
                } else if (!long_press_sent) {
                    bool show_clock = current_app_view != APP_VIEW_CLOCK;
                    show_view(show_clock ? APP_VIEW_CLOCK : APP_VIEW_DESKTOP);
                    ESP_LOGI(TAG, "BOOT short press: request %s",
                             show_clock ? "clock" : "desktop");
                }
            }
        } else {
            changed_samples = 0;
        }

        if (stable_pressed && !long_press_sent &&
            pdTICKS_TO_MS(xTaskGetTickCount() - pressed_at) >= HOME_BUTTON_LONG_PRESS_MS) {
            force_update_requested = true;
            long_press_sent = true;
            ESP_LOGI(TAG, "BOOT long press: wallpaper update requested");
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static esp_err_t start_home_button(void)
{
    gpio_config_t config = {
        .pin_bit_mask = 1ULL << HOME_BUTTON_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&config), TAG, "BOOT button setup failed");
    return xTaskCreate(home_button_task, "home_button", 3072, NULL, 4, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t axp2101_read(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(axp2101, &reg, 1, value, 1, 100);
}

static esp_err_t axp2101_write(uint8_t reg, uint8_t value)
{
    uint8_t payload[2] = {reg, value};
    return i2c_master_transmit(axp2101, payload, sizeof(payload), 100);
}

static void power_button_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint8_t status = 0;
        if (axp2101_read(AXP2101_INTSTS2, &status) == ESP_OK && status != 0) {
            axp2101_write(AXP2101_INTSTS2, status);
            if ((status & AXP2101_PKEY_LONG_MASK) != 0) {
                requested_screen_power = 0;
                ESP_LOGI(TAG, "PWR long press: request screen off");
            } else if ((status & AXP2101_PKEY_SHORT_MASK) != 0) {
                if (!screen_on) {
                    requested_screen_power = 1;
                    ESP_LOGI(TAG, "PWR short press: request screen on");
                    vTaskDelay(pdMS_TO_TICKS(50));
                    continue;
                }
                bool show_clock = current_app_view != APP_VIEW_CLOCK;
                show_view(show_clock ? APP_VIEW_CLOCK : APP_VIEW_DESKTOP);
                ESP_LOGI(TAG, "PWR short press: request %s",
                         show_clock ? "clock" : "desktop");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static esp_err_t start_power_button(void)
{
    i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_ADDRESS,
        .scl_speed_hz = 400000,
    };
    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(bsp_i2c_get_handle(), &device_config,
                                                   &axp2101),
                        TAG, "AXP2101 setup failed");

    uint8_t enabled = 0;
    ESP_RETURN_ON_ERROR(axp2101_read(AXP2101_INTEN2, &enabled), TAG,
                        "AXP2101 interrupt read failed");
    const uint8_t key_masks = AXP2101_PKEY_SHORT_MASK | AXP2101_PKEY_LONG_MASK;
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_INTEN2, enabled | key_masks),
                        TAG, "AXP2101 interrupt setup failed");
    ESP_LOGI(TAG, "AXP2101 INTEN2: 0x%02X -> 0x%02X",
             enabled, enabled | key_masks);
    ESP_RETURN_ON_ERROR(axp2101_write(AXP2101_INTSTS2, key_masks),
                        TAG, "AXP2101 interrupt clear failed");
    return xTaskCreate(power_button_task, "power_button", 3072, NULL, 4, NULL) == pdPASS
               ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_main(void)
{
    ESP_ERROR_CHECK(nvs_flash_init());
    esp_err_t content_result=content_assets_init();
    ESP_LOGI(TAG,"Content partition: %s",esp_err_to_name(content_result));
    load_system_settings();
    music_input_set_volume(audio_volume);

    esp_vfs_spiffs_conf_t spiffs = {
        .base_path = "/wallpaper",
        .partition_label = "wallpaper",
        .max_files = 4,
        .format_if_mount_failed = false, /* Never erase user artwork on a mount failure. */
    };
    ESP_ERROR_CHECK(esp_vfs_spiffs_register(&spiffs));
    ESP_ERROR_CHECK(avatar_store_init());

    for (int i = 0; i < 2; ++i) {
        download_buffers[i] = heap_caps_malloc(WALLPAPER_BYTES,
                                               MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        ESP_ERROR_CHECK(download_buffers[i] == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    }
    avatar_buffer = heap_caps_malloc(AVATAR_BYTES+AVATAR_WIDTH*AVATAR_HEIGHT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(avatar_buffer == NULL ? ESP_ERR_NO_MEM : ESP_OK);
    for(int y=0;y<AVATAR_HEIGHT;y++) for(int x=0;x<AVATAR_WIDTH;x++) {
        int dx=2*x-279,dy=2*y-279;
        avatar_buffer[AVATAR_BYTES+y*AVATAR_WIDTH+x]=(dx*dx+dy*dy<=272*272)?255:0;
    }
    avatar_original=heap_caps_malloc(AVATAR_BYTES,MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(avatar_original==NULL?ESP_ERR_NO_MEM:ESP_OK);
    ESP_ERROR_CHECK((size_t)(default_wallpaper_end - default_wallpaper_start) == WALLPAPER_BYTES
                        ? ESP_OK : ESP_ERR_INVALID_SIZE);

    bsp_display_start();
    bsp_display_lock(-1);
    create_ui();
    lv_refr_now(NULL);
    bsp_display_backlight_on();
    bsp_display_brightness_set(display_brightness);
    bsp_display_unlock();

#if CONFIG_ROUND_CLOCK_USB_TEST_BRIDGE
    ESP_ERROR_CHECK(screenshot_bridge_start());
#else
    ESP_LOGI(TAG, "USB test bridge disabled in release build");
#endif
    ESP_ERROR_CHECK(start_home_button());
    ESP_ERROR_CHECK(start_power_button());
    esp_err_t wifi_result = start_wifi();
    if (wifi_result == ESP_OK) {
        start_network_time();
    } else {
        ESP_LOGW(TAG, "Wi-Fi not started: %s", esp_err_to_name(wifi_result));
    }
    xTaskCreateWithCaps(wallpaper_task, "wallpaper", 8192, NULL, 4, &wallpaper_refresh_task_handle,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    xTaskCreateWithCaps(weather_task, "weather", 6144, NULL, 4, NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    xTaskCreateWithCaps(quota_task, "quota", 8192, NULL, 4, NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    xTaskCreateWithCaps(codex_reset_task, "codex_reset", 6144, NULL, 4, NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    ESP_ERROR_CHECK(companion_apps_start());
}
