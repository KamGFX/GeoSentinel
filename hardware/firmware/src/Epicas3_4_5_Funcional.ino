#include <SPI.h>
#include <MFRC522.h>
#include <ESP32Servo.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>

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

// ---------- WIFI / TELEGRAM ----------
const char* WIFI_SSID = "CASA-6";
const char* WIFI_PASS = "HG.27.SOCORRO";

const String BOT_TOKEN = "8769951849:AAF0aEVHre27v6hb_vQz23us2mSPI5Cjy5U";
const String CHAT_ID   = "1514893281";
// -------------------------------------

MFRC522 mfrc522(SS_PIN, RST_PIN);
Servo servo;
WiFiClientSecure client;

// UID autorizado
byte UID_AUTORIZADO[] = {0xF2, 0x1D, 0x25, 0x02};
const byte UID_SIZE = 4;

// Servo
const int SERVO_CERRADO = 0;
const int SERVO_ABIERTO = 90;

// Inclinación
float anguloInicial = 0.0;
bool calibrado = false;
const float UMBRAL_ACTIVAR = 20.0;
const float UMBRAL_DESACTIVAR = 8.0;

// Máquina de estados principal
enum EstadoSistema {
  ESTADO_CERRADA,
  ESTADO_ABIERTA,
  ESTADO_DENEGADO
};

EstadoSistema estadoActual = ESTADO_CERRADA;

// Tiempo general
unsigned long tiempoActual = 0;

// Acceso denegado
bool denegadoActivo = false;
unsigned long denegadoUltimoCambio = 0;
int denegadoPaso = 0;
const int DENEGADO_TOTAL_PASOS = 8;
const unsigned long DENEGADO_TIEMPO_ON = 120;
const unsigned long DENEGADO_TIEMPO_OFF = 120;

// Pitido acceso autorizado
bool pitidoAccesoActivo = false;
unsigned long pitidoAccesoInicio = 0;
const unsigned long PITIDO_ACCESO_DURACION = 80;

// Anti-relectura
bool tarjetaProcesada = false;

// Alarma por inclinación
bool alarmaInclinacionActiva = false;

// WiFi
bool wifiConectado = false;

// -------- Cola simple de Telegram --------
bool telegramPendiente = false;
String mensajeTelegramPendiente = "";
unsigned long telegramEnviarDespues = 0;
// ----------------------------------------

// ---------------- TELEGRAM ----------------

String urlEncode(String str) {
  String encoded = "";
  char c;
  char code0;
  char code1;

  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);

    if (isalnum((unsigned char)c)) {
      encoded += c;
    } else if (c == ' ') {
      encoded += "%20";
    } else {
      code1 = (c & 0x0F) + '0';
      if ((c & 0x0F) > 9) code1 = (c & 0x0F) - 10 + 'A';

      c = (c >> 4) & 0x0F;
      code0 = c + '0';
      if (c > 9) code0 = c - 10 + 'A';

      encoded += '%';
      encoded += code0;
      encoded += code1;
    }
  }
  return encoded;
}

void conectarWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    return;
  }

  Serial.print("Conectando a WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  unsigned long inicio = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - inicio < 15000) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    wifiConectado = true;
    Serial.println("WiFi conectado");
    Serial.print("IP: ");
    Serial.println(WiFi.localIP());
  } else {
    wifiConectado = false;
    Serial.println("No se pudo conectar a WiFi");
  }
}

void enviarTelegramAhora(String mensaje) {
  if (WiFi.status() != WL_CONNECTED) {
    conectarWiFi();
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("Telegram no enviado: sin WiFi");
    return;
  }

  HTTPClient https;
  client.setInsecure();

  String url = "https://api.telegram.org/bot" + BOT_TOKEN +
               "/sendMessage?chat_id=" + CHAT_ID +
               "&text=" + urlEncode(mensaje);

  if (https.begin(client, url)) {
    int httpCode = https.GET();
    Serial.print("Telegram HTTP code: ");
    Serial.println(httpCode);
    https.end();
  } else {
    Serial.println("No se pudo iniciar conexion HTTPS con Telegram");
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
    mensajeTelegramPendiente = "";
  }
}

// ---------------- FUNCIONES GENERALES ----------------

bool esAutorizado(byte *uid) {
  for (byte i = 0; i < UID_SIZE; i++) {
    if (uid[i] != UID_AUTORIZADO[i]) return false;
  }
  return true;
}

void encenderBuzzer() {
  digitalWrite(BUZZER_PIN, HIGH);
}

void apagarBuzzer() {
  digitalWrite(BUZZER_PIN, LOW);
}

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
  float ax = 0, ay = 0, az = 0;
  leerMPU(ax, ay, az);

  float anguloActual = atan2(ay, az) * 180.0 / PI;

  if (!calibrado) {
    anguloInicial = anguloActual;
    calibrado = true;
  }

  return abs(anguloActual - anguloInicial);
}

bool leerTarjeta(byte *uidLeido, byte &uidSize) {
  if (!mfrc522.PICC_IsNewCardPresent()) {
    tarjetaProcesada = false;
    return false;
  }

  if (tarjetaProcesada) {
    return false;
  }

  if (!mfrc522.PICC_ReadCardSerial()) {
    return false;
  }

  uidSize = mfrc522.uid.size;
  for (byte i = 0; i < uidSize; i++) {
    uidLeido[i] = mfrc522.uid.uidByte[i];
  }

  tarjetaProcesada = true;

  mfrc522.PICC_HaltA();
  mfrc522.PCD_StopCrypto1();

  return true;
}

void imprimirUID(byte *uid, byte uidSize) {
  Serial.print("UID leído: ");
  for (byte i = 0; i < uidSize; i++) {
    if (uid[i] < 0x10) Serial.print("0");
    Serial.print(uid[i], HEX);
    Serial.print(" ");
  }
  Serial.println();
}

// ---------------- SONIDOS ----------------

void iniciarPitidoAcceso() {
  pitidoAccesoActivo = true;
  pitidoAccesoInicio = tiempoActual;
  encenderBuzzer();
}

void actualizarPitidoAcceso() {
  if (pitidoAccesoActivo && (tiempoActual - pitidoAccesoInicio >= PITIDO_ACCESO_DURACION)) {
    if (!alarmaInclinacionActiva) {
      apagarBuzzer();
    }
    pitidoAccesoActivo = false;
  }
}

void iniciarDenegado() {
  estadoActual = ESTADO_DENEGADO;
  denegadoActivo = true;
  denegadoPaso = 0;
  denegadoUltimoCambio = tiempoActual;

  digitalWrite(LED_ROJO_PIN, HIGH);
  encenderBuzzer();

  Serial.println("Acceso DENEGADO");

  // Programado después del patrón de 4 pitidos
  programarTelegram("🚫 Acceso denegado en la vitrina", 1200);
}

void actualizarDenegado() {
  if (!denegadoActivo) return;

  unsigned long intervalo = (digitalRead(LED_ROJO_PIN) == HIGH) ? DENEGADO_TIEMPO_ON : DENEGADO_TIEMPO_OFF;

  if (tiempoActual - denegadoUltimoCambio >= intervalo) {
    denegadoUltimoCambio = tiempoActual;

    if (digitalRead(LED_ROJO_PIN) == HIGH) {
      digitalWrite(LED_ROJO_PIN, LOW);
      if (!alarmaInclinacionActiva) {
        apagarBuzzer();
      }
    } else {
      digitalWrite(LED_ROJO_PIN, HIGH);
      encenderBuzzer();
    }

    denegadoPaso++;

    if (denegadoPaso >= DENEGADO_TOTAL_PASOS) {
      digitalWrite(LED_ROJO_PIN, LOW);
      if (!alarmaInclinacionActiva) {
        apagarBuzzer();
      }
      denegadoActivo = false;
      estadoActual = ESTADO_CERRADA;
      Serial.println("Sistema vuelve a estado CERRADA");
    }
  }
}

// ---------------- ACCIONES ----------------

void abrirVitrina() {
  servo.write(SERVO_ABIERTO);
  digitalWrite(LED_VERDE_PIN, HIGH);
  iniciarPitidoAcceso();
  estadoActual = ESTADO_ABIERTA;
  Serial.println("Acceso CONCEDIDO - ABRIENDO");

  // Se envía después de que termine el pitido corto
  programarTelegram("✅ La puerta de la vitrina fue ABIERTA", 250);
}

void cerrarVitrina() {
  servo.write(SERVO_CERRADO);
  digitalWrite(LED_VERDE_PIN, LOW);
  iniciarPitidoAcceso();
  estadoActual = ESTADO_CERRADA;
  Serial.println("Acceso CONCEDIDO - CERRANDO");

  // Se envía después de que termine el pitido corto
  programarTelegram("🔒 La puerta de la vitrina fue CERRADA", 250);
}

void actualizarAlarmaInclinacion(float diferenciaInclinacion) {
  if (estadoActual == ESTADO_CERRADA) {
    if (diferenciaInclinacion > UMBRAL_ACTIVAR) {
      if (!alarmaInclinacionActiva) {
        alarmaInclinacionActiva = true;
        digitalWrite(LED_AMBAR_PIN, HIGH);
        encenderBuzzer();
        Serial.println("ALERTA: Inclinacion sospechosa!");

        // Aquí no importa si bloquea un poco porque queremos sonido continuo
        programarTelegram("⚠️ Alerta: inclinacion sospechosa detectada en la vitrina", 100);
      } else {
        digitalWrite(LED_AMBAR_PIN, HIGH);
        encenderBuzzer();
      }
    } else if (diferenciaInclinacion < UMBRAL_DESACTIVAR) {
      if (alarmaInclinacionActiva) {
        alarmaInclinacionActiva = false;
        digitalWrite(LED_AMBAR_PIN, LOW);
        if (!pitidoAccesoActivo && !denegadoActivo) {
          apagarBuzzer();
        }
        Serial.println("Inclinacion normal nuevamente");

        programarTelegram("✅ La vitrina volvió a una posicion recta/normal", 100);
      }
    }
  } else {
    if (alarmaInclinacionActiva) {
      alarmaInclinacionActiva = false;
      digitalWrite(LED_AMBAR_PIN, LOW);
      if (!pitidoAccesoActivo && !denegadoActivo) {
        apagarBuzzer();
      }
    }
  }
}

// ---------------- SETUP ----------------

void setup() {
  Serial.begin(115200);

  SPI.begin();
  mfrc522.PCD_Init();

  Wire.begin(SDA_MPU, SCL_MPU);

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x6B);
  Wire.write(0);
  Wire.endTransmission();

  pinMode(LED_ROJO_PIN, OUTPUT);
  pinMode(LED_VERDE_PIN, OUTPUT);
  pinMode(LED_AMBAR_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);

  digitalWrite(LED_ROJO_PIN, LOW);
  digitalWrite(LED_VERDE_PIN, LOW);
  digitalWrite(LED_AMBAR_PIN, LOW);
  digitalWrite(BUZZER_PIN, LOW);

  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(SERVO_CERRADO);

  conectarWiFi();
  enviarTelegramAhora("🤖 Sistema de vitrina iniciado y conectado");

  Serial.println("Sistema listo. Acerca una tarjeta...");
}

// ---------------- LOOP ----------------

void loop() {
  tiempoActual = millis();

  if (WiFi.status() != WL_CONNECTED) {
    wifiConectado = false;
  }

  actualizarPitidoAcceso();
  actualizarTelegramPendiente();

  float diferenciaInclinacion = obtenerDiferenciaInclinacion();
  actualizarAlarmaInclinacion(diferenciaInclinacion);

  byte uidLeido[10];
  byte uidSize = 0;
  bool hayTarjeta = leerTarjeta(uidLeido, uidSize);

  switch (estadoActual) {
    case ESTADO_CERRADA: {
      digitalWrite(LED_VERDE_PIN, LOW);

      if (hayTarjeta) {
        imprimirUID(uidLeido, uidSize);

        if (uidSize == UID_SIZE && esAutorizado(uidLeido)) {
          abrirVitrina();
        } else {
          iniciarDenegado();
        }
      }
      break;
    }

    case ESTADO_ABIERTA: {
      digitalWrite(LED_VERDE_PIN, HIGH);

      if (hayTarjeta) {
        imprimirUID(uidLeido, uidSize);

        if (uidSize == UID_SIZE && esAutorizado(uidLeido)) {
          cerrarVitrina();
        } else {
          iniciarDenegado();
        }
      }
      break;
    }

    case ESTADO_DENEGADO: {
      actualizarDenegado();
      break;
    }
  }
}