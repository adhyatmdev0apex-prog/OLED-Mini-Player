#ifndef COMMAND_PROTOCOL_H
#define COMMAND_PROTOCOL_H

#include <Arduino.h>

class CommandProtocol {
public:
    static CommandProtocol& getInstance();

    void begin();
    void pollSerial();

private:
    CommandProtocol();
    ~CommandProtocol() = default;

    CommandProtocol(const CommandProtocol&) = delete;
    CommandProtocol& operator=(const CommandProtocol&) = delete;

    void handleLine(const char* line);

    char m_buf[256];
    uint8_t m_len;
};

#endif // COMMAND_PROTOCOL_H
