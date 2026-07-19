# IrrigaHome 🌱

Sistema de irrigação residencial inteligente com controle via app Android, firmware ESP32 e integração com Firebase.

---

## Visão Geral

O IrrigaHome é uma solução completa de irrigação automatizada para uso residencial ou de pequena escala. O sistema combina três componentes principais:

- **App Android** (Kotlin + Jetpack Compose) — controle, monitoramento em tempo real e histórico de irrigações
- **Firmware ESP32** (Arduino/C++) — leitura de sensores, acionamento da bomba e operação local independente de nuvem
- **Backend Firebase** (Firestore + Auth + Cloud Functions Node.js) — autenticação, histórico persistente e automações

```
┌─────────────────┐        MQTT/TLS        ┌─────────────────────┐
│   App Android   │ ◄────────────────────► │     ESP32           │
│  (Kotlin/Compose)│                       │  (Firmware C++)     │
└────────┬────────┘                        └──────────┬──────────┘
         │                                            │
         │  Firebase SDK                   HTTP (Cloud Functions)
         ▼                                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                        Firebase                                  │
│          Auth  ·  Firestore  ·  Cloud Functions (Node.js)        │
└─────────────────────────────────────────────────────────────────┘
```

---

## Funcionalidades

### App Mobile
- Login e registro com Firebase Authentication (email/senha)
- Auto-login e auto-logout por tempo em background (30 s)
- Dashboard com telemetria em tempo real: umidade do solo, temperatura, umidade do ar e nível da caixa d'água
- Irrigação manual com contador regressivo, cooldown visual e detecção de conflito com agendamentos
- Modo automático configurável (limiar de solo, duração, cooldown)
- Até 4 agendamentos por dispositivo com seletor de dias da semana e horário
- Histórico de eventos filtráveis por tipo (manual/automático/agendado) e por período
- Suporte a múltiplos usuários com vinculação de dispositivo por MAC address

### Firmware ESP32
- Operação local autônoma sem dependência de nuvem
- Irrigação manual, automática por sensor de solo e por agendamento
- Sensores: DHT22 (temperatura + umidade do ar), solo resistivo (ADC), bóia de nível
- Fluxômetro com dois modos de calibração (simples e regressão linear por mínimos quadrados)
- Display TFT ST7789 240×320 com dashboard animado e carrossel de cabeçalho
- Web server de manutenção (HTTP) com interface SPA para calibração de sensores e diagnóstico
- Persistência de configurações e agendamentos na NVS e histórico em LittleFS
- Recuperação automática pós-reboot de irrigação interrompida
- Telemetria delta via MQTT (publica apenas quando há mudança)
- Duplo núcleo: Core 1 (controle/sensores) e Core 0 (MQTT, Firebase, WebServer)

---

## Arquitetura

### App Android — MVVM + Clean Architecture

```
Presentation  →  Domain  →  Data
  (Compose)      (UseCases  (Firebase /
  (ViewModel)    + Models)   SharedPrefs)
```

| Camada | Responsabilidade |
|---|---|
| `presentation/` | Telas Compose + ViewModels (AuthViewModel, IrrigaViewModel, HistoricoViewModel) |
| `domain/` | Modelos de negócio, interfaces de repositório e use cases |
| `data/remote/` | Repositórios Firebase (Firestore, Auth) |
| `data/local/` | Repositórios SharedPreferences (settings, login, device identity) |
| `util/` | MqttManager, ScheduleConflictDetector, SessionManager |
| `di/` | Módulos Hilt (AppModule, RepositoryModule) |

### Firmware ESP32 — Orientado a Gerenciadores

| Arquivo | Responsabilidade |
|---|---|
| `main.ino` | Orquestrador principal, ciclo de controle de 2 s, watchdog |
| `config.h` | Constantes, pinos e inclusão de `secrets.h` |
| `wifi_manager` | Wi-Fi com backoff exponencial + sincronização NTP |
| `mqtt_manager` | MQTT/TLS, telemetria delta, fila Firebase assíncrona |
| `sensor_manager` | DHT22, solo ADC (6 amostras), bóia de nível, offsets calibráveis |
| `actuator_manager` | Relé da bomba, modos, agendamentos (NVS) |
| `irrigation_event_manager` | Histórico JSONL em LittleFS, fila MQTT, deduplicação |
| `display_manager` | Dashboard TFT ST7789, dirty flags, carrossel |
| `web_server_manager` | SPA de manutenção (HTTP/80), offsets e diagnóstico |

---

## Stack Tecnológica

### App Android
| Tecnologia | Versão |
|---|---|
| Kotlin + Jetpack Compose | BOM 2024.09.00 |
| Hilt (injeção de dependência) | 2.59.2 |
| KSP | 2.2.10-2.0.2 |
| Firebase BOM | 32.7.0 |
| Eclipse Paho MQTT | 1.2.5 |
| Lifecycle ViewModel Compose | 2.10.0 |
| Java target | 11 |
| minSdk | 24 (Android 7.0) |

### Firmware ESP32
| Tecnologia | Detalhe |
|---|---|
| Arduino/C++ para ESP32 | Dual-core (Xtensa LX6) |
| PubSubClient + WiFiClientSecure | MQTT sobre TLS (porta 8883) |
| DHT22 | Temperatura e umidade do ar |
| Adafruit ST7789 | Display TFT 240×320 |
| LittleFS | Histórico de eventos em JSONL |
| NVS (Preferences) | Configurações e agendamentos persistentes |
| FreeRTOS | Tasks, queues e mutexes multi-core |

### Backend
| Tecnologia | Detalhe |
|---|---|
| Firebase Authentication | Login email/senha + reset de senha |
| Firebase Firestore | Dispositivos, histórico e agendamentos |
| Firebase Cloud Functions | Node.js v2 — recebe eventos do ESP32 via HTTP |
| HiveMQ Cloud | Broker MQTT gerenciado, SSL porta 8883 |

---

## Estrutura do Projeto

```
IrrigaHome/
├── app/                              # App Android
│   └── src/main/java/com/nr/irrigahome/
│       ├── di/                       # Módulos Hilt
│       ├── domain/                   # Modelos, interfaces, use cases
│       ├── data/                     # Repositórios (remote + local)
│       ├── presentation/             # Telas e ViewModels
│       ├── ui/                       # Tema Material 3 e componentes
│       └── util/                     # MQTT, sessão, conflito de agenda
├── Esp32 Firmware - Irriga Home/
│   └── main/                         # Firmware C++/Arduino
│       ├── secrets.example.h         # Template de credenciais (copie para secrets.h)
│       └── ...                       # Demais módulos do firmware
├── irrigahome-functions/             # Firebase Cloud Functions
│   └── functions/
│       ├── index.js
│       └── .env.example              # Template de variáveis de ambiente
├── local.properties.example          # Template de credenciais Android
└── app/google-services.json.example  # Template Firebase Android
```

---

## Configuração e Instalação

### Pré-requisitos

- Android Studio Hedgehog ou superior
- Arduino IDE 2.x com suporte ao ESP32 (ESP32 Arduino Core 2.x)
- Conta Firebase com projeto configurado
- Broker MQTT (ex: HiveMQ Cloud — plano gratuito disponível)
- Node.js 18+ (para deploy das Cloud Functions)

---

### 1. Clone o repositório

```bash
git clone https://github.com/educadiz/irrigahome.git
cd irrigahome
```

---

### 2. Configurar o App Android

Copie o template de credenciais:

```bash
cp local.properties.example local.properties
```

Edite `local.properties` com suas credenciais MQTT:

```properties
sdk.dir=/caminho/para/Android/Sdk
mqtt.brokerUrl=ssl://SEU_BROKER.s1.eu.hivemq.cloud:8883
mqtt.username=SEU_USUARIO_MQTT
mqtt.password=SUA_SENHA_MQTT
```

Baixe o arquivo `google-services.json` do seu projeto Firebase ([Console Firebase](https://console.firebase.google.com)) e coloque em `app/google-services.json`.

Abra o projeto no Android Studio e execute o build normalmente.

---

### 3. Configurar o Firmware ESP32

Copie o template de credenciais:

```bash
cp "Esp32 Firmware - Irriga Home/main/secrets.example.h" \
   "Esp32 Firmware - Irriga Home/main/secrets.h"
```

Edite `secrets.h` com seus dados:

```cpp
#define WIFI_SSID        "SUA_REDE_WIFI"
#define WIFI_PASS        "SUA_SENHA_WIFI"
#define MQTT_SERVER      "SEU_BROKER.s1.eu.hivemq.cloud"
#define MQTT_PORT        8883
#define MQTT_USER        "SEU_USUARIO_MQTT"
#define MQTT_PASS        "SUA_SENHA_MQTT"
#define FIREBASE_PROJECT_URL "https://us-central1-SEU_PROJETO.cloudfunctions.net"
#define WEBSERVER_PASS   "SUA_SENHA_WEB"
```

Instale as bibliotecas necessárias na Arduino IDE:
- `PubSubClient`
- `DHT sensor library` (Adafruit)
- `Adafruit ST7789` + `Adafruit GFX`
- `TFT_eSPI`
- `ArduinoJson`

Abra `Esp32 Firmware - Irriga Home/main/main.ino`, selecione a placa **ESP32 Dev Module** e faça o upload.

---

### 4. Configurar as Cloud Functions

```bash
cd irrigahome-functions/functions
cp .env.example .env
```

Edite `.env`:

```env
MQTT_BROKER_URL=ssl://SEU_BROKER.s1.eu.hivemq.cloud:8883
MQTT_USERNAME=SEU_USUARIO_MQTT
MQTT_PASSWORD=SUA_SENHA_MQTT
MQTT_TOPIC_COMMANDS=irrigahome/commands
MQTT_RETAIN_MESSAGES=false
MQTT_CLIENT_ID=irrigahome-fn-prod
```

Faça o deploy:

```bash
cd irrigahome-functions
npm install
firebase deploy --only functions
```

---

## Mapeamento de Pinos (ESP32)

| GPIO | Componente | Direção |
|------|---|---|
| 2 | Relé da bomba | Saída |
| 4 | DHT22 (temp + umidade do ar) | Entrada |
| 5 | Sensor de nível d'água (bóia) | Entrada |
| 12 | LED de status | Saída |
| 16 | TFT DC (SPI) | Saída |
| 17 | TFT CS (SPI) | Saída |
| 19 | TFT RST (SPI) | Saída |
| 21 | TFT MOSI (SPI) | Saída |
| 22 | TFT CLK (SPI) | Saída |
| 23 | Fluxômetro (ISR FALLING) | Entrada |
| 32 | Umidade do solo (ADC) | Entrada |

---

## Tópicos MQTT

`{deviceId}` = MAC address do ESP32 normalizado (minúsculas, sem separadores)

| Tópico | Direção | Exemplo de payload |
|---|---|---|
| `irrigahome/{deviceId}/commands` | App → ESP32 | `{"action":"irrigate","duration":30}` |
| `irrigahome/{deviceId}/telemetry` | ESP32 → App | `{"solo":45,"temp":28.5,"umidAr":72,"nivelAgua":true,"bomba":false}` |
| `irrigahome/{deviceId}/status` | ESP32 → App | `{"modo":"manual","duracao":10,"threshold":40,"agendamentos":2}` |

**Comandos disponíveis:**

```json
{"action":"irrigate","duration":30}
{"action":"setMode","mode":"auto"}
{"action":"setConfig","duration":20,"threshold":50,"cooldown":120}
{"action":"addSchedule","id":"s1","ativo":true,"diasMask":127,"hour":6,"minute":0,"durationSeconds":30}
{"action":"removeSchedule","id":"s1"}
```

---

## Segurança

Este repositório **não contém credenciais**. Todos os dados sensíveis são gerenciados por arquivos locais ignorados pelo git:

| Arquivo | Status | Conteúdo protegido |
|---|---|---|
| `secrets.h` | gitignored | Wi-Fi, MQTT, Firebase URL, senha web |
| `local.properties` | gitignored | MQTT (Android) |
| `app/google-services.json` | gitignored | API key Firebase |
| `irrigahome-functions/functions/.env` | gitignored | MQTT (Cloud Functions) |

Templates com placeholders estão disponíveis como referência:
- `secrets.example.h`
- `local.properties.example`
- `app/google-services.json.example`
- `irrigahome-functions/functions/.env.example`

---

## Estrutura do Firestore

```
/users/{uid}
    /devices/{deviceId}        → metadata do dispositivo
/irrigationHistory/{deviceId}
    /events/{eventId}          → registros de cada irrigação
/schedules/{deviceId}
    /schedule_01..04/{id}      → agendamentos (máx. 4 por dispositivo)
```

---

## Licença

Este projeto é de uso pessoal/educacional. Consulte o autor para outros usos.
