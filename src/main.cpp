#include <lvgl.h>
#include <TFT_eSPI.h>
#include "CST820.h" // Include the touch controller header
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <mqtt_client.h>
#include <atomic>
#include <math.h>
#include <WebServer.h>
#include <Preferences.h>
#include "Config.h"
#include "web_pages.h"

#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""
#endif

// Define touch pins (from your notUsed/tftOutput.cpp)
#define I2C_SDA 33
#define I2C_SCL 32
#define TP_RST 25
#define TP_INT 21
static constexpr gpio_num_t HEATER_PIN = GPIO_NUM_26;
#define MAX6675_SCK 18
#define MAX6675_SO 19
#define MAX6675_CS 23

static WebServer server(80);
static lv_obj_t *network_label;
static lv_obj_t *setup_ap_label;
static String current_network_status;
static Preferences settings;
static bool settings_open = false;
static bool ota_password_required = true;
static bool setup_ap = false;
static char setup_ap_name[sizeof("Reflow999")];
static char setup_ap_password[9];
static bool scanning_wifi = false;
static bool restart_pending = false;
static uint32_t restart_at;
static uint32_t wifi_started_at;

struct WifiSettings {
    char ssid[33];
    char password[65];
};

struct MqttSettings {
    char host[128];
    uint16_t port;
    char user[128];
    char password[128];
    char base[128];
};

static WifiSettings wifi_config = {WIFI_SSID, WIFI_PASSWORD};
static MqttSettings mqtt_config = {"", 1883, "", "", "reflowplate"};

struct MqttSnapshot {
    MqttSettings config;
    char payload[384];
};

struct MqttText {
    char payload[384];
};

struct MqttCommand {
    char text[24];
};

static QueueHandle_t mqtt_queue;
static QueueHandle_t mqtt_table_queue;
static QueueHandle_t mqtt_command_queue;
static std::atomic<bool> mqtt_connected(false);
static char mqtt_command_topic[sizeof(MqttSettings::base) + sizeof("/set")];

static void mqtt_event_handler(void *, esp_event_base_t, int32_t event_id, void *event_data)
{
    const auto event = static_cast<esp_mqtt_event_handle_t>(event_data);
    if (event_id == MQTT_EVENT_CONNECTED) {
        mqtt_connected.store(true);
        esp_mqtt_client_subscribe(event->client, mqtt_command_topic, 0);
    } else if (event_id == MQTT_EVENT_DISCONNECTED || event_id == MQTT_EVENT_ERROR) {
        mqtt_connected.store(false);
    // Retained commands would restart heating after every reconnect.
    } else if (event_id == MQTT_EVENT_DATA && !event->retain && mqtt_command_queue &&
               event->topic_len == static_cast<int>(strlen(mqtt_command_topic)) &&
               !strncmp(event->topic, mqtt_command_topic, event->topic_len) &&
               event->data_len == event->total_data_len &&
               event->data_len < static_cast<int>(sizeof(MqttCommand::text))) {
        MqttCommand command = {};
        memcpy(command.text, event->data, event->data_len);
        xQueueSend(mqtt_command_queue, &command, 0);
    }
}

static void mqtt_task(void *)
{
    MqttSnapshot snapshot = {};
    MqttSettings active_config = {};
    esp_mqtt_client_handle_t client = nullptr;
    while (true) {
        if (xQueueReceive(mqtt_queue, &snapshot, portMAX_DELAY) != pdTRUE) continue;
        if (memcmp(&snapshot.config, &active_config, sizeof(active_config)) != 0) {
            if (client) {
                mqtt_connected.store(false);
                esp_mqtt_client_stop(client);
                esp_mqtt_client_destroy(client);
                client = nullptr;
            }
            active_config = snapshot.config;
        }
        if (!client && active_config.host[0] != '\0') {
            esp_mqtt_client_config_t config = {};
            config.host = active_config.host;
            config.port = active_config.port;
            config.transport = MQTT_TRANSPORT_OVER_TCP;
            config.username = active_config.user[0] ? active_config.user : nullptr;
            config.password = active_config.password[0] ? active_config.password : nullptr;
            config.network_timeout_ms = 3000;
            snprintf(mqtt_command_topic, sizeof(mqtt_command_topic), "%s/set", active_config.base);
            client = esp_mqtt_client_init(&config);
            if (client && (esp_mqtt_client_register_event(client, MQTT_EVENT_ANY, mqtt_event_handler, nullptr) != ESP_OK ||
                           esp_mqtt_client_start(client) != ESP_OK)) {
                esp_mqtt_client_destroy(client);
                client = nullptr;
            }
        }
        if (client && mqtt_connected.load()) {
            char topic[sizeof(active_config.base) + sizeof("/status")];
            snprintf(topic, sizeof(topic), "%s/status", active_config.base);
            esp_mqtt_client_publish(client, topic, snapshot.payload, 0, 0, 0);
            MqttText table;
            if (mqtt_table_queue && xQueueReceive(mqtt_table_queue, &table, 0) == pdTRUE) {
                snprintf(topic, sizeof(topic), "%s/table", active_config.base);
                esp_mqtt_client_publish(client, topic, table.payload, 0, 0, 0);
            }
        }
    }
}

static void handle_gpio_set()
{
    const String pin_arg = server.arg("pin");
    const String value = server.arg("value");
    uint8_t pin;
    if (pin_arg == "22") pin = 22;
    else {
        server.send(400, "text/plain", "Ungueltiger GPIO");
        return;
    }

    if (value == "0") digitalWrite(pin, LOW);
    else if (value == "1") digitalWrite(pin, HIGH);
    else {
        server.send(400, "text/plain", "Ungueltiger Pegel");
        return;
    }

    server.sendHeader("Location", "/");
    server.send(303);
}

static void update_network_status(lv_timer_t *timer)
{
    (void)timer;
    const bool connected = WiFi.status() == WL_CONNECTED;
    String status;
    if (connected) status = WiFi.localIP().toString();
    else if (setup_ap) status = "AP: " + String(setup_ap_name);
    else status = T_DISPLAY_WIFI_CONNECTING;

    if (status != current_network_status) {
        current_network_status = status;
        lv_label_set_text(network_label, current_network_status.c_str());
        if (setup_ap && !connected) {
            lv_label_set_text_fmt(setup_ap_label, "PW: %s\n%s/setup",
                                  setup_ap_password, WiFi.softAPIP().toString().c_str());
            lv_obj_clear_flag(setup_ap_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(setup_ap_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

TFT_eSPI tft = TFT_eSPI(); // TFT instance
CST820 touch(I2C_SDA, I2C_SCL, TP_RST, TP_INT); /* Touch instance */

// LVGL display buffer
static lv_disp_draw_buf_t draw_buf;
// Using two buffers for double buffering, allocated from DMA-capable memory
static lv_color_t *buf1;
static lv_color_t *buf2;

// LVGL display flush callback
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    // Use pushImageDMA for better performance, like in the working demoData.cpp
    tft.pushImageDMA(area->x1, area->y1, w, h, (uint16_t *)&color_p->full);
    lv_disp_flush_ready(disp);
}

// LVGL touchpad read callback
static uint32_t touch_rejected;
static lv_point_t touch_last_point = {-1, -1};
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    (void)indev_driver;
    static constexpr int MAX_TOUCH_JUMP = 40;
    static constexpr int MAX_PRESSED_JUMP = 70;
    static bool pressed = false;
    static uint8_t hits = 0;
    static uint8_t misses = 0;
    static int previous_x, previous_y;
    static int anchor_x, anchor_y;
    static lv_point_t point;

    uint8_t gesture;
    uint16_t touchX, touchY;
    bool touched = touch.getTouch(&touchX, &touchY, &gesture) && touchX < 240 && touchY < 320;
    // A finger cannot cross the panel between two reads; such a sample is interference.
    if (touched && pressed && (abs(touchX - anchor_x) > MAX_PRESSED_JUMP || abs(touchY - anchor_y) > MAX_PRESSED_JUMP)) {
        touched = false;
        ++touch_rejected;
    }
    data->point = point;
    if (!touched) {
        hits = 0;
        // A single missed read during a press would otherwise end it with a click.
        if (pressed && ++misses < 2) {
            data->state = LV_INDEV_STATE_PR;
            return;
        }
        pressed = false;
        misses = 0;
        data->state = LV_INDEV_STATE_REL;
        return;
    }
    misses = 0;
    const bool consistent = hits > 0 && abs(touchX - previous_x) <= MAX_TOUCH_JUMP &&
                            abs(touchY - previous_y) <= MAX_TOUCH_JUMP;
    previous_x = touchX;
    previous_y = touchY;
    hits = consistent ? (hits < 255 ? hits + 1 : hits) : 1;
    if (hits < 2) {
        data->state = pressed ? LV_INDEV_STATE_PR : LV_INDEV_STATE_REL;
        return;
    }
    pressed = true;
    anchor_x = touchX;
    anchor_y = touchY;
    // Transform coordinates for landscape mode (rotation 1)
    // Raw touch (X:0-239, Y:0-319) -> Screen (X:0-319, Y:0-239)
    point.x = touchY;
    point.y = 239 - touchX;
    touch_last_point = point;
    data->point = point;
    data->state = LV_INDEV_STATE_PR;
}

static lv_obj_t *actual_temp_label;
static lv_obj_t *target_temp_label;
static lv_obj_t *status_label;
static lv_obj_t *increase_button;
static lv_obj_t *decrease_button;
static lv_obj_t *start_button;
static lv_obj_t *limit_label;
static lv_obj_t *main_page;
static lv_obj_t *limit_page;
static lv_obj_t *system_page;
static lv_obj_t *limit_buttons[4];
static constexpr uint32_t LIMIT_MINUTES[] = {15, 30, 45, 0};
static float actual_temp;
static bool actual_temp_read = false;
static bool actual_temp_valid = false;
static uint32_t last_sample_at;
static int target_temp = 220;
static int draft_temp = 220;
// Continuous heating levelled off near 234 C with 400 W and no insulation.
static constexpr int MAX_TARGET_TEMP = 230;
static constexpr int TARGET_TEMP_STEP = 5;
static constexpr uint32_t SAMPLE_TIMEOUT_MS = 1500;
// 0 means unlimited; deliberately not persisted so every boot starts at 15 minutes.
static uint32_t heat_limit_ms = 15 * 60 * 1000;
static constexpr float MAX_PLATE_TEMP = 260.0f;
static constexpr float SETTLE_NOISE = 0.5f;
// The sensor first responded about 20 s after the heater turned on.
static constexpr uint32_t SETTLE_TIME_MS = 40000;
static constexpr uint32_t MAX_TEST_PULSE_MS = 15000;
static constexpr uint32_t MAX_BULK_MS = 1800000;
static constexpr uint32_t MAX_REHEAT_MS = 120000;
// The sensor needs ~20 s to respond, so an earlier "rise" is noise.
static constexpr uint32_t MIN_REHEAT_MS = 15000;
static constexpr uint32_t MIN_OFF_MS = 20000;
// Regulation uses 5 s means of the 0.5 s readings, taken now, 5 s ago and 10 s ago.
static constexpr size_t SMOOTH_WINDOW = 10;
static constexpr size_t SMOOTH_HISTORY = 3 * SMOOTH_WINDOW;
static constexpr float TREND_STEP = 0.1f;
static constexpr float TREND_SECONDS = SMOOTH_WINDOW * 0.5f;
static float raw_history[SMOOTH_HISTORY];
static size_t raw_count;
static size_t raw_index;
static float smooth_temp;
static float trend_now;
static float trend_before;

// Support points interpolated by target temperature; saved in NVS and adjusted over MQTT.
struct TunePoint {
    int16_t temp;
    float offset; // switch-off point below the target
    float hyst;   // re-heat this far below the switch-off point
    float lag;    // seconds of the final rise still expected after continuous heating; 0 disables it
};

struct TuneTable {
    uint8_t count;
    TunePoint points[8];
};

// Afterheat was several degrees at 80-160 C but under 1 C at 220-230 C; the rise continued about 28 s.
static constexpr TuneTable DEFAULT_TUNE = {3, {{80, 9.0f, 1.0f, 28.0f}, {150, 5.0f, 1.0f, 28.0f}, {220, 1.0f, 1.0f, 28.0f}}};
static TuneTable tune = DEFAULT_TUNE;

static TunePoint tune_at(int temperature)
{
    if (temperature <= tune.points[0].temp) return tune.points[0];
    for (uint8_t index = 1; index < tune.count; ++index) {
        const TunePoint &upper = tune.points[index];
        if (temperature > upper.temp) continue;
        const TunePoint &lower = tune.points[index - 1];
        const float fraction = static_cast<float>(temperature - lower.temp) / (upper.temp - lower.temp);
        return {static_cast<int16_t>(temperature), lower.offset + (upper.offset - lower.offset) * fraction,
                lower.hyst + (upper.hyst - lower.hyst) * fraction, lower.lag + (upper.lag - lower.lag) * fraction};
    }
    return tune.points[tune.count - 1];
}

static bool bulk_done = false;
static uint32_t heater_off_at;
static constexpr uint32_t PROGRESS_DELAY_MS = 15000;
static constexpr uint32_t PROGRESS_INTERVAL_MS = 30000;
static constexpr float PROGRESS_RISE = 1.0f;
// Near 220 C a re-heat only turned the reading around after 45 s and then rose about 0.08 C/s.
static constexpr uint32_t REHEAT_PROGRESS_DELAY_MS = 45000;
static constexpr float REHEAT_PROGRESS_RISE = 0.5f;
// The reading keeps falling for about 20 s after the heater turns on.
static constexpr uint32_t DROP_DELAY_MS = 30000;
static constexpr float DROP_LIMIT = 2.0f;
static bool regulation_running = false;
static bool test_pulse = false;
static uint32_t regulation_started_at;
static uint32_t regulation_elapsed;
static bool heater_on = false;
static uint32_t pulse_started_at;
static uint32_t pulse_ms;
static float drop_peak;
static float progress_temp;
static uint32_t progress_at;
static float settle_peak;
static uint32_t settle_peak_at;
static esp_timer_handle_t heater_watchdog;
static bool heater_watchdog_ready = false;
static portMUX_TYPE heater_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t heater_deadline_us;
static bool heater_expired = false;
static const char *regulation_fault = nullptr;

static void check_heater_deadline(void *)
{
    portENTER_CRITICAL(&heater_lock);
    if (heater_deadline_us != 0 && esp_timer_get_time() >= heater_deadline_us) {
        gpio_set_level(HEATER_PIN, HIGH);
        heater_deadline_us = 0;
        heater_expired = true;
    }
    portEXIT_CRITICAL(&heater_lock);
}

static bool sample_is_fresh()
{
    return actual_temp_valid && millis() - last_sample_at < SAMPLE_TIMEOUT_MS;
}

static float window_mean(size_t windows_back)
{
    float sum = 0.0f;
    for (size_t offset = 0; offset < SMOOTH_WINDOW; ++offset) {
        sum += raw_history[(raw_index + SMOOTH_HISTORY - 1 - windows_back * SMOOTH_WINDOW - offset) % SMOOTH_HISTORY];
    }
    return sum / SMOOTH_WINDOW;
}

static void add_sample(float temperature)
{
    raw_history[raw_index] = temperature;
    raw_index = (raw_index + 1) % SMOOTH_HISTORY;
    if (raw_count < SMOOTH_HISTORY) ++raw_count;
    if (raw_count < SMOOTH_WINDOW) {
        float sum = 0.0f;
        for (size_t offset = 0; offset < raw_count; ++offset) {
            sum += raw_history[(raw_index + SMOOTH_HISTORY - 1 - offset) % SMOOTH_HISTORY];
        }
        smooth_temp = sum / raw_count;
    } else {
        smooth_temp = window_mean(0);
    }
    trend_now = trend_before = 0.0f;
    if (raw_count == SMOOTH_HISTORY) {
        const float before = window_mean(1);
        trend_now = smooth_temp - before;
        trend_before = before - window_mean(2);
    }
}

static bool temperature_rising()
{
    return raw_count == SMOOTH_HISTORY && trend_now >= TREND_STEP && trend_before >= TREND_STEP;
}

static const char *fault_reason()
{
    bool expired;
    portENTER_CRITICAL(&heater_lock);
    expired = heater_expired;
    portEXIT_CRITICAL(&heater_lock);
    if (regulation_fault) return regulation_fault;
    if (expired || !heater_watchdog_ready) return "watchdog";
    if (actual_temp_read && !sample_is_fresh()) return "sensor";
    return nullptr;
}

static const char *plate_state()
{
    if (fault_reason()) return "error";
    if (regulation_running && !test_pulse && bulk_done) return "target";
    if (regulation_running) return "heating";
    if (actual_temp >= 40.0f) return "cooling";
    return "cold";
}

static void update_status_led()
{
    const char *state = plate_state();
    digitalWrite(4, (!strcmp(state, "target") || !strcmp(state, "cooling") || !strcmp(state, "error")) ? LOW : HIGH);
    digitalWrite(16, (!strcmp(state, "heating") || !strcmp(state, "error")) ? LOW : HIGH);
    digitalWrite(17, (!strcmp(state, "cold") || !strcmp(state, "cooling")) ? LOW : HIGH);
}

static float off_threshold()
{
    return target_temp - tune_at(target_temp).offset;
}

static void queue_mqtt_status()
{
    static uint32_t last_queued_at;
    const uint32_t now = millis();
    if (!mqtt_queue || now - last_queued_at < 1000) return;
    last_queued_at = now;
    MqttSnapshot snapshot = {};
    snapshot.config = mqtt_config;
    char actual[16] = "null";
    if (sample_is_fresh()) snprintf(actual, sizeof(actual), "%.2f", actual_temp);
    bool output_on;
    portENTER_CRITICAL(&heater_lock);
    output_on = heater_on && !heater_expired;
    portEXIT_CRITICAL(&heater_lock);
    const char *reason = fault_reason();
    char reason_json[16] = "null";
    if (reason) snprintf(reason_json, sizeof(reason_json), "\"%s\"", reason);
    const TunePoint active = tune_at(target_temp);
    snprintf(snapshot.payload, sizeof(snapshot.payload),
             "{\"uptime_ms\":%lu,\"actual\":%s,\"target\":%d,\"running\":%s,\"heater\":%s,\"state\":\"%s\",\"reason\":%s,\"pulse_ms\":%lu,\"limit_min\":%lu,\"touch_err\":%lu,\"touch_rej\":%lu,\"touch_x\":%d,\"touch_y\":%d,\"off\":%.1f,\"hyst\":%.1f,\"lag\":%.0f,\"smooth\":%.2f,\"trend\":%.2f}",
             static_cast<unsigned long>(now), actual, target_temp, regulation_running ? "true" : "false",
             output_on ? "true" : "false", plate_state(), reason_json, static_cast<unsigned long>(pulse_ms),
             static_cast<unsigned long>(heat_limit_ms / 60000), static_cast<unsigned long>(touch.readErrors()),
             static_cast<unsigned long>(touch_rejected), touch_last_point.x, touch_last_point.y,
             target_temp - active.offset, active.hyst, active.lag, smooth_temp, trend_now);
    xQueueOverwrite(mqtt_queue, &snapshot);
}

static int minimum_target_temp()
{
    if (!sample_is_fresh()) return MAX_TARGET_TEMP + TARGET_TEMP_STEP;
    return static_cast<int>(ceilf(actual_temp / TARGET_TEMP_STEP)) * TARGET_TEMP_STEP;
}

static lv_obj_t *create_panel(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                              lv_coord_t width, lv_coord_t height, lv_color_t color)
{
    lv_obj_t *panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, width, height);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(panel, 9, 0);
    lv_obj_set_style_border_width(panel, 0, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_set_style_bg_color(panel, color, 0);
    return panel;
}

static lv_obj_t *create_label(lv_obj_t *parent, const char *text, const lv_font_t *font,
                              lv_color_t color, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_pos(label, x, y);
    return label;
}

static void update_temperature_labels()
{
    if (!actual_temp_read) lv_label_set_text(actual_temp_label, "--.- \xc2\xb0" "C");
    else if (!actual_temp_valid) lv_label_set_text(actual_temp_label, "Fehler");
    else lv_label_set_text_fmt(actual_temp_label, "%.2f \xc2\xb0" "C", actual_temp);
    lv_label_set_text_fmt(target_temp_label, "%d \xc2\xb0" "C", draft_temp);
    if (regulation_running || draft_temp >= minimum_target_temp()) {
        lv_obj_clear_state(start_button, LV_STATE_DISABLED);
    } else {
        lv_obj_add_state(start_button, LV_STATE_DISABLED);
    }
    if (!limit_label) return;
    char limit_text[48];
    if (heat_limit_ms) snprintf(limit_text, sizeof(limit_text), T_DISPLAY_LIMIT_MIN, static_cast<unsigned long>(heat_limit_ms / 60000));
    else snprintf(limit_text, sizeof(limit_text), "%s", T_DISPLAY_LIMIT_NONE);
    if (regulation_running && heat_limit_ms) {
        const uint32_t elapsed = millis() - regulation_started_at;
        const uint32_t left = elapsed < heat_limit_ms ? (heat_limit_ms - elapsed) / 1000 : 0;
        const size_t used = strlen(limit_text);
        snprintf(limit_text + used, sizeof(limit_text) - used, T_DISPLAY_LIMIT_LEFT,
                 static_cast<unsigned long>(left / 60), static_cast<unsigned long>(left % 60));
    }
    lv_label_set_text(limit_label, limit_text);
}

static void stop_regulation()
{
    portENTER_CRITICAL(&heater_lock);
    gpio_set_level(HEATER_PIN, HIGH);
    heater_deadline_us = 0;
    portEXIT_CRITICAL(&heater_lock);
    if (heater_on) {
        settle_peak = actual_temp;
        settle_peak_at = millis();
    }
    heater_on = false;
    test_pulse = false;
    if (!regulation_running) return;
    regulation_elapsed = millis() - regulation_started_at;
    regulation_running = false;
    lv_label_set_text(status_label, T_DISPLAY_READY);
    lv_label_set_text(lv_obj_get_child(start_button, 0), T_DISPLAY_SET);
    lv_obj_set_style_bg_color(start_button, lv_color_hex(0x168B58), 0);
}

static bool temperature_is_safe(int requested)
{
    return sample_is_fresh() && actual_temp < MAX_PLATE_TEMP && actual_temp < requested + 15.0f;
}

static void fail_regulation(const char *reason, const char *display_text)
{
    regulation_fault = reason;
    stop_regulation();
    lv_label_set_text(status_label, display_text);
}

static void regulate_heater()
{
    if (!regulation_running) return;
    const uint32_t now = millis();
    if (!sample_is_fresh()) {
        fail_regulation("sensor", T_FAULT_SENSOR);
        return;
    }
    if (!temperature_is_safe(test_pulse ? MAX_TARGET_TEMP : target_temp)) {
        fail_regulation("overtemp", T_FAULT_OVERTEMP);
        return;
    }
    if (heat_limit_ms && now - regulation_started_at >= heat_limit_ms) {
        fail_regulation("limit", T_FAULT_LIMIT);
        return;
    }

    bool enable;
    const TunePoint active = tune_at(target_temp);
    const float off_at = target_temp - active.offset;
    if (heater_on) {
        if (now - pulse_started_at >= DROP_DELAY_MS) {
            if (smooth_temp > drop_peak) drop_peak = smooth_temp;
            if (smooth_temp < drop_peak - DROP_LIMIT) {
                fail_regulation("drop", T_FAULT_DROP);
                return;
            }
        }
        // A hot plate keeps cooling at the sensor for ~25 s after switch-on, so measure from the lowest reading.
        if (smooth_temp < progress_temp) progress_temp = smooth_temp;
        if (static_cast<int32_t>(now - progress_at) >= static_cast<int32_t>(PROGRESS_INTERVAL_MS)) {
            if (smooth_temp < progress_temp + (bulk_done ? REHEAT_PROGRESS_RISE : PROGRESS_RISE)) {
                fail_regulation("progress", T_FAULT_PROGRESS);
                return;
            }
            progress_temp = smooth_temp;
            progress_at = now;
        }
        enable = now - pulse_started_at < pulse_ms && (test_pulse || smooth_temp < off_at);
        if (!test_pulse && !bulk_done && active.lag > 0.0f && raw_count == SMOOTH_HISTORY &&
            smooth_temp + (trend_now > 0.0f ? trend_now / TREND_SECONDS : 0.0f) * active.lag >= target_temp) enable = false;
        // The rise arrives after the dead time; once it is above the re-heat point, the plate is caught up.
        if (!test_pulse && bulk_done && now - pulse_started_at >= MIN_REHEAT_MS && temperature_rising() &&
            smooth_temp > off_at - active.hyst) enable = false;
    } else {
        enable = test_pulse || (now - heater_off_at >= MIN_OFF_MS &&
                                smooth_temp <= off_at - (bulk_done ? active.hyst : 0.0f));
    }
    if (!heater_on && enable && !test_pulse) pulse_ms = bulk_done ? MAX_REHEAT_MS : MAX_BULK_MS;
    const int64_t now_us = esp_timer_get_time();
    const uint32_t sample_remaining = SAMPLE_TIMEOUT_MS - (now - last_sample_at);
    const uint32_t session_remaining = heat_limit_ms ? heat_limit_ms - (now - regulation_started_at) : UINT32_MAX;
    const uint32_t remaining = sample_remaining < session_remaining ? sample_remaining : session_remaining;
    portENTER_CRITICAL(&heater_lock);
    const bool expired = heater_expired || (heater_deadline_us != 0 && now_us >= heater_deadline_us);
    if (!expired) {
        heater_deadline_us = now_us + static_cast<int64_t>(remaining) * 1000;
        gpio_set_level(HEATER_PIN, enable ? LOW : HIGH);
    }
    portEXIT_CRITICAL(&heater_lock);
    if (expired) {
        fail_regulation("watchdog", T_FAULT_WATCHDOG);
        return;
    }
    const bool pulse_ended = heater_on && !enable;
    if (!heater_on && enable) {
        pulse_started_at = now;
        progress_temp = smooth_temp;
        progress_at = now + (bulk_done && !test_pulse ? REHEAT_PROGRESS_DELAY_MS : PROGRESS_DELAY_MS);
        drop_peak = -1000.0f;
    } else if (pulse_ended) {
        settle_peak = actual_temp;
        settle_peak_at = now;
        heater_off_at = now;
        if (!test_pulse) bulk_done = true;
    }
    heater_on = enable;
    if (test_pulse && pulse_ended) {
        stop_regulation();
        return;
    }
    lv_label_set_text(status_label, (test_pulse || !bulk_done) ? T_DISPLAY_HEATING : T_DISPLAY_READY);
}

static bool start_regulation(int requested)
{
    if (test_pulse || !heater_watchdog_ready || restart_pending || requested < minimum_target_temp() ||
        requested > MAX_TARGET_TEMP || !temperature_is_safe(requested)) return false;
    if (!regulation_running) {
        portENTER_CRITICAL(&heater_lock);
        heater_expired = false;
        portEXIT_CRITICAL(&heater_lock);
        regulation_started_at = millis();
        regulation_elapsed = 0;
        regulation_running = true;
        pulse_ms = 0;
        bulk_done = false;
        heater_off_at = regulation_started_at - MIN_OFF_MS;
    }
    target_temp = requested;
    draft_temp = requested;
    regulation_fault = nullptr;
    lv_label_set_text(lv_obj_get_child(start_button, 0), T_PLATE_STOP);
    lv_obj_set_style_bg_color(start_button, lv_color_hex(0x9E3D36), 0);
    regulate_heater();
    return regulation_running;
}

static bool start_test_pulse(uint32_t length_ms)
{
    if (regulation_running || !heater_watchdog_ready || restart_pending || !temperature_is_safe(MAX_TARGET_TEMP) ||
        actual_temp >= MAX_TARGET_TEMP || millis() - settle_peak_at < SETTLE_TIME_MS) return false;
    portENTER_CRITICAL(&heater_lock);
    heater_expired = false;
    portEXIT_CRITICAL(&heater_lock);
    regulation_started_at = millis();
    regulation_elapsed = 0;
    regulation_running = true;
    test_pulse = true;
    regulation_fault = nullptr;
    pulse_ms = length_ms;
    lv_label_set_text(lv_obj_get_child(start_button, 0), T_PLATE_STOP);
    lv_obj_set_style_bg_color(start_button, lv_color_hex(0x9E3D36), 0);
    regulate_heater();
    return regulation_running;
}

static void publish_tune_table()
{
    MqttText table = {};
    size_t used = snprintf(table.payload, sizeof(table.payload), "{\"points\":[");
    for (uint8_t index = 0; index < tune.count && used < sizeof(table.payload); ++index) {
        const TunePoint &point = tune.points[index];
        used += snprintf(table.payload + used, sizeof(table.payload) - used,
                         "%s{\"temp\":%d,\"off\":%.2f,\"hyst\":%.2f,\"lag\":%.1f}", index ? "," : "",
                         point.temp, point.offset, point.hyst, point.lag);
    }
    if (used < sizeof(table.payload)) snprintf(table.payload + used, sizeof(table.payload) - used, "]}");
    if (mqtt_table_queue) xQueueOverwrite(mqtt_table_queue, &table);
}

// Accepts "set <temp> <off|hyst|lag> <value>", "del <temp>", "table" and "table reset".
static void handle_tune_command(const String &text)
{
    TuneTable updated = tune;
    if (text == "table reset") {
        updated = DEFAULT_TUNE;
    } else if (text.startsWith("set ") || text.startsWith("del ")) {
        char name[8] = "";
        int temperature = 0;
        float value = 0.0f;
        int consumed = 0;
        const bool set = text.startsWith("set ");
        const int fields = set ? sscanf(text.c_str() + 4, "%d %7s %f%n", &temperature, name, &value, &consumed)
                               : sscanf(text.c_str() + 4, "%d%n", &temperature, &consumed);
        if (fields != (set ? 3 : 1) || consumed != static_cast<int>(text.length()) - 4 ||
            temperature < 0 || temperature > MAX_TARGET_TEMP) return;
        uint8_t index = 0;
        while (index < updated.count && updated.points[index].temp < temperature) ++index;
        const bool exists = index < updated.count && updated.points[index].temp == temperature;
        if (!set) {
            if (!exists || updated.count <= 1) return;
            memmove(&updated.points[index], &updated.points[index + 1], (updated.count - index - 1) * sizeof(TunePoint));
            --updated.count;
        } else {
            float TunePoint::*field = nullptr;
            float minimum = 0.0f, maximum = 0.0f;
            if (!strcmp(name, "off")) {
                field = &TunePoint::offset;
                maximum = 30.0f;
            } else if (!strcmp(name, "hyst")) {
                field = &TunePoint::hyst;
                minimum = 0.25f;
                maximum = 10.0f;
            } else if (!strcmp(name, "lag")) {
                field = &TunePoint::lag;
                maximum = 60.0f;
            }
            if (!field || !(value >= minimum && value <= maximum)) return;
            if (!exists) {
                if (updated.count >= sizeof(updated.points) / sizeof(updated.points[0])) return;
                const TunePoint start = tune_at(temperature);
                memmove(&updated.points[index + 1], &updated.points[index], (updated.count - index) * sizeof(TunePoint));
                updated.points[index] = start;
                updated.points[index].temp = temperature;
                ++updated.count;
            }
            updated.points[index].*field = value;
        }
    } else if (text != "table") {
        return;
    }
    if (memcmp(&updated, &tune, sizeof(tune)) != 0) {
        if (!settings_open || settings.putBytes("tune", &updated, sizeof(updated)) != sizeof(updated)) return;
        tune = updated;
    }
    publish_tune_table();
}

static void handle_mqtt_command()
{
    MqttCommand command;
    if (!mqtt_command_queue || xQueueReceive(mqtt_command_queue, &command, 0) != pdTRUE) return;
    String text(command.text);
    text.trim();
    if (text == "stop") {
        stop_regulation();
    } else if (text.startsWith("pulse ")) {
        const String seconds_arg = text.substring(6);
        const long seconds = seconds_arg.toInt();
        if (seconds_arg == String(seconds) && seconds >= 1 && seconds <= static_cast<long>(MAX_TEST_PULSE_MS / 1000)) {
            start_test_pulse(static_cast<uint32_t>(seconds) * 1000);
        }
    } else if (text.startsWith("set ") || text.startsWith("del ") || text.startsWith("table")) {
        handle_tune_command(text);
    } else {
        const long temperature = text.toInt();
        if (text == String(temperature) && temperature % TARGET_TEMP_STEP == 0) start_regulation(temperature);
    }
    update_temperature_labels();
}

static void control_event_cb(lv_event_t *event)
{
    if (lv_event_get_code(event) != LV_EVENT_CLICKED) return;

    lv_obj_t *button = lv_event_get_target(event);
    const int minimum = minimum_target_temp();
    if (!regulation_running && minimum <= MAX_TARGET_TEMP) {
        if (button == increase_button && draft_temp < MAX_TARGET_TEMP) {
            draft_temp = draft_temp < minimum ? minimum : draft_temp + TARGET_TEMP_STEP;
        }
        if (button == decrease_button && draft_temp > minimum) draft_temp -= TARGET_TEMP_STEP;
    }
    if (button == start_button) {
        if (regulation_running) stop_regulation();
        else lv_label_set_text(status_label, T_DISPLAY_HOLD_TO_START);
        return;
    }
    update_temperature_labels();
}

static void start_long_press_cb(lv_event_t *event)
{
    (void)event;
    if (regulation_running) return;
    start_regulation(draft_temp);
    // Without this, releasing the finger would send a click that stops the new session.
    lv_indev_wait_release(lv_indev_get_act());
    update_temperature_labels();
}

static void page_button_cb(lv_event_t *event)
{
    lv_obj_t *target = static_cast<lv_obj_t *>(lv_event_get_user_data(event));
    for (lv_obj_t *page : {main_page, limit_page, system_page}) lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(target, LV_OBJ_FLAG_HIDDEN);
}

static void create_page_button(lv_obj_t *page, lv_coord_t x, const char *symbol, lv_obj_t *target)
{
    lv_obj_t *button = lv_btn_create(page);
    lv_obj_set_pos(button, x, 0);
    lv_obj_set_size(button, 52, 34);
    lv_obj_set_style_radius(button, 6, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x174A91), 0);
    lv_obj_add_event_cb(button, page_button_cb, LV_EVENT_CLICKED, target);
    lv_obj_t *label = create_label(button, symbol, &lv_font_montserrat_16, lv_color_white(), 0, 0);
    lv_obj_center(label);
}

static void read_max6675(lv_timer_t *timer)
{
    (void)timer;

    uint16_t reading = 0;
    digitalWrite(MAX6675_CS, LOW);
    delayMicroseconds(2);
    for (uint8_t bit = 0; bit < 16; ++bit) {
        digitalWrite(MAX6675_SCK, HIGH);
        delayMicroseconds(1);
        reading = (reading << 1) | digitalRead(MAX6675_SO);
        digitalWrite(MAX6675_SCK, LOW);
        delayMicroseconds(1);
    }
    digitalWrite(MAX6675_CS, HIGH);

    actual_temp_read = true;
    const uint16_t temperature_bits = (reading >> 3) & 0x0fff;
    actual_temp_valid = (reading & 0x8006) == 0 && temperature_bits != 0;
    if (actual_temp_valid) {
        actual_temp = temperature_bits * 0.25f;
        last_sample_at = millis();
        add_sample(actual_temp);
        if (!heater_on && actual_temp >= settle_peak + SETTLE_NOISE) {
            settle_peak = actual_temp;
            settle_peak_at = last_sample_at;
        }
    } else {
        raw_count = 0;
    }
    regulate_heater();
    update_temperature_labels();
}

static lv_obj_t *create_action_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                                      lv_coord_t height, const char *text, const lv_font_t *font,
                                      lv_color_t color)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 116, height);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, control_event_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = create_label(button, text, font, lv_color_white(), 0, 0);
    lv_obj_set_width(label, 116);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return button;
}

static void update_limit_buttons()
{
    for (size_t index = 0; index < 4; ++index) {
        const bool selected = LIMIT_MINUTES[index] * 60000 == heat_limit_ms;
        lv_color_t color = lv_color_hex(0x174A91);
        if (selected) color = LIMIT_MINUTES[index] ? lv_color_hex(0x168B58) : lv_color_hex(0x9E3D36);
        lv_obj_set_style_bg_color(limit_buttons[index], color, 0);
    }
    update_temperature_labels();
}

static void limit_event_cb(lv_event_t *event)
{
    heat_limit_ms = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event))) * 60000;
    update_limit_buttons();
}

static lv_obj_t *create_limit_button(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, const char *text, uint32_t minutes)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, 148, 54);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_add_event_cb(button, limit_event_cb, minutes ? LV_EVENT_CLICKED : LV_EVENT_LONG_PRESSED,
                        reinterpret_cast<void *>(static_cast<uintptr_t>(minutes)));
    lv_obj_t *label = create_label(button, text, minutes ? &lv_font_montserrat_24 : &lv_font_montserrat_16,
                                   lv_color_white(), 0, 0);
    lv_obj_center(label);
    return button;
}

static lv_obj_t *ota_button;
static lv_obj_t *wifi_reset_button;

static void update_ota_button()
{
    lv_label_set_text(lv_obj_get_child(ota_button, 0),
                      ota_password_required ? T_DISPLAY_OTA_PASS_ON : T_DISPLAY_OTA_PASS_OFF);
    lv_obj_set_style_bg_color(ota_button, ota_password_required ? lv_color_hex(0x168B58) : lv_color_hex(0x9E3D36), 0);
}

static bool set_ota_password_required(bool required)
{
    if (!settings_open || settings.putBool("ota_pw", required) != 1) return false;
    ota_password_required = required;
    if (ota_button) update_ota_button();
    return true;
}

static bool save_wifi(const WifiSettings &updated)
{
    stop_regulation();
    if (!settings_open || settings.putBytes("wifi", &updated, sizeof(updated)) != sizeof(updated)) return false;
    restart_at = millis();
    restart_pending = true;
    return true;
}

static void system_event_cb(lv_event_t *event)
{
    if (restart_pending) return;
    if (lv_event_get_target(event) == ota_button) {
        set_ota_password_required(!ota_password_required);
    } else if (save_wifi(WifiSettings{})) {
        lv_label_set_text(lv_obj_get_child(wifi_reset_button, 0), T_DISPLAY_RESTARTING);
    }
}

static lv_obj_t *create_system_button(lv_coord_t y, const char *text)
{
    lv_obj_t *button = lv_btn_create(system_page);
    lv_obj_set_pos(button, 8, y);
    lv_obj_set_size(button, 304, 54);
    lv_obj_set_style_radius(button, 9, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(0x174A91), 0);
    lv_obj_add_event_cb(button, system_event_cb, LV_EVENT_LONG_PRESSED, nullptr);
    lv_obj_t *label = create_label(button, text, &lv_font_montserrat_16, lv_color_white(), 0, 0);
    lv_obj_center(label);
    return button;
}

void handleSetup() {
    server.send(200, "text/html", SETUP_HTML);
}

static bool web_authorized()
{
    return !ota_password_required || OTA_PASSWORD[0] == '\0' || server.authenticate("ota", OTA_PASSWORD);
}

static bool web_control_available()
{
    return WiFi.status() == WL_CONNECTED && (!ota_password_required || OTA_PASSWORD[0] != '\0');
}

void handle_main_page() {
    if (!web_authorized()) {
        server.requestAuthentication(BASIC_AUTH, "ReflowPlate");
        return;
    }
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "text/html", INDEX_HTML);
}

static void handle_plate_status()
{
    const uint32_t elapsed = regulation_running ? millis() - regulation_started_at : regulation_elapsed;
    char actual[16] = "null";
    if (sample_is_fresh()) snprintf(actual, sizeof(actual), "%.2f", actual_temp);
    char json[256];
    snprintf(json, sizeof(json),
             "{\"actual\":%s,\"target\":%d,\"running\":%s,\"otaOpen\":%s,\"canSet\":%s,\"elapsed\":%lu,\"limit\":%lu,\"min\":%d,\"max\":%d,\"step\":%d}",
             actual, target_temp, regulation_running ? "true" : "false", ota_password_required ? "false" : "true",
             web_control_available() && heater_watchdog_ready &&
             !restart_pending && minimum_target_temp() <= MAX_TARGET_TEMP ? "true" : "false",
             static_cast<unsigned long>(elapsed / 1000), static_cast<unsigned long>(heat_limit_ms / 60000),
             minimum_target_temp(), MAX_TARGET_TEMP, TARGET_TEMP_STEP);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

// Unlimited heating is deliberately only selectable on the device.
static void handle_plate_limit()
{
    if (server.header("X-Reflow-Control") != "1") {
        server.send(403);
        return;
    }
    if (!web_authorized()) {
        server.requestAuthentication(BASIC_AUTH, "ReflowPlate");
        return;
    }
    const String minutes_arg = server.arg("minutes");
    const long minutes = minutes_arg.toInt();
    if (minutes_arg != String(minutes) || (minutes != 15 && minutes != 30 && minutes != 45)) {
        server.send(400);
        return;
    }
    heat_limit_ms = static_cast<uint32_t>(minutes) * 60000;
    update_limit_buttons();
    handle_plate_status();
}

static void handle_plate_set()
{
    if (!web_control_available() || server.header("X-Reflow-Control") != "1") {
        server.send(403);
        return;
    }
    if (!web_authorized()) {
        server.requestAuthentication(BASIC_AUTH, "ReflowPlate");
        return;
    }
    const String requested = server.arg("target");
    const long temperature = requested.toInt();
    if (temperature <= 0 || temperature > MAX_TARGET_TEMP ||
        temperature % TARGET_TEMP_STEP != 0 || requested != String(temperature)) {
        server.send(400);
        return;
    }
    if (!start_regulation(temperature)) {
        server.send(409);
        return;
    }
    update_temperature_labels();
    handle_plate_status();
}

static String json_string(const String &value)
{
    String result = "\"";
    for (size_t index = 0; index < value.length(); ++index) {
        const unsigned char character = value[index];
        if (character < 0x20) {
            char escaped[7];
            snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(character));
            result += escaped;
        } else {
            if (character == '"' || character == '\\') result += '\\';
            result += static_cast<char>(character);
        }
    }
    result += '"';
    return result;
}

static void send_json(const String &body, int status = 200)
{
    server.sendHeader("Cache-Control", "no-store");
    server.send(status, "application/json", body);
}

static void handle_wifi_scan()
{
    int16_t count = WiFi.scanComplete();
    if (!scanning_wifi) {
        count = WiFi.scanNetworks(true);
        scanning_wifi = true;
    }
    if (count == WIFI_SCAN_RUNNING) {
        send_json("null", 202);
        return;
    }
    scanning_wifi = false;
    if (count < 0) {
        WiFi.scanDelete();
        send_json("null", 503);
        return;
    }
    String result = "[";
    bool first = true;
    for (int16_t index = 0; index < count; ++index) {
        const String ssid = WiFi.SSID(index);
        if (ssid.isEmpty()) continue;
        if (!first) result += ',';
        first = false;
        result += "{\"ssid\":" + json_string(ssid) + ",\"rssi\":" + String(WiFi.RSSI(index)) + "}";
    }
    WiFi.scanDelete();
    send_json(result + ']');
}

static void store_wifi(const WifiSettings &updated)
{
    server.send(save_wifi(updated) ? 204 : 500);
}

static void handle_ota_set()
{
    if (server.header("X-Reflow-Control") != "1") {
        server.send(403);
        return;
    }
    if (!web_authorized()) {
        server.requestAuthentication(BASIC_AUTH, "ReflowPlate");
        return;
    }
    const String open = server.arg("open");
    if (open != "0" && open != "1") {
        server.send(400);
        return;
    }
    server.send(set_ota_password_required(open == "0") ? 204 : 500);
}

static void handle_wifi_save()
{
    const String ssid = server.arg("ssid");
    const String password = server.arg("pass");
    if (ssid.isEmpty() || ssid.length() > 32 || password.length() > 64 ||
        (!password.isEmpty() && password.length() < 8)) {
        server.send(400);
        return;
    }
    WifiSettings updated = {};
    ssid.toCharArray(updated.ssid, sizeof(updated.ssid));
    password.toCharArray(updated.password, sizeof(updated.password));
    store_wifi(updated);
}

static void handle_mqtt_config()
{
    String result = "{\"host\":";
    result += json_string(mqtt_config.host);
    result += ",\"port\":" + String(mqtt_config.port);
    result += ",\"user\":" + json_string(mqtt_config.user);
    result += ",\"base\":" + json_string(mqtt_config.base) + "}";
    send_json(result);
}

static void handle_mqtt_save()
{
    const String host = server.arg("host");
    const String port_arg = server.arg("port");
    const String user = server.arg("user");
    const String password = server.arg("pass");
    const String base = server.arg("base");
    const long port = port_arg.toInt();
    MqttSettings updated = mqtt_config;
    if (host.length() >= sizeof(updated.host) || user.length() >= sizeof(updated.user) ||
        password.length() >= sizeof(updated.password) || base.length() >= sizeof(updated.base) ||
        base.isEmpty() || base.indexOf('#') >= 0 || base.indexOf('+') >= 0 ||
        port < 1 || port > 65535 || port_arg != String(port)) {
        server.send(400);
        return;
    }
    host.toCharArray(updated.host, sizeof(updated.host));
    user.toCharArray(updated.user, sizeof(updated.user));
    base.toCharArray(updated.base, sizeof(updated.base));
    if (server.arg("clearPass") == "1") updated.password[0] = '\0';
    else if (!password.isEmpty()) password.toCharArray(updated.password, sizeof(updated.password));
    updated.port = port;
    if (!settings_open || settings.putBytes("mqtt", &updated, sizeof(updated)) != sizeof(updated)) {
        server.send(500);
        return;
    }
    mqtt_config = updated;
    server.send(204);
}

static void start_setup_ap()
{
    WiFi.mode(WIFI_AP_STA);
    snprintf(setup_ap_name, sizeof(setup_ap_name), "Reflow%u",
             static_cast<unsigned>(esp_random() % 999 + 1));
    static constexpr char alphabet[] = "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ";
    static constexpr unsigned alphabet_size = sizeof(alphabet) - 1;
    static constexpr unsigned sample_limit = (256 / alphabet_size) * alphabet_size;
    for (size_t index = 0; index < sizeof(setup_ap_password) - 1; ++index) {
        unsigned sample;
        do {
            sample = esp_random() & 0xff;
        } while (sample >= sample_limit);
        setup_ap_password[index] = alphabet[sample % alphabet_size];
    }
    setup_ap_password[sizeof(setup_ap_password) - 1] = '\0';
    setup_ap = WiFi.softAP(setup_ap_name, setup_ap_password);
    if (setup_ap) Serial.printf("Setup AP: %s / %s / http://%s/setup\n", setup_ap_name, setup_ap_password,
                                WiFi.softAPIP().toString().c_str());
}

static void handle_ota()
{
    static bool started = false;
    if (OTA_PASSWORD[0] == '\0' || WiFi.status() != WL_CONNECTED) return;

    if (!started) {
        ArduinoOTA.setHostname("reflowplate");
        ArduinoOTA.setPassword(OTA_PASSWORD);
        ArduinoOTA.onStart(stop_regulation);
        ArduinoOTA.begin();
        started = true;
        Serial.printf("OTA: reflowplate.local (%s)\n", WiFi.localIP().toString().c_str());
    }
    ArduinoOTA.handle();
}

static bool web_update_writing = false;
static bool web_update_received = false;
static const char *web_update_error = nullptr;

static bool web_update_available()
{
    return web_control_available();
}

static bool web_update_authorized()
{
    return web_update_available() && web_authorized();
}

static bool web_update_binary_request()
{
    return server.header("X-Firmware-Upload") == "1" &&
           server.header("Content-Type") == "application/octet-stream";
}

static void handle_web_update_data()
{
    if (!web_update_binary_request()) return;
    const HTTPRaw &chunk = server.raw();
    switch (chunk.status) {
    case RAW_START:
        web_update_received = false;
        web_update_error = nullptr;
        if (!web_update_authorized() || restart_pending) return;
        stop_regulation();
        if (server.clientContentLength() <= 0) {
            web_update_error = "Firmware fehlt.";
        } else if (Update.begin(static_cast<size_t>(server.clientContentLength()), U_FLASH)) {
            web_update_writing = true;
        } else {
            web_update_error = Update.errorString();
        }
        break;
    case RAW_WRITE:
        if (web_update_writing && Update.write(server.raw().buf, chunk.currentSize) != chunk.currentSize) {
            web_update_error = Update.errorString();
            if (Update.isRunning()) Update.abort();
            web_update_writing = false;
        }
        break;
    case RAW_END:
        web_update_received = web_update_writing;
        break;
    case RAW_ABORTED:
        if (web_update_writing) Update.abort();
        web_update_writing = false;
        web_update_received = false;
        break;
    }
}

static void handle_web_update()
{
    server.sendHeader("Cache-Control", "no-store");
    if (!web_update_available()) {
        server.send(404, "text/plain", "OTA nicht verfuegbar.");
    } else if (!web_update_authorized()) {
        server.requestAuthentication(BASIC_AUTH, "ReflowPlate OTA", "Anmeldung erforderlich.");
    } else if (server.method() == HTTP_GET) {
        server.send_P(200, "text/html; charset=utf-8", OTA_PAGE);
    } else if (restart_pending) {
        server.send(409, "text/plain", "Neustart steht bereits aus.");
    } else if (!web_update_binary_request()) {
        server.send(415, "text/plain", "Firmware-Upload erwartet.");
    } else if (web_update_received && Update.end()) {
        server.send(200, "text/plain", "Update erfolgreich. Das Geraet startet neu.");
        restart_at = millis();
        restart_pending = true;
    } else {
        const char *message = web_update_error ? web_update_error :
                              (web_update_received ? Update.errorString() : "Firmware fehlt oder ist unvollstaendig.");
        server.send(400, "text/plain", message);
    }

    if (server.method() == HTTP_POST) {
        if (web_update_writing && Update.isRunning()) Update.abort();
        web_update_writing = false;
        web_update_received = false;
        web_update_error = nullptr;
    }
}

void web_server_setup(){
    const char *update_headers[] = {"Content-Type", "X-Firmware-Upload", "X-Reflow-Control"};
    server.collectHeaders(update_headers, sizeof(update_headers) / sizeof(update_headers[0]));
    server.on("/update", HTTP_GET, handle_web_update);
    server.on("/update", HTTP_POST, handle_web_update, handle_web_update_data);
    server.on("/", HTTP_GET, handle_main_page);
    server.on("/ui.css", HTTP_GET, []() { server.send_P(200, "text/css", UI_CSS); });
    server.on("/api/plate", HTTP_GET, handle_plate_status);
    server.on("/api/plate", HTTP_POST, handle_plate_set);
    server.on("/api/plate/limit", HTTP_POST, handle_plate_limit);
    server.on("/api/plate/stop", HTTP_POST, []() {
        stop_regulation();
        handle_plate_status();
    });
    server.on("/api/ota", HTTP_POST, handle_ota_set);
    server.on("/gpio", HTTP_POST, handle_gpio_set);
    server.on("/setup", HTTP_GET, handleSetup);
    server.on("/api/status", HTTP_GET, []() {
        String result = setup_ap ? "{\"ap_mode\":true" : "{\"ap_mode\":false";
        result += ",\"ssid\":";
        result += json_string(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String());
        send_json(result + '}');
    });
    server.on("/api/wifi/scan", HTTP_GET, handle_wifi_scan);
    server.on("/api/wifi/save", HTTP_POST, handle_wifi_save);
    server.on("/api/wifi/reset", HTTP_POST, []() { store_wifi(WifiSettings{}); });
    server.on("/api/mqtt/config", HTTP_GET, handle_mqtt_config);
    server.on("/api/mqtt/config", HTTP_POST, handle_mqtt_save);
    server.begin();
}

void setup() {
    gpio_set_level(HEATER_PIN, HIGH);
    pinMode(HEATER_PIN, OUTPUT);
    gpio_set_level(HEATER_PIN, HIGH);
    esp_timer_create_args_t watchdog_args = {};
    watchdog_args.callback = check_heater_deadline;
    watchdog_args.name = "heater_guard";
    heater_watchdog_ready = esp_timer_create(&watchdog_args, &heater_watchdog) == ESP_OK &&
                            esp_timer_start_periodic(heater_watchdog, 100000) == ESP_OK;
    Serial.begin(115200);

    for (uint8_t pin : {17, 4, 16}) {
        digitalWrite(pin, HIGH);
        pinMode(pin, OUTPUT);
    }
    for (uint8_t pin : {22}) {
        digitalWrite(pin, HIGH);
        pinMode(pin, OUTPUT);
    }
    pinMode(MAX6675_SCK, OUTPUT);
    digitalWrite(MAX6675_SCK, LOW);
    pinMode(MAX6675_SO, INPUT_PULLUP);
    pinMode(MAX6675_CS, OUTPUT);
    digitalWrite(MAX6675_CS, HIGH);
    settings_open = settings.begin("reflowplate");
    if (settings_open) {
        if (settings.getBytesLength("wifi") == sizeof(wifi_config)) settings.getBytes("wifi", &wifi_config, sizeof(wifi_config));
        if (settings.getBytesLength("mqtt") == sizeof(mqtt_config)) settings.getBytes("mqtt", &mqtt_config, sizeof(mqtt_config));
        ota_password_required = settings.getBool("ota_pw", true);
        TuneTable stored;
        if (settings.getBytesLength("tune") == sizeof(stored) &&
            settings.getBytes("tune", &stored, sizeof(stored)) == sizeof(stored) &&
            stored.count >= 1 && stored.count <= sizeof(stored.points) / sizeof(stored.points[0])) tune = stored;
    }
    WiFi.persistent(false);
    if (wifi_config.ssid[0] != '\0') {
        WiFi.mode(WIFI_STA);
        WiFi.begin(wifi_config.ssid, wifi_config.password);
        wifi_started_at = millis();
    } else {
        start_setup_ap();
    }
    web_server_setup();

    lv_init();

    // Turn on the backlight
    // To turn off the backlight, use digitalWrite(27, LOW);
    // You can call this function at any point in your code (e.g., after a delay,
    // based on user input, or after a period of inactivity) to switch off the screen.
    // For example: digitalWrite(27, LOW);
    pinMode(27, OUTPUT);
    digitalWrite(27, HIGH);

    tft.begin();
    tft.setRotation(1);
    tft.initDMA(); // Initialize DMA for faster display operations

    // Initialize LVGL display buffer
    // Allocate LVGL display buffers using DMA-capable memory
    // A good starting point for buffer size is screenWidth * screenHeight / 20 for each buffer
    // 240 * 320 / 20 = 3840 pixels per buffer
    buf1 = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * 3840, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    buf2 = (lv_color_t *)heap_caps_malloc(sizeof(lv_color_t) * 3840, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);

    if (!buf1 || !buf2) {
        Serial.println("LVGL buffer allocation failed! Halting.");
        while (1); // Halt if memory allocation fails
    }
    lv_disp_draw_buf_init(&draw_buf, buf1, buf2, 3840); // Use both buffers for double buffering

    // Register display driver to LVGL
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = 320; // Set to landscape width
    disp_drv.ver_res = 240; // Set to landscape height
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // Initialize and register touchpad driver
    touch.begin(); // Initialize the touch controller
    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    indev_drv.long_press_time = 800;
    lv_indev_drv_register(&indev_drv);

    lv_obj_t *screen = lv_scr_act();
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    for (lv_obj_t **page : {&main_page, &limit_page, &system_page}) {
        *page = lv_obj_create(screen);
        lv_obj_set_pos(*page, 0, 0);
        lv_obj_set_size(*page, 320, 240);
        lv_obj_clear_flag(*page, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_opa(*page, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(*page, 0, 0);
        lv_obj_set_style_radius(*page, 0, 0);
        lv_obj_set_style_pad_all(*page, 0, 0);
    }
    lv_obj_add_flag(limit_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(system_page, LV_OBJ_FLAG_HIDDEN);
    create_page_button(main_page, 204, LV_SYMBOL_WIFI, system_page);
    create_page_button(main_page, 264, LV_SYMBOL_SETTINGS, limit_page);
    create_page_button(limit_page, 264, LV_SYMBOL_HOME, main_page);
    create_page_button(system_page, 264, LV_SYMBOL_HOME, main_page);

    network_label = create_label(screen, T_DISPLAY_WIFI_CONNECTING, &lv_font_montserrat_12,
                                 lv_color_hex(0xDDE7EE), 8, 10);
    lv_obj_set_width(network_label, 190);
    lv_obj_set_style_text_align(network_label, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t *actual_panel = create_panel(main_page, 8, 42, 180, 76, lv_color_hex(0x1B2B3D));
    create_label(actual_panel, T_DISPLAY_ACTUAL, &lv_font_montserrat_14,
                 lv_color_hex(0xB9CAD8), 10, 3);
    actual_temp_label = create_label(actual_panel, "", &lv_font_montserrat_32,
                                     lv_color_white(), 10, 19);
#ifdef T_DISPLAY_ACTUAL_DETAIL
    create_label(actual_panel, T_DISPLAY_ACTUAL_DETAIL, &lv_font_montserrat_12,
#else
    create_label(actual_panel, T_PLATE_ACTUAL, &lv_font_montserrat_12,
#endif
                 lv_color_hex(0xB9CAD8), 10, 58);

    lv_obj_t *target_panel = create_panel(main_page, 8, 124, 180, 66, lv_color_hex(0x1B2B3D));
    create_label(target_panel, T_DISPLAY_TARGET, &lv_font_montserrat_14,
                 lv_color_hex(0xB9CAD8), 10, 2);
    target_temp_label = create_label(target_panel, "", &lv_font_montserrat_24,
                                     lv_color_white(), 38, 18);
    create_label(target_panel, T_PLATE_TARGET, &lv_font_montserrat_12,
                 lv_color_hex(0xB9CAD8), 10, 47);

    increase_button = create_action_button(main_page, 196, 42, 50, "+", &lv_font_montserrat_48,
                                           lv_color_hex(0x174A91));
    decrease_button = create_action_button(main_page, 196, 108, 50, "-", &lv_font_montserrat_48,
                                           lv_color_hex(0x174A91));
    start_button = create_action_button(main_page, 196, 168, 38, T_DISPLAY_SET, &lv_font_montserrat_24,
                                        lv_color_hex(0x168B58));
    lv_obj_add_event_cb(start_button, start_long_press_cb, LV_EVENT_LONG_PRESSED, NULL);

    status_label = create_label(main_page, T_DISPLAY_READY, &lv_font_montserrat_12,
                                lv_color_hex(0x7FE0A7), 196, 212);
    setup_ap_label = create_label(main_page, "", &lv_font_montserrat_12, lv_color_hex(0xDDE7EE), 8, 196);
    lv_obj_set_width(setup_ap_label, 180);
    lv_obj_set_style_text_line_space(setup_ap_label, 0, 0);
    lv_obj_add_flag(setup_ap_label, LV_OBJ_FLAG_HIDDEN);

    create_label(limit_page, T_DISPLAY_LIMIT_TITLE, &lv_font_montserrat_14, lv_color_hex(0xB9CAD8), 8, 40);
    const char *limit_texts[] = {"15 min", "30 min", "45 min", T_DISPLAY_LIMIT_UNLIMITED};
    for (size_t index = 0; index < 4; ++index) {
        limit_buttons[index] = create_limit_button(limit_page, index % 2 ? 164 : 8, index < 2 ? 60 : 122,
                                                   limit_texts[index], LIMIT_MINUTES[index]);
    }
    lv_obj_t *limit_hint = create_label(limit_page, T_DISPLAY_LIMIT_HINT, &lv_font_montserrat_12,
                                        lv_color_hex(0xFFC16D), 8, 184);
    lv_obj_set_style_text_line_space(limit_hint, 0, 0);
    limit_label = create_label(limit_page, "", &lv_font_montserrat_14, lv_color_white(), 8, 220);
    update_limit_buttons();

    create_label(system_page, T_DISPLAY_SYSTEM_TITLE, &lv_font_montserrat_14, lv_color_hex(0xB9CAD8), 8, 40);
    ota_button = create_system_button(60, "");
    wifi_reset_button = create_system_button(122, T_DISPLAY_WIFI_RESET);
    update_ota_button();
    lv_obj_t *system_hint = create_label(system_page, T_DISPLAY_SYSTEM_HINT, &lv_font_montserrat_12,
                                         lv_color_hex(0xFFC16D), 8, 184);
    lv_obj_set_style_text_line_space(system_hint, 0, 0);

    update_network_status(NULL);
    update_temperature_labels();
    lv_timer_create(read_max6675, 500, NULL);
    lv_timer_create(update_network_status, 1000, NULL);

    mqtt_queue = xQueueCreate(1, sizeof(MqttSnapshot));
    mqtt_table_queue = xQueueCreate(1, sizeof(MqttText));
    mqtt_command_queue = xQueueCreate(4, sizeof(MqttCommand));
    if (!mqtt_queue || !mqtt_command_queue ||
        xTaskCreate(mqtt_task, "mqtt_logger", 6144, nullptr, 1, nullptr) != pdPASS) {
        if (mqtt_queue) vQueueDelete(mqtt_queue);
        if (mqtt_command_queue) vQueueDelete(mqtt_command_queue);
        mqtt_queue = nullptr;
        mqtt_command_queue = nullptr;
    }

    Serial.println("Setup done");

    // This is a key step from the working demoData.cpp. It puts the TFT into a
    // write-ready state for the LVGL flush function to use continuously.
    tft.startWrite();
}

void loop() {
    if (restart_pending && millis() - restart_at >= 1000) {
        stop_regulation();
        ESP.restart();
    }
    if (!setup_ap && WiFi.status() != WL_CONNECTED && millis() - wifi_started_at >= 20000) {
        stop_regulation();
        wifi_started_at = millis();
        start_setup_ap();
    }
    handle_ota();
    server.handleClient();
    handle_mqtt_command();
    lv_timer_handler();
    update_status_led();
    queue_mqtt_status();
    delay(5);
}