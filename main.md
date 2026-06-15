// =============================================================================
// SISTEM ABSENSI IoT — ESP32 + AS608 + OLED SSD1306
// Fix dari kode lama:
// 1. Wire.begin() dipanggil PERTAMA sebelum apapun
// 2. display.clearDisplay() + display.display() selalu dipasangkan
// 3. Tidak ada delay() di dalam showOLED()
// 4. Typo WIFI_PASSWORD → WIFI_PASS diperbaiki
// 5. sendAttendance() pakai endpoint & payload sesuai PRD
// =============================================================================

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <time.h>

// --- OLED ---
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
#define OLED_ADDR 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
bool oledOk = false;

// --- AS608 ---
HardwareSerial mySerial(2); // UART2: RX=16, TX=17
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

// --- Konfigurasi ---
#define WIFI_SSID "Bengkod WD"
#define WIFI_PASS "sayangucup"
#define BACKEND_BASE "http://192.168.1.142:8000/api/device"
#define DEVICE_TOKEN "alat-gudang1-secret"

// --- Timing ---
#define SCAN_COOLDOWN_MS 3000
#define WIFI_RETRY_MS 30000
#define POLL_INTERVAL_MS 5000

// --- State ---
bool wifiConnected = false;
unsigned long lastScanMs = 0;
unsigned long lastPollMs = 0;
unsigned long lastWifiRetryMs = 0;

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================
void showOLED(const String& l1, const String& l2 = "", const String& l3 = "");
void showOLEDIdle();
String generateScanUid();
String getTimestamp();
void sendAttendance(int fingerprintId);
void checkWifiReconnect();
void tickScan();
void tickTaskPoller();

// =============================================================================
// SETUP
// =============================================================================
void setup() {
Serial.begin(9600);
delay(200);
Serial.println(F("\n[BOOT] Sistem Absensi IoT"));

// ----------------------------------------------------------------
// STEP 1: I2C + OLED — harus PERTAMA agar bisa tampil status boot
// ----------------------------------------------------------------
Wire.begin(21, 22); // SDA=21, SCL=22 (default ESP32)
delay(50);

if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
Serial.println(F("[ERROR] OLED gagal init!"));
oledOk = false;
// Lanjut tanpa OLED, jangan halt — AS608 & WiFi tetap jalan
} else {
oledOk = true;
display.clearDisplay();
display.display(); // push buffer kosong dulu — ini yang hilangkan noise!
delay(50);
Serial.println(F("[OK] OLED init berhasil"));
}

showOLED("Sistem Absensi", "Initializing...", "");

// ----------------------------------------------------------------
// STEP 2: AS608 Fingerprint Sensor
// ----------------------------------------------------------------
mySerial.begin(57600, SERIAL_8N1, 16, 17);
delay(100);
finger.begin(57600);
delay(200);

if (finger.verifyPassword()) {
Serial.println(F("[OK] AS608 terdeteksi"));
showOLED("AS608 OK!", "Sensor siap", "");
} else {
Serial.println(F("[ERROR] AS608 tidak ditemukan! Cek RX/TX."));
showOLED("AS608 ERROR!", "Cek kabel!", "RX/TX kebalik?");
while (true) delay(1000);
}

// ----------------------------------------------------------------
// STEP 3: WiFi
// ----------------------------------------------------------------
showOLED("Connecting WiFi", WIFI_SSID, "");
WiFi.mode(WIFI_STA);
WiFi.begin(WIFI_SSID, WIFI_PASS); // <-- fix typo: WIFI_PASS bukan WIFI_PASSWORD
Serial.print(F("[WiFi] Menghubungkan"));

int attempts = 0;
while (WiFi.status() != WL_CONNECTED && attempts < 20) {
delay(500);
Serial.print(".");
attempts++;
}
Serial.println();

if (WiFi.status() == WL_CONNECTED) {
wifiConnected = true;
Serial.println("[OK] WiFi: " + WiFi.localIP().toString());
showOLED("WiFi Connected!", WiFi.localIP().toString(), "");

    // NTP Sync (WIB = UTC+7)
    configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print(F("[NTP] Sync"));
    struct tm t;
    int ntpTry = 0;
    while (!getLocalTime(&t) && ntpTry < 20) {
      delay(500);
      Serial.print(".");
      ntpTry++;
    }
    Serial.println();
    if (getLocalTime(&t)) {
      char buf[32];
      strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
      Serial.printf("[OK] Waktu: %s\n", buf);
    }

} else {
wifiConnected = false;
Serial.println(F("[WARN] WiFi gagal — mode offline"));
showOLED("WiFi Gagal!", "Mode offline", "");
}

delay(1500);
showOLEDIdle();
Serial.println(F("[BOOT] Siap."));
}

// =============================================================================
// LOOP
// =============================================================================
void loop() {
checkWifiReconnect();
tickScan();
tickTaskPoller();
}

// =============================================================================
// WiFi RECONNECT (non-blocking)
// =============================================================================
void checkWifiReconnect() {
if (WiFi.status() == WL_CONNECTED) {
wifiConnected = true;
return;
}
wifiConnected = false;
if (millis() - lastWifiRetryMs < WIFI_RETRY_MS) return;

lastWifiRetryMs = millis();
Serial.println(F("[WiFi] Reconnecting..."));
showOLED("WiFi Putus!", "Reconnecting...", "");
WiFi.reconnect();

for (int i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) delay(500);

if (WiFi.status() == WL_CONNECTED) {
wifiConnected = true;
Serial.println(F("[OK] WiFi terhubung kembali"));
}
showOLEDIdle();
}

// =============================================================================
// SCAN SIDIK JARI (non-blocking, pakai cooldown)
// =============================================================================
void tickScan() {
if (millis() - lastScanMs < SCAN_COOLDOWN_MS) return;

uint8_t p = finger.getImage();
if (p == FINGERPRINT_NOFINGER) return; // tidak ada jari, normal
if (p != FINGERPRINT_OK) return; // noise, abaikan

p = finger.image2Tz();
if (p != FINGERPRINT_OK) return;

p = finger.fingerSearch();

if (p == FINGERPRINT_OK) {
Serial.printf("[SCAN] Match: slot=%d confidence=%d\n",
finger.fingerID, finger.confidence);
showOLED("Memproses...", "", "");
sendAttendance(finger.fingerID);
lastScanMs = millis();
} else if (p == FINGERPRINT_NOTFOUND) {
Serial.println(F("[SCAN] Jari tidak dikenal"));
showOLED("Tidak Dikenal!", "Jari tidak", "terdaftar");
lastScanMs = millis();
delay(2000);
showOLEDIdle();
}
}

// =============================================================================
// KIRIM ABSENSI KE SERVER
// Endpoint: POST /api/device/attendance
// Header : X-Device-Token
// Body : { fingerprint_id, scan_uid, waktu_scan }
// =============================================================================
void sendAttendance(int fingerprintId) {
if (!wifiConnected) {
showOLED("Offline!", "Data tidak", "terkirim");
delay(2000);
showOLEDIdle();
return;
}

String scanUid = generateScanUid();
String waktuScan = getTimestamp();

// Build JSON dengan ArduinoJson
StaticJsonDocument<256> doc;
doc["fingerprint_id"] = fingerprintId;
doc["scan_uid"] = scanUid;
doc["waktu_scan"] = waktuScan;

String body;
serializeJson(doc, body);

Serial.printf("[ATTEND] POST slot=%d uid=%s\n",
fingerprintId, scanUid.c_str());

HTTPClient http;
http.begin(String(BACKEND_BASE) + "/attendance");
http.addHeader("Content-Type", "application/json");
http.addHeader("X-Device-Token", DEVICE_TOKEN);
http.setTimeout(10000);

int httpCode = http.POST(body);
String resp = (httpCode > 0) ? http.getString() : "";
http.end();

Serial.printf("[ATTEND] HTTP=%d\n", httpCode);

if (httpCode == 200) {
// Parse response: { status, hasil, display_oled, nama_karyawan }
StaticJsonDocument<512> respDoc;
DeserializationError err = deserializeJson(respDoc, resp);

    if (!err) {
      String hasil      = respDoc["hasil"]       | "gagal";
      String oledText   = respDoc["display_oled"] | "OK";
      String nama       = respDoc["nama_karyawan"] | "";

      // Format display_oled dari server: "BUDI · Masuk · 07:14"
      // Tampilkan nama di baris 1, status di baris 2, jam di baris 3
      String l1 = nama.length()   > 0 ? nama.substring(0, 21)     : oledText.substring(0, 21);
      String l2 = "";
      String l3 = "";

      // Coba parse separator " · " atau " - "
      int d = oledText.indexOf(" - ");
      if (d < 0) d = oledText.indexOf("  "); // double space fallback
      if (d >= 0) {
        l1 = oledText.substring(0, d).substring(0, 21);
        String rest = oledText.substring(d + 3);
        int d2 = rest.indexOf(" - ");
        if (d2 >= 0) {
          l2 = rest.substring(0, d2).substring(0, 21);
          l3 = rest.substring(d2 + 3).substring(0, 21);
        } else {
          l2 = rest.substring(0, 21);
        }
      }

      showOLED(l1, l2, l3);
      Serial.println("[ATTEND] hasil=" + hasil);
    } else {
      showOLED("Absen OK!", "", "");
    }

} else if (httpCode < 0) {
showOLED("Coba Lagi!", "Network error", "");
} else {
showOLED("Coba Lagi!", "Server error", String(httpCode));
}

delay(3000);
showOLEDIdle();
}

// =============================================================================
// TASK POLLING — cek tugas dari server setiap 5 detik
// (enroll/sync/delete template — dihandle server-side di fase ini)
// =============================================================================
void tickTaskPoller() {
if (millis() - lastPollMs < POLL_INTERVAL_MS) return;
lastPollMs = millis();
if (!wifiConnected) return;

HTTPClient http;
http.begin(String(BACKEND_BASE) + "/tasks");
http.addHeader("X-Device-Token", DEVICE_TOKEN);
http.setTimeout(10000);

int httpCode = http.GET();
String resp = (httpCode > 0) ? http.getString() : "";
http.end();

if (httpCode != 200 || resp.isEmpty()) return;

DynamicJsonDocument doc(4096);
if (deserializeJson(doc, resp)) return;

// Tampilkan warning slot jika hampir penuh
bool slotWarning = doc["slot_warning"] | false;
if (slotWarning) {
int used = doc["slot_used"] | 0;
int total = doc["slot_total"] | 127;
showOLED("Peringatan!", "Slot hampir penuh",
String(used) + "/" + String(total));
delay(3000);
showOLEDIdle();
}

JsonArray tasks = doc["tasks"].as<JsonArray>();
if (tasks.size() == 0) return;

Serial.printf("[POLL] %d task masuk\n", tasks.size());

for (JsonObject task : tasks) {
int id = task["id"] | 0;
String action = task["action"] | "";
int fpId = task["fingerprint_id"]| 0;

    Serial.printf("[TASK] id=%d action=%s fpId=%d\n",
                  id, action.c_str(), fpId);

    // --- DELETE TEMPLATE ---
    if (action == "delete_template") {
      showOLED("Hapus Template", "Slot #" + String(fpId), "");
      finger.deleteModel((uint16_t)fpId);
      // Kirim result done
      StaticJsonDocument<128> res;
      res["status"]        = "done";
      res["error_message"] = nullptr;
      res["template"]      = nullptr;
      String resBody;
      serializeJson(res, resBody);

      HTTPClient hRes;
      hRes.begin(String(BACKEND_BASE) + "/tasks/" + String(id) + "/result");
      hRes.addHeader("Content-Type", "application/json");
      hRes.addHeader("X-Device-Token", DEVICE_TOKEN);
      hRes.setTimeout(10000);
      hRes.POST(resBody);
      hRes.end();

      showOLED("Hapus OK!", "Slot #" + String(fpId), "");
      delay(1000);
    }

    // --- ENROLL CAPTURE & SYNC TEMPLATE ---
    // Memerlukan ArduinoJson & logic tambahan — lihat kode lengkap di file main.cpp
    // Untuk fase awal ini, task enroll/sync diterima tapi dilewati
    // karena memerlukan interaksi fisik (tempel jari) yang perlu UI lebih lengkap

}

showOLEDIdle();
}

// =============================================================================
// HELPERS
// =============================================================================
String generateScanUid() {
uint32_t r1 = esp_random();
uint32_t r2 = esp_random();
char uid[17];
snprintf(uid, sizeof(uid), "%08X%08X", r1, r2);
return String(uid);
}

String getTimestamp() {
struct tm t;
if (!getLocalTime(&t)) return "1970-01-01 00:00:00";
char buf[32];
strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &t);
return String(buf);
}

// =============================================================================
// OLED DISPLAY
// PENTING: selalu clearDisplay() + tulis + display() dalam satu fungsi
// Tidak ada delay() di sini — delay ada di caller
// =============================================================================
void showOLED(const String& l1, const String& l2, const String& l3) {
if (!oledOk) {
Serial.println("[OLED] " + l1 + " | " + l2 + " | " + l3);
return;
}

display.clearDisplay(); // 1. bersihkan buffer
display.setTextSize(1);
display.setTextColor(SSD1306_WHITE);

if (l1.length() > 0) {
display.setCursor(0, 0);
display.println(l1.substring(0, 21));
}
if (l2.length() > 0) {
display.setCursor(0, 22);
display.println(l2.substring(0, 21));
}
if (l3.length() > 0) {
display.setCursor(0, 44);
display.println(l3.substring(0, 21));
}

display.display(); // 2. push ke layar
}

void showOLEDIdle() {
if (!oledOk) return;

char jam[9] = "--:--";
struct tm t;
if (getLocalTime(&t)) strftime(jam, sizeof(jam), "%H:%M:%S", &t);

display.clearDisplay();
display.setTextSize(1);
display.setTextColor(SSD1306_WHITE);

display.drawFastHLine(0, 11, 128, SSD1306_WHITE);

display.setTextSize(2);
display.setCursor(5, 18);
display.println("Absensi");

display.setTextSize(1);
display.drawFastHLine(0, 50, 128, SSD1306_WHITE);
display.setCursor(34, 54);
display.println(jam);

display.display();
}
