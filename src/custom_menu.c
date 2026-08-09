/** \file
 * Slim Settings panel — explicit order via placeholders.
 * Module/core entries fill placeholders by name.
 */
#include "dryos.h"
#include "menu.h"
#include "config.h"

#ifdef CONFIG_SLIM_MENUS

/* Order of Settings panel rows. Entries fill these placeholders by name. */
static struct menu_entry custom_menu_placeholders[] =
{
    { .name = "Digic Peaking",       .placeholder = 1 },
    { .name = "Screen Layout",       .placeholder = 1 },
    { .name = "INFO Button",         .placeholder = 1 },
    { .name = "SET Button",          .placeholder = 1 },
    { .name = "Up/Down Button",      .placeholder = 1 },
    { .name = "Shutter zoom",        .placeholder = 1 },
    { .name = "HDMI Output",         .placeholder = 1 },
    { .name = "HDMI Resolution",     .placeholder = 1 },
    { .name = "SD Overclock",        .placeholder = 1 },
    { .name = "SD Access Mode",      .placeholder = 1 },
    { .name = "Small Hacks",         .placeholder = 1 },
    { .name = "More Hacks",          .placeholder = 1 },
    { .name = "Card Benchmark",      .placeholder = 1 },
};

static void custom_menu_init(void)
{
    menu_add("Settings", custom_menu_placeholders, COUNT(custom_menu_placeholders));
}

INIT_FUNC(__FILE__, custom_menu_init);

#endif /* CONFIG_SLIM_MENUS */
