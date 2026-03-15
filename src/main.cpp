#define CORE_DEBUG_LEVEL 3

#include <Arduino.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <WiFiMulti.h>

#include <mutex>
#include <thread>

#include "credentials.h"

class FroniusMeter {
 private:
  // Returns a reference to the specific JSON node to prevent path duplication
  auto get_target_node(JsonDocument& doc) { return doc["Body"]["Data"]["0"]["PowerReal_P_Sum"]; }

 public:
  FroniusMeter(String hostname) {
    m_url = "http://" + hostname + "/solar_api/v1/GetMeterRealtimeData.cgi?Scope=System";
  }

  bool read(float& value) {
    m_http.begin(m_url);
    bool success = false;

    if (m_http.GET() == HTTP_CODE_OK) {
      // 1. Set up the filter using the helper method
      JsonDocument filter_doc;
      get_target_node(filter_doc) = true;

      // 2. Parse directly from the TCP stream
      JsonDocument doc;
      DeserializationOption::Filter filter{filter_doc};
      DeserializationError error = deserializeJson(doc, m_http.getStream(), std::move(filter));

      if (!error) {
        // 3. Extract the value using the same helper method
        value = get_target_node(doc).as<float>();
        success = true;
      }
    }

    m_http.end();
    return success;
  }

 private:
  String m_url;
  HTTPClient m_http;
};

template <int Size>
class Display {
 public:
  Display() {
    FastLED.addLeds<WS2812B, D4, GRB>(m_leds, Size).setCorrection(TypicalLEDStrip);
    FastLED.setBrightness(255);
    FastLED.setDither(BINARY_DITHER);

    for (uint8_t i = 0; i < Size; ++i) {
      m_green[i] = HUE_GREEN - 10 + i * 3;
      m_red[i] = HUE_RED + i * 3;
    }
  }

  void display(float grid) {
    light(10000, fabs(grid), (grid <= 0) ? m_green : m_red);
    animate(10000, fabs(grid));
  }

 private:
  void animate(float max, float value) {
    const float ratio = constrain(value / max, 0, 1);

    const uint8_t fade = beatsin8(25, 0, 64 + 128 * ratio);
    for (int i = 0; i < Size; ++i) {
      m_leds[i] = m_leds_internal[i];
      m_leds[i].fadeLightBy(fade);
    }

    if (m_effect) {
      uint8_t pos = cubicwave8(beat8(60, m_timebase));
      pos = map8(pos, 0, 10);
      if (pos == 10) {
        m_effect = false;
      } else {
        if (m_leds[pos]) {
          m_leds[pos] = CRGB::White;
          m_leds[pos].fadeLightBy(162 + 10 * pos);
          // log_i("m_leds[%d]=%d", pos, m_leds[pos].getAverageLight());
        }
      }
    } else {
      const uint16_t limit = 512 + 4096 * (1 - ratio);
      if (random16(limit) == 0) {
        m_effect = true;
        m_timebase = millis();
      }
    }
  }

  void light(float max, float value, uint8_t* palette) {
    const float bucket = max / Size;
    // log_i("######### value=%f", value);
    for (int i = 0; i < Size; ++i) {
      uint8_t fill = constrain(255 * value / bucket, 0, 255);
      // m_leds_internal[i] = blend(m_leds_internal[i], CHSV(palette[i], 255, fill), 5);
      nblend(m_leds_internal[i], CHSV(palette[i], 240, fill), 32);
      // m_leds_internal[i] = CHSV(palette[i], 240, fill);
      value -= bucket;
      // log_i("m_leds[%d]=%d", i, fill);
    }
  }

 private:
  uint8_t m_green[Size] = {0};
  uint8_t m_red[Size] = {0};

  CRGB m_leds[Size] = {0};
  CRGB m_leds_internal[Size] = {0};
  bool m_effect = false;
  uint32_t m_timebase = 0;
};

WiFiMulti wifi;
FroniusMeter fronius_meter{hostname};

volatile float grid = 0.f;
std::mutex mutex;

void display_loop(void*) {
  Display<10> display;

  while (true) {
    float value = 0;
    {
      std::lock_guard<std::mutex> _(mutex);
      value = grid;
    }
    display.display(value);
    //  display.animate();
    // log_i("d");
    FastLED.show();

    // Yield to FreeRTOS for 5ms to prevent watchdog resets
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  // Never reach here
  vTaskDelete(nullptr);
}

void setup() {
  delay(1000);  // power-up safety delay

  Serial.begin(115200);

  WiFi.setHostname("fronius-ws2812");
  wifi.addAP(wifi_ssid, wifi_pass);

  xTaskCreatePinnedToCore(display_loop, "display", 3000, nullptr, 1, nullptr, 0);
}

void loop() {
  EVERY_N_SECONDS(3) {
    // log_i("=> %d", millis());
    if (wifi.run() == WL_CONNECTED) {
      float value = 0;

      if (fronius_meter.read(value)) {
        log_i("grid: %fW", value);
        std::lock_guard<std::mutex> _(mutex);
        grid = value;  //(grid < 0) ? 2000 : -3000;
      } else {
        log_e("Unable to read data");
      }

      // {
      //   std::lock_guard<std::mutex> _(mutex);
      //   grid = (grid < 0) ? 10000 : -10000;
      // }

      // const auto consumption = max(0.f, production + grid);

      // log_i("production: %fW", production);
      // log_i("consumption: %fW", consumption);

      // display.display(grid);
    }
    // log_i("<= %d", millis());
  }
  // EVERY_N_SECONDS(2) {
  //   std::lock_guard<std::mutex> _(mutex);
  //   static int counter = 0;
  //   // grid = counter;
  //   grid = (grid < 0) ? 10000 : -10000;
  //   counter += 500;
  // }
}