#include "cm8_input.h"

#include <string.h>

bool cm8_input_decode(uint16_t handle, const uint8_t *data, size_t length,
                      cm8_event_t *event) {
    if (data == NULL || event == NULL) return false;
    memset(event, 0, sizeof(*event));

    if (handle == CM8_HANDLE_RECORD && length >= 2 && data[1] == 0) {
        event->raw_code = data[0];
        switch (data[0]) {
        case 0x00: event->type = CM8_EVENT_RECORD_RELEASE; return true;
        case 0xE9:
        case 0xEA: event->type = CM8_EVENT_RECORD_SHORT; return true;
        case 0xA0: event->type = CM8_EVENT_RECORD_LONG; return true;
        case 0x82: event->type = CM8_EVENT_RECORD_DOUBLE; return true;
        default: return false;
        }
    }

    if (handle == CM8_HANDLE_WHEEL && length == 6 &&
        data[2] == 0x02 && data[3] == 0x08) {
        event->type = CM8_EVENT_WHEEL_POSITION;
        event->wheel_value = (uint16_t)data[4] | ((uint16_t)data[5] << 8);
        return true;
    }

    if (handle == CM8_HANDLE_CONTROL && length >= 7 &&
        data[0] == 0xAA && data[1] == 0xAA && data[2] == 0x81 &&
        data[3] == 0x01 && data[4] == 0xC6 && data[5] <= 1) {
        event->type = CM8_EVENT_MODE_TOGGLE;
        event->mode_state = data[5];
        return true;
    }

    return false;
}
