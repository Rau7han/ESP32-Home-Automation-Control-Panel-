/**
 * VIEWE UEDX80480070 7.0-inch ESP32-S3 Touch Display
 * Resolution: 800 x 480, IPS RGB interface / Touch: GT911 (I2C) / MCU: ESP32-S3
 *
 * Smart Home Panel UI (LVGL v8), Phase 1 display firmware.
 * Relay GPIO and DHT22 acquisition run on the separate relay-node firmware.
 */

#include <Arduino.h>
#include <Preferences.h>
#include <math.h>
#include <limits.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <time.h>
#include <esp_display_panel.hpp>
#include <lvgl.h>
#include "lvgl_v8_port.h"
#include "dashboard_icons.h"
#include "espnow_protocol.h"

#if __has_include("secrets.h")
#include "secrets.h"
#define DASHBOARD_HAS_SECRETS 1
#else
#define DASHBOARD_HAS_SECRETS 0
#define WIFI_SSID ""
#define WIFI_PASSWORD ""
#define OPENWEATHER_KEY ""
#define WEATHER_LATITUDE ""
#define WEATHER_LONGITUDE ""
#define TIMEZONE_INFO "UTC0"
#endif

using namespace esp_panel::drivers;
using namespace esp_panel::board;

/* Only the degree sign and the bullet exist outside ASCII in the built-in
   fonts. Kept as macros so an adjacent hex digit cannot extend the escape. */
#define DEG  "\xC2\xB0"
#define MDOT " \xE2\x80\xA2 "

/* Type scale. Sizes below 12 px are deliberately absent: small grey text was
   unreadable on this panel, so 12 px is the floor and captions use text2. */
#define F_CAP   &lv_font_montserrat_12
#define F_BODY  &lv_font_montserrat_14
#define F_TITLE &lv_font_montserrat_16
#define F_RELAY &lv_font_montserrat_18
#define F_PAGE  &lv_font_montserrat_20
#define F_CLOCK &lv_font_montserrat_22
#define F_HERO  &lv_font_montserrat_46

#define RELAY_COUNT  4
#define SCENE_COUNT  4
#define HISTORY_MAX  8
#define REG_MAX      420

static constexpr uint32_t TOUCH_GUARD_MS = 180;
static constexpr uint32_t RELAY_PENDING_TIMEOUT_MS = 2500;
static constexpr uint32_t RELAY_OFFLINE_TIMEOUT_MS = 15000;
static constexpr uint32_t STATUS_REQUEST_INTERVAL_MS = 5000;
static constexpr uint32_t WEATHER_INTERVAL_MS = 10UL * 60UL * 1000UL;
static constexpr uint32_t WEATHER_RETRY_MS = 60UL * 1000UL;
static constexpr uint8_t RELAY_MASK_ALL = (1U << RELAY_COUNT) - 1;

static const uint8_t RELAY_ESP32_MAC[6] = {0xC0, 0x5D, 0x89, 0xF5, 0xAD, 0xFC};

/* =========================================================
   THEME PALETTES
   ========================================================= */
struct Palette {
    uint32_t bg, chrome, card, raised, sunken, border;
    uint32_t text, text2, text3;
    uint32_t accent, accent_soft, accent_bay, accent_line, accent_hi;
    uint32_t amber, violet, green, coral;
    uint32_t chart_past, grid, knob_off, icon_off;
    uint32_t nav_on[4];
    uint32_t relay_on[RELAY_COUNT];
    uint32_t scene_fg[SCENE_COUNT];
    uint32_t scene_soft[SCENE_COUNT];
    uint32_t scene_line[SCENE_COUNT];
};

enum { THEME_SLATE = 0, THEME_AMOLED = 1, THEME_AURORA = 2, THEME_COUNT = 3 };

static const Palette palettes[THEME_COUNT] = {
    /* ---- Slate: the original graphite look, with lifted greys for contrast ---- */
    {
        0x0D1117, 0x121821, 0x18202A, 0x202A36, 0x10161F, 0x2A3644,
        0xF3F6F8, 0xA9B6C4, 0x8A97A8,
        0x4E9BFF, 0x142232, 0x1D3B60, 0x335D8A, 0x7DB6FF,
        0xFFB84D, 0x8D8BEB, 0x61C786, 0xE4676B,
        0x8A97A8, 0x263341, 0x8A97A8, 0xA9B6C4,
        {0x7DB6FF, 0x7DB6FF, 0x7DB6FF, 0x7DB6FF},
        {0x7DB6FF, 0x7DB6FF, 0x7DB6FF, 0x7DB6FF},
        {0x7DB6FF, 0x8D8BEB, 0xFFB84D, 0xE4676B},
        {0x142232, 0x212038, 0x2A2218, 0x321A1E},
        {0x335D8A, 0x4A4785, 0x5C4325, 0x6E2D33},
    },
    /* ---- AMOLED: true black chrome for the panel's deepest state ---- */
    {
        0x000000, 0x07090C, 0x0F141A, 0x171E26, 0x05070A, 0x263039,
        0xFFFFFF, 0xB2BECB, 0x94A1AF,
        0x4E9BFF, 0x0E1A28, 0x16324F, 0x2F5680, 0x8CC0FF,
        0xFFC163, 0x9B99F5, 0x6FD494, 0xF07178,
        0x94A1AF, 0x1B242C, 0x94A1AF, 0xB2BECB,
        {0x8CC0FF, 0x8CC0FF, 0x8CC0FF, 0x8CC0FF},
        {0x8CC0FF, 0x8CC0FF, 0x8CC0FF, 0x8CC0FF},
        {0x8CC0FF, 0x9B99F5, 0xFFC163, 0xF07178},
        {0x0E1A28, 0x15142A, 0x231C12, 0x2A1216},
        {0x2F5680, 0x413E77, 0x543D22, 0x63272D},
    },
    /* ---- Aurora: deep navy with cyan/violet accents and per-icon hues ---- */
    {
        0x08111F, 0x101A33, 0x18284C, 0x223862, 0x0C1830, 0x315582,
        0xF8FAFF, 0xA9B7D0, 0x8C9CBA,
        0x32D5FF, 0x123A55, 0x1B4E6E, 0x2F7FA5, 0x7FE6FF,
        0xFFBF47, 0xA277FF, 0x4ADE80, 0xFF5D73,
        0x8C9CBA, 0x1E3559, 0x8C9CBA, 0x8C9CBA,
        {0x32D5FF, 0xFFBF47, 0xA277FF, 0x4ADE80},
        {0xFFE066, 0xFF9F4A, 0x32D5FF, 0x4ADE80},
        {0x32D5FF, 0xA277FF, 0xFFBF47, 0xFF5D73},
        {0x123A55, 0x2A2050, 0x3A2E14, 0x3A1723},
        {0x2F7FA5, 0x5B45A5, 0x7A5C24, 0x8A3140},
    },
};

/* Style roles. Every themed widget is tagged with one at creation time so a
   theme switch can repaint the whole tree without rebuilding it. */
enum {
    R_NONE = 0, R_SCREEN, R_CHROME, R_CARD, R_RAISED, R_SUNKEN, R_DIVIDER,
    R_TXT, R_TXT2, R_TXT3, R_TXT_AMBER, R_TXT_ACCENT,
    R_GRID, R_ARC, R_AMBER_FILL, R_ACCENT_FILL, R_ACCENT_RING, R_PAST_FILL,
    R_ICON_AMBER, R_ICON_GREEN,
    R_SLIDER, R_SWITCH, R_SEG_GROUP, R_SEG_BTN,
};

/* =========================================================
   PERSISTED SETTINGS (NVS namespace + keys are all <= 15 chars)
   ========================================================= */
struct Settings {
    uint8_t brightness;   /* 15..100 */
    uint8_t theme;        /* 0..2 */
    uint8_t unit;         /* 0 = Celsius, 1 = Fahrenheit */
    uint16_t dim_sec;     /* 0 = off, else 30 / 60 */
    uint16_t sleep_sec;   /* 0 = never, else 120 / 300 */
    bool show_indoor;
    bool show_date;
    bool show_scenes;
};

static Settings settings = {80, THEME_SLATE, 0, 30, 120, true, true, true};
static Preferences prefs;
static volatile bool settings_dirty = false;
static uint32_t settings_dirty_at = 0;

static const uint16_t dim_values[3] = {0, 30, 60};
static const uint16_t sleep_values[3] = {120, 300, 0};

/* =========================================================
   LIVE MODELS
   ========================================================= */
struct RelayDef {
    const char *name;
    const char *sub;
    const char *log_name;
    const lv_img_dsc_t *icon;
};

static const RelayDef relay_defs[RELAY_COUNT] = {
    {"Living Room", "Ceiling Light" MDOT "Relay 1",    "Living Room",  &icon_lightbulb_24},
    {"Kitchen",     "Pendant Lamp" MDOT "Relay 2",     "Kitchen Lamp", &icon_lamp_desk_24},
    {"Main Fan",    "Circulation" MDOT "Relay 3",      "Main Fan",     &icon_fan_24},
    {"Garden",      "Irrigation Valve" MDOT "Relay 4", "Garden Valve", &icon_sprout_24},
};

struct SceneDef {
    const char *name;
    const lv_img_dsc_t *icon;
    bool states[RELAY_COUNT];
};

static const SceneDef scene_defs[SCENE_COUNT] = {
    {"Home",    &icon_house_18,   {true, true, true, false}},
    {"Night",   &icon_moon_18,    {false, false, true, true}},
    {"Away",    &icon_log_out_18, {false, false, false, true}},
    {"All Off", &icon_power_18,   {false, false, false, false}},
};

static uint8_t confirmedRelayMask = 0;
static uint8_t requestedRelayMask = 0;
static uint8_t relayPendingMask = 0;
static uint32_t nextCommandSequence = 1;
static uint32_t relayLastSeenMs = 0;
static bool relayNodeOnline = false;
static bool relayStateKnown = false;
static uint32_t relayPendingSince[RELAY_COUNT] = {};
static uint32_t relayPendingSequence[RELAY_COUNT] = {};
static uint32_t relayLastTouchMs[RELAY_COUNT] = {};
static int8_t pendingScene = -1;
static uint32_t runtime_ms[RELAY_COUNT] = {};
static uint32_t relay_since[RELAY_COUNT] = {0, 0, 0, 0};
static int active_scene = -1;   /* -1 = custom / no scene */

struct ConditionDef {
    const char *label;
    const lv_img_dsc_t *icon32;
    const lv_img_dsc_t *icon20;
};

static const ConditionDef conditions[4] = {
    {"Sunny",         &icon_sun_32,               &icon_sun_20},
    {"Partly Cloudy", &icon_cloud_sun_32,         &icon_cloud_sun_20},
    {"Cloudy",        &icon_cloud_32,             &icon_cloud_20},
    {"Light Rain",    &icon_cloud_rain_lucide_32, &icon_cloud_rain_lucide_20},
};

static const char *const wind_dirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
static const char *const wday_long[7] = {"Sunday", "Monday", "Tuesday", "Wednesday",
                                         "Thursday", "Friday", "Saturday"};
static const char *const wday_short[7] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
struct WeatherData {
    bool valid;
    bool uvValid;
    bool windDirectionValid;
    uint32_t observationUnix;
    int16_t temperatureDc;
    int16_t feelsLikeDc;
    int16_t dewPointDc;
    uint8_t humidityPercent;
    uint8_t rainPercent;
    uint8_t uvIndex;
    uint8_t cloudPercent;
    uint8_t windDirectionSector;
    uint16_t pressureHpa;
    uint16_t windKmh;
    uint16_t windGustKmh;
    uint16_t visibilityMeters;
    uint16_t rainHundredthMm;
    uint16_t weatherId;
    char description[40];
    uint32_t sunriseUnix;
    uint32_t sunsetUnix;
    uint32_t hourlyUnix[9];
    int16_t hourlyTempDc[9];
    uint8_t hourlyRainPercent[9];
    uint32_t forecastUnix[5];
    int16_t forecastHighDc[5];
    int16_t forecastLowDc[5];
    uint8_t forecastRainPercent[5];
    uint16_t forecastRainHundredthMm[5];
    uint8_t forecastUvIndex[5];
    uint16_t forecastWeatherId[5];
};

static WeatherData weatherData = {};
static WeatherData pendingWeatherData = {};
static portMUX_TYPE weatherMux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool weatherUiDirty = false;
static volatile bool weatherRequestFailed = false;
static bool weatherEverUpdated = false;
static volatile uint32_t lastWeatherSuccessMs = 0;
static volatile bool wifiConnected = false;
static bool indoorTempValid = false;
static bool indoorHumidityValid = false;
static int16_t indoorTempCentiC = 0;
static uint16_t indoorHumidityCentiPercent = 0;
static int16_t chart_dc[9] = {};
static int16_t chart_lo_dc, chart_hi_dc;

/* Fixed y positions of the two reference lines inside the 170 px plot. */
#define CHART_PLOT_Y 32
#define CHART_PLOT_H 170
#define CHART_REF_HI_Y 77
#define CHART_REF_LO_Y 145

struct Event {
    char time[6];
    char name[22];
    char detail[28];
    uint8_t kind;    /* 0 = relay off, 1 = relay on, 2 = scene */
    uint8_t scene;
};

static Event events[HISTORY_MAX];
static int event_count = 0;
struct HistoryRowWidgets {
    lv_obj_t *container;
    lv_obj_t *timeLabel;
    lv_obj_t *nameLabel;
    lv_obj_t *detailLabel;
    lv_obj_t *statePill;
    lv_obj_t *stateLabel;
};
static HistoryRowWidgets historyRows[HISTORY_MAX];
static bool history_dirty = false;

struct EspNowRxItem {
    RelaySensorStatusPacket packet;
};
static QueueHandle_t espnowRxQueue = nullptr;
static bool espNowReady = false;
static bool wifiWasConnected = false;
static bool ntpConfigured = false;
static uint32_t lastStatusRequestMs = 0;
static uint32_t lastEspNowAttemptMs = 0;
static volatile bool relayUiDirty = false;
static volatile bool indoorUiDirty = false;
static volatile bool relayTransportLost = false;
static uint8_t relayUiChangedMask = RELAY_MASK_ALL;

enum { PS_ACTIVE = 0, PS_DIM, PS_SLEEP };
static int power_state = PS_ACTIVE;
static volatile int8_t power_state_log = -1;   /* drained by loop(), see below */

/* =========================================================
   WIDGET HANDLES
   ========================================================= */
static Board *board = nullptr;
static lv_obj_t *scr_root;
static lv_obj_t *header_date;
static lv_obj_t *clock_label;
static lv_obj_t *wifi_status_label;
static lv_obj_t *weather_status_label;
static lv_obj_t *relay_status_label;
static lv_obj_t *stage;
static lv_obj_t *pages[4];
static lv_obj_t *nav_tabs[4];
static lv_obj_t *nav_icons[4];
static int active_page = -1;

static lv_obj_t *relay_cards[RELAY_COUNT];
static lv_obj_t *relay_chips[RELAY_COUNT];
static lv_obj_t *relay_icons[RELAY_COUNT];
static lv_obj_t *relay_pills[RELAY_COUNT];
static lv_obj_t *relay_pill_labels[RELAY_COUNT];
static lv_obj_t *relay_subs[RELAY_COUNT];

static lv_obj_t *scene_rail;
static lv_obj_t *scene_btns[SCENE_COUNT];
static lv_obj_t *scene_icons[SCENE_COUNT];

static lv_obj_t *hero_temp;
static lv_obj_t *hero_cond;
static lv_obj_t *hero_icon;
static lv_obj_t *hero_metrics[4];
static lv_obj_t *hero_sunset_label;
static lv_obj_t *indoor_card;
static lv_obj_t *indoor_temp_label;
static lv_obj_t *indoor_hum_label;

static lv_obj_t *wx_temp;
static lv_obj_t *wx_cond;
static lv_obj_t *wx_stats[4];
static lv_obj_t *sun_arc;
static lv_obj_t *sun_dot;
static lv_obj_t *sunrise_label;
static lv_obj_t *sunset_label;
static lv_obj_t *fc_days[5];
static lv_obj_t *fc_icons[5];
static lv_obj_t *fc_temps[5];
static lv_obj_t *chart;
static lv_chart_series_t *ser_fc;
static lv_obj_t *chart_ref_hi;
static lv_obj_t *chart_ref_lo;

static lv_obj_t *history_list;
static lv_obj_t *runtime_bars[RELAY_COUNT];
static lv_obj_t *runtime_labels[RELAY_COUNT];

static lv_obj_t *brightness_label;
static lv_obj_t *wake_overlay = nullptr;

static uint8_t lastRenderedRelayMask = 0xFF;
static uint8_t lastRenderedPendingMask = 0xFF;
static bool lastRenderedRelayKnown = true;
static int16_t lastRenderedIndoorTemp = INT16_MIN;
static uint16_t lastRenderedIndoorHumidity = UINT16_MAX;
static int lastRenderedClockMinute = -1;
static int lastRenderedDateDay = -1;
static int8_t lastRenderedWifi = -1;
static int8_t lastRenderedWeather = -1;
static int8_t lastRenderedRelayOnline = -1;
static uint32_t lastRenderedRuntimeMinutes[RELAY_COUNT] = {
    UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX
};
static lv_coord_t lastRenderedRuntimeHeight[RELAY_COUNT] = {-1, -1, -1, -1};

static lv_obj_t *reg_obj[REG_MAX];
static uint8_t reg_role[REG_MAX];
static uint16_t reg_count = 0;

/* Grid template for the 2x2 relay block; LVGL keeps the pointers. */
static lv_coord_t relay_cols[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};
static lv_coord_t relay_rows[] = {LV_GRID_FR(1), LV_GRID_FR(1), LV_GRID_TEMPLATE_LAST};

/* =========================================================
   HELPERS
   ========================================================= */
static const Palette &pal()
{
    return palettes[settings.theme < THEME_COUNT ? settings.theme : 0];
}

static lv_color_t hex(uint32_t c)
{
    return lv_color_hex(c);
}

static void printMac(const char *label, const uint8_t *mac)
{
    Serial.printf(
        "%s%02X:%02X:%02X:%02X:%02X:%02X\n",
        label,
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

static void reg(lv_obj_t *o, uint8_t role)
{
    if (role == R_NONE || o == nullptr) return;
    if (reg_count >= REG_MAX) {
        Serial.println("WARNING: theme registry full, widget will not re-theme");
        return;
    }
    reg_obj[reg_count] = o;
    reg_role[reg_count] = role;
    reg_count++;
}

static lv_obj_t *box(lv_obj_t *parent, lv_coord_t w, lv_coord_t h, uint8_t role, lv_coord_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, w, h);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_radius(o, radius, 0);
    reg(o, role);
    return o;
}

static lv_obj_t *txt(lv_obj_t *parent, const char *s, const lv_font_t *font, uint8_t role)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, font, 0);
    reg(l, role);
    return l;
}

static lv_obj_t *icon_img(lv_obj_t *parent, const lv_img_dsc_t *src, uint8_t role)
{
    lv_obj_t *img = lv_img_create(parent);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    lv_img_set_src(img, src);
    lv_obj_set_style_img_recolor_opa(img, LV_OPA_COVER, 0);
    reg(img, role);
    return img;
}

static void tint(lv_obj_t *img, uint32_t color)
{
    lv_obj_set_style_img_recolor(img, hex(color), 0);
}

static void set_flex(lv_obj_t *o, lv_flex_flow_t flow, lv_flex_align_t main,
                     lv_flex_align_t cross, lv_coord_t gap)
{
    lv_obj_set_flex_flow(o, flow);
    lv_obj_set_flex_align(o, main, cross, cross);
    lv_obj_set_style_pad_row(o, gap, 0);
    lv_obj_set_style_pad_column(o, gap, 0);
}

static void set_border(lv_obj_t *o, lv_coord_t width, lv_border_side_t side)
{
    lv_obj_set_style_border_width(o, width, 0);
    lv_obj_set_style_border_side(o, side, 0);
    lv_obj_set_style_border_opa(o, LV_OPA_COVER, 0);
}

/* Press feedback never physically moves large controls. */
static void add_press_feedback(lv_obj_t *o)
{
    lv_obj_set_style_bg_opa(o, LV_OPA_80, LV_STATE_PRESSED);
}

static void set_collapsed(lv_obj_t *o, bool collapsed)
{
    if (collapsed) lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *parent = lv_obj_get_parent(o);
    if (parent) lv_obj_mark_layout_as_dirty(parent);
}

/* =========================================================
   UNITS
   ========================================================= */
static int32_t to_disp_dc(int32_t celsius_dc)
{
    return settings.unit ? (celsius_dc * 9 / 5 + 320) : celsius_dc;
}

static int disp_whole(int celsius_dc)
{
    int32_t v = to_disp_dc(celsius_dc);
    return (int)((v + (v >= 0 ? 5 : -5)) / 10);
}

static const char *unit_letter()
{
    return settings.unit ? "F" : "C";
}

static void hhmm(uint32_t min_of_day, char *out, size_t len)
{
    if (min_of_day >= 1440) {
        snprintf(out, len, "--:--");
        return;
    }
    snprintf(out, len, "%02u:%02u", (unsigned)((min_of_day / 60) % 24), (unsigned)(min_of_day % 60));
}

static bool local_tm(struct tm &out)
{
    return getLocalTime(&out, 0);
}

static uint32_t current_minute_of_day()
{
    struct tm now;
    return local_tm(now) ? (uint32_t)(now.tm_hour * 60 + now.tm_min) : UINT32_MAX;
}

/* =========================================================
   HISTORY
   ========================================================= */
static void refresh_history_rows();

static void push_event(uint8_t kind, uint8_t scene, const char *name, const char *detail,
                       uint32_t min_of_day)
{
    if (event_count == HISTORY_MAX) event_count--;
    for (int i = event_count; i > 0; i--) events[i] = events[i - 1];

    Event &e = events[0];
    hhmm(min_of_day, e.time, sizeof(e.time));
    snprintf(e.name, sizeof(e.name), "%s", name);
    snprintf(e.detail, sizeof(e.detail), "%s", detail);
    e.kind = kind;
    e.scene = scene;
    event_count++;
}

/* =========================================================
   STATE REFRESH
   ========================================================= */
static bool confirmed_relay_on(int i)
{
    return relayStateKnown && ((confirmedRelayMask & (1U << i)) != 0);
}

static void refresh_relay_card(uint8_t i)
{
    const Palette &p = pal();
    bool on = confirmed_relay_on(i);
    bool pending = (relayPendingMask & (1U << i)) != 0;
    lv_obj_set_style_bg_color(relay_cards[i], hex(on ? p.accent_soft : p.card), 0);
        lv_obj_set_style_bg_opa(relay_cards[i], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(relay_cards[i], hex(p.accent_line), 0);
    lv_obj_set_style_border_opa(relay_cards[i], (on || pending) ? LV_OPA_COVER : LV_OPA_TRANSP, 0);

        lv_obj_set_style_bg_color(relay_chips[i], hex(on ? p.accent_bay : p.raised), 0);
        lv_obj_set_style_bg_opa(relay_chips[i], LV_OPA_COVER, 0);
    tint(relay_icons[i], on ? p.relay_on[i] : (pending ? p.accent_hi : p.icon_off));

        lv_obj_set_style_bg_color(relay_pills[i], hex(on ? p.accent : p.raised), 0);
        lv_obj_set_style_bg_opa(relay_pills[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(relay_pills[i], hex(on ? p.bg : p.text2), 0);
    lv_label_set_text(relay_pill_labels[i], pending ? "..." : (!relayStateKnown ? "--" : (on ? "ON" : "OFF")));

        lv_obj_set_style_text_color(relay_subs[i], hex(on ? p.text2 : p.text3), 0);
}

static void refresh_all_relay_cards()
{
    if (lastRenderedRelayMask == confirmedRelayMask &&
        lastRenderedPendingMask == relayPendingMask &&
        lastRenderedRelayKnown == relayStateKnown) return;
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        refresh_relay_card(i);
    }
    lastRenderedRelayMask = confirmedRelayMask;
    lastRenderedPendingMask = relayPendingMask;
    lastRenderedRelayKnown = relayStateKnown;
}

static void refresh_scene_rail()
{
    const Palette &p = pal();
    for (int i = 0; i < SCENE_COUNT; i++) {
        bool sel = (active_scene == i);
        lv_obj_set_style_bg_color(scene_btns[i], hex(p.scene_soft[i]), 0);
        lv_obj_set_style_bg_opa(scene_btns[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(scene_btns[i], hex(p.scene_line[i]), 0);
        lv_obj_set_style_border_opa(scene_btns[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(scene_btns[i], hex(sel ? p.scene_fg[i] : p.text2), 0);
        tint(scene_icons[i], sel ? p.scene_fg[i] : p.icon_off);
    }
}

static void refresh_nav()
{
    const Palette &p = pal();
    for (int i = 0; i < 4; i++) {
        bool sel = (i == active_page);
        lv_obj_set_style_bg_color(nav_tabs[i], hex(p.accent_soft), 0);
        lv_obj_set_style_bg_opa(nav_tabs[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(nav_tabs[i], hex(p.accent_line), 0);
        lv_obj_set_style_border_opa(nav_tabs[i], sel ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(nav_tabs[i], hex(sel ? p.text : p.text2), 0);
        tint(nav_icons[i], sel ? p.nav_on[i] : p.icon_off);
    }
}

static void refresh_runtime()
{
    const Palette &p = pal();
    const uint32_t bar_area = 168;
    const uint32_t full_ms = 4UL * 3600000UL;   /* 4 h fills the column */
    const uint32_t colors[RELAY_COUNT] = {p.accent, p.green, p.amber, p.violet};

    for (int i = 0; i < RELAY_COUNT; i++) {
        uint32_t ms = runtime_ms[i];
        if (confirmed_relay_on(i)) ms += millis() - relay_since[i];

        uint32_t h = (uint32_t)((uint64_t)ms * bar_area / full_ms);
        if (h < 6) h = 6;
        if (h > bar_area) h = bar_area;
        uint32_t minutes = ms / 60000UL;
        if (lastRenderedRuntimeMinutes[i] == minutes &&
            lastRenderedRuntimeHeight[i] == (lv_coord_t)h) {
            continue;
        }
        lv_obj_set_height(runtime_bars[i], (lv_coord_t)h);
        lv_obj_set_style_bg_color(runtime_bars[i], hex(colors[i]), 0);
        lv_obj_set_style_bg_opa(runtime_bars[i], LV_OPA_COVER, 0);

        lv_label_set_text_fmt(runtime_labels[i], "%u.%uh", (unsigned)(minutes / 60),
                              (unsigned)((minutes % 60) * 10 / 60));
        lastRenderedRuntimeMinutes[i] = minutes;
        lastRenderedRuntimeHeight[i] = (lv_coord_t)h;
    }
}

/* Value shown at a given pixel row of the plot, so the reference labels stay
   truthful whatever the range or unit is. */
static int chart_value_at(int y_px)
{
    return chart_hi_dc - (int)((int32_t)(y_px - CHART_PLOT_Y) * (chart_hi_dc - chart_lo_dc) / CHART_PLOT_H);
}

static void refresh_weather_chart()
{
    if (!weatherData.valid) return;
    const Palette &p = pal();
    lv_chart_set_series_color(chart, ser_fc, hex(p.accent));
    lv_chart_set_range(chart, LV_CHART_AXIS_PRIMARY_Y, to_disp_dc(chart_lo_dc), to_disp_dc(chart_hi_dc));

    for (int i = 0; i < 9; i++) {
        int32_t v = to_disp_dc(chart_dc[i]);
        lv_chart_set_value_by_id(chart, ser_fc, i, (lv_coord_t)v);
    }
    lv_chart_refresh(chart);

    lv_label_set_text_fmt(chart_ref_hi, "%d" DEG, disp_whole(chart_value_at(CHART_REF_HI_Y)));
    lv_label_set_text_fmt(chart_ref_lo, "%d" DEG, disp_whole(chart_value_at(CHART_REF_LO_Y)));
}

/* Sun position is driven by NTP time and the API sunrise/sunset timestamps. */
static int weather_condition_index(uint16_t id)
{
    if (id >= 200 && id < 600) return 3;
    if (id >= 600 && id < 700) return 2;
    if (id >= 700 && id < 800) return 2;
    if (id == 800) return 0;
    if (id == 801 || id == 802) return 1;
    return 2;
}

static void refresh_sun_widgets()
{
    if (!weatherData.valid || weatherData.sunsetUnix <= weatherData.sunriseUnix) return;
    time_t now = time(nullptr);
    int32_t span = (int32_t)(weatherData.sunsetUnix - weatherData.sunriseUnix);
    int32_t prog = (int32_t)(((int64_t)now - weatherData.sunriseUnix) * 1000 / span);
    if (prog < 0) prog = 0;
    if (prog > 1000) prog = 1000;

    uint16_t end_angle = (uint16_t)(180 + (prog * 180) / 1000);
    lv_arc_set_angles(sun_arc, 180, end_angle);
    lv_obj_clear_flag(sun_dot, LV_OBJ_FLAG_HIDDEN);

    /* Arc box is 180x180 at (16,6) inside a 212 wide holder: centre (106,96). */
    float rad = (float)end_angle * 3.14159265f / 180.0f;
    int cx = 106 + (int)(88.0f * cosf(rad));
    int cy = 96 + (int)(88.0f * sinf(rad));
    lv_obj_set_pos(sun_dot, cx - 5, cy - 5);
    char buf[8];
    time_t rise = weatherData.sunriseUnix;
    time_t set = weatherData.sunsetUnix;
    struct tm riseTm, setTm;
    localtime_r(&rise, &riseTm);
    localtime_r(&set, &setTm);
    strftime(buf, sizeof(buf), "%H:%M", &riseTm);
    lv_label_set_text(sunrise_label, buf);
    strftime(buf, sizeof(buf), "%H:%M", &setTm);
    lv_label_set_text(sunset_label, buf);
    lv_label_set_text(hero_sunset_label, buf);
}

static void refresh_current_weather()
{
    if (!weatherData.valid) return;
    const Palette &p = pal();
    int condition = weather_condition_index(weatherData.weatherId);
    const ConditionDef &c = conditions[condition];
    uint32_t wx_tint = (condition == 3) ? p.accent : p.amber;

    lv_label_set_text_fmt(hero_temp, "%d" DEG "%s", disp_whole(weatherData.temperatureDc), unit_letter());
    lv_label_set_text(hero_cond, weatherData.description[0] ? weatherData.description : c.label);
    lv_img_set_src(hero_icon, c.icon32);
    tint(hero_icon, wx_tint);

    lv_label_set_text_fmt(hero_metrics[0], "%d" DEG "%s", disp_whole(weatherData.feelsLikeDc), unit_letter());
    lv_label_set_text_fmt(hero_metrics[1], "%u%%", weatherData.humidityPercent);
    lv_label_set_text_fmt(hero_metrics[2], "%u%%", weatherData.rainPercent);
    if (weatherData.windDirectionValid) {
        lv_label_set_text_fmt(hero_metrics[3], "%u km/h %s", weatherData.windKmh,
                              wind_dirs[weatherData.windDirectionSector]);
    } else {
        lv_label_set_text_fmt(hero_metrics[3], "%u km/h", weatherData.windKmh);
    }

    lv_label_set_text_fmt(wx_temp, "%d" DEG "%s", disp_whole(weatherData.temperatureDc), unit_letter());
    if (weatherData.uvValid) {
        lv_label_set_text_fmt(wx_cond, "%s" MDOT "UV %u",
                              weatherData.description[0] ? weatherData.description : c.label,
                              weatherData.uvIndex);
    } else {
        lv_label_set_text_fmt(wx_cond, "%s" MDOT "UV --",
                              weatherData.description[0] ? weatherData.description : c.label);
    }

    lv_label_set_text_fmt(wx_stats[0], "%d" DEG "%s", disp_whole(weatherData.feelsLikeDc), unit_letter());
    lv_label_set_text_fmt(wx_stats[1], "%u%%", weatherData.humidityPercent);
    lv_label_set_text_fmt(wx_stats[2], "%u hPa", weatherData.pressureHpa);
    if (weatherData.windDirectionValid) {
        lv_label_set_text_fmt(wx_stats[3], "%u km/h %s", weatherData.windKmh,
                              wind_dirs[weatherData.windDirectionSector]);
    } else {
        lv_label_set_text_fmt(wx_stats[3], "%u km/h", weatherData.windKmh);
    }
}

static void refresh_weather_forecast()
{
    if (!weatherData.valid) return;
    const Palette &p = pal();
    struct tm todayTm = {};
    bool haveToday = local_tm(todayTm);
    for (int i = 0; i < 5; i++) {
        struct tm forecastTm = {};
        time_t forecastTime = weatherData.forecastUnix[i];
        bool haveForecastDate = forecastTime > 0 && localtime_r(&forecastTime, &forecastTm);
        int weekday = haveForecastDate ? forecastTm.tm_wday : i;
        int condition = weather_condition_index(weatherData.forecastWeatherId[i]);
        bool isToday = haveToday && haveForecastDate &&
                       todayTm.tm_year == forecastTm.tm_year &&
                       todayTm.tm_yday == forecastTm.tm_yday;
        lv_label_set_text(fc_days[i], isToday ? "TODAY" : wday_short[weekday]);
        lv_img_set_src(fc_icons[i], conditions[condition].icon20);
        tint(fc_icons[i], condition == 3 ? p.accent : p.amber);
        lv_label_set_text_fmt(fc_temps[i], "%d" DEG " / %d" DEG,
                              disp_whole(weatherData.forecastHighDc[i]),
                              disp_whole(weatherData.forecastLowDc[i]));
    }
}

static void refresh_indoor_card()
{
    int16_t temp = indoorTempValid ? indoorTempCentiC : INT16_MIN;
    uint16_t humidity = indoorHumidityValid ? indoorHumidityCentiPercent : UINT16_MAX;
    if (temp == lastRenderedIndoorTemp && humidity == lastRenderedIndoorHumidity) return;

    if (indoorTempValid) {
        int32_t deciC = (indoorTempCentiC + (indoorTempCentiC >= 0 ? 5 : -5)) / 10;
        int32_t display = to_disp_dc(deciC);
        lv_label_set_text_fmt(indoor_temp_label, "%ld.%ld" DEG "%s",
                              (long)(display / 10), (long)abs(display % 10), unit_letter());
    } else {
        lv_label_set_text(indoor_temp_label, "--");
    }
    if (indoorHumidityValid) {
        lv_label_set_text_fmt(indoor_hum_label, "%u.%02u%%",
                              indoorHumidityCentiPercent / 100,
                              indoorHumidityCentiPercent % 100);
    } else {
        lv_label_set_text(indoor_hum_label, "--");
    }
    lastRenderedIndoorTemp = temp;
    lastRenderedIndoorHumidity = humidity;
}

static void refresh_clock_from_tm(const struct tm &now)
{
    char buf[32];
    strftime(buf, sizeof(buf), "%H:%M", &now);
    lv_label_set_text(clock_label, buf);
    strftime(buf, sizeof(buf), "%A" MDOT "%b %e", &now);
    lv_label_set_text(header_date, buf);
}

static void refresh_connection_status()
{
    bool wifi = wifiConnected;
    if (lastRenderedWifi != (int8_t)wifi) {
        if (wifi) {
            lv_obj_add_flag(wifi_status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(wifi_status_label, "Wi-Fi: Offline");
            lv_obj_clear_flag(wifi_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        lastRenderedWifi = wifi;
    }
    bool weatherFresh =
        weatherEverUpdated &&
        wifiConnected &&
        !weatherRequestFailed &&
        millis() - lastWeatherSuccessMs < WEATHER_INTERVAL_MS * 2;
    if (lastRenderedWeather != (int8_t)weatherFresh) {
        if (weatherFresh) {
            lv_obj_add_flag(weather_status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(weather_status_label, "Weather: Stale");
            lv_obj_clear_flag(weather_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        lastRenderedWeather = weatherFresh;
    }
    if (lastRenderedRelayOnline != (int8_t)relayNodeOnline) {
        if (relayNodeOnline) {
            lv_obj_add_flag(relay_status_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(relay_status_label, "Relay: Offline");
            lv_obj_clear_flag(relay_status_label, LV_OBJ_FLAG_HIDDEN);
        }
        lastRenderedRelayOnline = relayNodeOnline;
    }
}

/* =========================================================
   THEME APPLICATION
   ========================================================= */
static void style_slider(lv_obj_t *s);
static void style_switch(lv_obj_t *sw);
static void style_seg_group(lv_obj_t *g);
static void style_seg_btn(lv_obj_t *b);

static void apply_theme()
{
    const Palette &p = pal();

    for (uint16_t i = 0; i < reg_count; i++) {
        lv_obj_t *o = reg_obj[i];
        switch (reg_role[i]) {
            case R_SCREEN:
                lv_obj_set_style_bg_color(o, hex(p.bg), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                break;
            case R_CHROME:
                lv_obj_set_style_bg_color(o, hex(p.chrome), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                lv_obj_set_style_border_color(o, hex(p.border), 0);
                break;
            case R_CARD:
                lv_obj_set_style_bg_color(o, hex(p.card), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                break;
            case R_RAISED:
                lv_obj_set_style_bg_color(o, hex(p.raised), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                break;
            case R_SUNKEN:
                lv_obj_set_style_bg_color(o, hex(p.sunken), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                break;
            case R_DIVIDER:  lv_obj_set_style_border_color(o, hex(p.border), 0); break;
            case R_TXT:      lv_obj_set_style_text_color(o, hex(p.text), 0); break;
            case R_TXT2:     lv_obj_set_style_text_color(o, hex(p.text2), 0); break;
            case R_TXT3:     lv_obj_set_style_text_color(o, hex(p.text3), 0); break;
            case R_TXT_AMBER: lv_obj_set_style_text_color(o, hex(p.amber), 0); break;
            case R_TXT_ACCENT: lv_obj_set_style_text_color(o, hex(p.accent_hi), 0); break;
            case R_GRID:
                lv_obj_set_style_bg_color(o, hex(p.grid), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                break;
            case R_ARC:
                lv_obj_set_style_arc_color(o, hex(p.border), LV_PART_MAIN);
                lv_obj_set_style_arc_color(o, hex(p.amber), LV_PART_INDICATOR);
                break;
            case R_AMBER_FILL:
                lv_obj_set_style_bg_color(o, hex(p.amber), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                break;
            case R_ACCENT_FILL:
                lv_obj_set_style_bg_color(o, hex(p.accent), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                break;
            case R_ACCENT_RING: lv_obj_set_style_border_color(o, hex(p.accent), 0); break;
            case R_PAST_FILL:
                lv_obj_set_style_bg_color(o, hex(p.chart_past), 0);
                lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
                break;
            case R_ICON_AMBER: tint(o, p.amber); break;
            case R_ICON_GREEN: tint(o, p.green); break;
            case R_SLIDER:    style_slider(o); break;
            case R_SWITCH:    style_switch(o); break;
            case R_SEG_GROUP: style_seg_group(o); break;
            case R_SEG_BTN:   style_seg_btn(o); break;
            default: break;
        }
    }

    lastRenderedRelayMask = 0xFF;
    refresh_all_relay_cards();
    refresh_scene_rail();
    refresh_nav();
    refresh_current_weather();
    refresh_weather_forecast();
    refresh_weather_chart();
    refresh_indoor_card();
    for (int i = 0; i < RELAY_COUNT; i++) {
        lastRenderedRuntimeMinutes[i] = UINT32_MAX;
        lastRenderedRuntimeHeight[i] = -1;
    }
    refresh_runtime();
    history_dirty = true;
}

/* =========================================================
   CONTROL STYLING (shared by creation and re-theming)
   ========================================================= */
static void style_slider(lv_obj_t *s)
{
    const Palette &p = pal();
    lv_obj_set_style_bg_color(s, hex(p.sunken), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_MAIN);

    lv_obj_set_style_bg_color(s, hex(p.accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(s, hex(p.accent_hi), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_radius(s, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_border_color(s, hex(p.bg), LV_PART_KNOB);
    lv_obj_set_style_border_width(s, 3, LV_PART_KNOB);
    lv_obj_set_style_border_opa(s, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s, 8, LV_PART_KNOB);       /* 10 px track -> 26 px knob */
}

static void style_switch(lv_obj_t *sw)
{
    const Palette &p = pal();
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_color(sw, hex(p.sunken), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, hex(p.border), LV_PART_MAIN);
    lv_obj_set_style_border_width(sw, 1, LV_PART_MAIN);
    lv_obj_set_style_border_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(sw, hex(p.accent), LV_STATE_CHECKED);

    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, hex(p.accent), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER,
                            (lv_style_selector_t)LV_PART_INDICATOR | (lv_style_selector_t)LV_STATE_CHECKED);

    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(sw, -4, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, hex(p.knob_off), LV_PART_KNOB);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, hex(p.bg),
                              (lv_style_selector_t)LV_PART_KNOB | (lv_style_selector_t)LV_STATE_CHECKED);
}

static void style_seg_group(lv_obj_t *g)
{
    const Palette &p = pal();
    lv_obj_set_style_bg_color(g, hex(p.sunken), 0);
    lv_obj_set_style_bg_opa(g, LV_OPA_COVER, 0);
}

static void style_seg_btn(lv_obj_t *b)
{
    const Palette &p = pal();
    lv_obj_set_style_bg_color(b, hex(p.accent_soft), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_set_style_border_color(b, hex(p.accent_line), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_opa(b, LV_OPA_COVER, LV_STATE_CHECKED);
    /* Labels inherit text colour, so state lives on the button only. */
    lv_obj_set_style_text_color(b, hex(p.text3), 0);
    lv_obj_set_style_text_color(b, hex(p.accent_hi), LV_STATE_CHECKED);
}

/* =========================================================
   BACKLIGHT / POWER
   ========================================================= */
static void set_backlight(int percent)
{
    if (board == nullptr || board->getBacklight() == nullptr) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    board->getBacklight()->setBrightness(percent);
}

static int dim_percent()
{
    int v = settings.brightness / 3;
    return v < 12 ? 12 : v;
}

static void power_wake()
{
    power_state = PS_ACTIVE;
    set_backlight(settings.brightness);
}

/* The overlay swallows the touch that wakes the panel so the control underneath
   is never triggered by the same press. */
static void wake_overlay_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED) {
        power_wake();
        power_state_log = PS_ACTIVE;
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (wake_overlay) {
            lv_obj_del_async(wake_overlay);
            wake_overlay = nullptr;
        }
    }
}

static void show_wake_overlay()
{
    if (wake_overlay) return;
    wake_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(wake_overlay);
    lv_obj_set_size(wake_overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_clear_flag(wake_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(wake_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wake_overlay, wake_overlay_cb, LV_EVENT_ALL, NULL);
}

static void power_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    if (wake_overlay) return;   /* asleep: waiting for the dismissing touch */

    uint32_t idle = lv_disp_get_inactive_time(NULL);

    if (settings.sleep_sec && idle > settings.sleep_sec * 1000UL) {
        if (power_state != PS_SLEEP) {
            power_state = PS_SLEEP;
            set_backlight(0);
            show_wake_overlay();
            power_state_log = PS_SLEEP;
        }
    } else if (settings.dim_sec && idle > settings.dim_sec * 1000UL) {
        if (power_state != PS_DIM) {
            power_state = PS_DIM;
            set_backlight(dim_percent());
            power_state_log = PS_DIM;
        }
    } else if (power_state != PS_ACTIVE) {
        power_wake();
        power_state_log = PS_ACTIVE;
    }
}

/* =========================================================
   SETTINGS PERSISTENCE
   ========================================================= */
static void settings_mark_dirty()
{
    settings_dirty = true;
    settings_dirty_at = millis();
}

static uint16_t validate_choice(uint16_t value, const uint16_t *allowed, int count, uint16_t fallback)
{
    for (int i = 0; i < count; i++) {
        if (value == allowed[i]) return value;
    }
    return fallback;
}

static void settings_load()
{
    if (!prefs.begin("ui_cfg", true)) {
        Serial.println("NVS: no stored settings, using defaults");
        return;
    }

    int b = prefs.getUChar("bright", 80);
    settings.brightness = (uint8_t)(b < 15 ? 15 : (b > 100 ? 100 : b));

    uint8_t th = prefs.getUChar("theme", THEME_SLATE);
    settings.theme = (th < THEME_COUNT) ? th : (uint8_t)THEME_SLATE;

    uint8_t u = prefs.getUChar("temp_unit", 0);
    settings.unit = (u <= 1) ? u : 0;

    settings.dim_sec = validate_choice(prefs.getUShort("dim_sec", 30), dim_values, 3, 30);
    settings.sleep_sec = validate_choice(prefs.getUShort("sleep_sec", 120), sleep_values, 3, 120);

    settings.show_indoor = prefs.getBool("show_ind", true);
    settings.show_date = prefs.getBool("show_date", true);
    settings.show_scenes = prefs.getBool("show_scen", true);

    prefs.end();
    Serial.printf("NVS: loaded bright=%u theme=%u unit=%u dim=%u sleep=%u\n",
                  settings.brightness, settings.theme, settings.unit,
                  settings.dim_sec, settings.sleep_sec);
}

/* Called from loop(), never from an LVGL callback: a flash write would stall
   rendering and make touch feel like it froze. */
static void settings_save()
{
    if (!prefs.begin("ui_cfg", false)) {
        Serial.println("NVS: open for write failed");
        return;
    }
    prefs.putUChar("bright", settings.brightness);
    prefs.putUChar("theme", settings.theme);
    prefs.putUChar("temp_unit", settings.unit);
    prefs.putUShort("dim_sec", settings.dim_sec);
    prefs.putUShort("sleep_sec", settings.sleep_sec);
    prefs.putBool("show_ind", settings.show_indoor);
    prefs.putBool("show_date", settings.show_date);
    prefs.putBool("show_scen", settings.show_scenes);
    prefs.end();
}

/* =========================================================
   RELAY / SCENE LOGIC
   ========================================================= */
/* A scene stays highlighted only while the relays still match it, so manual
   changes silently fall back to "custom" (no chip selected). */
static void resync_active_scene()
{
    active_scene = -1;
    if (!relayStateKnown || relayPendingMask != 0) return;
    for (int s = 0; s < SCENE_COUNT; s++) {
        uint8_t mask = 0;
        for (int i = 0; i < RELAY_COUNT; i++) if (scene_defs[s].states[i]) mask |= 1U << i;
        if (mask == confirmedRelayMask) {
            active_scene = s;
            break;
        }
    }
}

static void update_confirmed_runtime(uint8_t oldMask, uint8_t newMask)
{
    uint32_t now = millis();
    for (int i = 0; i < RELAY_COUNT; i++) {
        bool wasOn = (oldMask & (1U << i)) != 0;
        bool isOn = (newMask & (1U << i)) != 0;
        if (!wasOn && isOn) relay_since[i] = now;
        if (wasOn && !isOn) runtime_ms[i] += now - relay_since[i];
    }
}

static void relay_event_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    uint32_t now = millis();
    if (now - relayLastTouchMs[i] < TOUCH_GUARD_MS) return;
    relayLastTouchMs[i] = now;
    if (!espNowReady || (relayPendingMask & (1U << i))) return;

    bool desired = relayStateKnown ? !confirmed_relay_on(i) : true;
    requestedRelayMask = desired ? (requestedRelayMask | (1U << i))
                                 : (requestedRelayMask & ~(1U << i));
    relayPendingMask |= 1U << i;
    relayPendingSince[i] = now;
    RelayCommandPacket packet = {
        ESPNOW_PROTOCOL_VERSION, MSG_RELAY_COMMAND, (uint8_t)i,
        (uint8_t)desired, nextCommandSequence++
    };
    relayPendingSequence[i] = packet.sequence;
    pendingScene = -1;
    esp_err_t result = esp_now_send(
        RELAY_ESP32_MAC, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
    if (result != ESP_OK) {
        relayPendingMask &= ~(1U << i);
        relayPendingSequence[i] = 0;
    }
    refresh_relay_card((uint8_t)i);
    lastRenderedPendingMask = relayPendingMask;
    refresh_scene_rail();
}

static void scene_event_cb(lv_event_t *e)
{
    int s = (int)(intptr_t)lv_event_get_user_data(e);
    if (!espNowReady || relayPendingMask) return;
    uint8_t mask = 0;
    for (int i = 0; i < RELAY_COUNT; i++) if (scene_defs[s].states[i]) mask |= 1U << i;
    requestedRelayMask = mask;
    relayPendingMask = RELAY_MASK_ALL;
    uint32_t now = millis();
    for (int i = 0; i < RELAY_COUNT; i++) relayPendingSince[i] = now;
    SceneCommandPacket packet = {
        ESPNOW_PROTOCOL_VERSION, MSG_SCENE_COMMAND, mask, 0, nextCommandSequence++
    };
    for (int i = 0; i < RELAY_COUNT; i++) relayPendingSequence[i] = packet.sequence;
    pendingScene = (int8_t)s;
    esp_err_t result = esp_now_send(
        RELAY_ESP32_MAC, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
    if (result != ESP_OK) {
        relayPendingMask = 0;
        memset(relayPendingSequence, 0, sizeof(relayPendingSequence));
        pendingScene = -1;
    }
    active_scene = -1;
    refresh_all_relay_cards();
    refresh_scene_rail();
}

/* =========================================================
   PAGE ROUTING
   ========================================================= */
static void switch_page(int idx)
{
    if (idx < 0 || idx >= 4 || idx == active_page) return;
    active_page = idx;
    for (int i = 0; i < 4; i++) {
        if (i == idx) lv_obj_clear_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(pages[i], LV_OBJ_FLAG_HIDDEN);
    }
    refresh_nav();
    if (idx == 2 && history_dirty) refresh_history_rows();
}

static void nav_event_cb(lv_event_t *e)
{
    switch_page((int)(intptr_t)lv_event_get_user_data(e));
}

static void hero_event_cb(lv_event_t *e)
{
    LV_UNUSED(e);
    switch_page(1);
}

/* =========================================================
   SEGMENTED CONTROLS AND SWITCHES
   ========================================================= */
static void seg_event_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_target(e);
    lv_obj_t *group = lv_obj_get_parent(btn);
    uint32_t n = lv_obj_get_child_cnt(group);
    int idx = 0;
    for (uint32_t i = 0; i < n; i++) {
        lv_obj_t *child = lv_obj_get_child(group, i);
        if (child == btn) {
            idx = (int)i;
            lv_obj_add_state(child, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(child, LV_STATE_CHECKED);
        }
    }
    void (*handler)(int) = (void (*)(int))lv_obj_get_user_data(group);
    if (handler) handler(idx);
}

static lv_obj_t *make_seg(lv_obj_t *parent, const char *const *labels, int count,
                          int active, void (*handler)(int))
{
    lv_obj_t *group = box(parent, LV_SIZE_CONTENT, LV_SIZE_CONTENT, R_SEG_GROUP, 10);
    lv_obj_set_style_pad_all(group, 3, 0);
    set_flex(group, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 3);
    lv_obj_set_user_data(group, (void *)handler);
    style_seg_group(group);

    for (int i = 0; i < count; i++) {
        lv_obj_t *btn = box(group, LV_SIZE_CONTENT, 40, R_SEG_BTN, 8);
        lv_obj_set_style_pad_hor(btn, 14, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_ext_click_area(btn, 2);
        lv_obj_add_event_cb(btn, seg_event_cb, LV_EVENT_CLICKED, NULL);
        set_flex(btn, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 0);
        txt(btn, labels[i], F_CAP, R_NONE);
        style_seg_btn(btn);
        if (i == active) lv_obj_add_state(btn, LV_STATE_CHECKED);
    }
    return group;
}

static lv_obj_t *make_toggle(lv_obj_t *parent, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_remove_style_all(sw);
    lv_obj_set_size(sw, 48, 26);
    lv_obj_add_flag(sw, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(sw, 10);   /* 68 x 46 touch target */
    reg(sw, R_SWITCH);
    style_switch(sw);
    if (on) lv_obj_add_state(sw, LV_STATE_CHECKED);
    if (cb) lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static lv_obj_t *make_setting_row(lv_obj_t *parent, const char *label)
{
    lv_obj_t *row = box(parent, LV_PCT(100), 44, R_NONE, 0);
    set_flex(row, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 0);
    txt(row, label, F_BODY, R_TXT2);
    return row;
}

/* =========================================================
   HEADER AND NAVIGATION
   ========================================================= */
static void build_header(lv_obj_t *parent)
{
    lv_obj_t *bar = box(parent, 800, 52, R_CHROME, 0);
    lv_obj_set_pos(bar, 0, 0);
    set_border(bar, 1, LV_BORDER_SIDE_BOTTOM);

    lv_obj_t *left = box(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT, R_NONE, 0);
    set_flex(left, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 18, 0);
    header_date = txt(left, "", F_BODY, R_TXT2);

    lv_obj_t *status = box(bar, LV_SIZE_CONTENT, LV_SIZE_CONTENT, R_NONE, 0);
    set_flex(status, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 8);
    lv_obj_align(status, LV_ALIGN_CENTER, 70, 0);
    wifi_status_label = txt(status, "Wi-Fi: Offline", F_CAP, R_TXT3);
    weather_status_label = txt(status, "Weather: Stale", F_CAP, R_TXT3);
    relay_status_label = txt(status, "Relay: Offline", F_CAP, R_TXT3);

    clock_label = txt(bar, "--:--", F_CLOCK, R_TXT);
    lv_obj_align(clock_label, LV_ALIGN_RIGHT_MID, -18, 0);
}

static void build_nav(lv_obj_t *parent)
{
    static const char *labels[4] = {"Home", "Weather", "History", "Settings"};
    static const lv_img_dsc_t *icons[4] = {&icon_house_20, &icon_cloud_20,
                                           &icon_rotate_ccw_clock_20, &icon_settings_20};

    lv_obj_t *bar = box(parent, 800, 56, R_CHROME, 0);
    lv_obj_set_pos(bar, 0, 424);
    set_border(bar, 1, LV_BORDER_SIDE_TOP);

    for (int i = 0; i < 4; i++) {
        lv_obj_t *tab = box(bar, 184, 44, R_NONE, 10);
        lv_obj_set_pos(tab, 14 + i * 196, 6);
        set_border(tab, 1, LV_BORDER_SIDE_FULL);
        set_flex(tab, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 3);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
        add_press_feedback(tab);
        lv_obj_add_event_cb(tab, nav_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        nav_icons[i] = icon_img(tab, icons[i], R_NONE);
        txt(tab, labels[i], F_CAP, R_NONE);
        nav_tabs[i] = tab;
    }
}

/* =========================================================
   HOME PAGE
   ========================================================= */
static void build_relay_card(lv_obj_t *grid, int i, uint8_t col, uint8_t row)
{
    lv_obj_t *card = box(grid, 0, 0, R_NONE, 14);
    lv_obj_set_grid_cell(card, LV_GRID_ALIGN_STRETCH, col, 1, LV_GRID_ALIGN_STRETCH, row, 1);
    set_border(card, 1, LV_BORDER_SIDE_FULL);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 14, 0);
    set_flex(card, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    add_press_feedback(card);
    lv_obj_add_event_cb(card, relay_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

    lv_obj_t *top = box(card, LV_PCT(100), 44, R_NONE, 0);
    set_flex(top, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 0);

    lv_obj_t *chip = box(top, 44, 44, R_NONE, 12);
    lv_obj_t *ic = icon_img(chip, relay_defs[i].icon, R_NONE);
    lv_obj_center(ic);

    lv_obj_t *pill = box(top, LV_SIZE_CONTENT, LV_SIZE_CONTENT, R_NONE, LV_RADIUS_CIRCLE);
    lv_obj_set_style_pad_hor(pill, 12, 0);
    lv_obj_set_style_pad_ver(pill, 5, 0);
    lv_obj_t *pill_label = txt(pill, "OFF", F_CAP, R_NONE);

    lv_obj_t *bottom = box(card, LV_PCT(100), LV_SIZE_CONTENT, R_NONE, 0);
    set_flex(bottom, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 2);
    txt(bottom, relay_defs[i].name, F_RELAY, R_TXT);
    lv_obj_t *sub = txt(bottom, relay_defs[i].sub, F_CAP, R_NONE);

    relay_cards[i] = card;
    relay_chips[i] = chip;
    relay_icons[i] = ic;
    relay_pills[i] = pill;
    relay_pill_labels[i] = pill_label;
    relay_subs[i] = sub;
}

static void build_scene_rail(lv_obj_t *parent)
{
    scene_rail = box(parent, LV_PCT(100), 48, R_CARD, 12);
    lv_obj_set_style_pad_all(scene_rail, 4, 0);
    set_flex(scene_rail, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 6);

    for (int i = 0; i < SCENE_COUNT; i++) {
        lv_obj_t *btn = box(scene_rail, 0, 40, R_NONE, 8);
        lv_obj_set_flex_grow(btn, 1);
        set_border(btn, 1, LV_BORDER_SIDE_FULL);
        set_flex(btn, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 7);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        add_press_feedback(btn);
        lv_obj_add_event_cb(btn, scene_event_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        scene_icons[i] = icon_img(btn, scene_defs[i].icon, R_NONE);
        txt(btn, scene_defs[i].name, F_CAP, R_NONE);
        scene_btns[i] = btn;
    }
}

static lv_obj_t *build_mini_metric(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *cell = box(parent, 73, LV_SIZE_CONTENT, R_NONE, 0);
    set_flex(cell, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 1);
    txt(cell, caption, F_CAP, R_TXT3);
    return txt(cell, "--", F_CAP, R_TXT2);
}

static void build_weather_hero(lv_obj_t *parent)
{
    lv_obj_t *card = box(parent, LV_PCT(100), 0, R_CARD, 14);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 14, 0);
    set_flex(card, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 10);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, hero_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *left = box(card, 0, LV_PCT(100), R_NONE, 0);
    lv_obj_set_flex_grow(left, 1);
    set_flex(left, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 0);

    hero_temp = txt(left, "--", F_HERO, R_TXT);
    hero_cond = txt(left, "--", F_BODY, R_TXT2);

    lv_obj_t *grid = box(left, LV_PCT(100), LV_SIZE_CONTENT, R_NONE, 0);
    lv_obj_set_style_pad_top(grid, 14, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 8, 0);
    lv_obj_set_style_pad_column(grid, 6, 0);
    hero_metrics[0] = build_mini_metric(grid, "FEELS LIKE");
    hero_metrics[1] = build_mini_metric(grid, "HUMIDITY");
    hero_metrics[2] = build_mini_metric(grid, "RAIN");
    hero_metrics[3] = build_mini_metric(grid, "WIND");

    lv_obj_t *strip = box(card, 84, LV_PCT(100), R_DIVIDER, 0);
    set_border(strip, 1, LV_BORDER_SIDE_LEFT);
    lv_obj_set_style_pad_left(strip, 6, 0);
    set_flex(strip, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 0);

    hero_icon = icon_img(strip, &icon_sun_32, R_NONE);
    icon_img(strip, &icon_botanical_72, R_ICON_GREEN);

    lv_obj_t *pill = box(strip, LV_SIZE_CONTENT, LV_SIZE_CONTENT, R_RAISED, 6);
    lv_obj_set_style_pad_hor(pill, 6, 0);
    lv_obj_set_style_pad_ver(pill, 4, 0);
    set_flex(pill, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 4);
    icon_img(pill, &icon_sun_12, R_ICON_AMBER);
    hero_sunset_label = txt(pill, "--:--", F_CAP, R_TXT_AMBER);
}

static void build_indoor_card(lv_obj_t *parent)
{
    indoor_card = box(parent, LV_PCT(100), 90, R_CARD, 14);
    lv_obj_set_style_pad_hor(indoor_card, 18, 0);
    lv_obj_set_style_pad_ver(indoor_card, 14, 0);
    set_flex(indoor_card, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 0);

    lv_obj_t *l = box(indoor_card, LV_SIZE_CONTENT, LV_SIZE_CONTENT, R_NONE, 0);
    set_flex(l, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 2);
    txt(l, "INDOOR TEMP", F_CAP, R_TXT3);
    indoor_temp_label = txt(l, "--", F_CLOCK, R_TXT);

    lv_obj_t *r = box(indoor_card, LV_SIZE_CONTENT, LV_SIZE_CONTENT, R_NONE, 0);
    set_flex(r, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, 2);
    txt(r, "HUMIDITY", F_CAP, R_TXT3);
    indoor_hum_label = txt(r, "--", F_CLOCK, R_TXT_ACCENT);
}

static void build_home_page(lv_obj_t *page)
{
    set_flex(page, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 10);

    lv_obj_t *left = box(page, 0, LV_PCT(100), R_NONE, 0);
    lv_obj_set_flex_grow(left, 1);
    set_flex(left, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 10);

    lv_obj_t *grid = box(left, LV_PCT(100), 0, R_NONE, 0);
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_set_grid_dsc_array(grid, relay_cols, relay_rows);
    lv_obj_set_style_pad_row(grid, 10, 0);
    lv_obj_set_style_pad_column(grid, 10, 0);
    build_relay_card(grid, 0, 0, 0);
    build_relay_card(grid, 1, 1, 0);
    build_relay_card(grid, 2, 0, 1);
    build_relay_card(grid, 3, 1, 1);

    build_scene_rail(left);

    lv_obj_t *right = box(page, 280, LV_PCT(100), R_NONE, 0);
    set_flex(right, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 10);
    build_weather_hero(right);
    build_indoor_card(right);
}

/* =========================================================
   WEATHER PAGE
   ========================================================= */
static lv_obj_t *build_stat_tile(lv_obj_t *parent, const char *caption)
{
    lv_obj_t *tile = box(parent, 102, LV_SIZE_CONTENT, R_RAISED, 8);
    lv_obj_set_style_pad_hor(tile, 10, 0);
    lv_obj_set_style_pad_ver(tile, 8, 0);
    set_flex(tile, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 2);
    txt(tile, caption, F_CAP, R_TXT3);
    return txt(tile, "--", F_BODY, R_TXT);
}

/* The dome is a 180x180 arc whose visible upper half fits the 118 px holder, so
   nothing is clipped and the times sit clear of the arc ends. */
static void build_sun_gauge(lv_obj_t *parent)
{
    lv_obj_t *holder = box(parent, 212, 118, R_RAISED, 10);

    sun_arc = lv_arc_create(holder);
    lv_obj_remove_style_all(sun_arc);
    lv_obj_set_size(sun_arc, 180, 180);
    lv_obj_set_pos(sun_arc, 16, 6);
    lv_obj_clear_flag(sun_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(sun_arc, 0);
    lv_arc_set_bg_angles(sun_arc, 180, 360);
    lv_arc_set_angles(sun_arc, 180, 180);
    lv_obj_set_style_arc_width(sun_arc, 3, LV_PART_MAIN);
    lv_obj_set_style_arc_opa(sun_arc, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sun_arc, 4, LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(sun_arc, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(sun_arc, true, LV_PART_INDICATOR);
    reg(sun_arc, R_ARC);

    sun_dot = box(holder, 10, 10, R_AMBER_FILL, LV_RADIUS_CIRCLE);
    lv_obj_add_flag(sun_dot, LV_OBJ_FLAG_HIDDEN);

    sunrise_label = txt(holder, "--:--", F_CAP, R_TXT2);
    lv_obj_align(sunrise_label, LV_ALIGN_BOTTOM_LEFT, 6, -2);
    sunset_label = txt(holder, "--:--", F_CAP, R_TXT2);
    lv_obj_align(sunset_label, LV_ALIGN_BOTTOM_RIGHT, -6, -2);
}

static void build_weather_detail(lv_obj_t *parent)
{
    lv_obj_t *card = box(parent, 240, LV_PCT(100), R_CARD, 14);
    lv_obj_set_style_pad_all(card, 14, 0);
    set_flex(card, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, 0);

    lv_obj_t *head = box(card, 212, LV_SIZE_CONTENT, R_NONE, 0);
    set_flex(head, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 2);
    wx_temp = txt(head, "--", F_HERO, R_TXT);
    wx_cond = txt(head, "--", F_BODY, R_TXT2);
    lv_obj_set_width(wx_cond, 212);
    lv_label_set_long_mode(wx_cond, LV_LABEL_LONG_WRAP);

    build_sun_gauge(card);

    lv_obj_t *matrix = box(card, 212, LV_SIZE_CONTENT, R_NONE, 0);
    lv_obj_set_flex_flow(matrix, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(matrix, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(matrix, 8, 0);
    lv_obj_set_style_pad_column(matrix, 8, 0);
    wx_stats[0] = build_stat_tile(matrix, "FEELS LIKE");
    wx_stats[1] = build_stat_tile(matrix, "HUMIDITY");
    wx_stats[2] = build_stat_tile(matrix, "PRESSURE");
    wx_stats[3] = build_stat_tile(matrix, "WIND");
}

/* One flat flex row: nesting size-to-content rows inside a size-to-content row
   collapses the outer width in LVGL and clips the first entry. */
static void build_chart_legend(lv_obj_t *parent)
{
    lv_obj_t *legend = box(parent, 88, 20, R_NONE, 0);
    set_flex(legend, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, 6);
    box(legend, 14, 4, R_ACCENT_FILL, 2);
    txt(legend, "Forecast", F_CAP, R_TXT2);
}

static void build_chart_card(lv_obj_t *parent)
{
    lv_obj_t *card = box(parent, LV_PCT(100), 0, R_CARD, 14);
    lv_obj_set_flex_grow(card, 1);
    lv_obj_set_style_pad_hor(card, 16, 0);
    lv_obj_set_style_pad_ver(card, 12, 0);

    lv_obj_t *head = box(card, LV_PCT(100), LV_SIZE_CONTENT, R_NONE, 0);
    lv_obj_set_pos(head, 0, 0);
    set_flex(head, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 0);
    txt(head, "Next 24 Hours", F_BODY, R_TXT);
    build_chart_legend(head);

    lv_obj_set_pos(box(card, 490, 1, R_GRID, 0), 0, 77);
    lv_obj_set_pos(box(card, 490, 1, R_GRID, 0), 0, 145);

    chart_ref_hi = txt(card, "--", F_CAP, R_TXT3);
    lv_obj_set_pos(chart_ref_hi, 4, 60);
    chart_ref_lo = txt(card, "--", F_CAP, R_TXT3);
    lv_obj_set_pos(chart_ref_lo, 4, 128);

    chart = lv_chart_create(card);
    lv_obj_remove_style_all(chart);
    lv_obj_set_size(chart, 490, 170);
    lv_obj_set_pos(chart, 0, 32);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 0, 0);
    lv_chart_set_point_count(chart, 9);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(chart, 3, LV_PART_ITEMS);
    lv_obj_set_style_line_rounded(chart, true, LV_PART_ITEMS);
    ser_fc = lv_chart_add_series(chart, hex(pal().accent), LV_CHART_AXIS_PRIMARY_Y);

    static const char *hourLabels[9] = {
        "0h", "3h", "6h", "9h", "12h", "15h", "18h", "21h", "24h"
    };
    for (int i = 0; i < 9; i++) {
        lv_obj_t *label = txt(card, hourLabels[i], F_CAP,
                              i == 0 ? R_TXT_ACCENT : R_TXT3);
        lv_obj_set_width(label, 34);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        int x = (i * 456) / 8;
        lv_obj_set_pos(label, x, 204);
    }
}

static void build_forecast_strip(lv_obj_t *parent)
{
    lv_obj_t *strip = box(parent, LV_PCT(100), 96, R_CARD, 12);
    set_flex(strip, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, 0);

    for (int i = 0; i < 5; i++) {
        lv_obj_t *cell = box(strip, 0, LV_PCT(100), i < 4 ? R_DIVIDER : R_NONE, 0);
        lv_obj_set_flex_grow(cell, 1);
        if (i < 4) set_border(cell, 1, LV_BORDER_SIDE_RIGHT);
        set_flex(cell, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, 5);

        fc_days[i] = txt(cell, "--", F_CAP, R_TXT);
        fc_icons[i] = icon_img(cell, &icon_sun_20, R_NONE);
        fc_temps[i] = txt(cell, "--", F_CAP, R_TXT2);
    }
}

static void build_weather_page(lv_obj_t *page)
{
    set_flex(page, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 10);
    build_weather_detail(page);

    lv_obj_t *stack = box(page, 0, LV_PCT(100), R_NONE, 0);
    lv_obj_set_flex_grow(stack, 1);
    set_flex(stack, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 10);
    build_chart_card(stack);
    build_forecast_strip(stack);
}

/* =========================================================
   HISTORY PAGE
   ========================================================= */
static void refresh_history_rows()
{
    const Palette &p = pal();
    for (int i = 0; i < HISTORY_MAX; i++) {
        HistoryRowWidgets &row = historyRows[i];
        if (i >= event_count) {
            lv_obj_add_flag(row.container, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(row.container, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(row.container, hex(p.raised), 0);
        lv_obj_set_style_text_color(row.timeLabel, hex(p.text3), 0);
        lv_obj_set_style_text_color(row.nameLabel, hex(p.text), 0);
        lv_obj_set_style_text_color(row.detailLabel, hex(p.text3), 0);
        const Event &e = events[i];
        bool scene = (e.kind == 2);
        bool on = (e.kind == 1);
        lv_label_set_text(row.timeLabel, e.time);
        lv_label_set_text(row.nameLabel, e.name);
        lv_label_set_text(row.detailLabel, e.detail);

        uint32_t pill_bg = scene ? p.scene_soft[e.scene] : (on ? p.accent_bay : p.sunken);
        uint32_t pill_fg = scene ? p.scene_fg[e.scene] : (on ? p.accent_hi : p.text3);
        lv_obj_set_style_bg_color(row.statePill, hex(pill_bg), 0);
        lv_obj_set_style_text_color(row.stateLabel, hex(pill_fg), 0);
        lv_label_set_text(row.stateLabel, scene ? "SCENE" : (on ? "ON" : "OFF"));
    }
    history_dirty = false;
}

static void build_history_page(lv_obj_t *page)
{
    set_flex(page, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 10);

    lv_obj_t *left = box(page, 462, LV_PCT(100), R_CARD, 14);
    lv_obj_set_style_pad_hor(left, 16, 0);
    lv_obj_set_style_pad_ver(left, 14, 0);
    set_flex(left, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 12);
    txt(left, "Relay Activity", F_TITLE, R_TXT);

    history_list = box(left, LV_PCT(100), 0, R_NONE, 0);
    lv_obj_set_flex_grow(history_list, 1);
    set_flex(history_list, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 8);
    lv_obj_add_flag(history_list, LV_OBJ_FLAG_CLICKABLE |
                                  LV_OBJ_FLAG_SCROLLABLE |
                                  LV_OBJ_FLAG_SCROLL_MOMENTUM);
    lv_obj_clear_flag(history_list, LV_OBJ_FLAG_SCROLL_ELASTIC | LV_OBJ_FLAG_SCROLL_CHAIN);
    lv_obj_set_scroll_dir(history_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(history_list, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < HISTORY_MAX; i++) {
        HistoryRowWidgets &row = historyRows[i];
        row.container = box(history_list, LV_PCT(100), 50, R_NONE, 8);
        lv_obj_set_style_bg_color(row.container, hex(pal().raised), 0);
        lv_obj_set_style_bg_opa(row.container, LV_OPA_COVER, 0);
        row.timeLabel = txt(row.container, "--:--", F_CAP, R_NONE);
        lv_obj_align(row.timeLabel, LV_ALIGN_LEFT_MID, 12, 0);
        lv_obj_t *mid = box(row.container, 250, LV_SIZE_CONTENT, R_NONE, 0);
        set_flex(mid, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 1);
        lv_obj_align(mid, LV_ALIGN_LEFT_MID, 58, 0);
        row.nameLabel = txt(mid, "", F_BODY, R_NONE);
        row.detailLabel = txt(mid, "", F_CAP, R_NONE);
        row.statePill = box(row.container, 56, 24, R_NONE, 6);
        lv_obj_align(row.statePill, LV_ALIGN_RIGHT_MID, -12, 0);
        row.stateLabel = txt(row.statePill, "", F_CAP, R_NONE);
        lv_obj_center(row.stateLabel);
        lv_obj_add_flag(row.container, LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_t *right = box(page, 300, LV_PCT(100), R_CARD, 14);
    lv_obj_set_style_pad_hor(right, 16, 0);
    lv_obj_set_style_pad_ver(right, 14, 0);
    set_flex(right, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 16);
    txt(right, "Session Runtime", F_TITLE, R_TXT);

    lv_obj_t *bars = box(right, LV_PCT(100), 210, R_NONE, 0);
    set_flex(bars, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, 14);
    for (int i = 0; i < RELAY_COUNT; i++) {
        lv_obj_t *col = box(bars, 0, LV_PCT(100), R_NONE, 0);
        lv_obj_set_flex_grow(col, 1);
        set_flex(col, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, 6);

        runtime_bars[i] = box(col, LV_PCT(100), 10, R_NONE, 6);
        char tag[4];
        snprintf(tag, sizeof(tag), "R%d", i + 1);
        txt(col, tag, F_CAP, R_TXT);
        runtime_labels[i] = txt(col, "--", F_CAP, R_TXT2);
    }
}

/* =========================================================
   SETTINGS PAGE
   ========================================================= */
static void brightness_event_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int value = (int)lv_slider_get_value(s);
    settings.brightness = (uint8_t)value;
    lv_label_set_text_fmt(brightness_label, "%d%%", value);
    set_backlight(value);
    power_state = PS_ACTIVE;
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) settings_mark_dirty();
}

static void theme_handler(int index)
{
    settings.theme = (uint8_t)index;
    apply_theme();
    settings_mark_dirty();
}

static void unit_handler(int index)
{
    settings.unit = (uint8_t)index;
    lastRenderedIndoorTemp = INT16_MIN;
    refresh_current_weather();
    refresh_weather_forecast();
    refresh_weather_chart();
    refresh_indoor_card();
    settings_mark_dirty();
}

static void dim_handler(int index)
{
    settings.dim_sec = dim_values[index];
    power_wake();
    settings_mark_dirty();
}

static void sleep_handler(int index)
{
    settings.sleep_sec = sleep_values[index];
    power_wake();
    settings_mark_dirty();
}

static void show_indoor_cb(lv_event_t *e)
{
    settings.show_indoor = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    set_collapsed(indoor_card, !settings.show_indoor);
    settings_mark_dirty();
}

static void show_date_cb(lv_event_t *e)
{
    settings.show_date = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    set_collapsed(header_date, !settings.show_date);
    settings_mark_dirty();
}

static void show_scenes_cb(lv_event_t *e)
{
    settings.show_scenes = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    set_collapsed(scene_rail, !settings.show_scenes);
    settings_mark_dirty();
}

static int index_of(uint16_t value, const uint16_t *list, int count)
{
    for (int i = 0; i < count; i++) {
        if (list[i] == value) return i;
    }
    return 0;
}

static lv_obj_t *build_settings_panel(lv_obj_t *page, lv_coord_t w, const char *title, lv_coord_t gap)
{
    lv_obj_t *panel = box(page, w, LV_PCT(100), R_CARD, 14);
    lv_obj_set_style_pad_hor(panel, 16, 0);
    lv_obj_set_style_pad_ver(panel, 14, 0);
    set_flex(panel, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, gap);
    txt(panel, title, F_TITLE, R_TXT);
    return panel;
}

static void build_settings_page(lv_obj_t *page)
{
    static const char *dim_opts[3] = {"Off", "30s", "1m"};
    static const char *sleep_opts[3] = {"2m", "5m", "Never"};
    static const char *theme_opts[3] = {"Slate", "AMOLED", "Aurora"};
    static const char *unit_opts[2] = {DEG "C", DEG "F"};

    set_flex(page, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 10);

    lv_obj_t *left = build_settings_panel(page, 380, "Display", 18);

    lv_obj_t *bright = box(left, LV_PCT(100), LV_SIZE_CONTENT, R_NONE, 0);
    set_flex(bright, LV_FLEX_FLOW_COLUMN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, 6);
    lv_obj_t *brow = box(bright, LV_PCT(100), LV_SIZE_CONTENT, R_NONE, 0);
    set_flex(brow, LV_FLEX_FLOW_ROW, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, 0);
    txt(brow, "Brightness", F_BODY, R_TXT2);
    brightness_label = txt(brow, "--", F_BODY, R_TXT_ACCENT);

    /* Slim visible track, generous invisible touch band around it. */
    lv_obj_t *holder = box(bright, LV_PCT(100), 44, R_NONE, 0);
    lv_obj_t *slider = lv_slider_create(holder);
    lv_obj_remove_style_all(slider);
    lv_obj_set_size(slider, 316, 10);
    lv_obj_add_flag(slider, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_center(slider);
    lv_obj_set_ext_click_area(slider, 17);
    lv_slider_set_range(slider, 15, 100);
    lv_slider_set_value(slider, settings.brightness, LV_ANIM_OFF);
    reg(slider, R_SLIDER);
    style_slider(slider);
    lv_obj_add_event_cb(slider, brightness_event_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(slider, brightness_event_cb, LV_EVENT_RELEASED, NULL);

    make_seg(make_setting_row(left, "Dim After"), dim_opts, 3,
             index_of(settings.dim_sec, dim_values, 3), dim_handler);
    make_seg(make_setting_row(left, "Sleep After"), sleep_opts, 3,
             index_of(settings.sleep_sec, sleep_values, 3), sleep_handler);

    lv_obj_t *right = build_settings_panel(page, 382, "Preferences", 14);
    make_seg(make_setting_row(right, "Theme"), theme_opts, 3, settings.theme, theme_handler);
    make_seg(make_setting_row(right, "Temperature Unit"), unit_opts, 2, settings.unit, unit_handler);
    make_toggle(make_setting_row(right, "Show Indoor Climate"), settings.show_indoor, show_indoor_cb);
    make_toggle(make_setting_row(right, "Show Header Date"), settings.show_date, show_date_cb);
    make_toggle(make_setting_row(right, "Show Scene Rail"), settings.show_scenes, show_scenes_cb);
}

/* =========================================================
   NETWORKING (never calls LVGL)
   ========================================================= */
static int16_t to_deci_celsius(float value)
{
    return (int16_t)lroundf(value * 10.0f);
}

static uint8_t wind_direction_sector(float degrees)
{
    return ((int)lroundf(degrees / 45.0f)) & 7;
}

static bool fetch_and_parse_weather()
{
    if (WiFi.status() != WL_CONNECTED || !DASHBOARD_HAS_SECRETS ||
        OPENWEATHER_KEY[0] == '\0' ||
        WEATHER_LATITUDE[0] == '\0' ||
        WEATHER_LONGITUDE[0] == '\0') return false;

    WeatherData next = {};

    auto percent = [](float probability) -> uint8_t {
        int value = (int)lroundf(probability * 100.0f);
        if (value < 0) value = 0;
        if (value > 100) value = 100;
        return (uint8_t)value;
    };
    auto hundredth_mm = [](float millimeters) -> uint16_t {
        int32_t value = (int32_t)lroundf(millimeters * 100.0f);
        if (value < 0) value = 0;
        if (value > 65535) value = 65535;
        return (uint16_t)value;
    };

    char url[320];

    /* Current Weather 2.5 is available with the standard OpenWeather key. */
    {
        snprintf(url, sizeof(url),
                 "http://api.openweathermap.org/data/2.5/weather?lat=%s&lon=%s"
                 "&appid=%s&units=metric",
                 WEATHER_LATITUDE, WEATHER_LONGITUDE, OPENWEATHER_KEY);

        WiFiClient client;
        HTTPClient http;
        http.setConnectTimeout(10000);
        http.setTimeout(15000);
        if (!http.begin(client, url)) return false;
        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("Current weather HTTP failed: %d (%s)\n", code,
                          HTTPClient::errorToString(code).c_str());
            if (code > 0) {
                String response = http.getString();
                if (response.length() > 240) response.remove(240);
                Serial.printf("Weather response: %s\n", response.c_str());
            }
            http.end();
            return false;
        }

        StaticJsonDocument<1024> filter;
        filter["dt"] = true;
        filter["main"]["temp"] = true;
        filter["main"]["feels_like"] = true;
        filter["main"]["pressure"] = true;
        filter["main"]["humidity"] = true;
        filter["visibility"] = true;
        filter["wind"]["speed"] = true;
        filter["wind"]["gust"] = true;
        filter["wind"]["deg"] = true;
        filter["clouds"]["all"] = true;
        filter["rain"]["1h"] = true;
        filter["sys"]["sunrise"] = true;
        filter["sys"]["sunset"] = true;
        filter["weather"][0]["id"] = true;
        filter["weather"][0]["description"] = true;

        DynamicJsonDocument doc(6144);
        DeserializationError error = deserializeJson(
            doc, http.getStream(), DeserializationOption::Filter(filter));
        http.end();
        if (error) {
            Serial.printf("Current weather JSON failed: %s\n", error.c_str());
            return false;
        }

        if (doc["main"]["temp"].isNull() ||
            doc["main"]["feels_like"].isNull() ||
            doc["main"]["humidity"].isNull() ||
            doc["main"]["pressure"].isNull() ||
            doc["wind"]["speed"].isNull() ||
            doc["sys"]["sunrise"].isNull() ||
            doc["sys"]["sunset"].isNull() ||
            doc["weather"][0]["id"].isNull()) {
            Serial.println("Current weather JSON is missing required fields");
            return false;
        }

        next.observationUnix = doc["dt"] | 0UL;
        next.temperatureDc = to_deci_celsius(doc["main"]["temp"] | 0.0f);
        next.feelsLikeDc = to_deci_celsius(doc["main"]["feels_like"] | 0.0f);
        next.humidityPercent = doc["main"]["humidity"] | 0;
        next.pressureHpa = doc["main"]["pressure"] | 0;
        next.cloudPercent = doc["clouds"]["all"] | 0;
        next.visibilityMeters =
            (uint16_t)min((uint32_t)(doc["visibility"] | 0UL), 65535UL);
        next.windKmh = (uint16_t)lroundf((doc["wind"]["speed"] | 0.0f) * 3.6f);
        next.windGustKmh = (uint16_t)lroundf((doc["wind"]["gust"] | 0.0f) * 3.6f);
        next.windDirectionValid = !doc["wind"]["deg"].isNull();
        if (next.windDirectionValid) {
            next.windDirectionSector = wind_direction_sector(doc["wind"]["deg"] | 0.0f);
        }
        next.rainHundredthMm = hundredth_mm(doc["rain"]["1h"] | 0.0f);
        next.sunriseUnix = doc["sys"]["sunrise"] | 0UL;
        next.sunsetUnix = doc["sys"]["sunset"] | 0UL;
        next.weatherId = doc["weather"][0]["id"] | 0;
        snprintf(next.description, sizeof(next.description), "%s",
                 doc["weather"][0]["description"] | "");
    }

    /* The free forecast endpoint supplies 3-hour steps for five days. */
    {
        snprintf(url, sizeof(url),
                 "http://api.openweathermap.org/data/2.5/forecast?lat=%s&lon=%s"
                 "&appid=%s&units=metric",
                 WEATHER_LATITUDE, WEATHER_LONGITUDE, OPENWEATHER_KEY);

        WiFiClient client;
        HTTPClient http;
        http.setConnectTimeout(10000);
        http.setTimeout(15000);
        if (!http.begin(client, url)) return false;
        int code = http.GET();
        if (code != HTTP_CODE_OK) {
            Serial.printf("Forecast HTTP failed: %d (%s)\n", code,
                          HTTPClient::errorToString(code).c_str());
            if (code > 0) {
                String response = http.getString();
                if (response.length() > 240) response.remove(240);
                Serial.printf("Forecast response: %s\n", response.c_str());
            }
            http.end();
            return false;
        }

        StaticJsonDocument<1024> filter;
        filter["list"][0]["dt"] = true;
        filter["list"][0]["main"]["temp"] = true;
        filter["list"][0]["main"]["temp_min"] = true;
        filter["list"][0]["main"]["temp_max"] = true;
        filter["list"][0]["pop"] = true;
        filter["list"][0]["rain"]["3h"] = true;
        filter["list"][0]["weather"][0]["id"] = true;

        DynamicJsonDocument doc(24576);
        DeserializationError error = deserializeJson(
            doc, http.getStream(), DeserializationOption::Filter(filter));
        http.end();
        JsonArray list = doc["list"].as<JsonArray>();
        if (error || list.size() < 8) {
            Serial.printf("Forecast JSON failed: %s\n",
                          error ? error.c_str() : "missing forecast entries");
            return false;
        }

        next.hourlyUnix[0] = next.observationUnix;
        next.hourlyTempDc[0] = next.temperatureDc;
        next.hourlyRainPercent[0] = percent(list[0]["pop"] | 0.0f);
        next.rainPercent = next.hourlyRainPercent[0];
        for (int i = 1; i < 9; i++) {
            JsonObject entry = list[i - 1];
            next.hourlyUnix[i] = entry["dt"] | 0UL;
            next.hourlyTempDc[i] = to_deci_celsius(entry["main"]["temp"] | 0.0f);
            next.hourlyRainPercent[i] = percent(entry["pop"] | 0.0f);
        }

        int dayKeys[5] = {-1, -1, -1, -1, -1};
        uint8_t bestHourDistance[5] = {255, 255, 255, 255, 255};
        int dayCount = 0;
        for (JsonObject entry : list) {
            time_t timestamp = entry["dt"] | 0UL;
            struct tm forecastTm = {};
            localtime_r(&timestamp, &forecastTm);
            int dayKey = forecastTm.tm_year * 400 + forecastTm.tm_yday;
            int day = -1;
            for (int i = 0; i < dayCount; i++) {
                if (dayKeys[i] == dayKey) {
                    day = i;
                    break;
                }
            }
            if (day < 0) {
                if (dayCount >= 5) continue;
                day = dayCount++;
                dayKeys[day] = dayKey;
                next.forecastUnix[day] = (uint32_t)timestamp;
                next.forecastHighDc[day] = INT16_MIN;
                next.forecastLowDc[day] = INT16_MAX;
            }

            int16_t high = to_deci_celsius(entry["main"]["temp_max"] | 0.0f);
            int16_t low = to_deci_celsius(entry["main"]["temp_min"] | 0.0f);
            if (high > next.forecastHighDc[day]) next.forecastHighDc[day] = high;
            if (low < next.forecastLowDc[day]) next.forecastLowDc[day] = low;

            uint8_t chance = percent(entry["pop"] | 0.0f);
            if (chance > next.forecastRainPercent[day]) {
                next.forecastRainPercent[day] = chance;
            }
            uint32_t totalRain = next.forecastRainHundredthMm[day] +
                                 hundredth_mm(entry["rain"]["3h"] | 0.0f);
            next.forecastRainHundredthMm[day] =
                (uint16_t)min(totalRain, (uint32_t)65535);

            uint8_t distance = (uint8_t)abs(forecastTm.tm_hour - 12);
            if (distance < bestHourDistance[day]) {
                bestHourDistance[day] = distance;
                next.forecastWeatherId[day] = entry["weather"][0]["id"] | 0;
            }
        }
        if (dayCount < 5) {
            Serial.println("Forecast JSON contains fewer than five local days");
            return false;
        }
    }

    /* OpenWeather 2.5 has no UV field. Open-Meteo provides the real current
       cloud-adjusted UV index without requiring another API key. A UV failure
       does not discard otherwise valid OpenWeather data. */
    next.uvValid = false;
    {
        snprintf(url, sizeof(url),
                 "http://air-quality-api.open-meteo.com/v1/air-quality"
                 "?latitude=%s&longitude=%s&current=uv_index&forecast_days=1",
                 WEATHER_LATITUDE, WEATHER_LONGITUDE);

        WiFiClient client;
        HTTPClient http;
        http.setConnectTimeout(8000);
        http.setTimeout(10000);
        if (http.begin(client, url)) {
            int code = http.GET();
            if (code == HTTP_CODE_OK) {
                String payload = http.getString();
                DynamicJsonDocument doc(1024);
                DeserializationError error = deserializeJson(doc, payload);
                if (!error && !doc["current"]["uv_index"].isNull()) {
                    float uv = doc["current"]["uv_index"] | 0.0f;
                    if (uv < 0.0f) uv = 0.0f;
                    if (uv > 20.0f) uv = 20.0f;
                    next.uvIndex = (uint8_t)lroundf(uv);
                    next.uvValid = true;
                } else {
                    Serial.printf("UV JSON failed: %s\n",
                                  error ? error.c_str() : "missing UV index");
                }
            } else {
                Serial.printf("UV HTTP failed: %d\n", code);
            }
            http.end();
        }
    }
    next.valid = true;

    portENTER_CRITICAL(&weatherMux);
    pendingWeatherData = next;
    weatherUiDirty = true;
    weatherRequestFailed = false;
    lastWeatherSuccessMs = millis();
    portEXIT_CRITICAL(&weatherMux);
    if (next.uvValid) {
        Serial.printf("Weather updated (UV %u)\n", (unsigned)next.uvIndex);
    } else {
        Serial.println("Weather updated (UV unavailable)");
    }
    return true;
}

static void weather_task(void *parameter)
{
    LV_UNUSED(parameter);
    bool connectionSettled = false;
    for (;;) {
        if (WiFi.status() == WL_CONNECTED) {
            if (!connectionSettled) {
                connectionSettled = true;
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            bool updated = fetch_and_parse_weather();
            if (!updated) weatherRequestFailed = true;
            vTaskDelay(pdMS_TO_TICKS(updated ? WEATHER_INTERVAL_MS : WEATHER_RETRY_MS));
        } else {
            connectionSettled = false;
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
}

static void espnow_send_cb(const esp_now_send_info_t *info, esp_now_send_status_t status)
{
    LV_UNUSED(info);
    LV_UNUSED(status);
}

static void espnow_recv_cb(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    if (!info || !data || !espnowRxQueue ||
        memcmp(info->src_addr, RELAY_ESP32_MAC, 6) != 0 ||
        len != (int)sizeof(RelaySensorStatusPacket)) return;
    EspNowRxItem item;
    memcpy(&item.packet, data, sizeof(item.packet));
    if (item.packet.version != ESPNOW_PROTOCOL_VERSION ||
        item.packet.type != MSG_RELAY_SENSOR_STATUS) return;
    xQueueSend(espnowRxQueue, &item, 0);
}

static bool begin_espnow()
{
    if (espNowReady) return true;
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed");
        return false;
    }
    if (esp_now_register_send_cb(espnow_send_cb) != ESP_OK ||
        esp_now_register_recv_cb(espnow_recv_cb) != ESP_OK) {
        Serial.println("ESP-NOW callback registration failed");
        esp_now_deinit();
        return false;
    }
    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, RELAY_ESP32_MAC, 6);
    peer.channel = WiFi.channel();
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_err_t result = esp_now_add_peer(&peer);
    espNowReady = result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;
    Serial.printf("ESP-NOW peer registration: %s (%d)\n", espNowReady ? "OK" : "FAILED", result);
    if (!espNowReady) esp_now_deinit();
    return espNowReady;
}

static void send_status_request()
{
    if (!espNowReady) return;
    StatusRequestPacket packet = {
        ESPNOW_PROTOCOL_VERSION, MSG_STATUS_REQUEST, 0, nextCommandSequence++
    };
    esp_now_send(RELAY_ESP32_MAC, reinterpret_cast<const uint8_t *>(&packet), sizeof(packet));
}

static bool sequence_at_least(uint32_t received, uint32_t expected)
{
    return (int32_t)(received - expected) >= 0;
}

static void process_espnow_receive_queue()
{
    EspNowRxItem item;
    while (espnowRxQueue && xQueueReceive(espnowRxQueue, &item, 0) == pdTRUE) {
        const RelaySensorStatusPacket &packet = item.packet;
        uint8_t oldMask = confirmedRelayMask;
        uint8_t oldPendingMask = relayPendingMask;
        bool wasKnown = relayStateKnown;
        int8_t sceneBeingConfirmed = pendingScene;
        confirmedRelayMask = packet.relayMask & RELAY_MASK_ALL;
        relayStateKnown = true;
        relayNodeOnline = true;
        relayLastSeenMs = millis();
        update_confirmed_runtime(wasKnown ? oldMask : 0, confirmedRelayMask);

        for (int i = 0; i < RELAY_COUNT; i++) {
            uint8_t bit = 1U << i;
            bool wasPending = (oldPendingMask & bit) != 0;
            bool stateMatches = (requestedRelayMask & bit) == (confirmedRelayMask & bit);
            bool currentAck = wasPending &&
                              sequence_at_least(packet.sequence, relayPendingSequence[i]);
            if (currentAck && stateMatches) {
                relayPendingMask &= ~bit;
                relayPendingSequence[i] = 0;
            }
            if (!wasPending) {
                requestedRelayMask =
                    (requestedRelayMask & ~bit) | (confirmedRelayMask & bit);
            }

            bool manualConfirmation = sceneBeingConfirmed < 0 && currentAck && stateMatches;
            bool externalChange = sceneBeingConfirmed < 0 && !wasPending && wasKnown &&
                                  ((oldMask ^ confirmedRelayMask) & bit);
            if (manualConfirmation || externalChange) {
                bool on = (confirmedRelayMask & bit) != 0;
                push_event(on ? 1 : 0, 0, relay_defs[i].log_name,
                           "Confirmed by relay", current_minute_of_day());
                history_dirty = true;
            }
        }

        if (sceneBeingConfirmed >= 0 &&
            relayPendingMask == 0 &&
            confirmedRelayMask == requestedRelayMask) {
            push_event(2, (uint8_t)sceneBeingConfirmed,
                       scene_defs[sceneBeingConfirmed].name,
                       "Confirmed by relay", current_minute_of_day());
            history_dirty = true;
            pendingScene = -1;
        }

        uint8_t stateChanges = wasKnown ? (oldMask ^ confirmedRelayMask) : RELAY_MASK_ALL;
        uint8_t renderChanges = stateChanges | (oldPendingMask ^ relayPendingMask);
        if (renderChanges != 0) {
            relayUiChangedMask |= renderChanges;
            relayUiDirty = true;
        }

        bool newTempValid = (packet.sensorValidMask & SENSOR_TEMP_VALID) != 0;
        bool newHumidityValid = (packet.sensorValidMask & SENSOR_HUMIDITY_VALID) != 0;
        bool indoorChanged =
            newTempValid != indoorTempValid ||
            newHumidityValid != indoorHumidityValid ||
            (newTempValid && packet.indoorTemperatureCentiC != indoorTempCentiC) ||
            (newHumidityValid &&
             packet.indoorHumidityCentiPercent != indoorHumidityCentiPercent);
        indoorTempValid = newTempValid;
        indoorHumidityValid = newHumidityValid;
        indoorTempCentiC = packet.indoorTemperatureCentiC;
        indoorHumidityCentiPercent = packet.indoorHumidityCentiPercent;
        if (indoorChanged) indoorUiDirty = true;
    }
}

static void process_relay_timeouts()
{
    uint32_t now = millis();
    bool commandTimedOut = false;
    for (int i = 0; i < RELAY_COUNT; i++) {
        uint8_t bit = 1U << i;
        if ((relayPendingMask & bit) &&
            now - relayPendingSince[i] >= RELAY_PENDING_TIMEOUT_MS) {
            commandTimedOut = true;
            break;
        }
    }

    bool statusTimedOut = relayNodeOnline &&
                          now - relayLastSeenMs >= RELAY_OFFLINE_TIMEOUT_MS;
    if (!relayTransportLost && !commandTimedOut && !statusTimedOut) return;

    relayTransportLost = false;
    if (relayStateKnown) update_confirmed_runtime(confirmedRelayMask, 0);
    relayNodeOnline = false;
    relayStateKnown = false;
    relayPendingMask = 0;
    memset(relayPendingSequence, 0, sizeof(relayPendingSequence));
    pendingScene = -1;
    relayUiChangedMask = RELAY_MASK_ALL;
    relayUiDirty = true;
}

/* =========================================================
   TIMERS
   ========================================================= */
static void clock_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    struct tm now;
    if (!local_tm(now)) return;
    if (now.tm_min != lastRenderedClockMinute) {
        lastRenderedClockMinute = now.tm_min;
        refresh_clock_from_tm(now);
        refresh_sun_widgets();
        refresh_runtime();
    }
    if (now.tm_yday != lastRenderedDateDay) {
        lastRenderedDateDay = now.tm_yday;
        refresh_clock_from_tm(now);
        refresh_weather_forecast();
    }
}

static void ui_sync_timer_cb(lv_timer_t *t)
{
    LV_UNUSED(t);
    process_espnow_receive_queue();
    process_relay_timeouts();
    if (relayUiDirty) {
        uint8_t changed = relayUiChangedMask;
        relayUiChangedMask = 0;
        relayUiDirty = false;
        for (int i = 0; i < RELAY_COUNT; i++) {
            if (changed & (1U << i)) refresh_relay_card(i);
        }
        lastRenderedRelayMask = confirmedRelayMask;
        lastRenderedPendingMask = relayPendingMask;
        lastRenderedRelayKnown = relayStateKnown;
        resync_active_scene();
        refresh_scene_rail();
        refresh_runtime();
    }
    if (indoorUiDirty) {
        indoorUiDirty = false;
        refresh_indoor_card();
    }
    if (weatherUiDirty) {
        portENTER_CRITICAL(&weatherMux);
        weatherData = pendingWeatherData;
        weatherUiDirty = false;
        portEXIT_CRITICAL(&weatherMux);
        memcpy(chart_dc, weatherData.hourlyTempDc, sizeof(chart_dc));
        chart_lo_dc = INT16_MAX;
        chart_hi_dc = INT16_MIN;
        for (int i = 0; i < 9; i++) {
            if (chart_dc[i] < chart_lo_dc) chart_lo_dc = chart_dc[i];
            if (chart_dc[i] > chart_hi_dc) chart_hi_dc = chart_dc[i];
        }
        chart_lo_dc -= 20;
        chart_hi_dc += 20;
        weatherEverUpdated = true;
        refresh_current_weather();
        refresh_weather_forecast();
        refresh_weather_chart();
        refresh_sun_widgets();
    }
    if (history_dirty && active_page == 2) refresh_history_rows();
    refresh_connection_status();
}

/* =========================================================
   UI ASSEMBLY
   ========================================================= */
static void create_dashboard_ui()
{
    scr_root = lv_scr_act();
    lv_obj_clear_flag(scr_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(scr_root, 0, 0);
    lv_obj_set_style_text_font(scr_root, F_BODY, 0);
    reg(scr_root, R_SCREEN);

    build_header(scr_root);
    build_nav(scr_root);

    stage = box(scr_root, 772, 352, R_NONE, 0);
    lv_obj_set_pos(stage, 14, 62);

    for (int i = 0; i < 4; i++) {
        pages[i] = box(stage, 772, 352, R_NONE, 0);
        lv_obj_set_pos(pages[i], 0, 0);
    }
    build_home_page(pages[0]);
    build_weather_page(pages[1]);
    build_history_page(pages[2]);
    build_settings_page(pages[3]);

    resync_active_scene();

    apply_theme();
    lv_label_set_text_fmt(brightness_label, "%d%%", settings.brightness);

    set_collapsed(indoor_card, !settings.show_indoor);
    set_collapsed(scene_rail, !settings.show_scenes);
    set_collapsed(header_date, !settings.show_date);

    switch_page(0);

    lv_timer_create(clock_timer_cb, 1000, NULL);
    lv_timer_create(ui_sync_timer_cb, 50, NULL);
    lv_timer_create(power_timer_cb, 500, NULL);
}

/* =========================================================
   ARDUINO ENTRY POINTS
   ========================================================= */
/* A failed GT911 probe is usually just a cold controller that has not finished
   its own reset, so the bus is retried before giving up on touch entirely. */
static bool begin_touch_with_retry(Touch *tp, int attempts)
{
    if (tp == nullptr) return false;
    for (int i = 1; i <= attempts; i++) {
        if (tp->isOverState(Touch::State::BEGIN)) return true;
        if (tp->begin()) {
            if (i > 1) Serial.printf("Touch initialized on attempt %d\n", i);
            return true;
        }
        Serial.printf("Touch init attempt %d/%d failed\n", i, attempts);
        delay(150);
    }
    return false;
}

static void tune_touch_indev()
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (indev->driver && indev->driver->type == LV_INDEV_TYPE_POINTER) {
            indev->driver->scroll_limit = 14;
            indev->driver->scroll_throw = 25;  /* damp the fling so lists settle */
        }
        indev = lv_indev_get_next(indev);
    }
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println("\n==========================================");
    Serial.println("VIEWE UEDX80480070 7.0-inch Smart Panel");
    Serial.println("ESP32-S3 + EK9716/ST7262 + GT911 Touch");
    Serial.println("==========================================");

    Serial.printf("Flash Size: %u MB\n", (unsigned)(ESP.getFlashChipSize() / 1024 / 1024));
    Serial.printf("PSRAM Size: %u MB\n", (unsigned)(ESP.getPsramSize() / 1024 / 1024));
    Serial.printf("Free PSRAM: %u KB\n", (unsigned)(ESP.getFreePsram() / 1024));
    Serial.printf("Free Heap: %u KB\n", (unsigned)(ESP.getFreeHeap() / 1024));
    Serial.printf("Anti-tearing mode: %d\n", LVGL_PORT_AVOID_TEARING_MODE);
#if LVGL_PORT_AVOID_TEARING_MODE
    Serial.printf("RGB framebuffer count: %d\n", LVGL_PORT_DISP_BUFFER_NUM);
#else
    Serial.println("RGB framebuffer count: port default");
#endif
    if (ESP.getPsramSize() == 0) Serial.println("WARNING: PSRAM not detected");

    settings_load();

    Serial.println("Initializing Board...");
    board = new Board();
    board->init();

    auto lcd = board->getLCD();
    if (lcd) {
#if LVGL_PORT_AVOID_TEARING_MODE
        lcd->configFrameBufferNumber(LVGL_PORT_DISP_BUFFER_NUM);
#endif
#if ESP_PANEL_DRIVERS_BUS_ENABLE_RGB && CONFIG_IDF_TARGET_ESP32S3
        auto lcd_bus = lcd->getBus();
        if (lcd_bus && lcd_bus->getBasicAttributes().type == ESP_PANEL_BUS_TYPE_RGB) {
            // Leave enough RGB DMA margin for sustained Wi-Fi/ESP-NOW traffic
            // without consuming the internal heap needed by the Wi-Fi task.
            // The 20-line buffer is the proven network-safe configuration.
            auto rgbBus = static_cast<BusRGB *>(lcd_bus);
            const uint32_t stablePixelClockHz = 20UL * 1000UL * 1000UL;
            const size_t bouncePixels = lcd->getFrameWidth() * 20;
            bool clockConfigured = rgbBus->configRGB_FreqHz(stablePixelClockHz);
            rgbBus->configRGB_BounceBufferSize(bouncePixels);
            Serial.printf("RGB pixel clock: %u MHz (%s)\n",
                          (unsigned)(stablePixelClockHz / 1000000UL),
                          clockConfigured ? "configured" : "configuration failed");
            Serial.printf("RGB bounce buffer: %u pixels (%u RGB565 bytes)\n",
                          (unsigned)bouncePixels, (unsigned)(bouncePixels * 2));
        }
#endif
    }

    bool touch_ok = false;
    if (board->begin()) {
        touch_ok = true;
        Serial.println("Board initialized successfully with Touch!");
    } else {
        Serial.println("Notice: board->begin() returned false; bringing devices up individually...");
        if (board->getLCD() != nullptr) {
            board->getLCD()->begin();
        }
        if (board->getBacklight() != nullptr) {
            board->getBacklight()->begin();
            board->getBacklight()->on();
        }
        touch_ok = begin_touch_with_retry(board->getTouch(), 4);
        Serial.printf("Touch recovery: %s\n", touch_ok ? "OK" : "unavailable");
    }

    if (board->getBacklight() != nullptr) {
        board->getBacklight()->setBrightness(settings.brightness);
    }

#if LVGL_PORT_AVOID_TEARING_MODE
    bool framebuffersReady = lcd != nullptr;
    for (int i = 0; framebuffersReady && i < LVGL_PORT_DISP_BUFFER_NUM; i++) {
        void *framebuffer = lcd->getFrameBufferByIndex(i);
        Serial.printf("RGB framebuffer[%d]: %p\n", i, framebuffer);
        if (framebuffer == nullptr) framebuffersReady = false;
    }
    if (!framebuffersReady) {
        Serial.println("ERROR: RGB framebuffer allocation failed");
        while (1) delay(1000);
    }
#endif

    Serial.println("Initializing LVGL...");
    auto touch_dev = touch_ok ? board->getTouch() : nullptr;
    if (!lvgl_port_init(board->getLCD(), touch_dev)) {
        Serial.println("ERROR: Failed to initialize LVGL port!");
        while (1) {
            delay(1000);
        }
    }
    Serial.println("LVGL initialized successfully!");
    Serial.printf("Free PSRAM after framebuffers: %u KB\n",
                  (unsigned)(ESP.getFreePsram() / 1024));
    Serial.printf("Free heap after LVGL: %u KB\n",
                  (unsigned)(ESP.getFreeHeap() / 1024));

    Serial.println("Creating Dashboard UI...");
    lvgl_port_lock(-1);
    lv_disp_set_rotation(lv_disp_get_default(), LV_DISP_ROT_180);
    Serial.println("Display rotation: 180 degrees");
    tune_touch_indev();
    create_dashboard_ui();
    lvgl_port_unlock();

    Serial.println("Dashboard UI created and active!");

    espnowRxQueue = xQueueCreate(6, sizeof(EspNowRxItem));
    WiFi.mode(WIFI_STA);
    uint8_t displayStaMac[6] = {};
    esp_read_mac(displayStaMac, ESP_MAC_WIFI_STA);
    printMac("Display STA MAC: ", displayStaMac);
    Serial.printf("Wi-Fi channel: %u\n", (unsigned)WiFi.channel());
    Serial.println("Relay peer MAC: C0:5D:89:F5:AD:FC");
    if (DASHBOARD_HAS_SECRETS && WIFI_SSID[0] != '\0') {
        WiFi.setAutoReconnect(true);
        WiFi.setSleep(false);
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        Serial.println("Wi-Fi connection started (nonblocking)");
    } else {
        Serial.println("Wi-Fi credentials absent; running offline");
    }
    BaseType_t weatherTaskResult = xTaskCreatePinnedToCore(
        weather_task, "weather", 12288, nullptr, 1, nullptr, 0);
    if (weatherTaskResult != pdPASS) {
        Serial.println("ERROR: weather task creation failed");
    }
}

void loop()
{
    uint32_t now = millis();
    bool connected = WiFi.status() == WL_CONNECTED;
    wifiConnected = connected;
    if (connected && !wifiWasConnected) {
        wifiWasConnected = true;
        Serial.printf("Wi-Fi connected, channel: %u\n", (unsigned)WiFi.channel());
        uint8_t displayStaMac[6] = {};
        esp_read_mac(displayStaMac, ESP_MAC_WIFI_STA);
        printMac("Display STA MAC: ", displayStaMac);
        if (!ntpConfigured) {
            configTzTime(TIMEZONE_INFO, "pool.ntp.org", "time.nist.gov");
            ntpConfigured = true;
            Serial.println("NTP synchronization configured");
        }
        begin_espnow();
        lastEspNowAttemptMs = now;
        send_status_request();
        lastStatusRequestMs = now;
    } else if (!connected && wifiWasConnected) {
        wifiWasConnected = false;
        espNowReady = false;
        esp_now_deinit();
        relayTransportLost = true;
        Serial.println("Wi-Fi disconnected");
    }

    if (connected && !espNowReady &&
        now - lastEspNowAttemptMs >= STATUS_REQUEST_INTERVAL_MS) {
        lastEspNowAttemptMs = now;
        begin_espnow();
    }
    if (espNowReady && now - lastStatusRequestMs >= STATUS_REQUEST_INTERVAL_MS) {
        send_status_request();
        lastStatusRequestMs = now;
    }

    /* NVS writes happen here, off the LVGL task, so a flash commit can never
       stall rendering or make touch feel unresponsive. */
    if (settings_dirty && millis() - settings_dirty_at > 600) {
        settings_dirty = false;
        settings_save();
        Serial.printf("NVS saved: bright=%u theme=%u unit=%u dim=%u sleep=%u ind=%d date=%d scen=%d\n",
                      settings.brightness, settings.theme, settings.unit, settings.dim_sec,
                      settings.sleep_sec, settings.show_indoor, settings.show_date,
                      settings.show_scenes);
    }

    if (power_state_log >= 0) {
        static const char *names[3] = {"awake", "dimmed", "asleep (backlight off)"};
        Serial.printf("Display %s\n", names[power_state_log]);
        power_state_log = -1;
    }
    delay(20);
}
