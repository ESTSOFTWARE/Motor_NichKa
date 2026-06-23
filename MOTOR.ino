#include <Arduino.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#define DIR_PIN    26
#define STEP_PIN   25
#define RELAY_PIN  27
#define BOMBA_PIN  14

#define ENC_CLK    18
#define ENC_DT     19
#define ENC_SW     5

const char* MQTT_HOST      = "shark.rmq.cloudamqp.com";
const int   MQTT_PORT      = 1883;
const char* MQTT_USER      = "kcugwwbq:kcugwwbq";
const char* MQTT_PASSWORD  = "LAG1cqP1fWpW-tXs2eQzyyq_fZMigTyj";
const char* MQTT_CLIENT_ID = "esp32-fermentador-1";
const int   CIRCUIT_ID     = 1;

#define RPM_INTERVAL   2000
#define ENC_MIN        0
#define ENC_MAX        100

SemaphoreHandle_t stateMutex;
volatile bool motorOn    = false;
volatile bool bombaOn    = false;
volatile int  encValue   = 50;
volatile int  sessionId  = -1;

WiFiClient   wifiClient;
PubSubClient mqttClient(wifiClient);

void mqttCallback(char* topic, byte* payload, unsigned int length);
void conectarMQTT();

int encoderToDelay(int enc) {
  return map(enc, ENC_MIN, ENC_MAX, 2000, 600);
}

float calcularRPM(int delayMicros) {
  if (delayMicros <= 0) return 0.0;
  const int STEPS_PER_REV = 200;
  float tiempoVuelta = (float)(delayMicros * 2 * STEPS_PER_REV);
  return (60.0 * 1000000.0) / tiempoVuelta;
}

void publicarSensor(const char* sensorType, float value) {
  if (!mqttClient.connected()) return;
  char topic[64];
  snprintf(topic, sizeof(topic), "sensors/%d/%s", CIRCUIT_ID, sensorType);
  StaticJsonDocument<128> doc;
  doc["value"] = value;
  if (sessionId > 0) { doc["session_id"] = sessionId; } else { doc["session_id"] = nullptr; }
  doc["active"] = true;
  char payload[128];
  serializeJson(doc, payload);
  mqttClient.publish(topic, payload);
  Serial.printf("[MQTT] %s → %s\n", topic, payload);
}

void aplicarEstado(const char* dispositivo, const char* estado) {
  bool encendido = (String(estado) == "encendido");
  if (String(dispositivo) == "motor") {
    motorOn = encendido;
    digitalWrite(RELAY_PIN, encendido ? LOW : HIGH);
    Serial.printf("[CMD] Motor → %s\n", estado);
  } else if (String(dispositivo) == "bomba") {
    bombaOn = encendido;
    digitalWrite(BOMBA_PIN, encendido ? LOW : HIGH);
    Serial.printf("[CMD] Bomba → %s\n", estado);
  }
}

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String message = "";
  for (unsigned int i = 0; i < length; i++) { message += (char)payload[i]; }
  Serial.printf("[MQTT CMD] Topic: %s | Payload: %s\n", topic, message.c_str());
  StaticJsonDocument<256> doc;
  DeserializationError err = deserializeJson(doc, message);
  if (err) { Serial.printf("[MQTT CMD] Error: %s\n", err.c_str()); return; }
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  for (JsonPair kv : doc.as<JsonObject>()) {
    aplicarEstado(kv.key().c_str(), kv.value().as<const char*>());
  }
  xSemaphoreGive(stateMutex);
}

void conectarWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setTitle("Fermentador IoT");

  if (!wm.autoConnect("Fermentador-Setup")) {
    Serial.println("[WiFi] Timeout, reiniciando...");
    ESP.restart();
  }

  Serial.printf("[WiFi] Conectado. IP: %s\n", WiFi.localIP().toString().c_str());
}

void conectarMQTT() {
  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCallback(mqttCallback);
  int intentos = 0;
  while (!mqttClient.connected() && intentos < 5) {
    Serial.print("[MQTT] Conectando...");
    if (mqttClient.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
      Serial.println(" conectado");
      char topicState[64];
      snprintf(topicState, sizeof(topicState), "commands/%d/state", CIRCUIT_ID);
      mqttClient.subscribe(topicState);
      Serial.printf("[MQTT] Suscrito a %s\n", topicState);
    } else {
      Serial.printf(" fallo (rc=%d), reintentando...\n", mqttClient.state());
      delay(2000);
      intentos++;
    }
  }
  if (!mqttClient.connected()) Serial.println("[MQTT] No se pudo conectar.");
}

void motorTask(void* parameter) {
  uint32_t steps = 0;
  while (true) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    bool activo = motorOn;
    int  enc    = encValue;
    xSemaphoreGive(stateMutex);
    if (activo) {
      int delayMicro = encoderToDelay(enc);
      digitalWrite(STEP_PIN, HIGH);
      delayMicroseconds(delayMicro);
      digitalWrite(STEP_PIN, LOW);
      delayMicroseconds(delayMicro);
      // Cede el CPU al IDLE cada 100 pasos para no disparar el Task Watchdog
      // (delayMicroseconds NO cede; sin esto el core 0 se satura y el ESP32 se reinicia).
      if (++steps % 100 == 0) vTaskDelay(1);
    } else {
      steps = 0;
      vTaskDelay(50 / portTICK_PERIOD_MS);
    }
  }
}

void encoderTask(void* parameter) {
  int clkAnterior = digitalRead(ENC_CLK);
  while (true) {
    int clkActual = digitalRead(ENC_CLK);
    if (clkActual != clkAnterior) {
      if (digitalRead(ENC_DT) != clkActual) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        if (encValue < ENC_MAX) encValue++;
        xSemaphoreGive(stateMutex);
      } else {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        if (encValue > ENC_MIN) encValue--;
        xSemaphoreGive(stateMutex);
      }
    }
    clkAnterior = clkActual;
    vTaskDelay(5 / portTICK_PERIOD_MS);
  }
}

void rpmTask(void* parameter) {
  while (true) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    bool motor = motorOn;
    bool bomba = bombaOn;
    int  enc   = encValue;
    xSemaphoreGive(stateMutex);
    int   delayMicro = encoderToDelay(enc);
    float rpm        = motor ? calcularRPM(delayMicro) : 0.0;
    publicarSensor("rpm", rpm);
    Serial.printf("[MONITOR] Enc: %d | RPM: %.1f | Motor: %s | Bomba: %s\n",
      enc, rpm, motor ? "ON" : "OFF", bomba ? "ON" : "OFF");
    mqttClient.loop();
    vTaskDelay(RPM_INTERVAL / portTICK_PERIOD_MS);
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(STEP_PIN,  OUTPUT);
  pinMode(DIR_PIN,   OUTPUT);
  pinMode(RELAY_PIN, OUTPUT);
  pinMode(BOMBA_PIN, OUTPUT);
  digitalWrite(DIR_PIN,   HIGH);
  digitalWrite(RELAY_PIN, HIGH);
  digitalWrite(BOMBA_PIN, HIGH);

  pinMode(ENC_CLK, INPUT_PULLUP);
  pinMode(ENC_DT,  INPUT_PULLUP);
  pinMode(ENC_SW,  INPUT_PULLUP);

  stateMutex = xSemaphoreCreateMutex();

  conectarWiFi();
  conectarMQTT();

  xTaskCreatePinnedToCore(motorTask,   "MotorTask",   2048, NULL, 2, NULL, 0);
  xTaskCreatePinnedToCore(encoderTask, "EncoderTask", 2048, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(rpmTask,     "RpmTask",     4096, NULL, 0, NULL, 1);

  Serial.println("[SETUP] Listo");
}

void loop() {
  if (!mqttClient.connected()) {
    Serial.println("[MQTT] Reconectando...");
    conectarMQTT();
  }
  mqttClient.loop();
  delay(10);
}