#include <Arduino.h>
#include <U8g2lib.h>
#include <SPI.h>

U8G2_SH1106_128X64_NONAME_F_4W_SW_SPI u8g2(
  U8G2_R0, 1, 2, 5, 4, 3
);

unsigned long startTime;

void setup() {
  startTime = millis();
  u8g2.begin();
}

void loop() {
  unsigned long elapsed = (millis() - startTime) / 1000;
  int ss = elapsed % 60;
  int mm = (elapsed / 60) % 60;
  int hh = elapsed / 3600;

  char timeBuf[9];
  sprintf(timeBuf, "%02d:%02d:%02d", hh, mm, ss);

  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_6x10_tf);
  u8g2.drawStr(28, 12, "IBMOVS CLOCK");
  u8g2.drawHLine(0, 15, 128);
  u8g2.setFont(u8g2_font_logisoso28_tf);
  u8g2.drawStr(4, 52, timeBuf);
  u8g2.drawHLine(0, 55, 128);
  u8g2.setFont(u8g2_font_5x7_tf);
  u8g2.drawStr(30, 63, "ESP32-S3-Zero");
  u8g2.sendBuffer();
  delay(500);
}
