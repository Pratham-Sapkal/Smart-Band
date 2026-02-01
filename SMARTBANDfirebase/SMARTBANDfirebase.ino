#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include <addons/TokenHelper.h>
#include <Wire.h>
#include "MAX30105.h"
#include "heartRate.h"
#include "spo2_algorithm.h"
#include "MPU6050.h"
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// ---------- WIFI ----------
#define WIFI_SSID       "ElectroPraxis"
#define WIFI_PASSWORD   "11111111"

// ---------- FIREBASE ----------
#define API_KEY         "AIzaSyC8qKyT3aHZKJtu1mTLHY4JyJSU9cREGmk"
#define FIREBASE_PROJECT_ID "smart-band-2f441"
#define USER_EMAIL     "prathamsapkal87@gmail.com"
#define USER_PASSWORD  "PaSsWoRd@9"

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

// ---------- OLED ----------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------- Sensors ----------
MAX30105 particleSensor;
MPU6050 mpu;

// ---------------- SOS BUTTON ----------------
#define SOS_PIN 13
volatile bool sosButtonPressed = false;
bool sosState = false;   // false = OFF, true = ON

void IRAM_ATTR sosISR() {
  sosButtonPressed = true;
}


// ---------- Heart Rate ----------
const byte RATE_SIZE = 6;
byte rates[RATE_SIZE];
byte rateSpot = 0;
long lastBeat = 0;
float beatsPerMinute = 0;
int beatAvg = 0;

// ---------------- SpO2 ----------------
#define BUFFER_SIZE 100
uint32_t irBuffer[BUFFER_SIZE];
uint32_t redBuffer[BUFFER_SIZE];
int32_t spo2;
int8_t validSPO2;
int32_t heartRate_SPO2;
int8_t validHeartRate;

// ---------- HRV / Stress ----------
#define HRV_SIZE 10
int rrIntervals[HRV_SIZE];
byte rrSpot = 0;
int stressLevel = 0;

// ---------- MPU ----------
int16_t ax, ay, az;
float ax_ms2, ay_ms2, az_ms2;
float accMagnitude;
float prevAccMagnitude = 0;
float movement;

// ---------- Sleep ----------
bool isSleeping = false;

// ---------- Timers ----------
unsigned long lastPrintTime = 0;
unsigned long lastFirebaseTime = 0;

void setup() {
  Serial.begin(115200);
   delay(1000);
  Wire.begin(21, 22);
  Wire.setClock(400000);

  // ---------- OLED ----------
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("❌ OLED not found");
    while (1);
  }
  display.clearDisplay();
  display.setTextColor(WHITE);

   // ---------- SOS Button ----------
pinMode(SOS_PIN, INPUT_PULLUP);
attachInterrupt(digitalPinToInterrupt(SOS_PIN), sosISR, FALLING);

  // ---------- MAX30102 ----------
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) {
    Serial.println("❌ MAX30102 not found!");
    while (1);
  }

  particleSensor.setup();
  particleSensor.setPulseAmplitudeRed(0x2F);
  particleSensor.setPulseAmplitudeIR(0x2F);
  particleSensor.setPulseAmplitudeGreen(0);

  // MPU6050
  mpu.initialize();

  // Initialize arrays
  for (byte i = 0; i < RATE_SIZE; i++) rates[i] = 75;
  for (byte i = 0; i < HRV_SIZE; i++) rrIntervals[i] = 800;

  // WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);   // Prevent WiFi sleep during sensor sampling
  while (WiFi.status() != WL_CONNECTED) delay(300);

  // Firebase
  config.api_key = API_KEY;
  auth.user.email = USER_EMAIL;
  auth.user.password = USER_PASSWORD;
  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);
}

void loop() {

  // ---------- SOS Toggle with debounce ----------
static unsigned long lastSOSPress = 0;

if (sosButtonPressed) {
  if (millis() - lastSOSPress > 300) {   // 300ms debounce
    sosState = !sosState;
    lastSOSPress = millis();

    if (sosState)
      Serial.println("🚨 SOS TURNED ON");
    else
      Serial.println("✅ SOS TURNED OFF");
  }
  sosButtonPressed = false;
}


 // ---------- Collect MAX30102 Samples ----------
  for (byte i = 0; i < BUFFER_SIZE; i++) {
    while (!particleSensor.available())
      particleSensor.check();

    redBuffer[i] = particleSensor.getRed();
    irBuffer[i]  = particleSensor.getIR();
    particleSensor.nextSample();

    if (checkForBeat(irBuffer[i])) {
      long delta = millis() - lastBeat;
      lastBeat = millis();
      beatsPerMinute = 60.0 / (delta / 1000.0);

      if (beatsPerMinute > 40 && beatsPerMinute < 180) {
        rates[rateSpot++] = (byte)beatsPerMinute;
        rateSpot %= RATE_SIZE;
        beatAvg = 0;
        for (byte x = 0; x < RATE_SIZE; x++)
          beatAvg += rates[x];
        beatAvg /= RATE_SIZE;
      }

      rrIntervals[rrSpot++] = delta;
      rrSpot %= HRV_SIZE;
    }
  }

  // ---------- SpO2 ----------
  maxim_heart_rate_and_oxygen_saturation(
    irBuffer, BUFFER_SIZE, redBuffer,
    &spo2, &validSPO2,
    &heartRate_SPO2, &validHeartRate
  );

  // ---------- HRV → Stress ----------
  int avgRR = 0;
  for (byte i = 0; i < HRV_SIZE; i++)
    avgRR += rrIntervals[i];
  avgRR /= HRV_SIZE;

  if (avgRR > 900) stressLevel = 0;
  else if (avgRR > 700) stressLevel = 1;
  else stressLevel = 2;

  // ---------- MPU6050 Motion in m/s² ----------
  mpu.getAcceleration(&ax, &ay, &az);

  ax_ms2 = (ax / 16384.0) * 9.81;
  ay_ms2 = (ay / 16384.0) * 9.81;
  az_ms2 = (az / 16384.0) * 9.81;

  accMagnitude = sqrt(ax_ms2 * ax_ms2 + ay_ms2 * ay_ms2 + az_ms2 * az_ms2);
  movement = abs(accMagnitude - prevAccMagnitude);
  prevAccMagnitude = accMagnitude;

  // ---------- Sleep Detection ----------
  if (movement < 0.20 && beatAvg < 70 && beatAvg > 45)
    isSleeping = true;
  else
    isSleeping = false;

  // ---------- SERIAL + OLED UPDATE ----------
  if (millis() - lastPrintTime >= 500) {
    lastPrintTime = millis();

    // ----- Serial -----
    Serial.print("BPM=");
    Serial.print(beatAvg);

    Serial.print(" | SpO2=");
    if (validSPO2) Serial.print(spo2);
    else Serial.print("Invalid");

    Serial.print(" | Move=");
    Serial.print(movement, 2);
    Serial.print(" m/s^2");

    Serial.print(" | Stress=");
    if (stressLevel == 0) Serial.print("Relax");
    else if (stressLevel == 1) Serial.print("Normal");
    else Serial.print("High");

    Serial.print(" | Sleep=");
    Serial.println(isSleeping ? "YES" : "NO");

    // ----- OLED -----
    display.clearDisplay();
    display.setTextSize(1);
    display.setCursor(0,0);
    display.print("BPM: "); display.print(beatAvg);

    display.setCursor(0,10);
    display.print("SpO2: ");
    if(validSPO2) display.print(spo2);
    else display.print("--");

    display.setCursor(0,20);
    display.print("Move:");
    display.print(movement,1);
    display.print("m/s2");

    display.setCursor(0,30);
    display.print("Stress:");
    if (stressLevel == 0) display.print("Relax");
    else if (stressLevel == 1) display.print("Normal");
    else display.print("High");

    display.setCursor(0,40);
    display.print("Sleep:");
    display.print(isSleeping ? "YES" : "NO");

display.setCursor(0,50);
display.print("SOS:");
display.print(sosState ? "ON" : "OFF");
    display.display();
  }

  // ---------- Firebase Upload ----------
  if (millis() - lastFirebaseTime > 2000) {
    lastFirebaseTime = millis();

    FirebaseJson content;
    content.set("fields/BPM/integerValue", beatAvg);
    content.set("fields/SpO2/integerValue", validSPO2 ? spo2 : 0);
    content.set("fields/Movement/doubleValue", movement);
    content.set("fields/Stress/integerValue", stressLevel);
    content.set("fields/Sleep/booleanValue", isSleeping);
    content.set("fields/SOS/booleanValue", sosState);

    Firebase.Firestore.patchDocument(
      &fbdo,
      FIREBASE_PROJECT_ID,
      "",
      "SmartBand/User1",
      content.raw(),
      "BPM,SpO2,Movement,Stress,Sleep,SOS"
    );
  }
}
