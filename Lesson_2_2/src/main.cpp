#include <Arduino.h>

constexpr uint8_t IN_PIN = 4;
constexpr uint8_t OUT_PIN = 18;

volatile unsigned long releTriggerTime = 0;
volatile bool releIsOff = false;

uint8_t loopCount = 0;
unsigned long SumTime = 0;

bool waiting = false;
unsigned long startTime = 0;

void IRAM_ATTR stateISR() {
    releIsOff = true;
    releTriggerTime = millis();
}

void setup() {
    Serial.begin(115200);
    pinMode(IN_PIN, INPUT_PULLDOWN);
    pinMode(OUT_PIN, OUTPUT);

    digitalWrite(OUT_PIN, LOW);   // початково реле вимкнене
    attachInterrupt(digitalPinToInterrupt(IN_PIN), stateISR, RISING);
}

void loop() {
    if (loopCount >= 10) return;

    if (waiting == false) {
        releIsOff = false;
        digitalWrite(OUT_PIN, HIGH);   // вмикаємо реле
        startTime = millis();          // запам'ятали момент старту
        waiting = true;
    }

    if (releIsOff == true && waiting == true) {
        unsigned long elapsed = releTriggerTime - startTime;
        Serial.print("Час спрацювання реле, ms: ");
        Serial.println(elapsed);

        SumTime += elapsed;
        releIsOff = false;
        digitalWrite(OUT_PIN, LOW);    // вимикаємо реле
        loopCount++;
        waiting = false;

        delay(500); // невелика пауза між вимірюваннями

        if (loopCount == 10) {
            unsigned long Ser_time = SumTime / 10;
            Serial.print("Середній час спрацювання реле, ms: ");
            Serial.println(Ser_time);
        }
    }
}