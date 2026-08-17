/** Slim menu grid launcher — 2x2 category picker using ML tab icons (cyan). */
#include "dryos.h"
#include "bmp.h"
#include "font.h"
#include "menu.h"
#include "menu-grid.h"
#include "slim-font.h"
#include "gui-common.h"
#include "config.h"
#include "lens.h"
#include "shoot.h"
#include "fps.h"

extern void lens_display_set_dirty(void);

#ifdef CONFIG_SLIM_MENUS

#define GRID_COLS       2
#define GRID_ROWS       2
#define GRID_COUNT      (GRID_COLS * GRID_ROWS)
/* One spacing value: left = mid = right = top = between = bottom. */
#define GRID_SPACE      40
#define GRID_RADIUS     36
#define GRID_SEL_BORDER 6
#define GRID_LABEL_PAD  16   /* baseline inset from bottom of each card */
#define GRID_ICON_GAP   14   /* clear space between icon area and label */

static int grid_active = 0;
static int grid_launched = 0;
/* Session-only: top-left on boot; remembered while camera stays on. */
static int grid_sel = 0;
static int quick_screen_active = 0;
static int quick_screen_feedback = -1;
static int quick_screen_touch_latched = 0;
static int white_card_wb_active = 0;
static int white_card_wb_capturing = 0;
static int white_card_wb_done = 0;
static int white_card_wb_draw_state = -1;
/* Bitmap VRAM must only be touched by the Live View rendering task. Input and
 * timer callbacks merely set these requests; drawing from those callbacks can
 * race Canon's display task and cause Err70 on EOS M. */
static volatile int white_card_wb_paint_pending = 0;
static volatile int white_card_wb_close_pending = 0;
/* Session-only: starts at White Balance after boot and is remembered. */
static int quick_screen_sel = 0;
/* Persisted Quick Panel preference: 0 = shutter angle, 1 = shutter speed. */
CONFIG_INT("menu.quick.shutter.speed", quick_screen_shutter_speed, 0);

#define QUICK_SCREEN_COLS  4
#define QUICK_SCREEN_ROWS  2
#define QUICK_SCREEN_COUNT (QUICK_SCREEN_COLS * QUICK_SCREEN_ROWS)
#define QUICK_SCREEN_CELL_W (720 / QUICK_SCREEN_COLS)
#define QUICK_SCREEN_TOUCH_HALF_W 74

static int quick_screen_option_enabled(int index);
static int quick_screen_next_enabled(int start, int direction);

typedef struct
{
    const char *value_menu;
    const char *value_entry;
    const char *adjust_menu;
    const char *adjust_entry;
} quick_screen_item_t;

/* Resolution displays the computed read-only value. Its arrows stay within
 * the Aspect Ratio currently selected beside it. */
static const quick_screen_item_t quick_screen_items[QUICK_SCREEN_COUNT] =
{
    { "Expo",  "White Balance", "Expo",  "White Balance"    },
    { "Movie", "Mode",          "Movie", "Mode"             },
    { "Movie", "Aspect Ratio",  "Movie", "Aspect Ratio"     },
    { "Movie", "Resolution",    "Movie", "Quick Resolution" },
    { "Movie", "Frame Rate",    "Movie", "Frame Rate"       },
    { "Expo",  "Shutter",       "Expo",  "Shutter"          },
    { "Expo",  "Aperture",      "Expo",  "Aperture"         },
    { "Expo",  "ISO",           "Expo",  "ISO"              },
};

static void quick_screen_feedback_clear(int timer, void *opaque)
{
    (void)timer;
    (void)opaque;
    quick_screen_feedback = -1;
    if (quick_screen_active)
        menu_redraw();
}

static void quick_screen_refresh(int timer, void *opaque)
{
    (void)timer;
    (void)opaque;
    if (quick_screen_active)
        menu_redraw();
}

typedef struct
{
    const char *label;
    const char *menu_name;
    int icon;   /* ICON_ML_* from baseline menu tab bar */
} grid_tile_t;

static void grid_fill_round_rect(int x, int y, int w, int h, int r, int color)
{
    if (w <= 0 || h <= 0) return;
    r = MIN(r, MIN(w, h) / 2);

    bmp_fill(color, x + r, y, w - 2 * r, h);
    bmp_fill(color, x, y + r, w, h - 2 * r);

    fill_circle(x + r, y + r, r, color);
    fill_circle(x + w - r - 1, y + r, r, color);
    fill_circle(x + r, y + h - r - 1, r, color);
    fill_circle(x + w - r - 1, y + h - r - 1, r, color);
}

/* Exposure = +/- Expo, Monitoring = Overlay waveform, Movie = camera, Settings = wrench. */
static const grid_tile_t grid_tiles[GRID_COUNT] =
{
    { "Exposure",   "Expo",     ICON_ML_EXPO    },
    { "Monitoring", "Overlay",  ICON_ML_OVERLAY },
    { "Movie",      "Movie",    ICON_ML_MOVIE   },
    { "Settings",   "Settings", ICON_ML_PREFS   },
};

static void grid_layout(int *ox, int *oy, int *cw, int *ch)
{
    *cw = (720 - 3 * GRID_SPACE) / GRID_COLS;
    *ch = (480 - 3 * GRID_SPACE) / GRID_ROWS;
    *ox = GRID_SPACE;
    *oy = GRID_SPACE;
}

static void grid_cell_rect(int idx, int *x, int *y, int *w, int *h)
{
    int ox, oy, cw, ch;
    grid_layout(&ox, &oy, &cw, &ch);
    int col = idx % GRID_COLS;
    int row = idx / GRID_COLS;
    *x = ox + col * (cw + GRID_SPACE);
    *y = oy + row * (ch + GRID_SPACE);
    *w = cw;
    *h = ch;
}

static void grid_draw_ml_icon(int icon, int cx, int cy)
{
    const int scale = 2;
    int iw = bfnt_char_get_width(icon) * scale;
    /* ML tab glyphs are ~40 px tall at 1x; 2x → ~80. */
    int ih = 40 * scale;
    int x = cx - iw / 2;
    int y = cy - ih / 2;
    bfnt_draw_char_scaled(icon, x, y, COLOR_CYAN, NO_BG_ERASE, scale);
}

int menu_grid_is_active(void)   { return grid_active; }
int menu_grid_is_launched(void) { return grid_launched; }

void menu_grid_open(void)
{
    grid_active = 1;
    grid_launched = 0;
    grid_sel = COERCE(grid_sel, 0, GRID_COUNT - 1);
}

void menu_grid_close(void)
{
    grid_active = 0;
    grid_launched = 0;
}

void menu_grid_return(void)
{
    grid_active = 1;
    grid_launched = 0;
    grid_sel = COERCE(grid_sel, 0, GRID_COUNT - 1);
}

void menu_grid_enter_launched(void)
{
    grid_active = 0;
    grid_launched = 1;
}

static void menu_grid_launch(int idx)
{
    if (idx < 0 || idx >= GRID_COUNT) return;

    select_menu_by_name((char *) grid_tiles[idx].menu_name, 0);
    menu_select_first_entry((char *) grid_tiles[idx].menu_name);
    grid_active = 0;
    grid_launched = 1;
    grid_sel = idx;
}

int menu_grid_handle_touch(int x, int y)
{
    if (!grid_active)
        return 1;

    for (int i = 0; i < GRID_COUNT; i++)
    {
        int tx, ty, tw, th;
        grid_cell_rect(i, &tx, &ty, &tw, &th);
        if (x >= tx && x < tx + tw && y >= ty && y < ty + th)
        {
            grid_sel = i;
            /* Launch directly; avoid a full-screen grid redraw here, which
             * produces a visible black flash immediately before the menu. */
            menu_grid_launch(i);
            menu_redraw();
            return 0;
        }
    }

    return 1;
}

int menu_quick_screen_is_active(void) { return quick_screen_active; }

void menu_quick_screen_open(void)
{
    quick_screen_active = 1;
    quick_screen_feedback = -1;
    quick_screen_touch_latched = 0;
    /* Do not query menu entries here: this runs before menu_open owns the
     * screen and doing so can race Canon's GUI task (Err70). */
    quick_screen_sel = COERCE(
        quick_screen_sel, 0, QUICK_SCREEN_COUNT - 1);
}

void menu_quick_screen_close(void)
{
    quick_screen_active = 0;
    quick_screen_feedback = -1;
    quick_screen_touch_latched = 0;
}

int menu_white_card_wb_is_active(void)
{
    return white_card_wb_active;
}

static void white_card_wb_clear_overlay(void)
{
    /* Include a generous glyph margin so no antialiasing/shadow pixels from
     * the previous frame remain after dismissing the overlay. */
    bmp_fill(COLOR_EMPTY, 100, 194, 520, 196);
}

void menu_white_card_wb_close(void)
{
    if (!white_card_wb_active)
        return;
    white_card_wb_paint_pending = 0;
    white_card_wb_close_pending = 1;
}

static void white_card_wb_close_delayed(int timer, void *opaque)
{
    (void)timer;
    (void)opaque;
    menu_white_card_wb_close();
}

void menu_white_card_wb_open(void)
{
    white_card_wb_active = 1;
    white_card_wb_capturing = 0;
    white_card_wb_done = 0;
    white_card_wb_draw_state = -1;
    white_card_wb_close_pending = 0;
    white_card_wb_paint_pending = 1;
    quick_screen_active = 0;
    quick_screen_touch_latched = 0;
    lens_display_set_dirty();
}

static void white_card_wb_panel_geometry(int *x, int *y, int *w, int *h,
                                         const char **line1, const char **line2)
{
    const int padding = 12;
    const int line_gap = 6;
    const int font_h = fontspec_font(FONT_MED)->height;
    int width1;
    int width2 = 0;

    if (white_card_wb_done)
    {
        *line1 = "White Balance Set";
        *line2 = 0;
    }
    else if (white_card_wb_capturing)
    {
        *line1 = "Setting White Balance...";
        *line2 = 0;
    }
    else
    {
        *line1 = "Place the White Card in";
        *line2 = "the box and press SET";
    }

    width1 = bmp_string_width(FONT_MED, *line1);
    if (*line2)
        width2 = bmp_string_width(FONT_MED, *line2);
    *w = MAX(width1, width2) + padding * 2;
    *h = font_h + padding * 2 + (*line2 ? font_h + line_gap : 0);
    *x = 360 - *w / 2;
    *y = 292;
}

void menu_white_card_wb_draw(void)
{
    const int border = white_card_wb_done ? COLOR_GREEN1 : COLOR_ORANGE;
    const int box_x = 326;
    const int box_y = 206;
    const int box_w = 68;
    const int box_h = 68;
    const int padding = 12;
    const int line_gap = 6;
    int text_x, text_y, text_w, text_h;
    const char *line1;
    const char *line2;
    int draw_state;

    if (!white_card_wb_active)
        return;

    draw_state = white_card_wb_done ? 2 : white_card_wb_capturing ? 1 : 0;
    if (draw_state != white_card_wb_draw_state)
    {
        /* State text has different dimensions. Clear the complete maximum
         * panel once before drawing the new state, otherwise the old two-line
         * message remains underneath the smaller progress message. */
        bmp_fill(COLOR_EMPTY, 100, 286, 520, 104);
        white_card_wb_draw_state = draw_state;
    }

    /* Thick guide border, deliberately centered on the spot sampled by
     * Magic Lantern's existing automatic Kelvin/green calculation. */
    /* Filled edges are more reliable than nested one-pixel rectangles on the
     * EOS M bitmap buffer and remain visibly thick over bright Live View. */
    bmp_fill(border, box_x, box_y, box_w, 5);
    bmp_fill(border, box_x, box_y + box_h - 5, box_w, 5);
    bmp_fill(border, box_x, box_y + 5, 5, box_h - 10);
    bmp_fill(border, box_x + box_w - 5, box_y + 5, 5, box_h - 10);

    white_card_wb_panel_geometry(
        &text_x, &text_y, &text_w, &text_h, &line1, &line2);
    bmp_fill(COLOR_BLACK, text_x, text_y, text_w, text_h);
    /* RBF fonts do not implement NO_BG_ERASE; using it became palette 0xFF
     * and produced a cyan fringe. The already-black panel is the true bg. */
    bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BLACK),
        360 - bmp_string_width(FONT_MED, line1) / 2,
        text_y + padding, "%s", line1);
    if (line2)
        bmp_printf(FONT(FONT_MED, COLOR_WHITE, COLOR_BLACK),
            360 - bmp_string_width(FONT_MED, line2) / 2,
            text_y + padding + fontspec_font(FONT_MED)->height + line_gap,
            "%s", line2);
}

void menu_white_card_wb_render_step(void)
{
    /* Called only from zebra's Live View renderer, while holding BMP_LOCK. */
    if (white_card_wb_close_pending)
    {
        white_card_wb_close_pending = 0;
        white_card_wb_paint_pending = 0;
        white_card_wb_active = 0;
        white_card_wb_capturing = 0;
        white_card_wb_done = 0;
        white_card_wb_draw_state = -1;
        white_card_wb_clear_overlay();
        lens_display_set_dirty();
        return;
    }

    /* Opening first closes the Quick Panel. Wait for that transition, then
     * paint the overlay exactly once. */
    if (white_card_wb_active && white_card_wb_paint_pending &&
        !gui_menu_shown())
    {
        menu_white_card_wb_draw();
        white_card_wb_paint_pending = 0;
    }
}

int menu_white_card_wb_handle_touch(int x, int y)
{
    int text_x, text_y, text_w, text_h;
    const char *line1;
    const char *line2;
    int in_guide;
    int in_panel;

    /* The guide and message are intentionally inert. A tap outside them is
     * the only touch gesture that dismisses this exclusive capture mode. */
    if (!white_card_wb_active)
        return 1;
    white_card_wb_panel_geometry(
        &text_x, &text_y, &text_w, &text_h, &line1, &line2);
    in_guide = x >= 326 && x < 394 && y >= 206 && y < 274;
    in_panel = x >= text_x && x < text_x + text_w &&
               y >= text_y && y < text_y + text_h;
    if (!in_guide && !in_panel)
        menu_white_card_wb_close();
    return 0;
}

int menu_white_card_wb_handle_key(int button_code, int is_fake)
{
    if (!white_card_wb_active)
        return 1;

    /* Touch coordinates are decoded by gui-common's Live View router. */
    if (button_code == BGMT_TOUCH_1_FINGER ||
        button_code == BGMT_TOUCH_2_FINGER ||
        button_code == BGMT_UNTOUCH_1_FINGER ||
        button_code == BGMT_UNTOUCH_2_FINGER)
        return 1;

    /* Preserve EOS M's physical DOWN long-press router, which generates the
     * ML grid-launch event. All other d-pad, INFO and assigned SET actions
     * remain blocked by this capture mode. */
    if ((button_code == BGMT_PRESS_DOWN || button_code == BGMT_UNPRESS_DOWN) &&
        !is_fake)
        return 1;

    if (button_code == BGMT_TRASH)
    {
        menu_white_card_wb_close();
        return 1;
    }

    if ((button_code == BGMT_PRESS_SET
#ifdef BGMT_Q_SET
         || button_code == BGMT_Q_SET
#endif
        ) && !white_card_wb_capturing && !white_card_wb_done)
    {
        white_card_wb_capturing = 1;
        white_card_wb_auto_start();
        lens_display_set_dirty();
        white_card_wb_paint_pending = 1;
    }
    return 0;
}

void menu_white_card_wb_capture_finished(void)
{
    if (!white_card_wb_active || !white_card_wb_capturing)
        return;
    white_card_wb_capturing = 0;
    white_card_wb_done = 1;
    lens_display_set_dirty();
    white_card_wb_paint_pending = 1;
    delayed_call(1000, white_card_wb_close_delayed, 0);
}

static void quick_screen_arrow(int cx, int tip_y, int up, int color)
{
    const int height = 26;
    const int half_width = 30;
    int i;
    for (i = 0; i <= height; i++)
    {
        int w = (half_width * i) / height;
        int yy = up ? tip_y + i : tip_y - i;
        draw_line(cx - w, yy, cx + w, yy, color);
    }
}

static int quick_screen_value(
    int index, char *buf, int size, int *draw_degree)
{
    struct menu_display_info info;
    struct menu_display_info adjust_info;
    const quick_screen_item_t *item = &quick_screen_items[index];
    char *value = menu_get_str_value_from_script(
        item->value_menu, item->value_entry, &info);
    char raw_value[MENU_MAX_VALUE_LEN];
    int enabled;

    snprintf(raw_value, sizeof(raw_value),
        "%s", value && value[0] ? value : "--");
    enabled = info.enabled;
    *draw_degree = 0;

    /* Resolution is displayed by a read-only row, but adjusted by the hidden
     * composite selector that spans every Aspect Ratio and preset. */
    if (index == 3)
    {
        menu_get_str_value_from_script(
            item->adjust_menu, item->adjust_entry, &adjust_info);
        enabled = adjust_info.enabled;
    }

    if (index == 5)
    {
        if (quick_screen_shutter_speed)
        {
            /* This is the effective current shutter speed, including the
             * active FPS/timing adjustment, e.g. 1/60.04. */
            snprintf(buf, size, "%s", lens_format_shutter_reciprocal(
                get_current_shutter_reciprocal_x1000(), 5));
        }
        else
        {
            /* The normal Exposure row already calculates the angle from
             * current FPS. Reuse those digits and draw the degree ring. */
            snprintf(buf, size, "%s", info.rinfo[0] ? info.rinfo : "--");
            *draw_degree = info.rinfo[0] != '\0';
        }
    }
    else if (index == 6)
    {
        snprintf(buf, size, "F%s", raw_value);
        if (streq(raw_value, "0.0"))
            enabled = 0;
    }
    else if (index == 7)
    {
        snprintf(buf, size, "ISO%s", raw_value);
    }
    else
    {
        snprintf(buf, size, "%s", raw_value);
    }

    return enabled;
}

static int quick_screen_option_enabled(int index)
{
    char value[MENU_MAX_VALUE_LEN];
    int draw_degree;
    index = COERCE(index, 0, QUICK_SCREEN_COUNT - 1);
    return quick_screen_value(index, value, sizeof(value), &draw_degree);
}

static int quick_screen_next_enabled(int start, int direction)
{
    int i;
    direction = direction < 0 ? -1 : 1;
    start = MOD(start, QUICK_SCREEN_COUNT);
    for (i = 0; i < QUICK_SCREEN_COUNT; i++)
    {
        int candidate = MOD(
            start + i * direction, QUICK_SCREEN_COUNT);
        if (quick_screen_option_enabled(candidate))
            return candidate;
    }
    return start;
}

static void quick_screen_geometry(
    int index, int *cx, int *value_y, int *up_tip_y, int *down_tip_y)
{
    int row = index / QUICK_SCREEN_COLS;
    int col = index % QUICK_SCREEN_COLS;
    static const int row_up_tip_y[2] = { 77, 278 };
    *cx = QUICK_SCREEN_CELL_W / 2 + col * QUICK_SCREEN_CELL_W;
    *up_tip_y = row_up_tip_y[row];
    *value_y = *up_tip_y + 42;
    *down_tip_y = *value_y + 82;
}

static int quick_screen_adjust(int index, int delta)
{
    int enabled;
    int draw_degree;
    char value[MENU_MAX_VALUE_LEN];
    const quick_screen_item_t *item;

    index = COERCE(index, 0, QUICK_SCREEN_COUNT - 1);
    item = &quick_screen_items[index];
    enabled = quick_screen_value(
        index, value, sizeof(value), &draw_degree);
    if (!enabled)
        return 0;

    quick_screen_feedback = index * 2 + (delta < 0);
    menu_adjust_value_by_name(
        item->adjust_menu, item->adjust_entry, delta);
    menu_redraw();
    delayed_call(220, quick_screen_feedback_clear, 0);
    delayed_call(500, quick_screen_refresh, 0);
    return 1;
}

void menu_quick_screen_draw(void)
{
    int index;
    bmp_fill(COLOR_BLACK, 0, 0, 720, 480);

    /* Menu task owns the screen here, so dynamic availability is safe to
     * evaluate. Never leave the yellow selector on a disabled control. */
    if (!quick_screen_option_enabled(quick_screen_sel))
        quick_screen_sel = quick_screen_next_enabled(
            quick_screen_sel + 1, 1);

    for (index = 0; index < QUICK_SCREEN_COUNT; index++)
    {
        int cx, value_y, up_tip_y, down_tip_y;
        char value[MENU_MAX_VALUE_LEN];
        int width;
        int enabled;
        int draw_degree;
        int color;
        int value_x;
        quick_screen_geometry(
            index, &cx, &value_y, &up_tip_y, &down_tip_y);
        enabled = quick_screen_value(
            index, value, sizeof(value), &draw_degree);
        color = enabled ? COLOR_WHITE : COLOR_GRAY(50);
        width = bmp_string_width(FONT_CANON, value);
        value_x = cx - (width + (draw_degree ? 12 : 0)) / 2;
        bmp_printf(
            FONT(FONT_CANON, color, NO_BG_ERASE),
            value_x, value_y, "%s", value);
        if (draw_degree)
        {
            int degree_x = value_x + width + 6;
            int degree_y = value_y + 7;
            draw_circle(degree_x, degree_y, 4, color);
            draw_circle(degree_x, degree_y, 3, color);
        }
        quick_screen_arrow(cx, up_tip_y, 1,
            !enabled ? COLOR_GRAY(50) :
            quick_screen_feedback == index * 2 ? COLOR_WHITE : COLOR_ORANGE);
        quick_screen_arrow(cx, down_tip_y, 0,
            !enabled ? COLOR_GRAY(50) :
            quick_screen_feedback == index * 2 + 1 ? COLOR_WHITE : COLOR_ORANGE);

        if (index == quick_screen_sel && enabled)
        {
            /* Slightly narrower than the 60px arrow for a lighter highlight. */
            bmp_fill(COLOR_YELLOW, cx - 24, up_tip_y - 17, 48, 4);
        }
    }
}

int menu_quick_screen_handle_touch(int x, int y)
{
    int index = -1;
    int row;
    int col;
    int delta;
    int draw_degree;
    int cx, value_y, up_tip_y, down_tip_y;
    char value[MENU_MAX_VALUE_LEN];
    if (!quick_screen_active)
        return 1;
    if (quick_screen_touch_latched)
        return 0;

    /* Large, non-overlapping arrow hitboxes. The 32px horizontal gaps and
     * vertical gaps around values remain true empty-space Back targets. */
    col = COERCE(x / QUICK_SCREEN_CELL_W, 0, QUICK_SCREEN_COLS - 1);
    for (row = 0; row < QUICK_SCREEN_ROWS; row++)
    {
        int candidate = row * QUICK_SCREEN_COLS + col;
        quick_screen_geometry(
            candidate, &cx, &value_y, &up_tip_y, &down_tip_y);
        if (x >= cx - QUICK_SCREEN_TOUCH_HALF_W &&
            x <= cx + QUICK_SCREEN_TOUCH_HALF_W &&
            y >= up_tip_y - 35 && y <= up_tip_y + 40)
        {
            index = candidate;
            delta = 1;
            break;
        }
        if (x >= cx - QUICK_SCREEN_TOUCH_HALF_W &&
            x <= cx + QUICK_SCREEN_TOUCH_HALF_W &&
            y >= down_tip_y - 40 && y <= down_tip_y + 35)
        {
            index = candidate;
            delta = -1;
            break;
        }
    }

    if (index >= 0)
    {
        quick_screen_touch_latched = 1;
        if (!quick_screen_option_enabled(index))
        {
            /* Disabled tiles are inert: do not move the yellow selector. */
            return 0;
        }
        else
        {
            quick_screen_sel = index;
            if (!quick_screen_adjust(index, delta))
                menu_redraw(); /* Move the yellow selection when read-only. */
        }
        return 0;
    }

    /* Text is not empty space. Only the shutter value itself toggles its
     * display format; its bounds deliberately stay clear of both arrows. */
    for (index = 0; index < QUICK_SCREEN_COUNT; index++)
    {
        int width;
        int text_x;
        int text_h = fontspec_font(FONT_CANON)->height;
        quick_screen_geometry(
            index, &cx, &value_y, &up_tip_y, &down_tip_y);
        quick_screen_value(
            index, value, sizeof(value), &draw_degree);
        width = bmp_string_width(FONT_CANON, value) +
                (draw_degree ? 12 : 0);
        text_x = cx - width / 2;
        if (index == 5 &&
            x >= text_x && x <= text_x + width &&
            y >= value_y && y < down_tip_y - 40)
        {
            quick_screen_touch_latched = 1;
            set_config_var_ptr(
                &quick_screen_shutter_speed,
                !quick_screen_shutter_speed);
            menu_redraw();
            return 0;
        }
        if (index == 0 &&
            x >= text_x && x <= text_x + width &&
            y >= value_y && y <= value_y + text_h)
        {
            quick_screen_touch_latched = 1;
            menu_white_card_wb_open();
            gui_stop_menu();
            return 0;
        }
        if (x >= text_x - 8 && x <= text_x + width + 8 &&
            y >= value_y - 6 && y <= value_y + text_h + 6)
        {
            quick_screen_touch_latched = 1;
            return 0;
        }
    }

    /* Any truly empty area is a one-tap Back action to Live View. */
    quick_screen_touch_latched = 1;
    menu_quick_screen_close();
    gui_stop_menu();
    return 0;
}

void menu_quick_screen_touch_release(void)
{
    quick_screen_touch_latched = 0;
}

int menu_quick_screen_handle_key(int button_code)
{
    if (!quick_screen_active)
        return 1;

    switch (button_code)
    {
    case BGMT_MENU:
    case BGMT_Q:
    case BGMT_INFO:
        menu_quick_screen_close();
        gui_stop_menu();
        return 0;

    case BGMT_PRESS_LEFT:
    case BGMT_WHEEL_LEFT:
        quick_screen_sel = quick_screen_next_enabled(quick_screen_sel - 1, -1);
        menu_redraw();
        return 0;

    case BGMT_PRESS_RIGHT:
    case BGMT_WHEEL_RIGHT:
        quick_screen_sel = quick_screen_next_enabled(quick_screen_sel + 1, 1);
        menu_redraw();
        return 0;

    case BGMT_PRESS_UP:
    case BGMT_WHEEL_UP:
        quick_screen_adjust(quick_screen_sel, 1);
        return 0;

    case BGMT_PRESS_DOWN:
    case BGMT_WHEEL_DOWN:
        quick_screen_adjust(quick_screen_sel, -1);
        return 0;

    default:
        return 0;
    }
}

void menu_grid_draw(void)
{
    bmp_fill(COLOR_BLACK, 0, 0, 720, 480);

    /* Grid cards have a solid background, so the Slim font can render cleanly. */
    int fnt = slim_ui_font_spec(COLOR_WHITE, COLOR_GRAY(20));
    int label_h = slim_ui_font_height();
    int b = GRID_SEL_BORDER;

    for (int i = 0; i < GRID_COUNT; i++)
    {
        int x, y, w, h;
        grid_cell_rect(i, &x, &y, &w, &h);
        int selected = (i == grid_sel);
        int r = MIN(GRID_RADIUS, MIN(w, h) / 2);

        if (selected)
            grid_fill_round_rect(x - b, y - b, w + 2 * b, h + 2 * b, r + b, COLOR_ORANGE);

        grid_fill_round_rect(x, y, w, h, r, COLOR_GRAY(20));

        /* Shared bottom baseline for all four labels. */
        int label_y = y + h - GRID_LABEL_PAD - label_h;
        int label_w = bmp_string_width(fnt, (char *) grid_tiles[i].label);
        int label_x = x + (w - label_w) / 2;

        /* Icon centered in the remaining space above the label. */
        int icon_zone_top = y + 10;
        int icon_zone_bot = label_y - GRID_ICON_GAP;
        int icon_cy = (icon_zone_top + icon_zone_bot) / 2;
        grid_draw_ml_icon(grid_tiles[i].icon, x + w / 2, icon_cy);

        bmp_printf(fnt, label_x, label_y, "%s", grid_tiles[i].label);
    }
}

int menu_grid_handle_key(int button_code, int *needs_full_redraw)
{
    if (!grid_active)
        return 1;

    int col = grid_sel % GRID_COLS;
    int row = grid_sel / GRID_COLS;

    switch (button_code)
    {
    case BGMT_PRESS_UP:
    case BGMT_WHEEL_UP:
        row = (row + GRID_ROWS - 1) % GRID_ROWS;
        grid_sel = row * GRID_COLS + col;
        break;

    case BGMT_PRESS_DOWN:
    case BGMT_WHEEL_DOWN:
        row = (row + 1) % GRID_ROWS;
        grid_sel = row * GRID_COLS + col;
        break;

    case BGMT_PRESS_LEFT:
    case BGMT_WHEEL_LEFT:
        col = (col + GRID_COLS - 1) % GRID_COLS;
        grid_sel = row * GRID_COLS + col;
        break;

    case BGMT_PRESS_RIGHT:
    case BGMT_WHEEL_RIGHT:
        col = (col + 1) % GRID_COLS;
        grid_sel = row * GRID_COLS + col;
        break;

    case BGMT_PRESS_SET:
#if defined(CONFIG_7D)
    case BGMT_JOY_CENTER:
#endif
#ifdef BGMT_Q_SET
    case BGMT_Q_SET:
#endif
        menu_grid_launch(grid_sel);
        *needs_full_redraw = 1;
        return 0;

    case BGMT_MENU:
        return 1;

    default:
        return 1;
    }

    *needs_full_redraw = 1;
    return 0;
}

#else /* !CONFIG_SLIM_MENUS */

int menu_grid_is_active(void)   { return 0; }
int menu_grid_is_launched(void) { return 0; }
void menu_grid_open(void)       { }
void menu_grid_close(void)      { }
void menu_grid_return(void)    { }
void menu_grid_enter_launched(void) { }
void menu_grid_draw(void)       { }
int menu_grid_handle_key(int button_code, int *needs_full_redraw)
{
    (void) button_code;
    (void) needs_full_redraw;
    return 1;
}
int menu_grid_handle_touch(int x, int y) { (void)x; (void)y; return 1; }
int menu_quick_screen_is_active(void) { return 0; }
void menu_quick_screen_open(void) { }
void menu_quick_screen_close(void) { }
void menu_quick_screen_draw(void) { }
int menu_quick_screen_handle_touch(int x, int y) { (void)x; (void)y; return 1; }
void menu_quick_screen_touch_release(void) { }
int menu_quick_screen_handle_key(int button_code) { (void)button_code; return 1; }
int menu_white_card_wb_is_active(void) { return 0; }
void menu_white_card_wb_open(void) { }
void menu_white_card_wb_close(void) { }
void menu_white_card_wb_draw(void) { }
void menu_white_card_wb_render_step(void) { }
int menu_white_card_wb_handle_touch(int x, int y) { (void)x; (void)y; return 1; }
int menu_white_card_wb_handle_key(int button_code, int is_fake) { (void)button_code; (void)is_fake; return 1; }
void menu_white_card_wb_capture_finished(void) { }

#endif
