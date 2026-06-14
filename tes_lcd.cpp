#include <Arduino.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// GANTI alamat sesuai hasil I2C Scanner
LiquidCrystal_I2C lcd(0x27, 16, 2);  // atau 0x3F

void setup() {
  Serial.begin(115200);
  Wire.begin(21, 22);
  
  Serial.println("Init LCD...");
  
  lcd.init();
  lcd.backlight();
  
  Serial.println("LCD initialized!");
  
  lcd.setCursor(0, 0);
  lcd.print("Hello World!");
  lcd.setCursor(0, 1);
  lcd.print("Testing...");
  
  Serial.println("Text printed to LCD!");
}

void loop() {
  delay(1000);
}