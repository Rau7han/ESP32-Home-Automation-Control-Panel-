/**
 * ESP32 relay and DHT22 node for the Smart Dashboard.
 *
 * Hardware:
 *   DHT22 data GPIO 13
 *   Relay outputs GPIO 26, 25, 33, 32
 *
 * The node joins the same Wi-Fi network as the display so ESP-NOW uses the
 * router channel. Relay outputs change only after a valid command from the
 * configured display MAC. Every command is followed by a confirmed status
 * packet; periodic status packets keep the display's online state fresh.
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_mac.h>
#include <DHT.h>

#include "espnow_protocol.h"
#include "relay_secrets.h"

#define DHT_PIN 13
#define DHT_TYPE DHT22

#define RELAY1 26
#define RELAY2 25
#define RELAY3 33
#define RELAY4 32

/* Most opto-isolated relay modules are active LOW. Change this to 0 before
 * connecting loads if your module is active HIGH. Boot always requests OFF. */
#define RELAY_ACTIVE_LOW 1

static constexpr uint8_t RELAY_COUNT = 4;
static constexpr uint8_t RELAY_MASK_ALL = 0x0F;
static constexpr uint32_t STATUS_INTERVAL_MS = 2000;
static constexpr uint32_t SENSOR_INTERVAL_MS = 5000;
static constexpr uint32_t SENSOR_STALE_MS = 15000;

static const uint8_t RELAY_PINS[RELAY_COUNT] = {
    RELAY1, RELAY2, RELAY3, RELAY4
};

static const uint8_t DISPLAY_ESP32_MAC[6] = {
    0xFC, 0x01, 0x2C, 0xD4, 0x9C, 0x38
};

static const uint8_t EXPECTED_RELAY_MAC[6] = {
    0xC0, 0x5D, 0x89, 0xF5, 0xAD, 0xFC
};

enum CommandKind : uint8_t {
    COMMAND_RELAY,
    COMMAND_SCENE,
    COMMAND_STATUS
};

struct QueuedCommand {
    CommandKind kind;
    uint8_t relayIndex;
    uint8_t desiredState;
    uint8_t relayMask;
    uint32_t sequence;
};

struct SensorSnapshot {
    uint8_t validMask;
    int16_t temperatureCentiC;
    uint16_t humidityCentiPercent;
    uint32_t sampledAtMs;
};

static QueueHandle_t commandQueue = nullptr;
static DHT dht(DHT_PIN, DHT_TYPE);

static portMUX_TYPE sensorMux = portMUX_INITIALIZER_UNLOCKED;
static SensorSnapshot sensorSnapshot = {};
static volatile bool sensorStatusDirty = false;

static uint8_t confirmedRelayMask = 0;
static bool espNowReady = false;
static bool wifiWasConnected = false;
static uint32_t lastStatusSentMs = 0;
static uint32_t lastEspNowAttemptMs = 0;
static uint32_t lastConfirmedSequence = 0;

static bool macMatches(const uint8_t *a, const uint8_t *b)
{
    return a != nullptr && b != nullptr && memcmp(a, b, 6) == 0;
}

static uint8_t relayOffLevel()
{
    return RELAY_ACTIVE_LOW ? HIGH : LOW;
}

static uint8_t relayOnLevel()
{
    return RELAY_ACTIVE_LOW ? LOW : HIGH;
}

static void initializeRelayOutputs()
{
    confirmedRelayMask = 0;
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        /* Set the output latch before enabling output mode to avoid an ON pulse. */
        digitalWrite(RELAY_PINS[i], relayOffLevel());
        pinMode(RELAY_PINS[i], OUTPUT);
        digitalWrite(RELAY_PINS[i], relayOffLevel());
    }
}

static void applyRelayMask(uint8_t mask)
{
    mask &= RELAY_MASK_ALL;
    for (uint8_t i = 0; i < RELAY_COUNT; i++) {
        bool on = (mask & (1U << i)) != 0;
        digitalWrite(RELAY_PINS[i], on ? relayOnLevel() : relayOffLevel());
    }
    confirmedRelayMask = mask;
}

static void sensorTask(void *parameter)
{
    (void)parameter;
    for (;;) {
        float humidity = dht.readHumidity();
        float temperature = dht.readTemperature();

        if (isnan(humidity) || isnan(temperature)) {
            Serial.println("DHT22 read failed; retaining last valid sample");
        } else {
            SensorSnapshot next = {};
            next.validMask = SENSOR_TEMP_VALID | SENSOR_HUMIDITY_VALID;
            next.temperatureCentiC =
                (int16_t)lroundf(temperature * 100.0f);
            humidity = constrain(humidity, 0.0f, 100.0f);
            next.humidityCentiPercent =
                (uint16_t)lroundf(humidity * 100.0f);
            next.sampledAtMs = millis();

            portENTER_CRITICAL(&sensorMux);
            sensorSnapshot = next;
            sensorStatusDirty = true;
            portEXIT_CRITICAL(&sensorMux);
        }

        vTaskDelay(pdMS_TO_TICKS(SENSOR_INTERVAL_MS));
    }
}

static void espNowSendCallback(
    const esp_now_send_info_t *info,
    esp_now_send_status_t status)
{
    (void)info;
    if (status != ESP_NOW_SEND_SUCCESS) {
        Serial.println("ESP-NOW status delivery failed");
    }
}

static void espNowReceiveCallback(
    const esp_now_recv_info_t *info,
    const uint8_t *data,
    int length)
{
    if (info == nullptr ||
        data == nullptr ||
        commandQueue == nullptr ||
        !macMatches(info->src_addr, DISPLAY_ESP32_MAC) ||
        length < 2 ||
        data[0] != ESPNOW_PROTOCOL_VERSION) {
        return;
    }

    QueuedCommand command = {};
    switch (data[1]) {
        case MSG_RELAY_COMMAND: {
            if (length != (int)sizeof(RelayCommandPacket)) return;
            RelayCommandPacket packet;
            memcpy(&packet, data, sizeof(packet));
            if (packet.relayIndex >= RELAY_COUNT || packet.desiredState > 1) return;
            command.kind = COMMAND_RELAY;
            command.relayIndex = packet.relayIndex;
            command.desiredState = packet.desiredState;
            command.sequence = packet.sequence;
            break;
        }
        case MSG_SCENE_COMMAND: {
            if (length != (int)sizeof(SceneCommandPacket)) return;
            SceneCommandPacket packet;
            memcpy(&packet, data, sizeof(packet));
            command.kind = COMMAND_SCENE;
            command.relayMask = packet.desiredRelayMask & RELAY_MASK_ALL;
            command.sequence = packet.sequence;
            break;
        }
        case MSG_STATUS_REQUEST: {
            if (length != (int)sizeof(StatusRequestPacket)) return;
            StatusRequestPacket packet;
            memcpy(&packet, data, sizeof(packet));
            command.kind = COMMAND_STATUS;
            command.sequence = packet.sequence;
            break;
        }
        default:
            return;
    }

    xQueueSend(commandQueue, &command, 0);
}

static bool initializeEspNow()
{
    if (espNowReady) return true;
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW initialization failed");
        return false;
    }

    if (esp_now_register_send_cb(espNowSendCallback) != ESP_OK ||
        esp_now_register_recv_cb(espNowReceiveCallback) != ESP_OK) {
        Serial.println("ESP-NOW callback registration failed");
        esp_now_deinit();
        return false;
    }

    esp_now_peer_info_t peer = {};
    memcpy(peer.peer_addr, DISPLAY_ESP32_MAC, 6);
    peer.channel = WiFi.channel();
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;

    esp_err_t result = esp_now_add_peer(&peer);
    espNowReady = result == ESP_OK || result == ESP_ERR_ESPNOW_EXIST;
    Serial.printf(
        "Display peer registration: %s (%d), channel %u\n",
        espNowReady ? "OK" : "FAILED",
        result,
        (unsigned)WiFi.channel());
    if (!espNowReady) esp_now_deinit();
    return espNowReady;
}

static void sendConfirmedStatus(uint32_t sequence)
{
    if (!espNowReady) return;

    SensorSnapshot sensor;
    portENTER_CRITICAL(&sensorMux);
    sensor = sensorSnapshot;
    portEXIT_CRITICAL(&sensorMux);

    if (sensor.sampledAtMs == 0 ||
        millis() - sensor.sampledAtMs > SENSOR_STALE_MS) {
        sensor.validMask = 0;
    }

    RelaySensorStatusPacket packet = {};
    packet.version = ESPNOW_PROTOCOL_VERSION;
    packet.type = MSG_RELAY_SENSOR_STATUS;
    packet.relayMask = confirmedRelayMask;
    packet.sensorValidMask = sensor.validMask;
    packet.sequence = sequence;
    packet.indoorTemperatureCentiC = sensor.temperatureCentiC;
    packet.indoorHumidityCentiPercent = sensor.humidityCentiPercent;
    packet.relayNodeUptimeMs = millis();

    esp_now_send(
        DISPLAY_ESP32_MAC,
        reinterpret_cast<const uint8_t *>(&packet),
        sizeof(packet));
    lastStatusSentMs = millis();
}

static void processCommandQueue()
{
    QueuedCommand command;
    while (commandQueue != nullptr &&
           xQueueReceive(commandQueue, &command, 0) == pdTRUE) {
        switch (command.kind) {
            case COMMAND_RELAY: {
                uint8_t bit = 1U << command.relayIndex;
                uint8_t desiredMask = command.desiredState
                    ? (confirmedRelayMask | bit)
                    : (confirmedRelayMask & ~bit);
                applyRelayMask(desiredMask);
                lastConfirmedSequence = command.sequence;
                Serial.printf(
                    "Relay %u confirmed %s, mask=0x%02X\n",
                    command.relayIndex + 1,
                    command.desiredState ? "ON" : "OFF",
                    confirmedRelayMask);
                sendConfirmedStatus(command.sequence);
                break;
            }
            case COMMAND_SCENE:
                applyRelayMask(command.relayMask);
                lastConfirmedSequence = command.sequence;
                Serial.printf(
                    "Scene confirmed, mask=0x%02X\n",
                    confirmedRelayMask);
                sendConfirmedStatus(command.sequence);
                break;
            case COMMAND_STATUS:
                sendConfirmedStatus(command.sequence);
                break;
        }
    }
}

static void initializeSensor()
{
    dht.begin();
    BaseType_t result = xTaskCreate(
        sensorTask, "dht22", 3072, nullptr, 1, nullptr);
    if (result == pdPASS) {
        Serial.printf("DHT22 ready on GPIO %u\n", DHT_PIN);
    } else {
        Serial.println("ERROR: DHT22 task creation failed");
    }
}

void setup()
{
    Serial.begin(115200);
    delay(500);
    Serial.println("\nESP32 Relay + DHT22 Node");
    Serial.printf(
        "Relay pins: %u, %u, %u, %u (%s)\n",
        RELAY1, RELAY2, RELAY3, RELAY4,
        RELAY_ACTIVE_LOW ? "active LOW" : "active HIGH");

    initializeRelayOutputs();
    initializeSensor();

    commandQueue = xQueueCreate(12, sizeof(QueuedCommand));
    if (commandQueue == nullptr) {
        Serial.println("ERROR: command queue allocation failed");
    }

    WiFi.mode(WIFI_STA);

    /* WiFi.macAddress() returns zeros until the Wi-Fi stack finishes starting,
     * so read the eFuse address directly. */
    uint8_t actualMac[6];
    esp_read_mac(actualMac, ESP_MAC_WIFI_STA);
    Serial.printf(
        "Relay STA MAC: %02X:%02X:%02X:%02X:%02X:%02X\n",
        actualMac[0], actualMac[1], actualMac[2],
        actualMac[3], actualMac[4], actualMac[5]);
    Serial.println("Expected relay MAC: C0:5D:89:F5:AD:FC");
    Serial.println("Display peer MAC: FC:01:2C:D4:9C:38");

    if (!macMatches(actualMac, EXPECTED_RELAY_MAC)) {
        Serial.println("WARNING: this board MAC differs from the configured relay peer MAC");
    }

    WiFi.setAutoReconnect(true);
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    Serial.println("Wi-Fi connection started");
}

void loop()
{
    uint32_t now = millis();
    bool connected = WiFi.status() == WL_CONNECTED;

    if (connected && !wifiWasConnected) {
        wifiWasConnected = true;
        Serial.printf(
            "Wi-Fi connected, IP=%s, channel=%u\n",
            WiFi.localIP().toString().c_str(),
            (unsigned)WiFi.channel());
        initializeEspNow();
        lastEspNowAttemptMs = now;
        sendConfirmedStatus(lastConfirmedSequence);
    } else if (!connected && wifiWasConnected) {
        wifiWasConnected = false;
        espNowReady = false;
        esp_now_deinit();
        Serial.println("Wi-Fi disconnected; relays keep their confirmed states");
    }

    if (connected && !espNowReady &&
        now - lastEspNowAttemptMs >= 5000) {
        lastEspNowAttemptMs = now;
        initializeEspNow();
    }

    processCommandQueue();

    if (espNowReady &&
        (sensorStatusDirty || now - lastStatusSentMs >= STATUS_INTERVAL_MS)) {
        sensorStatusDirty = false;
        sendConfirmedStatus(lastConfirmedSequence);
    }

    delay(10);
}
