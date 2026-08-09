#ifndef MENU_GRID_H
#define MENU_GRID_H

/** Slim EOS M menu launcher: 2x2 grid shown on long-press TRASH / menu open. */

int menu_grid_is_active(void);
int menu_grid_is_launched(void);
void menu_grid_open(void);
void menu_grid_close(void);
void menu_grid_return(void);
/** Skip grid launcher and show the selected category menu (after select_menu_by_name). */
void menu_grid_enter_launched(void);
void menu_grid_draw(void);
/** Returns 0 if handled, 1 if caller should continue normal menu key handling. */
int menu_grid_handle_key(int button_code, int *needs_full_redraw);
/** Returns 0 if a touch inside a tile was launched, 1 otherwise. */
int menu_grid_handle_touch(int x, int y);

/* Lightweight Live View Quick Screen overlay. */
int menu_quick_screen_is_active(void);
void menu_quick_screen_open(void);
void menu_quick_screen_close(void);
void menu_quick_screen_draw(void);
int menu_quick_screen_handle_touch(int x, int y);
void menu_quick_screen_touch_release(void);
int menu_quick_screen_handle_key(int button_code);

#endif
