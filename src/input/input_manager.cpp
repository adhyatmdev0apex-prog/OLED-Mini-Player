#include "input_manager.h"

InputManager& InputManager::getInstance() {
    static InputManager instance;
    return instance;
}

InputManager::InputManager()
    : m_scrollLastReading(HIGH)
    , m_scrollState(HIGH)
    , m_scrollDebounceMs(0)
    , m_scrollIsDown(false)
    , m_scrollDownStartMs(0)
    , m_scrollLastRepeatMs(0)
    , m_selectLastReading(HIGH)
    , m_selectState(HIGH)
    , m_selectDebounceMs(0)
    , m_selectIsDown(false)
    , m_selectDownStartMs(0)
    , m_selectLongPressFired(false) {
}

void InputManager::begin() {
    // Configure buttons with internal pullups (Active-LOW when pressed to GND)
    pinMode(PIN_BUTTON_SCROLL, INPUT_PULLUP);
    pinMode(PIN_BUTTON_SELECT, INPUT_PULLUP);

    m_scrollLastReading = digitalRead(PIN_BUTTON_SCROLL);
    m_scrollState = m_scrollLastReading;

    m_selectLastReading = digitalRead(PIN_BUTTON_SELECT);
    m_selectState = m_selectLastReading;

    Serial.printf("[INPUT] Buttons initialized: Scroll=GPIO%d, Select=GPIO%d (Active-LOW to GND)\n",
                  PIN_BUTTON_SCROLL, PIN_BUTTON_SELECT);
}

InputEvent InputManager::poll() {
    uint32_t now = millis();
    InputEvent result = InputEvent::NONE;

    // -------------------------------------------------------------
    // 1. Service Scroll Button (GPIO 2)
    // -------------------------------------------------------------
    int scrollReading = digitalRead(PIN_BUTTON_SCROLL);
    if (scrollReading != m_scrollLastReading) {
        m_scrollDebounceMs = now;
        m_scrollLastReading = scrollReading;
    }

    if ((now - m_scrollDebounceMs) > BUTTON_DEBOUNCE_MS) {
        if (scrollReading != m_scrollState) {
            m_scrollState = scrollReading;
            if (m_scrollState == LOW) {
                // Button just pressed down
                m_scrollIsDown = true;
                m_scrollDownStartMs = now;
                m_scrollLastRepeatMs = now;
                Serial.printf("[INPUT] Button 1 (GPIO %d) Pressed -> SCROLL\n", PIN_BUTTON_SCROLL);
                result = InputEvent::SCROLL_NEXT;
            } else {
                // Button released
                m_scrollIsDown = false;
            }
        }
    }

    // Auto-repeat scroll while held down
    if (m_scrollIsDown && result == InputEvent::NONE) {
        if ((now - m_scrollDownStartMs) >= BUTTON_HOLD_SCROLL_INITIAL_MS) {
            if ((now - m_scrollLastRepeatMs) >= BUTTON_HOLD_SCROLL_RATE_MS) {
                m_scrollLastRepeatMs = now;
                Serial.printf("[INPUT] Button 1 (GPIO %d) Held -> REPEAT SCROLL\n", PIN_BUTTON_SCROLL);
                result = InputEvent::SCROLL_NEXT;
            }
        }
    }

    // If scroll event occurred, return immediately
    if (result != InputEvent::NONE) {
        return result;
    }

    // -------------------------------------------------------------
    // 2. Service Select / Play Button (GPIO 15)
    // -------------------------------------------------------------
    int selectReading = digitalRead(PIN_BUTTON_SELECT);
    if (selectReading != m_selectLastReading) {
        m_selectDebounceMs = now;
        m_selectLastReading = selectReading;
    }

    if ((now - m_selectDebounceMs) > BUTTON_DEBOUNCE_MS) {
        if (selectReading != m_selectState) {
            m_selectState = selectReading;
            if (m_selectState == LOW) {
                // Button pressed down
                m_selectIsDown = true;
                m_selectDownStartMs = now;
                m_selectLongPressFired = false;
                Serial.printf("[INPUT] Button 2 (GPIO %d) Pressed down...\n", PIN_BUTTON_SELECT);
            } else {
                // Button released
                if (m_selectIsDown && !m_selectLongPressFired) {
                    m_selectIsDown = false;
                    Serial.printf("[INPUT] Button 2 (GPIO %d) Released -> SHORT PRESS (Select/Pause/Resume)\n", PIN_BUTTON_SELECT);
                    return InputEvent::SELECT_SHORT_PRESS;
                }
                m_selectIsDown = false;
            }
        }
    }

    // Long press check (>= 1.0s hold)
    if (m_selectIsDown && !m_selectLongPressFired) {
        if ((now - m_selectDownStartMs) >= BUTTON_LONG_PRESS_MS) {
            m_selectLongPressFired = true;
            Serial.printf("[INPUT] Button 2 (GPIO %d) Held >= 1s -> LONG PRESS (Stop / Back to Menu)\n", PIN_BUTTON_SELECT);
            return InputEvent::SELECT_LONG_PRESS;
        }
    }

    return InputEvent::NONE;
}
