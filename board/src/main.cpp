#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include "DHTesp.h"
#include "secrets.h"
#include <math.h>

// CHANGE THESE TWO PINS to match the wires in starter diagram.json.
const int DHT_PIN = 15;
const int FAN_LED_PIN = 2;

// CHANGE to your Azure VM's PUBLIC IP; Nginx forwards port 80 to FastAPI 8000.
const char *API_URL = "http://iot-aallen.northcentralus.cloudapp.azure.com/readings";
const char *MQTT_HOST = "135.232.203.144";
const int MQTT_PORT = 1883;

// CHANGE to EXACTLY the same lower-case name as api/.env's MQTT_NAME.
const char *MQTT_NAME = "aliah";

WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);
DHTesp dht;
String deviceMac;
String settingsTopic, stateTopic, statusTopic;
String mode = "off";
float maxHumidity = 0;
float currentHumidity = NAN;
bool haveSettings = false;
bool fanOn = false;
unsigned long lastReadingAt = 0;
unsigned long lastMqttTry = 0;

void changeFan(bool newState) {
  if (fanOn == newState) return;
  fanOn = newState;
  digitalWrite(FAN_LED_PIN, fanOn ? HIGH : LOW);
  Serial.printf("Fan: %s\n", fanOn ? "on" : "off");
  if (mqtt.connected()) {
    mqtt.publish(stateTopic.c_str(), fanOn ? "on" : "off", true);
  }
}

void applySettings() {
  if (!haveSettings) return;  // LED stays off until settings arrive.
  if (mode == "on") {
    changeFan(true);
  } else if (mode == "off") {
    changeFan(false);
  } else if (mode == "auto") {
    changeFan(!isnan(currentHumidity) && currentHumidity > maxHumidity);
  }
}

void settingsCallback(char *topic, byte *payload, unsigned int length) {
  if (String(topic) != settingsTopic) return;
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload, length);
  if (error || !doc.is<JsonObject>()) {
    Serial.println("Rejected settings: invalid JSON");
    return;
  }
  if (!doc["mode"].is<const char *>() || doc["max_humidity"].isNull()) {
    Serial.println("Rejected settings: missing field");
    return;
  }
  if (!doc["max_humidity"].is<int>() && !doc["max_humidity"].is<float>() &&
      !doc["max_humidity"].is<double>()) {
    Serial.println("Rejected settings: humidity limit not numeric");
    return;
  }
  String newMode = doc["mode"].as<String>();
  float newLimit = doc["max_humidity"].as<float>();
  if ((newMode != "auto" && newMode != "on" && newMode != "off") ||
      newLimit < 0 || newLimit > 100) {
    Serial.println("Rejected settings: invalid values");
    return;
  }
  // Only valid settings overwrite the settings already in use.
  mode = newMode;
  maxHumidity = newLimit;
  haveSettings = true;
  Serial.printf("Settings received: mode=%s, limit=%.1f\n", mode.c_str(), maxHumidity);
  applySettings();  // React immediately, without waiting for the next DHT read.
}

void ensureMqtt() {
  if (WiFi.status() != WL_CONNECTED || mqtt.connected()) return;
  unsigned long now = millis();
  if (lastMqttTry && now - lastMqttTry < 5000) return;
  lastMqttTry = now;

  String clientId = "fan-" + deviceMac;
  clientId.replace(":", "");
  // LWT: broker publishes retained offline if the ESP32 unexpectedly disconnects.
  bool connected = mqtt.connect(clientId.c_str(), statusTopic.c_str(),
                                1, true, "offline");
  if (connected) {
    Serial.println("MQTT connected");
    mqtt.subscribe(settingsTopic.c_str(), 1);
    mqtt.publish(statusTopic.c_str(), "online", true);
  } else {
    Serial.printf("MQTT connection failed: %d\n", mqtt.state());
  }
}

void postReading(float humidity) {
  if (WiFi.status() != WL_CONNECTED) return;
  WiFiClient httpClient;
  HTTPClient http;
  http.begin(httpClient, API_URL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", DEVICE_KEY);

  JsonDocument doc;
  doc["mac"] = deviceMac;
  doc["humidity"] = humidity;
  String body;
  serializeJson(doc, body);
  int status = http.POST(body);
  Serial.printf("POST /readings -> %d, humidity %.1f\n", status, humidity);
  if (status != 201) Serial.println(http.getString());
  http.end();
}

void setup() {
  Serial.begin(115200);
  pinMode(FAN_LED_PIN, OUTPUT);
  digitalWrite(FAN_LED_PIN, LOW);
  dht.setup(DHT_PIN, DHTesp::DHT22);

  WiFi.mode(WIFI_STA);
  WiFi.begin("Wokwi-GUEST", "", 6);
  Serial.print("Connecting to Wokwi WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(" connected");
  deviceMac = WiFi.macAddress();
  Serial.println("MAC ADDRESS (copy to POST /fans): " + deviceMac);

  String baseTopic = "ecse3038/" + String(MQTT_NAME);
  settingsTopic = baseTopic + "/fan/settings";
  stateTopic = baseTopic + "/fan/state";
  statusTopic = baseTopic + "/status";
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(settingsCallback);
  ensureMqtt();
  lastReadingAt = millis();
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
    delay(200);
  }
  ensureMqtt();
  if (mqtt.connected()) mqtt.loop();

  if (millis() - lastReadingAt >= 10000) {
    lastReadingAt = millis();
    TempAndHumidity result = dht.getTempAndHumidity();
    if (isnan(result.humidity)) {
      Serial.println("DHT22 read failed");
    } else {
      currentHumidity = result.humidity;
      applySettings();
      postReading(currentHumidity);
    }
  }
  delay(20);
}
