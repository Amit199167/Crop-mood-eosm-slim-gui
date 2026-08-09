#include <dryos.h>
#include <property.h>
#include <menu.h>
#include <bmp.h>
#include <lvinfo.h>
#include <lens.h>
#include <fps.h>
#include <module.h>

#ifdef CONFIG_SLIM_MENUS
static int (*dual_iso_is_enabled)() = MODULE_FUNCTION(dual_iso_is_enabled);
static int (*dual_iso_get_recovery_iso)() = MODULE_FUNCTION(dual_iso_get_recovery_iso);
#endif

#define MAX_ITEMS 64
#define MIN_SPACING 24
#define TOTAL_WIDTH 720

//~ #define LVINFO_PERF_MON

/* all registered info items go here */
/* note: these are somewhat private; they get first sorted in top/bottom bars,
 * and most of the code works at bar level, without accessing _info_items directly */
static struct semaphore * lvinfo_sem = 0;

static GUARDED_BY(lvinfo_sem)   struct lvinfo_item * _info_items[MAX_ITEMS];
static GUARDED_BY(lvinfo_sem)   int _info_items_count = 0;
static GUARDED_BY(lvinfo_sem)   int layout_dirty = 0;

static GUARDED_BY(lvinfo_sem)   int default_font = FONT_MED_LARGE | FONT_ALIGN_CENTER;   /* used in normal situations */
static GUARDED_BY(lvinfo_sem)   int small_font = FONT_MED | FONT_ALIGN_CENTER;           /* used if the layout gets really tight */

static enum lvinfo_touch_field lvinfo_touch_field = LVINFO_TOUCH_NONE;
static char lvinfo_touch_menu_value[2][32];
static int lvinfo_touch_menu_enabled[2] = { 1, 1 };
static int lvinfo_touch_feedback_slot = -1;
static int lvinfo_touch_feedback_sign;

/* The single-value editor is square.  Crop mode + resolution share a wider
 * rectangle with the same height.  Input uses these exact bounds too. */
#define LVINFO_TOUCH_BOX_Y          150
#define LVINFO_TOUCH_BOX_H          180
#define LVINFO_TOUCH_SINGLE_X       270
#define LVINFO_TOUCH_SINGLE_W       180
#define LVINFO_TOUCH_CROP_X         105
#define LVINFO_TOUCH_CROP_W         510
#define LVINFO_TOUCH_UP_TIP_Y       (LVINFO_TOUCH_BOX_Y + 28)
#define LVINFO_TOUCH_VALUE_Y        (LVINFO_TOUCH_UP_TIP_Y + 42)
#define LVINFO_TOUCH_DOWN_TIP_Y     (LVINFO_TOUCH_VALUE_Y + 82)

static const char * lvinfo_touch_field_name(enum lvinfo_touch_field field)
{
    switch (field)
    {
        case LVINFO_TOUCH_APERTURE: return "Aperture";
        case LVINFO_TOUCH_SHUTTER:  return "Shutter";
        case LVINFO_TOUCH_ISO:      return "ISO";
        case LVINFO_TOUCH_WB:       return "White Balance";
        case LVINFO_TOUCH_CROP:     return "Crop info";
        case LVINFO_TOUCH_FPS:      return "FPS";
        case LVINFO_TOUCH_BIT_DEPTH:return "Bitdepth info";
        default:                    return 0;
    }
}

static const char * lvinfo_touch_field_value(enum lvinfo_touch_field field)
{
    static char value[32];

    switch (field)
    {
        case LVINFO_TOUCH_APERTURE:
            return lens_info.raw_aperture ? lens_format_aperture(lens_info.raw_aperture) : "F0.0";
        case LVINFO_TOUCH_SHUTTER:
            return lens_format_shutter_reciprocal(get_current_shutter_reciprocal_x1000(), 2);
        case LVINFO_TOUCH_ISO:
#ifdef CONFIG_SLIM_MENUS
            /* The editor changes recovery ISO when Dual ISO is enabled, so
             * its center value must show that same second ISO. */
            if (dual_iso_is_enabled && dual_iso_is_enabled() &&
                dual_iso_get_recovery_iso)
            {
                int recovery_raw = dual_iso_get_recovery_iso();
                if (recovery_raw)
                    return lens_format_iso(recovery_raw);
            }
#endif
            return lens_info.raw_iso ? lens_format_iso(lens_info.raw_iso) : "ISO Auto";
        case LVINFO_TOUCH_WB:
            if (lens_info.wb_mode == WB_KELVIN)
            {
                snprintf(value, sizeof(value), "%dK", lens_info.kelvin);
                return value;
            }
            snprintf(value, sizeof(value), "%s",
                lens_info.wb_mode == WB_SUNNY ? "Sunny" :
                lens_info.wb_mode == WB_CLOUDY ? "Cloudy" :
                lens_info.wb_mode == WB_TUNGSTEN ? "Tungsten" :
                lens_info.wb_mode == WB_FLUORESCENT ? "Fluorescent" :
                lens_info.wb_mode == WB_FLASH ? "Flash" :
                lens_info.wb_mode == WB_SHADE ? "Shade" : "Auto WB");
            return value;
        default:
            return "";
    }
}

static void lvinfo_touch_draw_arrow(int cx, int tip_y, int up, int color)
{
    const int height = 26;
    const int half_width = 30;
    for (int i = 0; i <= height; i++)
    {
        int half = (half_width * i) / height;
        int y = up ? tip_y + i : tip_y - i;
        draw_line(cx - half, y, cx + half, y, color);
    }
}

static void lvinfo_touch_draw_value(int slot, int cx, int value_y,
                                    const char *value, int enabled)
{
    int color = enabled ? COLOR_WHITE : COLOR_GRAY(50);
    int width = bmp_string_width(FONT_CANON, value);
    int up_color = enabled ? COLOR_ORANGE : color;
    int down_color = enabled ? COLOR_ORANGE : color;

    if (enabled && lvinfo_touch_feedback_slot == slot)
    {
        if (lvinfo_touch_feedback_sign > 0) up_color = COLOR_WHITE;
        if (lvinfo_touch_feedback_sign < 0) down_color = COLOR_WHITE;
    }
    lvinfo_touch_draw_arrow(cx, LVINFO_TOUCH_UP_TIP_Y, 1, up_color);
    bmp_printf(FONT(FONT_CANON, color, NO_BG_ERASE),
               cx - width / 2, value_y, "%s", value);
    lvinfo_touch_draw_arrow(cx, LVINFO_TOUCH_DOWN_TIP_Y, 0, down_color);
}

static void lvinfo_touch_draw_editor(void)
{
    if (lvinfo_touch_field == LVINFO_TOUCH_NONE)
        return;

    int value_y = LVINFO_TOUCH_VALUE_Y;

    if (lvinfo_touch_field == LVINFO_TOUCH_CROP)
    {
        bmp_fill(COLOR_BLACK, LVINFO_TOUCH_CROP_X, LVINFO_TOUCH_BOX_Y,
                 LVINFO_TOUCH_CROP_W, LVINFO_TOUCH_BOX_H);
        lvinfo_touch_draw_value(0, 232, value_y, lvinfo_touch_menu_value[0],
                                lvinfo_touch_menu_enabled[0]);
        lvinfo_touch_draw_value(1, 488, value_y, lvinfo_touch_menu_value[1],
                                lvinfo_touch_menu_enabled[1]);
    }
    else
    {
        const char *value =
            (lvinfo_touch_field == LVINFO_TOUCH_FPS ||
             lvinfo_touch_field == LVINFO_TOUCH_BIT_DEPTH)
            ? lvinfo_touch_menu_value[0]
            : lvinfo_touch_field_value(lvinfo_touch_field);
        int enabled =
            (lvinfo_touch_field == LVINFO_TOUCH_FPS ||
             lvinfo_touch_field == LVINFO_TOUCH_BIT_DEPTH)
            ? lvinfo_touch_menu_enabled[0] : 1;
        bmp_fill(COLOR_BLACK, LVINFO_TOUCH_SINGLE_X, LVINFO_TOUCH_BOX_Y,
                 LVINFO_TOUCH_SINGLE_W, LVINFO_TOUCH_BOX_H);
        lvinfo_touch_draw_value(0, 360, value_y, value, enabled);
    }
}

/* fixme: false thread safety warning
 * when called from INIT_FUNC's, the semaphore may not be initialized yet
 * but these functions are called sequentially, so there's no race condition possible
 */
EXCLUDES(lvinfo_sem) NO_THREAD_SAFETY_ANALYSIS
void lvinfo_add_items(struct lvinfo_item * items, int count)
{
    if (lvinfo_sem) take_semaphore(lvinfo_sem, 0);
    for (int i = 0; i < count && _info_items_count < MAX_ITEMS; i++)
    {
        _info_items[_info_items_count] = &items[i];
        _info_items_count++;
    }
    layout_dirty = 1;
    if (lvinfo_sem) give_semaphore(lvinfo_sem);
}

void lvinfo_add_item(struct lvinfo_item * item)
{
    lvinfo_add_items(item, 1);
}

static int is_active(struct lvinfo_item * item)
{
    return item->width && !item->hidden && !item->disabled;
}

/* call the update functions and compute the metrics */
static REQUIRES(lvinfo_sem)
void lvinfo_update_items(struct lvinfo_item * items[], int count, int override_font)
{
    /* everybody measure themselves! */
    for (int i = 0; i < count; i++)
    {
        items[i]->width = 0;
        items[i]->height = 0;
        items[i]->hidden = 0;
        
        if (override_font) items[i]->fontspec = override_font;
        int fnt = items[i]->fontspec;
        items[i]->color_fg = FONT_FG(fnt);
        items[i]->color_bg = FONT_BG(fnt);

        if (items[i]->update)
        {
            items[i]->update(items[i], 0);
        }

        /* no width/height specified? use defaults */
        if (!items[i]->width && items[i]->value)
        {
            items[i]->width = bmp_string_width(fnt, items[i]->value);
        }
        if (!items[i]->height)
        {
            items[i]->height = fontspec_font(fnt)->height;
        }
    }
}

static REQUIRES(lvinfo_sem)
int lvinfo_check_if_needs_reflow(struct lvinfo_item * items[], int count, int bar_x, int bar_width)
{
    int too_tight = 0;
    int max_spacing = INT_MIN;
    int min_spacing = INT_MAX;
    int prev_right = bar_x;

    for (int i = 0; i <= count; i++)
    {
        /* how far we are from the previous one? */
        if (i == count || is_active(items[i]))
        {
            int now_left = (i < count) ?
                items[i]->x - items[i]->width/2 :   /* normal case: spacing between items */
                bar_x + bar_width ;                 /* special case: after last item */
            
            int spacing = now_left - prev_right;
            if (i == 0 || i == count)               /* spacing at the borders is normally half of spacing between items */
                spacing *= 2;                       /* multiply by 2 so we can compare these things */
            
            min_spacing = MIN(min_spacing, spacing);
            max_spacing = MAX(max_spacing, spacing);

            /* for debugging */
            //~ bmp_fill(i == 0 || i == count ? COLOR_BLUE : COLOR_RED, prev_right, 100, now_left - prev_right, 2);
            
            if (spacing < MIN_SPACING * 2/3)
            {
                too_tight = 1;
            }
            prev_right = items[i]->x + items[i]->width/2;
        }
    }
    
    int imbalanced = (max_spacing - min_spacing > MIN_SPACING*2);
    return too_tight || imbalanced;
}


/* assign some items from the global list to top/bottom bars */
static REQUIRES(lvinfo_sem)
void lvinfo_distribute_items(int which_bar, struct lvinfo_item * items[], int* count, int* space)
{
    for (int i = 0; i < _info_items_count; i++)
    {
        if (!_info_items[i]->placed && (which_bar == -1 || _info_items[i]->which_bar == which_bar))
        {
            *space -= _info_items[i]->width + (is_active(_info_items[i]) ? MIN_SPACING : 0);

            if (*space > 0)
            {
                items[(*count)++] = _info_items[i];
                _info_items[i]->placed = 1;
            }
        }
    }
}

static REQUIRES(lvinfo_sem)
void lvinfo_mark_all_as_not_placed()
{
    for (int i = 0; i < _info_items_count; i++)
    {
        _info_items[i]->placed = 0;
    }
}

/* how many items are still left to be placed? */
static REQUIRES(lvinfo_sem)
int lvinfo_remaining_items()
{
    int count = 0;
    for (int i = 0; i < _info_items_count; i++)
    {
        if (!_info_items[i]->placed)
        {
            count++;
        }
    }
    return count;
}

/* distribute spacing evenly between items */
static REQUIRES(lvinfo_sem)
void lvinfo_justify_items(struct lvinfo_item * items[], int count, int total_width)
{
    int used_items = 0;
    int used_width = 0;
    for (int i = 0; i < count; i++)
    {
        int active = is_active(items[i]);
        used_items += active ? 1 : 0;
        used_width += active ? items[i]->width : 0;
    }
    
    /* how much we can stretch? */
    int extra_spacing = total_width - used_width;

    /* todo: use Bresenham algorithm to get rid of these floats */
    float spacing_per_item = (float) extra_spacing / used_items;
    float x = spacing_per_item / 2;
    
    for (int i = 0; i < count; i++)
    {
        items[i]->x = x + items[i]->width/2;

        if (is_active(items[i]))
        {
            x += items[i]->width + spacing_per_item;
        }
    }
}

/* heuristic that tells whether we should try to enlarge the font */
static REQUIRES(lvinfo_sem)
int lvinfo_should_enlarge(struct lvinfo_item * items[], int count, int total_width)
{
    int used_items = 0;
    int used_width = 0;
    for (int i = 0; i < count; i++)
    {
        int active = is_active(items[i]);
        used_items += active ? 1 : 0;
        used_width += active ? items[i]->width : 0;
    }
    
    int extra_spacing = total_width - used_width;
    int spacing_per_item = extra_spacing / used_items;
        
    return spacing_per_item > MIN_SPACING * 5/4;
}

/* shrink some items or hide low-priority ones */
/* returns: INT_MIN if nothing was discarded, INT_MAX if error, otherwise it returns the priority of last item discarded */
static REQUIRES(lvinfo_sem)
int lvinfo_squeeze_space(struct lvinfo_item * items[], int count, int total_width)
{
    int used_items = 0;
    int used_width = 0;
    for (int i = 0; i < count; i++)
    {
        int active = is_active(items[i]);
        used_items += active ? 1 : 0;
        used_width += active ? items[i]->width + MIN_SPACING : 0;
    }
    
    /* how much we can stretch? */
    int spacing_needed = used_width - total_width;
    
    /* any real need to squeeze space? */
    if (spacing_needed <= MIN_SPACING)
        return INT_MIN;

    /* sort by priority (lowest first) */
    struct lvinfo_item * prio_items[MAX_ITEMS];
    memcpy(prio_items, items, sizeof(prio_items));
    for (int i = 0; i < count-1; i++)
    {
        for (int j = i+1; j < count; j++)
        {
            if (prio_items[i]->priority > prio_items[j]->priority)
            {
                struct lvinfo_item * aux = prio_items[i];
                prio_items[i] = prio_items[j];
                prio_items[j] = aux;
            }
        }
    }

    /* sort by width, largest first */
    struct lvinfo_item * big_items[MAX_ITEMS];
    memcpy(big_items, items, sizeof(big_items));
    for (int i = 0; i < count-1; i++)
    {
        for (int j = i+1; j < count; j++)
        {
            if (big_items[i]->width < big_items[j]->width)
            {
                struct lvinfo_item * aux = big_items[i];
                big_items[i] = big_items[j];
                big_items[j] = aux;
            }
        }
    }

    /* shrink items, starting with the largest ones */
    /* if we have to shrink 3 or more items, shrink them all */
    int shrunk = 0;
    for (int i = 0; i < count-1; i++)
    {
        if (is_active(big_items[i]))
        {
            int old_width = big_items[i]->width;
            lvinfo_update_items(&big_items[i], 1, small_font);
            int new_width = big_items[i]->width;
            spacing_needed -= (old_width - new_width);
            shrunk++;
            if (spacing_needed <= MIN_SPACING && shrunk < 3)
            {
                /* succeeded by shrinking 1 or 2 items? */
                return INT_MIN;
            }
        }
    }

    if (spacing_needed <= MIN_SPACING)
    {
        return INT_MIN;
    }

    /* discard all items until there's enough space; lower priority discarded first */
    for (int i = 0; i < count-1; i++)
    {
        if (is_active(prio_items[i]))
        {
            prio_items[i]->hidden = 1;
            spacing_needed -= (prio_items[i]->width + MIN_SPACING);
            if (spacing_needed <= MIN_SPACING)
            {
                return prio_items[i]->priority;
            }
        }
    }
    /* should be unreachable */
    return INT_MAX;
}

static REQUIRES(lvinfo_sem)
void lvinfo_valign_items(struct lvinfo_item * items[], int count, int bar_y, int bar_height)
{
    for (int i = 0; i < count; i++)
    {
        items[i]->y = bar_y + (bar_height - items[i]->height) / 2 + 2;
    }
}

static REQUIRES(lvinfo_sem)
void lvinfo_sort_by_position(struct lvinfo_item * items[], int count)
{
    /* sort by preferred position */
    /* we need to use a stable sorting algorithm, so items with the same preferred position will not get swapped */
    int done = 0;
    while (!done)
    {
        done = 1;
        for (int i = 0; i < count-1; i++)
        {
            if (items[i]->preferred_position > items[i+1]->preferred_position)
            {
                struct lvinfo_item * aux = items[i];
                items[i] = items[i+1];
                items[i+1] = aux;
                done = 0;
            }
        }
    }
}

/* top/bottom bar */
static GUARDED_BY(lvinfo_sem)   struct lvinfo_item * top_items[MAX_ITEMS];
static GUARDED_BY(lvinfo_sem)   struct lvinfo_item * bot_items[MAX_ITEMS];
static GUARDED_BY(lvinfo_sem)   int top_count = 0;
static GUARDED_BY(lvinfo_sem)   int bot_count = 0;

static REQUIRES(lvinfo_sem)
void lvinfo_refresh_layout()
{
    /* try 3 layouts:
     * normal (large font),
     * tight if there are still items that didn't fit,
     * and really tight, which attempts to squeeze everything and leave it up to the display routine to sort it out
     **/
    for (int tight = 0; tight <= 2; tight++)
    {
        /* reset the "placed" flag so we can rebuild the layout from scratch */
        lvinfo_mark_all_as_not_placed();
        
        /* reset top/bottom bars */
        top_count = bot_count = 0;
        
        /* distribute stuff to top/bottom bars */
        lvinfo_update_items(_info_items, _info_items_count, tight ? small_font : default_font);
        
        int top_space = tight == 2 ? TOTAL_WIDTH * 10 : TOTAL_WIDTH;
        int bot_space = top_space;
        
        /* first, move the items that can't be placed elsewhere */
        lvinfo_distribute_items(LV_TOP_BAR_ONLY,        top_items, &top_count, &top_space);
        lvinfo_distribute_items(LV_BOTTOM_BAR_ONLY,     bot_items, &bot_count, &bot_space);
        
        /* next, try to follow the preferences */
        lvinfo_distribute_items(LV_PREFER_TOP_BAR,      top_items, &top_count, &top_space);
        lvinfo_distribute_items(LV_PREFER_BOTTOM_BAR,   bot_items, &bot_count, &bot_space);
        
        /* still some items that couldn't fit according to preferences? move to the other bar */
        lvinfo_distribute_items(LV_PREFER_TOP_BAR,      bot_items, &bot_count, &bot_space);
        lvinfo_distribute_items(LV_PREFER_BOTTOM_BAR,   top_items, &top_count, &top_space);

        /* fill the remaining space with items that don't care where they are placed */
        lvinfo_distribute_items(LV_WHEREVER_IT_FITS,    top_items, &top_count, &top_space);
        lvinfo_distribute_items(LV_WHEREVER_IT_FITS,    bot_items, &bot_count, &bot_space);
        
        /* finished? hope so; otherwise, go back and try a tighter layout */
        if (lvinfo_remaining_items() == 0)
        {
            break;
        }
    }
    
    /* sort items */
    lvinfo_sort_by_position(top_items, top_count);
    lvinfo_sort_by_position(bot_items, bot_count);
    
    /* distribute spacing evenly between items */
    lvinfo_justify_items(top_items, top_count, TOTAL_WIDTH);
    lvinfo_justify_items(bot_items, bot_count, TOTAL_WIDTH);
}

static REQUIRES(lvinfo_sem)
void lvinfo_display_bar(struct lvinfo_item * items[], int count, int bar_x, int bar_y, int bar_width, int bar_height)
{
    int default_bg = FONT_BG(default_font);
    int default_bg_out = (default_bg == COLOR_BG_DARK ? 0 : default_bg);
    int touch_hx0 = -1;
    int touch_hy0 = -1;
    int touch_hw = 0;
    int touch_hh = 0;
    
    int prev_right = bar_x;
    int prev_bg = default_bg_out;
    for (int i = 0; i < count; i++)
    {
        /* don't process empty items */
        if (!is_active(items[i]))
            continue;
        
        /* position */
        int x = items[i]->x;
        int w = items[i]->width;
        int now_left = x - w/2;
        int now_right = x + w/2;
        int y = items[i]->y;
        int y0 = bar_y;
        int x0 = now_left;

        /* range checking */
        if (now_left < bar_x)
            continue;
        if (now_right > bar_x + bar_width)
            continue;

        /* font */
        int fnt = items[i]->fontspec;
        
        /* override colors */
        fnt = FONT(fnt, items[i]->color_fg, items[i]->color_bg);
        
        int bg = FONT_BG(fnt);

        /* fill the gap between this item and previous one */
        /* the Voronoi cell associated with each item will get filled by the same background color */
        if (prev_right >= 0 && now_left > prev_right)
        {
            int gap = now_left - prev_right + 1;
            bmp_fill(prev_bg, prev_right, y0, gap/2, bar_height);
            bmp_fill(bg, prev_right+gap/2, y0, gap/2, bar_height);
        }

        /* clear the space for current box */
        bmp_fill(bg, x0, y0, w, bar_height);
        
        /* for debugging: show the center of each item */
        //~ bmp_fill(COLOR_RED, x-1, y0-2, 2, 2);

        if (items[i]->custom_drawing)
        {
            /* anybody asked for custom drawing? */
            if (items[i]->update)
            {
                items[i]->update(items[i], 1);
            }
        }
        else
        {
            /* no custom draw? use our default print routine */
            bmp_printf(fnt, x, y, "%s", items[i]->value);
        }

        if (lvinfo_touch_field_name(lvinfo_touch_field) &&
            !strcmp(items[i]->name, lvinfo_touch_field_name(lvinfo_touch_field)))
        {
            int hx0 = MAX(bar_x, x0 - 4);
            int hx1 = MIN(bar_x + bar_width - 1, x0 + w + 4);
            int hy0 = MAX(bar_y, y0 + 1);
            int hy1 = MIN(bar_y + bar_height - 1, y0 + bar_height - 2);
            touch_hx0 = hx0;
            touch_hy0 = hy0;
            touch_hw = hx1 - hx0 + 1;
            touch_hh = hy1 - hy0 + 1;
        }
        prev_right = x + w/2;
        prev_bg = bg;
    }

    /* fill the remaining space till the far right */
    if (count > 0)
    {
        int now_left = TOTAL_WIDTH;
        int gap = now_left - prev_right;
        bmp_fill(prev_bg, prev_right, bar_y, gap / 2, bar_height);
        bmp_fill(default_bg_out, prev_right + gap / 2, bar_y, gap / 2, bar_height);
    }

    /* Draw selection last.  Gap and neighboring-item background fills used
     * to overwrite its right edge, leaving a three-sided orange outline. */
    if (touch_hx0 >= 0)
        bmp_draw_rect(COLOR_ORANGE, touch_hx0, touch_hy0, touch_hw, touch_hh);
}

static REQUIRES(lvinfo_sem)
void lvinfo_align_and_display(struct lvinfo_item * items[], int count, int bar_x, int bar_y, int bar_width, int bar_height)
{
    #ifdef LVINFO_PERF_MON
    int64_t t0 = get_us_clock();
    #endif
    
    /* choose a default font */
    /* try to borrow the color from the cropmarks; if it's fully transparent, use transparent gray */
    int bg = (items == top_items) ? TOPBAR_BGCOLOR : BOTTOMBAR_BGCOLOR;
    if (bg == 0) bg = COLOR_BG_DARK;
    default_font = FONT(default_font, COLOR_WHITE, bg);
    small_font = FONT(small_font, COLOR_WHITE, bg);
    
    int font_changed = 0;

    for (int i = 0; i < count; i++)
    {
        /* colors changed? reset the font to large and refresh the layout */
        /* this will also update the text and dimensions for all items */
        int prev_fnt = items[i]->fontspec;
        int colors_changed = (prev_fnt & 0xFFFF) != (default_font & 0xFFFF);
        if (colors_changed) font_changed++;
        lvinfo_update_items(&items[i], 1, colors_changed ? default_font : 0);
    }

    /* should we try to display everything in large font? */
    /* if it doesn't look bad, keep the previous layout */
    int should_enlarge = lvinfo_should_enlarge(items, count, bar_width);

    if (should_enlarge)
    {
        for (int i = 0; i < count; i++)
        {
            /* check each item; if it was small and now it should be enlarged, update the font */
            int prev_fnt = items[i]->fontspec;
            int font_should_change = (prev_fnt & FONT_MASK) != (default_font & FONT_MASK);
            if (font_should_change)
            {
                font_changed++;
                lvinfo_update_items(&items[i], 1, default_font);
            }
        }
    }
    
    int needs_reflow = font_changed || lvinfo_check_if_needs_reflow(items, count, bar_x, bar_width);
    if (needs_reflow)
    {
        /* some items got too tight? try to re-distribute the spacing between them */
        lvinfo_justify_items(items, count, bar_width);

        /* things got really tight */
        int still_needs_reflow = lvinfo_check_if_needs_reflow(items, count, bar_x, bar_width);
        if (still_needs_reflow)
        {
            int severity = lvinfo_squeeze_space(items, count, bar_width);
            lvinfo_justify_items(items, count, bar_width);
            if (severity >= 0)
            {
                /* important items were disabled */
                /* it may be better if we try to rebuild the layout from scratch */
                layout_dirty = 1;
            }
        }
    }

    /* center items vertically */
    lvinfo_valign_items(items, count, bar_y, bar_height);

    #ifdef LVINFO_PERF_MON
    int64_t t1 = get_us_clock();
    #endif

    /* and... finally, display them! */
    lvinfo_display_bar(items, count, bar_x, bar_y, bar_width, bar_height);

    #ifdef LVINFO_PERF_MON
    int64_t t2 = get_us_clock();
    bmp_printf(FONT_MED, 10, items == top_items ? 100 : 200, "Layout : %d "SYM_MICRO"s \nDrawing: %d "SYM_MICRO"s", (int)(t1-t0), (int)(t2-t1));
    #endif
}

EXCLUDES(lvinfo_sem)
void lvinfo_display(int top, int bottom)
{
    take_semaphore(lvinfo_sem, 0);

    static int refresh_timer = INT_MIN;
    if (layout_dirty && should_run_polling_action(2000, &refresh_timer))
    {
        lvinfo_refresh_layout();
        layout_dirty = 0;
    }
    
    if (top)
    {
        lvinfo_align_and_display(top_items, top_count, 0, get_ml_topbar_pos(), TOTAL_WIDTH, 32);
    }
    
    if (bottom)
    {
        lvinfo_align_and_display(bot_items, bot_count, 0, get_ml_bottombar_pos(), TOTAL_WIDTH, 32);
    }

    lvinfo_touch_draw_editor();
    
    give_semaphore(lvinfo_sem);
}

EXCLUDES(lvinfo_sem)
enum lvinfo_touch_field lvinfo_touch_field_at(int x, int y)
{
    enum lvinfo_touch_field result = LVINFO_TOUCH_NONE;
    int best_distance = INT_MAX;

    if (!lvinfo_sem)
        return result;

    take_semaphore(lvinfo_sem, 0);
    struct lvinfo_item ** items = 0;
    int count = 0;
    int top_y = get_ml_topbar_pos();
    int bottom_y = get_ml_bottombar_pos();

    if (y >= top_y - 20 && y < top_y + 52)
    {
        items = top_items;
        count = top_count;
    }
    else if (y >= bottom_y - 20 && y < bottom_y + 52)
    {
        items = bot_items;
        count = bot_count;
    }

    if (items)
    {
        for (int i = 0; i < count; i++)
        {
            struct lvinfo_item * item = items[i];
            int left = item->x - item->width / 2 - 40;
            int right = item->x + item->width / 2 + 40;
            int distance = ABS(x - item->x);
            const char * name = item->name;
            enum lvinfo_touch_field candidate = LVINFO_TOUCH_NONE;

            if (!is_active(item) || x < left || x > right)
                continue;

            if (!strcmp(name, "Aperture")) candidate = LVINFO_TOUCH_APERTURE;
            else if (!strcmp(name, "Shutter")) candidate = LVINFO_TOUCH_SHUTTER;
            else if (!strcmp(name, "ISO")) candidate = LVINFO_TOUCH_ISO;
            else if (!strcmp(name, "White Balance")) candidate = LVINFO_TOUCH_WB;
            else if (!strcmp(name, "Crop info")) candidate = LVINFO_TOUCH_CROP;
            else if (!strcmp(name, "FPS")) candidate = LVINFO_TOUCH_FPS;
            else if (!strcmp(name, "Bitdepth info")) candidate = LVINFO_TOUCH_BIT_DEPTH;

            /* Expanded targets may overlap; the closest visible field wins. */
            if (candidate != LVINFO_TOUCH_NONE && distance < best_distance)
            {
                result = candidate;
                best_distance = distance;
            }
        }
    }
    give_semaphore(lvinfo_sem);
    return result;
}

int lvinfo_touch_is_bar_area(int y)
{
    int top_y = get_ml_topbar_pos();
    int bottom_y = get_ml_bottombar_pos();
    return (y >= top_y - 20 && y < top_y + 52) ||
           (y >= bottom_y - 20 && y < bottom_y + 52);
}

void lvinfo_touch_editor_open(enum lvinfo_touch_field field)
{
    lvinfo_touch_field = field;
    lvinfo_touch_feedback_slot = -1;
    lvinfo_touch_menu_value[0][0] = '\0';
    lvinfo_touch_menu_value[1][0] = '\0';
    lvinfo_touch_menu_enabled[0] = 1;
    lvinfo_touch_menu_enabled[1] = 1;
    lens_display_set_dirty();
}

void lvinfo_touch_editor_close(void)
{
    lvinfo_touch_field = LVINFO_TOUCH_NONE;
    lvinfo_touch_feedback_slot = -1;
    bmp_fill(COLOR_EMPTY, LVINFO_TOUCH_CROP_X - 2,
             LVINFO_TOUCH_BOX_Y - 2,
             LVINFO_TOUCH_CROP_W + 4, LVINFO_TOUCH_BOX_H + 4);
    lens_display_set_dirty();
}

int lvinfo_touch_editor_is_open(void)
{
    return lvinfo_touch_field != LVINFO_TOUCH_NONE;
}

enum lvinfo_touch_field lvinfo_touch_editor_field(void)
{
    return lvinfo_touch_field;
}

void lvinfo_touch_editor_set_item(int slot, const char *value, int enabled)
{
    if (slot < 0 || slot > 1)
        return;
    snprintf(lvinfo_touch_menu_value[slot],
             sizeof(lvinfo_touch_menu_value[slot]), "%s", value ? value : "--");
    lvinfo_touch_menu_enabled[slot] = enabled;
    lens_display_set_dirty();
}

int lvinfo_touch_editor_item_enabled(int slot)
{
    return slot >= 0 && slot <= 1 && lvinfo_touch_menu_enabled[slot];
}

int lvinfo_touch_editor_hit_test(int x, int y, int *slot, int *sign)
{
    int box_x;
    int box_w;
    int arrow_cx;
    int arrow_left;
    int arrow_right;

    if (slot) *slot = -1;
    if (sign) *sign = 0;
    if (lvinfo_touch_field == LVINFO_TOUCH_NONE)
        return 0;

    box_x = lvinfo_touch_field == LVINFO_TOUCH_CROP
        ? LVINFO_TOUCH_CROP_X : LVINFO_TOUCH_SINGLE_X;
    box_w = lvinfo_touch_field == LVINFO_TOUCH_CROP
        ? LVINFO_TOUCH_CROP_W : LVINFO_TOUCH_SINGLE_W;
    if (x < box_x || x >= box_x + box_w ||
        y < LVINFO_TOUCH_BOX_Y || y >= LVINFO_TOUCH_BOX_Y + LVINFO_TOUCH_BOX_H)
        return 0;

    if (lvinfo_touch_field == LVINFO_TOUCH_CROP)
        *slot = x < LVINFO_TOUCH_CROP_X + LVINFO_TOUCH_CROP_W / 2 ? 0 : 1;
    else
        *slot = 0;

    arrow_cx = lvinfo_touch_field == LVINFO_TOUCH_CROP
        ? (*slot == 0 ? 232 : 488) : 360;
    arrow_left = arrow_cx - 65;
    arrow_right = arrow_cx + 65;

    /* Visible triangle is 60x26; use a comfortable 130x62 target around it,
     * without turning the rest of the black box into an adjustment target. */
    if (x >= arrow_left && x <= arrow_right &&
        y >= LVINFO_TOUCH_UP_TIP_Y - 18 &&
        y <= LVINFO_TOUCH_UP_TIP_Y + 43)
        *sign = 1;
    else if (x >= arrow_left && x <= arrow_right &&
             y >= LVINFO_TOUCH_DOWN_TIP_Y - 43 &&
             y <= LVINFO_TOUCH_DOWN_TIP_Y + 18)
        *sign = -1;

    return 1;
}

static void lvinfo_touch_feedback_clear(int timer, void *opaque)
{
    (void)timer;
    (void)opaque;
    lvinfo_touch_feedback_slot = -1;
    lens_display_set_dirty();
}

void lvinfo_touch_editor_feedback(int slot, int sign)
{
    lvinfo_touch_feedback_slot = slot;
    lvinfo_touch_feedback_sign = sign;
    lens_display_set_dirty();
    delayed_call(140, lvinfo_touch_feedback_clear, 0);
}

static void lvinfo_init()
{
    lvinfo_sem = create_named_semaphore("lvinfo_sem", 1);
}

INIT_FUNC("lvinfo", lvinfo_init);

