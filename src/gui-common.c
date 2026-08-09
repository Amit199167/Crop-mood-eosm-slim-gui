/** \file
 * Common GUI event handling code
 */
#include <dryos.h>
#include <propvalues.h>
#include <bmp.h>
#include <property.h>
#include <boot-hack.h>
#include <fps.h>
#include <zebra.h>
#include <lens.h>
#include <config.h>
#include <lvinfo.h>
#include <menu.h>
#include <menu-grid.h>
#include <module.h>

#if defined(FEATURE_AF_PATTERNS)
#include <af_patterns.h>
#endif

#if defined(CONFIG_LVAPP_HACK_RELOC) || defined(CONFIG_LVAPP_HACK_DEBUGMSG)
#define CONFIG_LVAPP_HACK
#endif

static int bottom_bar_dirty = 0;
static int last_time_active = 0;

#ifdef CONFIG_SLIM_MENUS
static uint32_t slim_touch_word(void *obj, int index)
{
    uint32_t p = (uint32_t)obj;

    /* Match spy_event's conservative pointer/value test. */
    if (!p || (p & 0xF0000000))
        return index == 0 ? p : 0;

    return ((volatile uint32_t *)p)[index];
}

static uint32_t slim_touch_nested_word(uint32_t p, int index)
{
    /* W1 observed on EOS M is a low-RAM pointer.  Probe only aligned, valid
     * looking RAM addresses; never dereference Canon's high/invalid values. */
    if (p < 0x00900000 || p >= 0x00A00000 || (p & 3))
        return 0;
    return ((volatile uint32_t *)p)[index];
}

int eosm_touch_get_xy(struct event *event, int *x, int *y)
{
    uint32_t w1 = slim_touch_word(event->obj, 1);
    uint32_t packed = slim_touch_nested_word(w1, 1);

    *x = packed & 0xFFFF;
    *y = (packed >> 16) & 0xFFFF;
    return *x < 720 && *y < 480;
}

#endif

int is_canon_bottom_bar_dirty() { return bottom_bar_dirty; }
int get_last_time_active() { return last_time_active; }

#ifdef CONFIG_SLIM_MENUS
/* While recording: ignore all touch. In idle movie Live View:
 * one tap = Quick Screen, two taps = grid launcher, three taps = Last Settings.
 * The tap router owns EOS M Live View touches, including the short boot-time
 * interval while Canon is still reporting an INFO display state. */
#define SLIM_TOUCH_TAP_WINDOW_MS 330
#define SLIM_TOUCH_BOOT_GESTURE_TIMEOUT_MS 20000
static int slim_touch_tap_count;
static int slim_touch_tap_deadline;
static int slim_touch_lv_pressed;
static int slim_touch_lv_control_consumed;
static int slim_touch_boot_tap_count;
static int slim_touch_boot_tap_deadline;
static int slim_touch_boot_pressed;
static int slim_touch_pending_menu_change;
static enum lvinfo_touch_field slim_touch_pending_field;
static int slim_touch_pending_slot;
static int slim_touch_pending_sign;
/* MODULE_FUNCTION requires the pointer variable and exported symbol to have
 * exactly the same name: the loader writes the resolved module address into
 * that variable.  Do not add an _fn suffix here. */
static int (*crop_rec_touch_adjust)(int, int) =
    MODULE_FUNCTION(crop_rec_touch_adjust);
static int (*crop_rec_touch_get_value)(int, int, char *, int, int *) =
    MODULE_FUNCTION(crop_rec_touch_get_value);
static int (*crop_rec_lv_transition_busy)() =
    MODULE_FUNCTION(crop_rec_lv_transition_busy);
static int (*dual_iso_is_enabled)() = MODULE_FUNCTION(dual_iso_is_enabled);
static int (*dual_iso_slim_step_recovery)(int) =
    MODULE_FUNCTION(dual_iso_slim_step_recovery);

static int slim_touch_dual_iso_enabled(void)
{
    return dual_iso_is_enabled && dual_iso_is_enabled();
}

static void slim_touch_step_dual_iso_recovery(int sign)
{
    /* Consume the ISO tap while Dual ISO is on, even at the end of its
     * permitted range.  Falling back to normal ISO would break the pair. */
    if (dual_iso_slim_step_recovery)
        dual_iso_slim_step_recovery(sign > 0 ? 1 : -1);
}

static int slim_crop_rec_transition_busy(void)
{
    return crop_rec_lv_transition_busy && crop_rec_lv_transition_busy();
}

/* Cache menu-backed values outside lvinfo's drawing semaphore.  This keeps
 * the normal menu selectors as the sole source of valid Mode/resolution/FPS
 * choices without creating a lock-order dependency in the overlay task. */
static void slim_touch_lv_refresh_menu_editor(enum lvinfo_touch_field field)
{
    int control = field == LVINFO_TOUCH_CROP ? 0 :
                  field == LVINFO_TOUCH_FPS ? 1 :
                  field == LVINFO_TOUCH_BIT_DEPTH ? 2 : -1;
    char value[32] = "--";
    int enabled = 0;

    if (control < 0)
        return;
    int value_ok = crop_rec_touch_get_value(
        control, 0, value, sizeof(value), &enabled);
    if (value_ok)
        lvinfo_touch_editor_set_item(0, value, enabled);
    else
        lvinfo_touch_editor_set_item(0, value, 0);
    if (control == 0)
    {
        enabled = 0;
        value_ok = crop_rec_touch_get_value(
            control, 1, value, sizeof(value), &enabled);
        lvinfo_touch_editor_set_item(1, value, value_ok && enabled);
    }
}

static void slim_touch_lv_refresh_menu_editor_delayed(int timer, void *opaque)
{
    (void)timer;
    (void)opaque;
    if (lvinfo_touch_editor_is_open())
        slim_touch_lv_refresh_menu_editor(lvinfo_touch_editor_field());
}

static void slim_touch_lv_apply_pending_menu_change(int timer, void *opaque)
{
    int changed = 0;
    (void)timer;
    (void)opaque;
    if (!slim_touch_pending_menu_change || !lvinfo_touch_editor_is_open())
        return;

    slim_touch_pending_menu_change = 0;
    if (slim_touch_pending_field == LVINFO_TOUCH_CROP)
    {
        changed = crop_rec_touch_adjust(
            slim_touch_pending_slot == 0 ? 0 : 1,
            slim_touch_pending_sign);
    }
    else if (slim_touch_pending_field == LVINFO_TOUCH_FPS)
    {
        changed = crop_rec_touch_adjust(2, slim_touch_pending_sign);
    }
    else if (slim_touch_pending_field == LVINFO_TOUCH_BIT_DEPTH)
    {
        changed = crop_rec_touch_adjust(3, slim_touch_pending_sign);
    }

    if (changed)
        lvinfo_touch_editor_feedback(
            slim_touch_pending_slot, slim_touch_pending_sign);
    slim_touch_lv_refresh_menu_editor(slim_touch_pending_field);
    delayed_call(500, slim_touch_lv_refresh_menu_editor_delayed, 0);
}

/* The editor itself is painted by lvinfo, after the status bars have been
 * laid out.  Keep input here with the other Live View touch routing. */
static void slim_touch_lv_change_field(enum lvinfo_touch_field field,
                                       int slot, int sign)
{
    int deferred = 0;
    if (!lvinfo_touch_editor_item_enabled(slot))
        return;

    switch (field)
    {
        case LVINFO_TOUCH_APERTURE:
            if (lens_info.lens_exists && lens_info.raw_aperture)
                aperture_toggle((void *)-1, sign);
            break;
        case LVINFO_TOUCH_SHUTTER:
            if (lens_info.raw_shutter)
                shutter_toggle((void *)-1, sign);
            break;
        case LVINFO_TOUCH_ISO:
            /* With Dual ISO, base ISO remains fixed and this editor steps
             * only the recovery ISO.  Otherwise retain the normal ISO path. */
            if (slim_touch_dual_iso_enabled())
                slim_touch_step_dual_iso_recovery(sign);
            else if (!menu_adjust_value_by_name("Expo", "ISO", sign))
                iso_toggle((void *)-1, sign);
            break;
        case LVINFO_TOUCH_WB:
            kelvin_toggle((void *)-1, sign);
            break;
        case LVINFO_TOUCH_CROP:
            deferred = 1;
            slim_touch_pending_menu_change = 1;
            slim_touch_pending_field = field;
            slim_touch_pending_slot = slot;
            slim_touch_pending_sign = sign;
            delayed_call(100, slim_touch_lv_apply_pending_menu_change, 0);
            break;
        case LVINFO_TOUCH_FPS:
            deferred = 1;
            slim_touch_pending_menu_change = 1;
            slim_touch_pending_field = field;
            slim_touch_pending_slot = slot;
            slim_touch_pending_sign = sign;
            delayed_call(100, slim_touch_lv_apply_pending_menu_change, 0);
            break;
        case LVINFO_TOUCH_BIT_DEPTH:
            deferred = 1;
            slim_touch_pending_menu_change = 1;
            slim_touch_pending_field = field;
            slim_touch_pending_slot = slot;
            slim_touch_pending_sign = sign;
            delayed_call(100, slim_touch_lv_apply_pending_menu_change, 0);
            break;
        default:
            break;
    }
    if (!deferred)
        lvinfo_touch_editor_feedback(slot, sign);
    lens_display_set_dirty();
}

static int slim_touch_lv_direct_editor(struct event * event)
{
    int x, y;
    if (!eosm_touch_get_xy(event, &x, &y))
        return 0;

    if (monitoring_graph_touch_toggle(x, y))
        return 1;

    if (lvinfo_touch_editor_is_open())
    {
        enum lvinfo_touch_field field = lvinfo_touch_editor_field();
        int slot = -1;
        int arrow = 0;

        if (!lvinfo_touch_editor_hit_test(x, y, &slot, &arrow))
            lvinfo_touch_editor_close();
        else if (arrow && slot >= 0)
            slim_touch_lv_change_field(field, slot, arrow);
        return 1;
    }

    enum lvinfo_touch_field field = lvinfo_touch_field_at(x, y);
    if (field == LVINFO_TOUCH_NONE)
        return 0;

    /* Do not open controls that have no valid camera-side adjustment. */
    if ((field == LVINFO_TOUCH_APERTURE &&
         (!lens_info.lens_exists || !lens_info.raw_aperture)) ||
        (field == LVINFO_TOUCH_SHUTTER && !lens_info.raw_shutter))
        return 1;

    lvinfo_touch_editor_open(field);
    /* A status-bar touch must cancel any unfinished Live View tap sequence;
     * otherwise its delayed single-tap action could open Quick Panel over the
     * editor a fraction of a second later. */
    slim_touch_tap_count = 0;
    slim_touch_tap_deadline = 0;
    if (field == LVINFO_TOUCH_CROP || field == LVINFO_TOUCH_FPS ||
        field == LVINFO_TOUCH_BIT_DEPTH)
        delayed_call(20, slim_touch_lv_refresh_menu_editor_delayed, 0);
    return 1;
}

static int slim_touch_lv_context_ok(void)
{
    return lv && is_movie_mode() && !RECORDING &&
           !gui_menu_shown() && lv_dispsize != 10 &&
           !slim_crop_rec_transition_busy();
}

static void slim_touch_open_for_taps(int taps)
{
    if (!slim_touch_lv_context_ok() || lvinfo_touch_editor_is_open())
        return;

    if (taps == 1)
    {
        menu_quick_screen_open();
        gui_open_menu();
    }
    else if (taps == 2)
    {
        /* Double-tap is the fast return to the last setting the user
         * changed. Its saved menu/entry pair is restored at boot. */
        gui_open_last_menu_selection();
    }
    else if (taps >= 3)
    {
        gui_open_last_menu_selection();
    }
}

/* A boot/menu-return transition may hold the crop guard for several seconds.
 * Do not turn an early double tap into a later single tap: retain the gesture
 * and apply it only after the validated Live View pipeline is ready. */
static void slim_touch_resolve_boot_taps(int timer, void *opaque)
{
    int taps;
    (void)timer;
    (void)opaque;

    if (!slim_touch_boot_tap_count)
        return;

    if (slim_crop_rec_transition_busy())
    {
        if (get_ms_clock() < slim_touch_boot_tap_deadline)
            delayed_call(100, slim_touch_resolve_boot_taps, 0);
        else
            slim_touch_boot_tap_count = 0;
        return;
    }

    taps = slim_touch_boot_tap_count;
    slim_touch_boot_tap_count = 0;
    slim_touch_boot_tap_deadline = 0;
    if (slim_touch_lv_context_ok() && !lvinfo_touch_editor_is_open())
        slim_touch_open_for_taps(taps);
}

static void slim_touch_resolve_taps(int timer, void *opaque)
{
    int remaining;
    int taps;
    (void)timer;
    (void)opaque;

    if (!slim_touch_tap_count)
        return;

    remaining = slim_touch_tap_deadline - get_ms_clock();
    if (remaining > 0)
    {
        delayed_call(remaining + 1, slim_touch_resolve_taps, 0);
        return;
    }

    taps = slim_touch_tap_count;
    slim_touch_tap_count = 0;
    slim_touch_tap_deadline = 0;
    slim_touch_open_for_taps(taps);
}

static void slim_touch_register_tap(void)
{
    slim_touch_tap_count++;
    if (slim_touch_tap_count >= 3)
    {
        slim_touch_tap_count = 0;
        slim_touch_tap_deadline = 0;
        slim_touch_open_for_taps(3);
        return;
    }

    slim_touch_tap_deadline =
        get_ms_clock() + SLIM_TOUCH_TAP_WINDOW_MS;
    delayed_call(
        SLIM_TOUCH_TAP_WINDOW_MS + 1,
        slim_touch_resolve_taps, 0);
}

static int handle_slim_rec_touch_block(struct event * event)
{
    if (slim_crop_rec_transition_busy())
    {
        /* The crop module is validating Canon's newly-created LV buffers.
         * Let no touch/dial shortcut alter another property in this window.
         * Image-area taps are retained so a deliberate boot-time double tap
         * remains a double tap once the guard has finished. */
        if (event->param == BGMT_TOUCH_1_FINGER)
        {
            int x, y;
            slim_touch_boot_pressed = eosm_touch_get_xy(event, &x, &y) &&
                !lvinfo_touch_is_bar_area(y);
            return 0;
        }
        if (event->param == BGMT_UNTOUCH_1_FINGER)
        {
            if (slim_touch_boot_pressed)
            {
                slim_touch_boot_pressed = 0;
                if (slim_touch_boot_tap_count < 3)
                    slim_touch_boot_tap_count++;
                slim_touch_boot_tap_deadline = get_ms_clock() +
                    SLIM_TOUCH_BOOT_GESTURE_TIMEOUT_MS;
                delayed_call(100, slim_touch_resolve_boot_taps, 0);
            }
            return 0;
        }
        if (lvinfo_touch_editor_is_open())
            lvinfo_touch_editor_close();
        slim_touch_tap_count = 0;
        slim_touch_tap_deadline = 0;
        slim_touch_lv_pressed = 0;
        slim_touch_lv_control_consumed = 0;
        return 0;
    }

    if (RECORDING)
    {
        /* Recording owns the entire touchscreen, independent of Global Draw.
         * Also discard any editor/tap state that began before REC engaged. */
        if (lvinfo_touch_editor_is_open())
            lvinfo_touch_editor_close();
        slim_touch_tap_count = 0;
        slim_touch_tap_deadline = 0;
        slim_touch_lv_pressed = 0;
        slim_touch_lv_control_consumed = 0;
    }

    switch (event->param)
    {
    case BGMT_TOUCH_1_FINGER:
    {
        int touch_x;
        int touch_y;
        if (RECORDING)
            return 0;
        if (slim_touch_lv_context_ok())
        {
            slim_touch_lv_control_consumed = slim_touch_lv_direct_editor(event);
            if (slim_touch_lv_control_consumed)
                return 0;
            /* Tap gestures (including Quick Panel) only originate from the
             * image area. Unassigned space in either status bar is inert. */
            if (!eosm_touch_get_xy(event, &touch_x, &touch_y) ||
                lvinfo_touch_is_bar_area(touch_y))
            {
                slim_touch_lv_control_consumed = 1;
                slim_touch_lv_pressed = 0;
                return 0;
            }
            slim_touch_lv_pressed = 1;
            /* Keep a previous tap pending while the next finger press is held. */
            if (slim_touch_tap_count)
                slim_touch_tap_deadline =
                    get_ms_clock() + SLIM_TOUCH_TAP_WINDOW_MS;
            return 0;
        }
        break;
    }
#ifdef BGMT_TOUCH_MOVE
    case BGMT_TOUCH_MOVE:
        if (RECORDING)
            return 0;
        if (slim_touch_lv_context_ok())
            return 0;
        break;
#endif
    case BGMT_TOUCH_2_FINGER:
        slim_touch_tap_count = 0;
        slim_touch_tap_deadline = 0;
        slim_touch_lv_pressed = 0;
        if (RECORDING)
            return 0;
        if (slim_touch_lv_context_ok())
        {
            /* Keep the existing simultaneous two-finger Last Settings shortcut. */
            gui_open_last_menu_selection();
            return 0;
        }
        break;
    case BGMT_UNTOUCH_1_FINGER:
        slim_touch_scroll_cancel();
        if (RECORDING)
        {
            slim_touch_lv_pressed = 0;
            return 0;
        }
        if (slim_touch_lv_context_ok())
        {
            if (slim_touch_lv_control_consumed)
            {
                slim_touch_lv_control_consumed = 0;
                slim_touch_lv_pressed = 0;
                return 0;
            }
            if (slim_touch_lv_pressed)
                slim_touch_register_tap();
            slim_touch_lv_pressed = 0;
            return 0;
        }
        slim_touch_lv_pressed = 0;
        break;

    case BGMT_UNTOUCH_2_FINGER:
        slim_touch_scroll_cancel();
#ifdef BGMT_TOUCH_MOVE
    /* handled above so the diagnostic overlay can sample movement */
#endif
#ifdef BGMT_TOUCH_PINCH_START
    case BGMT_TOUCH_PINCH_START:
#endif
#ifdef BGMT_TOUCH_PINCH_STOP
    case BGMT_TOUCH_PINCH_STOP:
#endif
        if (RECORDING)
            return 0;
        if (slim_touch_lv_context_ok())
            return 0;
        break;
    }
    return 1;
}

/* The Live View editor is touch-driven.  While it is visible, keep the rear
 * dial pad and assigned shortcuts from changing an unrelated setting behind
 * it. MENU remains available and dismisses the editor before normal handling. */
static int handle_slim_lv_editor_keys(struct event *event)
{
    if (!lvinfo_touch_editor_is_open())
        return 1;

    if (event->param == BGMT_MENU)
    {
        lvinfo_touch_editor_close();
        slim_touch_tap_count = 0;
        slim_touch_tap_deadline = 0;
        return 1;
    }

    switch (event->param)
    {
        case BGMT_PRESS_UP:
        case BGMT_UNPRESS_UP:
        case BGMT_PRESS_DOWN:
        case BGMT_UNPRESS_DOWN:
        case BGMT_PRESS_LEFT:
        case BGMT_UNPRESS_LEFT:
        case BGMT_PRESS_RIGHT:
        case BGMT_UNPRESS_RIGHT:
        case BGMT_WHEEL_UP:
        case BGMT_WHEEL_DOWN:
        case BGMT_WHEEL_LEFT:
        case BGMT_WHEEL_RIGHT:
        case BGMT_PRESS_SET:
        case BGMT_UNPRESS_SET:
#ifdef BGMT_Q_SET
        case BGMT_Q_SET:
#endif
        case BGMT_INFO:
#ifdef BGMT_UNPRESS_INFO
        case BGMT_UNPRESS_INFO:
#endif
            return 0;
        default:
            return 1;
    }
}

/* Idle movie LV (not recording, ML overlays): open last changed setting from INFO.
 *
 * Prefer true long-press (1s) when BGMT_UNPRESS_INFO is observed.
 * EOS M historically has no INFO unpress in gui.h; qemu also omits it. Until an
 * unpress is seen, use double-press INFO (2nd press within ~450ms) instead, and
 * defer the short press so Settings → INFO Button still works on a single tap.
 *
 * Short press → re-inject INFO for Settings → INFO Button mapping. */
static int slim_info_lp_pressed;
static int slim_info_lp_count;
static int slim_info_lp_long_fired;
static int slim_info_have_unpress;   /* latched after first BGMT_UNPRESS_INFO */
static int slim_info_pending_short;  /* double-press / press-only path */

static int slim_info_lp_context_ok(void)
{
    return lv && is_movie_mode() && !RECORDING && !gui_menu_shown() && lv_disp_mode == 0;
}

static void slim_info_lp_tick(int timer, void * opaque)
{
    (void)timer;
    (void)opaque;

    if (!slim_info_lp_pressed)
        return;

    if (!slim_info_lp_context_ok())
    {
        slim_info_lp_pressed = 0;
        slim_info_lp_count = 0;
        return;
    }

    slim_info_lp_count++;
    /* 50 * 20ms = 1000ms */
    if (slim_info_lp_count >= 50)
    {
        slim_info_lp_long_fired = 1;
        slim_info_lp_pressed = 0;
        slim_info_lp_count = 0;
        gui_open_last_menu_selection();
        return;
    }

    delayed_call(20, slim_info_lp_tick, 0);
}

static void slim_info_deferred_short(int timer, void * opaque)
{
    (void)timer;
    (void)opaque;

    if (!slim_info_pending_short)
        return;
    slim_info_pending_short = 0;
    if (slim_info_lp_context_ok())
        fake_simple_button(BGMT_INFO);
}

static void slim_info_lp_release(int fire_short)
{
    int count = slim_info_lp_count;
    int long_fired = slim_info_lp_long_fired;

    slim_info_lp_pressed = 0;
    slim_info_lp_count = 0;
    slim_info_lp_long_fired = 0;

    /* Released before 1s → normal INFO (Settings mapping in crop_rec). */
    if (fire_short && !long_fired && count < 50)
        fake_simple_button(BGMT_INFO);
}

static int handle_slim_info_longpress(struct event * event)
{
    /* Fake INFO is our short-press re-inject — never re-arm. */
    if (event->param == BGMT_INFO && IS_FAKE(event))
        return 1;

    if (event->param == BGMT_INFO)
    {
        if (!slim_info_lp_context_ok())
            return 1;

        /* True long-press path (INFO release events are available). */
        if (slim_info_have_unpress)
        {
            if (slim_info_lp_pressed)
            {
                slim_info_lp_release(1);
                return 0;
            }
            slim_info_lp_pressed = 1;
            slim_info_lp_count = 0;
            slim_info_lp_long_fired = 0;
            delayed_call(20, slim_info_lp_tick, 0);
            return 0;
        }

        /* Press-only path: double-press INFO → last setting; single → deferred short. */
        if (slim_info_pending_short)
        {
            slim_info_pending_short = 0;
            gui_open_last_menu_selection();
            return 0;
        }
        slim_info_pending_short = 1;
        delayed_call(450, slim_info_deferred_short, 0);
        return 0;
    }

#ifdef BGMT_UNPRESS_INFO
    if (event->param == BGMT_UNPRESS_INFO)
    {
        slim_info_have_unpress = 1;

        /* First unpress may arrive while press-only deferred short is pending. */
        if (slim_info_pending_short)
        {
            slim_info_pending_short = 0;
            fake_simple_button(BGMT_INFO);
            return 0;
        }

        if (slim_info_lp_pressed || slim_info_lp_long_fired)
        {
            slim_info_lp_release(1);
            return 0;
        }
        return 1;
    }
#endif

    /* Other real buttons cancel a pending long-press / deferred short. */
    if (event->param >= 0 && event->param != GMT_OLC_INFO_CHANGED)
    {
        if (slim_info_lp_pressed)
            slim_info_lp_release(0);
        slim_info_pending_short = 0;
    }

    return 1;
}
#endif

// disable Canon bottom bar

#if defined(CONFIG_LVAPP_HACK_DEBUGMSG) || defined(CONFIG_LVAPP_HACK)
static int bottom_bar_hack = 0;
#endif

#if defined(CONFIG_LVAPP_HACK_DEBUGMSG)

#ifdef CONFIG_5D3
extern int cf_card_workaround;
#endif

static void hacked_DebugMsg(int class, int level, char* fmt, ...)
{
    #if defined(CONFIG_LVAPP_HACK_DEBUGMSG)
    if (bottom_bar_hack && class == 131 && level == 1)
    {
        MEM(JUDGE_BOTTOM_INFO_DISP_TIMER_STATE) = 0;
    }
    #endif

    #ifdef CONFIG_5D3
    if (cf_card_workaround)
    {
        if (class == 34 && level == 1) // cfDMAWriteBlk
        {
            for (int i = 0; i < 10000; i++) 
                asm("nop");
        }
    }
    #endif

#ifdef FRAME_SHUTTER_BLANKING_WRITE
    if (class == 145) /* 5D3-specific? */
        fps_override_shutter_blanking();
#endif

    return;
}

static uint32_t orig_DebugMsg_instr = 0;

static void DebugMsg_hack()
{
    if (!orig_DebugMsg_instr)
    {
        uint32_t d = (uint32_t)&DryosDebugMsg;
        orig_DebugMsg_instr = *(uint32_t*)(d);
        *(uint32_t*)(d) = B_INSTR((uint32_t)&DryosDebugMsg, hacked_DebugMsg);
    }
}

static void DebugMsg_uninstall()
{
    // uninstall our mean hack (not used)
    
    if (orig_DebugMsg_instr)
    {
        uint32_t d = (uint32_t)&DryosDebugMsg;
        *(uint32_t*)(d) = orig_DebugMsg_instr;
        orig_DebugMsg_instr = 0;
    }
}

INIT_FUNC("debugmsg-hack", DebugMsg_hack);

#endif // CONFIG_LVAPP_HACK_DEBUGMSG

int handle_other_events(struct event * event)
{
    extern int ml_started;
    if (!ml_started) return 1;

#ifdef CONFIG_LVAPP_HACK

    unsigned short int lv_refreshing = lv && event->type == 2 && event->param == GMT_LOCAL_DIALOG_REFRESH_LV;
    unsigned short int should_hide = lv_disp_mode == 0 && get_global_draw_setting() && liveview_display_idle() && lv_dispsize == 1;
    
    if(lv_refreshing)
    {
        if(should_hide)
        {
            #ifdef CONFIG_LVAPP_HACK_RELOC
            extern void reloc_liveviewapp_install();  /* liveview.c */
            reloc_liveviewapp_install();
            #endif
            
            bottom_bar_hack = 1;

            if (get_halfshutter_pressed()) bottom_bar_dirty = 10;

            #ifdef UNAVI_FEEDBACK_TIMER_ACTIVE
            /*
             * Hide Canon's Q menu (aka UNAVI) as soon as the user quits it.
             * 
             * By default, this menu remains on screen for a few seconds.
             * After it disappears, we would have to redraw cropmarks, zebras and so on,
             * which looks pretty ugly, since our redraw is slow.
             * Better hide the menu right away, then redraw - it feels a lot less sluggish.
             */
            if (UNAVI_FEEDBACK_TIMER_ACTIVE)
            {
                /* Canon stub */
                extern void HideUnaviFeedBack_maybe();
                HideUnaviFeedBack_maybe();
                bottom_bar_dirty = 0;
            }
            #endif
        }
        else
        {
            #ifdef CONFIG_LVAPP_HACK_RELOC
            extern void reloc_liveviewapp_uninstall();  /* liveview.c */
            reloc_liveviewapp_uninstall();
            #endif

            bottom_bar_hack  = 0;
            bottom_bar_dirty = 0;
        }

        /* Redraw ML bottom bar if Canon bar was displayed over it */
        if (!liveview_display_idle()) bottom_bar_dirty = 0;
        if (bottom_bar_dirty) bottom_bar_dirty--;
        if (bottom_bar_dirty == 1)
        {
            lens_display_set_dirty();
        }
    }
#endif
    return 1;
}

int handle_common_events_startup(struct event * event)
{   
    extern int ml_gui_initialized;
    ml_gui_initialized = 1;
    
    if (handle_tricky_canon_calls(event) == 0) return 0;

    extern int ml_started;
    if (!ml_started)    {
#if defined(BGMT_Q_SET) // combined Q/SET button?
        if (event->param == BGMT_Q_SET) { _disable_ml_startup(); return 0;} // don't load ML
#else
        if (event->param == BGMT_PRESS_SET) { _disable_ml_startup(); return 0;} // don't load ML
#endif
        
        if (handle_select_config_file_by_key_at_startup(event) == 0) return 0;

        #ifdef CONFIG_60D
        if (event->param == BGMT_MENU) return 0; // otherwise would interfere with swap menu-erase
        #endif
        
        #if !defined(CONFIG_50D) && !defined(CONFIG_5D2) && !defined(CONFIG_5D3) && !defined(CONFIG_650D) && !defined(CONFIG_700D) && !defined(CONFIG_100D)
        if (event->param == BGMT_LV) return 0; // discard REC button if it's pressed too early
        #endif
        
        #ifdef CONFIG_5D3
        // block LV button at startup to avoid lockup with manual lenses (Canon bug?)
        if (event->param == BGMT_LV && !lv && (lv_movie_select == 0 || is_movie_mode()) && !GUIMODE_MOVIE_ENSURE_A_LENS_IS_ATTACHED && !GUIMODE_MOVIE_PRESS_LV_TO_RESUME)
            return 0;
        #endif
                
        return 1; // don't alter any other buttons/events until ML is fully initialized
    }
    return 1;
}

static int pre_shutdown_requested = 0; // used for preventing wakeup from paused LiveView at shutdown (causes race condition with Canon code and crashes)

void reset_pre_shutdown_flag_step() // called every second
{
    if (pre_shutdown_requested && !sensor_cleaning)
    {
        pre_shutdown_requested--;
        
        if (!pre_shutdown_requested)
        {
            /* false shutdown alarm? */
            info_led_off();
            _card_led_off();
        }
    }
}

void check_pre_shutdown_flag() // called from ml_shutdown
{
    if (LV_PAUSED && !pre_shutdown_requested)
    {
        // if this happens, camera will probably not shutdown normally from "LV paused" state
        NotifyBox(10000, "Double-check GMT_GUICMD consts");
        info_led_blink(50,50,50);
        // can't call ASSERT from here
    }
}

// ML doesn't know how to handle multiple button clicks in the same event code,
// so we'll split them in individual events

// this has some side effects on Canon code, visible e.g. on 5d3 when adjusting WB,
// so for now we'll only call it when needed (e.g. menu, scripts)
int handle_scrollwheel_fast_clicks(struct event * event)
{
    if (event->arg <= 1) return 1;
    
    if (event->param == BGMT_WHEEL_UP || event->param == BGMT_WHEEL_DOWN || event->param == BGMT_WHEEL_LEFT ||  event->param == BGMT_WHEEL_RIGHT)
    {
        for (int i = 0; i < event->arg; i++)
            GUI_Control(event->param, 0, 1, 0);
        return 0;
    }
    return 1;
}

/* Q is always defined */
/* if some models don't have it, we are going to use some other button instead. */
/* some mappings are valid for cameras with a Q button as well */
static int handle_Q_button_equiv(struct event * event)
{
    /* Some cameras (at least 600D and 1100D) use two button codes for Q,
     * depending on operating mode (e.g. Canon menu vs LiveView)
     * Canon firmware happily reacts to both codes, in other words,
     * fake_simple_button appears to work with any of them;
     * to keep things simple, we'll just remap the "alternate" code
     * so the rest of ML will just use BGMT_Q to handle both codes */
    switch (event->param)
    {
#ifdef BGMT_Q_ALT_
    case BGMT_Q_ALT_:
#endif
        fake_simple_button(BGMT_Q);
        return 0;
    }

    if (!gui_menu_shown())
    {
        /* only remap other buttons while in ML menu */
        /* note: in ML menu, these buttons will no longer be available
         * to other modules/scripts directly (they will be all seen as Q).
         * outside ML menu, they retain their regular functionality.
         */
        return 1;
    }

    switch (event->param)
    {
#ifdef BGMT_Q_ALT
    #error please use BGMT_Q
#endif
#ifdef BGMT_RATE
    case BGMT_RATE:
#endif
#if defined(CONFIG_5D2) || defined(CONFIG_7D)
    case BGMT_PICSTYLE:
#endif
#ifdef CONFIG_50D
    case BGMT_FUNC:
#endif
#ifdef CONFIG_500D
    case BGMT_LV:
#endif
#ifdef CONFIG_5DC
    case BGMT_JUMP:
    case BGMT_PRESS_DIRECT_PRINT:
#endif
        fake_simple_button(BGMT_Q);
        return 0;
    }
    
    return 1;
}

#ifdef CONFIG_MENU_WITH_AV
int bgmt_av_status;
int get_bgmt_av_status() {
    return bgmt_av_status;
}

/** 
 * FIXME: Totally statically-coded for 1100D..
 * But at least Canon decided to keep the TRASH button
 * in the newer models...Thanks!
 */
int update_bgmt_av_status(struct event * event) {
    if(!BGMT_AV) return -1;
    if(event == NULL) return -1;
    if(event->obj == NULL) return -1;
    int gmt_int_ev_obj = *(int*)(event->obj);
    switch(shooting_mode) {
        case SHOOTMODE_MOVIE:
        case SHOOTMODE_P:
        case SHOOTMODE_ADEP:
            if(gmt_int_ev_obj == 0x3010040) return 1;
            if(gmt_int_ev_obj == 0x1010040) return 0;
            break;
        case SHOOTMODE_M:
            if(gmt_int_ev_obj == 0x1010006) return 1;
            if(gmt_int_ev_obj == 0x3010006) return 0;
            break;
        case SHOOTMODE_AV:
        case SHOOTMODE_TV:
            if(gmt_int_ev_obj == (0x1010040+2*shooting_mode)) return 1;
            if(gmt_int_ev_obj == (0x3010040+2*shooting_mode)) return 0;
            break;
        default:
            break;
    }
    return -1; //Annoying compiler :)
}

/** AV long/short press management code. **/
int handle_av_short_for_menu(struct event* event) {
    static int t_press   = 0;
    static int t_unpress = 0;
    unsigned int dt = 0;
    unsigned int is_idle = (gui_menu_shown() || gui_state == GUISTATE_IDLE);
    bgmt_av_status = update_bgmt_av_status(event);

    /* Assumes that the press event is fired only once 
     * even if the button is held
     */ 
    if(bgmt_av_status == 1) { // AV PRESSED
        t_press = get_ms_clock();
        dt = t_press - t_unpress; // Time elapsed since the button was unpressed
        if(dt < 200) { // Ignore if happened less than 200ms ago (anti-bump)
            t_press = 0; 
        } 
    } else if (bgmt_av_status == 0) { // AV UNPRESSED
        t_unpress = get_ms_clock();
        dt = t_unpress - t_press; // Time elapsed since the AV button was pressed
        if (dt < 500 && is_idle) { // 500ms  -> short press
            fake_simple_button(BGMT_TRASH);
        }
    }
    //NotifyBox(1000, "AV DEBUG: S %d I %d DT %d", bgmt_av_status, is_idle, dt);
    return 1;
} 
#endif //CONFIG_MENU_WITH_AV

#ifdef FEATURE_DIGITAL_ZOOM_SHORTCUT
PROP_INT(PROP_DIGITAL_ZOOM_RATIO, digital_zoom_ratio);

int video_mode[5];
PROP_HANDLER(PROP_VIDEO_MODE)
{
    memcpy(video_mode, buf, 20);
}

int disp_pressed = 0;
int get_disp_pressed() { return disp_pressed; }
int disp_zoom_pressed = 0;

int handle_digital_zoom_shortcut(struct event * event)
{
    switch(event->param) {
        case BGMT_PRESS_DISP:
            disp_pressed = 1; 
            disp_zoom_pressed = 0; 
            break;
        case BGMT_UNPRESS_DISP:
            disp_pressed = 0;
            break;
        case BGMT_PRESS_ZOOM_IN: 
        case BGMT_PRESS_ZOOM_OUT:
            disp_zoom_pressed = 1;
            break;
        default:
            break;
    }

    extern int digital_zoom_shortcut;
    if (digital_zoom_shortcut && lv && is_movie_mode() && disp_pressed)
    {
        if (!video_mode_crop)
        {
            if (video_mode_resolution == 0 && event->param == BGMT_PRESS_ZOOM_IN)
            {
                if (NOT_RECORDING)
                {
                    video_mode[0] = 0xc;
                    video_mode[4] = 2;
                    prop_request_change(PROP_VIDEO_MODE, video_mode, 20);
                }
                return 0;
            }
        }
        else
        {
            if (event->param == BGMT_PRESS_ZOOM_IN)
            {
                if (NOT_RECORDING)
                {
                    int x = 300;
                    prop_request_change(PROP_DIGITAL_ZOOM_RATIO, &x, 4);
                }
                NotifyBox(2000, "Zoom greater than 3x is disabled.\n");
                return 0; // don't allow more than 3x zoom
            }
            if (event->param == BGMT_PRESS_ZOOM_OUT)
            {
                if (NOT_RECORDING)
                {
                    video_mode[0] = 0;
                    video_mode[4] = 0;
                    prop_request_change(PROP_VIDEO_MODE, video_mode, 20);
                }
                return 0;
            }
        }
    }
    return 1;
}
#endif //FEATURE_DIGITAL_ZOOM_SHORTCUT

static int null_event_handler(struct event * event) { return 1; }
int handle_module_keys(struct event * event) __attribute__((weak,alias("null_event_handler")));
int handle_flexinfo_keys(struct event * event) __attribute__((weak,alias("null_event_handler")));

int handle_common_events_by_feature(struct event * event)
{
    // common to most cameras
    // there may be exceptions

    /* log button codes, if enabled from the Debug menu */
    spy_event(event);

#ifdef FEATURE_POWERSAVE_LIVEVIEW
    // these are required for correct shutdown from "LV paused" state
    if (event->param == GMT_GUICMD_START_AS_CHECK || 
        event->param == GMT_GUICMD_OPEN_SLOT_COVER || 
        event->param == GMT_GUICMD_LOCK_OFF)
    {
        pre_shutdown_requested = 4;
        info_led_on(); _card_led_on();
        return 1;
    }

    if (LV_PAUSED && event->param != GMT_OLC_INFO_CHANGED && event->param >= 0) 
    { 
        int ans = (ml_shutdown_requested || pre_shutdown_requested || sensor_cleaning);
        idle_wakeup_reset_counters(event->param);
        if (handle_disp_preset_key(event) == 0) return 0;
        return ans;  // if LiveView was resumed, don't do anything else (just wakeup)
    }
#endif

    if (event->param != GMT_OLC_INFO_CHANGED && event->param >= 0)
    {
        /* powersave: ignore internal Canon events and ML events, but wake up on any other key press */
        idle_wakeup_reset_counters(event->param);
    }

    
    // If we're here, we're dealing with a button press.  Record the timestamp
    // as a record of when the user was last actively pushing buttons.
    if (event->param != GMT_OLC_INFO_CHANGED)
        last_time_active = get_seconds_clock();

#ifdef CONFIG_SLIM_MENUS
    if (handle_slim_rec_touch_block(event) == 0) return 0;
    if (handle_slim_lv_editor_keys(event) == 0) return 0;
    if (handle_slim_info_longpress(event) == 0) return 0;
#endif

    /* convert Q replacement events into BGMT_Q */
    if (handle_Q_button_equiv(event) == 0) return 0;

    #ifdef CONFIG_MENU_WITH_AV
    if (handle_av_short_for_menu(event) == 0) return 0;
    #endif

    /* before module_keys, to be able to process long-press SET/Q events and forward them to modules/scripts */
    if (handle_longpress_events(event) == 0) return 0;

    #ifdef FEATURE_MAGIC_ZOOM
    /* must be before handle_module_keys to allow zoom while recording raw,
     * but also let the raw recording modules block the zoom keys to avoid crashing */
    if (handle_zoom_overlay(event) == 0) return 0;
    #endif

    if (handle_module_keys(event) == 0) return 0;
    if (handle_flexinfo_keys(event) == 0) return 0;

    #ifdef FEATURE_DIGITAL_ZOOM_SHORTCUT
    if (handle_digital_zoom_shortcut(event) == 0) return 0;
    #endif

    #ifdef FEATURE_UPSIDE_DOWN
    if (handle_upside_down(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_SWAP_MENU_ERASE
    if (handle_swap_menu_erase(event) == 0) return 0;
    #endif

    #ifdef FEATURE_SWAP_INFO_PLAY
    if (handle_swap_info_play(event) == 0) return 0;
    #endif

    if (handle_ml_menu_erase(event) == 0) return 0;
    if (handle_ml_menu_keys(event) == 0) return 0;
    
    #ifdef CONFIG_DIGIC_POKE
    if (handle_digic_poke(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_MLU_HANDHELD
    if (handle_mlu_handheld(event) == 0) return 0;
    #endif
    
    if (RECORDING && event->param == BGMT_MENU) redraw(); // MENU while RECORDING => force a redraw
    
    if (handle_buttons_being_held(event) == 0) return 0;
    //~ if (handle_morse_keys(event) == 0) return 0;

    #ifdef FEATURE_ZOOM_TRICK_5D3 // not reliable
    if (handle_zoom_trick_event(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_INTERVALOMETER
    if (handle_intervalometer(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_GHOST_IMAGE
    if (handle_transparent_overlay(event) == 0) return 0; // on 500D, these two share the same key
    #endif
    
    #if defined(FEATURE_OVERLAYS_IN_PLAYBACK_MODE)
    if (handle_overlays_playback(event) == 0) return 0;
    #endif

    #if defined(FEATURE_SET_MAINDIAL) || defined(FEATURE_QUICK_ERASE)
    if (handle_set_wheel_play(event) == 0) return 0;
    #endif

    #ifdef FEATURE_ARROW_SHORTCUTS
    if (handle_arrow_keys(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_TRAP_FOCUS
    if (handle_trap_focus(event) == 0) return 0;
    #endif

    #ifdef FEATURE_FOLLOW_FOCUS
    if (handle_follow_focus(event) == 0) return 0;
    if (handle_follow_focus_save_restore(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_LV_ZOOM_SETTINGS
    if (handle_zoom_x5_x10(event) == 0) return 0;
    #endif
    
    #if !defined(CONFIG_50D) && !defined(CONFIG_5D2) && !defined(CONFIG_5D3) && !defined(CONFIG_6D)
    if (handle_quick_access_menu_items(event) == 0) return 0;
    #endif
    
#ifdef CONFIG_RESTORE_AFTER_FORMAT
    if (handle_keep_ml_after_format_toggle(event) == 0) return 0;
#endif
        
    #ifdef FEATURE_FPS_OVERRIDE
    if (handle_fps_events(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_EXPO_PRESET
    if (handle_expo_preset(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_LV_DISPLAY_PRESETS
    if (handle_disp_preset_key(event) == 0) return 0;
    #endif
    
    #if defined(FEATURE_QUICK_ZOOM) || defined(FEATURE_REMEMBER_LAST_ZOOM_POS_5D3)
    if (handle_fast_zoom_in_play_mode(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_LV_FOCUS_BOX_FAST
    if (handle_fast_zoom_box(event) == 0) return 0;
    #endif
    
    #ifdef FEATURE_AF_PATTERNS
    if (handle_af_patterns(event) == 0) return 0;
    #endif

    #ifdef FEATURE_VOICE_TAGS
    if (handle_voice_tags(event) == 0) return 0;
    #endif

    #if defined(FEATURE_LV_BUTTON_PROTECT) || defined(FEATURE_LV_BUTTON_RATE)
    if (handle_lv_play(event) == 0) return 0;
    #endif
    
    /* if nothing else uses the arrow keys, use them for moving the focus box */
    /* (some cameras may block it in certain modes) */
    if (handle_lv_afframe_workaround(event) == 0) return 0;

    return 1;
}

int detect_double_click(int key, int pressed_code, int unpressed_code)
{
    /*       pressed    unpressed   pressed   unpressed */
    /* ____|``````````|__________|``````````|__________ */
    /*    tp1        tu1        tp2        tu2=t        */
    /*     |<---p1--->|<---u1--->|<---p2--->|           */
    
    /* note: half-shutter events may come twice, e.g. P P U U, so we need to ignore some of them */
    
    static int tp1 = 0;
    static int tu1 = 0;
    static int tp2 = 0;
    static int last_was_pressed = 0;

    if (key == pressed_code && !last_was_pressed)
    {
        last_was_pressed = 1;
        int t = get_ms_clock();
        tp1 = tu1;
        tu1 = tp2;
        tp2 = t;
    }
    else if (key == unpressed_code && last_was_pressed)
    {
        last_was_pressed = 0;
        int tu2 = get_ms_clock();
        int p1 = tu1 - tp1;
        int u1 = tp2 - tu1;
        int p2 = tu2 - tp2;

        // bmp_printf(FONT_MED, 100, 100, "%d %d %d  ", p1, u1, p2);
        
        if ((ABS(p1 - 120) < 80) && (ABS(u1 - 120) < 80) && (ABS(p2 - 120) < 80))
        {
            tp1 = tu1 = tp2 = tu2 = 0;
            return 1;
        }
        
        tp1 = tu1;
        tu1 = tp2;
        tp2 = tu2;
    }
    return 0;
}

char* get_info_button_name() { return INFO_BTN_NAME; }

void gui_uilock(int what)
{
    int old = icu_uilock;

    if ((icu_uilock & 0xFFFF) != UILOCK_NONE && what != UILOCK_NONE)
    {
        /* this is needed when going from some locked state to a different locked state */
        int unlocked = UILOCK_REQUEST | (UILOCK_NONE & 0xFFFF);
        prop_request_change_wait(PROP_ICU_UILOCK, &unlocked, 4, 2000);
    }

    /* change just the lower 16 bits, to ensure correct requests;
     * the higher bits appear to be for requesting the change */
    what = UILOCK_REQUEST | (what & 0xFFFF);
    prop_request_change_wait(PROP_ICU_UILOCK, &what, 4, 2000);

    printf("UILock: %08x -> %08x => %08x %s\n", old, what, icu_uilock, (icu_uilock & 0xFFFF) != (what & 0xFFFF) ? "(!!!)" : "");
}

void fake_simple_button(int bgmt_code)
{
    if ((icu_uilock & (0xFFFF & ~UILOCK_SHUTTER)) && (bgmt_code >= 0))
    {
        // Canon events may not be safe to send when UI is locked; ML events are (and should be sent)
        printf("fake_simple_button(%d): UI locked (%x)\n", bgmt_code, icu_uilock);
        return;
    }

    if (ml_shutdown_requested) return;
    GUI_Control(bgmt_code, 0, FAKE_BTN, 0);
}

int display_is_on()
{
    return DISPLAY_IS_ON;
}

void delayed_call(int delay_ms, void(*function)(), void* arg)
{
    SetTimerAfter(delay_ms, (timerCbr_t)function, (timerCbr_t)function, arg);
}

static void redraw_after_cbr()
{
    redraw();   /* this one simply posts an event to the GUI task */
}

void redraw_after(int msec)
{
    delayed_call(msec, redraw_after_cbr, 0);
}

int get_gui_mode()
{
    /* this is GUIMode from SetGUIRequestMode */
    return CURRENT_GUI_MODE;
}

/* enter PLAY mode */
void enter_play_mode()
{
    if (PLAY_MODE) return;
    
    /* request new mode */
    SetGUIRequestMode(GUIMODE_PLAY);

    /* wait up to 2 seconds to enter the PLAY mode */
    for (int i = 0; i < 20 && !PLAY_MODE; i++)
    {
        msleep(100);
    }

    /* also wait for display to come up, up to 1 second */
    for (int i = 0; i < 10 && !DISPLAY_IS_ON; i++)
    {
        msleep(100);
    }
    
    /* wait a little extra for the new mode to settle */
    msleep(500);
}

/* fixme: duplicate code (similar to enter_play_mode) */
void enter_menu_mode()
{
    if (MENU_MODE) return;
    
    /* request new mode */
    SetGUIRequestMode(GUIMODE_MENU);

    /* wait up to 2 seconds to enter the MENU mode */
    for (int i = 0; i < 20 && !MENU_MODE; i++)
    {
        msleep(100);
    }

    /* also wait for display to come up, up to 1 second */
    for (int i = 0; i < 10 && !DISPLAY_IS_ON; i++)
    {
        msleep(100);
    }
    
    /* wait a little extra for the new mode to settle */
    msleep(500);
}

/* exit from PLAY/QR/MENU modes (to LiveView or plain photo mode) */
void exit_play_qr_menu_mode()
{
    /* request new mode */
    SetGUIRequestMode(0);

    /* wait up to 2 seconds */
    for (int i = 0; i < 20 && PLAY_OR_QR_MODE; i++)
    {
        msleep(100);
    }

    /* if in LiveView, wait for the first frame */
    if (lv)
    {
        wait_lv_frames(1);
    }

    /* wait for display to come up, up to 1 second */
    for (int i = 0; i < 10 && !DISPLAY_IS_ON; i++)
    {
        msleep(100);
    }

    /* wait for any remaining GUI stuff to settle, up to 2 seconds */
    for (int i = 0; i < 20 && !display_idle(); i++)
    {
        msleep(100);
    }
}

/* same as above, but only from PLAY or QR modes */
void exit_play_qr_mode()
{
    /* not there? */
    if (!PLAY_OR_QR_MODE) return;
    exit_play_qr_menu_mode();
}

/* same as above, but only from MENU mode */
void exit_menu_mode()
{
    /* not there? */
    if (!MENU_MODE) return;
    exit_play_qr_menu_mode();
}

int is_pure_play_photo_mode() // no other dialogs active (such as delete)
{
    if (!PLAY_MODE) return 0;
#ifdef CONFIG_5DC
    return 1;
#else
    extern thunk PlayMain_handler;
    return (intptr_t)get_current_dialog_handler() == (intptr_t)&PlayMain_handler;
#endif
}

int is_pure_play_movie_mode() // no other dialogs active (such as delete)
{
    if (!PLAY_MODE) return 0;
#ifdef CONFIG_VXWORKS
    return 0;
#else
    extern thunk PlayMovieGuideApp_handler;
    return (intptr_t)get_current_dialog_handler() == (intptr_t)&PlayMovieGuideApp_handler;
#endif
}

int is_pure_play_photo_or_movie_mode() { return is_pure_play_photo_mode() || is_pure_play_movie_mode(); }

int is_play_or_qr_mode()
{
    return PLAY_OR_QR_MODE;
}

int is_play_mode()
{
    return PLAY_MODE;
}

int is_menu_mode()
{
    return MENU_MODE;
}
