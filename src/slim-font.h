#ifndef SLIM_FONT_H
#define SLIM_FONT_H

#include <stdint.h>

/* Roboto Thin is reserved for Slim menu surfaces only.  It is loaded lazily,
 * after boot, and must not be used by Live View status bars or FONT_MED paths. */
uint32_t slim_ui_font_spec(uint32_t foreground, uint32_t background);
int slim_ui_font_height(void);

#endif
