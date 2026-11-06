# aLabs HMI Widgets

An LV2 extension, `https://github.com/sejerpz/alabs-hmi-widgets`, that lets a plugin draw on areas of the
display of a device (HMI). It is implemented on the MOD Dwarf by the
[aLabs custom images](https://github.com/sejerpz/alabs-mod-custom-images).

The bundle `alabs-hmi-widgets.lv2` holds the specification (`alabs-hmi-widgets.ttl`) and the C header
(`alabs-hmi-widgets.h`). The extension stands on its own: it does not need nor change the MOD HMI extension
(`http://moddevices.com/ns/hmi`), and a plugin can use both.

## Overview

A plugin declares **widgets** in its TTL and implements the **`ahw:WidgetNotification`** interface. A widget is
not tied to any parameter: for each area of the display the user decides, page by page and subpage by subpage,
whether it shows the default content of the device (e.g. the knob addressing) or one of the widgets of the
pedalboard. The hardware controls keep working as usual either way, only what is drawn changes.

Which areas a display has is up to the device. A widget only says how much room it needs and how much it would
like, in pixels, and the host offers it where it fits.

A widget may also suggest which of its ports it would like bound to the controls of the device. The host offers
the bindings to the user when the widget is placed; where the widget sits and what the controls are bound to stay
independent choices of the user.

While a widget is visible the host gives the plugin a handle, and the plugin draws with **`draw_bitmap()`** of
the **`ahw:WidgetDraw`** feature. Bitmaps use a plain pixel format (1 bit per pixel, rows top to bottom on the
Dwarf) at any position: the device adapts them to its display controller, the plugin needs to know nothing
about it.

## Declaring widgets

```turtle
@prefix ahw: <https://github.com/sejerpz/alabs-hmi-widgets#> .
@prefix lv2: <http://lv2plug.in/ns/lv2core#> .

<http://example.org/plugin>
    lv2:optionalFeature ahw:WidgetDraw ;
    lv2:extensionData ahw:WidgetNotification ;
    ahw:widget [
        a ahw:Widget ;
        lv2:symbol "meter" ;
        lv2:name "Meter" ;
        ahw:minWidth 16 ;
        ahw:minHeight 10 ;
        ahw:naturalWidth 40 ;
        ahw:naturalHeight 37 ;
        ahw:pixelFormat ahw:Mono1 ;
    ] , [
        a ahw:Widget ;
        lv2:symbol "stop" ;
        lv2:name "Stop all" ;
        ahw:minWidth 32 ;
        ahw:minHeight 8 ;
        ahw:suggestedPorts ( "stop_all" ) ;
    ] .
```

| Term | Kind | Meaning |
|---|---|---|
| `ahw:WidgetDraw` | `lv2:Feature` | Host feature the plugin draws with. |
| `ahw:WidgetNotification` | `lv2:ExtensionData` | Plugin interface notified when its widgets are shown and hidden. |
| `ahw:Widget` | class | Something the plugin draws on an area of the display. Has an `lv2:symbol`, unique among the widgets of the plugin, and an `lv2:name` shown to the user. |
| `ahw:widget` | property of `lv2:Plugin` | A widget the plugin can draw. |
| `ahw:minWidth`, `ahw:minHeight` | `xsd:positiveInteger` | The smallest area, in pixels, the widget can draw in. Any size when missing. |
| `ahw:naturalWidth`, `ahw:naturalHeight` | `xsd:positiveInteger` | The size, in pixels, the widget looks best at. The minimum when missing or smaller. |
| `ahw:suggestedPorts` | `rdf:List` of strings | The `lv2:symbol` of the input control ports the widget would like bound to the controls of the device, in order. |
| `ahw:pixelFormat` | `ahw:PixelFormat` | A pixel format the widget can draw in, `ahw:Mono1` alone when missing. Only offered on a display using one of them. |
| `ahw:Mono1`, `ahw:Gray4`, `ahw:RGB565`, `ahw:RGB888` | `ahw:PixelFormat` | The pixel formats, matching `ALABS_HMI_PixelFormat`. |

### Size

- The host only offers a widget in areas at least `ahw:minWidth` wide and `ahw:minHeight` high. A missing minimum
  counts as 1 pixel: the widget fits any area.
- The natural size is a hint, never a limit: the host uses it to order the areas it offers, closest first, and
  may preselect one. It never excludes an area for being bigger: a small icon in a big area is the user's choice.
- The closest area is the one with the smallest `|width − naturalWidth| + |height − naturalHeight|`, ties going
  to the area the device lists first. Since a missing natural size is the minimum, and a missing minimum is 1
  pixel, a widget that declares neither gets the smallest area first.
- The plugin draws in **any** area at least as big as its minimum, and learns its size in `widget_shown()`.
  In an area bigger than its natural size it decides itself whether to center, scale or fill. It never
  hardcodes a size.
- Sizes are in pixels of the device display: a widget on a denser display simply gets bigger areas.
- The sizes of the areas of the MOD Dwarf, to copy into `ahw:minWidth`, `ahw:minHeight`, `ahw:naturalWidth` and
  `ahw:naturalHeight`:

  | Area on the MOD Dwarf | Width × height (px) |
  |---|---|
  | One column, above a knob | 40 × 37 |
  | Two columns | 82 × 37 |
  | Three columns | 124 × 37 |
  | One footer, above footswitch B or C | 51 × 10 (footer B is 53 wide) |
  | Both footers | 104 × 10 |
  | Full screen | 128 × 64 |

  A widget that fits one footer on the Dwarf declares at most 51 × 10 as its minimum, so that it fits footer C too.

### Suggested ports and bindings

`ahw:suggestedPorts` answers a simple worry: a widget that draws "STOP ALL" promises something, and it is easier
to keep the promise if placing the widget also offers to bind the port. It is a suggestion, and a host may ignore
it. A host that follows it:

- applies it only when the user places the widget, in the host's layout UI, never when a pedalboard is loaded;
- shows the user what it is about to bind, and the user can change or refuse it;
- creates ordinary bindings, independent from the widget: they stay when the widget is removed, and removing a
  binding leaves the widget where it is;
- never replaces a control already in use and never touches a port already bound;
- uses the controls nearest to the widget first: those of its page (and of its subpage, where controls have
  subpages), then those of the following pages, never going back to earlier ones;
- binds the ports in the order they are listed, to contiguous free controls in the order the device presents
  them, and respects controls that cannot take a binding (e.g. a footswitch that belongs to a group in use);
- binds toggled and trigger ports to switches, the others to knobs, and toggled and trigger ports to knobs only
  when no switch is free;
- binds what it can when there are not enough free controls, and tells the user which ports are left.

Where a widget sits and what the controls are bound to are **independent choices of the user**. The host does
not promise that a widget is above the controls bound to its ports: it is perfectly fine to place a widget above a
control bound to another plugin, or above no control at all.

A widget that shows the state of its ports learns whether they are bound from the MOD HMI extension
(`http://moddevices.com/ns/hmi`): `addressed()` and `unaddressed()` of `LV2_HMI_PluginNotification` tell it, and the
functions of `LV2_HMI_WidgetControl` (`set_label()`, `set_led_with_blink()`, `set_value()`, ...) act on the real
control without the plugin knowing which one it is. Since it does not know where that control is, a widget should
draw a state ("STOP ALL", "not bound") rather than a direction ("press here"). Known limits:

- `addressed()` is only called for HMI bindings: MIDI and Control Chain do not call it.
- Where a widget covers the default content of a control, what `set_label()`, `set_value()` and `set_unit()`
  change is not seen there: the device keeps it and shows it again when the area goes back to the default
  content, and wherever else it shows the control (e.g. overlays). LEDs are not on the display and always work.
- `set_label()` replaces the label the user gave to the binding.

## C API

All of it is in `alabs-hmi-widgets.h`.

### `ALABS_HMI_WidgetNotification` (plugin interface)

Returned by `extension_data()` for `ALABS_HMI__WidgetNotification`. It starts with its own size, so callbacks
can be appended later and the host checks which ones the plugin has.

```c
typedef struct {
    size_t size;  /* sizeof(ALABS_HMI_WidgetNotification) as known by the plugin */

    void (*widget_shown)(LV2_Handle handle,
                         const char* symbol,
                         ALABS_HMI_Widget widget,
                         const ALABS_HMI_WidgetArea* area);

    void (*widget_hidden)(LV2_Handle handle, ALABS_HMI_Widget widget);
} ALABS_HMI_WidgetNotification;
```

- **`widget_shown`**: the widget declared with `symbol` is now visible on an area of the display, e.g. because
  the user moved to its page or placed it on the current one. The plugin keeps `widget` and uses it with
  `draw_bitmap()` until `widget_hidden()`. Every showing gets a new handle. `area` is only valid during the call.
  The plugin **must redraw the whole area** after this call: nothing drawn during a previous showing is
  guaranteed to be there. Until it does, the area shows nothing, a device placeholder or a stale copy.
  The same widget may be visible in more than one area at once, each with its own handle.
- **`widget_hidden`**: the widget is no longer visible. The plugin must not draw with that handle afterwards.

### `ALABS_HMI_WidgetArea`

```c
typedef struct {
    size_t size;                        /* fields are only ever appended: check before reading later ones */
    uint16_t width, height;             /* in pixels, origin at the top-left corner of the area */
    ALABS_HMI_PixelFormat pixelFormat;  /* always one the widget declares with ahw:pixelFormat */
} ALABS_HMI_WidgetArea;
```

Where the area sits on the display is up to the device. The plugin draws in `0..width-1`, `0..height-1`.

### `ALABS_HMI_PixelFormat`

In every format a bitmap holds its rows from top to bottom, and all bits zero is the background.

| Value | Layout |
|---|---|
| `ALABS_HMI_PixelFormat_Mono1 = 0` | 1 bit per pixel, a set bit is lit. A row is `(width + 7) / 8` bytes, the most significant bit of its first byte leftmost. The format of the MOD Dwarf. |
| `ALABS_HMI_PixelFormat_Gray4 = 1` | 4 bits per pixel, 0 background, 15 brightest. A row is `(width + 1) / 2` bytes, high nibble leftmost. |
| `ALABS_HMI_PixelFormat_RGB565 = 2` | 16 bits per pixel, 5 red, 6 green, 5 blue, big endian. A row is `width * 2` bytes. |
| `ALABS_HMI_PixelFormat_RGB888 = 3` | 24 bits per pixel, red, green, blue. A row is `width * 3` bytes. |

Only `Mono1` is implemented; the others are there so that a colour display can be added without changing
the API.

### `ALABS_HMI_WidgetDraw` (host feature)

`LV2_Feature::data` of `ALABS_HMI__WidgetDraw`. It starts with its own size, so functions can be appended later
and the plugin checks which ones the host has.

```c
typedef struct {
    size_t size;                         /* sizeof(ALABS_HMI_WidgetDraw) as known by the host */
    ALABS_HMI_WidgetDraw_Handle handle;  /* opaque host data */

    void (*draw_bitmap)(ALABS_HMI_WidgetDraw_Handle handle,
                        ALABS_HMI_Widget widget,
                        uint16_t x, uint16_t y,
                        uint16_t width, uint16_t height,
                        const uint8_t* bits);
} ALABS_HMI_WidgetDraw;
```

`draw_bitmap()`:

- Draws into the area of a visible widget, in the rectangle `x, y, width, height` given in area coordinates.
  Any rectangle is fine, so the plugin can update just what changed; pixels outside the area are clipped.
  The bitmap replaces the pixels of the rectangle, the rest of the area is untouched.
- `bits` holds `height` rows in the pixel format of the area. Every row starts at the beginning of its first
  byte whatever `x` is: when `x` does not fall on a byte boundary, the device shifts, not the plugin.
- `bits == NULL` fills the rectangle with the background without sending pixel data. Clearing the whole area
  is `NULL` with `0, 0` and the size of the area.
- Updates, clears included, are applied in call order. The host may merge pending updates before they reach the
  display, never changing the final result.
- Only valid with a handle received in `widget_shown()` and not yet hidden.

### Threads

- `widget_shown()` and `widget_hidden()` are called from a non real-time thread, one at a time for the same
  plugin instance, but possibly while `run()` is executing in the audio thread. They are never called
  concurrently with `instantiate()`, `activate()`, `deactivate()` or `cleanup()`.
- The plugin synchronizes what it shares between them and `run()`. The example publishes the widget handle
  with an atomic store after the rest of the state of the showing, and `run()` reads it with an atomic load.
- `draw_bitmap()` may be called from any thread, `run()` included. The host makes it safe for the audio thread:
  it never waits for the display, it only queues the update, and serializes calls coming from different threads.
- Once `widget_hidden()` has returned the plugin starts no new `draw_bitmap()` with that handle. A call already
  running in another thread at that moment is harmless: the host drops it.

### Compatibility

`ahw:WidgetDraw` is an optional feature. A plugin with widgets runs on every host: where the feature is missing
it does not draw, and where widgets are not supported it is simply never shown one.

## Reference implementation (MOD Dwarf)

- **Geometry** comes from the hardware descriptor, extended with drop-in files merged by mod-ui
  (`/etc/mod-hardware-descriptor.d/*.json`), never hardcoded. It lists every area the device offers, the merged
  ones included, each with its rectangle in display coordinates and the **cells** it takes:

  ```json
  {
      "hmi_widget_areas": {
          "col1":    { "rect": [2, 12, 40, 37],   "cells": ["col1"] },
          "col2":    { "rect": [44, 12, 40, 37],  "cells": ["col2"] },
          "col3":    { "rect": [86, 12, 40, 37],  "cells": ["col3"] },
          "col1-2":  { "rect": [2, 12, 82, 37],   "cells": ["col1", "col2"] },
          "col2-3":  { "rect": [44, 12, 82, 37],  "cells": ["col2", "col3"] },
          "col1-3":  { "rect": [2, 12, 124, 37],  "cells": ["col1", "col2", "col3"] },
          "footB":   { "rect": [24, 54, 53, 10],  "cells": ["footB"], "subpages": false },
          "footC":   { "rect": [77, 54, 51, 10],  "cells": ["footC"], "subpages": false },
          "footB-C": { "rect": [24, 54, 104, 10], "cells": ["footB", "footC"], "subpages": false },
          "full":    { "rect": [0, 0, 128, 64],
                       "cells": ["title", "col1", "col2", "col3", "index", "footB", "footC"] }
      },
      "hmi_widget_pixel_format": "mono1"
  }
  ```

  - Three columns of 40×37 pixels above the knobs, 2 pixels apart, alone or merged (82 and 124 pixels wide);
    the footers of footswitches B and C, alone or together; the whole 128×64 display.
  - Two areas sharing a cell exclude each other on the same page and subpage. Which areas can be merged is
    decided by whoever writes the descriptor, by listing them: there is no geometry to compute, and odd shapes
    simply do not exist.
  - `"subpages": false` marks an area whose content does not change with the subpage, like the footers of the
    footswitches: a widget placed there is shown on every subpage of its page, and excludes, on every subpage,
    the areas sharing its cells. Missing means one placement per subpage.
  - The title band is not offered on its own for now; only `full` covers it. Areas dedicated to the title band
    may come later.
  - A device without `hmi_widget_areas` offers no widgets.
- **mod-ui** has a *Display* dialog (Device menu) where the user picks, for each page and subpage, which area
  shows which widget, among the widgets that fit it (closest natural size first), or leaves the default content.
  When a widget with `ahw:suggestedPorts` is placed, the dialog lists the bindings it is about to make, with the
  rules above, and the user confirms, changes or drops them. It tells the controller about every area of every
  page up front, so switching pages needs no round trip, and tells mod-host only about the widgets in view.
- **mod-host** gets `hmi_widget_show <instance> <symbol> <area_id> <width> <height> <format>` and
  `hmi_widget_hide <area_id|-1>`, calls the plugin's `widget_shown()`/`widget_hidden()`, clips each
  `draw_bitmap()` to its area and forwards it through the same shared memory path labels and LEDs already use.
- **mod-system-control** sends bitmaps to the controller over the system serial line, split into bands of whole
  rows that fit one protocol message.
- **Controller firmware** keeps a 1-bit backing buffer for each visible area only, so popups, overlays and the
  tuner can cover an area and give it back without asking the plugin. It converts rows to the display's page
  layout itself, with any `x` and `y`. Areas leaving the view drop their pixels; the plugin redraws on the next
  `widget_shown()`.

### Saving the layout with a pedalboard

Not part of the extension: how a host remembers where the user placed the widgets is up to the host. mod-ui
saves which widget each area shows next to the pedalboard, as `hmi-widgets.json` in the bundle:

```json
{
    "format": "https://github.com/sejerpz/alabs-hmi-widgets#WidgetLayout",
    "version": 2,
    "widgets": [
        { "page": 0, "subpage": 0, "area": "col1",
          "instance": "/graph/meter_1", "symbol": "meter" },
        { "page": 0, "area": "footC",
          "instance": "/graph/looper_1", "symbol": "stop" },
        { "page": 0, "subpage": 2, "area": "full",
          "instance": "/graph/scope_1", "symbol": "scope" }
    ]
}
```

- `format` identifies what the file holds and `version` its revision, so a later mod-ui can tell what it reads and
  migrate it. The version is bumped only when an older mod-ui would misread the layout; fields it can skip do not
  change it.
- `area` names an area of the hardware descriptor. `subpage` is left out for areas with `"subpages": false`.
- Version 1, with `column`, `columns` and `fullscreen`, is not read: it was never released.
- mod-ui, finding a newer version than it knows, does not use it, and writes it back unchanged when the
  pedalboard is saved, unless the user places widgets in the meantime.
- Entries that no longer fit (a plugin removed, a widget no longer declared, an area the device does not have or
  that is now too small for the widget, an overlap) are dropped when the layout is loaded.
- The bindings made from `ahw:suggestedPorts` are ordinary bindings, saved with the others: the layout does not
  record them.
- No new RDF predicate is written into the pedalboard TTL.

### Measured on a Dwarf

| | |
|---|---|
| Full 128×64 frame | 34 ms, ~23 fps sustained (serial link ~15 ms, per-message round trips ~15 ms, firmware ~4 ms) |
| Clearing a whole area | 1.3 ms, about one round trip |
| Partial updates | a few bytes per frame for a moving object (bouncing ball / brick breaker demo) |

Example plugin with widgets of many sizes, from a footer to the full screen (meter, waveform, bouncing ball, looper,
starfield, brick breaker game with suggested ports): [`examples/hmi-widget-draw`](examples/hmi-widget-draw).

## Open points

- **Message budget.** The host does not limit or merge updates yet: everything a plugin draws is forwarded. A
  shared budget for the whole display (plus merging of pending rectangles) is not implemented and has to be
  sized on real plugin traffic first.
- **Keeping pixels out of view.** The firmware keeps pixels only for the areas in view, and plugins redraw on
  every showing. Caching the screens reachable in one step (other subpages, next and previous page) would avoid
  the redraw, but needs the free RAM of the controller measured first.
- **Grouped footswitches.** When footswitches B and C are grouped on a page, the firmware draws one footer across
  both: a widget on `footB` or `footC` alone covers part of it, and the firmware skips the rest. To be checked on
  a Dwarf, and possibly offered only as `footB-C` on those pages.
- **Freeform layout.** The areas of the descriptor are a list. A later revision can add a canvas, e.g.
  `"hmi_widget_canvas": { "rect": [0, 12, 128, 52], "grid": 1 }`, where the user draws any rectangle: layout
  entries would then carry a `rect` instead of an `area`, and overlaps would be computed on the rectangles. The
  plugin side does not change: it only ever sees the size of its area.

## License

ISC, see `LICENSE`.
