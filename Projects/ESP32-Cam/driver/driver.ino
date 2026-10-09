#include "esp_camera.h"
#include "driver/gpio.h"
#include <ESPmDNS.h>
#include <Wire.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Adafruit_Sensor.h>
#include <Adafruit_BME680.h>

// Raspberry PI Network
const char* ssid = "SmartFridge";
const char* password = "team2SmartFridge";
WebServer server(80);

// Image Frequency
const int frequency = 15; // camera capture frequency in seconds
const int flash_delay = 2000; // turns on flash and waits until X/2ms passes before and after capture
const int transfer_frequency = 3; // attempt to transfer every Xs before connection closes

// Network Check Frequency
const int wifi_check_frequency = 300; // Checks every Nms 

// Camera UID (#)
int camera_num = 4;
const int total_cams = 4;

// Image Buffer
uint8_t* image_buffer  = NULL;
size_t image_length = 0;
SemaphoreHandle_t mutex;

// Sensor Data (time insensitive)
// The arrays are treated as a queue, where 0 <= i <= 100 and i -> 0 when i = 101
// Data is coupled with image taking
bool sensor_available = false;
const int readings = 3;
int current_reading = 0;
bool completed_full = false;
float temperature[readings]; // C
float pressure[readings]; // hPa
float humidity[readings]; // %
float gas[readings]; // kOhms

// Sensor & Wire
TwoWire GRAVITY_WIRE = TwoWire(0);
Adafruit_BME680 bme(&GRAVITY_WIRE);

// LED Pin
#define ONBOARD_LED       GPIO_NUM_3
// Camera pins
#define XCLK_GPIO_NUM     5
#define Y9_GPIO_NUM       4
#define Y8_GPIO_NUM       6
#define Y7_GPIO_NUM       7
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       17
#define Y4_GPIO_NUM       21
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM       16
#define VSYNC_GPIO_NUM    1
#define HREF_GPIO_NUM     2
#define PCLK_GPIO_NUM     15
#define SIOD_GPIO_NUM     8
#define SIOC_GPIO_NUM     9
// BME Sensor
#define I2C_SDA           43
#define I2C_SCL           44

gpio_config_t setup_gpio_config() {
  gpio_config_t config;
  config.intr_type = GPIO_INTR_DISABLE;
  config.mode = GPIO_MODE_OUTPUT;
  config.pin_bit_mask = 1ULL << ONBOARD_LED;
  config.pull_down_en = GPIO_PULLDOWN_DISABLE;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&config);
  gpio_set_level(ONBOARD_LED, 0);

  // Start Sensor
  GRAVITY_WIRE.begin(I2C_SDA, I2C_SCL);
  if (!bme.begin(0x77)) { 
    Serial.println(F("No sensor detected"));
  } else {
    sensor_available = true;
    bme.setTemperatureOversampling(BME680_OS_8X);
    bme.setHumidityOversampling(BME680_OS_2X);
    bme.setPressureOversampling(BME680_OS_4X);
    bme.setIIRFilterSize(BME680_FILTER_SIZE_3);
    bme.setGasHeater(320, 150); // 320*C for 150 ms
  }
  return config;
}

camera_config_t setup_camera_config() {
  camera_config_t config;
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
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = -1;
  config.pin_reset = -1;

  config.xclk_freq_hz = 8000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 5;
  config.fb_count = 2;

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.println(F("Camera failed!"));
    exit(1);
  }  

  // Flip camera orientation
  sensor_t *s = esp_camera_sensor_get();
  s->set_vflip(s, 1);

  return config;
}

void setup_mDNS() {
  String hostname = "cam" + String(camera_num);
  while (!MDNS.begin(hostname)) {
    camera_num += 1;
    hostname = "cam" + String(camera_num);
    if (camera_num > total_cams) camera_num = 1; // Reset on 5
  }

  Serial.print(F("mDNS responder started: "));
  Serial.print(hostname);
  Serial.println(F(".local"));
}

void capture_sensor_reading() {
  if (!sensor_available) {
    Serial.println("[WARN] Failed to read. Reason: No sensor available");
  } else if (!bme.performReading()) {
        Serial.println("[WARN] Failed to read. Reason: No sensor data");
  } else {
    temperature[current_reading] = bme.temperature;
    pressure[current_reading] = bme.pressure / 100;
    humidity[current_reading] = bme.humidity;
    gas[current_reading] = bme.gas_resistance / 1000.0;

    Serial.print(F("Temperature = "));
    Serial.print(temperature[current_reading]);
    Serial.println(F(" *C"));

    Serial.print(F("Pressure = "));
    Serial.print(pressure[current_reading]);
    Serial.println(F(" hPa"));

    Serial.print(F("Humidity = "));
    Serial.print(humidity[current_reading]);
    Serial.println(F(" %"));

    Serial.print(F("Gas = "));
    Serial.print(gas[current_reading]);
    Serial.println(F(" Kohms"));
    Serial.println("");

    completed_full = (current_reading == readings - 1) || completed_full;
    current_reading = (current_reading + 1) % readings;
  }
}

// Captures an image and sensor data when called
void capture_image(void *pvParameters) {
  while (1) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb) {
      if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {  
        if (image_buffer != NULL) {
          free(image_buffer);
          image_buffer = NULL;
        }

        image_buffer = (uint8_t*)malloc(fb->len);
        memcpy(image_buffer, fb->buf, fb->len);
        image_length = fb->len;

        xSemaphoreGive(mutex);
      }
      
      gpio_set_level(ONBOARD_LED, 1);
      vTaskDelay(pdMS_TO_TICKS(flash_delay / 2));
      esp_camera_fb_return(fb);
      capture_sensor_reading();
      vTaskDelay(pdMS_TO_TICKS(flash_delay / 2));
      gpio_set_level(ONBOARD_LED, 0);
    }

    // Wait for N seconds before the next capture
    vTaskDelay(pdMS_TO_TICKS(frequency * 1000 - flash_delay)); 
  }
}

String array_to_json_str(float arr[]) {
  String json = "[";
  int max = completed_full ? readings : current_reading;
  for (int i = 0; i < max; ++i) {
    json += String(arr[i]);
    if (i != max - 1) {
      json += ",";
    }
  }
  return json + "]";
}

void setup_server() {
  server.on("/image", []() {
    if (xSemaphoreTake(mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (image_buffer != NULL) {
        server.send_P(200, "image/jpeg", (const char*)image_buffer, image_length);
      } else {
        server.send(404, "text/plain", "No image captured yet");
      }

      xSemaphoreGive(mutex);
    } else {
      server.send(503, "text/plain", "Server busy");
    }
  });

  server.on("/sensor/bulk", []() {
    if (sensor_available) {
      String json = "{";
      json.reserve(4000);

      json += "\"temperature\":" + array_to_json_str(temperature) + ",";
      json += "\"pressure\":" + array_to_json_str(pressure) + ",";
      json += "\"humidity\":" + array_to_json_str(humidity) + ",";
      json += "\"gas\":" + array_to_json_str(gas);
      json += "}";
      server.send(200, "application/json", json);
    } else {
      server.send(404, "text/plain", "Sensor unavailable");
    }
  });

  server.on("/sensor/latest", []() {
    if (sensor_available) {
      int latest = current_reading - 1;
      if (latest < 0) latest = readings - 1;

      String json = "{";
      json += "\"temperature\":" + String(temperature[latest]) + ",";
      json += "\"pressure\":" + String(pressure[latest]) + ",";
      json += "\"humidity\":" + String(humidity[latest]) + ",";
      json += "\"gas\":" + String(gas[latest]);
      json += "}";

      server.send(200, "application/json", json);
    } else {
      server.send(404, "text/plain", "Sensor unavailable");
    }
  });
}

void setup() {
  Serial.begin(115200);
  gpio_config_t gpio_config = setup_gpio_config();
  camera_config_t camera_config = setup_camera_config();

  mutex = xSemaphoreCreateMutex();
  xTaskCreate(capture_image, "capture_image", 8192, NULL, 5, NULL);

  WiFi.begin(ssid, password);
  setup_mDNS();
  setup_server();
  server.begin();
}

void loop() {
  server.handleClient();

  static unsigned long last_wifi_check = 0;
  if (millis() - last_wifi_check > wifi_check_frequency) {
    wl_status_t status = WiFi.status();
    if (status == WL_CONNECT_FAILED 
        || status == WL_CONNECTION_LOST 
        || status == WL_DISCONNECTED) {
      WiFi.begin(ssid, password);
    }
    last_wifi_check = millis();
  }
}