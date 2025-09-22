/*
  ESP32-CAM Cat feeder Motion detection using Telegram
  - PIR on GPIO13
  - SD: saves as /catcam/YYYY-MM-DD_HH-MM-SS.jpg 
  - NTP time sync (Pacific Time with DST)
  - Live stream: http://<IP>:81/stream
  
*/

#include <Arduino.h>
#include <WiFi.h>
#include "esp_camera.h"
#include <WiFiClientSecure.h>
#include "FS.h"
#include "SD_MMC.h"
#include <time.h>

// ======== USER CONFIG ========
#define WIFI_SSID       "ENTER YOUR WIFI SSID"
#define WIFI_PASS       "ENTER YOU WIFI PASSWORD"
#define TELEGRAM_TOKEN  "ENTER YOUR TELEGRAM TOKEN"   // from BotFather
#define TELEGRAM_CHATID "ENTER YOUR CHAT ID"             // your numeric chat id from Get ID bot

#define ENABLE_TELEGRAM  1    // 1=send to Telegram, 0=disable
#define ENABLE_SD        1    // 1=save to SD, 0=disable
// =============================

// Pins / camera options
#define PIR_PIN                   13
#define FLASH_PIN                 4
#define PRE_CAPTURE_SETTLE_MS     900
#define FLASH_PULSE_MS            120
#define FRAME_SIZE                FRAMESIZE_SVGA
#define JPEG_QUALITY              12

// PIR filters (tuned “sensitive but sane”)
#define PIR_STABLE_HIGH_MS        40
#define PIR_WARMUP_MS             15000
#define PIR_COOLDOWN_MS           5000

// Reliability knobs
#define WIFI_RETRY_MAX            30
#define TG_SEND_RETRIES           3
#define TG_SOCKET_TIMEOUT_MS      8000
#define FAILS_BEFORE_RESTART      5
#define SD_RETRY_MOUNT            3

// AI Thinker ESP32-CAM pin map
#define PWDN_GPIO_NUM             32
#define RESET_GPIO_NUM            -1
#define XCLK_GPIO_NUM              0
#define SIOD_GPIO_NUM             26
#define SIOC_GPIO_NUM             27
#define Y9_GPIO_NUM               35
#define Y8_GPIO_NUM               34
#define Y7_GPIO_NUM               39
#define Y6_GPIO_NUM               36
#define Y5_GPIO_NUM               21
#define Y4_GPIO_NUM               19
#define Y3_GPIO_NUM               18
#define Y2_GPIO_NUM                5
#define VSYNC_GPIO_NUM            25
#define HREF_GPIO_NUM             23
#define PCLK_GPIO_NUM             22

// Globals
WiFiServer streamServer(81);
const char* TG_HOST = "api.telegram.org";
WiFiClientSecure tls;

RTC_DATA_ATTR uint32_t photo_counter = 0; // persists over deep sleep/restarts
int consecutiveFails = 0;

// ---------- Helpers ----------
void proofBlink(int n=2,int on=80,int off=120){
  for(int i=0;i<n;i++){ digitalWrite(FLASH_PIN,HIGH); delay(on); digitalWrite(FLASH_PIN,LOW); delay(off); }
}

// Wi-Fi
bool ensureWiFi() {
  if (!ENABLE_TELEGRAM) {
    // you still may need Wi-Fi for time sync; but this function is
    // harmless if called when Telegram disabled.
  }
  if (WiFi.status() == WL_CONNECTED) return true;
  Serial.println("[WiFi] Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  for (int i=0;i<WIFI_RETRY_MAX;i++){
    if (WiFi.status()==WL_CONNECTED){ Serial.print("[WiFi] IP: "); Serial.println(WiFi.localIP()); return true; }
    delay(250);
  }
  Serial.println("[WiFi] Connect timeout");
  return false;
}

// ---- Time sync (NTP) ----
// Pacific Time with DST: PST8PDT,M3.2.0/2,M11.1.0/2
// Change TZ if you want a different zone.
void configureTimezone() {
  setenv("TZ", "PST8PDT,M3.2.0/2,M11.1.0/2", 1);
  tzset();
}

bool timeIsValid() {
  // Consider time valid if after Jan 1, 2021
  time_t now = time(nullptr);
  return now > 1609459200;
}

void syncTimeOnce() {
  if (timeIsValid()) return; 
  if (!ensureWiFi()) return;
  configureTimezone();
  // GMT offset and DST are handled via TZ string, so use zeros here
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("[NTP] Syncing");
  for (int i=0; i<30 && !timeIsValid(); i++) { delay(500); Serial.print("."); }
  Serial.println();
  if (timeIsValid()) {
    time_t now = time(nullptr);
    Serial.print("[NTP] Time set: "); Serial.println(ctime(&now));
  } else {
    Serial.println("[NTP] Failed to set time (will fallback to counter filenames)");
  }
}

// Camera
bool initCamera() {
  camera_config_t c;
  c.ledc_channel = LEDC_CHANNEL_0;
  c.ledc_timer   = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM;  c.pin_d1 = Y3_GPIO_NUM;  c.pin_d2 = Y4_GPIO_NUM;  c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM;  c.pin_d5 = Y7_GPIO_NUM;  c.pin_d6 = Y8_GPIO_NUM;  c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sscb_sda = SIOD_GPIO_NUM; c.pin_sscb_scl = SIOC_GPIO_NUM; c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.frame_size   = FRAME_SIZE;
  c.pixel_format = PIXFORMAT_JPEG;
  c.grab_mode    = CAMERA_GRAB_WHEN_EMPTY;
  c.fb_location  = CAMERA_FB_IN_PSRAM;
  c.jpeg_quality = JPEG_QUALITY;
  c.fb_count     = 2;

  Serial.println("[CAM] Init...");
  if (esp_camera_init(&c) != ESP_OK) { Serial.println("[CAM] Init FAILED"); return false; }
  Serial.println("[CAM] Ready");
  return true;
}

// SD 1-bit
bool ensureSD() {
  if (!ENABLE_SD) return true;
  for (int i=0;i<SD_RETRY_MOUNT;i++){
    if (SD_MMC.begin("/sdcard", /*mode1bit=*/true)) {
      if (!SD_MMC.exists("/catcam")) SD_MMC.mkdir("/catcam");
      Serial.println("[SD] Mounted (1-bit)");
      return true;
    }
    Serial.println("[SD] Mount failed, retrying...");
    delay(300);
  }
  Serial.println("[SD] Mount FAILED");
  return false;
}

// Build filename from current local time; fallback to counter if time invalid
String buildPhotoPath() {
  if (timeIsValid()) {
    time_t now = time(nullptr);
    struct tm t; localtime_r(&now, &t);
    char namebuf[64];
    // Example: /catcam/2025-09-22_15-41-07.jpg
    strftime(namebuf, sizeof(namebuf), "/catcam/%Y-%m-%d_%H-%M-%S.jpg", &t);
    return String(namebuf);
  } else {
    char namebuf[64];
    snprintf(namebuf, sizeof(namebuf), "/catcam/%08lu.jpg", (unsigned long)photo_counter);
    return String(namebuf);
  }
}

bool saveFrameToSD(camera_fb_t* fb, String &outPath) {
  if (!ENABLE_SD) return false;
  if (!fb) return false;

  outPath = buildPhotoPath();

  File f = SD_MMC.open(outPath, FILE_WRITE);
  if (!f) { Serial.println("[SD] Open file failed"); return false; }
  size_t w = f.write(fb->buf, fb->len);
  f.close();
  if (w != fb->len) { Serial.println("[SD] Write incomplete"); return false; }
  Serial.print("[SD] Saved: "); Serial.println(outPath);
  return true;
}

// Telegram
bool sendPhotoToTelegram(uint8_t* buf, size_t len) {
  if (!ENABLE_TELEGRAM) return false;
  for (int attempt=1; attempt<=TG_SEND_RETRIES; attempt++){
    if (!ensureWiFi()){ delay(300); continue; }
    tls.setInsecure();
    tls.setTimeout(TG_SOCKET_TIMEOUT_MS);
    Serial.printf("[TG] Connect attempt %d...\n", attempt);
    if (!tls.connect(TG_HOST, 443)) { Serial.println("[TG] TLS connect failed"); tls.stop(); delay(300); continue; }

    String boundary = "----ESP32CAMFormBoundary";
    String head =
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"chat_id\"\r\n\r\n" + String(TELEGRAM_CHATID) + "\r\n" +
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"caption\"\r\n\r\n" +
      String("Motion from ") + WiFi.localIP().toString() + " • Live: http://" + WiFi.localIP().toString() + ":81/stream\r\n" +
      "--" + boundary + "\r\n"
      "Content-Disposition: form-data; name=\"photo\"; filename=\"cat.jpg\"\r\n"
      "Content-Type: image/jpeg\r\n\r\n";
    String tail = "\r\n--" + boundary + "--\r\n";
    String url = String("/bot") + TELEGRAM_TOKEN + "/sendPhoto";
    String req =
      "POST " + url + " HTTP/1.1\r\n"
      "Host: " + String(TG_HOST) + "\r\n"
      "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n"
      "Content-Length: " + String(head.length() + len + tail.length()) + "\r\n"
      "Connection: close\r\n\r\n";

    if (tls.print(req) == 0 || tls.print(head) == 0) { tls.stop(); delay(200); continue; }
    // body
    size_t sent = 0, chunk = 1024;
    while (sent < len) {
      size_t n = min(chunk, len - sent);
      int w = tls.write(buf + sent, n);
      if (w <= 0) { tls.stop(); delay(200); goto retry; }
      sent += w;
      yield();
    }
    if (tls.print(tail) == 0) { tls.stop(); delay(200); continue; }

    // read status line
    {
      unsigned long t0 = millis();
      bool ok = false;
      while (millis() - t0 < TG_SOCKET_TIMEOUT_MS) {
        String line = tls.readStringUntil('\n');
        if (line.startsWith("HTTP/1.1 200")) { ok = true; break; }
        if (line.length()==0) break;
      }
      tls.stop();
      if (ok) { Serial.println("[TG] Sent OK"); return true; }
      Serial.println("[TG] No 200; assuming fail");
    }
retry:
    tls.stop(); delay(250);
  }
  Serial.println("[TG] All attempts failed");
  return false;
}

// Stream
void startStreamServer() { streamServer.begin(); Serial.println("[HTTP] Stream on :81/stream"); }

void handleStreamClient(WiFiClient &client) {
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Cache-Control: no-cache");
  client.println("Pragma: no-cache");
  client.println();
  while (client.connected() && WiFi.status()==WL_CONNECTED) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) break;
    client.println("--frame");
    client.println("Content-Type: image/jpeg");
    client.print("Content-Length: "); client.println(fb->len);
    client.println();
    client.write(fb->buf, fb->len);
    client.println();
    esp_camera_fb_return(fb);
    delay(50);
    yield();
  }
  client.stop();
}
void streamLoop() { WiFiClient client = streamServer.available(); if (client) handleStreamClient(client); }

// Capture (delay + flash)
camera_fb_t* takeFrame() {
  delay(PRE_CAPTURE_SETTLE_MS);
  digitalWrite(FLASH_PIN, HIGH);
  delay(FLASH_PULSE_MS);
  camera_fb_t *fb = esp_camera_fb_get();
  digitalWrite(FLASH_PIN, LOW);
  return fb;
}

// PIR rising-edge + short stability check
bool motionRisingEdgeStable(uint32_t stable_ms) {
  static bool prevHigh = false;
  bool nowHigh = (digitalRead(PIR_PIN) == HIGH);
  if (!prevHigh && nowHigh) {
    uint32_t t0 = millis();
    while (millis() - t0 < stable_ms) {
      if (digitalRead(PIR_PIN) == LOW) { prevHigh = false; return false; }
      delay(1);
    }
    prevHigh = true; return true;
  }
  if (!nowHigh) prevHigh = false;
  return false;
}

// ---------- Setup / Loop ----------
void setup() {
  pinMode(FLASH_PIN, OUTPUT); digitalWrite(FLASH_PIN, LOW);
  pinMode(PIR_PIN, INPUT_PULLDOWN);

  Serial.begin(115200); delay(200);
  proofBlink(2);

  if (!initCamera()) { delay(1000); ESP.restart(); }
  if (!ensureSD()) { Serial.println("[SD] Not mounted; will retry on capture."); }

  // Bring up Wi-Fi (for stream, Telegram, and NTP), then sync time
  ensureWiFi();
  syncTimeOnce();

  startStreamServer();
}

void loop() {
  static uint32_t bootStart = millis();
  static uint32_t lastShot  = 0;

  // serve stream
  streamLoop();

  // Warm-up ignore
  if (millis() - bootStart < PIR_WARMUP_MS) { delay(5); return; }

  // Cooldown
  if (millis() - lastShot < PIR_COOLDOWN_MS) { delay(5); return; }

  // Motion?
  if (motionRisingEdgeStable(PIR_STABLE_HIGH_MS)) {
    lastShot = millis();
    Serial.println("[PIR] Motion → capture");

    // (re)sync time occasionally if it wasn't valid before
    if (!timeIsValid()) syncTimeOnce();

    // Capture
    camera_fb_t *fb = takeFrame();
    if (!fb) {
      Serial.println("[CAP] fb=NULL, reinit camera");
      esp_camera_deinit();
      if (!initCamera()) { Serial.println("[CAM] Reinit failed"); consecutiveFails++; }
      if (consecutiveFails >= FAILS_BEFORE_RESTART) { Serial.println("[SYS] Restarting"); ESP.restart(); }
      delay(5);
      return;
    }

    bool sdOk=false, tgOk=false;

    // Save to SD
    if (ENABLE_SD) {
      if (SD_MMC.cardType()==CARD_NONE) { Serial.println("[SD] Not mounted; trying mount..."); ensureSD(); }
      String path;
      sdOk = saveFrameToSD(fb, path);
    }

    // Send to Telegram
    if (ENABLE_TELEGRAM) {
      if (WiFi.status() != WL_CONNECTED) ensureWiFi();
      tgOk = sendPhotoToTelegram(fb->buf, fb->len);
    }

    esp_camera_fb_return(fb);
    photo_counter++;

    if (!sdOk)  Serial.println("[SD] Save failed (ignored if SD disabled)");
    if (!tgOk && ENABLE_TELEGRAM) Serial.println("[TG] Send failed");

    if (sdOk || tgOk) {
      consecutiveFails = 0;
    } else {
      consecutiveFails++;
      if (consecutiveFails >= FAILS_BEFORE_RESTART) {
        Serial.println("[SYS] Too many consecutive failures → restart");
        ESP.restart();
      }
    }
  }

  delay(5);
}
