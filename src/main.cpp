#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_Fingerprint.h>

// OLED
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// AS608
HardwareSerial mySerial(2);
Adafruit_Fingerprint finger = Adafruit_Fingerprint(&mySerial);

void showOLED(String line1, String line2 = "", String line3 = "")
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println(line1);
  display.setCursor(0, 20);
  display.println(line2);
  display.setCursor(0, 40);
  display.println(line3);
  display.display();
}

void setup()
{
  Serial.begin(115200);
  delay(500);

  // 1. Init AS608 DULU
  mySerial.begin(57600, SERIAL_8N1, 16, 17);
  finger.begin(57600);
  delay(500);

  // 2. Baru init I2C dan OLED
  Wire.begin(21, 22);
  delay(100);

  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C))
  {
    Serial.println("OLED tidak ditemukan!");
    while (true)
      ; // berhenti kalau OLED gagal
  }

  Serial.println("OLED OK!");
  showOLED("Initializing...", "Please wait...");
  delay(1000);

  // 3. Cek AS608
  if (finger.verifyPassword())
  {
    Serial.println("AS608 OK!");
    showOLED("AS608 Found!", "Sensor Ready", "Place finger...");
  }
  else
  {
    Serial.println("AS608 not found!");
    showOLED("AS608 ERROR!", "Cek kabel!", "RX/TX kebalik?");
    while (true)
      ;
  }

  delay(2000);
  showOLED("Ready", "Place finger", "to scan...");
}

void loop()
{
  uint8_t p = finger.getImage();

  if (p == FINGERPRINT_OK)
  {
    showOLED("Finger detected!", "Processing...");

    p = finger.image2Tz();
    if (p != FINGERPRINT_OK)
    {
      showOLED("Image error!", "Try again...");
      delay(2000);
      return;
    }

    p = finger.fingerSearch();
    if (p == FINGERPRINT_OK)
    {
      showOLED("MATCH FOUND!", "ID: " + String(finger.fingerID), "Conf: " + String(finger.confidence));
      Serial.println("ID: " + String(finger.fingerID));
    }
    else
    {
      showOLED("No match!", "Not registered", "Try again...");
    }

    delay(3000);
    showOLED("Ready", "Place finger", "to scan...");
  }
}