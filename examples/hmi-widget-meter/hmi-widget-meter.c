/*
  hmi-widget-meter: a gain with an output meter drawn on the display of the device,
  the plugin of the aLabs HMI widgets tutorial (TUTORIAL.md). ISC license, see LICENSE.
*/

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "lv2/core/lv2.h"
#include "lv2/core/lv2_util.h"

#include "alabs-hmi-widgets.h"

#define METER_URI "https://github.com/sejerpz/alabs-hmi-widgets/examples/hmi-widget-meter"

#define MAX_SHOWINGS      4    // the same widget may be in view in more areas at once
#define MAX_WIDTH         128  // the largest area of the devices known so far
#define MAX_HEIGHT        64
#define FRAMES_PER_SECOND 25
#define FLOOR_DB          -48.0f

enum { PORT_IN, PORT_OUT, PORT_GAIN };

typedef struct {
    ALABS_HMI_Widget widget;  // NULL while free: written last by widget_shown, with an atomic store
    uint16_t width, height;
    int shown;                // pixels of the bar drawn last time, -1 for none
} Showing;

typedef struct {
    const float* in;
    float* out;
    const float* gain;

    const ALABS_HMI_WidgetDraw* hmi;  // NULL on a host without widgets: the plugin works, it just does not draw
    Showing showings[MAX_SHOWINGS];

    uint32_t frame_samples, counter;
    float peak, level;
    uint8_t bitmap[MAX_WIDTH / 8 * MAX_HEIGHT];  // used by run() only
} Meter;

// how much of the bar the level lights, 0..1
static float
fraction(float level)
{
    const float db = 20.0f * log10f(level + 1e-9f);
    return fminf(fmaxf((db - FLOOR_DB) / -FLOOR_DB, 0.0f), 1.0f);
}

// pixels of the bar for a level, in an area of this width
static int
bar_length(float level, int width)
{
    return (int)(fraction(level) * (width - 4) + 0.5f);
}

// a frame on the edge of the area and, two pixels inside, a bar growing from the left:
// one bit per pixel (ahw:Mono1), rows top to bottom, the most significant bit leftmost
static void
render(uint8_t* bits, int width, int height, int bar)
{
    const int stride = (width + 7) / 8;
    memset(bits, 0, (size_t)stride * height);

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool edge = x == 0 || y == 0 || x == width - 1 || y == height - 1;
            const bool lit = x >= 2 && x < 2 + bar && y >= 2 && y < height - 2;
            if (edge || lit)
                bits[y * stride + x / 8] |= 0x80 >> (x % 8);
        }
    }
}

static LV2_Handle
instantiate(const LV2_Descriptor* descriptor, double rate, const char* bundle_path,
            const LV2_Feature* const* features)
{
    Meter* self = calloc(1, sizeof(Meter));
    if (!self)
        return NULL;

    // optional: a host without widgets does not give it
    lv2_features_query(features, ALABS_HMI__WidgetDraw, &self->hmi, false, NULL);
    if (self->hmi && self->hmi->size < ALABS_HMI_WIDGETDRAW_SIZE_BASE)
        self->hmi = NULL;

    self->frame_samples = (uint32_t)(rate / FRAMES_PER_SECOND);
    return self;
}

static void
connect_port(LV2_Handle handle, uint32_t port, void* data)
{
    Meter* self = handle;

    switch (port) {
    case PORT_IN:   self->in = data;   break;
    case PORT_OUT:  self->out = data;  break;
    case PORT_GAIN: self->gain = data; break;
    }
}

static void
run(LV2_Handle handle, uint32_t samples)
{
    Meter* self = handle;
    const float gain = powf(10.0f, *self->gain / 20.0f);

    for (uint32_t i = 0; i < samples; ++i) {
        self->out[i] = self->in[i] * gain;
        self->peak = fmaxf(self->peak, fabsf(self->out[i]));
    }

    self->counter += samples;
    if (self->counter < self->frame_samples)
        return;
    self->counter = 0;

    // a new frame: the level follows the peaks up at once and falls back slowly
    self->level = fmaxf(self->peak, self->level * 0.8f);
    self->peak = 0.0f;

    if (!self->hmi)
        return;

    // draw_bitmap only queues the update, so it is fine here in the audio thread
    for (int i = 0; i < MAX_SHOWINGS; ++i) {
        Showing* s = &self->showings[i];
        const ALABS_HMI_Widget widget = __atomic_load_n(&s->widget, __ATOMIC_ACQUIRE);
        if (!widget)
            continue;

        // nothing changed, nothing sent: the display keeps what it has
        const int bar = bar_length(self->level, s->width);
        if (bar == s->shown)
            continue;

        render(self->bitmap, s->width, s->height, bar);
        self->hmi->draw_bitmap(self->hmi->handle, widget, 0, 0, s->width, s->height, self->bitmap);
        s->shown = bar;
    }
}

static void
cleanup(LV2_Handle handle)
{
    free(handle);
}

// the widget comes into view: keep its handle and size, and draw the whole area at once
static void
widget_shown(LV2_Handle handle, const char* symbol, ALABS_HMI_Widget widget, const ALABS_HMI_WidgetArea* area)
{
    Meter* self = handle;

    // one widget only, "meter", in Mono1, the only pixel format it declares
    if (strcmp(symbol, "meter") || area->width > MAX_WIDTH || area->height > MAX_HEIGHT)
        return;

    for (int i = 0; i < MAX_SHOWINGS; ++i) {
        Showing* s = &self->showings[i];
        if (__atomic_load_n(&s->widget, __ATOMIC_ACQUIRE))
            continue;

        // the area may be any size from the minimum up: draw what it is now, never a size known in advance
        uint8_t bits[MAX_WIDTH / 8 * MAX_HEIGHT];
        const int bar = bar_length(self->level, area->width);
        render(bits, area->width, area->height, bar);
        self->hmi->draw_bitmap(self->hmi->handle, widget, 0, 0, area->width, area->height, bits);

        // the rest of the showing first, the handle last: run() takes it from there
        s->width = area->width;
        s->height = area->height;
        s->shown = bar;
        __atomic_store_n(&s->widget, widget, __ATOMIC_RELEASE);
        return;
    }
}

// out of view: forget the handle, the host drops a draw still on its way
static void
widget_hidden(LV2_Handle handle, ALABS_HMI_Widget widget)
{
    Meter* self = handle;

    for (int i = 0; i < MAX_SHOWINGS; ++i)
        if (__atomic_load_n(&self->showings[i].widget, __ATOMIC_ACQUIRE) == widget)
            __atomic_store_n(&self->showings[i].widget, NULL, __ATOMIC_RELEASE);
}

static const void*
extension_data(const char* uri)
{
    static const ALABS_HMI_WidgetNotification notification = {
        sizeof(ALABS_HMI_WidgetNotification),
        widget_shown,
        widget_hidden,
    };

    if (!strcmp(uri, ALABS_HMI__WidgetNotification))
        return &notification;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    METER_URI,
    instantiate,
    connect_port,
    NULL,  // activate
    run,
    NULL,  // deactivate
    cleanup,
    extension_data,
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
    return index == 0 ? &descriptor : NULL;
}
