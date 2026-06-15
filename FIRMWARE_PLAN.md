# Rencana Implementasi Firmware ESP32

Sistem absensi IoT berbasis ESP32 + AS608 fingerprint sensor + OLED SSD1306.  
Terhubung ke backend Laravel 10 via REST API.

---

## Kondisi Kode Sekarang (`src/main.cpp`)

| Komponen | Status | Catatan |
|---|---|---|
| WiFi connect | Ada, perlu update | SSID/pass masih lama |
| OLED init | Ada, bisa dipakai | — |
| AS608 init | Ada, bisa dipakai | GPIO 16/17, 57600 baud |
| `sendAttendance()` | Perlu ditulis ulang | Endpoint salah, payload salah (tidak ada `scan_uid`, `waktu_scan`) |
| Enroll via serial | Perlu diganti | Harus task-based, bukan command serial |
| Task polling | Belum ada | — |
| Template download/upload | Belum ada | — |
| NTP / timestamp | Belum ada | — |
| JSON parsing | Belum ada | — |

---

## Perubahan `platformio.ini`

Tambahkan satu library:

```ini
lib_deps =
  adafruit/Adafruit SSD1306@^2.5.7
  adafruit/Adafruit GFX Library@^1.11.9
  adafruit/Adafruit Fingerprint Sensor Library@^2.1
  bblanchon/ArduinoJson@^6.21.0        ; TAMBAH INI
```

ArduinoJson untuk parse response server dan build JSON body.  
Base64 encode/decode diimplementasi sendiri (~30 baris) tanpa tambahan dependency.

---

## Konfigurasi Konstanta (Top of `main.cpp`)

```cpp
// WiFi
#define WIFI_SSID      "Bengkod WD"
#define WIFI_PASS      "sayangucup"

// Server — ganti IP sesuai mesin server di jaringan lokal yang sama
#define BACKEND_BASE   "http://192.168.1.100:8000/api/device"

// Token plaintext per alat
// Cara dapat: saat buat alat di halaman web admin, simpan plaintext token
// sebelum di-hash. Isi nilai ini per device sebelum flash.
#define DEVICE_TOKEN   "token_plaintext_alat_ini"

// Timing
#define POLL_INTERVAL_MS   5000    // polling task setiap 5 detik
#define SCAN_COOLDOWN_MS   3000    // jeda antar scan
#define WIFI_RETRY_MS      30000   // retry WiFi jika putus
#define ENROLL_TIMEOUT_MS  30000   // timeout tunggu jari saat enroll
```

---

## Arsitektur Loop Utama

```
setup():
  1. Serial (9600 baud)
  2. Wire + OLED SSD1306 @ 0x3C → tampil "Booting..."
  3. AS608 UART2 (GPIO16=RX, GPIO17=TX, 57600 baud) → verifyPassword()
  4. setPacketSize(FINGERPRINT_PACKET_SIZE_32)  ← penting untuk template transfer
  5. WiFi connect (max 20 attempts × 500ms = 10 detik)
  6. NTP sync → configTime(7*3600, 0, "pool.ntp.org")  [WIB = UTC+7]
  7. OLED: "Tempel Jari"

loop():
  checkWifiReconnect()        // passive: retry jika putus
  tickFingerprintScan()       // non-blocking scan jari
  tickTaskPoller()            // polling setiap 5 detik
```

`loop()` tidak punya `delay()` global — semua flow pakai `millis()` timer.  
Scan jari dan task polling berjalan bersamaan tanpa saling block.

---

## Flow A: Scan Absensi (Normal)

```
tickFingerprintScan():
  jika millis() - lastScanMs < SCAN_COOLDOWN_MS → return

  p = finger.getImage()
  jika NOFINGER → return               // tidak ada jari, normal
  jika bukan OK → return               // noise/error, abaikan

  finger.image2Tz()                    // konversi image ke feature di char buffer 1
  finger.fingerSearch()                // cari di semua template tersimpan

  jika FINGERPRINT_OK:
    showOled("Memproses...", "", "")
    sendAttendance(finger.fingerID)
    lastScanMs = millis()

  jika FINGERPRINT_NOTFOUND:
    showOled("Tidak Dikenal!", "Jari tidak", "terdaftar")
    lastScanMs = millis()
```

---

## Flow B: `sendAttendance(fingerprintId)`

```
1. Cek WiFi connected
   → jika tidak: showOled("Offline!", ...), return

2. Buat scan_uid (16-char hex, idempotency key):
   uint32_t r1 = esp_random(), r2 = esp_random()
   scan_uid = sprintf("%08X%08X", r1, r2)
   Contoh: "A3F2B1C8D9E0F123"

3. Ambil timestamp NTP:
   getLocalTime(&timeinfo)
   waktu_scan = strftime("%Y-%m-%d %H:%M:%S")
   Contoh: "2026-06-15 08:32:17"

4. Buat JSON body:
   { "fingerprint_id": 5, "scan_uid": "...", "waktu_scan": "..." }

5. HTTP POST → BACKEND_BASE + "/attendance"
   Header: X-Device-Token: <DEVICE_TOKEN>

6. Parse response:
   httpCode 200: hasil, display_oled, nama_karyawan
   httpCode 422: hasil = "gagal"
   httpCode -1 (timeout): JANGAN update lastScanMs, bisa retry dengan scan_uid SAMA

7. Tampil OLED:
   line1: hasil (Masuk / Keluar / Luar Jadwal / dll)
   line2: nama karyawan (truncate 21 char)
   line3: jam scan

8. Delay 3 detik → kembali ke idle
```

**Kenapa `scan_uid` penting?**  
Jika POST terkirim tapi WiFi putus sebelum ACK diterima, device tidak tahu apakah server menerima. Jika retry dengan UUID baru → server catat absen ganda. Dengan `scan_uid` sama → server deteksi idempotency dan return hasil yang sama tanpa catat ulang (lihat `AttendanceService::processIot()` baris 155).

**Response server yang mungkin:**

| `hasil` | Arti |
|---|---|
| `masuk` | Absen masuk berhasil dicatat |
| `keluar` | Absen keluar berhasil dicatat |
| `luar_jadwal` | Scan di luar jam masuk/keluar |
| `duplikat` | Sudah absen dalam 2 menit terakhir |
| `gagal` | Fingerprint ID tidak terdaftar di gudang |

---

## Flow C: Task Polling

```
tickTaskPoller():
  jika millis() - lastPollMs < POLL_INTERVAL_MS → return
  lastPollMs = millis()
  jika !WiFi.isConnected() → return

  HTTP GET BACKEND_BASE + "/tasks"
  Header: X-Device-Token

  Response JSON:
  {
    "tasks": [...],
    "slot_used": 45,
    "slot_total": 127,
    "slot_warning": false
  }

  jika slot_warning:
    showOled("Peringatan!", "Slot " + slot_used + "/127", "Hampir penuh!")
    delay(3000)

  untuk setiap task:
    switch task["action"]:
      "enroll_capture"  → handleEnrollTask(task)
      "sync_template"   → handleSyncTask(task)
      "delete_template" → handleDeleteTask(task)
```

> Task polling memblokir loop selama ~500ms (HTTP request). Ini acceptable karena hanya terjadi setiap 5 detik.

---

## Flow D: `handleEnrollTask` (master device)

Task yang diterima dari server:
```json
{
  "id": 123,
  "action": "enroll_capture",
  "fingerprint_id": 7,
  "karyawan": { "nama_lengkap": "Ahmad Fauzi", "fingerprint_id": 7 }
}
```

```
handleEnrollTask(taskId, fingerprintId, namaKaryawan):

  [STEP 1 — Capture jari pertama]
  showOledEnroll(namaKaryawan, 1)   // "ENROLL | Ahmad Fauzi | Tempel Jari 1/2"

  tunggu jari dengan ENROLL_TIMEOUT_MS:
    loop: p = finger.getImage()
    jika OK → break
    jika timeout → postTaskResult(taskId, "failed", "Timeout step1", ""), return

  finger.image2Tz(1)                // simpan feature ke char buffer slot 1

  [STEP 2 — Angkat jari]
  showOled("ENROLL", "Angkat Jari...", "")
  tunggu NOFINGER (max 5 detik)

  [STEP 3 — Capture jari kedua]
  showOledEnroll(namaKaryawan, 2)   // "ENROLL | Ahmad Fauzi | Tempel Jari 2/2"

  tunggu jari dengan ENROLL_TIMEOUT_MS:
    jika timeout → postTaskResult(taskId, "failed", "Timeout step2", ""), return

  finger.image2Tz(2)                // simpan ke char buffer slot 2

  [STEP 4 — Buat model & simpan ke flash sensor]
  p = finger.createModel()          // gabung slot 1 + slot 2
  jika bukan OK → postTaskResult("failed", "Kualitas buruk/tidak cocok"), return

  p = finger.storeModel(fingerprintId)
  jika bukan OK → postTaskResult("failed", "Gagal simpan ke slot"), return

  showOled("Enroll OK!", namaKaryawan, "Mengupload...")

  [STEP 5 — Download template dari sensor]
  templateBase64 = downloadTemplate(fingerprintId)   // lihat Flow G
  jika kosong → postTaskResult("failed", "Gagal download template"), return

  [STEP 6 — Kirim hasil ke server]
  postTaskResult(taskId, "done", "", templateBase64)
  // Server: simpan finger_template ke DB karyawan,
  //         buat sync_template task untuk semua alat lain di gudang

  showOled("Enroll Selesai!", namaKaryawan, "Slot #" + fingerprintId)
```

---

## Flow E: `handleSyncTask` (semua device)

Task yang diterima:
```json
{
  "id": 124,
  "action": "sync_template",
  "fingerprint_id": 7,
  "payload": "<base64 template, 684 char>"
}
```

```
handleSyncTask(taskId, fingerprintId, payloadBase64):
  showOled("Sync Template", "Slot #" + fingerprintId, "Memproses...")

  uint8_t templateBuf[512];
  int templateLen = 0;
  bool ok = base64Decode(payloadBase64, templateBuf, &templateLen)

  jika !ok atau templateLen != 512:
    postTaskResult(taskId, "failed", "Decode base64 gagal", ""), return

  bool uploaded = uploadTemplate(fingerprintId, templateBuf, templateLen)  // lihat Flow H

  jika uploaded:
    postTaskResult(taskId, "done", "", "")
    showOled("Sync OK!", "Slot #" + fingerprintId, "")
  lain:
    postTaskResult(taskId, "failed", "Upload ke sensor gagal", "")
```

---

## Flow F: `handleDeleteTask` (semua device)

```
handleDeleteTask(taskId, fingerprintId):
  p = finger.deleteModel(fingerprintId)

  // Apapun hasilnya, kirim "done" (idempotent — slot kosong = sudah terhapus)
  postTaskResult(taskId, "done", "", "")
  showOled("Hapus OK", "Slot #" + fingerprintId, "")
```

> Jika slot sudah kosong, `deleteModel` return error tapi itu tidak masalah — tujuan tercapai.

---

## Flow G: `downloadTemplate(slot)` → String base64

Setelah enrollment berhasil di master device, template 512 bytes perlu diambil dari sensor dan dikirim ke server.

**Protokol AS608 — Upload template (sensor → host):**
- Command `UpChar` (0x08 via `FINGERPRINT_UPLOAD`)
- Sensor mengirim template sebagai serangkaian data packets via UART
- Default packet size = 32 bytes payload per packet (kita set di `setup()`)
- Template 512 bytes → 16 paket (15 `DATAPACKET` + 1 `ENDDATAPACKET`)

**Kenapa packet size 32?**  
Buffer `Adafruit_Fingerprint_Packet.data[64]`. Wire format = payload + 2 bytes checksum.  
Dengan 32 payload: butuh 34 bytes → aman di buffer 64.  
Dengan 64 payload: butuh 66 bytes → overflow buffer sebesar 2 bytes.

```cpp
String downloadTemplate(uint16_t slot) {
  if (finger.loadModel(slot) != FINGERPRINT_OK) return "";
  if (finger.getModel()      != FINGERPRINT_OK) return "";

  uint8_t templateBuf[512];
  int total = 0;

  Adafruit_Fingerprint_Packet pkt(FINGERPRINT_DATAPACKET, 0, nullptr);

  for (int i = 0; i < 20 && total < 512; i++) {
    if (finger.getStructuredPacket(&pkt, 3000) != FINGERPRINT_OK) break;

    int dataBytes = pkt.length - 2;   // wire length - 2 checksum bytes
    memcpy(templateBuf + total, pkt.data, dataBytes);
    total += dataBytes;

    if (pkt.type == FINGERPRINT_ENDDATAPACKET) break;
  }

  if (total != 512) return "";
  return base64Encode(templateBuf, 512);
}
```

**Catatan `pkt.length`:**  
`getStructuredPacket` menyimpan `wire_length = data_len + 2` ke `pkt.length`.  
Jadi data aktual template ada di `pkt.data[0 .. pkt.length - 3]` sebanyak `pkt.length - 2` bytes.

---

## Flow H: `uploadTemplate(slot, buf, len)` → bool

Library Adafruit **tidak punya** fungsi upload template. Kita implementasi manual menggunakan `writeStructuredPacket` dan `getStructuredPacket` yang sudah `public`.

**Protokol AS608 — Download template (host → sensor):**
- Command `DownChar` (0x09) — tidak ada di library
- Kirim command packet, baca ACK, kirim data packets, baca ACK akhir
- Lalu `storeModel(slot)` untuk simpan ke flash

```cpp
bool uploadTemplate(uint16_t slot, uint8_t* buf, int len) {
  // 1. Kirim command DownChar: [0x09, bufId=0x01]
  uint8_t cmdData[] = {0x09, 0x01};
  Adafruit_Fingerprint_Packet cmd(FINGERPRINT_COMMANDPACKET, 2, cmdData);
  finger.writeStructuredPacket(cmd);

  // 2. Baca ACK
  Adafruit_Fingerprint_Packet ack(FINGERPRINT_ACKPACKET, 0, cmdData);
  if (finger.getStructuredPacket(&ack, 2000) != FINGERPRINT_OK) return false;
  if (ack.data[0] != FINGERPRINT_OK) return false;

  // 3. Kirim data 32 bytes per paket
  int offset = 0;
  while (offset < len) {
    int chunkSize = min(32, len - offset);
    bool isLast   = (offset + chunkSize >= len);
    uint8_t pktType = isLast ? FINGERPRINT_ENDDATAPACKET : FINGERPRINT_DATAPACKET;

    Adafruit_Fingerprint_Packet dataPkt(pktType, chunkSize, buf + offset);
    finger.writeStructuredPacket(dataPkt);
    delay(5);   // beri sensor waktu memproses setiap paket
    offset += chunkSize;
  }

  // 4. Baca ACK akhir (sensor konfirmasi penerimaan semua data)
  Adafruit_Fingerprint_Packet finalAck(FINGERPRINT_ACKPACKET, 0, cmdData);
  if (finger.getStructuredPacket(&finalAck, 5000) != FINGERPRINT_OK) return false;
  if (finalAck.data[0] != FINGERPRINT_OK) return false;

  // 5. Simpan dari char buffer ke flash di slot yang ditentukan
  return (finger.storeModel(slot) == FINGERPRINT_OK);
}
```

---

## Base64 Encode/Decode (Custom)

Template 512 bytes → 684 char base64. Diimplementasi sendiri tanpa library tambahan.

```
base64Encode(data, len) → String
  proses 3 bytes → 4 chars
  padding '=' jika len % 3 != 0

base64Decode(str, outBuf, &outLen) → bool
  proses 4 chars → 3 bytes
  return false jika karakter tidak valid
```

Standard Base64 alphabet: `A–Z a–z 0–9 + /`

---

## HTTP Helper

```cpp
// POST JSON, return response body. httpCode via pointer.
String httpPost(const String& endpoint, const String& body, int* httpCode);

// GET, return response body.
String httpGet(const String& endpoint, int* httpCode);
```

Kedua fungsi:
- Header `X-Device-Token: DEVICE_TOKEN`
- `Content-Type: application/json` (POST)
- Timeout 10 detik (`http.setTimeout(10000)`)
- `http.end()` selalu dipanggil (tidak leak)
- Return `""` jika `httpCode < 0` (network error)

---

## `postTaskResult(taskId, status, errorMsg, template)`

```
HTTP POST → BACKEND_BASE + "/tasks/" + taskId + "/result"
Header: X-Device-Token

Body:
{
  "status": "done" | "failed",
  "error_message": "..." | null,
  "template": "<base64>" | null   // hanya untuk enroll_capture
}
```

---

## OLED Display

OLED 128×64 px, font size 1 = 6×8 px/char → 21 char × 8 baris.  
Digunakan 3 baris dengan spacing:

```
Line 1: y = 0   (status utama)
Line 2: y = 22  (nama / detail)
Line 3: y = 44  (info tambahan)
```

```cpp
void showOled(const String& l1, const String& l2 = "", const String& l3 = "");
// Auto-truncate tiap baris ke 21 char, tampil tanpa delay

void showOledIdle();
// "Tempel Jari" + jam NTP realtime

void showOledEnroll(const String& nama, int step);
// "ENROLL [step]/2" + nama
```

Tidak ada `delay()` di dalam fungsi OLED. Delay untuk user-facing pause ada di caller.

---

## WiFi Reconnect

```cpp
void checkWifiReconnect() {
  if (WiFi.isConnected()) return;
  if (millis() - lastWifiRetryMs < WIFI_RETRY_MS) return;

  lastWifiRetryMs = millis();
  showOled("WiFi Putus!", "Reconnecting...", "");
  WiFi.reconnect();

  // Tunggu max 5 detik
  for (int i = 0; i < 10 && !WiFi.isConnected(); i++) delay(500);

  if (WiFi.isConnected()) showOledIdle();
}
```

---

## Scan UID — Idempotency

```cpp
String generateScanUid() {
  uint32_t r1 = esp_random();
  uint32_t r2 = esp_random();
  char uid[17];
  snprintf(uid, sizeof(uid), "%08X%08X", r1, r2);
  return String(uid);   // "A3F2B1C8D9E0F123" — 16 char, maks 64 per validasi server
}
```

`esp_random()` menggunakan hardware RNG ESP32 → kualitas entropi baik.

---

## Struktur File Final

```
src/main.cpp
├── #include & #define
├── Konstanta konfigurasi
├── State variables global
├── Deklarasi fungsi (forward declarations)
│
├── setup()
├── loop()
│
├── checkWifiReconnect()
├── tickFingerprintScan()
├── sendAttendance(fingerprintId)
│
├── tickTaskPoller()
├── handleEnrollTask(task JSON)
├── handleSyncTask(task JSON)
├── handleDeleteTask(task JSON)
├── postTaskResult(id, status, errorMsg, template)
│
├── downloadTemplate(slot) → String
├── uploadTemplate(slot, buf, len) → bool
│
├── base64Encode(data, len) → String
├── base64Decode(str, outBuf, &outLen) → bool
│
├── httpPost(endpoint, body, &httpCode) → String
├── httpGet(endpoint, &httpCode) → String
│
├── showOled(l1, l2, l3)
├── showOledIdle()
└── showOledEnroll(nama, step)
```

Semua dalam satu file — konsisten dengan struktur proyek yang ada.

---

## API Contracts (Referensi)

### POST `/api/device/attendance`
**Request:**
```json
{
  "fingerprint_id": 5,
  "scan_uid": "A3F2B1C8D9E0F123",
  "waktu_scan": "2026-06-15 08:32:17"
}
```
**Response 200:**
```json
{
  "status": "ok",
  "hasil": "masuk",
  "display_oled": "Ahmad Fauzi · Masuk · 08:32",
  "nama_karyawan": "Ahmad Fauzi"
}
```

### GET `/api/device/tasks`
**Response:**
```json
{
  "tasks": [
    {
      "id": 123,
      "action": "enroll_capture",
      "fingerprint_id": 7,
      "payload": null,
      "karyawan": { "nama_lengkap": "Ahmad Fauzi", "fingerprint_id": 7 }
    }
  ],
  "slot_used": 45,
  "slot_total": 127,
  "slot_warning": false
}
```

### POST `/api/device/tasks/{id}/result`
**Request:**
```json
{
  "status": "done",
  "error_message": null,
  "template": "<base64 string, hanya untuk enroll_capture>"
}
```

**Semua request pakai header:**
```
X-Device-Token: <plaintext token>
Content-Type: application/json
```

---

## Hal yang Perlu Disiapkan Sebelum Flash

| Item | Cara |
|---|---|
| IP server | `ip addr` atau `hostname -I` di mesin server |
| Token device | Saat buat alat di web admin, simpan plaintext sebelum disimpan ke DB |
| Upload port | `/dev/ttyUSB0` sudah di `platformio.ini`, sesuaikan jika beda |

---

## Alur Enrollment End-to-End

```
Manager klik "Mulai Enroll" di web
  → Server: buat DeviceTask enroll_capture untuk master alat
      → Master device polling → dapat task
          → OLED: "Tempel Jari 1/2"
          → AS608: capture dua kali
          → Buat model, simpan ke slot N
          → Download template 512 bytes → base64
          → POST result + template ke server
              → Server: simpan finger_template di DB karyawan
              → Server: buat sync_template task untuk semua alat lain
                  → Semua alat polling → dapat sync task
                      → Decode base64 → upload ke slot N masing-masing
                      → POST result "done"
```
