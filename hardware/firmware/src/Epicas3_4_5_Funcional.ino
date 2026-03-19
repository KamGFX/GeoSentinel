#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <math.h>

// --- CONFIGURACION DE RED ---
const char* WIFI_SSID = "Wifi";
const char* WIFI_PASS = "Contraseña Wifi";
const String serverIP = "192.168.80.24"; 
const String BOT_TOKEN = "Token";
const String CHAT_ID   = "1514893281";

// --- DEFINICION DE PINES ---
#define SS_PIN 21
#define RST_PIN 22
#define SERVO_PIN 13
#define LED_ROJO_PIN 4
#define LED_VERDE_PIN 2
#define LED_AMBAR_PIN 5
#define BUZZER_PIN 27
#define SDA_MPU 25
#define SCL_MPU 26

// --- CONSTANTES DE HARDWARE ---
#define MPU_ADDR 0x68
const int SERVO_CERRADO = 0;
const int SERVO_ABIERTO = 90;
const float UMBRAL_ACTIVAR = 5.0;

// --- OBJETOS ---
MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo servo;
WiFiClient clientLocal;
WiFiClientSecure clientSecure;

// --- ESTADOS Y CONTROL ---
enum EstadoSistema { ESTADO_CERRADA, ESTADO_ABIERTA, ESTADO_DENEGADO };
EstadoSistema estadoActual = ESTADO_CERRADA;

unsigned long tiempoActual = 0;
unsigned long denegadoUltimoCambio = 0;
int denegadoPaso = 0;
bool denegadoActivo = false;
bool pitidoAccesoActivo = false;
unsigned long pitidoAccesoInicio = 0;
bool alarmaInclinacionActiva = false;
float refAngleX = 0.0, refAngleY = 0.0;

// ---------------- COMUNICACION TELEGRAM ----------------
String urlEncode(String str) {
  String encoded = "";
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (isalnum(c)) encoded += c;
    else if (c == ' ') encoded += "%20";
    else {
      encoded += '%';
      char buf[3];
      sprintf(buf, "%02X", c);
      encoded += buf;
    }
  }
  return encoded;
}

void enviarTelegram(String mensaje) {
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient https;
  clientSecure.setInsecure();
  String url = "https://api.telegram.org/bot" + BOT_TOKEN + "/sendMessage?chat_id=" + CHAT_ID + "&text=" + urlEncode(mensaje);
  if (https.begin(clientSecure, url)) {
    https.GET();
    https.end();
  }
}

// ---------------- GESTION MPU-6500 ----------------
void writeMPU(byte reg, byte data) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg); Wire.write(data);
  Wire.endTransmission();
}

void leerInclinacion(float &x, float &y) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x3B);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 6, true);
  
  int16_t ax = (Wire.read() << 8) | Wire.read();
  int16_t ay = (Wire.read() << 8) | Wire.read();
  int16_t az = (Wire.read() << 8) | Wire.read();

  x = atan2(ay, sqrt(pow(ax, 2) + pow(az, 2))) * 180.0 / PI;
  y = atan2(-ax, sqrt(pow(ay, 2) + pow(az, 2))) * 180.0 / PI;
}

// ---------------- SONIDOS DEL BUZZER ----------------
void sonidoAccesoCorto() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(120);
  digitalWrite(BUZZER_PIN, LOW);
}

void sonidoDenegado4Tonos() {
  for (int i = 0; i < 4; i++) {
    digitalWrite(LED_ROJO_PIN, HIGH);
    digitalWrite(BUZZER_PIN, HIGH);
    delay(160);

    digitalWrite(LED_ROJO_PIN, LOW);
    digitalWrite(BUZZER_PIN, LOW);
    delay(140);
  }
}

// ---------------- LOGICA DE PETICION API ----------------
void ejecutarApertura();
void ejecutarCierre();
void ejecutarDenegado();

void procesarAccesoRFID(String uid) {
  if (WiFi.status() != WL_CONNECTED) return;
  
  HTTPClient http;
  String url = "http://" + serverIP + ":3000/api/verificar/" + uid;

  if (http.begin(clientLocal, url)) {
    int httpCode = http.GET();
    if (httpCode == 200) {
      String payload = http.getString();
      StaticJsonDocument<200> doc;
      deserializeJson(doc, payload);
      
      bool acceso = doc["acceso"];
      String operacion = doc["operacion"]; // 'abrir' o 'cerrar'
      String nombre = doc["nombre"];

      if (acceso) {
        Serial.println("Acceso: " + nombre + " | Operacion: " + operacion);
        if (operacion == "abrir") ejecutarApertura();
        else if (operacion == "cerrar") ejecutarCierre();
      } else {
        ejecutarDenegado();
      }
    }
    http.end();
  }
}

// ---------------- ACCIONES FISICAS ----------------
void ejecutarDenegado() {
  estadoActual = ESTADO_DENEGADO;
  denegadoActivo = true;
  denegadoPaso = 0;
  denegadoUltimoCambio = millis();

  enviarTelegram("Alerta: Intento de acceso denegado.");

  // 4 tonos intermitentes
  sonidoDenegado4Tonos();

  denegadoActivo = false;
  estadoActual = ESTADO_CERRADA;

  // Si justo hay alerta sospechosa activa, devuelve el buzzer a continuo
  if (alarmaInclinacionActiva) {
    digitalWrite(BUZZER_PIN, HIGH);
    digitalWrite(LED_AMBAR_PIN, HIGH);
  } else {
    digitalWrite(BUZZER_PIN, LOW);
    digitalWrite(LED_ROJO_PIN, LOW);
  }
}

void ejecutarApertura() {
  servo.write(SERVO_ABIERTO);
  digitalWrite(LED_VERDE_PIN, HIGH);
  estadoActual = ESTADO_ABIERTA;

  // 1 tono corto
  sonidoAccesoCorto();

  enviarTelegram("Vitrina ABIERTA - Sesion iniciada.");
}

void ejecutarCierre() {
  servo.write(SERVO_CERRADO);
  digitalWrite(LED_VERDE_PIN, LOW);
  estadoActual = ESTADO_CERRADA;

  enviarTelegram("Vitrina CERRADA - Sesion finalizada.");
}

// ---------------- CONFIGURACION INICIAL ----------------
void setup() {
  Serial.begin(115200);
  SPI.begin();
  mfrc522.PCD_Init();
  Wire.begin(SDA_MPU, SCL_MPU);
  
  writeMPU(0x6B, 0x00);
  delay(500);
  leerInclinacion(refAngleX, refAngleY);

  pinMode(LED_ROJO_PIN, OUTPUT);
  pinMode(LED_VERDE_PIN, OUTPUT);
  pinMode(LED_AMBAR_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  
  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(SERVO_CERRADO);

  digitalWrite(LED_ROJO_PIN, LOW);
  digitalWrite(LED_VERDE_PIN, LOW);
  digitalWrite(LED_AMBAR_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi OK. IP: " + WiFi.localIP().toString());
  enviarTelegram("Sistema de Vitrina Operativo.");
}

// ---------------- BUCLE PRINCIPAL ----------------
void loop() {
  tiempoActual = millis();

  // 1. Monitoreo de seguridad (MPU)
  float curX, curY;
  leerInclinacion(curX, curY);
  float diff = max(abs(curX - refAngleX), abs(curY - refAngleY));

  if (estadoActual == ESTADO_CERRADA) {
    if (diff > UMBRAL_ACTIVAR && !alarmaInclinacionActiva) {
      alarmaInclinacionActiva = true;
      digitalWrite(LED_AMBAR_PIN, HIGH);

      // tono continuo
      if (!denegadoActivo) {
        digitalWrite(BUZZER_PIN, HIGH);
      }

      enviarTelegram("ALERTA: Movimiento no autorizado detectado.");
    } else if (diff < UMBRAL_ACTIVAR && alarmaInclinacionActiva) {
      alarmaInclinacionActiva = false;
      digitalWrite(LED_AMBAR_PIN, LOW);

      if (!denegadoActivo) {
        digitalWrite(BUZZER_PIN, LOW);
      }
    }
  }

  // 2. Lectura RFID
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uidLeido = "";
    for (byte i = 0; i < mfrc522.uid.size; i++) {
      uidLeido += String(mfrc522.uid.uidByte[i] < 0x10 ? "0" : "");
      uidLeido += String(mfrc522.uid.uidByte[i], HEX);
    }
    uidLeido.toUpperCase();
    procesarAccesoRFID(uidLeido);
    mfrc522.PICC_HaltA();
    mfrc522.PCD_StopCrypto1();
  }
}
