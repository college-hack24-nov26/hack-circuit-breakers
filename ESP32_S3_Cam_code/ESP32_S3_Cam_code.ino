/*
  AGRIMIND CAMERA - AI THINKER ESP32-CAM
  Arduino-ESP32 Core 2.0.17

  Features:
  1. Live camera streaming in Chrome
  2. Flash brightness slider (0-255)
  3. Servo angle slider (0-180)
  4. RGB565 to JPEG software conversion

  Hardware:
  Camera: AI Thinker ESP32-CAM
  Servo signal: GPIO13
  Flash LED: GPIO4

  IMPORTANT:
  - Servo requires a separate regulated 5V supply.
  - Connect servo GND to ESP32-CAM GND.
  - Do not use microSD while servo uses GPIO13.
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <esp_http_server.h>
#include <ESP32Servo.h>

// -------- YOUR WIFI DETAILS --------
const char* WIFI_SSID = "OnePlus Nord CE4";
const char* WIFI_PASSWORD = "Joshi@606";
// -----------------------------------

#define SERVO_PIN       13
#define FLASH_LED_PIN    4

#define FLASH_PWM_CHANNEL 7
#define FLASH_PWM_FREQ    5000
#define FLASH_PWM_RES     8

// AI Thinker ESP32-CAM pin configuration
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// MJPEG stream configuration
static const char* STREAM_CONTENT_TYPE =
  "multipart/x-mixed-replace;boundary=agrimindframe";

static const char* STREAM_BOUNDARY =
  "\r\n--agrimindframe\r\n";

static const char* STREAM_PART =
  "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t camera_httpd = NULL;
httpd_handle_t stream_httpd = NULL;

Servo cameraServo;

int currentServoAngle = 90;
int currentFlashBrightness = 0;

// ---------------- WEB PAGE ----------------

const char INDEX_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html>
<head>
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>AgriMind Camera</title>

<style>
body {
  font-family: Arial, sans-serif;
  background: #101820;
  color: white;
  text-align: center;
  margin: 0;
  padding: 18px;
}

.card {
  max-width: 760px;
  margin: 14px auto;
  background: #1c2b36;
  padding: 18px;
  border-radius: 14px;
}

img {
  width: 100%;
  max-width: 640px;
  background: black;
  border-radius: 10px;
}

input {
  width: 90%;
  margin: 16px 0;
  accent-color: #35c98b;
}

.value {
  font-size: 20px;
  color: #35c98b;
  font-weight: bold;
}

h2 {
  margin-top: 4px;
}

small {
  color: #c4d0d8;
}
</style>
</head>

<body>

<h2>AgriMind - Live Camera</h2>

<div class="card">
  <img id="stream" alt="Camera stream loading">
  <p><small>Live video from ESP32-CAM</small></p>
</div>

<div class="card">
  <h3>Camera Flash Brightness</h3>

  <input id="flash" type="range"
         min="0" max="255" value="0"
         oninput="setFlash(this.value)">

  <div class="value">
    <span id="flashValue">0</span> / 255
  </div>
</div>

<div class="card">
  <h3>Camera Horizontal View (Servo)</h3>

  <input id="angle" type="range"
         min="0" max="180" value="90"
         oninput="setAngle(this.value)">

  <div class="value">
    <span id="angleValue">90</span>&deg;
  </div>

  <small>
    Move slowly. Reduce the range if the servo reaches a mechanical stop.
  </small>
</div>

<script>

const host = window.location.hostname;

document.getElementById('stream').src =
  'http://' + host + ':81/stream';

let flashTimer = null;
let angleTimer = null;

function setFlash(v) {

  document.getElementById('flashValue').textContent = v;

  clearTimeout(flashTimer);

  flashTimer = setTimeout(() => {

    fetch('/flash?value=' + encodeURIComponent(v))
      .catch(() => {});

  }, 80);
}

function setAngle(v) {

  document.getElementById('angleValue').textContent = v;

  clearTimeout(angleTimer);

  angleTimer = setTimeout(() => {

    fetch('/servo?angle=' + encodeURIComponent(v))
      .catch(() => {});

  }, 80);
}

</script>

</body>
</html>
)HTML";

// ---------------- HOME PAGE HANDLER ----------------

esp_err_t index_handler(httpd_req_t *req) {

  httpd_resp_set_type(req, "text/html");

  httpd_resp_set_hdr(req, "Cache-Control", "no-store");

  return httpd_resp_send(
    req,
    INDEX_HTML,
    HTTPD_RESP_USE_STRLEN
  );
}

// ---------------- FLASH HANDLER ----------------

esp_err_t flash_handler(httpd_req_t *req) {

  char query[64];
  char value[16];

  if (
    httpd_req_get_url_query_str(
      req, query, sizeof(query)
    ) == ESP_OK
  ) {

    if (
      httpd_query_key_value(
        query, "value", value, sizeof(value)
      ) == ESP_OK
    ) {

      currentFlashBrightness =
        constrain(atoi(value), 0, 255);

      ledcWrite(
        FLASH_PWM_CHANNEL,
        currentFlashBrightness
      );
    }
  }

  char response[48];

  snprintf(
    response,
    sizeof(response),
    "Flash brightness: %d",
    currentFlashBrightness
  );

  httpd_resp_set_type(req, "text/plain");

  return httpd_resp_send(
    req,
    response,
    HTTPD_RESP_USE_STRLEN
  );
}

// ---------------- SERVO HANDLER ----------------

esp_err_t servo_handler(httpd_req_t *req) {

  char query[64];
  char value[16];

  if (
    httpd_req_get_url_query_str(
      req, query, sizeof(query)
    ) == ESP_OK
  ) {

    if (
      httpd_query_key_value(
        query, "angle", value, sizeof(value)
      ) == ESP_OK
    ) {

      currentServoAngle =
        constrain(atoi(value), 0, 180);

      cameraServo.write(currentServoAngle);
    }
  }

  char response[40];

  snprintf(
    response,
    sizeof(response),
    "Servo angle: %d",
    currentServoAngle
  );

  httpd_resp_set_type(req, "text/plain");

  return httpd_resp_send(
    req,
    response,
    HTTPD_RESP_USE_STRLEN
  );
}

// ---------------- CAMERA STREAM HANDLER ----------------

esp_err_t stream_handler(httpd_req_t *req) {

  camera_fb_t *fb = NULL;

  esp_err_t res = ESP_OK;

  char part_buf[80];

  res = httpd_resp_set_type(
    req,
    STREAM_CONTENT_TYPE
  );

  if (res != ESP_OK) {
    return res;
  }

  httpd_resp_set_hdr(
    req,
    "Access-Control-Allow-Origin",
    "*"
  );

  httpd_resp_set_hdr(
    req,
    "Cache-Control",
    "no-store"
  );

  while (true) {

    fb = esp_camera_fb_get();

    if (!fb) {

      Serial.println("Camera capture failed");

      res = ESP_FAIL;

      break;
    }

    // RGB565 TO JPEG CONVERSION

    uint8_t *jpg_buf = NULL;

    size_t jpg_len = 0;

    bool converted = false;

    if (fb->format == PIXFORMAT_JPEG) {

      // Already JPEG
      jpg_buf = fb->buf;

      jpg_len = fb->len;

    } else {

      // Convert RGB565 frame into JPEG
      converted = frame2jpg(
        fb,
        70,
        &jpg_buf,
        &jpg_len
      );

      if (
        !converted ||
        jpg_buf == NULL ||
        jpg_len == 0
      ) {

        Serial.println(
          "RGB565 to JPEG conversion failed"
        );

        esp_camera_fb_return(fb);

        fb = NULL;

        res = ESP_FAIL;

        break;
      }
    }

    // Send MJPEG boundary

    res = httpd_resp_send_chunk(
      req,
      STREAM_BOUNDARY,
      strlen(STREAM_BOUNDARY)
    );

    // Send JPEG header

    if (res == ESP_OK) {

      size_t hlen = snprintf(
        part_buf,
        sizeof(part_buf),
        STREAM_PART,
        (unsigned int)jpg_len
      );

      res = httpd_resp_send_chunk(
        req,
        part_buf,
        hlen
      );
    }

    // Send JPEG image

    if (res == ESP_OK) {

      res = httpd_resp_send_chunk(
        req,
        (const char*)jpg_buf,
        jpg_len
      );
    }

    // Release converted JPEG buffer

    if (converted && jpg_buf != NULL) {

      free(jpg_buf);
    }

    // Return original camera frame

    esp_camera_fb_return(fb);

    fb = NULL;

    if (res != ESP_OK) {
      break;
    }
  }

  if (fb != NULL) {

    esp_camera_fb_return(fb);
  }

  return res;
}

// ---------------- START WEB SERVER ----------------

void startCameraServer() {

  httpd_config_t config =
    HTTPD_DEFAULT_CONFIG();

  config.server_port = 80;

  config.ctrl_port = 32768;

  config.max_uri_handlers = 8;

  httpd_uri_t index_uri = {

    .uri = "/",

    .method = HTTP_GET,

    .handler = index_handler,

    .user_ctx = NULL
  };

  httpd_uri_t flash_uri = {

    .uri = "/flash",

    .method = HTTP_GET,

    .handler = flash_handler,

    .user_ctx = NULL
  };

  httpd_uri_t servo_uri = {

    .uri = "/servo",

    .method = HTTP_GET,

    .handler = servo_handler,

    .user_ctx = NULL
  };

  if (
    httpd_start(
      &camera_httpd,
      &config
    ) == ESP_OK
  ) {

    httpd_register_uri_handler(
      camera_httpd,
      &index_uri
    );

    httpd_register_uri_handler(
      camera_httpd,
      &flash_uri
    );

    httpd_register_uri_handler(
      camera_httpd,
      &servo_uri
    );

    Serial.println("Web control server started");

  } else {

    Serial.println(
      "ERROR: Web control server could not start"
    );
  }

  // Separate stream server

  httpd_config_t streamConfig =
    HTTPD_DEFAULT_CONFIG();

  streamConfig.server_port = 81;

  streamConfig.ctrl_port = 32769;

  streamConfig.max_uri_handlers = 2;

  streamConfig.stack_size = 8192;

  httpd_uri_t stream_uri = {

    .uri = "/stream",

    .method = HTTP_GET,

    .handler = stream_handler,

    .user_ctx = NULL
  };

  if (
    httpd_start(
      &stream_httpd,
      &streamConfig
    ) == ESP_OK
  ) {

    httpd_register_uri_handler(
      stream_httpd,
      &stream_uri
    );

    Serial.println("Camera stream server started");

  } else {

    Serial.println(
      "ERROR: Camera stream server could not start"
    );
  }
}

// ---------------- CAMERA INITIALIZATION ----------------

bool initializeCamera() {

  camera_config_t config = {};

  config.ledc_channel = LEDC_CHANNEL_0;

  config.ledc_timer = LEDC_TIMER_0;

  config.pin_d0 = Y2_GPIO_NUM;

  config.pin_d1 = Y3_GPIO_NUM;

  config.pin_d2 = Y4_GPIO_NUM;

  config.pin_d3 = Y5_GPIO_NUM;

  config.pin_d4 = Y6_GPIO_NUM;

  config.pin_d5 = Y7_GPIO_NUM;

  config.pin_d6 = Y8_GPIO_NUM;

  config.pin_d7 = Y9_GPIO_NUM;

  config.pin_xclk = XCLK_GPIO_NUM;

  config.pin_pclk = PCLK_GPIO_NUM;

  config.pin_vsync = VSYNC_GPIO_NUM;

  config.pin_href = HREF_GPIO_NUM;

  config.pin_sscb_sda = SIOD_GPIO_NUM;

  config.pin_sscb_scl = SIOC_GPIO_NUM;

  config.pin_pwdn = PWDN_GPIO_NUM;

  config.pin_reset = RESET_GPIO_NUM;

  config.xclk_freq_hz = 20000000;

  // Your camera accepted RGB565.
  // Convert captured frames to JPEG in stream_handler().

  config.pixel_format = PIXFORMAT_RGB565;

  // Use smaller resolution to reduce RAM usage
  // and make software JPEG conversion easier.

  config.frame_size = FRAMESIZE_QVGA;

  config.jpeg_quality = 12;

  config.fb_count = 1;

  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {

    Serial.printf(
      "Camera init failed: 0x%x\n",
      err
    );

    return false;
  }

  sensor_t *s = esp_camera_sensor_get();

  if (s) {

    s->set_framesize(
      s,
      FRAMESIZE_QVGA
    );
  }

  Serial.println("Camera initialized successfully");

  return true;
}

// ---------------- SETUP ----------------

void setup() {

  Serial.begin(115200);

  delay(1000);

  Serial.println();

  Serial.println(
    "Starting AgriMind ESP32-CAM..."
  );

  // Flash PWM setup for core 2.0.17

  ledcSetup(
    FLASH_PWM_CHANNEL,
    FLASH_PWM_FREQ,
    FLASH_PWM_RES
  );

  ledcAttachPin(
    FLASH_LED_PIN,
    FLASH_PWM_CHANNEL
  );

  ledcWrite(
    FLASH_PWM_CHANNEL,
    0
  );

  // Servo setup

  cameraServo.setPeriodHertz(50);

  cameraServo.attach(
    SERVO_PIN,
    500,
    2400
  );

  cameraServo.write(
    currentServoAngle
  );

  // Initialize camera

  if (!initializeCamera()) {

    Serial.println(
      "Camera initialization failed"
    );

    while (true) {
      delay(1000);
    }
  }

  // Wi-Fi setup

  WiFi.mode(WIFI_STA);

  WiFi.setSleep(false);

  WiFi.begin(
    WIFI_SSID,
    WIFI_PASSWORD
  );

  Serial.print("Connecting to Wi-Fi");

  unsigned long startAttempt = millis();

  while (
    WiFi.status() != WL_CONNECTED &&
    millis() - startAttempt < 30000UL
  ) {

    delay(500);

    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {

    Serial.println("Wi-Fi connected!");

    Serial.print("Open in Chrome: http://");

    Serial.println(WiFi.localIP());

    startCameraServer();

  } else {

    Serial.println(
      "Wi-Fi connection failed. Check SSID and password."
    );
  }
}

// ---------------- LOOP ----------------

void loop() {

  delay(1000);

  static unsigned long lastReconnect = 0;

  if (
    WiFi.status() != WL_CONNECTED &&
    millis() - lastReconnect > 10000UL
  ) {

    lastReconnect = millis();

    Serial.println(
      "Wi-Fi lost; reconnecting..."
    );

    WiFi.reconnect();
  }
}
