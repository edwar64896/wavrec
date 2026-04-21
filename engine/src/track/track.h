#pragma once
#include <stdint.h>
#include <stdbool.h>

#define TRACK_LABEL_MAX 64

typedef enum {
    TRACK_STATE_IDLE = 0,
    TRACK_STATE_ARMED,
    TRACK_STATE_RECORDING,
    TRACK_STATE_PLAYING,
} TrackState;

typedef struct Track {
    uint8_t    id;
    char       label[TRACK_LABEL_MAX];
    uint8_t    hw_input;     /* hardware input channel index */
    float      gain_db;
    bool       armed;
    bool       monitor;      /* route input to output during record */
    TrackState state;
    int8_t     pre_armed;    /* -1 = none; 0 = pending disarm; 1 = pending arm */
} Track;

void  track_init(Track *t, uint8_t id);
void  track_set_label(Track *t, const char *label);
float track_gain_linear(const Track *t); /* converts gain_db to linear */
