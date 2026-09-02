# PomoBox Firmware (ESP32 S3)

Firmware untuk perangkat PomoBox, sebuah AIoT study companion berbasis ESP32 S3 yang menggabungkan Pomodoro timer, mekanisme Phone Jail berbasis NFC, dan voice assistant AI. Firmware ini menjalankan seluruh state machine sesi belajar, membaca input fisik dari rotary encoder dan tombol, menampilkan status di layar OLED, mengendalikan indikator LED NeoPixel, serta berkomunikasi dengan backend melalui HTTP dan MQTT.

## Fitur Utama

* State machine sesi belajar lengkap, mulai dari pemilihan tugas, pemilihan durasi, konfirmasi Phone Jail, sesi fokus, istirahat pendek, istirahat panjang, hingga penyimpanan data sesi
* Phone Jail berbasis modul NFC PN532, mendeteksi keberadaan HP di dalam kotak selama sesi fokus berlangsung dan mencatat waktu distraksi
* Voice assistant berbasis alur rekam audio ke Speech to Text (AssemblyAI), diproses oleh Gemini API untuk menghasilkan jawaban, lalu diubah kembali menjadi suara melalui ElevenLabs Text to Speech
* Dashboard sinkronisasi melalui MQTT untuk status heartbeat, status NFC, mulai sesi, akhir sesi, dan penyelesaian tugas
* Pengambilan daftar tugas dan rekomendasi durasi belajar dari server backend melalui HTTP
* Tampilan wajah ekspresif di layar OLED dan efek cahaya berputar di NeoPixel sebagai indikator status
* Voice feedback yang bisa dikirim dari dashboard ke perangkat melalui topik MQTT tertentu, lalu diucapkan langsung oleh perangkat

## Perangkat Keras yang Dibutuhkan

| Komponen | Keterangan |
|---|---|
| Mikrokontroler | ESP32 S3 |
| Layar | OLED SSD1306 128x64 (I2C) |
| Modul NFC | PN532 (SPI) |
| LED Indikator | NeoPixel 8 piksel |
| Input | Rotary encoder, 4 tombol push button |
| Audio | Speaker I2S dan mikrofon I2S |
| Buzzer | Buzzer aktif untuk umpan balik suara pendek |

## Pinout

### OLED (I2C)
SDA pada pin 1, SCL pada pin 2

### NFC PN532 (SPI)
SCK pada pin 18, MOSI pada pin 17, SS pada pin 15, MISO pada pin 16

### NeoPixel
Data pada pin 48, jumlah piksel 8

### Tombol dan Rotary
Rotary CLK pada pin 4, rotary DT pada pin 5, rotary SW (OK) pada pin 10, tombol merah Back/No pada pin 13, tombol hijau Start/Yes pada pin 14, tombol kuning Distraksi pada pin 12, tombol hitam Voice pada pin 21

### Audio I2S
Speaker BCLK pada pin 41, speaker LRC pada pin 42, speaker DOUT pada pin 40, mikrofon SCK pada pin 38, mikrofon WS pada pin 39, mikrofon SD pada pin 6

### Lainnya
Buzzer pada pin 9

## Library yang Digunakan

* Adafruit GFX Library
* Adafruit SSD1306
* Adafruit PN532
* Adafruit NeoPixel
* WiFi (ESP32 core)
* HTTPClient dan WiFiClientSecure (ESP32 core)
* ArduinoJson
* PubSubClient
* ESP32 Audio (I2S audio playback dan streaming)
* FS dan SPIFFS
* driver/i2s_std (ESP IDF I2S driver)

## Konfigurasi Sebelum Upload

Sebelum meng upload firmware ini, sesuaikan bagian konfigurasi di awal file dengan milik Anda sendiri. Jangan menyimpan kredensial asli langsung di file yang akan dipush ke repository publik.

```cpp
const char* ssid = "NAMA_WIFI_ANDA";
const char* password = "PASSWORD_WIFI_ANDA";

const char* server_host = "IP_LOKAL_SERVER_BACKEND";
const int server_port = 5000;
const char* mqtt_server = "broker.hivemq.com";

const char* assembly_api_key = "API_KEY_ASSEMBLYAI_ANDA";
const char* gemini_api_key = "API_KEY_GEMINI_ANDA";
const char* eleven_api_key = "API_KEY_ELEVENLABS_ANDA";
String eleven_voice_id = "VOICE_ID_ELEVENLABS_ANDA";
```

Nilai `server_host` harus diisi dengan alamat IP lokal komputer yang menjalankan backend Python (app.py). Alamat ini bisa dicek melalui perintah ipconfig pada Windows.

Peringatan keamanan, kode asli proyek ini sempat menyimpan seluruh kredensial di atas secara langsung dalam bentuk teks polos. Jika riwayat ini pernah terupload ke repository publik, sebaiknya seluruh API key yang bersangkutan segera dicabut (revoke) dan diganti dengan yang baru, lalu kredensial dipindahkan ke file konfigurasi terpisah yang dimasukkan ke .gitignore.

## Arsitektur Komunikasi

### HTTP
Digunakan untuk mengambil daftar tugas dari endpoint /api/tasks/list dan mengambil rekomendasi durasi belajar dari endpoint /api/recommendation pada backend.

### MQTT
Broker yang digunakan adalah broker.hivemq.com. Perangkat subscribe ke seluruh topik dengan pola pomobox/# dan mempublikasikan data ke topik topik berikut.

| Topik | Fungsi |
|---|---|
| pomobox/heartbeat | Menandakan perangkat sedang online, dikirim tiap 5 detik |
| pomobox/nfc_status | Status keberadaan HP di Phone Jail (LOCKED, REMOVED, UNUSED) |
| pomobox/session_start | Dikirim saat sesi fokus dimulai |
| pomobox/session_end | Dikirim saat data sesi disimpan, berisi durasi rencana, durasi aktual, durasi distraksi, dan jumlah distraksi |
| pomobox/task_done | Dikirim saat sebuah tugas dinyatakan selesai |
| pomobox/voice_feedback | Topik yang disubscribe perangkat untuk menerima pesan dari dashboard yang kemudian diucapkan melalui speaker |

## Alur Voice Assistant

Alur voice assistant berjalan sebagai berikut. Pengguna menekan dan menahan tombol Voice untuk merekam audio melalui mikrofon I2S. Audio hasil rekaman diunggah ke AssemblyAI untuk diubah menjadi teks (Speech to Text). Teks tersebut dikirim ke Gemini API dengan system prompt sebagai Pobo, asisten belajar yang menjawab singkat dan santai dalam Bahasa Indonesia. Jawaban dari Gemini kemudian dikirim ke ElevenLabs untuk diubah menjadi audio (Text to Speech), disimpan sementara di SPIFFS sebagai reply.mp3, lalu diputar melalui speaker perangkat.

## State Machine Sesi Belajar

Perangkat menjalankan state machine dengan urutan besar sebagai berikut, boot menunggu tombol hijau, pemilihan tugas dari daftar yang diambil dari server, pemilihan durasi Pomodoro (dengan indikator rekomendasi dari sistem), konfirmasi penggunaan Phone Jail, menunggu perangkat siap (mengecek tag NFC jika Phone Jail aktif), sesi fokus berjalan, jeda menunggu konfirmasi lanjut ke istirahat, istirahat pendek atau istirahat panjang tergantung jumlah siklus, konfirmasi tugas selesai, konfirmasi pengakhiran sesi lebih awal, konfirmasi penyimpanan data, lalu kembali ke boot.

Pilihan durasi Pomodoro yang tersedia meliputi 15/3/10 menit, 20/5/15 menit, 25/5/20 menit, 30/7/25 menit, 35/10/30 menit, serta satu mode pengujian dengan satuan detik untuk keperluan testing cepat.

## Struktur Kode

Firmware ini berada dalam satu file .ino dengan pembagian bagian sebagai berikut, konfigurasi jaringan dan server, definisi pin, objek dan variabel global, prototipe fungsi, fungsi setup, fungsi loop utama, implementasi logika tiap state, fungsi pengiriman data MQTT, serta fungsi fungsi AI (rekam audio, Speech to Text, Gemini, Text to Speech, dan pembuatan header WAV).

## Cara Upload

1. Buka file firmware ini menggunakan Arduino IDE dengan board package ESP32 terpasang
2. Pilih board ESP32 S3 yang sesuai pada menu Tools
3. Pastikan opsi PSRAM diaktifkan karena buffer audio menggunakan ps_malloc
4. Isi bagian konfigurasi WiFi, server backend, dan API key sesuai milik Anda
5. Hubungkan seluruh komponen sesuai pinout di atas
6. Upload firmware ke board

## Catatan Batasan

Durasi Pomodoro pada firmware ini bersifat tetap sesuai pilihan yang tersedia dan tidak menyesuaikan secara otomatis di tengah sesi. Pemutaran suara sistem (playVoice) memanfaatkan endpoint Google Translate TTS sehingga membutuhkan koneksi internet aktif dan bergantung pada ketersediaan layanan tersebut.
