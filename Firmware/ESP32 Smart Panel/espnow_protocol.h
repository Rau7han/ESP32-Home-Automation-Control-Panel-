#pragma once

#ifdef ARDUINO
#include <Arduino.h>
#else
#include <stdint.h>
#endif

static constexpr uint8_t ESPNOW_PROTOCOL_VERSION = 2;

enum EspNowMessageType : uint8_t {
    MSG_RELAY_COMMAND = 1,
    MSG_SCENE_COMMAND = 2,
    MSG_STATUS_REQUEST = 3,
    MSG_RELAY_SENSOR_STATUS = 4
};

struct RelayCommandPacket {
    uint8_t version;
    uint8_t type;
    uint8_t relayIndex;
    uint8_t desiredState;
    uint32_t sequence;
};

struct SceneCommandPacket {
    uint8_t version;
    uint8_t type;
    uint8_t desiredRelayMask;
    uint8_t reserved;
    uint32_t sequence;
};

struct StatusRequestPacket {
    uint8_t version;
    uint8_t type;
    uint16_t reserved;
    uint32_t sequence;
};

enum SensorValidBits : uint8_t {
    SENSOR_TEMP_VALID = 1 << 0,
    SENSOR_HUMIDITY_VALID = 1 << 1
};

struct RelaySensorStatusPacket {
    uint8_t version;
    uint8_t type;
    uint8_t relayMask;
    uint8_t sensorValidMask;
    uint32_t sequence;
    int16_t indoorTemperatureCentiC;
    uint16_t indoorHumidityCentiPercent;
    uint32_t relayNodeUptimeMs;
};

static_assert(sizeof(RelayCommandPacket) <= 250, "RelayCommandPacket is too large");
static_assert(sizeof(SceneCommandPacket) <= 250, "SceneCommandPacket is too large");
static_assert(sizeof(StatusRequestPacket) <= 250, "StatusRequestPacket is too large");
static_assert(sizeof(RelaySensorStatusPacket) <= 250, "RelaySensorStatusPacket is too large");
