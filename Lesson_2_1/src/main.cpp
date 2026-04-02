
#include <Arduino.h>

// 1. Оголосити enum class для стану LED:
enum class LedState : uint8_t
{
    Off = 0,
    On = 1
};

// 2. Створити constexpr-константи
constexpr uint8_t LED_PIN = 4;
constexpr uint8_t BUTTON_PIN = 6;
constexpr unsigned long BLINK_INTERVAL_MS = 500;

// 3. Створити клас-конфігурацію або namespace для додаткових параметрів:
class Config
{
public:
    static const uint8_t BlinksNum = 10;
    static const unsigned long DebounceTimeMs = 50;
};

// 4. Створити клас Led.
class Led
{
public:
    explicit Led(uint8_t pin);
    void init();
    void set(LedState state);
    LedState state() const;

private:
    uint8_t pin_;
    LedState currentState_{LedState::Off};
};
// Конструктор
Led::Led(uint8_t pin) : pin_(pin) {}

void Led::init() {
    pinMode(pin_, OUTPUT);
}

void Led::set(LedState state)
{
    currentState_ = state;
    digitalWrite(pin_, (state == LedState::On) ? HIGH : LOW);
}

LedState Led::state() const
{
    return currentState_;
}
// 7. Створити об'єкт Led.
Led led(LED_PIN);

// 5. Робимо розширену версію з режимами,
enum class LedMode : uint8_t
{
    Blinking,
    AlwaysOn,
    AlwaysOff
};
LedMode currentMode = LedMode::Blinking;

// 6.Кнопка з перериванням:
volatile bool buttonPressed = false;
unsigned long lastPressedTime = 0;

// 8. Написати ISR для кнопки.
void buttonISR() {
    buttonPressed = true;
}

void setup()
{
    // 9. Ініціалізувати Serial, якщо потрібен для відлагодження
    Serial.begin(115200);
    // 10. Викликати led.init()
    led.init();

    // 11. Якщо є кнопка:
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    attachInterrupt(digitalPinToInterrupt(BUTTON_PIN), buttonISR, FALLING);

}

unsigned long previousMillis = 0;
unsigned long loopCount = 0;
unsigned long loopStartTime = 0;

void loop()
{
    // 13. Отримати поточний час через millis()
    unsigned long currentMillis = millis();
    loopCount++;

    // 15. Якщо є кнопка:
    if (buttonPressed == true) {
        buttonPressed = false;

        if (currentMode == LedMode::Blinking) {
            currentMode = LedMode::AlwaysOn;
        } else if (currentMode == LedMode::AlwaysOn) {
            currentMode = LedMode::AlwaysOff;
        } else {
            currentMode = LedMode::Blinking;
        }
    }

    // 14. Реалізувати неблокуюче блимання:

    if (currentMode == LedMode::Blinking) {
        // блимання прямо тут
        if (currentMillis - previousMillis >= BLINK_INTERVAL_MS) {
            previousMillis = currentMillis;
            if (led.state() == LedState::Off) {
                led.set(LedState::On);
            } else {
                led.set(LedState::Off);
            }
        }
    } else if (currentMode == LedMode::AlwaysOn) {
        led.set(LedState::On);
    } else {
        led.set(LedState::Off);
    }



    // 19. Опційно: виміряти час однієї ітерації superloop
    if (loopCount >= 1000) {
        unsigned long elapsed = currentMillis - loopStartTime;
        Serial.print("Loop 1000 iterations, ms: ");
        Serial.println(elapsed);
        loopCount = 0;
        loopStartTime = currentMillis;
    }
}