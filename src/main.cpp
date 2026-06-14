#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
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
int templateCount = 0; // Track manually

// WiFi
const char* WIFI_SSID = "alamak";
const char* WIFI_PASSWORD = "harusbisa";
const char* BACKEND_URL = "http://10.117.2.241:8000/index.php";

bool wifiConnected = false;

void showOLED(String line1, String line2, String line3);
void enrollFingerprint(int id);
int countTemplates();
void sendAttendance(int fingerprintID, const char* status);

void showOLED(String line1, String line2 = "", String line3 = "")
{
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Truncate each line to max 21 chars (128 pixels / 6 pixels per char)
  if (line1.length() > 21) line1 = line1.substring(0, 18) + "...";
  if (line2.length() > 21) line2 = line2.substring(0, 18) + "...";
  if (line3.length() > 21) line3 = line3.substring(0, 18) + "...";

  display.setCursor(0, 0);
  display.println(line1);
  display.setCursor(0, 16);
  display.println(line2);
  display.setCursor(0, 32);
  display.println(line3);
  display.display();
  delay(2000); // Delay for display to refresh properly
}

void setup()
{
  Serial.begin(9600);
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
    // Auto count templates from sensor
    templateCount = countTemplates();
    Serial.println("Templates: " + String(templateCount) + " / 127");
    showOLED("AS608 Found!", "Templates: " + String(templateCount) + "/127", "Place finger...");
  }
  else
  {
    Serial.println("AS608 not found!");
    showOLED("AS608 ERROR!", "Cek kabel!", "RX/TX kebalik?");
    while (true)
      ;
  }

  // 4. Connect WiFi
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.println("Connecting to WiFi...");
  showOLED("Connecting...", "WiFi...", "");

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20)
  {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED)
  {
    wifiConnected = true;
    Serial.println("\nWiFi Connected!");
    Serial.println("IP: " + WiFi.localIP().toString());
    showOLED("WiFi Connected!", "IP: " + WiFi.localIP().toString(), "");
  }
  else
  {
    wifiConnected = false;
    Serial.println("\nWiFi FAILED!");
    showOLED("WiFi FAILED!", "Check credentials", "");
  }

  delay(2000);
  showOLED("Ready (" + String(templateCount) + "/127)", "Place finger to scan", "");
}

void loop()
{
  // Check for serial commands
  if (Serial.available() > 0)
  {
    String command = Serial.readStringUntil('\n');
    command.trim();

    if (command.startsWith("enroll"))
    {
      int id = command.substring(7).toInt();
      if (id > 0 && id <= 127)
      {
        enrollFingerprint(id);
      }
      else
      {
        Serial.println("Invalid ID. Use: enroll 1-127");
      }
    }
    else if (command == "templates")
    {
      templateCount = countTemplates();
      Serial.println("Templates: " + String(templateCount) + " / 127");
    }
    else if (command == "listids")
    {
      Serial.println("Registered IDs:");
      for (int id = 1; id <= 127; id++)
      {
        if (finger.loadModel(id) == FINGERPRINT_OK)
        {
          Serial.println("  ID: " + String(id));
        }
      }
    }
    else if (command.startsWith("delete"))
    {
      int id = command.substring(7).toInt();
      if (id > 0 && id <= 127)
      {
        if (finger.deleteModel(id) == FINGERPRINT_OK)
        {
          Serial.println("Deleted ID: " + String(id));
          templateCount = countTemplates();
        }
        else
        {
          Serial.println("Failed to delete ID: " + String(id));
        }
      }
    }
    else if (command.startsWith("setcount"))
    {
      int count = command.substring(9).toInt();
      if (count >= 0 && count <= 127)
      {
        templateCount = count;
        Serial.println("Template count set to: " + String(templateCount));
      }
    }
    else if (command == "clearall")
    {
      finger.emptyDatabase();
      templateCount = 0;
      Serial.println("All templates cleared!");
      showOLED("CLEARED!", "All templates", "deleted");
      delay(2000);
    }
    else if (command == "wifi")
    {
      if (WiFi.status() == WL_CONNECTED)
      {
        Serial.println("WiFi: Connected");
        Serial.println("IP: " + WiFi.localIP().toString());
      }
      else
      {
        Serial.println("WiFi: Not connected");
      }
    }
  }

  // Normal fingerprint scan
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

      // Send to backend
      if (wifiConnected)
      {
        sendAttendance(finger.fingerID, "in");
      }
    }
    else
    {
      showOLED("No match!", "Not registered", "Try again...");
    }

    delay(3000);
    showOLED("Ready (" + String(templateCount) + "/127)", "Place finger to scan", "");
  }
}

void enrollFingerprint(int id)
{
  Serial.println("=== ENROLL MODE ===");
  Serial.println("Place finger on sensor...");

  // Capture first image - wait for finger to be placed
  showOLED("Enroll ID:" + String(id), "Place finger", "on sensor...");

  // Wait up to 10 seconds for finger
  uint8_t p;
  int timeout = 0;
  while ((p = finger.getImage()) != FINGERPRINT_OK && timeout < 100)
  {
    delay(100);
    timeout++;
  }

  if (p != FINGERPRINT_OK)
  {
    Serial.println("Failed to capture first image");
    showOLED("Enroll FAILED!", "Sensor error", "Try again...");
    delay(2000);
    return;
  }

  Serial.println("Remove finger...");
  showOLED("Remove finger", "then place again", "...");

  // Wait for finger removal
  delay(3000);
  while (finger.getImage() != FINGERPRINT_NOFINGER)
  {
    delay(100);
  }

  Serial.println("Place same finger again...");
  showOLED("Place same", "finger again", "...");

  // Wait up to 10 seconds for finger again
  timeout = 0;
  while ((p = finger.getImage()) != FINGERPRINT_OK && timeout < 100)
  {
    delay(100);
    timeout++;
  }

  if (p != FINGERPRINT_OK)
  {
    Serial.println("Failed to capture second image");
    showOLED("Enroll FAILED!", "Sensor error", "Try again...");
    delay(2000);
    return;
  }

  // Generate model and store
  p = finger.image2Tz(1); // Save to slot 1
  if (p != FINGERPRINT_OK)
  {
    Serial.println("Failed to process image 1");
    showOLED("Enroll FAILED!", "Process error", "Try again...");
    delay(2000);
    return;
  }

  p = finger.image2Tz(2); // Save to slot 2
  if (p != FINGERPRINT_OK)
  {
    Serial.println("Failed to process image 2");
    showOLED("Enroll FAILED!", "Process error", "Try again...");
    delay(2000);
    return;
  }

  p = finger.createModel();
  if (p != FINGERPRINT_OK)
  {
    Serial.println("Failed to create model");
    showOLED("Enroll FAILED!", "Model error", "Try again...");
    delay(2000);
    return;
  }

  p = finger.storeModel(id);
  if (p == FINGERPRINT_OK)
  {
    Serial.println("SUCCESS! Fingerprint stored as ID: " + String(id));
    showOLED("Enroll SUCCESS!", "ID:" + String(id) + " stored", "");
  }
  else
  {
    Serial.println("Failed to store model. Error: " + String(p));
    showOLED("Enroll FAILED!", "Store error", "Try again...");
    delay(2000);
    return;
  }

  delay(3000);
  templateCount = countTemplates(); // Auto count after enroll
  showOLED("Ready (" + String(templateCount) + "/127)", "Place finger to scan", "");
}

// Auto count templates by scanning all IDs
int countTemplates()
{
  int count = 0;
  for (int id = 1; id <= 127; id++)
  {
    if (finger.loadModel(id) == FINGERPRINT_OK)
    {
      count++;
    }
  }
  return count;
}

// Send attendance to backend
void sendAttendance(int fingerprintID, const char* status)
{
  HTTPClient http;
  http.begin(BACKEND_URL);
  http.addHeader("Content-Type", "application/json");

  String jsonPayload = "{\"fingerprint_id\":" + String(fingerprintID) + ",\"status\":\"" + String(status) + "\"}";

  int httpResponseCode = http.POST(jsonPayload);

  if (httpResponseCode > 0)
  {
    String response = http.getString();
    Serial.println("Backend Response: " + String(httpResponseCode));
  }
  else
  {
    Serial.println("Backend Error: " + http.errorToString(httpResponseCode));
  }

  http.end();
}
