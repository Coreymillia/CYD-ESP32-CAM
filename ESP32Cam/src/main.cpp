/*
 * ESP32-CAM Live Feed
 * -------------------
 * Connects to the CYD SoftAP "StickVCam" as a second image source.
 * Captures a fresh JPEG on every request — no background loop needed.
 *
 * HTTP endpoints (mirrors C3 relay interface):
 *   GET /latest.jpg  → fresh JPEG frame (320×240 QVGA)
 *   GET /status      → {"type":"cam","count":N}
 *   GET /            → simple HTML live-reload page
 *
 * The "type":"cam" field lets the CYD distinguish this device from the
 * C3 relay (which has no "type" field in its /status response).
 *
 * Hardware: AI Thinker ESP32-CAM (OV2640)
 */

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include "esp_camera.h"

// ── AI Thinker ESP32-CAM pin map ──────────────────────────────────
#define PWDN_GPIO_NUM   32
#define RESET_GPIO_NUM  -1
#define XCLK_GPIO_NUM    0
#define SIOD_GPIO_NUM   26
#define SIOC_GPIO_NUM   27
#define Y9_GPIO_NUM     35
#define Y8_GPIO_NUM     34
#define Y7_GPIO_NUM     39
#define Y6_GPIO_NUM     36
#define Y5_GPIO_NUM     21
#define Y4_GPIO_NUM     19
#define Y3_GPIO_NUM     18
#define Y2_GPIO_NUM      5
#define VSYNC_GPIO_NUM  25
#define HREF_GPIO_NUM   23
#define PCLK_GPIO_NUM   22

// ── Network — must match CYD SoftAP ───────────────────────────────
const char* WIFI_SSID = "StickVCam";
const char* WIFI_PASS = "";   // open AP

// ── Globals ───────────────────────────────────────────────────────
WebServer   server(80);
uint32_t    captureCount = 0;

// ── HTTP handlers ─────────────────────────────────────────────────
void handleStatus() {
    char json[64];
    snprintf(json, sizeof(json), "{\"type\":\"cam\",\"count\":%u}", captureCount);
    server.send(200, "application/json", json);
}

void handleLatestJpeg() {
    camera_fb_t* fb = esp_camera_fb_get();
    if (!fb) {
        server.send(500, "text/plain", "Camera capture failed");
        return;
    }
    captureCount++;
    server.sendHeader("Content-Type",   "image/jpeg");
    server.sendHeader("Content-Length", String(fb->len));
    server.sendHeader("Cache-Control",  "no-cache");
    server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
    Serial.printf("[CAM] Frame #%u served (%u bytes)\n", captureCount, fb->len);
    esp_camera_fb_return(fb);
}

void handleRoot() {
    server.send(200, "text/html",
        "<h2>ESP32-CAM Live Feed</h2>"
        "<img id='f' src='/latest.jpg' style='max-width:320px'>"
        "<script>setInterval(()=>{document.getElementById('f').src="
        "'/latest.jpg?'+Date.now()},1000)</script>");
}

// ── Camera init ───────────────────────────────────────────────────
bool initCamera() {
    camera_config_t cfg = {};
    cfg.ledc_channel = LEDC_CHANNEL_0;
    cfg.ledc_timer   = LEDC_TIMER_0;
    cfg.pin_d0       = Y2_GPIO_NUM;
    cfg.pin_d1       = Y3_GPIO_NUM;
    cfg.pin_d2       = Y4_GPIO_NUM;
    cfg.pin_d3       = Y5_GPIO_NUM;
    cfg.pin_d4       = Y6_GPIO_NUM;
    cfg.pin_d5       = Y7_GPIO_NUM;
    cfg.pin_d6       = Y8_GPIO_NUM;
    cfg.pin_d7       = Y9_GPIO_NUM;
    cfg.pin_xclk     = XCLK_GPIO_NUM;
    cfg.pin_pclk     = PCLK_GPIO_NUM;
    cfg.pin_vsync    = VSYNC_GPIO_NUM;
    cfg.pin_href     = HREF_GPIO_NUM;
    cfg.pin_sscb_sda = SIOD_GPIO_NUM;
    cfg.pin_sscb_scl = SIOC_GPIO_NUM;
    cfg.pin_pwdn     = PWDN_GPIO_NUM;
    cfg.pin_reset    = RESET_GPIO_NUM;
    cfg.xclk_freq_hz = 20000000;
    cfg.pixel_format = PIXFORMAT_JPEG;
    cfg.frame_size   = FRAMESIZE_QVGA;   // 320×240 — matches CYD screen
    cfg.jpeg_quality = 12;               // 0-63, lower = better quality

    if (psramFound()) {
        cfg.jpeg_quality = 10;
        cfg.fb_count     = 2;
        cfg.grab_mode    = CAMERA_GRAB_LATEST;
    } else {
        cfg.fb_count  = 1;
        cfg.grab_mode = CAMERA_GRAB_WHEN_EMPTY;
    }

    esp_err_t err = esp_camera_init(&cfg);
    if (err != ESP_OK) {
        Serial.printf("[CAM] Camera init failed: 0x%x\n", err);
        return false;
    }

    sensor_t* s = esp_camera_sensor_get();
    s->set_vflip(s, 1);     // flip vertically (adjust if image appears upside-down)
    s->set_hmirror(s, 0);
    return true;
}

// ── Setup ─────────────────────────────────────────────────────────
void setup() {
    Serial.begin(115200);
    Serial.println("\n[CAM] ESP32-CAM booting...");

    if (!initCamera()) {
        Serial.println("[CAM] Camera init failed — halting. Check board and wiring.");
        while (true) delay(1000);
    }
    Serial.println("[CAM] Camera OK");

    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.print("[CAM] Connecting to StickVCam AP");

    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
        if (millis() - t > 20000) {
            Serial.println("\n[CAM] WiFi timeout — restarting");
            ESP.restart();
        }
    }
    Serial.println("\n[CAM] Connected — IP: " + WiFi.localIP().toString());

    server.on("/",           handleRoot);
    server.on("/status",     handleStatus);
    server.on("/latest.jpg", handleLatestJpeg);
    server.begin();
    Serial.println("[CAM] HTTP server started — ready");
}

// ── Loop ──────────────────────────────────────────────────────────
void loop() {
    if (WiFi.status() != WL_CONNECTED) {
        static unsigned long lastReconnect = 0;
        if (millis() - lastReconnect >= 5000) {
            lastReconnect = millis();
            Serial.println("[CAM] WiFi lost — reconnecting");
            WiFi.reconnect();
        }
    }
    server.handleClient();
}
