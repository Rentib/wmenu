#ifndef WMENU_MENU_H
#define WMENU_MENU_H

#include <xkbcommon/xkbcommon.h>

#include "pool-buffer.h"

// A menu item.
struct item {
	char *text;
	int width;
	struct item *next;       // traverses all items
	struct item *prev_match; // previous matching item
	struct item *next_match; // next matching item
	struct page *page;       // the page holding this item
};

// A page of menu items.
struct page {
	struct item *first; // first item in the page
	struct item *last;  // last item in the page
	struct page *prev;  // previous page
	struct page *next;  // next page
};

// A Wayland output.
struct output {
	struct menu *menu;
	struct wl_output *output;
	const char *name;
	int32_t scale;
};

// Keyboard state.
struct keyboard {
	struct menu *menu;

	struct xkb_context *xkb_context;
	struct xkb_state *xkb_state;

	int repeat_timer;
	int repeat_delay;
	int repeat_period;
	enum wl_keyboard_key_state repeat_key_state;
	xkb_keysym_t repeat_sym;
};

// Menu state.
struct menu {
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_data_device_manager *data_device_manager;
	struct zwlr_layer_shell_v1 *layer_shell;

	struct wl_display *display;
	struct wl_surface *surface;
	struct wl_data_offer *offer;

	struct keyboard *keyboard;
	struct output *output;
	char *output_name;

	struct pool_buffer buffers[2];
	struct pool_buffer *current;

	int width;
	int height;
	int line_height;
	int padding;
	int inputw;
	int promptw;
	int left_arrow;
	int right_arrow;

	bool bottom;
	int (*strncmp)(const char *, const char *, size_t);
	char *font;
	int lines;
	char *prompt;
	uint32_t background, foreground;
	uint32_t promptbg, promptfg;
	uint32_t selectionbg, selectionfg;

	char input[BUFSIZ];
	size_t cursor;

	struct item *items;       // list of all items
	struct item *matches;     // list of matching items
	struct item *matches_end; // last matching item
	struct item *sel;         // selected item
	struct page *pages;       // list of pages

	bool exit;
	bool failure;
};

void menu_init(struct menu *menu, int argc, char *argv[]);
void read_menu_items(struct menu *menu);
void menu_keypress(struct menu *menu, enum wl_keyboard_key_state key_state,
		xkb_keysym_t sym);

#endif
