# ESP32 Relay + DHT22 Node

This folder is the relay-side firmware for the Smart Dashboard display.

## Wiring

- DHT22 VCC: 3.3 V
- DHT22 OUT: GPIO 13
- DHT22 GND: GND
- Relay 1: GPIO 26
- Relay 2: GPIO 25
- Relay 3: GPIO 33
- Relay 4: GPIO 32
- Display and relay boards must join the same 2.4 GHz Wi-Fi network

The sketch defaults to active-LOW relay modules. Set `RELAY_ACTIVE_LOW` to `0`
before connecting loads if the module is active HIGH.

## Dependency

Install **DHT sensor library** by Adafruit. Its **Adafruit Unified Sensor**
dependency is installed automatically. This project was compiled with DHT
library 1.4.7 and Arduino-ESP32 3.3.11.

A 3-pin DHT22 module normally includes the required data pull-up resistor. If
using a bare 4-pin sensor, add a 4.7–10 kΩ resistor between OUT and 3.3 V.

## Safety

All relay outputs are driven to OFF before their pins become outputs. Confirm
relay polarity with LEDs or a multimeter before connecting mains voltage.

The OpenWeather key belongs only in the display's private `secrets.h`; this
relay node does not make weather requests.
