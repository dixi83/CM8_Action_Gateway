#ifndef CM8_INPUT_H
#define CM8_INPUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CM8_HANDLE_CONTROL 0x001E
#define CM8_HANDLE_RECORD  0x0034
#define CM8_HANDLE_WHEEL   0x0038

typedef enum {
    CM8_EVENT_NONE = 0,
    CM8_EVENT_RECORD_SHORT,
    CM8_EVENT_RECORD_LONG,
    CM8_EVENT_RECORD_DOUBLE,
    CM8_EVENT_RECORD_RELEASE,
    CM8_EVENT_MODE_TOGGLE,
    CM8_EVENT_WHEEL_POSITION,
} cm8_event_type_t;

typedef struct {
    cm8_event_type_t type;
    uint8_t raw_code;
    uint8_t mode_state;
    uint16_t wheel_value;
    int64_t received_at_us;
} cm8_event_t;

bool cm8_input_decode(uint16_t handle, const uint8_t *data, size_t length,
                      cm8_event_t *event);

#endif
