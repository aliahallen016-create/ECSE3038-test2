# ECSE3038 Test 2 – Smart Bathroom Extractor Fan

A simulated ESP32 with a DHT22 humidity sensor and LED acting as a fan. Every 10 seconds the ESP32 posts its humidity to the FastAPI service, and its fan mode/threshold are delivered by retained MQTT messages. MongoDB Atlas stores fan registrations, settings and readings.

## Contents
- `api/`: FastAPI routes, MongoDB integration and MQTT settings publisher.
- `board/`: PlatformIO ESP32 Arduino project and Wokwi simulator files.

## How to run the API on the Azure VM
1. Clone this repository on the VM.
2. `cd ECSE3038-test2/api`
3. `python3 -m venv venv && source venv/bin/activate`
4. `python -m pip install -r requirements.txt`
5. Create `api/.env` with `MONGODB_URI=<your Atlas URI>` and `MQTT_NAME=<lowercase letters/digits>`. Never commit this file.
6. Stop the old tutorial service with `sudo systemctl stop iot-api`.
7. `uvicorn app:app --host 127.0.0.1 --port 8000`. Nginx already forwards port 80 to this port.
8. Access `http://<VM_PUBLIC_IP>/docs`.

## How to run the board in Wokwi
1. Open `board` as a PlatformIO project in VS Code.
2. Use the provided `diagram.json` and `wokwi.toml` starter files.
3. Add ArduinoJson, PubSubClient and DHTesp in `platformio.ini`.
4. Create `board/include/secrets.h` with `#define DEVICE_KEY "<key returned by POST /fans>"`; this file is gitignored.
5. In `board/src/main.cpp`, set the API VM public IP, MQTT name, DHT pin and LED pin based on the starter circuit.
6. Build and Run Wokwi. Copy the ESP32 MAC from the serial monitor and register it using `POST /fans` in Swagger UI. Then add the API key to `secrets.h`, rebuild and run again.

## API endpoints
| Method | Route | Purpose |
|---|---|---|
| POST | `/fans` | Register fan, generate key, return 201; duplicate MAC 409 |
| GET | `/fans` | List registered fans without keys |
| GET | `/fans/{mac}` | Return one fan without key; unknown MAC 404 |
| POST | `/readings` | Requires `X-API-Key`, records humidity and server timestamp |
| GET | `/readings` | Latest first; optional `mac`; `limit` defaults to 10 and must be 1–100 |
| PUT | `/fans/{mac}/settings` | Set on/off/auto and 0–100 threshold, retained MQTT publish |
| DELETE | `/fans/{mac}` | Requires that fan's own key; returns 204 |

## MQTT topics
With `MQTT_NAME` set to `<name>` (lowercase letters and digits):
- `ecse3038/<name>/fan/settings`: API publishes retained JSON `{"mode":"auto","max_humidity":65.0}`.
- `ecse3038/<name>/fan/state`: ESP32 publishes retained `on` or `off` only when the LED changes.
- `ecse3038/<name>/status`: ESP32 publishes retained `online`; the broker sends retained `offline` as the last will after an unexpected disconnect.

Broker: `135.232.203.144:1883`. The MQTT monitor is at `http://135.232.203.144/mqtt/`.

## Test
Create the fan and obtain its key using `POST /fans`. Check `POST /readings` every ten seconds, `GET /readings`, and `PUT /fans/{mac}/settings` with mode `auto` and max humidity `65`. Adjust DHT22 humidity above/below 65 in Wokwi and watch the LED and retained state topic.
