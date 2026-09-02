#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_PN532.h>
#include <Adafruit_NeoPixel.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
#include "Audio.h"
#include "FS.h"
#include "SPIFFS.h"
#include <driver/i2s_std.h>

// ==========================================
// 1. KONFIGURASI NETWORK & SERVER
// ==========================================

const char* ssid = "xxx";
const char* password = "xxxxx";

//  Ganti ini dengan IP Laptop kamu yang menjalankan app.py
// Cara cek di Windows: buka cmd -> ketik ipconfig -> lihat IPv4 Address
const char* server_host = "0.0.0.0.";
const int server_port = 5000;
const char* mqtt_server = "broker.hivemq.com";

// API KEYS
const char* assembly_api_key = "xxxxxx"; // Ganti dengan API Key kamu
const char* gemini_api_key = "xxxxxx"; // Ganti dengan API Key kamu
const char* eleven_api_key = "xxxxx"; // Ganti dengan API Key kamu
String eleven_voice_id = "xxxxxx"; // Ganti dengan API Key kamu

// ==========================================
// 2. DEFINISI PIN
// ==========================================
#define BUZZER_PIN 9

// OLED
#define SDA_PIN 1
#define SCL_PIN 2
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

// NFC (SPI)
#define PN532_SCK (18)
#define PN532_MOSI (17)
#define PN532_SS (15)
#define PN532_MISO (16)

// NEOPIXEL
#define PIN_NEOPIXEL 48
#define NUM_PIXELS 8

// INPUT TOMBOL
#define ROTARY_CLK 4
#define ROTARY_DT 5
#define ROTARY_SW 10     // Tombol Rotary (Enter/OK)
#define BTN_NO 13        // Merah (Back/No/Cancel)
#define BTN_YES 14       // Hijau (Start/Trigger/Yes)
#define BTN_DISTRACT 12  // Kuning (Distraksi)
#define BTN_VOICE 21     // Hitam (AI Push-to-Talk)

// AUDIO (I2S)
#define SPK_BCLK 41
#define SPK_LRC 42
#define SPK_DOUT 40
#define MIC_SCK 38
#define MIC_WS 39
#define MIC_SD 6

// ==========================================
// 3. OBJEK & VARIABEL GLOBAL
// ==========================================

Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
Adafruit_PN532 nfc(PN532_SCK, PN532_MISO, PN532_MOSI, PN532_SS);
Adafruit_NeoPixel strip(NUM_PIXELS, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
Audio audio;
WiFiClient espClient;
PubSubClient client(espClient);
bool focusPaused = false;
bool isNewSession = true;



// Variabel Audio Recording
bool isAlarmActive = false;
bool isRecording = false;
const int record_time_max = 5;
uint8_t* microphonedata0;
size_t bytes_recorded = 0;
const int headerSize = 44;
#define SAMPLE_RATE 16000
i2s_chan_handle_t rx_handle = NULL;

// Struktur Data Tugas (To-Do List dari Web)
struct Task {
  int id;
  String title;
  int est_pomodoros;
  int completed_pomodoros;
};
Task currentTasks[10];  // Max 10 tugas di memori
int taskCount = 0;
int selectedTaskIndex = 0;

// Struktur Waktu Belajar (Sesuai RL Engine Python)
struct PomoConfig {
  int focus;
  int shortBreak;
  int longBreak;
  String label;
  String actionId;  // A0 - A4
};

void startAlarm() {
  isAlarmActive = true;
}

void stopAlarm() {
  isAlarmActive = false;
  digitalWrite(BUZZER_PIN, LOW);
}



PomoConfig timeOptions[] = {
  { 15, 3, 10, "15/3/10 M", "A0" },
  { 20, 5, 15, "20/5/15 M", "A1" },
  { 25, 5, 20, "25/5/20 M", "A2" },
  { 30, 7, 25, "30/7/25 M", "A3" },
  { 35, 10, 30, "35/10/30 M", "A4" },

  // 🔬 MODE TESTING
  { 30, 30, 15, "30/30/15 S", "AS" }
};
const int configCount = 6;
int selectedConfigIndex = 2;
String recommendedActionId = "";  // Disimpan dari API Recommendation

// STATE MACHINE UTAMA
enum SystemState {
  STATE_BOOT,
  STATE_SELECT_TASK,       // Pilih Tugas dari Web
  STATE_SELECT_TIME,       // Pilih Waktu (Ada Rekomendasi)
  STATE_ASK_JAIL,          // Phone Jail?
  STATE_READY_TO_START,    // Tunggu Tombol Hijau
  STATE_POMODORO_RUN,      // Timer Belajar
  STATE_SHORT_BREAK_WAIT,  // Tunggu mulai istirahat pendek
  STATE_SHORT_BREAK_RUN,   // Timer istirahat pendek
  STATE_LONG_BREAK_WAIT,   // Tunggu mulai istirahat panjang
  STATE_LONG_BREAK_RUN,    // Timer istirahat panjang
  STATE_ASK_FINISHED,      // Tanya "Sudah Selesai?"
  STATE_CONFIRM_END,       // Tanya "Akhiri Sesi?"
  STATE_SAVE_DATA,         // Tanya "Simpan Data?"
  STATE_FINISH,            // Selesai -> Balik ke Halo
  STATE_AI_PROCESS,        // Mode AI Voice
  STATE_FOCUS_FINISHED_WAIT,
};
SystemState currentState = STATE_BOOT;

// Variabel Pomodoro
int cycleCount = 0;  // Menghitung 1-4
int totalPomodorosDone = 0;
unsigned long remainingSeconds = 0;
unsigned long lastMillis = 0;
unsigned long focusStartMillis = 0;


// Variabel Fitur & Safety
bool usePhoneJail = false;
bool isDistracted = false;  // Status Distraksi (Toggle)
bool phoneLocked = false;
unsigned long distractionStartTime = 0;
unsigned long totalDistractionTime = 0;
int distractCount = 0;
unsigned long lastDistractReminder = 0;
unsigned long lastJailWarning = 0;
unsigned long lastInputTime = 0;     // Untuk Timeout
unsigned long lastWarningSound = 0;  // Untuk suara "Menunggu Input"
unsigned long lastHeartbeat = 0;     // Untuk status online di dashboard

// Variabel Hardware UI
int lastClk;
int pixelHead = 0;
int colorState = 0;
unsigned long lastPixelStep = 0;

// ==========================================
// 4. PROTOTYPE FUNGSI
// ==========================================
void connectWiFi();
void connectMQTT();
void fetchTasksFromWeb();
void fetchRecommendationFromWeb();
void sendSessionData(bool saved);
void sendHeartbeat();
bool isSecondsMode();


bool checkNFC(int retries);
void setEyeColor(int r, int g, int b);
void rotatingEffect();
void drawFace(String expression, String text = "");
void beep();
void playVoice(String text);
void checkTimeoutAndVoice();  // Gabungan cek timeout & tombol AI

// Logic Loops
void loopBoot();
void loopSelectTask();
void loopSelectTime();
void loopAskJail();
void loopReadyToStart();
void loopPomodoroRun();
void loopBreakWait(bool isLong);
void loopBreakRun(bool isLong);
void loopAskFinished();
void loopConfirmEnd();
void loopSaveData();
void loopFinish();

// AI Functions
void recordAudio();
String stt_assemblyai();
String ask_gemini(String text);
void tts_elevenlabs(String text);
void generateWavHeader(byte* header, int waveDataSize);
void processAI();

void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String msg = "";
  for (unsigned int i = 0; i < length; i++) {
    msg += (char)payload[i];
  }

  // === VOICE FEEDBACK DARI DASHBOARD ===
  if (String(topic) == "pomobox/voice_feedback") {
    DynamicJsonDocument doc(256);
    DeserializationError err = deserializeJson(doc, msg);

    if (!err) {
      String voiceMsg = doc["message"].as<String>();
      if (voiceMsg.length() > 0) {
        playVoice(voiceMsg);  // 🔊 SUARA KELUAR DI ESP32
      }
    }
  }
}


// ==========================================
// 5. SETUP
// ==========================================
void setup() {
  Serial.begin(115200);

  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(BUZZER_PIN, LOW);
  beep();

  Wire.begin(SDA_PIN, SCL_PIN);
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println(F("OLED Gagal!"));

  if (!SPIFFS.begin(true)) Serial.println("SPIFFS Error");

  connectWiFi();

  // Init MQTT
  client.setServer(mqtt_server, 1883);
  client.setCallback(mqttCallback);


  // Init NFC
  nfc.begin();
  if (!nfc.getFirmwareVersion()) Serial.println("NFC Error");
  nfc.setPassiveActivationRetries(0xFF);
  nfc.SAMConfig();

  strip.begin();
  strip.setBrightness(50);
  strip.show();

  pinMode(ROTARY_CLK, INPUT_PULLUP);
  pinMode(ROTARY_DT, INPUT_PULLUP);
  pinMode(ROTARY_SW, INPUT_PULLUP);
  pinMode(BTN_YES, INPUT);
  pinMode(BTN_NO, INPUT);
  pinMode(BTN_DISTRACT, INPUT);
  pinMode(BTN_VOICE, INPUT_PULLDOWN);

  lastClk = digitalRead(ROTARY_CLK);

  audio.setPinout(SPK_BCLK, SPK_LRC, SPK_DOUT);
  audio.setVolume(20);

  microphonedata0 = (uint8_t*)ps_malloc((SAMPLE_RATE * 2 * record_time_max) + 44);
  if (microphonedata0 == NULL) microphonedata0 = (uint8_t*)malloc(SAMPLE_RATE * 2 * record_time_max);

  lastInputTime = millis();
  currentState = STATE_BOOT;
  drawFace("HAPPY", "HALO!");
  playVoice("Sistem Pomobox aktif, silahkan tekan tombol hijau untuk memulai!");
}

void setFocusLight() {
  for (int i = 0; i < NUM_PIXELS; i++) {
    strip.setPixelColor(i, strip.Color(180, 180, 180));  // putih lembut
  }
  strip.show();
}

void setIdleLight() {
  // pakai efek muter yang sudah ada
  // jadi tidak perlu isi apa-apa
}


// ==========================================
// 6. MAIN LOOP
// ==========================================
void loop() {

  if (isAlarmActive) {
    digitalWrite(BUZZER_PIN, (millis() / 300) % 2);  // bunyi panjang beep-beep
  }


  audio.loop();
  if (!client.connected()) connectMQTT();
  client.loop();

  sendHeartbeat();  // Kirim status online ke dashboard

  checkTimeoutAndVoice();  // Cek timeout & tombol AI

  switch (currentState) {
    case STATE_BOOT: loopBoot(); break;
    case STATE_SELECT_TASK: loopSelectTask(); break;
    case STATE_SELECT_TIME: loopSelectTime(); break;
    case STATE_ASK_JAIL: loopAskJail(); break;
    case STATE_READY_TO_START: loopReadyToStart(); break;
    case STATE_POMODORO_RUN: loopPomodoroRun(); break;

    // Break States
    case STATE_SHORT_BREAK_WAIT: loopBreakWait(false); break;
    case STATE_SHORT_BREAK_RUN: loopBreakRun(false); break;
    case STATE_LONG_BREAK_WAIT: loopBreakWait(true); break;
    case STATE_LONG_BREAK_RUN: loopBreakRun(true); break;
    case STATE_FOCUS_FINISHED_WAIT: loopFocusFinishedWait(); break;


    // Finish & Save
    case STATE_ASK_FINISHED: loopAskFinished(); break;
    case STATE_CONFIRM_END: loopConfirmEnd(); break;
    case STATE_SAVE_DATA: loopSaveData(); break;
    case STATE_FINISH: loopFinish(); break;

    case STATE_AI_PROCESS: break;  // Blocking di processAI()
  }
}

// ==========================================
// 7. IMPLEMENTASI LOGIC PER STATE
// ==========================================

// 1. BOOT (TAMPILAN AWAL)
void loopBoot() {
  rotatingEffect();  // Lampu muter
  if (millis() % 2000 < 1000) drawFace("HAPPY", "HALO!");
  else drawFace("IDLE", "TEKAN HIJAU");

  if (digitalRead(BTN_YES) == HIGH) {
    setEyeColor(0, 255, 0);
    beep();
    lastInputTime = millis();

    // Fetch Data
    fetchTasksFromWeb();
    isNewSession = true;
    currentState = STATE_SELECT_TASK;
    delay(300);
  }
}

// FETCH TASKS FROM WEB
void fetchTasksFromWeb() {
  drawFace("THINK", "Ambil Data...");
  HTTPClient http;
  // Endpoint disesuaikan dengan app.py Python
  String url = "http://" + String(server_host) + ":" + String(server_port) + "/api/tasks/list";
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode == 200) {
    String payload = http.getString();
    DynamicJsonDocument doc(4096);
    deserializeJson(doc, payload);
    JsonArray arr = doc.as<JsonArray>();

    taskCount = 0;
    for (JsonObject obj : arr) {
      if (taskCount < 10) {
        currentTasks[taskCount].id = obj["id"];
        currentTasks[taskCount].title = obj["title"].as<String>();
        currentTasks[taskCount].est_pomodoros = obj["est"];
        currentTasks[taskCount].completed_pomodoros = obj["done"];
        taskCount++;
      }
    }
  } else {
    playVoice("Gagal mengambil data, gunakan tugas default");
    // Fallback dummy task
    taskCount = 1;
    currentTasks[0].id = 0;
    currentTasks[0].title = "Tugas Manual";
    currentTasks[0].est_pomodoros = 4;
    currentTasks[0].completed_pomodoros = 0;
  }
  http.end();
}

// 2. SELECT TASK
void loopSelectTask() {
  rotatingEffect();

  // Rotary Logic
  int currentClk = digitalRead(ROTARY_CLK);
  if (currentClk != lastClk && currentClk == 1) {
    if (digitalRead(ROTARY_DT) != currentClk) {
      selectedTaskIndex++;
      if (selectedTaskIndex >= taskCount) selectedTaskIndex = 0;
    } else {
      selectedTaskIndex--;
      if (selectedTaskIndex < 0) selectedTaskIndex = taskCount - 1;
    }
    beep();
    lastInputTime = millis();
  }
  lastClk = currentClk;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  display.setCursor(0, 0);
  display.println("PILIH TUGAS:");

  if (taskCount > 0) {
    display.setCursor(0, 25);
    display.print(">");
    display.println(currentTasks[selectedTaskIndex].title);
    display.setCursor(10, 45);
    display.print(currentTasks[selectedTaskIndex].completed_pomodoros);
    display.print("/");
    display.print(currentTasks[selectedTaskIndex].est_pomodoros);
    display.print(" Sesi");
  } else {
    display.setCursor(0, 30);
    display.println("Tidak ada tugas");
  }
  display.display();

  // Konfirmasi (Rotary SW)
  if (digitalRead(ROTARY_SW) == LOW) {
    setEyeColor(0, 255, 0);
    beep();
    lastInputTime = millis();
    fetchRecommendationFromWeb();  // Ambil Rekomendasi RL
    currentState = STATE_SELECT_TIME;
    delay(300);
  }
}

void fetchRecommendationFromWeb() {
  HTTPClient http;
  // Endpoint disesuaikan dengan app.py Python
  String url = "http://" + String(server_host) + ":" + String(server_port) + "/api/recommendation";
  http.begin(url);
  if (http.GET() == 200) {
    String payload = http.getString();
    StaticJsonDocument<200> doc;
    deserializeJson(doc, payload);
    recommendedActionId = doc["action_id"].as<String>();
  }
  http.end();
}

// 3. SELECT TIME
void loopSelectTime() {
  rotatingEffect();

  // Tombol Merah (Back)
  if (digitalRead(BTN_NO) == HIGH) {
    setEyeColor(255, 0, 0);
    beep();
    currentState = STATE_BOOT;
    delay(300);
    return;
  }

  // Rotary Timer
  int currentClk = digitalRead(ROTARY_CLK);
  if (currentClk != lastClk && currentClk == 1) {
    if (digitalRead(ROTARY_DT) != currentClk) {
      selectedConfigIndex = (selectedConfigIndex + 1) % configCount;
    } else {
      selectedConfigIndex = (selectedConfigIndex - 1 + configCount) % configCount;
    }
    beep();
    lastInputTime = millis();
  }
  lastClk = currentClk;

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("PILIH WAKTU:");
  display.setTextSize(2);
  display.setCursor(0, 25);
  display.print(">");
  display.print(timeOptions[selectedConfigIndex].label);

  // Tampilkan Rekomendasi
  if (timeOptions[selectedConfigIndex].actionId == recommendedActionId) {
    display.setTextSize(1);
    display.setCursor(0, 50);
    display.print("(Rekomendasi)");
  }
  display.display();

  if (digitalRead(ROTARY_SW) == LOW) {
    setEyeColor(0, 255, 0);
    beep();
    lastInputTime = millis();
    currentState = STATE_ASK_JAIL;
    delay(300);
  }
}

// 4. ASK JAIL
void loopAskJail() {
  rotatingEffect();

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Gunakan Fitur");
  display.println("Phone Jail?");
  display.println("");
  display.println("YA");
  display.println("TIDAK");
  display.display();

  // ===== PILIH YA → AKTIFKAN PHONE JAIL =====
  if (digitalRead(BTN_YES) == HIGH) {
    setEyeColor(0, 255, 0);
    beep();

    usePhoneJail = true;
    lastInputTime = millis();

    playVoice("Silakan taruh handphone di dalam kotak");
    currentState = STATE_READY_TO_START;
    delay(300);
  }

  // ===== PILIH TIDAK → NONAKTIFKAN PHONE JAIL =====
  else if (digitalRead(BTN_NO) == HIGH) {
    setEyeColor(255, 0, 0);
    beep();

    usePhoneJail = false;
    lastInputTime = millis();

    // Kirim ke web bahwa fitur NFC tidak digunakan
    client.publish("pomobox/nfc_status", "{\"status\": \"UNUSED\"}");

    currentState = STATE_READY_TO_START;
    delay(300);
  }
}


// 5. READY & WAIT (NFC Check)
void loopReadyToStart() {
  rotatingEffect();

  // Cek NFC jika jail aktif
  if (usePhoneJail) {
    if (!checkNFC(2)) {
      drawFace("IDLE", "TARUH HP!");
      phoneLocked = false;
      return;
    } else {
      lastInputTime = millis();

      if (!phoneLocked) {
        client.publish("pomobox/nfc_status", "{\"status\":\"LOCKED\"}");
        phoneLocked = true;
      }
    }
  }


  drawFace("IDLE", "TEKAN HIJAU MULAI");

  if (digitalRead(BTN_YES) == HIGH) {
    stopAlarm();
    // ===== START FOCUS SESSION =====
    focusStartMillis = millis();
    totalDistractionTime = 0;
    distractCount = 0;
    isDistracted = false;
    lastDistractReminder = 0;

    setEyeColor(0, 255, 0);
    beep();
    lastInputTime = millis();  // <-- TOMBOL = input user

    phoneLocked = true;

    // Hitung Mundur
    playVoice("Belajar Akan Dimulai Dalam 10 detik");
    for (int i = 10; i > 0; i--) {
      display.clearDisplay();
      display.setTextSize(3);
      display.setCursor(50, 20);
      display.print(i);
      display.display();
      delay(1000);
    }


    // INIT SESI FOKUS
    // HANYA reset jika ini benar-benar sesi baru (dari BOOT)
    if (isNewSession) {
      totalPomodorosDone = 0;
      cycleCount = 0;
      totalDistractionTime = 0;
      distractCount = 0;
      isNewSession = false;
    }



    remainingSeconds = isSecondsMode()
                         ? timeOptions[selectedConfigIndex].focus
                         : timeOptions[selectedConfigIndex].focus * 60;


    // Info ke Web: Sesi Mulai
    String json = "{\"task_id\": " + String(currentTasks[selectedTaskIndex].id) + ", \"use_nfc\": " + String(usePhoneJail ? "true" : "false") + "}";
    client.publish("pomobox/session_start", json.c_str());

    playVoice("Waktunya Belajar!");
    lastMillis = millis();
    focusPaused = false;
    setFocusLight();
    currentState = STATE_POMODORO_RUN;
  }
}

void loopFocusFinishedWait() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(10, 20);
  display.println("Sesi selesai");
  display.println("Tekan hijau");
  display.display();

  if (digitalRead(BTN_YES) == HIGH) {
    stopAlarm();  // 🔥
    beep();
    // 🔥 KUNCI INTEGRITAS DATA
    sendSessionData(true);  // kirim 1 pomodoro ke server

    totalPomodorosDone++;
    cycleCount++;

    if (totalPomodorosDone >= currentTasks[selectedTaskIndex].est_pomodoros) {
      cycleCount = 0;
      currentState = STATE_ASK_FINISHED;
      return;
    }

    if (cycleCount >= 4) {
      cycleCount = 0;
      currentState = STATE_LONG_BREAK_WAIT;
    } else {
      currentState = STATE_SHORT_BREAK_WAIT;
    }
    delay(300);
  }
}


// 6. POMODORO RUN (CORE)
void loopPomodoroRun() {

  // =============================
  // 1. TIMER 1 DETIK
  // =============================
  if (!focusPaused && millis() - lastMillis >= 1000) {

    if (remainingSeconds > 0) {
      remainingSeconds--;

      // ⚠️ Warning 5 detik sebelum habis
      if (remainingSeconds == 5) {
        playVoice("Lima detik lagi sesi fokus selesai");
      }
    } else {
      // =============================
      // WAKTU HABIS
      // =============================
      playVoice("Sesi fokus selesai. Tekan tombol hijau");
      startAlarm();
      currentState = STATE_FOCUS_FINISHED_WAIT;
      return;
    }

    lastMillis = millis();
  }

  // =============================
  // 2. PHONE JAIL
  // =============================
  if (usePhoneJail) {
    if (!checkNFC(2)) {
      if (!isDistracted) {
        isDistracted = true;
        distractionStartTime = millis();
        lastDistractReminder = millis();
        distractCount++;
        client.publish("pomobox/nfc_status", "{\"status\":\"REMOVED\"}");
      }

      if (millis() - lastJailWarning > 10000) {
        playVoice("Mohon letakkan handphone anda kembali");
        lastJailWarning = millis();
      }

    } else {
      if (isDistracted) {
        isDistracted = false;
        totalDistractionTime += (millis() - distractionStartTime) / 1000;
        client.publish("pomobox/nfc_status", "{\"status\":\"LOCKED\"}");
        lastInputTime = millis();
      }
    }
  }

  // =============================
  // 3. DISPLAY
  // =============================
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.print("FOKUS #");
  display.print(totalPomodorosDone + 1);

  int m = remainingSeconds / 60;
  int s = remainingSeconds % 60;
  display.setTextSize(3);
  display.setCursor(20, 25);
  if (m < 10) display.print("0");
  display.print(m);
  display.print(":");
  if (s < 10) display.print("0");
  display.print(s);
  display.display();

  // =============================
  // 4. MANUAL DISTRACT
  // =============================
  if (digitalRead(BTN_DISTRACT) == HIGH && !isDistracted) {
    beep();
    isDistracted = true;
    distractionStartTime = millis();
    lastDistractReminder = millis();
    distractCount++;

    if (!usePhoneJail) {
      client.publish("pomobox/nfc_status", "{\"status\":\"REMOVED\"}");
    }
    delay(300);
  }

  // =============================
  // 5. FORCE STOP
  // =============================
  if (digitalRead(BTN_NO) == HIGH && !focusPaused) {
    focusPaused = true;
    beep();
    currentState = STATE_CONFIRM_END;
    delay(300);
  }
}

// 7. BREAK WAITING (Trigger Hijau)
void loopBreakWait(bool isLong) {
  rotatingEffect();  // Lampu muter (Standby)
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  if (isLong) display.println("Istirahat Panjang?");
  else display.println("Istirahat Pendek?");
  display.println("");
  display.println("Mulai");
  display.display();

  if (digitalRead(BTN_YES) == HIGH) {
    setEyeColor(0, 255, 0);
    beep();
    lastInputTime = millis();
    // Set Waktu
    if (isLong) {
      remainingSeconds = isSecondsMode()
                           ? timeOptions[selectedConfigIndex].longBreak
                           : timeOptions[selectedConfigIndex].longBreak * 60;
    } else {
      remainingSeconds = isSecondsMode()
                           ? timeOptions[selectedConfigIndex].shortBreak
                           : timeOptions[selectedConfigIndex].shortBreak * 60;
    }


    lastMillis = millis();
    playVoice("Waktunya Istirahat!");
    currentState = isLong ? STATE_LONG_BREAK_RUN : STATE_SHORT_BREAK_RUN;
  }
}

// 8. BREAK RUNNING
void loopBreakRun(bool isLong) {
  rotatingEffect();  // Lampu muter (Istirahat)

  if (millis() - lastMillis >= 1000) {
    if (remainingSeconds > 0) remainingSeconds--;
    else {
      playVoice("Persiapan Belajar Kembali!");
      // Setelah break selesai -> Perlu Trigger lagi untuk mulai fokus
      startAlarm();
      currentState = STATE_READY_TO_START;
      return;
    }
    lastMillis = millis();
  }

  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println(isLong ? "LONG BREAK" : "SHORT BREAK");
  int m = remainingSeconds / 60;
  int s = remainingSeconds % 60;
  display.setTextSize(3);
  display.setCursor(20, 25);
  if (m < 10) display.print("0");
  display.print(m);
  display.print(":");
  if (s < 10) display.print("0");
  display.print(s);
  display.display();
}

// 9. ASK FINISHED (Setelah 4 Siklus / Estimasi)
void loopAskFinished() {
  rotatingEffect();
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Tugas Selesai?");
  display.println("");
  display.println("Ya");
  display.println("Tidak");
  display.display();

  if (digitalRead(BTN_YES) == HIGH) {  // YA SELESAI
    setEyeColor(0, 255, 0);
    beep();
    lastInputTime = millis();
    sendSessionData(true);  // Kirim Data Finish
    currentState = STATE_FINISH;
  } else if (digitalRead(BTN_NO) == HIGH) {  // BELUM SELESAI
    setEyeColor(255, 0, 0);
    beep();
    lastInputTime = millis();
    // Cek apakah tadi trigger dari 4 siklus?
    currentState = STATE_SHORT_BREAK_WAIT;
  }
}

// 10. CONFIRM END (Berhenti Tengah Jalan) - MODIFIKASI TAMPILAN
void loopConfirmEnd() {
  rotatingEffect();
  display.clearDisplay();

  // UKURAN KECIL SESUAI PERMINTAAN
  display.setTextSize(1);
  display.setTextColor(WHITE);

  // Header ditengah
  display.setCursor(25, 10);
  display.println("AKHIRI SESI?");

  // Menu Pilihan yang Rapi
  display.setCursor(10, 35);
  display.println("YA");

  display.setCursor(10, 50);
  display.println("BATAL");

  display.display();

  if (digitalRead(BTN_YES) == HIGH) {
    setEyeColor(0, 255, 0);
    beep();
    currentState = STATE_SAVE_DATA;
    delay(300);
  } else if (digitalRead(BTN_NO) == HIGH) {
    setEyeColor(255, 0, 0);
    beep();
    focusPaused = false;
    lastMillis = millis();
    currentState = STATE_POMODORO_RUN;
    delay(300);
  }
}

// 11. SAVE DATA
void loopSaveData() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("Simpan Data?");
  display.println("");
  display.println("YA");
  display.println("TIDAK");
  display.display();

  if (digitalRead(BTN_YES) == HIGH) {
    setEyeColor(0, 255, 0);
    beep();
    sendSessionData(true);
    display.clearDisplay();
    display.setCursor(25, 25);
    display.println("Data Disimpan");
    display.display();
    delay(2000);
    currentState = STATE_BOOT;  // Balik Halo
  } else if (digitalRead(BTN_NO) == HIGH) {
    setEyeColor(255, 0, 0);
    beep();
    display.clearDisplay();
    display.setCursor(25, 25);
    display.println("Data Dibuang");
    display.display();
    delay(2000);
    currentState = STATE_BOOT;  // Balik Halo
    isNewSession = true;
  }
}

// 12. FINISH
void loopFinish() {
  display.clearDisplay();
  drawFace("HAPPY", "SELESAI!");
  playVoice("Terima Kasih Telah Menggunakan PomoBox");
  delay(5000);
  currentState = STATE_BOOT;  // Balik Halo
  isNewSession = true;
}

// --- SEND DATA TO MQTT ---
void sendSessionData(bool saved) {
  if (!saved) return;

  if (currentState == STATE_ASK_FINISHED) {
    client.publish(
      "pomobox/task_done",
      ("{\"task_id\": " + String(currentTasks[selectedTaskIndex].id) + "}").c_str());
  }

  DynamicJsonDocument doc(1024);

  doc["task_id"] = currentTasks[selectedTaskIndex].id;
  doc["action_id"] = timeOptions[selectedConfigIndex].actionId;
  doc["pomodoro_index"] = totalPomodorosDone;
  doc["cycle"] = cycleCount;

  int planned = isSecondsMode()
                  ? timeOptions[selectedConfigIndex].focus
                  : timeOptions[selectedConfigIndex].focus * 60;

  if (isDistracted) {
    totalDistractionTime += (millis() - distractionStartTime) / 1000;
    isDistracted = false;
  }

  unsigned long now = millis();
  unsigned long elapsed = (now - focusStartMillis) / 1000;
  long actual = elapsed - totalDistractionTime;

  if (actual < 0) actual = 0;
  if (actual > planned) actual = planned;

  doc["planned_sec"] = planned;
  doc["actual_sec"] = actual;
  doc["distract_sec"] = totalDistractionTime;
  doc["distract_count"] = distractCount;

  String payload;
  serializeJson(doc, payload);
  client.publish("pomobox/session_end", payload.c_str());
}


// KIRIM HEARTBEAT (Untuk Status Online Dashboard)
void sendHeartbeat() {
  if (millis() - lastHeartbeat > 5000) {  // Kirim tiap 5 detik
    lastHeartbeat = millis();
    client.publish("pomobox/heartbeat", "{\"status\":\"online\"}");
  }
}

// --- FUNGSI AI (RECORD -> STT -> GEMINI -> TTS) ---
void recordAudio() {
  isRecording = true;
  audio.stopSong();
  delay(50);
  drawFace("LISTEN", "Mendengar...");

  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  i2s_new_channel(&chan_cfg, NULL, &rx_handle);
  i2s_std_config_t std_cfg = {
    .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)MIC_SCK,
      .ws = (gpio_num_t)MIC_WS,
      .dout = I2S_GPIO_UNUSED,
      .din = (gpio_num_t)MIC_SD,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;
  i2s_channel_init_std_mode(rx_handle, &std_cfg);
  i2s_channel_enable(rx_handle);

  int buffer_ptr = headerSize / 2;
  int32_t sample_buffer[64];
  int16_t* recording_buffer = (int16_t*)microphonedata0;
  size_t bytes_read = 0;
  unsigned long start_t = millis();

  while (digitalRead(BTN_VOICE) == HIGH) {
    rotatingEffect();  // Efek Putar saat rekam
    if (millis() - start_t > (record_time_max * 1000)) break;
    if (i2s_channel_read(rx_handle, sample_buffer, sizeof(sample_buffer), &bytes_read, 1000) == ESP_OK) {
      int samples = bytes_read / 4;
      for (int i = 0; i < samples; i++) {
        int32_t val = sample_buffer[i] >> 14;
        val = val * 8;
        if (val > 32767) val = 32767;
        else if (val < -32768) val = -32768;
        recording_buffer[buffer_ptr++] = (int16_t)val;
      }
    }
  }

  beep();
  size_t audioDataSize = (buffer_ptr * 2) - headerSize;
  bytes_recorded = buffer_ptr * 2;
  byte wavHeader[headerSize];
  generateWavHeader(wavHeader, audioDataSize);
  memcpy(microphonedata0, wavHeader, headerSize);
  i2s_channel_disable(rx_handle);
  i2s_del_channel(rx_handle);
  rx_handle = NULL;

  processAI();
}

void processAI() {
  drawFace("THINK", "Memproses...");
  rotatingEffect();  // Efek Putar saat mikir
  String textInput = stt_assemblyai();
  if (textInput.length() > 1) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("Kamu:");
    display.println(textInput);
    display.display();
    delay(2000);
    drawFace("THINK", "Pobo Mikir...");
    String reply = ask_gemini(textInput);
    drawFace("SPEAK", "Pobo Bicara...");
    setEyeColor(150, 150, 150);  // Putih saat bicara
    tts_elevenlabs(reply);
    while (audio.isRunning()) audio.loop();
  } else {
    drawFace("SAD", "Gak Denger...");
    beep();
    delay(1000);
  }
}

// ... HELPER FUNGSI (Sama seperti sebelumnya) ...
void connectWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  display.clearDisplay();
  display.setCursor(0, 20);
  display.println("Connect WiFi...");
  display.display();
  int retry = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    retry++;
    if (retry > 20) ESP.restart();
  }
}
// ==========================================
// TIMEOUT & VOICE BUTTON HANDLER
// ==========================================
void checkTimeoutAndVoice() {

  // === AI Push-to-talk ===
  if (digitalRead(BTN_VOICE) == HIGH && !isRecording) {
    lastInputTime = millis();
    SystemState beforeAI = currentState;
    currentState = STATE_AI_PROCESS;
    recordAudio();
    currentState = beforeAI;

    return;
  }

  // === Timeout reminder ===
  if (millis() - lastInputTime > 60000) {  // 60 detik tanpa input
    if (millis() - lastWarningSound > 60000) {
      playVoice("Silakan lanjutkan sesi Anda");
      lastWarningSound = millis();
    }
  }
}

// ==============================
// TIME MODE CHECK
// ==============================
bool isSecondsMode() {
  return timeOptions[selectedConfigIndex].actionId == "AS";
}

void connectMQTT() {
  while (!client.connected()) {
    if (client.connect("PomoBox_ESP32")) {
      client.subscribe("pomobox/#");
    } else delay(2000);
  }
}
void playVoice(String text) {
  if (audio.isRunning()) audio.stopSong();
  String url = "http://translate.google.com/translate_tts?ie=UTF-8&tl=id&client=tw-ob&q=" + text;
  audio.connecttohost(url.c_str());
}
void setEyeColor(int r, int g, int b) {
  for (int i = 0; i < NUM_PIXELS; i++) strip.setPixelColor(i, strip.Color(r, g, b));
  strip.show();
}
void beep() {
  digitalWrite(BUZZER_PIN, HIGH);
  delay(50);
  digitalWrite(BUZZER_PIN, LOW);
}
bool checkNFC(int retries) {
  uint8_t uid[] = { 0, 0, 0, 0, 0, 0, 0 };
  uint8_t len;
  nfc.setPassiveActivationRetries(retries);
  return nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &len);
}

// Efek Putar
void rotatingEffect() {
  if (millis() - lastPixelStep > 100) {
    lastPixelStep = millis();
    uint32_t dim = strip.Color(0, 5, 10);
    uint32_t bright;
    if (colorState == 0) bright = strip.Color(0, 150, 150);
    else if (colorState == 1) bright = strip.Color(0, 0, 255);
    else bright = strip.Color(100, 0, 200);
    for (int i = 0; i < NUM_PIXELS; i++) strip.setPixelColor(i, dim);
    strip.setPixelColor(pixelHead, bright);
    strip.show();
    pixelHead++;
    if (pixelHead >= NUM_PIXELS) {
      pixelHead = 0;
      pixelHead = 0;
      colorState = (colorState + 1) % 3;
    }
  }
}

void drawFace(String expression, String text) {
  display.clearDisplay();
  if (expression == "HAPPY") {
    display.fillRoundRect(30, 20, 10, 10, 2, WHITE);
    display.fillRoundRect(88, 20, 10, 10, 2, WHITE);
    display.drawCircle(64, 40, 10, WHITE);
    display.fillRect(50, 30, 30, 12, BLACK);
  } else if (expression == "IDLE") {
    display.drawCircle(35, 25, 8, WHITE);
    display.drawCircle(93, 25, 8, WHITE);
    display.drawLine(50, 45, 78, 45, WHITE);
  } else if (expression == "THINK") {
    display.drawCircle(35, 25, 8, WHITE);
    display.fillCircle(35, 20, 3, WHITE);
    display.drawCircle(93, 25, 8, WHITE);
    display.fillCircle(93, 20, 3, WHITE);
    display.drawLine(55, 45, 73, 40, WHITE);
  } else if (expression == "LISTEN") {
    display.fillCircle(35, 25, 8, WHITE);
    display.fillCircle(93, 25, 8, WHITE);
    display.drawLine(50, 45, 78, 45, WHITE);
  } else if (expression == "SPEAK") {
    display.fillCircle(35, 25, 6, WHITE);
    display.fillRect(25, 25, 20, 10, BLACK);
    display.fillCircle(93, 25, 6, WHITE);
    display.fillRect(83, 25, 20, 10, BLACK);
    display.fillCircle(64, 45, 8, WHITE);
  }
  display.setTextSize(1);
  display.setCursor(20, 55);
  display.print(text);
  display.display();
}

// Copy fungsi STT/LLM/TTS/WavHeader dari kode sebelumnya agar hemat tempat
// (Logic API Network sama persis)

// ==========================================
// FUNGSI AUDIO & AI (LENGKAP)
// ==========================================

// 1. TEXT TO SPEECH (ELEVENLABS)
void tts_elevenlabs(String text) {
  if (text == "") return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // URL API ElevenLabs
  String url = "https://api.elevenlabs.io/v1/text-to-speech/" + eleven_voice_id + "?optimize_streaming_latency=3";

  // JSON Payload
  StaticJsonDocument<1024> doc;
  doc["text"] = text;
  doc["model_id"] = "eleven_multilingual_v2";
  doc["voice_settings"]["stability"] = 0.5;
  doc["voice_settings"]["similarity_boost"] = 0.8;
  String json;
  serializeJson(doc, json);

  http.begin(client, url);
  http.addHeader("xi-api-key", eleven_api_key);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(json);
  if (code == 200) {
    // Hapus file lama jika ada
    if (SPIFFS.exists("/reply.mp3")) SPIFFS.remove("/reply.mp3");

    // Simpan Stream Audio ke File
    File file = SPIFFS.open("/reply.mp3", FILE_WRITE);
    if (!file) {
      Serial.println("Gagal buka file SPIFFS");
      return;
    }

    http.writeToStream(&file);
    file.close();
    http.end();

    // Putar Audio
    audio.connecttoFS(SPIFFS, "/reply.mp3");
  } else {
    Serial.print("TTS Error: ");
    Serial.println(http.getString());
    http.end();
  }
}

// 2. GEMINI AI (POBO MIKIR)
String ask_gemini(String text) {
  if (text == "") return "Maaf, saya tidak dengar.";

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // Timeout 15 detik
  http.setTimeout(15000);

  String url = "https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=" + String(gemini_api_key);

  StaticJsonDocument<2048> doc;
  // Prompt System
  String systemPrompt = "Kamu adalah Pobo, asisten belajar yang ceria. Jawablah dengan santai, informatif, singkat (max 40 kata) dalam bahasa Indonesia: ";

  doc["contents"][0]["parts"][0]["text"] = systemPrompt + text;
  String json;
  serializeJson(doc, json);

  http.begin(client, url);
  http.addHeader("Content-Type", "application/json");

  int code = http.POST(json);
  String res = http.getString();
  http.end();

  if (code != 200) {
    Serial.print("Gemini Error: ");
    Serial.println(code);
    return "Maaf, Pobo lagi pusing.";
  }

  // Parse JSON Response
  DynamicJsonDocument responseDoc(5000);
  DeserializationError error = deserializeJson(responseDoc, res);

  if (!error) {
    const char* answer = responseDoc["candidates"][0]["content"]["parts"][0]["text"];
    if (answer) {
      String finalAns = String(answer);
      // Bersihkan simbol markdown agar enak dibaca TTS
      finalAns.replace("*", "");
      finalAns.replace("#", "");
      finalAns.replace("\n", ". ");
      return finalAns;
    }
  }

  return "Maaf, Pobo tidak mengerti.";
}

// 3. SPEECH TO TEXT (ASSEMBLY AI)
String stt_assemblyai() {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  // A. UPLOAD AUDIO
  Serial.println("1. Uploading Audio...");
  String upload_url = "";

  if (http.begin(client, "https://api.assemblyai.com/v2/upload")) {
    http.addHeader("Authorization", assembly_api_key);
    http.addHeader("Content-Type", "application/octet-stream");

    int httpCode = http.POST(microphonedata0, bytes_recorded);

    if (httpCode == 200) {
      String response = http.getString();
      StaticJsonDocument<512> doc;
      deserializeJson(doc, response);
      upload_url = doc["upload_url"].as<String>();
    } else {
      Serial.println("Upload Failed");
      http.end();
      return "";
    }
    http.end();
  }

  if (upload_url == "") return "";

  // B. REQUEST TRANSCRIPT
  Serial.println("2. Requesting Transcript...");
  String transcript_id = "";

  if (http.begin(client, "https://api.assemblyai.com/v2/transcript")) {
    http.addHeader("Authorization", assembly_api_key);
    http.addHeader("Content-Type", "application/json");

    StaticJsonDocument<512> doc;
    doc["audio_url"] = upload_url;
    doc["language_code"] = "id";  // Bahasa Indonesia
    String json;
    serializeJson(doc, json);

    int httpCode = http.POST(json);
    if (httpCode == 200) {
      String response = http.getString();
      StaticJsonDocument<512> respDoc;
      deserializeJson(respDoc, response);
      transcript_id = respDoc["id"].as<String>();
    } else {
      http.end();
      return "";
    }
    http.end();
  }

  // C. POLLING STATUS
  Serial.print("3. Processing");
  String status = "queued";
  String textResult = "";

  while (status != "completed" && status != "error") {
    // Efek Lampu Loading saat menunggu
    rotatingEffect();
    delay(500);
    Serial.print(".");

    if (http.begin(client, "https://api.assemblyai.com/v2/transcript/" + transcript_id)) {
      http.addHeader("Authorization", assembly_api_key);
      int httpCode = http.GET();
      if (httpCode == 200) {
        String response = http.getString();
        StaticJsonDocument<2048> doc;
        deserializeJson(doc, response);

        status = doc["status"].as<String>();
        if (status == "completed") {
          textResult = doc["text"].as<String>();
        }
      }
      http.end();
    }
  }

  Serial.println("\nResult: " + textResult);
  return textResult;
}

// 4. GENERATE WAV HEADER
void generateWavHeader(byte* header, int waveDataSize) {
  int sampleRate = SAMPLE_RATE;
  int numChannels = 1;
  int bitsPerSample = 16;
  int byteRate = sampleRate * numChannels * (bitsPerSample / 8);

  header[0] = 'R';
  header[1] = 'I';
  header[2] = 'F';
  header[3] = 'F';
  unsigned int fileSize = waveDataSize + headerSize - 8;
  header[4] = (byte)(fileSize & 0xFF);
  header[5] = (byte)((fileSize >> 8) & 0xFF);
  header[6] = (byte)((fileSize >> 16) & 0xFF);
  header[7] = (byte)((fileSize >> 24) & 0xFF);
  header[8] = 'W';
  header[9] = 'A';
  header[10] = 'V';
  header[11] = 'E';
  header[12] = 'f';
  header[13] = 'm';
  header[14] = 't';
  header[15] = ' ';
  header[16] = 16;
  header[17] = 0;
  header[18] = 0;
  header[19] = 0;
  header[20] = 1;
  header[21] = 0;
  header[22] = (byte)numChannels;
  header[23] = 0;
  header[24] = (byte)(sampleRate & 0xFF);
  header[25] = (byte)((sampleRate >> 8) & 0xFF);
  header[26] = (byte)((sampleRate >> 16) & 0xFF);
  header[27] = (byte)((sampleRate >> 24) & 0xFF);
  header[28] = (byte)(byteRate & 0xFF);
  header[29] = (byte)((byteRate >> 8) & 0xFF);
  header[30] = (byte)((byteRate >> 16) & 0xFF);
  header[31] = (byte)((byteRate >> 24) & 0xFF);
  header[32] = (byte)(numChannels * (bitsPerSample / 8));
  header[33] = 0;
  header[34] = 16;
  header[35] = 0;
  header[36] = 'd';
  header[37] = 'a';
  header[38] = 't';
  header[39] = 'a';
  header[40] = (byte)(waveDataSize & 0xFF);
  header[41] = (byte)((waveDataSize >> 8) & 0xFF);
  header[42] = (byte)((waveDataSize >> 16) & 0xFF);
  header[43] = (byte)((waveDataSize >> 24) & 0xFF);
}
