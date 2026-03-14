#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <math.h>

// ============================================================
// ESP DJ — Receptor MIDI
// Placa: ESP32-S3 DevKit (USB CDC)
// Recebe telemetria via ESP-NOW e converte para MIDI Serial
// Usar com: Hairless MIDI <-> loopMIDI -> Serato DJ Pro
// ============================================================

// ===== Configuração =====
static const uint32_t MIDI_SERIAL_BAUD = 115200;
static const uint32_t BOOT_DELAY_MS = 2000;
static const uint8_t PROTOCOL_VERSION = 2;
static const uint8_t ESPNOW_CHANNEL = 1;

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

// ===== Estado por Deck =====
struct DeckState {
  TelemetryPacket packet;
  bool pending;
  uint16_t last_pitch;
  bool last_moving;
};

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
DeckState decks[2] = {};
bool midi_ready = false;

// ===== Callback ESP-NOW =====
void onDataReceived(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len != sizeof(TelemetryPacket)) return;
  
  TelemetryPacket p;
  memcpy(&p, data, sizeof(p));

  if (p.deck_id >= 1 && p.deck_id <= 2) {
    digitalWrite(2, HIGH);
    portENTER_CRITICAL_ISR(&mux);
    decks[p.deck_id-1].packet = p;
    decks[p.deck_id-1].pending = true;
    portEXIT_CRITICAL_ISR(&mux);
  }
}

// ===== Funções MIDI (envio atômico) =====
void sendMidiPitchBend(uint8_t channel, uint16_t value) {
  uint8_t msg[3];
  msg[0] = 0xE0 | (channel & 0x0F);
  msg[1] = value & 0x7F;
  msg[2] = (value >> 7) & 0x7F;
  Serial.write(msg, 3);
}

void sendMidiNoteOn(uint8_t channel, uint8_t note, uint8_t velocity) {
  uint8_t msg[3];
  msg[0] = 0x90 | (channel & 0x0F);
  msg[1] = note & 0x7F;
  msg[2] = velocity & 0x7F;
  Serial.write(msg, 3);
}

void sendMidiNoteOff(uint8_t channel, uint8_t note) {
  uint8_t msg[3];
  msg[0] = 0x80 | (channel & 0x0F);
  msg[1] = note & 0x7F;
  msg[2] = 0;
  Serial.write(msg, 3);
}

// ===== Setup =====
void setup() {
  pinMode(2, OUTPUT); 
  digitalWrite(2, LOW);
  
  Serial.begin(MIDI_SERIAL_BAUD);
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_now_init();
  esp_now_register_recv_cb(onDataReceived);
  
  pinMode(2, OUTPUT);

  // Espera o boot USB CDC estabilizar
  delay(BOOT_DELAY_MS);
  while (Serial.available()) Serial.read();
  Serial.flush();
  midi_ready = true;
}

// ===== Loop principal =====
void loop() {
  if (!midi_ready) return;

  for (int i = 0; i < 2; i++) {
    TelemetryPacket p;
    bool has_data = false;

    portENTER_CRITICAL(&mux);
    if (decks[i].pending) {
      p = decks[i].packet;
      decks[i].pending = false;
      has_data = true;
    }
    portEXIT_CRITICAL(&mux);

    if (has_data) {
      uint8_t ch = i; // Canal 0 = Deck A, Canal 1 = Deck B
      
      // Posição angular -> Pitch Bend (14-bit, 0-16383)
      float val = fmodf(p.angular_position_deg, 360.0f);
      if (val < 0) val += 360.0f;
      uint16_t pitch = (uint16_t)((val / 360.0f) * 16383.0f);
      
      sendMidiPitchBend(ch, pitch);

      // Detecção de toque (Note 61 = Platter Touch no Serato)
      bool is_moving = (p.flags & (1 << 5)) != 0; 
      
      if (is_moving != decks[i].last_moving) {
        if (is_moving) {
          sendMidiNoteOn(ch, 61, 127);
        } else {
          sendMidiNoteOff(ch, 61);
        }
        decks[i].last_moving = is_moving;
      }
    }
  }
  
  // LED pisca ao receber dados
  static uint32_t last_pulse = 0;
  if (millis() - last_pulse > 20) {
    digitalWrite(2, LOW);
    last_pulse = millis();
  }
}
