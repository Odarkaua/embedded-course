#include <Arduino.h>

constexpr uint8_t IN_PIN = 4;
constexpr uint8_t OUT_PIN = 6;

volatile unsigned long releTriggerTime = 0;
volatile bool releIsOff = false;

uint8_t loopCount = 0;
unsigned long SumTime = 0;

void stateISR() {
    releIsOff = true;
    releTriggerTime = millis();
}

void setup() {
    Serial.begin(115200);
    pinMode(IN_PIN, INPUT_PULLDOWN);
    pinMode(OUT_PIN, OUTPUT);
    attachInterrupt(digitalPinToInterrupt(IN_PIN), stateISR, RISING);
}

void loop() {
    if (loopCount >= 10) return;
    digitalWrite(OUT_PIN, HIGH);
    unsigned long startTime = millis();

    if (releIsOff == true) {
        unsigned long elapsed = releTriggerTime - startTime;
        Serial.print("Час спрацювання реле, ms: ");
        Serial.println(elapsed);
        SumTime += elapsed;
        releIsOff = false;
        digitalWrite(OUT_PIN, LOW);
        loopCount++;

        if (loopCount == 10) {
            unsigned long Ser_time = SumTime / 10;
            Serial.print("Середній час спрацювання реле, ms: ");
            Serial.println(Ser_time);
        }
    }
}