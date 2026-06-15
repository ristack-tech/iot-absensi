// =============================================================================
// SISTEM ABSENSI IoT — ESP32 + AS608 + OLED SSD1306 + Buzzer
// Board : ESP32 (NodeMCU-32S atau sejenisnya)
// OLED : GMO09605 / SSD1306 128x64, I2C 0x3C
// Sensor : AS608, UART2 RX=16 TX=17, 57600 baud
// =============================================================================

#include <Arduino.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Fingerprint.h>
#include <ArduinoJson.h>
#include <time.h>

// =============================================================================
// KONFIGURASI — sesuaikan sebelum flash
// =============================================================================
#define WIFI_SSID "Bengkod WD"
#define WIFI_PASS "sayangucup"
#define BACKEND_BASE "http://192.168.1.142:8000/api/device"
#define DEVICE_TOKEN "alat-gudang1-secret"

// Timing (ms)
#define POLL_INTERVAL_MS 5000
#define SCAN_COOLDOWN_MS 3000
#define WIFI_RETRY_MS 30000
#define ENROLL_TIMEOUT_MS 30000

// OLED
#define OLED_WIDTH 128
#define OLED_HEIGHT 64
#define OLED_ADDR 0x3C
#define OLED_RESET -1

// =============================================================================
// OBJEK GLOBAL
// =============================================================================
Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
HardwareSerial fingerSerial(2); // UART2
Adafruit_Fingerprint finger(&fingerSerial);

// State
unsigned long lastScanMs = 0;
unsigned long lastPollMs = 0;
unsigned long lastWifiRetryMs = 0;
bool oledOk = false;

// =============================================================================
// FORWARD DECLARATIONS
// =============================================================================
void checkWifiReconnect();
void tickFingerprintScan();
void sendAttendance(uint16_t fingerprintId);
void tickTaskPoller();
void handleEnrollTask(int taskId, int fingerprintId, const String &namaKaryawan);
void handleSyncTask(int taskId, int fingerprintId, const String &payloadB64);
void handleDeleteTask(int taskId, int fingerprintId);
void postTaskResult(int taskId, const String &status,
const String &errorMsg, const String &tmplB64);
String downloadTemplate(uint16_t slot);
bool uploadTemplate(uint16_t slot, uint8_t *buf, int len);
String base64Encode(const uint8_t *data, size_t len);
bool base64Decode(const String &str, uint8_t *outBuf, int *outLen);
String httpPost(const String &endpoint, const String &body, int *httpCode);
String httpGet(const String &endpoint, int *httpCode);
String generateScanUid();
void showOled(const String &l1, const String &l2 = "", const String &l3 = "");
void showOledIdle();
void showOledEnroll(const String &nama, int step);

// =============================================================================
// SETUP
// =============================================================================
void setup()
{
Serial.begin(9600);
Serial.println(F("[BOOT] Sistem Absensi IoT"));

    // --- OLED ---
    Wire.begin();
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
    {
        Serial.println(F("[ERROR] OLED gagal init! Cek wiring I2C."));
        // Coba sekali lagi setelah delay
        delay(200);
        if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR))
        {
            Serial.println(F("[ERROR] OLED tetap gagal — lanjut tanpa display"));
            oledOk = false;
        }
        else
        {
            oledOk = true;
        }
    }
    else
    {
        oledOk = true;
    }

    if (oledOk)
    {
        display.clearDisplay();
        display.setTextColor(SSD1306_WHITE);
        display.setTextSize(1);
        display.setCursor(0, 22);
        display.println(F("  Sistem Absensi IoT"));
        display.setCursor(0, 36);
        display.println(F("     Booting..."));
        display.display();
        Serial.println(F("[OK] OLED init berhasil"));
    }

    // --- AS608 Fingerprint ---
    fingerSerial.begin(57600, SERIAL_8N1, 16, 17); // RX=16, TX=17
    delay(100);
    finger.begin(57600);

    if (finger.verifyPassword())
    {
        Serial.println(F("[OK] AS608 terdeteksi"));
        // Set packet size 32 bytes — WAJIB untuk template transfer
        finger.setPacketSize(FINGERPRINT_PACKET_SIZE_32);
    }
    else
    {
        Serial.println(F("[ERROR] AS608 tidak terdeteksi! Cek wiring UART."));
        showOled("ERROR!", "Sensor jari", "tidak terdeteksi");
        while (true)
            delay(1000); // halt
    }

    // --- WiFi ---
    showOled("Connecting WiFi", WIFI_SSID, "");
    WiFi.mode(WIFI_STA);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WiFi] Menghubungkan ke %s", WIFI_SSID);

    int attempt = 0;
    while (!WiFi.isConnected() && attempt < 20)
    {
        delay(500);
        Serial.print(".");
        attempt++;
    }
    Serial.println();

    if (WiFi.isConnected())
    {
        Serial.printf("[OK] WiFi terhubung: %s\n", WiFi.localIP().toString().c_str());

        // --- NTP Sync ---
        configTime(7 * 3600, 0, "pool.ntp.org", "time.nist.gov");
        Serial.print(F("[NTP] Sinkronisasi waktu"));
        struct tm t;
        int ntpTry = 0;
        while (!getLocalTime(&t) && ntpTry < 20)
        {
            delay(500);
            Serial.print(".");
            ntpTry++;
        }
        Serial.println();

        if (getLocalTime(&t))
        {
            char buf[32];
            strftime(buf, sizeof(buf), "%d/%m/%Y %H:%M:%S", &t);
            Serial.printf("[OK] Waktu: %s\n", buf);
        }
        else
        {
            Serial.println(F("[WARN] NTP gagal — timestamp mungkin tidak akurat"));
        }
    }
    else
    {
        Serial.println(F("[WARN] WiFi gagal — mode offline"));
        showOled("WiFi Gagal!", "Mode offline", "Silakan absen");
        delay(2000);
    }

    showOledIdle();
    Serial.println(F("[BOOT] Siap."));

}

// =============================================================================
// LOOP
// =============================================================================
void loop()
{
checkWifiReconnect();
tickFingerprintScan();
tickTaskPoller();
}

// =============================================================================
// WiFi RECONNECT
// =============================================================================
void checkWifiReconnect()
{
if (WiFi.isConnected())
return;
if (millis() - lastWifiRetryMs < WIFI_RETRY_MS)
return;

    lastWifiRetryMs = millis();
    Serial.println(F("[WiFi] Koneksi putus, mencoba reconnect..."));
    showOled("WiFi Putus!", "Reconnecting...", "");

    WiFi.reconnect();
    for (int i = 0; i < 10 && !WiFi.isConnected(); i++)
        delay(500);

    if (WiFi.isConnected())
    {
        Serial.println(F("[OK] WiFi terhubung kembali"));
        showOledIdle();
    }
    else
    {
        Serial.println(F("[WARN] Reconnect gagal, coba lagi nanti"));
        showOledIdle();
    }

}

// =============================================================================
// SCAN SIDIK JARI (non-blocking)
// =============================================================================
void tickFingerprintScan()
{
if (millis() - lastScanMs < SCAN_COOLDOWN_MS)
return;

    uint8_t p = finger.getImage();
    if (p == FINGERPRINT_NOFINGER)
        return; // tidak ada jari, normal
    if (p != FINGERPRINT_OK)
        return; // noise/error, abaikan

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK)
        return;

    p = finger.fingerSearch();

    if (p == FINGERPRINT_OK)
    {
        Serial.printf("[SCAN] Jari terdeteksi: slot=%d confidence=%d\n",
                      finger.fingerID, finger.confidence);
        showOled("Memproses...", "", "");
        sendAttendance(finger.fingerID);
        lastScanMs = millis();
    }
    else if (p == FINGERPRINT_NOTFOUND)
    {
        Serial.println(F("[SCAN] Jari tidak dikenal"));
        showOled("Tidak Dikenal!", "Jari tidak", "terdaftar");
        lastScanMs = millis();
    }

}

// =============================================================================
// KIRIM ABSENSI KE SERVER
// =============================================================================
void sendAttendance(uint16_t fingerprintId)
{
if (!WiFi.isConnected())
{
showOled("Offline!", "Tidak bisa", "kirim data");
return;
}

    // Buat scan_uid (idempotency key)
    String scanUid = generateScanUid();

    // Ambil timestamp
    struct tm t;
    char waktuScan[32] = "1970-01-01 00:00:00";
    if (getLocalTime(&t))
    {
        strftime(waktuScan, sizeof(waktuScan), "%Y-%m-%d %H:%M:%S", &t);
    }

    // Build JSON body
    StaticJsonDocument<256> doc;
    doc["fingerprint_id"] = fingerprintId;
    doc["scan_uid"] = scanUid;
    doc["waktu_scan"] = waktuScan;

    String body;
    serializeJson(doc, body);

    Serial.printf("[ATTEND] POST fingerprint_id=%d scan_uid=%s\n",
                  fingerprintId, scanUid.c_str());

    int httpCode = 0;
    String resp = httpPost("/attendance", body, &httpCode);

    if (httpCode < 0)
    {
        // Network error — jangan update lastScanMs agar retry bisa pakai scan_uid sama
        // Tapi kita sudah update lastScanMs di caller, jadi user perlu scan ulang
        Serial.println(F("[ATTEND] Network error"));
        showOled("Coba Lagi", "Jaringan error", "");
        return;
    }

    // Parse response
    StaticJsonDocument<512> respDoc;
    DeserializationError err = deserializeJson(respDoc, resp);

    if (err || httpCode != 200)
    {
        Serial.printf("[ATTEND] Error HTTP=%d\n", httpCode);
        showOled("Coba Lagi", "Server error", String(httpCode));
        return;
    }

    String hasil = respDoc["hasil"].as<String>();
    String displayOled = respDoc["display_oled"].as<String>();

    Serial.printf("[ATTEND] hasil=%s display=%s\n",
                  hasil.c_str(), displayOled.c_str());

    // Tampil OLED — split display_oled berdasarkan " · "
    // Format dari server: "BUDI · Masuk · 07:14"
    String l1 = "", l2 = "", l3 = "";
    int d1 = displayOled.indexOf(" \xc2\xb7 "); // UTF-8 middle dot
    if (d1 < 0)
        d1 = displayOled.indexOf(" - "); // fallback separator
    if (d1 >= 0)
    {
        l1 = displayOled.substring(0, d1);
        String rest = displayOled.substring(d1 + 3);
        int d2 = rest.indexOf(" \xc2\xb7 ");
        if (d2 < 0)
            d2 = rest.indexOf(" - ");
        if (d2 >= 0)
        {
            l2 = rest.substring(0, d2);
            l3 = rest.substring(d2 + 3);
        }
        else
        {
            l2 = rest;
        }
    }
    else
    {
        l1 = displayOled.substring(0, 21);
    }

    showOled(l1, l2, l3);

    // Buzzer sesuai hasil
    if (hasil == "masuk" || hasil == "keluar")
    {
    }
    else
    {
    }

}

// =============================================================================
// TASK POLLING
// =============================================================================
void tickTaskPoller()
{
if (millis() - lastPollMs < POLL_INTERVAL_MS)
return;
lastPollMs = millis();

    if (!WiFi.isConnected())
        return;

    int httpCode = 0;
    String resp = httpGet("/tasks", &httpCode);

    if (httpCode != 200 || resp.isEmpty())
    {
        Serial.printf("[POLL] Gagal HTTP=%d\n", httpCode);
        return;
    }

    DynamicJsonDocument doc(4096);
    DeserializationError err = deserializeJson(doc, resp);
    if (err)
    {
        Serial.println(F("[POLL] JSON parse error"));
        return;
    }

    // Cek slot warning
    bool slotWarning = doc["slot_warning"] | false;
    int slotUsed = doc["slot_used"] | 0;
    int slotTotal = doc["slot_total"] | 127;

    if (slotWarning)
    {
        Serial.printf("[POLL] PERINGATAN slot hampir penuh: %d/%d\n", slotUsed, slotTotal);
        showOled("Peringatan!", "Slot " + String(slotUsed) + "/" + String(slotTotal), "Hampir penuh!");
        delay(3000);
    }

    JsonArray tasks = doc["tasks"].as<JsonArray>();
    if (tasks.size() == 0)
        return;

    Serial.printf("[POLL] %d task diterima\n", tasks.size());

    for (JsonObject task : tasks)
    {
        int taskId = task["id"] | 0;
        String action = task["action"] | "";
        int fpId = task["fingerprint_id"] | 0;
        String payload = task["payload"] | "";
        String namaKaryw = "";

        if (task.containsKey("karyawan") && task["karyawan"].is<JsonObject>())
        {
            namaKaryw = task["karyawan"]["nama_lengkap"] | "";
        }

        Serial.printf("[TASK] id=%d action=%s fpId=%d\n", taskId, action.c_str(), fpId);

        if (action == "enroll_capture")
        {
            handleEnrollTask(taskId, fpId, namaKaryw);
        }
        else if (action == "sync_template")
        {
            handleSyncTask(taskId, fpId, payload);
        }
        else if (action == "delete_template")
        {
            handleDeleteTask(taskId, fpId);
        }
        else
        {
            Serial.printf("[TASK] action tidak dikenal: %s\n", action.c_str());
        }
    }

    showOledIdle();

}

// =============================================================================
// HANDLE TASK: ENROLL CAPTURE (master device)
// =============================================================================
void handleEnrollTask(int taskId, int fingerprintId, const String &namaKaryawan)
{
Serial.printf("[ENROLL] Mulai enroll slot=%d nama=%s\n",
fingerprintId, namaKaryawan.c_str());

    uint8_t p;

    // --- STEP 1: Capture jari pertama ---
    showOledEnroll(namaKaryawan, 1);
    Serial.println(F("[ENROLL] Menunggu jari pertama..."));

    unsigned long t0 = millis();
    while (true)
    {
        if (millis() - t0 > ENROLL_TIMEOUT_MS)
        {
            Serial.println(F("[ENROLL] Timeout step 1"));
            postTaskResult(taskId, "failed", "Timeout: tidak ada jari (step1)", "");
            showOled("Enroll Gagal!", "Timeout", "");
            return;
        }
        p = finger.getImage();
        if (p == FINGERPRINT_OK)
            break;
        if (p != FINGERPRINT_NOFINGER)
        {
            delay(50);
        }
        delay(100);
    }

    p = finger.image2Tz(1);
    if (p != FINGERPRINT_OK)
    {
        postTaskResult(taskId, "failed", "image2Tz(1) gagal", "");
        showOled("Enroll Gagal!", "Kualitas jari", "buruk");
        return;
    }

    // --- STEP 2: Angkat jari ---
    showOled("ENROLL", "Angkat Jari...", "");
    Serial.println(F("[ENROLL] Tunggu jari diangkat..."));
    unsigned long t1 = millis();
    while (millis() - t1 < 5000)
    {
        if (finger.getImage() == FINGERPRINT_NOFINGER)
            break;
        delay(100);
    }

    // --- STEP 3: Capture jari kedua ---
    showOledEnroll(namaKaryawan, 2);
    Serial.println(F("[ENROLL] Menunggu jari kedua..."));

    unsigned long t2 = millis();
    while (true)
    {
        if (millis() - t2 > ENROLL_TIMEOUT_MS)
        {
            Serial.println(F("[ENROLL] Timeout step 3"));
            postTaskResult(taskId, "failed", "Timeout: tidak ada jari (step3)", "");
            showOled("Enroll Gagal!", "Timeout", "");
            return;
        }
        p = finger.getImage();
        if (p == FINGERPRINT_OK)
            break;
        delay(100);
    }

    p = finger.image2Tz(2);
    if (p != FINGERPRINT_OK)
    {
        postTaskResult(taskId, "failed", "image2Tz(2) gagal", "");
        showOled("Enroll Gagal!", "Kualitas jari", "buruk");
        return;
    }

    // --- STEP 4: Buat model & simpan ke flash sensor ---
    p = finger.createModel();
    if (p != FINGERPRINT_OK)
    {
        postTaskResult(taskId, "failed", "createModel gagal: jari tidak cocok", "");
        showOled("Enroll Gagal!", "Jari tidak", "cocok, ulangi");
        return;
    }

    p = finger.storeModel(fingerprintId);
    if (p != FINGERPRINT_OK)
    {
        postTaskResult(taskId, "failed", "storeModel gagal: slot penuh?", "");
        showOled("Enroll Gagal!", "Gagal simpan", "ke sensor");
        return;
    }

    showOled("Enroll OK!", namaKaryawan.substring(0, 21), "Mengupload...");
    Serial.println(F("[ENROLL] Model tersimpan, download template..."));

    // --- STEP 5: Download template dari sensor ---
    String tmplB64 = downloadTemplate((uint16_t)fingerprintId);
    if (tmplB64.isEmpty())
    {
        postTaskResult(taskId, "failed", "Download template dari sensor gagal", "");
        showOled("Enroll Gagal!", "Download tmpl", "gagal");
        return;
    }

    // --- STEP 6: Kirim hasil ke server ---
    postTaskResult(taskId, "done", "", tmplB64);

    showOled("Enroll Selesai!", namaKaryawan.substring(0, 21),
             "Slot #" + String(fingerprintId));
    Serial.printf("[ENROLL] Selesai slot=%d\n", fingerprintId);

}

// =============================================================================
// HANDLE TASK: SYNC TEMPLATE (semua device)
// =============================================================================
void handleSyncTask(int taskId, int fingerprintId, const String &payloadB64)
{
Serial.printf("[SYNC] slot=%d payload_len=%d\n",
fingerprintId, payloadB64.length());

    showOled("Sync Template", "Slot #" + String(fingerprintId), "Memproses...");

    uint8_t templateBuf[512];
    int templateLen = 0;

    if (!base64Decode(payloadB64, templateBuf, &templateLen))
    {
        Serial.println(F("[SYNC] base64Decode gagal"));
        postTaskResult(taskId, "failed", "Decode base64 gagal", "");
        return;
    }

    if (templateLen != 512)
    {
        Serial.printf("[SYNC] Panjang template salah: %d (harus 512)\n", templateLen);
        postTaskResult(taskId, "failed",
                       "Panjang template salah: " + String(templateLen), "");
        return;
    }

    if (!uploadTemplate((uint16_t)fingerprintId, templateBuf, templateLen))
    {
        Serial.println(F("[SYNC] Upload ke sensor gagal"));
        postTaskResult(taskId, "failed", "Upload template ke sensor gagal", "");
        showOled("Sync Gagal!", "Slot #" + String(fingerprintId), "");
        return;
    }

    postTaskResult(taskId, "done", "", "");
    showOled("Sync OK!", "Slot #" + String(fingerprintId), "");
    Serial.printf("[SYNC] Berhasil slot=%d\n", fingerprintId);

}

// =============================================================================
// HANDLE TASK: DELETE TEMPLATE (semua device)
// =============================================================================
void handleDeleteTask(int taskId, int fingerprintId)
{
Serial.printf("[DELETE] slot=%d\n", fingerprintId);

    finger.deleteModel((uint16_t)fingerprintId);
    // Apapun hasilnya → done (idempotent: slot kosong = tujuan tercapai)

    postTaskResult(taskId, "done", "", "");
    showOled("Hapus OK", "Slot #" + String(fingerprintId), "");
    Serial.printf("[DELETE] Selesai slot=%d\n", fingerprintId);

}

// =============================================================================
// POST TASK RESULT KE SERVER
// =============================================================================
void postTaskResult(int taskId, const String &status,
const String &errorMsg, const String &tmplB64)
{
StaticJsonDocument<1024> doc;
doc["status"] = status;

    if (errorMsg.length() > 0)
    {
        doc["error_message"] = errorMsg;
    }
    else
    {
        doc["error_message"] = nullptr;
    }

    if (tmplB64.length() > 0)
    {
        doc["template"] = tmplB64;
    }
    else
    {
        doc["template"] = nullptr;
    }

    String body;
    serializeJson(doc, body);

    int httpCode = 0;
    String endpoint = "/tasks/" + String(taskId) + "/result";
    httpPost(endpoint, body, &httpCode);

    Serial.printf("[TASK_RESULT] id=%d status=%s HTTP=%d\n",
                  taskId, status.c_str(), httpCode);

}

// =============================================================================
// DOWNLOAD TEMPLATE DARI SENSOR (slot → base64 string)
// AS608 UpChar: loadModel → getModel → baca data packets
// =============================================================================
String downloadTemplate(uint16_t slot)
{
if (finger.loadModel(slot) != FINGERPRINT_OK)
{
Serial.println(F("[DL_TMPL] loadModel gagal"));
return "";
}
if (finger.getModel() != FINGERPRINT_OK)
{
Serial.println(F("[DL_TMPL] getModel gagal"));
return "";
}

    uint8_t templateBuf[512];
    int total = 0;
    uint8_t dummyData[64] = {0};

    Adafruit_Fingerprint_Packet pkt(FINGERPRINT_DATAPACKET, 0, dummyData);

    for (int i = 0; i < 20 && total < 512; i++)
    {
        if (finger.getStructuredPacket(&pkt, 3000) != FINGERPRINT_OK)
        {
            Serial.printf("[DL_TMPL] getStructuredPacket gagal di paket %d\n", i);
            break;
        }

        // pkt.length = wire_length = data_bytes + 2 (checksum)
        int dataBytes = pkt.length - 2;
        if (dataBytes <= 0 || total + dataBytes > 512)
            break;

        memcpy(templateBuf + total, pkt.data, dataBytes);
        total += dataBytes;

        if (pkt.type == FINGERPRINT_ENDDATAPACKET)
            break;
    }

    if (total != 512)
    {
        Serial.printf("[DL_TMPL] Total bytes tidak 512: %d\n", total);
        return "";
    }

    Serial.println(F("[DL_TMPL] Berhasil 512 bytes"));
    return base64Encode(templateBuf, 512);

}

// =============================================================================
// UPLOAD TEMPLATE KE SENSOR (base64 → slot)
// AS608 DownChar: kirim command 0x09, baca ACK, kirim data packets, simpan
// =============================================================================
bool uploadTemplate(uint16_t slot, uint8_t \*buf, int len)
{
// 1. Kirim command DownChar [0x09, bufId=0x01]
uint8_t cmdData[2] = {0x09, 0x01};
Adafruit_Fingerprint_Packet cmd(FINGERPRINT_COMMANDPACKET, 2, cmdData);
finger.writeStructuredPacket(cmd);

    // 2. Baca ACK
    uint8_t ackBuf[64] = {0};
    Adafruit_Fingerprint_Packet ack(FINGERPRINT_ACKPACKET, 0, ackBuf);
    if (finger.getStructuredPacket(&ack, 2000) != FINGERPRINT_OK)
    {
        Serial.println(F("[UL_TMPL] ACK command gagal"));
        return false;
    }
    if (ack.data[0] != FINGERPRINT_OK)
    {
        Serial.printf("[UL_TMPL] ACK error code: 0x%02X\n", ack.data[0]);
        return false;
    }

    // 3. Kirim data 32 bytes per paket
    int offset = 0;
    while (offset < len)
    {
        int chunkSize = min(32, len - offset);
        bool isLast = (offset + chunkSize >= len);
        uint8_t pktType = isLast ? FINGERPRINT_ENDDATAPACKET : FINGERPRINT_DATAPACKET;

        Adafruit_Fingerprint_Packet dataPkt(pktType, chunkSize, buf + offset);
        finger.writeStructuredPacket(dataPkt);
        delay(5); // beri sensor waktu memproses
        offset += chunkSize;
    }

    // 4. Baca ACK akhir
    uint8_t finalBuf[64] = {0};
    Adafruit_Fingerprint_Packet finalAck(FINGERPRINT_ACKPACKET, 0, finalBuf);
    if (finger.getStructuredPacket(&finalAck, 5000) != FINGERPRINT_OK)
    {
        Serial.println(F("[UL_TMPL] ACK akhir gagal"));
        return false;
    }
    if (finalAck.data[0] != FINGERPRINT_OK)
    {
        Serial.printf("[UL_TMPL] ACK akhir error: 0x%02X\n", finalAck.data[0]);
        return false;
    }

    // 5. Simpan ke flash pada slot yang ditentukan
    bool ok = (finger.storeModel(slot) == FINGERPRINT_OK);
    if (!ok)
        Serial.println(F("[UL_TMPL] storeModel gagal"));
    return ok;

}

// =============================================================================
// BASE64 ENCODE
// =============================================================================
static const char B64_CHARS[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

String base64Encode(const uint8_t _data, size_t len)
{
String out;
out.reserve(((len + 2) / 3) _ 4 + 4);

    for (size_t i = 0; i < len; i += 3)
    {
        uint8_t b0 = data[i];
        uint8_t b1 = (i + 1 < len) ? data[i + 1] : 0;
        uint8_t b2 = (i + 2 < len) ? data[i + 2] : 0;

        out += B64_CHARS[(b0 >> 2) & 0x3F];
        out += B64_CHARS[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)];
        out += (i + 1 < len) ? B64_CHARS[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=';
        out += (i + 2 < len) ? B64_CHARS[b2 & 0x3F] : '=';
    }
    return out;

}

// =============================================================================
// BASE64 DECODE
// =============================================================================
static int b64CharIndex(char c)
{
if (c >= 'A' && c <= 'Z')
return c - 'A';
if (c >= 'a' && c <= 'z')
return c - 'a' + 26;
if (c >= '0' && c <= '9')
return c - '0' + 52;
if (c == '+')
return 62;
if (c == '/')
return 63;
if (c == '=')
return 0; // padding
return -1; // karakter tidak valid
}

bool base64Decode(const String &str, uint8_t *outBuf, int *outLen)
{
\*outLen = 0;
int strLen = str.length();
if (strLen % 4 != 0)
return false;

    for (int i = 0; i < strLen; i += 4)
    {
        int idx[4];
        for (int j = 0; j < 4; j++)
        {
            idx[j] = b64CharIndex(str[i + j]);
            if (idx[j] < 0)
                return false;
        }

        outBuf[(*outLen)++] = (uint8_t)((idx[0] << 2) | (idx[1] >> 4));

        if (str[i + 2] != '=')
        {
            outBuf[(*outLen)++] = (uint8_t)((idx[1] << 4) | (idx[2] >> 2));
        }
        if (str[i + 3] != '=')
        {
            outBuf[(*outLen)++] = (uint8_t)((idx[2] << 6) | idx[3]);
        }
    }
    return true;

}

// =============================================================================
// HTTP HELPERS
// =============================================================================
String httpPost(const String &endpoint, const String &body, int *httpCode)
{
if (!WiFi.isConnected())
{
*httpCode = -1;
return "";
}

    HTTPClient http;
    String url = String(BACKEND_BASE) + endpoint;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-Device-Token", DEVICE_TOKEN);
    http.setTimeout(10000);

    *httpCode = http.POST(body);

    String resp = "";
    if (*httpCode > 0)
    {
        resp = http.getString();
    }
    http.end();

    Serial.printf("[HTTP] POST %s → %d\n", endpoint.c_str(), *httpCode);
    return resp;

}

String httpGet(const String &endpoint, int *httpCode)
{
if (!WiFi.isConnected())
{
*httpCode = -1;
return "";
}

    HTTPClient http;
    String url = String(BACKEND_BASE) + endpoint;
    http.begin(url);
    http.addHeader("X-Device-Token", DEVICE_TOKEN);
    http.setTimeout(10000);

    *httpCode = http.GET();

    String resp = "";
    if (*httpCode > 0)
    {
        resp = http.getString();
    }
    http.end();

    return resp;

}

// =============================================================================
// SCAN UID (idempotency key)
// =============================================================================
String generateScanUid()
{
uint32_t r1 = esp_random();
uint32_t r2 = esp_random();
char uid[17];
snprintf(uid, sizeof(uid), "%08X%08X", r1, r2);
return String(uid); // 16 char hex, contoh: "A3F2B1C8D9E0F123"
}

// =============================================================================
// OLED HELPERS
// Font size 1 = 6×8 px/char → 21 char × 8 baris
// Layout 3 baris: y=0, y=22, y=44
// =============================================================================
void showOled(const String &l1, const String &l2, const String &l3)
{
if (!oledOk)
return;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // Baris 1 — bold style: tulis 2x offset 1px untuk efek tebal
    if (l1.length() > 0)
    {
        String t = l1.substring(0, 21);
        display.setCursor(0, 0);
        display.println(t);
        display.setCursor(1, 0); // sedikit geser untuk efek pseudo-bold
        display.println(t);
    }

    // Baris 2
    if (l2.length() > 0)
    {
        display.setCursor(0, 22);
        display.println(l2.substring(0, 21));
    }

    // Baris 3
    if (l3.length() > 0)
    {
        display.setCursor(0, 44);
        display.println(l3.substring(0, 21));
    }

    display.display();

}

void showOledIdle()
{
if (!oledOk)
return;

    struct tm t;
    char jam[9] = "--:--:--";
    if (getLocalTime(&t))
    {
        strftime(jam, sizeof(jam), "%H:%M:%S", &t);
    }

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    // Garis atas
    display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

    // Teks utama
    display.setCursor(22, 18);
    display.setTextSize(1);
    display.println(F("  Tempel Jari  "));

    // Garis bawah
    display.drawFastHLine(0, 52, 128, SSD1306_WHITE);

    // Jam NTP
    display.setCursor(34, 55);
    display.setTextSize(1);
    display.println(jam);

    display.display();

}

void showOledEnroll(const String &nama, int step)
{
if (!oledOk)
return;

    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);

    display.setCursor(0, 0);
    display.printf("ENROLL [%d/2]", step);

    display.drawFastHLine(0, 10, 128, SSD1306_WHITE);

    display.setCursor(0, 18);
    display.println(nama.substring(0, 21));

    display.setCursor(0, 38);
    if (step == 1)
    {
        display.println(F("Tempel Jari 1/2"));
    }
    else
    {
        display.println(F("Tempel Jari 2/2"));
    }

    display.display();

}
