#include <lvgl.h>
#include <TFT_eSPI.h>
#include "CST820.h" // Include the touch controller header
#include <WiFi.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <driver/gpio.h>
#include <esp_timer.h>
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


static void handle_gpio_set()
{
    const String pin_arg = server.arg("pin");
    const String value = server.arg("value");
    uint8_t pin;
    if (pin_arg == "17") pin = 17;
    else if (pin_arg == "4") pin = 4;
    else if (pin_arg == "16") pin = 16;
    else if (pin_arg == "22") pin = 22;
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
void my_touchpad_read(lv_indev_drv_t *indev_driver, lv_indev_data_t *data)
{
    bool touched;
    uint8_t gesture;
    uint16_t touchX, touchY;

    touched = touch.getTouch(&touchX, &touchY, &gesture);

    if (!touched)
    {
        data->state = LV_INDEV_STATE_REL;
    }
    else
    {
        data->state = LV_INDEV_STATE_PR;
        // Transform coordinates for landscape mode (rotation 1)
        // Raw touch (X:0-239, Y:0-319) -> Screen (X:0-319, Y:0-239)
        data->point.x = touchY;
        data->point.y = 239 - touchX;
    }
}

static lv_obj_t *actual_temp_label;
static lv_obj_t *target_temp_label;
static lv_obj_t *status_label;
static lv_obj_t *increase_button;
static lv_obj_t *decrease_button;
static lv_obj_t *start_button;
static float actual_temp;
static bool actual_temp_read = false;
static bool actual_temp_valid = false;
static uint32_t last_sample_at;
static int target_temp = 220;
static int draft_temp = 220;
static constexpr int MAX_TARGET_TEMP = 250;
static constexpr int TARGET_TEMP_STEP = 5;
static constexpr uint32_t SAMPLE_TIMEOUT_MS = 1500;
static constexpr uint32_t MAX_HEAT_TIME_MS = 15 * 60 * 1000;
static constexpr float HEATER_HYSTERESIS = 2.0f;
static constexpr float MAX_PLATE_TEMP = 260.0f;
static bool regulation_running = false;
static uint32_t regulation_started_at;
static uint32_t regulation_elapsed;
static bool heater_on = false;
static uint32_t heater_off_at;
static esp_timer_handle_t heater_watchdog;
static bool heater_watchdog_ready = false;
static portMUX_TYPE heater_lock = portMUX_INITIALIZER_UNLOCKED;
static int64_t heater_deadline_us;
static bool heater_expired = false;

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
}

static void stop_regulation()
{
    portENTER_CRITICAL(&heater_lock);
    gpio_set_level(HEATER_PIN, HIGH);
    heater_deadline_us = 0;
    portEXIT_CRITICAL(&heater_lock);
    if (heater_on) heater_off_at = millis();
    heater_on = false;
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

static void regulate_heater()
{
    if (!regulation_running) return;
    const uint32_t now = millis();
    if (!temperature_is_safe(target_temp) || now - regulation_started_at >= MAX_HEAT_TIME_MS) {
        stop_regulation();
        return;
    }

    const bool enable = heater_on ? actual_temp < target_temp :
                        actual_temp <= target_temp - HEATER_HYSTERESIS && now - heater_off_at >= 5000;
    const int64_t now_us = esp_timer_get_time();
    const uint32_t sample_remaining = SAMPLE_TIMEOUT_MS - (now - last_sample_at);
    const uint32_t session_remaining = MAX_HEAT_TIME_MS - (now - regulation_started_at);
    const uint32_t remaining = sample_remaining < session_remaining ? sample_remaining : session_remaining;
    portENTER_CRITICAL(&heater_lock);
    const bool expired = heater_expired || (heater_deadline_us != 0 && now_us >= heater_deadline_us);
    if (!expired) {
        heater_deadline_us = now_us + static_cast<int64_t>(remaining) * 1000;
        gpio_set_level(HEATER_PIN, enable ? LOW : HIGH);
    }
    portEXIT_CRITICAL(&heater_lock);
    if (expired) {
        stop_regulation();
        return;
    }
    if (heater_on && !enable) heater_off_at = now;
    heater_on = enable;
    lv_label_set_text(status_label, enable ? T_DISPLAY_HEATING : T_DISPLAY_READY);
}

static bool start_regulation(int requested)
{
    if (!heater_watchdog_ready || restart_pending || requested < minimum_target_temp() ||
        requested > MAX_TARGET_TEMP || !temperature_is_safe(requested)) return false;
    if (!regulation_running) {
        portENTER_CRITICAL(&heater_lock);
        heater_expired = false;
        portEXIT_CRITICAL(&heater_lock);
        regulation_started_at = millis();
        regulation_elapsed = 0;
        regulation_running = true;
    }
    target_temp = requested;
    draft_temp = requested;
    lv_label_set_text(lv_obj_get_child(start_button, 0), T_PLATE_STOP);
    lv_obj_set_style_bg_color(start_button, lv_color_hex(0x9E3D36), 0);
    regulate_heater();
    return regulation_running;
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
        else start_regulation(draft_temp);
    }
    update_temperature_labels();
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
void handleSetup() {
    server.send(200, "text/html", SETUP_HTML);
}

void handle_main_page() {
    if (OTA_PASSWORD[0] != '\0' && !server.authenticate("ota", OTA_PASSWORD)) {
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
    char json[224];
    snprintf(json, sizeof(json),
             "{\"actual\":%s,\"target\":%d,\"running\":%s,\"canSet\":%s,\"elapsed\":%lu,\"min\":%d,\"max\":%d,\"step\":%d}",
             actual, target_temp, regulation_running ? "true" : "false",
             OTA_PASSWORD[0] != '\0' && WiFi.status() == WL_CONNECTED && heater_watchdog_ready &&
             !restart_pending && minimum_target_temp() <= MAX_TARGET_TEMP ? "true" : "false",
             static_cast<unsigned long>(elapsed / 1000), minimum_target_temp(), MAX_TARGET_TEMP, TARGET_TEMP_STEP);
    server.sendHeader("Cache-Control", "no-store");
    server.send(200, "application/json", json);
}

static void handle_plate_set()
{
    if (OTA_PASSWORD[0] == '\0' || WiFi.status() != WL_CONNECTED || server.header("X-Reflow-Control") != "1") {
        server.send(403);
        return;
    }
    if (!server.authenticate("ota", OTA_PASSWORD)) {
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
    stop_regulation();
    if (!settings_open || settings.putBytes("wifi", &updated, sizeof(updated)) != sizeof(updated)) {
        server.send(500);
        return;
    }
    server.send(204);
    restart_at = millis();
    restart_pending = true;
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

static bool web_update_authorized()
{
    return OTA_PASSWORD[0] != '\0' && WiFi.status() == WL_CONNECTED &&
           server.authenticate("ota", OTA_PASSWORD);
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
    if (OTA_PASSWORD[0] == '\0' || WiFi.status() != WL_CONNECTED) {
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
    server.on("/api/plate/stop", HTTP_POST, []() {
        stop_regulation();
        handle_plate_status();
    });
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
    lv_indev_drv_register(&indev_drv);

    lv_obj_t *screen = lv_scr_act();
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_hex(0x101820), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    network_label = create_label(screen, T_DISPLAY_WIFI_CONNECTING, &lv_font_montserrat_12,
                                 lv_color_hex(0xDDE7EE), 8, 5);
    lv_obj_set_width(network_label, 220);
    lv_obj_set_style_text_align(network_label, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t *actual_panel = create_panel(screen, 8, 28, 180, 82, lv_color_hex(0x1B2B3D));
    create_label(actual_panel, T_DISPLAY_ACTUAL, &lv_font_montserrat_14,
                 lv_color_hex(0xB9CAD8), 10, 4);
    actual_temp_label = create_label(actual_panel, "", &lv_font_montserrat_32,
                                     lv_color_white(), 10, 21);
#ifdef T_DISPLAY_ACTUAL_DETAIL
    create_label(actual_panel, T_DISPLAY_ACTUAL_DETAIL, &lv_font_montserrat_12,
#else
    create_label(actual_panel, T_PLATE_ACTUAL, &lv_font_montserrat_12,
#endif
                 lv_color_hex(0xB9CAD8), 10, 62);

    lv_obj_t *target_panel = create_panel(screen, 8, 118, 180, 70, lv_color_hex(0x1B2B3D));
    create_label(target_panel, T_DISPLAY_TARGET, &lv_font_montserrat_14,
                 lv_color_hex(0xB9CAD8), 10, 3);
    target_temp_label = create_label(target_panel, "", &lv_font_montserrat_24,
                                     lv_color_white(), 38, 20);
    create_label(target_panel, T_PLATE_TARGET, &lv_font_montserrat_12,
                 lv_color_hex(0xB9CAD8), 10, 49);

    increase_button = create_action_button(screen, 196, 28, 58, "+", &lv_font_montserrat_48,
                                           lv_color_hex(0x174A91));
    decrease_button = create_action_button(screen, 196, 92, 58, "-", &lv_font_montserrat_48,
                                           lv_color_hex(0x174A91));
    start_button = create_action_button(screen, 196, 158, 42, T_DISPLAY_SET, &lv_font_montserrat_24,
                                        lv_color_hex(0x168B58));

    status_label = create_label(screen, T_DISPLAY_READY, &lv_font_montserrat_12,
                                lv_color_hex(0x7FE0A7), 196, 207);
    setup_ap_label = create_label(screen, "", &lv_font_montserrat_12, lv_color_hex(0xDDE7EE), 8, 193);
    lv_obj_set_width(setup_ap_label, 180);
    lv_obj_set_style_text_line_space(setup_ap_label, 0, 0);
    lv_obj_add_flag(setup_ap_label, LV_OBJ_FLAG_HIDDEN);
    update_network_status(NULL);
    update_temperature_labels();
    lv_timer_create(read_max6675, 500, NULL);
    lv_timer_create(update_network_status, 1000, NULL);

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
    lv_timer_handler();
    delay(5);
}