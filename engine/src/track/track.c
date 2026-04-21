#include "track.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

void track_init(Track *t, uint8_t id)
{
    memset(t, 0, sizeof(*t));
    t->id        = id;
    t->gain_db   = 0.0f;
    t->state     = TRACK_STATE_IDLE;
    t->pre_armed = -1;
    t->folder_id = -1;
    snprintf(t->label, TRACK_LABEL_MAX, "Track %u", id + 1);
}

void track_set_label(Track *t, const char *label)
{
    strncpy(t->label, label, TRACK_LABEL_MAX - 1);
    t->label[TRACK_LABEL_MAX - 1] = '\0';
}

float track_gain_linear(const Track *t)
{
    return powf(10.0f, t->gain_db / 20.0f);
}
