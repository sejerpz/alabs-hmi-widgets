# Tutorial: a plugin with a widget

This tutorial shows how to write an LV2 plugin that draws on the display of a MOD device with the aLabs HMI widgets
extension, and how to try it on a desktop computer.

## What it is

aLabs HMI widgets is an LV2 extension that lets a plugin draw its own things on the display of the device: a meter,
a waveform, a looper position, a tuner, a small game. The plugin declares **widgets** in its TTL, each with the
smallest area it can draw in and the size it looks best at. The user decides, page by page, which areas of the
display show a widget and which keep the usual content of the device. While a widget is in view, the plugin gets a
handle and draws 1 bit per pixel bitmaps on it.

It is useful whenever a plugin has something to show that a label and a value cannot tell: levels, positions in
time, shapes, states. The knobs and footswitches keep working as usual: only what the display shows changes.

> **Status: experimental.** The extension, its C API and the hardware descriptor keys may still change in
> incompatible ways. It runs on the MOD Dwarf with the [aLabs custom images](https://github.com/sejerpz/alabs-mod-custom-images)
> and on desktop computers with the mod-hw-emulator packages below. MOD Audio's stock firmware does not
> implement it: on other hosts a plugin with widgets works as usual, it is just never shown a widget.

## What you need

- **mod-hw-emulator with MOD Desktop**, the package for Linux, Windows or macOS from
  [sejerpz/mod-hw-emulator](https://github.com/sejerpz/mod-hw-emulator) (Releases, or built with
  `scripts/package-*.sh`, see its README). It is MOD Desktop with mod-host and mod-ui supporting the widgets and an
  emulated MOD Dwarf, running its real controller firmware. Plugins, pedalboards and settings go in `data/`, next
  to `mhe-desktop.sh` (or `mhe-desktop.cmd` on Windows).
- A C compiler and the LV2 headers (`lv2-dev` on Debian and Ubuntu).
- This repository, for `alabs-hmi-widgets.lv2/alabs-hmi-widgets.h`.

The complete plugin of this tutorial is in [`examples/hmi-widget-meter`](examples/hmi-widget-meter).

## The plugin

A gain with a meter of its output level: a bar that grows from the left inside a frame. It fits the areas above a
knob and the footers above the footswitches, and it adapts to any size it gets.

### 1. Declare the widget

In the TTL of the plugin, next to the usual ports:

```turtle
@prefix ahw: <https://github.com/sejerpz/alabs-hmi-widgets#> .

<https://github.com/sejerpz/alabs-hmi-widgets/examples/hmi-widget-meter>
    a lv2:Plugin, lv2:AmplifierPlugin ;
    lv2:optionalFeature lv2:hardRTCapable, ahw:WidgetDraw ;
    lv2:extensionData ahw:WidgetNotification ;

    ahw:widget [
        a ahw:Widget ;
        lv2:symbol "meter" ;
        lv2:name "Output meter" ;
        ahw:minWidth 16 ;
        ahw:minHeight 8 ;
        ahw:naturalWidth 40 ;
        ahw:naturalHeight 37 ;
        ahw:suggestedPorts ( "gain" ) ;
    ] ;
    # lv2:port ... in, out and gain, as in any plugin
```

- `ahw:WidgetDraw` is an **optional** feature: without it the plugin still loads, it just does not draw.
- `ahw:minWidth`, `ahw:minHeight` are a limit: the host only offers the widget in areas at least that big. With 16 × 8
  the meter fits everywhere, including the footers of the Dwarf (51 × 10) and of the Duo X (63 × 8).
- `ahw:naturalWidth`, `ahw:naturalHeight` are a wish: the host offers the closest area first. 40 × 37 is a column
  above a knob of the Dwarf.
- `ahw:suggestedPorts` is optional too: when the user places the meter, the host offers to bind `gain` to a free knob.
- No `ahw:pixelFormat`: the default is `ahw:Mono1`, 1 bit per pixel, the format of the Dwarf.

### 2. Get the drawing feature

The host gives `ALABS_HMI_WidgetDraw` as a feature. Look it up in `instantiate()`, without requiring it:

```c
#include "alabs-hmi-widgets.h"

typedef struct {
    ALABS_HMI_Widget widget;   /* NULL while free: written last, with an atomic store */
    uint16_t width, height;
    int shown;                 /* pixels of the bar drawn last time */
} Showing;

typedef struct {
    /* ports ... */
    const ALABS_HMI_WidgetDraw* hmi;   /* NULL on a host without widgets */
    Showing showings[MAX_SHOWINGS];    /* the same widget may be in view in more areas at once */
    /* ... */
} Meter;

lv2_features_query(features, ALABS_HMI__WidgetDraw, &self->hmi, false, NULL);
if (self->hmi && self->hmi->size < ALABS_HMI_WIDGETDRAW_SIZE_BASE)
    self->hmi = NULL;
```

### 3. Draw a bitmap

A `Mono1` bitmap holds its rows from top to bottom, `(width + 7) / 8` bytes each, the most significant bit
leftmost; a set bit is a lit pixel. The meter draws a frame on the edge and a bar two pixels inside it:

```c
static void
render(uint8_t* bits, int width, int height, int bar)
{
    const int stride = (width + 7) / 8;
    memset(bits, 0, (size_t)stride * height);

    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x) {
            const bool edge = x == 0 || y == 0 || x == width - 1 || y == height - 1;
            const bool lit = x >= 2 && x < 2 + bar && y >= 2 && y < height - 2;
            if (edge || lit)
                bits[y * stride + x / 8] |= 0x80 >> (x % 8);
        }
}
```

Everything depends on `width` and `height`: a widget never assumes a size, it draws in the area it is given. In the
example, `bar_length(level, width)` turns the level into pixels of bar for that width: from −48 dB (no bar) to 0 dB
(the full width).

### 4. Implement `ALABS_HMI_WidgetNotification`

The host tells the plugin when its widget comes into view and when it leaves it. In `widget_shown()` the plugin
keeps the handle and the size of the area, and **draws the whole area at once**: nothing drawn before is guaranteed
to still be there.

```c
static void
widget_shown(LV2_Handle handle, const char* symbol, ALABS_HMI_Widget widget, const ALABS_HMI_WidgetArea* area)
{
    Meter* self = handle;

    if (strcmp(symbol, "meter") || area->width > MAX_WIDTH || area->height > MAX_HEIGHT)
        return;

    for (int i = 0; i < MAX_SHOWINGS; ++i) {
        Showing* s = &self->showings[i];
        if (__atomic_load_n(&s->widget, __ATOMIC_ACQUIRE))
            continue;

        uint8_t bits[MAX_WIDTH / 8 * MAX_HEIGHT];
        const int bar = bar_length(self->level, area->width);
        render(bits, area->width, area->height, bar);
        self->hmi->draw_bitmap(self->hmi->handle, widget, 0, 0, area->width, area->height, bits);

        /* the rest of the showing first, the handle last: run() takes it from there */
        s->width = area->width;
        s->height = area->height;
        s->shown = bar;
        __atomic_store_n(&s->widget, widget, __ATOMIC_RELEASE);
        return;
    }
}

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

    return strcmp(uri, ALABS_HMI__WidgetNotification) ? NULL : &notification;
}
```

Both callbacks come from a non real-time thread, possibly while `run()` is running: that is why the handle is
published with an atomic store after the rest of the showing, and read with an atomic load.

### 5. Update it from `run()`

`draw_bitmap()` never waits for the display, it only queues the update, so it can be called from the audio thread.
The meter measures the peak of each block and, 25 times a second, redraws the areas whose bar changed:

```c
    /* in run(), once per frame */
    self->level = fmaxf(self->peak, self->level * 0.8f);   /* up at once, down slowly */
    self->peak = 0.0f;

    for (int i = 0; i < MAX_SHOWINGS; ++i) {
        Showing* s = &self->showings[i];
        const ALABS_HMI_Widget widget = __atomic_load_n(&s->widget, __ATOMIC_ACQUIRE);
        if (!widget)
            continue;

        const int bar = bar_length(self->level, s->width);
        if (bar == s->shown)
            continue;   /* nothing changed, nothing sent */

        render(self->bitmap, s->width, s->height, bar);
        self->hmi->draw_bitmap(self->hmi->handle, widget, 0, 0, s->width, s->height, self->bitmap);
        s->shown = bar;
    }
```

Every update travels to the controller of the device, so draw only when something changed: the meter redraws when
its bar moves, and once the level has fallen to nothing it sends nothing at all.

The meter redraws its whole area for simplicity, but partial updates are supported too: `draw_bitmap()` takes any
rectangle inside the area, at any `x` and `y`, and leaves the rest of the area untouched. With `bits == NULL` it
clears the rectangle without sending pixels. The meter could send just the columns between the old and the new end
of the bar; the brick game in [`examples/hmi-widget-draw`](examples/hmi-widget-draw) works this way, sending only the
ball, the paddle and the bricks hit on each frame.

### 6. Build and install

```sh
cd examples/hmi-widget-meter
make                                   # hmi-widget-meter.lv2/hmi-widget-meter.so
```

On macOS `make LIB_EXT=.dylib`, on Windows `make CC=x86_64-w64-mingw32-gcc LIB_EXT=.dll`; in both cases change
`lv2:binary` in `manifest.ttl` to match.

Copy the bundle into the `lv2` folder of the package data, here with the package unpacked in your home folder:

```sh
mkdir -p ~/mod-hw-emulator/data/lv2
cp -r hmi-widget-meter.lv2 ~/mod-hw-emulator/data/lv2/
```

(with `--shared-data` the folder is `~/Documents/MOD Desktop/lv2`, the one of an installed MOD Desktop).

## Try it

1. Start the package: `~/mod-hw-emulator/mhe-desktop.sh` (`mhe-desktop.cmd` on Windows). The emulated Dwarf opens
   in its own window, then MOD Desktop. MOD Desktop reads the plugins when it starts: restart it after adding one.
2. Open the pedalboard editor and add **HMI Widget Meter** between the capture and the playback.
3. Open **Device ▸ Display**. The dialog lists the widgets of the pedalboard on one side and a drawing of the
   display on the other.
4. Pick **Output meter** and tap column 1 on the drawing. As the widget suggests, the dialog offers to bind **Gain**
   to a free knob of that page: keep it or change it.
5. **Save**. The meter appears on the emulated Dwarf above knob 1: play into the input and the bar moves, turn the
   knob bound to Gain and the level changes.

Then try the other areas: the meter in a footer (51 × 10), in two columns, in more areas at once. The same code draws
in all of them.

## Full screen and more

A widget can also take the **whole display**: declare a natural size of 128 × 64 (and a minimum up to that) and the
host offers the full screen first. [`examples/hmi-widget-draw`](examples/hmi-widget-draw) has a starfield and a
brick game on the full screen. The game sends only the parts that move with partial `draw_bitmap()` calls, and binds
its controls through `ahw:suggestedPorts`.

The [specification](README.md) covers the rest: all the areas of the Dwarf and of the Duo X with their sizes, how
to choose a minimum that works on both, the rules of the suggested bindings, the threading contract, how the layout
is saved with the pedalboard, and the other pixel formats reserved for colour displays.
