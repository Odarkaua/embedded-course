#include <Arduino.h>
const int LED_GRN = 4;
const int LED_RED = 5;
const int LED_BLUE = 6;

const unsigned long BLINK_INTERVAL_GRN = 200;
const unsigned long BLINK_INTERVAL_RED = 500;
const unsigned long BLINK_INTERVAL_BLUE = 1000;

unsigned long lastBlinkTimeGrn = 0;
unsigned long lastBlinkTimeRed = 0;
unsigned long lastBlinkTimeBlue = 0;

void setup() {
    pinMode(LED_GRN, OUTPUT);
    pinMode(LED_RED, OUTPUT);
    pinMode(LED_BLUE, OUTPUT);
}

void loop() {
    unsigned long currentTime = millis();
    if (currentTime - lastBlinkTimeGrn >= BLINK_INTERVAL_GRN) {
        lastBlinkTimeGrn = currentTime;
        digitalWrite(LED_GRN, !digitalRead(LED_GRN));
    }

    if (currentTime - lastBlinkTimeRed >= BLINK_INTERVAL_RED) {
        lastBlinkTimeRed = currentTime;
        digitalWrite(LED_RED, !digitalRead(LED_RED));
    }

    if (currentTime - lastBlinkTimeBlue >= BLINK_INTERVAL_BLUE) {
        lastBlinkTimeBlue = currentTime;
        digitalWrite(LED_BLUE, !digitalRead(LED_BLUE));
    }
}