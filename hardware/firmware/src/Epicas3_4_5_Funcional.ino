#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h> // Necesaria para procesar la respuesta de la API

#define SS_PIN 21
#define RST_PIN 22

// Pines
#define SERVO_PIN 13
#define LED_ROJO_PIN 4
#define LED_VERDE_PIN 2
#define LED_AMBAR_PIN 5
#define BUZZER_PIN 27

// MPU-6500
#define SDA_MPU 25
#define SCL_MPU 26
#define MPU_ADDR 0x68

// ---------- CONFIGURACIÓN RED Y API ----------
const char* WIFI_SSID = "CASA-6";
const char* WIFI_PASS = "HG.27.SOCORRO";

// REEMPLAZA CON LA IP DE TU PC (Escribe ipconfig en tu terminal)
const String API_URL = "http://172.26.96.1/api"; 

const String BOT_TOKEN = "8769951849:AAF0aEVHre27v6hb_vQz23us2mSPI5Cjy5U";
const String CHAT_ID   = "1514893281";
// -------------------------------------

MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo servo;
WiFiClientSecure client; 

// Datos de control
int usuarioIdActual = -1; // Se llena al validar la tarjeta
const int SERVO_CERRADO = 0;
const int SERVO_ABIERTO = 90;

// Inclinación
float anguloInicial = 0.0;
bool calibrado = false;
const float UMBRAL_ACTIVAR = 20.0;
const float UMBRAL_DESACTIVAR = 8.0;

enum EstadoSistema { ESTADO_CERRADA, ESTADO_ABIERTA, ESTADO_DENEGADO };
EstadoSistema estadoActual = ESTADO_CERRADA;

unsigned long tiempoActual = 0;

// Acceso denegado
bool denegadoActivo = false;
unsigned long denegadoUltimoCambio = 0;
int denegadoPaso = 0;
const int DENEGADO_TOTAL_PASOS = 8;
const unsigned long DENEGADO_TIEMPO_ON = 120;
const unsigned long DENEGADO_TIEMPO_OFF = 120;

// Sonidos y estados
bool pitidoAccesoActivo = false;
unsigned long pitidoAccesoInicio = 0;
const unsigned long PITIDO_ACCESO_DURACION = 80;
bool tarjetaProcesada = false;
bool alarmaInclinacionActiva = false;

// Cola Telegram
bool telegramPendiente = false;
String mensajeTelegramPendiente = "";
unsigned long telegramEnviarDespues = 0;

// ---------------- UTILIDADES ----------------

String uidToString(byte *uid, byte size) {
  String res = "";
  for (byte i = 0; i < size; i++) {
    if (uid[i] < 0x10) res += "0";
    res += String(uid[i], HEX);
  }
  res.toUpperCase();
  return res;
}

// ---------------- TELEGRAM ----------------

String urlEncode(String str) {
  String encoded = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c)) encoded += c;
    else if (c == ' ') encoded += "%20";
  }
  return encoded;
}

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  Serial.print("Conectando a WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) {
    delay(500); Serial.print(".");
  }
  if (WiFi.status() == WL_CONNECTED) Serial.println("\nWiFi Conectado: " + WiFi.localIP().toString());
}

void enviarTelegramAhora(String mensaje) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient https;
  client.setInsecure();
  String url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage?chat_id=" + CHAT_ID + "&text=" + urlEncode(mensaje);
  if (https.begin(client, url)) {
    https.GET();
    https.end();
  }
}

void programarTelegram(String mensaje, unsigned long retrasoMs) {
  telegramPendiente = true;
  mensajeTelegramPendiente = mensaje;
  telegramEnviarDespues = millis() + retrasoMs;
}

void actualizarTelegramPendiente() {
  if (telegramPendiente && millis() >= telegramEnviarDespues) {
    enviarTelegramAhora(mensajeTelegramPendiente);
    telegramPendiente = false;
  }
}

// ---------------- SENSORES Y ACTUADORES ----------------

void leerMPU(float &ax, float &ay, float &az) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);
  if (Wire.available() >= 6) {
    ax = (Wire.read() << 8 | Wire.read()) / 16384.0;
    ay = (Wire.read() << 8 | Wire.read()) / 16384.0;
    az = (Wire.read() << 8 | Wire.read()) / 16384.0;
  }
}

float obtenerDiferenciaInclinacion() {
  float ax, ay, az;
  leerMPU(ax, ay, az);
  float anguloActual = atan2(ay, az) * 180.0 / PI;
  if (!calibrado) { anguloInicial = anguloActual; calibrado = true; }
  return abs(anguloActual - anguloInicial);
}

void encenderBuzzer() { digitalWrite(BUZZER_PIN, HIGH); }
void apagarBuzzer() { digitalWrite(BUZZER_PIN, LOW); }

void iniciarPitidoAcceso() {
  pitidoAccesoActivo = true;
  pitidoAccesoInicio = millis();
  encenderBuzzer();
}

void actualizarPitidoAcceso() {
  if (pitidoAccesoActivo && (millis() - pitidoAccesoInicio >= PITIDO_ACCESO_DURACION)) {
    if (!alarmaInclinacionActiva) apagarBuzzer();
    pitidoAccesoActivo = false;
  }
}

void iniciarDenegado() {
  estadoActual = ESTADO_DENEGADO;
  denegadoActivo = true;
  denegadoPaso = 0;
  denegadoUltimoCambio = millis();
  digitalWrite(LED_ROJO_PIN, HIGH);
  encenderBuzzer();
  programarTelegram("🚫 Acceso denegado en la vitrina", 1200);
}

void actualizarDenegado() {
  if (!denegadoActivo) return;
  unsigned long intervalo = (digitalRead(LED_ROJO_PIN) == HIGH) ? DENEGADO_TIEMPO_ON : DENEGADO_TIEMPO_OFF;
  if (millis() - denegadoUltimoCambio >= intervalo) {
    denegadoUltimoCambio = millis();
    bool estado = !digitalRead(LED_ROJO_PIN);
    digitalWrite(LED_ROJO_PIN, estado);
    if (estado) encenderBuzzer(); else if (!alarmaInclinacionActiva) apagarBuzzer();
    if (++denegadoPaso >= DENEGADO_TOTAL_PASOS) {
      digitalWrite(LED_ROJO_PIN, LOW);
      if (!alarmaInclinacionActiva) apagarBuzzer();
      denegadoActivo = false;
      estadoActual = ESTADO_CERRADA;
    }
  }
}

void abrirVitrina(String msg) {
  servo.write(SERVO_ABIERTO);
  digitalWrite(LED_VERDE_PIN, HIGH);
  iniciarPitidoAcceso();
  estadoActual = ESTADO_ABIERTA;
  programarTelegram("✅ " + msg, 250);
}

void cerrarVitrina() {
  // Petición a la API para registrar el cierre
  if (usuarioIdActual != -1) {
    WiFiClient apiWifi;
    HTTPClient http;
    http.begin(apiWifi, API_URL + "/cerrar");
    http.addHeader("Content-Type", "application/json");
    String body = "{\"usuario_id\":" + String(usuarioIdActual) + "}";
    http.POST(body);
    http.end();
  }
  
  servo.write(SERVO_CERRADO);
  digitalWrite(LED_VERDE_PIN, LOW);
  iniciarPitidoAcceso();
  estadoActual = ESTADO_CERRADA;
  programarTelegram("🔒 La puerta de la vitrina fue CERRADA", 250);
}

// ---------------- SETUP ----------------

void setup() {
  Serial.begin(115200);
  SPI.begin();
  mfrc522.PCD_Init();
  Wire.begin(SDA_MPU, SCL_MPU);
  Wire.beginTransmission(MPU_ADDR); Wire.write(0x6B); Wire.write(0); Wire.endTransmission();

  pinMode(LED_ROJO_PIN, OUTPUT); pinMode(LED_VERDE_PIN, OUTPUT);
  pinMode(LED_AMBAR_PIN, OUTPUT); pinMode(BUZZER_PIN, OUTPUT);

  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(SERVO_CERRADO);

  conectarWiFi();
  enviarTelegramAhora("🤖 Sistema de vitrina iniciado y conectado");
}

// ---------------- LOOP ----------------

void loop() {
  tiempoActual = millis();
  conectarWiFi();
  actualizarPitidoAcceso();
  actualizarTelegramPendiente();

  float diff = obtenerDiferenciaInclinacion();
  if (estadoActual == ESTADO_CERRADA) {
    if (diff > UMBRAL_ACTIVAR) {
      if (!alarmaInclinacionActiva) {
        alarmaInclinacionActiva = true;
        digitalWrite(LED_AMBAR_PIN, HIGH); encenderBuzzer();
        programarTelegram("⚠️ Alerta: inclinacion sospechosa detectada", 100);
      }
    } else if (diff < UMBRAL_DESACTIVAR && alarmaInclinacionActiva) {
      alarmaInclinacionActiva = false;
      digitalWrite(LED_AMBAR_PIN, LOW); if (!pitidoAccesoActivo && !denegadoActivo) apagarBuzzer();
      programarTelegram("✅ Vitrina en posicion normal", 100);
    }
  }

  // Lectura de tarjeta
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uidStr = uidToString(mfrc522.uid.uidByte, mfrc522.uid.size);
    Serial.println("Tarjeta detectada: " + uidStr);

    WiFiClient apiWifi;
    HTTPClient http;
    http.begin(apiWifi, API_URL + "/verificar");
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<200> doc;
    doc["uid_tarjeta"] = uidStr;
    String requestBody;
    serializeJson(doc, requestBody);

    int httpCode = http.POST(requestBody);
    if (httpCode == 200) {
      String payload = http.getString();
      StaticJsonDocument<300> res;
      deserializeJson(res, payload);

      if (res["acceso"] == true) {
        usuarioIdActual = 1; // Aquí podrías mapear el ID real si la API lo envía
        if (estadoActual == ESTADO_CERRADA) abrirVitrina(res["mensaje"]);
        else cerrarVitrina();
      } else {
        iniciarDenegado();
      }
    } else {
      Serial.println("Error de conexión con API");
    }
    http.end();
    mfrc522.PICC_HaltA();
  }

  if (estadoActual == ESTADO_DENEGADO) actualizarDenegado();
}
