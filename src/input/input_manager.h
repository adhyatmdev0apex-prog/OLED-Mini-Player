#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <Arduino.h>
#include "../config/app_config.h"

enum class InputEvent {
    NONE,
    SCROLL_NEXT,          // GPIO 2: single tap or auto-repeat continuous hold
    SELECT_SHORT_PRESS,   // GPIO 4: short tap released (Select/Play or Pause/Resume)
    SELECT_LONG_PRESS     // GPIO 4: held >= 1.0s (Stop playback and return to menu)
};

class InputManager {
public:
    static InputManager& getInstance();

    void begin();
    
    // Polls physical buttons and returns pending event
    InputEvent poll();

private:
    InputManager();
    ~InputManager() = default;

    InputManager(const InputManager&) = delete;
    InputManager& operator=(const InputManager&) = delete;

    // Scroll Button State (GPIO 2)
    int m_scrollLastReading;
    int m_scrollState;
    uint32_t m_scrollDebounceMs;
    bool m_scrollIsDown;
    uint32_t m_scrollDownStartMs;
    uint32_t m_scrollLastRepeatMs;

    // Select Button State (GPIO 4)
    int m_selectLastReading;
    int m_selectState;
    uint32_t m_selectDebounceMs;
    bool m_selectIsDown;
    uint32_t m_selectDownStartMs;
    bool m_selectLongPressFired;
};

#endif // INPUT_MANAGER_H
