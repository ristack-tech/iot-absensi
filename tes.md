#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Adafruit_Fingerprint.h>

// ==========================================
// 1. KONFIGURASI PIN HARDWARE
// ==========================================
#define LCD_SDA 21
#define LCD_SCL 22
#define FP_RX 16 // Hubungkan ke TX (Kuning) AS608
#define FP_TX 17 // Hubungkan ke RX (Putih) AS608

// ==========================================
// 2. INISIALISASI OBJEK
// ==========================================
LiquidCrystal_I2C lcd(0x3F, 16, 2); // Alamat I2C LCD (biasanya 0x27 atau 0x3F)
HardwareSerial fingerSerial(2);     // Gunakan UART2 (Hardware Serial)
Adafruit_Fingerprint finger(&fingerSerial);

// Variabel untuk mencegah double scan
unsigned long lastScanTime = 0;
const unsigned long scanCooldown = 3000; // Jeda 3 detik setelah scan

// ==========================================
// 3. FUNCTION DECLARATIONS
// ==========================================
void showReadyScreen();
int getFingerprintID();

// ==========================================
// 4. SETUP
// ==========================================
void setup()
{
  // Init Serial Monitor untuk debugging
  Serial.begin(115200);
  delay(1000); // Tunggu serial monitor siap

  Serial.println("\n\n=== SISTEM ABSENSI FINGERPRINT ===");

  // --- Init LCD ---
  Wire.begin(LCD_SDA, LCD_SCL);
  lcd.init();
  lcd.backlight();
  lcd.setCursor(0, 0);
  lcd.print("Sistem Absensi");
  lcd.setCursor(0, 1);
  lcd.print("Booting...      ");
  delay(1000);

  // --- Init Fingerprint Sensor ---
  Serial.println("Inisialisasi sensor fingerprint...");
  fingerSerial.begin(57600, SERIAL_8N1, FP_RX, FP_TX);

  // FIX: Tambahkan parameter baud rate 57600
  finger.begin(57600);

  lcd.setCursor(0, 1);
  lcd.print("Cek Sensor...   ");

  if (finger.verifyPassword())
  {
    Serial.println("✅ Fingerprint sensor ditemukan!");
    lcd.setCursor(0, 1);
    lcd.print("Sensor OK!      ");
    delay(1500);
  }
  else
  {
    Serial.println("❌ Fingerprint sensor TIDAK ditemukan!");
    lcd.setCursor(0, 1);
    lcd.print("Sensor ERROR!   ");
    Serial.println("Periksa wiring TX/RX dan power sensor!");
    // Halt program jika sensor tidak terdeteksi
    while (1)
    {
      delay(1000);
    }
  }

  // Tampilkan layar siap scan
  showReadyScreen();
  Serial.println("Sistem siap! Silakan tempelkan jari...\n");
}

// ==========================================
// 5. MAIN LOOP
// ==========================================
void loop()
{
  int fingerprintID = getFingerprintID();

  if (fingerprintID > 0)
  {
    // Cek cooldown agar tidak double scan berturut-turut
    if (millis() - lastScanTime > scanCooldown)
    {
      lastScanTime = millis();

      // Tampilkan hasil di LCD
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("BERHASIL!       ");
      lcd.setCursor(0, 1);
      lcd.print("ID: ");
      lcd.print(fingerprintID);

      // Kirim info ke Serial Monitor
      Serial.print("✅ Fingerprint cocok! ID: ");
      Serial.println(fingerprintID);

      delay(3000); // Tunggu 3 detik agar user bisa melihat layar
      showReadyScreen();
    }
  }
  else if (fingerprintID == -2)
  {
    // Jika jari terdeteksi tapi tidak cocok dengan database sensor
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("GAGAL!          ");
    lcd.setCursor(0, 1);
    lcd.print("Jari Tidak Dikenal");
    Serial.println("❌ Jari tidak dikenal!");
    delay(2000);
    showReadyScreen();
  }

  delay(50); // Jeda kecil agar loop tidak membebani CPU
}

// ==========================================
// 6. HELPER FUNCTIONS
// ==========================================

// Menampilkan layar siap scan
void showReadyScreen()
{
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Tempel Jari Anda");
  lcd.setCursor(0, 1);
  lcd.print("untuk Absen     ");
}

// Fungsi mengambil dan mencocokkan fingerprint
int getFingerprintID()
{
  uint8_t p = finger.getImage();
  if (p != FINGERPRINT_OK)
    return -1; // Belum ada jari

  p = finger.image2Tz();
  if (p != FINGERPRINT_OK)
    return -1; // Gagal memproses gambar

  p = finger.fingerFastSearch();
  if (p == FINGERPRINT_OK)
  {
    return finger.fingerID; // Berhasil cocok, kembalikan ID
  }

  return -2; // Jari ada, tapi tidak cocok di database sensor
}nnnnnnnnnnnnnnn