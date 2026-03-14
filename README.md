ESP DJ Giroscopio


# ESP DJ — Controlador MIDI com Giroscópio

Sistema de controle MIDI wireless para DJ usando ESP32 + MPU6050.  
Converte movimentos de rotação em mensagens MIDI para controlar pratos virtuais no Serato DJ Pro.

---

## 📦 Conteúdo do Projeto

```
release/
├── transmissor_a/          # Firmware do Deck A (ESP32)
│   └── transmissor_a.ino
├── transmissor_b/          # Firmware do Deck B (ESP32)
│   └── transmissor_b.ino
├── receptor/               # Firmware do Receptor MIDI (ESP32-S3)
│   └── receptor.ino
└── README.md               # Este ficheiro
```

---

## 🔧 Hardware Necessário

### Por cada Transmissor (Deck A / Deck B):
| Componente | Descrição |
|---|---|
| **ESP32 DevKit V1** | ESP-WROOM-32 (não é S3) |
| **MPU6050** | Módulo giroscópio/acelerómetro |
| **Botão** | Botão momentâneo para calibração |
| **LED** | LED indicador de status (opcional, usa o LED integrado) |

### Receptor:
| Componente | Descrição |
|---|---|
| **ESP32-S3 DevKit** | Com porta USB nativa (CDC) |

### Ligações do MPU6050 ao ESP32:
| MPU6050 | ESP32 |
|---|---|
| VCC | 3.3V |
| GND | GND |
| SDA | GPIO 21 |
| SCL | GPIO 22 |

### Botão de Calibração:
| Botão | ESP32 |
|---|---|
| Um terminal | GPIO 4 |
| Outro terminal | GND |

---

## 💻 Software Necessário no PC

1. **Arduino IDE** ou **arduino-cli** com o pacote ESP32 instalado
2. **[loopMIDI](https://www.tobias-erichsen.de/software/loopmidi.html)** — Cria uma porta MIDI virtual no Windows
3. **[Hairless MIDI Serial Bridge](https://projectgus.github.io/hairless-midiserial/)** — Converte Serial para MIDI
4. **Serato DJ Pro** (ou outro software DJ com suporte a MIDI)

---

## 🚀 Guia de Instalação

### 1. Instalar Cores do Arduino
No Arduino IDE, adicione o URL do ESP32 nas preferências:
```
https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
```
Depois instale o pacote **esp32 by Espressif Systems**.

### 2. Carregar Firmware nos Transmissores

#### Transmissor A (Deck A):
- Abra `transmissor_a/transmissor_a.ino`
- Selecione a placa: **ESP32 Dev Module**
- Selecione a porta COM correspondente
- Carregue (Upload)

#### Transmissor B (Deck B):
- Abra `transmissor_b/transmissor_b.ino`
- Selecione a placa: **ESP32 Dev Module**
- Selecione a porta COM correspondente
- Carregue (Upload)

### 3. Carregar Firmware no Receptor

- Abra `receptor/receptor.ino`
- Selecione a placa: **ESP32S3 Dev Module**
- **Importante — Opções da placa:**
  - USB Mode: **Hardware CDC and JTAG**
  - USB CDC On Boot: **Enabled**
- Selecione a porta COM correspondente
- Carregue (Upload)

### 4. Configurar o PC

#### loopMIDI:
1. Abra o loopMIDI
2. Crie uma nova porta com o nome: `ESP_MIDI`
3. Deixe o loopMIDI aberto (minimizado na bandeja)

#### Hairless MIDI Serial Bridge:
1. Abra o Hairless
2. **Serial port:** Selecione a COM do Receptor (ESP32-S3)
3. **MIDI Out:** Selecione `ESP_MIDI`
4. Marque **Serial <-> MIDI Bridge On**
5. As luzes verdes devem acender indicando que a ponte está ativa

#### Serato DJ Pro:
1. Abra o Serato DJ Pro
2. Vá em **Settings → MIDI**
3. Ative o dispositivo `ESP_MIDI`
4. Clique no botão **MIDI** na barra superior para entrar no modo de mapeamento
5. Clique no **jog wheel** do Deck A e mova o Transmissor A
6. Clique na área de **toque do prato** e toque/solte o sensor
7. Repita para o Deck B se necessário
8. Saia do modo MIDI

---

## 🎮 Como Usar

1. **Ligue os transmissores** — O LED pisca durante a calibração (~1 segundo)
2. **Mantenha os transmissores parados** durante o boot para calibrar o zero
3. **Mova o sensor** para controlar o prato virtual
4. **Pressione o botão** (GPIO 4) para recalibrar a qualquer momento

---

## 📡 Arquitetura do Sistema

```
┌─────────────────┐     ESP-NOW      ┌──────────────┐     USB Serial     ┌──────────┐
│  Transmissor A  │ ───────────────→ │              │ ──────────────────→ │          │
│  (ESP32 + MPU)  │    Canal 1       │   Receptor   │    MIDI bytes      │ Hairless │
│  Deck A = ID 1  │                  │  (ESP32-S3)  │    115200 baud     │  MIDI    │
└─────────────────┘                  │              │                    │  Bridge  │
                                     │  Converte    │                    │          │
┌─────────────────┐     ESP-NOW      │  telemetria  │                    │          │
│  Transmissor B  │ ───────────────→ │  para MIDI   │     loopMIDI      │          │
│  (ESP32 + MPU)  │    Canal 1       │              │ ←─────────────────→│          │
│  Deck B = ID 2  │                  └──────────────┘    ESP_MIDI port   └──────────┘
└─────────────────┘                                                           │
                                                                              │ MIDI
                                                                              ▼
                                                                     ┌──────────────┐
                                                                     │  Serato DJ   │
                                                                     │     Pro      │
                                                                     └──────────────┘
```

---

## 📋 Mensagens MIDI Enviadas

| Mensagem | Canal | Descrição |
|---|---|---|
| **Pitch Bend** (0xE0) | 0 (Deck A) / 1 (Deck B) | Posição angular (0-360° → 0-16383) |
| **Note On** (0x90) Note 61, Vel 127 | 0 / 1 | Platter Touch — mão tocou o sensor |
| **Note Off** (0x80) Note 61, Vel 0 | 0 / 1 | Platter Release — mão saiu do sensor |

---

## ⚙️ Parâmetros Ajustáveis

Estes valores podem ser alterados no código dos transmissores:

| Parâmetro | Deck A | Deck B | Descrição |
|---|---|---|---|
| `GYRO_DEADBAND_DPS` | 0.20 | 0.35 | Zona morta do giroscópio (°/s) |
| `TOUCH_THRESHOLD_DPS` | 0.80 | 0.80 | Limiar para considerar "movendo" |
| `FILTER_ALPHA_SLOW` | 0.18 | 0.18 | Suavização em movimentos lentos |
| `FILTER_ALPHA_FAST` | 0.68 | 0.68 | Resposta em movimentos rápidos |
| `REVERSAL_THRESHOLD_DPS` | 45.0 | 45.0 | Limiar para detectar inversão de direção |
| `BOOST_MULTIPLIER` | 1.18 | 1.18 | Amplificação em inversões de scratch |
| `SAMPLE_RATE_HZ` | 500 | 500 | Taxa de amostragem do sensor |

---

## 🔍 Resolução de Problemas

### O Transmissor mostra "MPU FAIL"
- Verifique as ligações SDA/SCL
- Confirme que o MPU6050 está em 3.3V
- Verifique se o endereço I2C é 0x68 (AD0 ligado ao GND)

### O Receptor não recebe dados (LED não pisca)
- Confirme que todos os dispositivos estão no mesmo canal WiFi (`ESPNOW_CHANNEL = 1`)
- O MAC do receptor está em modo broadcast (0xFF:...) — deve funcionar automaticamente

### O Hairless mostra erro "unexpected data byte"
- Feche o Hairless, reinicie o receptor (desligar/ligar USB), abra o Hairless novamente
- O receptor tem um delay de 2 segundos no boot para evitar isso

### O Serato não responde ao MIDI
- Confirme que `ESP_MIDI` está ativado em Settings → MIDI
- Entre no modo de mapeamento MIDI (botão "MIDI" no topo do Serato)
- Mapeie manualmente o Pitch Bend ao jog wheel

---

## 📄 Licença

Projeto de uso pessoal. Código livre para modificação e redistribuição.

---

*Última atualização: Março 2026*
