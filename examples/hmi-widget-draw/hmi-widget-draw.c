#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lv2/core/lv2.h"
#include "lv2/core/lv2_util.h"
#include "lv2/log/log.h"
#include "lv2/log/logger.h"

#include "alabs-hmi-widgets.h"

#define PLUGIN_URI "https://github.com/sejerpz/alabs-hmi-widgets/examples/hmi-widget-draw"

// the largest area this example draws, the whole Dwarf display
#define MAX_WIDTH  128
#define MAX_HEIGHT 64

// how many places the widgets can be visible in at once
#define MAX_SHOWINGS 6

#define FRAMES_PER_SECOND 25

#define LEVEL_BARS 4

// how many slices the loop of the looper widget is drawn in, round the circle
#define LOOP_SLICES 96

#define STARS 48

// Bricks, the game
#define BRICK_COLS 15
#define BRICK_ROWS 5
#define BRICK_W    7
#define BRICK_H    4
#define BRICK_LEFT 4     // (128 - 15 bricks of 7 - 14 gaps of 1) / 2
#define BRICK_TOP  9
#define PADDLE_W   16
#define PADDLE_H   2
#define BALL       2
#define HUD_H      6     // score and balls left, above a dotted line
#define GAME_BALLS 3
// MOVE is an integer, so the HMI turns it one step per encoder detent: a few pixels each, not a fraction of one
#define GAME_MOVE_STEPS 30

typedef struct {
    bool started, over;
    bool waiting;                         // the ball sits on the paddle until FIRE
    int score, balls, level, combo, left;
    float paddle_x;
    float ball_x, ball_y, ball_vx, ball_vy;
    uint8_t bricks[BRICK_ROWS][BRICK_COLS]; // hits left: 0 gone, 1, or 2 for a tough one

    // What the display shows, so that only the differences go out. A full redraw is the exception: when the
    // widget comes into view, and when the whole picture changes (a new level, the end of the game).
    bool full;
    int shown_ball_x, shown_ball_y, shown_paddle_x, shown_score, shown_balls;
    uint8_t changed_count;
    uint8_t changed[BRICK_ROWS * BRICK_COLS][2];
} Game;

typedef enum {
    PARAMETER_SPEED = 0,
    PARAMETER_FIRE,     // the game: launches the ball on every flip of a plain switch, meant for a footswitch
    PARAMETER_MOVE,     // the game: where the paddle is, 0 left .. GAME_MOVE_STEPS right, meant for a knob
    PARAMETER_COUNT
} Parameters;

// the widgets declared in the TTL, by number of columns
typedef enum {
    WIDGET_NONE = 0,
    WIDGET_LEVEL,   // 1 column: bars bouncing like a level meter, with a falling peak
    WIDGET_WAVE,    // 2 columns: an audio waveform scrolling to the left, notes plucked at random
    WIDGET_BOUNCE,  // 3 columns: a ball bouncing on the bottom of the area, with a short trail
    WIDGET_FILL,    // 1, 2 and 3 columns: every pixel of the area lit, to see exactly how much room there is
    WIDGET_LOOP,    // 1 column: a recorded loop drawn round a circle, turning under a fixed playhead
    WIDGET_STARS,   // full screen: stars flying towards you, inside a frame on the very edge of the display
    WIDGET_GAME,    // full screen: Bricks, a ball-and-paddle game drawn with partial updates
} WidgetKind;

typedef struct {
    // set by widget_shown / widget_hidden on the host thread, read by run()
    ALABS_HMI_Widget widget;
    WidgetKind kind;
    uint16_t width, height;

    // animation state, owned by run()
    float rotation;
    float level[LEVEL_BARS], peak[LEVEL_BARS];
    // the waveform, one amplitude per pixel column, oldest at `head`
    uint8_t history[MAX_WIDTH];
    uint8_t head;
    float envelope;
    float position;
    int countdown;
    float ball_x, ball_y, ball_vx, ball_vy;
    int16_t trail_x[4], trail_y[4];
    // for widgets that never change: drawn once per showing
    bool drawn;
    // the starfield: positions relative to the centre, depth 0 (far) .. 1 (here)
    float star_x[STARS], star_y[STARS], star_z[STARS];
    // a full screen frame is a lot of bytes: draw every other frame
    uint32_t frame;
} Showing;

typedef struct {
    float* ports[PARAMETER_COUNT];

    LV2_URID_Map* map;
    LV2_Log_Logger logger;
    ALABS_HMI_WidgetDraw* hmi;

    Showing showings[MAX_SHOWINGS];

    uint32_t frame_samples;
    uint32_t sample_counter;
    uint32_t random;

    // the loop the looper widget shows, made up once: peak per slice, 0..1
    float loop[LOOP_SLICES];

    // the game, one for the plugin: it only moves on while some showing of it is in view
    Game game;
    float last_fire;
    bool fire_pending;
    // the first showing of the game this frame, the one that moves it on
    Showing* game_showing;

    uint8_t bitmap[MAX_HEIGHT * ((MAX_WIDTH + 7) / 8)];
} Plugin;

static float random_unit(Plugin* self);

static LV2_Handle
instantiate(const LV2_Descriptor*     descriptor,
            double                    rate,
            const char*               bundle_path,
            const LV2_Feature* const* features)
{
    Plugin* self = (Plugin*)calloc(1, sizeof(Plugin));

    // clang-format off
    const char* missing = lv2_features_query(
            features,
            LV2_LOG__log,           &self->logger.log, false,
            LV2_URID__map,          &self->map,        true,
            ALABS_HMI__WidgetDraw,  &self->hmi,        false,
            NULL);
    // clang-format on

    lv2_log_logger_set_map(&self->logger, self->map);

    if (missing) {
        lv2_log_error(&self->logger, "Missing feature <%s>\n", missing);
        free(self);
        return NULL;
    }

    // no host drawing support: the plugin still runs, it just draws nothing
    if (self->hmi != NULL && self->hmi->size < ALABS_HMI_WIDGETDRAW_SIZE_BASE)
        self->hmi = NULL;

    self->frame_samples = (uint32_t)(rate / FRAMES_PER_SECOND);
    self->random = 0x1234567;

    // a loop of a few plucked notes, each a sharp attack decaying away, like the waveform widget
    float envelope = 0.f;
    for (int i = 0; i < LOOP_SLICES; ++i) {
        if (i % 24 == 0)
            envelope = 0.7f + 0.3f * random_unit(self);
        self->loop[i] = envelope * (0.75f + 0.25f * random_unit(self));
        envelope *= 0.93f;
    }

    return (LV2_Handle)self;
}

static void
connect_port(LV2_Handle instance, uint32_t port, void* data)
{
    Plugin* self = (Plugin*)instance;

    if (port < PARAMETER_COUNT)
        self->ports[port] = (float*)data;
}

static void
activate(LV2_Handle instance)
{
    Plugin* self = (Plugin*)instance;

    self->sample_counter = 0;
}

// --------------------------------------------------------------------------------------------------------------------
// drawing helpers, on a bitmap in the layout draw_bitmap takes

static float
random_unit(Plugin* self)
{
    // xorshift, good enough to make bars dance
    self->random ^= self->random << 13;
    self->random ^= self->random >> 17;
    self->random ^= self->random << 5;
    return (self->random & 0xffff) / 65535.f;
}

static void
set_pixel(uint8_t* bitmap, uint16_t stride, uint16_t width, uint16_t height, int x, int y)
{
    if (x < 0 || y < 0 || x >= width || y >= height)
        return;
    bitmap[y * stride + (x >> 3)] |= 0x80 >> (x & 7);
}

static void
fill_rect(uint8_t* bitmap, uint16_t stride, uint16_t width, uint16_t height, int x, int y, int w, int h)
{
    for (int j = y; j < y + h; ++j)
        for (int i = x; i < x + w; ++i)
            set_pixel(bitmap, stride, width, height, i, j);
}

static void
vline(uint8_t* bitmap, uint16_t stride, uint16_t width, uint16_t height, int x, int y0, int y1)
{
    if (y0 > y1) {
        const int t = y0;
        y0 = y1;
        y1 = t;
    }
    for (int y = y0; y <= y1; ++y)
        set_pixel(bitmap, stride, width, height, x, y);
}

// --------------------------------------------------------------------------------------------------------------------
// the three widgets

static void
draw_level(Plugin* self, Showing* s, float speed, uint16_t stride, uint16_t width, uint16_t height)
{
    const int gap = 2;
    const int bar_w = (width - gap * (LEVEL_BARS + 1)) / LEVEL_BARS;

    for (int b = 0; b < LEVEL_BARS; ++b) {
        // chase a new random target, faster up than down like a meter
        const float target = random_unit(self);
        const float k = target > s->level[b] ? 0.5f : 0.15f;
        s->level[b] += (target - s->level[b]) * k * speed;
        if (s->level[b] < 0.f) s->level[b] = 0.f;
        if (s->level[b] > 1.f) s->level[b] = 1.f;

        if (s->level[b] >= s->peak[b])
            s->peak[b] = s->level[b];
        else
            s->peak[b] -= 0.02f * speed;

        const int x = gap + b * (bar_w + gap);
        const int bar_h = (int)(s->level[b] * (height - 4));
        const int peak_y = height - 2 - (int)(s->peak[b] * (height - 4));

        fill_rect(self->bitmap, stride, width, height, x, height - 1 - bar_h, bar_w, bar_h);
        fill_rect(self->bitmap, stride, width, height, x, peak_y, bar_w, 1);
    }
}

// what the next column of the waveform holds: notes start at random, each a sharp attack decaying
// away, with a fast random flutter on top like the peaks of real audio
static float
next_wave_amplitude(Plugin* self, Showing* s)
{
    if (--s->countdown <= 0) {
        s->envelope = 0.55f + 0.45f * random_unit(self);
        s->countdown = 12 + (int)(random_unit(self) * 40.f);
    }

    const float amp = s->envelope * (0.55f + 0.45f * random_unit(self));
    s->envelope *= 0.93f;

    // the noise floor never quite goes away
    return amp > 0.04f ? amp : 0.04f * random_unit(self);
}

static void
draw_wave(Plugin* self, Showing* s, float speed, uint16_t stride, uint16_t width, uint16_t height)
{
    // whole columns scroll by, speed sets how many per frame
    s->position += 2.f * speed;
    while (s->position >= 1.f) {
        s->position -= 1.f;
        s->history[s->head] = (uint8_t)(next_wave_amplitude(self, s) * 255.f);
        s->head = (s->head + 1) % width;
    }

    const int mid = (height - 1) / 2;
    const int half_max = mid - 1;

    // oldest on the left, so the newest audio comes in from the right edge
    for (int x = 0; x < width; ++x) {
        const int half = s->history[(s->head + x) % width] * half_max / 255;
        vline(self->bitmap, stride, width, height, x, mid - half, mid + half);
    }
}

static void
draw_bounce(Plugin* self, Showing* s, float speed, uint16_t stride, uint16_t width, uint16_t height)
{
    const int ball = 4;
    // the bottom of the area is the floor
    const float floor_y = height - ball;

    if (s->ball_vx == 0.f) {
        // first frame of this showing
        s->ball_x = 1.f;
        s->ball_y = 1.f;
        s->ball_vx = 1.6f;
        s->ball_vy = 0.f;
    }

    for (int i = 3; i > 0; --i) {
        s->trail_x[i] = s->trail_x[i - 1];
        s->trail_y[i] = s->trail_y[i - 1];
    }
    s->trail_x[0] = (int16_t)s->ball_x;
    s->trail_y[0] = (int16_t)s->ball_y;

    s->ball_vy += 0.35f * speed;
    s->ball_x += s->ball_vx * speed;
    s->ball_y += s->ball_vy * speed;

    if (s->ball_y >= floor_y) {
        s->ball_y = floor_y;
        // lose some energy, kick it again once it is almost still
        s->ball_vy = -s->ball_vy * 0.8f;
        if (s->ball_vy > -1.5f)
            s->ball_vy = -4.5f;
    }
    if (s->ball_x <= 0.f || s->ball_x >= width - ball) {
        s->ball_vx = -s->ball_vx;
        s->ball_x = s->ball_x <= 0.f ? 0.f : width - ball;
    }

    // trail, a dot where the ball was
    for (int i = 1; i < 4; ++i)
        set_pixel(self->bitmap, stride, width, height, s->trail_x[i] + ball / 2, s->trail_y[i] + ball / 2);

    fill_rect(self->bitmap, stride, width, height, (int)s->ball_x, (int)s->ball_y, ball, ball);
}

static void
line(uint8_t* bitmap, uint16_t stride, uint16_t width, uint16_t height, int x0, int y0, int x1, int y1)
{
    const int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    const int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    const int sx = x0 < x1 ? 1 : -1;
    const int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy;

    for (;;) {
        set_pixel(bitmap, stride, width, height, x0, y0);
        if (x0 == x1 && y0 == y1)
            break;
        const int e2 = err * 2;
        if (e2 > -dy) {
            err -= dy;
            x0 += sx;
        }
        if (e2 < dx) {
            err += dx;
            y0 += sy;
        }
    }
}

// The loop drawn as spokes round a circle, longer where it is louder, turning clockwise under a playhead
// that stays at twelve o'clock: what passes under it is what is playing. The spokes are dense enough to
// touch at the rim, so the loop reads as a solid ring rather than teeth. The part already played in this
// turn is left as just its outline, the tips of its spokes, so the turn shows how far it has got.
static void
draw_loop(Plugin* self, Showing* s, float speed, uint16_t stride, uint16_t width, uint16_t height)
{
    // one turn every 4 seconds at speed 1
    s->rotation += speed * 2.f * (float)M_PI / (4.f * FRAMES_PER_SECOND);
    if (s->rotation >= 2.f * (float)M_PI)
        s->rotation -= 2.f * (float)M_PI;

    const float cx = (width - 1) / 2.f;
    const float cy = (height - 1) / 2.f;
    const float radius = (height < width ? height : width) / 2.f - 0.5f;
    const float inner = radius * 0.3f;

    // the slices from here on have gone past the playhead in this turn
    const int played_from = LOOP_SLICES - (int)(s->rotation / (2.f * (float)M_PI) * LOOP_SLICES);

    for (int i = 0; i < LOOP_SLICES; ++i) {
        // 0 is straight up, angles grow clockwise on the display (y goes down)
        const float angle = s->rotation + 2.f * (float)M_PI * i / LOOP_SLICES;
        const float c = sinf(angle), d = -cosf(angle);
        const float outer = inner + self->loop[i] * (radius - inner);
        const int ox = (int)(cx + c * outer + 0.5f), oy = (int)(cy + d * outer + 0.5f);

        if (i >= played_from)
            set_pixel(self->bitmap, stride, width, height, ox, oy);
        else
            line(self->bitmap, stride, width, height,
                 (int)(cx + c * inner + 0.5f), (int)(cy + d * inner + 0.5f), ox, oy);
    }

    // the playhead, from the centre up to the top edge
    const int px = (int)(cx + 0.5f);
    line(self->bitmap, stride, width, height, px, (int)(cy + 0.5f), px, 0);
}

static void
new_star(Plugin* self, Showing* s, int i)
{
    s->star_x[i] = random_unit(self) * 2.f - 1.f;
    s->star_y[i] = random_unit(self) * 2.f - 1.f;
    s->star_z[i] = random_unit(self) * 0.2f;
}

static void
draw_stars(Plugin* self, Showing* s, float speed, uint16_t stride, uint16_t width, uint16_t height)
{
    const float cx = (width - 1) / 2.f;
    const float cy = (height - 1) / 2.f;

    // a frame on the outermost pixels, to see the whole display is ours
    for (int x = 0; x < width; ++x) {
        set_pixel(self->bitmap, stride, width, height, x, 0);
        set_pixel(self->bitmap, stride, width, height, x, height - 1);
    }
    for (int y = 0; y < height; ++y) {
        set_pixel(self->bitmap, stride, width, height, 0, y);
        set_pixel(self->bitmap, stride, width, height, width - 1, y);
    }

    for (int i = 0; i < STARS; ++i) {
        if (s->star_z[i] <= 0.f)
            new_star(self, s, i);

        // perspective: the closer, the further from the centre
        const float prev_depth = 1.05f - s->star_z[i];
        const int px = (int)(cx + s->star_x[i] * cx * 0.25f / prev_depth);
        const int py = (int)(cy + s->star_y[i] * cy * 0.25f / prev_depth);

        s->star_z[i] += 0.03f * speed * (0.5f + s->star_z[i]);

        const float depth = 1.05f - s->star_z[i];
        const int x = (int)(cx + s->star_x[i] * cx * 0.25f / depth);
        const int y = (int)(cy + s->star_y[i] * cy * 0.25f / depth);

        // the star size; nothing may touch the frame on the outermost pixels
        const int size = s->star_z[i] > 0.75f ? 3 : s->star_z[i] > 0.45f ? 2 : 1;

        if (s->star_z[i] >= 1.f || x <= 0 || y <= 0 || x + size >= width - 1 || y + size >= height - 1) {
            new_star(self, s, i);
            continue;
        }

        // past halfway it leaves a streak from where it was, so the motion shows even at half the frame rate
        if (s->star_z[i] > 0.35f)
            line(self->bitmap, stride, width, height, px, py, x, y);

        // and it grows as it comes closer
        fill_rect(self->bitmap, stride, width, height, x, y, size, size);
    }
}

// --------------------------------------------------------------------------------------------------------------------
// Bricks: a ball-and-paddle game, and a demo of partial updates. The whole display goes out only when the widget
// comes into view or the picture changes as a whole; on every other frame just what moved is sent: the ball,
// cleared where it was and drawn where it is, the paddle strip, the bricks hit, the score. A few dozen bytes per
// frame instead of the kilobyte of a full screen, so it runs at the full 25 frames per second.
//
// Where the ball hits the paddle sets the angle it leaves at, every paddle hit makes it a little faster, and bricks
// hit in a row without touching the paddle score more and more. Tough bricks, drawn hollow, take two hits.

// a 3x5 font for the few characters the game writes, one byte per row, the low 3 bits left to right
static const uint8_t* glyph(char c)
{
    static const uint8_t digits[10][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 }, { 7, 1, 7, 4, 7 }, { 7, 1, 3, 1, 7 }, { 5, 5, 7, 1, 1 },
        { 7, 4, 7, 1, 7 }, { 7, 4, 7, 5, 7 }, { 7, 1, 1, 2, 2 }, { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
    };
    static const uint8_t letters[][5] = {
        { 7, 4, 5, 5, 7 }, // G
        { 2, 5, 7, 5, 5 }, // A
        { 5, 7, 7, 5, 5 }, // M
        { 7, 4, 6, 4, 7 }, // E
        { 7, 5, 5, 5, 7 }, // O
        { 5, 5, 5, 5, 2 }, // V
        { 6, 5, 6, 5, 5 }, // R
        { 7, 4, 7, 1, 7 }, // S
        { 7, 4, 4, 4, 7 }, // C
        { 6, 5, 5, 5, 6 }, // D
        { 5, 5, 7, 5, 5 }, // H
        { 5, 5, 5, 5, 7 }, // U
        { 6, 5, 5, 5, 5 }, // N
        { 7, 2, 2, 2, 2 }, // T
        { 7, 5, 7, 4, 4 }, // P
        { 4, 4, 4, 4, 7 }, // L
        { 7, 4, 6, 4, 4 }, // F
        { 7, 2, 2, 2, 7 }, // I
        { 5, 5, 2, 2, 2 }, // Y
        { 5, 5, 7, 7, 5 }, // W
        { 6, 5, 6, 5, 6 }, // B
        { 5, 5, 6, 5, 5 }, // K
    };
    static const char order[] = "GAMEOVRSCDHUNTPLFIYWBK";

    if (c >= '0' && c <= '9')
        return digits[c - '0'];
    for (int i = 0; order[i]; ++i)
        if (order[i] == c)
            return letters[i];
    return NULL;
}

static void
text(uint8_t* bitmap, uint16_t stride, uint16_t width, uint16_t height, int x, int y, const char* str)
{
    for (; *str; ++str, x += 4) {
        const uint8_t* g = glyph(*str);
        if (g == NULL)
            continue;
        for (int row = 0; row < 5; ++row)
            for (int col = 0; col < 3; ++col)
                if (g[row] & (4 >> col))
                    set_pixel(bitmap, stride, width, height, x + col, y + row);
    }
}

#define PADDLE_Y (64 - 4)

static float
ball_speed(const Game* g)
{
    return 1.6f + 0.2f * (g->level - 1);
}

static void
game_new_level(Plugin* self, Game* g)
{
    // the first level spells it out, one brick per pixel of letters 5 bricks tall
    static const char* const mod[BRICK_ROWS] = {
        "#...#..##..###.",
        "##.##.#..#.#..#",
        "#.#.#.#..#.#..#",
        "#...#.#..#.#..#",
        "#...#..##..###.",
    };

    g->left = 0;
    for (int r = 0; r < BRICK_ROWS; ++r) {
        for (int c = 0; c < BRICK_COLS; ++c) {
            uint8_t hits = 1;
            switch ((g->level - 1) % 5) {
            case 0: hits = mod[r][c] == '#' ? 1 : 0; break;           // M O D
            case 2: hits = r == 0 ? 2 : 1; break;                     // a tough roof
            case 3: hits = (r + c) % 2 ? 2 : 1; break;                // a checkerboard
            case 4: hits = random_unit(self) < 0.15f ? 0             // holes and tough ones at random
                         : random_unit(self) < 0.3f ? 2 : 1; break;
            }
            g->bricks[r][c] = hits;
            if (hits)
                ++g->left;
        }
    }
    g->waiting = true;
    g->combo = 0;
    g->full = true;
}

static void
game_start(Plugin* self, Game* g)
{
    memset(g, 0, sizeof(Game));
    g->started = true;
    g->balls = GAME_BALLS;
    g->level = 1;
    game_new_level(self, g);
}

static void
game_hit_brick(Game* g, int r, int c)
{
    if (--g->bricks[r][c] == 0) {
        --g->left;
        g->score += 10 * (1 + g->combo);
        ++g->combo;
    }
    g->changed[g->changed_count][0] = r;
    g->changed[g->changed_count][1] = c;
    ++g->changed_count;
}

// the brick at a point, if any, as row and column
static bool
brick_at(const Game* g, float x, float y, int* r, int* c)
{
    const int col = (int)(x - BRICK_LEFT) / (BRICK_W + 1);
    const int row = (int)(y - BRICK_TOP) / (BRICK_H + 1);
    if (x < BRICK_LEFT || y < BRICK_TOP || col >= BRICK_COLS || row >= BRICK_ROWS)
        return false;
    // the gaps between bricks are not bricks
    if ((int)(x - BRICK_LEFT) % (BRICK_W + 1) == BRICK_W || (int)(y - BRICK_TOP) % (BRICK_H + 1) == BRICK_H)
        return false;
    if (g->bricks[row][col] == 0)
        return false;
    *r = row;
    *c = col;
    return true;
}

// whether the ball at x, y would touch a brick, and which
static bool
ball_hits_brick(const Game* g, float x, float y, int* r, int* c)
{
    return brick_at(g, x, y, r, c) || brick_at(g, x + BALL - 1, y, r, c) ||
           brick_at(g, x, y + BALL - 1, r, c) || brick_at(g, x + BALL - 1, y + BALL - 1, r, c);
}

static void
game_tick(Plugin* self, Game* g, uint16_t width, uint16_t height)
{
    bool fire = self->fire_pending;
    self->fire_pending = false;

    if (!g->started || g->over) {
        if (!fire)
            return;
        // carry on into this frame, so that paddle and ball are in place before anything is drawn; the
        // press that started the game does not launch the ball too
        game_start(self, g);
        fire = false;
    }

    g->paddle_x = *self->ports[PARAMETER_MOVE] / GAME_MOVE_STEPS * (width - PADDLE_W);

    if (g->waiting) {
        g->ball_x = g->paddle_x + PADDLE_W / 2 - 1;
        g->ball_y = PADDLE_Y - BALL;
        if (fire) {
            const float speed = ball_speed(g);
            g->ball_vx = (random_unit(self) - 0.5f) * speed;
            g->ball_vy = -speed;
            g->waiting = false;
        }
        return;
    }

    // in small steps, so that a fast ball cannot go through a brick
    for (int step = 0; step < 2; ++step) {
        const float dx = g->ball_vx / 2, dy = g->ball_vy / 2;
        float nx = g->ball_x + dx, ny = g->ball_y + dy;
        int r, c;

        if (nx < 0.f) {
            nx = 0.f;
            g->ball_vx = -g->ball_vx;
        } else if (nx > width - BALL) {
            nx = width - BALL;
            g->ball_vx = -g->ball_vx;
        }
        if (ny < HUD_H + 1) {
            ny = HUD_H + 1;
            g->ball_vy = -g->ball_vy;
        }

        if (ball_hits_brick(g, nx, ny, &r, &c)) {
            // bounce off the side it came from, and stay where it was: the ball never overlaps a brick
            if (ball_hits_brick(g, nx, g->ball_y, &r, &c))
                g->ball_vx = -g->ball_vx;
            else
                g->ball_vy = -g->ball_vy;
            game_hit_brick(g, r, c);
            continue;
        }

        // the paddle: where it is hit sets the angle, up to 60 degrees either side, and the ball speeds up a bit
        if (g->ball_vy > 0.f && ny + BALL >= PADDLE_Y && g->ball_y + BALL <= PADDLE_Y &&
            nx + BALL > g->paddle_x && nx < g->paddle_x + PADDLE_W) {
            float rel = (nx + BALL / 2.f - (g->paddle_x + PADDLE_W / 2.f)) / (PADDLE_W / 2.f);
            if (rel < -1.f) rel = -1.f;
            if (rel > 1.f) rel = 1.f;
            float speed = sqrtf(g->ball_vx * g->ball_vx + g->ball_vy * g->ball_vy) * 1.03f;
            if (speed > 4.f)
                speed = 4.f;
            g->ball_vx = speed * sinf(rel * 1.05f);
            g->ball_vy = -speed * cosf(rel * 1.05f);
            ny = PADDLE_Y - BALL;
            g->combo = 0;
        }

        g->ball_x = nx;
        g->ball_y = ny;

        // missed it
        if (g->ball_y >= height) {
            if (--g->balls <= 0) {
                g->over = true;
                g->full = true;
            } else {
                g->waiting = true;
                g->combo = 0;
            }
            return;
        }
    }

    if (g->left == 0) {
        ++g->level;
        g->score += 100;
        game_new_level(self, g);
    }
}

// a brick as it should look: solid, or hollow when it still takes two hits
static void
brick_bitmap(uint8_t* bitmap, uint8_t hits)
{
    const uint16_t stride = (BRICK_W + 7) / 8;
    memset(bitmap, 0, stride * BRICK_H);
    for (int y = 0; y < BRICK_H; ++y)
        for (int x = 0; x < BRICK_W; ++x)
            if (hits == 1 || y == 0 || y == BRICK_H - 1 || x == 0 || x == BRICK_W - 1)
                set_pixel(bitmap, stride, BRICK_W, BRICK_H, x, y);
}

static void
hud_bitmap(uint8_t* bitmap, const Game* g, uint16_t width)
{
    const uint16_t stride = (width + 7) / 8;
    char str[16];
    memset(bitmap, 0, stride * HUD_H);
    snprintf(str, sizeof(str), "%d", g->score);
    text(bitmap, stride, width, HUD_H, 1, 0, str);
    for (int i = 0; i < g->balls; ++i)
        fill_rect(bitmap, stride, width, HUD_H, width - 4 - i * 5, 1, 3, 3);
}

static void
paddle_bitmap(uint8_t* bitmap, const Game* g, uint16_t width)
{
    const uint16_t stride = (width + 7) / 8;
    memset(bitmap, 0, stride * PADDLE_H);
    fill_rect(bitmap, stride, width, PADDLE_H, (int)g->paddle_x, 0, PADDLE_W, PADDLE_H);
}

static void
game_draw_full(Plugin* self, const Game* g, uint16_t stride, uint16_t width, uint16_t height)
{
    if (!g->started) {
        text(self->bitmap, stride, width, height, width / 2 - 10, height / 2 - 8, "BRICKS");
        text(self->bitmap, stride, width, height, width / 2 - 22, height / 2 + 4, "FIRE TO PLAY");
        return;
    }
    if (g->over) {
        char str[24];
        text(self->bitmap, stride, width, height, width / 2 - 17, height / 2 - 8, "GAME OVER");
        snprintf(str, sizeof(str), "SCORE %d", g->score);
        text(self->bitmap, stride, width, height, width / 2 - 2 * (int)strlen(str), height / 2 + 2, str);
        return;
    }

    uint8_t brick[((BRICK_W + 7) / 8) * BRICK_H];
    for (int r = 0; r < BRICK_ROWS; ++r) {
        for (int c = 0; c < BRICK_COLS; ++c) {
            if (g->bricks[r][c] == 0)
                continue;
            brick_bitmap(brick, g->bricks[r][c]);
            const int bx = BRICK_LEFT + c * (BRICK_W + 1), by = BRICK_TOP + r * (BRICK_H + 1);
            for (int y = 0; y < BRICK_H; ++y)
                for (int x = 0; x < BRICK_W; ++x)
                    if (brick[y * ((BRICK_W + 7) / 8) + (x >> 3)] & (0x80 >> (x & 7)))
                        set_pixel(self->bitmap, stride, width, height, bx + x, by + y);
        }
    }

    uint8_t hud[16 * HUD_H];
    hud_bitmap(hud, g, width);
    memcpy(self->bitmap, hud, stride * HUD_H);
    for (int x = 0; x < width; x += 2)
        set_pixel(self->bitmap, stride, width, height, x, HUD_H);

    fill_rect(self->bitmap, stride, width, height, (int)g->paddle_x, PADDLE_Y, PADDLE_W, PADDLE_H);
    fill_rect(self->bitmap, stride, width, height, (int)g->ball_x, (int)g->ball_y, BALL, BALL);
}

static void
draw_game(Plugin* self, Showing* s, uint16_t stride, uint16_t width, uint16_t height)
{
    Game* g = &self->game;
    ALABS_HMI_WidgetDraw* hmi = self->hmi;

    // everything, once: after widget_shown, and when the picture changes as a whole
    if (!s->drawn || g->full) {
        memset(self->bitmap, 0, stride * height);
        game_draw_full(self, g, stride, width, height);
        hmi->draw_bitmap(hmi->handle, s->widget, 0, 0, width, height, self->bitmap);
        s->drawn = true;
        g->full = false;
        g->changed_count = 0;
        g->shown_ball_x = (int)g->ball_x;
        g->shown_ball_y = (int)g->ball_y;
        g->shown_paddle_x = (int)g->paddle_x;
        g->shown_score = g->score;
        g->shown_balls = g->balls;
        return;
    }

    if (!g->started || g->over)
        return;

    // the bricks hit: gone ones cleared without sending any pixel, damaged ones drawn again
    for (int i = 0; i < g->changed_count; ++i) {
        const int r = g->changed[i][0], c = g->changed[i][1];
        const int bx = BRICK_LEFT + c * (BRICK_W + 1), by = BRICK_TOP + r * (BRICK_H + 1);
        if (g->bricks[r][c] == 0) {
            hmi->draw_bitmap(hmi->handle, s->widget, bx, by, BRICK_W, BRICK_H, NULL);
        } else {
            uint8_t brick[((BRICK_W + 7) / 8) * BRICK_H];
            brick_bitmap(brick, g->bricks[r][c]);
            hmi->draw_bitmap(hmi->handle, s->widget, bx, by, BRICK_W, BRICK_H, brick);
        }
    }
    g->changed_count = 0;

    if (g->score != g->shown_score || g->balls != g->shown_balls) {
        uint8_t hud[16 * HUD_H];
        hud_bitmap(hud, g, width);
        hmi->draw_bitmap(hmi->handle, s->widget, 0, 0, width, HUD_H, hud);
        g->shown_score = g->score;
        g->shown_balls = g->balls;
    }

    const int bx = (int)g->ball_x, by = (int)g->ball_y;
    const bool ball_moved = bx != g->shown_ball_x || by != g->shown_ball_y;

    // the ball where it was, cleared without any pixel data
    if (ball_moved)
        hmi->draw_bitmap(hmi->handle, s->widget, g->shown_ball_x, g->shown_ball_y, BALL, BALL, NULL);

    // the paddle strip when the paddle moved, or when the ball went through it and may have cut it
    const bool ball_in_strip = (by + BALL > PADDLE_Y && by < PADDLE_Y + PADDLE_H) ||
                               (g->shown_ball_y + BALL > PADDLE_Y && g->shown_ball_y < PADDLE_Y + PADDLE_H);
    if ((int)g->paddle_x != g->shown_paddle_x || (ball_moved && ball_in_strip)) {
        uint8_t strip[16 * PADDLE_H];
        paddle_bitmap(strip, g, width);
        hmi->draw_bitmap(hmi->handle, s->widget, 0, PADDLE_Y, width, PADDLE_H, strip);
        g->shown_paddle_x = (int)g->paddle_x;
    }

    // and where it is now, if it is still on the display
    if (ball_moved && by < height) {
        static const uint8_t ball[BALL] = { 0xc0, 0xc0 };
        hmi->draw_bitmap(hmi->handle, s->widget, bx, by, BALL, BALL, ball);
    }
    g->shown_ball_x = bx;
    g->shown_ball_y = by;
}

static void
draw_showing(Plugin* self, Showing* s, float speed)
{
    const uint16_t width = s->width < MAX_WIDTH ? s->width : MAX_WIDTH;
    const uint16_t height = s->height < MAX_HEIGHT ? s->height : MAX_HEIGHT;
    const uint16_t stride = (width + 7) / 8;

    // a still picture goes out once after widget_shown and costs nothing afterwards
    if (s->kind == WIDGET_FILL) {
        if (!s->drawn) {
            memset(self->bitmap, 0xff, stride * height);
            self->hmi->draw_bitmap(self->hmi->handle, s->widget, 0, 0, width, height, self->bitmap);
            s->drawn = true;
        }
        return;
    }

    // a full screen frame is 1 KB on the wire, half the frame rate is plenty
    if (s->kind == WIDGET_STARS && (s->frame++ & 1))
        return;

    // the game moves on once per frame, and only the place that moves it draws it, with partial updates
    if (s->kind == WIDGET_GAME) {
        if (s == self->game_showing) {
            game_tick(self, &self->game, width, height);
            draw_game(self, s, stride, width, height);
        }
        return;
    }

    memset(self->bitmap, 0, stride * height);

    switch (s->kind) {
    case WIDGET_LEVEL:
        draw_level(self, s, speed, stride, width, height);
        break;
    case WIDGET_WAVE:
        draw_wave(self, s, speed, stride, width, height);
        break;
    case WIDGET_BOUNCE:
        draw_bounce(self, s, speed, stride, width, height);
        break;
    case WIDGET_LOOP:
        draw_loop(self, s, speed, stride, width, height);
        break;
    case WIDGET_STARS:
        draw_stars(self, s, speed * 2.f, stride, width, height);
        break;
    case WIDGET_GAME:
        break;
    case WIDGET_FILL:
    case WIDGET_NONE:
        return;
    }

    // the whole area every frame: simple, and always right after widget_shown
    self->hmi->draw_bitmap(self->hmi->handle, s->widget, 0, 0, width, height, self->bitmap);
}

static void
run(LV2_Handle instance, uint32_t n_samples)
{
    Plugin* self = (Plugin*)instance;

    if (self->hmi == NULL)
        return;

    // a plain switch: every press flips it, so a change either way is a shot; caught on every cycle,
    // the game reads it on its next frame
    const float fire = *self->ports[PARAMETER_FIRE];
    if ((fire > 0.5f) != (self->last_fire > 0.5f))
        self->fire_pending = true;
    self->last_fire = fire;

    self->sample_counter += n_samples;

    if (self->sample_counter < self->frame_samples)
        return;

    self->sample_counter = 0;

    const float speed = *self->ports[PARAMETER_SPEED];

    self->game_showing = NULL;
    for (int i = 0; i < MAX_SHOWINGS; ++i) {
        Showing* s = &self->showings[i];
        if (s->kind == WIDGET_GAME && __atomic_load_n(&s->widget, __ATOMIC_ACQUIRE) != NULL) {
            self->game_showing = s;
            break;
        }
    }

    for (int i = 0; i < MAX_SHOWINGS; ++i) {
        Showing* s = &self->showings[i];

        if (__atomic_load_n(&s->widget, __ATOMIC_ACQUIRE) != NULL)
            draw_showing(self, s, speed);
    }
}

static void
deactivate(LV2_Handle instance)
{
}

static void
cleanup(LV2_Handle instance)
{
    free(instance);
}

// --------------------------------------------------------------------------------------------------------------------
// ALABS_HMI_WidgetNotification

static void
widget_shown(LV2_Handle handle, const char* symbol, ALABS_HMI_Widget widget, const ALABS_HMI_WidgetArea* area)
{
    Plugin* self = (Plugin*)handle;

    // every widget here draws 1 bit per pixel, the only format the TTL declares; the host never offers
    // another, but a plugin should not draw in a format it does not know
    if (area->pixelFormat != ALABS_HMI_PixelFormat_Mono1)
        return;

    const uint16_t area_width = area->width;
    const uint16_t area_height = area->height;

    WidgetKind kind;
    if (!strcmp(symbol, "level"))
        kind = WIDGET_LEVEL;
    else if (!strcmp(symbol, "wave"))
        kind = WIDGET_WAVE;
    else if (!strcmp(symbol, "bounce"))
        kind = WIDGET_BOUNCE;
    else if (!strcmp(symbol, "fill1") || !strcmp(symbol, "fill2") || !strcmp(symbol, "fill3"))
        kind = WIDGET_FILL;
    else if (!strcmp(symbol, "loop"))
        kind = WIDGET_LOOP;
    else if (!strcmp(symbol, "stars"))
        kind = WIDGET_STARS;
    else if (!strcmp(symbol, "bricks"))
        kind = WIDGET_GAME;
    else
        return;

    for (int i = 0; i < MAX_SHOWINGS; ++i) {
        Showing* s = &self->showings[i];

        if (s->widget != NULL)
            continue;

        // a fresh animation for every showing; the handle goes in last, it is what run() looks at
        memset(s, 0, sizeof(Showing));
        s->kind = kind;
        s->width = area_width;
        s->height = area_height;
        __atomic_store_n(&s->widget, widget, __ATOMIC_RELEASE);
        return;
    }
}

static void
widget_hidden(LV2_Handle handle, ALABS_HMI_Widget widget)
{
    Plugin* self = (Plugin*)handle;

    for (int i = 0; i < MAX_SHOWINGS; ++i) {
        if (self->showings[i].widget == widget)
            __atomic_store_n(&self->showings[i].widget, NULL, __ATOMIC_RELEASE);
    }
}

static const void*
extension_data(const char* uri)
{
    static const ALABS_HMI_WidgetNotification widgetNotif = {
        sizeof(ALABS_HMI_WidgetNotification),
        widget_shown,
        widget_hidden,
    };
    if (!strcmp(uri, ALABS_HMI__WidgetNotification))
        return &widgetNotif;
    return NULL;
}

static const LV2_Descriptor descriptor = {
    PLUGIN_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    extension_data
};

LV2_SYMBOL_EXPORT
const LV2_Descriptor*
lv2_descriptor(uint32_t index)
{
    switch (index) {
        case 0:  return &descriptor;
        default: return NULL;
    }
}
