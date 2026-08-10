/** \file
 * Reorganise Movie menu for RAW/h264 recording modes
 */
#include "dryos.h"
#include "math.h"
#include "version.h"
#include "bmp.h"
#include "gui.h"
#include "config.h"
#include "property.h"
#include "lens.h"
#include "font.h"
#include "menu.h"
#include "beep.h"
#include "zebra.h"
#include "focus.h"
#include "menuhelp.h"
#include "console.h"
#include "debug.h"
#include "lvinfo.h"
#include "powersave.h"

/* Explicit Movie menu order. Module/core entries fill placeholders by name. */
static struct menu_entry movie_menu_raw_toggle[] =
{
#ifdef CONFIG_SLIM_MENUS
    /* Flat Crop Mode settings — top of Movie page */
    { .name = "Mode",                 .placeholder = 1 },
    { .name = "Aspect Ratio",         .placeholder = 1 },
    { .name = "Preset",               .placeholder = 1 },
    { .name = "Resolution",           .placeholder = 1 },
    { .name = "Frame Rate",           .placeholder = 1 },
    { .name = "Bit Depth",            .placeholder = 1 },
    { .name = "Sound recording",      .placeholder = 1 },
    { .name = "Kill Global Draw",     .placeholder = 1 },
#else
    { .name = "Crop Mode",            .placeholder = 1 },
    { .name = "RAW video",            .placeholder = 1 },
    { .name = "Bit-depth",            .placeholder = 1 },
    { .name = "Framerate:",           .placeholder = 1 },
    { .name = "Aspect ratio:",        .placeholder = 1 },
    { .name = "Customize buttons",    .placeholder = 1 },
    { .name = "Custom modes",         .placeholder = 1 },
    { .name = "Sound recording",      .placeholder = 1 },
    { .name = "Shutter Expo",         .placeholder = 1 },
    { .name = "Aperture Expo",        .placeholder = 1 },
    { .name = "ISO Expo",             .placeholder = 1 },
    { .name = "Shutter lock",         .placeholder = 1 },
    { .name = "Shutter fine-tuning",  .placeholder = 1 },
    { .name = "Shutter range",        .placeholder = 1 },
    { .name = "SD Overclock",         .placeholder = 1 },
    { .name = "HDR video",            .placeholder = 1 },
    { .name = "FPS override",         .placeholder = 1 },
    { .name = "FPS modifier",         .placeholder = 1 },
    { .name = "intervalometer",       .placeholder = 1 },
    { .name = "recording delay",      .placeholder = 1 },
#endif
};

static void movie_menu_raw_only_init()
{
    menu_add("Movie", movie_menu_raw_toggle, COUNT(movie_menu_raw_toggle));
}

INIT_FUNC(__FILE__, movie_menu_raw_only_init);
