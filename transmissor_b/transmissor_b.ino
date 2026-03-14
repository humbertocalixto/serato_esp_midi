#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>

// ============================================================
// ESP DJ — Transmissor Deck B
// Placa: ESP32 DevKit V1 / ESP-WROOM-32
// Sensor: MPU6050 (Giroscópio eixo Z)
// Comunicação: ESP-NOW (Broadcast)
// ============================================================

// ===== Hardware config =====
#define DECK_ID 2
static const int PIN_MPU_SDA = 21;
static const int PIN_MPU_SCL = 22;
static const int PIN_CAL_BUTTON = 4;
static const int PIN_STATUS_LED = 2;
static const int PIN_BATTERY_SENSE = -1;

// ===== Comunicação =====
static const uint8_t ESPNOW_CHANNEL = 1;
static const uint8_t RECEIVER_MAC[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}; // Broadcast

// ===== Parâmetros de sensibilidade =====
static const uint16_t SAMPLE_RATE_HZ = 500;
static const uint16_t CALIBRATION_SAMPLES = 400;
static const float GYRO_DEADBAND_DPS = 0.35f;
static const float TOUCH_THRESHOLD_DPS = 0.80f;
static const float FILTER_ALPHA_SLOW = 0.18f;
static const float FILTER_ALPHA_FAST = 0.68f;
static const float REVERSAL_THRESHOLD_DPS = 45.0f;
static const float BOOST_MULTIPLIER = 1.18f;
static const float AUTO_ZERO_STILLNESS_DPS = 1.5f;
static const uint32_t AUTO_ZERO_SETTLE_MS = 1400;
static const float AUTO_ZERO_POSITION_BLEND = 0.10f;
static const float AUTO_ZERO_BIAS_BLEND = 0.02f;
static const uint16_t BATTERY_DIVIDER_RATIO_MILLI = 2000;

// ===== Protocolo de telemetria =====
struct __attribute__((packed)) TelemetryPacket {
  uint8_t version;
  uint8_t deck_id;
  uint16_t sequence;
  uint32_t timestamp_us;
  float gyro_z_dps;
  float angular_velocity_dps;
  float angular_position_deg;
  uint16_t battery_mv;
  uint8_t flags;
  uint8_t reserved[3];
};

enum PacketFlags : uint8_t {
  FLAG_SENSOR_OK = 1 << 0,
  FLAG_BATTERY_VALID = 1 << 1,
  FLAG_CALIBRATING = 1 << 2,
  FLAG_BUTTON_PRESSED = 1 << 3,
  FLAG_REVERSAL = 1 << 4,
  FLAG_MOVING = 1 << 5,
};

static const uint8_t PROTOCOL_VERSION = 2;

// ===== Driver MPU6050 simplificado =====
class MPU6050Lite {
 public:
  bool begin(TwoWire& wire, uint8_t address = 0x68) {
    wire_ = &wire;
    address_ = address;
    if (!writeRegister(0x6B, 0x00)) return false;
    delay(50);
    uint8_t who_am_i = 0;
    if (!readWhoAmI(who_am_i) || who_am_i != 0x68) return false;
    if (!writeRegister(0x1B, 0x18)) return false;  // ±2000 dps
    if (!writeRegister(0x1A, 0x03)) return false;  // DLPF ~44Hz
    if (!writeRegister(0x19, 0x01)) return false;  // Sample rate divider
    return true;
  }

  bool readGyroZ(float& gyro_z_dps) {
    uint8_t data[2] = {};
    if (!readRegisters(0x47, data, sizeof(data))) return false;
    const int16_t raw_z = static_cast<int16_t>((data[0] << 8) | data[1]);
    gyro_z_dps = static_cast<float>(raw_z) / 16.4f;
    return true;
  }

 private:
  bool readWhoAmI(uint8_t& value) { return readRegisters(0x75, &value, 1); }
  bool writeRegister(uint8_t reg, uint8_t value) {
    wire_->beginTransmission(address_);
    wire_->write(reg);
    wire_->write(value);
    return wire_->endTransmission() == 0;
  }
  bool readRegisters(uint8_t start_reg, uint8_t* data, size_t length) {
    wire_->beginTransmission(address_);
    wire_->write(start_reg);
    if (wire_->endTransmission(false) != 0) return false;
    const size_t read_count = wire_->requestFrom(static_cast<int>(address_), static_cast<int>(length));
    if (read_count != length) return false;
    for (size_t i = 0; i < length; ++i) data[i] = wire_->read();
    return true;
  }
  TwoWire* wire_ = nullptr;
  uint8_t address_ = 0x68;
};

// ===== Variáveis globais =====
MPU6050Lite imu;
TelemetryPacket packet = {};
esp_now_peer_info_t peer = {};

float gyro_bias_dps = 0.0f;
float angular_position_deg = 0.0f;
float filtered_velocity_dps = 0.0f;
uint32_t last_sample_us = 0;
uint32_t last_button_change_ms = 0;
uint32_t last_send_status_ms = 0;
bool button_state = false;
bool sensor_ok = false;
bool calibrating = false;
bool reversal_detected = false;
bool moving_detected = false;
bool espnow_ready = false;
bool peer_ready = false;
bool last_send_ok = false;
bool has_send_result = false;
uint32_t still_since_ms = 0;

uint32_t samplePeriodUs() { return 1000000UL / SAMPLE_RATE_HZ; }
bool statusLedEnabled() { return PIN_STATUS_LED >= 0; }
bool batterySenseEnabled() { return PIN_BATTERY_SENSE >= 0; }

void onEspNowSend(const wifi_tx_info_t*, esp_now_send_status_t status) {
  last_send_ok = status == ESP_NOW_SEND_SUCCESS;
  has_send_result = true;
}

void setStatusLed(bool state) {
  if (statusLedEnabled()) digitalWrite(PIN_STATUS_LED, state ? HIGH : LOW);
}

bool readButtonPressed() {
  const bool raw = digitalRead(PIN_CAL_BUTTON) == LOW;
  const uint32_t now = millis();
  if (raw != button_state && now - last_button_change_ms > 30) {
    button_state = raw;
    last_button_change_ms = now;
  }
  return button_state;
}

uint16_t readBatteryMillivolts() {
  if (!batterySenseEnabled()) return 0;
  const uint16_t raw = analogRead(PIN_BATTERY_SENSE);
  const float sensed_v = (static_cast<float>(raw) / 4095.0f) * 3.3f;
  const float battery_v = sensed_v * (static_cast<float>(BATTERY_DIVIDER_RATIO_MILLI) / 1000.0f);
  return static_cast<uint16_t>(battery_v * 1000.0f);
}

bool configureEspNow() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) return false;
  if (esp_now_init() != ESP_OK) return false;
  espnow_ready = true;
  esp_now_register_send_cb(onEspNowSend);
  memset(&peer, 0, sizeof(peer));
  memcpy(peer.peer_addr, RECEIVER_MAC, sizeof(RECEIVER_MAC));
  peer.channel = ESPNOW_CHANNEL;
  peer.encrypt = false;
  peer_ready = esp_now_add_peer(&peer) == ESP_OK;
  return peer_ready;
}

bool calibrateGyroBias() {
  calibrating = true;
  setStatusLed(true);
  float accumulator = 0.0f;
  uint16_t collected = 0;
  for (uint16_t i = 0; i < CALIBRATION_SAMPLES; ++i) {
    float gyro_z_dps = 0.0f;
    if (!imu.readGyroZ(gyro_z_dps)) {
      calibrating = false;
      return false;
    }
    accumulator += gyro_z_dps;
    ++collected;
    delay(2);
  }
  gyro_bias_dps = collected ? accumulator / collected : 0.0f;
  angular_position_deg = 0.0f;
  filtered_velocity_dps = 0.0f;
  reversal_detected = false;
  moving_detected = false;
  still_since_ms = millis();
  last_sample_us = micros();
  calibrating = false;
  setStatusLed(false);
  return true;
}

float applyScratchFilter(float corrected_gyro_z_dps) {
  const bool sign_change =
      (filtered_velocity_dps > REVERSAL_THRESHOLD_DPS && corrected_gyro_z_dps < -REVERSAL_THRESHOLD_DPS) ||
      (filtered_velocity_dps < -REVERSAL_THRESHOLD_DPS && corrected_gyro_z_dps > REVERSAL_THRESHOLD_DPS);
  const float delta = fabsf(corrected_gyro_z_dps - filtered_velocity_dps);
  const bool fast_motion = delta > REVERSAL_THRESHOLD_DPS;
  const float alpha = (sign_change || fast_motion) ? FILTER_ALPHA_FAST : FILTER_ALPHA_SLOW;
  filtered_velocity_dps += alpha * (corrected_gyro_z_dps - filtered_velocity_dps);
  if (fabsf(filtered_velocity_dps) < GYRO_DEADBAND_DPS) filtered_velocity_dps = 0.0f;
  reversal_detected = sign_change;
  moving_detected = (fabsf(filtered_velocity_dps) > TOUCH_THRESHOLD_DPS) || (fabsf(corrected_gyro_z_dps) > TOUCH_THRESHOLD_DPS * 2.0f);
  if (reversal_detected) filtered_velocity_dps *= BOOST_MULTIPLIER;
  return filtered_velocity_dps;
}

void updateAutoZero(float raw_gyro_z_dps, float filtered_dps) {
  const uint32_t now_ms = millis();
  const bool still_now =
      fabsf(raw_gyro_z_dps - gyro_bias_dps) <= AUTO_ZERO_STILLNESS_DPS &&
      fabsf(filtered_dps) <= AUTO_ZERO_STILLNESS_DPS;
  if (!still_now) {
    still_since_ms = now_ms;
    return;
  }
  if (still_since_ms == 0) still_since_ms = now_ms;
  if (now_ms - still_since_ms < AUTO_ZERO_SETTLE_MS) return;
  gyro_bias_dps = (1.0f - AUTO_ZERO_BIAS_BLEND) * gyro_bias_dps + AUTO_ZERO_BIAS_BLEND * raw_gyro_z_dps;
  angular_position_deg *= (1.0f - AUTO_ZERO_POSITION_BLEND);
  if (fabsf(angular_position_deg) < 0.05f) angular_position_deg = 0.0f;
}

void populatePacket(float raw_gyro_z_dps, float corrected_gyro_z_dps) {
  packet.version = PROTOCOL_VERSION;
  packet.deck_id = DECK_ID;
  packet.sequence++;
  packet.timestamp_us = micros();
  packet.gyro_z_dps = raw_gyro_z_dps;
  packet.angular_velocity_dps = corrected_gyro_z_dps;
  packet.angular_position_deg = angular_position_deg;
  packet.battery_mv = readBatteryMillivolts();
  packet.flags = 0;
  if (sensor_ok) packet.flags |= FLAG_SENSOR_OK;
  if (packet.battery_mv > 0) packet.flags |= FLAG_BATTERY_VALID;
  if (calibrating) packet.flags |= FLAG_CALIBRATING;
  if (readButtonPressed()) packet.flags |= FLAG_BUTTON_PRESSED;
  if (reversal_detected) packet.flags |= FLAG_REVERSAL;
  if (moving_detected) packet.flags |= FLAG_MOVING;
}

// ===== Setup =====
void setup() {
  Serial.begin(115200);
  delay(250);
  Serial.println("TX-B BOOT");
  pinMode(PIN_CAL_BUTTON, INPUT_PULLUP);
  if (statusLedEnabled()) {
    pinMode(PIN_STATUS_LED, OUTPUT);
    setStatusLed(false);
  }
  if (batterySenseEnabled()) analogReadResolution(12);
  Wire.begin(PIN_MPU_SDA, PIN_MPU_SCL, 400000);
  sensor_ok = imu.begin(Wire);
  Serial.println(sensor_ok ? "MPU OK" : "MPU FAIL");
  const bool esp_now_ok = configureEspNow();
  Serial.println(espnow_ready ? "TX OK" : "TX FAIL");
  Serial.println(esp_now_ok ? "PAIR OK" : "PAIR FAIL");
  if (sensor_ok) {
    sensor_ok = calibrateGyroBias();
    Serial.println(sensor_ok ? "CAL OK" : "CAL FAIL");
  }
}

// ===== Loop principal =====
void loop() {
  if (!sensor_ok) {
    delay(250);
    return;
  }
  if (readButtonPressed() && !calibrating) {
    sensor_ok = calibrateGyroBias();
    delay(100);
  }
  const uint32_t now_us = micros();
  if (now_us - last_sample_us < samplePeriodUs()) return;
  const float dt_s = static_cast<float>(now_us - last_sample_us) / 1000000.0f;
  last_sample_us = now_us;
  float raw_gyro_z_dps = 0.0f;
  sensor_ok = imu.readGyroZ(raw_gyro_z_dps);
  if (!sensor_ok) {
    delay(50);
    return;
  }
  float corrected_gyro_z_dps = raw_gyro_z_dps - gyro_bias_dps;
  if (fabsf(corrected_gyro_z_dps) < GYRO_DEADBAND_DPS) corrected_gyro_z_dps = 0.0f;
  const float filtered_dps = applyScratchFilter(corrected_gyro_z_dps);
  updateAutoZero(raw_gyro_z_dps, filtered_dps);
  angular_position_deg += filtered_dps * dt_s;
  populatePacket(raw_gyro_z_dps, filtered_dps);
  const esp_err_t send_result =
      esp_now_send(peer.peer_addr, reinterpret_cast<const uint8_t*>(&packet), sizeof(packet));
  if (send_result != ESP_OK) {
    Serial.println("SEND FAIL");
  } else if (has_send_result && millis() - last_send_status_ms > 100) {
    Serial.printf("[%s] Gyro_Z: %.2f | Filt_DPS: %.2f | Pos: %.2f\n",
                  last_send_ok ? "OK" : "FAIL", raw_gyro_z_dps, filtered_dps, angular_position_deg);
    last_send_status_ms = millis();
    has_send_result = false;
  }
  if (statusLedEnabled()) setStatusLed(moving_detected);
}
