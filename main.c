#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <cairo/cairo.h>
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "pango.h"
#include "pool-buffer.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

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

struct output {
	struct menu *menu;
	struct wl_output *output;
	int32_t scale;
};

struct menu {
	struct output *output;
	char *output_name;

	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_data_device_manager *data_device_manager;
	struct zwlr_layer_shell_v1 *layer_shell;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

	struct wl_data_offer *offer;

	struct pool_buffer buffers[2];
	struct pool_buffer *current;

	int width;
	int height;
	int line_height;
	int padding;
	int inputw;
	int promptw;
	int left_arrow, right_arrow;

	bool bottom;
	int (*strncmp)(const char *, const char *, size_t);
	char *font;
	bool vertical;
	int lines;
	char *prompt;
	uint32_t background, foreground;
	uint32_t promptbg, promptfg;
	uint32_t selectionbg, selectionfg;

	char input[BUFSIZ];
	size_t cursor;

	int repeat_timer;
	int repeat_delay;
	int repeat_period;
	enum wl_keyboard_key_state repeat_key_state;
	xkb_keysym_t repeat_sym;

	bool run;
	bool failure;

	struct item *items;       // list of all items
	struct item *matches;     // list of matching items
	struct item *matches_end; // last matching item
	struct item *sel;         // selected item
	struct page *pages;       // list of pages
};

static void append_page(struct page *page, struct page **first, struct page **last) {
	if (*last) {
		(*last)->next = page;
	} else {
		*first = page;
	}
	page->prev = *last;
	page->next = NULL;
	*last = page;
}

static void page_items(struct menu *menu) {
	// Free existing pages
	while (menu->pages != NULL) {
		struct page *page = menu->pages;
		menu->pages = menu->pages->next;
		free(page);
	}

	if (!menu->matches) {
		return;
	}

	// Make new pages
	if (menu->vertical) {
		struct page *pages_end = NULL;
		struct item *item = menu->matches;
		while (item) {
			struct page *page = calloc(1, sizeof(struct page));
			page->first = item;

			for (int i = 1; item && i <= menu->lines; i++) {
				item->page = page;
				page->last = item;
				item = item->next_match;
			}
			append_page(page, &menu->pages, &pages_end);
		}
	} else {
		// Calculate available space
		int max_width = menu->width - menu->inputw - menu->promptw
			- menu->left_arrow - menu->right_arrow;

		struct page *pages_end = NULL;
		struct item *item = menu->matches;
		while (item) {
			struct page *page = calloc(1, sizeof(struct page));
			page->first = item;

			int total_width = 0;
			while (item) {
				total_width += item->width + 2 * menu->padding;
				if (total_width > max_width) {
					break;
				}

				item->page = page;
				page->last = item;
				item = item->next_match;
			}
			append_page(page, &menu->pages, &pages_end);
		}
	}
}

static const char *fstrstr(struct menu *menu, const char *s, const char *sub) {
	for (size_t len = strlen(sub); *s; s++) {
		if (!menu->strncmp(s, sub, len)) {
			return s;
		}
	}
	return NULL;
}

static void append_item(struct item *item, struct item **first, struct item **last) {
	if (*last) {
		(*last)->next_match = item;
	} else {
		*first = item;
	}
	item->prev_match = *last;
	item->next_match = NULL;
	*last = item;
}

static void match_items(struct menu *menu) {
	struct item *lexact = NULL, *exactend = NULL;
	struct item *lprefix = NULL, *prefixend = NULL;
	struct item *lsubstr  = NULL, *substrend = NULL;
	char buf[sizeof menu->input], *tok;
	char **tokv = NULL;
	int i, tokc = 0;
	size_t tok_len;
	menu->matches = NULL;
	menu->matches_end = NULL;
	menu->sel = NULL;

	size_t text_len = strlen(menu->input);

	/* tokenize text by space for matching the tokens individually */
	strcpy(buf, menu->input);
	tok = strtok(buf, " ");
	while (tok) {
		tokv = realloc(tokv, (tokc + 1) * sizeof *tokv);
		if (!tokv) {
			fprintf(stderr, "could not realloc %zu bytes",
					(tokc + 1) * sizeof *tokv);
			exit(EXIT_FAILURE);
		}
		tokv[tokc] = tok;
		tokc++;
		tok = strtok(NULL, " ");
	}
	tok_len = tokc ? strlen(tokv[0]) : 0;

	struct item *item;
	for (item = menu->items; item; item = item->next) {
		for (i = 0; i < tokc; i++) {
			if (!fstrstr(menu, item->text, tokv[i])) {
				/* token does not match */
				break;
			}
		}
		if (i != tokc) {
			/* not all tokens match */
			continue;
		}
		if (!tokc || !menu->strncmp(menu->input, item->text, text_len + 1)) {
			append_item(item, &lexact, &exactend);
		} else if (!menu->strncmp(tokv[0], item->text, tok_len)) {
			append_item(item, &lprefix, &prefixend);
		} else {
			append_item(item, &lsubstr, &substrend);
		}
	}

	if (lexact) {
		menu->matches = lexact;
		menu->matches_end = exactend;
	}
	if (lprefix) {
		if (menu->matches_end) {
			menu->matches_end->next_match = lprefix;
			lprefix->prev_match = menu->matches_end;
		} else {
			menu->matches = lprefix;
		}
		menu->matches_end = prefixend;
	}
	if (lsubstr) {
		if (menu->matches_end) {
			menu->matches_end->next_match = lsubstr;
			lsubstr->prev_match = menu->matches_end;
		} else {
			menu->matches = lsubstr;
		}
		menu->matches_end = substrend;
	}

	page_items(menu);
	if (menu->pages) {
		menu->sel = menu->pages->first;
	}
}

static void insert(struct menu *menu, const char *s, ssize_t n) {
	if (strlen(menu->input) + n > sizeof menu->input - 1) {
		return;
	}
	memmove(menu->input + menu->cursor + n, menu->input + menu->cursor,
			sizeof menu->input - menu->cursor - MAX(n, 0));
	if (n > 0 && s != NULL) {
		memcpy(menu->input + menu->cursor, s, n);
	}
	menu->cursor += n;
}

static size_t nextrune(struct menu *menu, int incr) {
	size_t n, len;

	len = strlen(menu->input);
	for(n = menu->cursor + incr; n < len && (menu->input[n] & 0xc0) == 0x80; n += incr);
	return n;
}

// Move the cursor to the beginning or end of the word, skipping over any preceding whitespace.
static void movewordedge(struct menu *menu, int dir) {
	size_t len = strlen(menu->input);
	while (menu->cursor > 0 && menu->cursor < len && menu->input[nextrune(menu, dir)] == ' ') {
		menu->cursor = nextrune(menu, dir);
	}
	while (menu->cursor > 0 && menu->cursor < len && menu->input[nextrune(menu, dir)] != ' ') {
		menu->cursor = nextrune(menu, dir);
	}
}

// Calculate text widths.
static void calc_widths(struct menu *menu) {
	cairo_t *cairo = menu->current->cairo;

	// Calculate prompt width
	if (menu->prompt) {
		menu->promptw = text_width(cairo, menu->font, menu->prompt) + menu->padding + menu->padding/2;
	} else {
		menu->promptw = 0;
	}

	// Calculate scroll indicator widths
	menu->left_arrow = text_width(cairo, menu->font, "<") + 2 * menu->padding;
	menu->right_arrow = text_width(cairo, menu->font, ">") + 2 * menu->padding;

	// Calculate item widths and input area width
	for (struct item *item = menu->items; item; item = item->next) {
		item->width = text_width(cairo, menu->font, item->text);
		if (item->width > menu->inputw) {
			menu->inputw = item->width;
		}
	}
}

static void cairo_set_source_u32(cairo_t *cairo, uint32_t color) {
	cairo_set_source_rgba(cairo,
			(color >> (3*8) & 0xFF) / 255.0,
			(color >> (2*8) & 0xFF) / 255.0,
			(color >> (1*8) & 0xFF) / 255.0,
			(color >> (0*8) & 0xFF) / 255.0);
}

// Renders text to cairo.
static int render_text(struct menu *menu, cairo_t *cairo, const char *str,
		int x, int y, int width, uint32_t bg_color, uint32_t fg_color,
		int left_padding, int right_padding) {

	int text_width, text_height;
	get_text_size(cairo, menu->font, &text_width, &text_height, NULL, 1, str);
	int text_y = (menu->line_height / 2.0) - (text_height / 2.0);

	if (width == 0) {
		width = text_width + left_padding + right_padding;
	}
	if (bg_color) {
		cairo_set_source_u32(cairo, bg_color);
		cairo_rectangle(cairo, x, y, width, menu->line_height);
		cairo_fill(cairo);
	}
	cairo_move_to(cairo, x + left_padding, y + text_y);
	cairo_set_source_u32(cairo, fg_color);
	pango_printf(cairo, menu->font, 1, str);

	return width;
}

// Renders the prompt message.
static void render_prompt(struct menu *menu, cairo_t *cairo) {
	if (!menu->prompt) {
		return;
	}
	render_text(menu, cairo, menu->prompt, 0, 0, 0,
		menu->promptbg, menu->promptfg, menu->padding, menu->padding/2);
}

// Renders the input text.
static void render_input(struct menu *menu, cairo_t *cairo) {
	render_text(menu, cairo, menu->input, menu->promptw, 0, 0,
		0, menu->foreground, menu->padding, menu->padding);
}

// Renders a cursor for the input field.
static void render_cursor(struct menu *menu, cairo_t *cairo) {
	const int cursor_width = 2;
	const int cursor_margin = 2;
	int cursor_pos = menu->promptw + menu->padding
		+ text_width(cairo, menu->font, menu->input)
		- text_width(cairo, menu->font, &menu->input[menu->cursor])
		- cursor_width / 2;
	cairo_rectangle(cairo, cursor_pos, cursor_margin, cursor_width,
			menu->line_height - 2 * cursor_margin);
	cairo_fill(cairo);
}

// Renders a single menu item horizontally.
static int render_horizontal_item(struct menu *menu, cairo_t *cairo, struct item *item, int x) {
	uint32_t bg_color = menu->sel == item ? menu->selectionbg : menu->background;
	uint32_t fg_color = menu->sel == item ? menu->selectionfg : menu->foreground;

	return render_text(menu, cairo, item->text, x, 0, 0,
		bg_color, fg_color, menu->padding, menu->padding);
}

// Renders a single menu item vertically.
static int render_vertical_item(struct menu *menu, cairo_t *cairo, struct item *item, int x, int y) {
	uint32_t bg_color = menu->sel == item ? menu->selectionbg : menu->background;
	uint32_t fg_color = menu->sel == item ? menu->selectionfg : menu->foreground;

	render_text(menu, cairo, item->text, x, y, menu->width - x,
		bg_color, fg_color, menu->padding, 0);
	return menu->line_height;
}

// Renders a page of menu items horizontally.
static void render_horizontal_page(struct menu *menu, cairo_t *cairo, struct page *page) {
	int x = menu->promptw + menu->inputw + menu->left_arrow;
	for (struct item *item = page->first; item != page->last->next_match; item = item->next_match) {
		x += render_horizontal_item(menu, cairo, item, x);
	}

	// Draw left and right scroll indicators if necessary
	if (page->prev) {
		cairo_move_to(cairo, menu->promptw + menu->inputw + menu->padding, 0);
		pango_printf(cairo, menu->font, 1, "<");
	}
	if (page->next) {
		cairo_move_to(cairo, menu->width - menu->right_arrow + menu->padding, 0);
		pango_printf(cairo, menu->font, 1, ">");
	}
}

// Renders a page of menu items vertically.
static void render_vertical_page(struct menu *menu, cairo_t *cairo, struct page *page) {
	int x = menu->promptw;
	int y = menu->line_height;
	for (struct item *item = page->first; item != page->last->next_match; item = item->next_match) {
		y += render_vertical_item(menu, cairo, item, x, y);
	}
}

// Renders the menu to cairo.
static void render_menu(struct menu *menu, cairo_t *cairo) {
	// Render background
	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, menu->background);
	cairo_paint(cairo);

	// Render prompt and input
	render_prompt(menu, cairo);
	render_input(menu, cairo);
	render_cursor(menu, cairo);

	// Render selected page
	if (!menu->sel) {
		return;
	}
	if (menu->vertical) {
		render_vertical_page(menu, cairo, menu->sel->page);
	} else {
		render_horizontal_page(menu, cairo, menu->sel->page);
	}
}

static void render_frame(struct menu *menu) {
	cairo_surface_t *recorder = cairo_recording_surface_create(
			CAIRO_CONTENT_COLOR_ALPHA, NULL);
	cairo_t *cairo = cairo_create(recorder);
	cairo_set_antialias(cairo, CAIRO_ANTIALIAS_BEST);
	cairo_font_options_t *fo = cairo_font_options_create();
	cairo_set_font_options(cairo, fo);
	cairo_font_options_destroy(fo);
	cairo_save(cairo);
	cairo_set_operator(cairo, CAIRO_OPERATOR_CLEAR);
	cairo_paint(cairo);
	cairo_restore(cairo);

	render_menu(menu, cairo);

	int scale = menu->output ? menu->output->scale : 1;
	menu->current = get_next_buffer(menu->shm,
		menu->buffers, menu->width, menu->height, scale);
	if (!menu->current) {
		goto cleanup;
	}

	cairo_t *shm = menu->current->cairo;
	cairo_save(shm);
	cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
	cairo_paint(shm);
	cairo_restore(shm);
	cairo_set_source_surface(shm, recorder, 0, 0);
	cairo_paint(shm);

	wl_surface_set_buffer_scale(menu->surface, scale);
	wl_surface_attach(menu->surface, menu->current->buffer, 0, 0);
	wl_surface_damage(menu->surface, 0, 0, menu->width, menu->height);
	wl_surface_commit(menu->surface);

cleanup:
	cairo_destroy(cairo);
}

static void noop() {
	// Do nothing
}

static void surface_enter(void *data, struct wl_surface *surface,
		struct wl_output *wl_output) {
	struct menu *menu = data;
	menu->output = wl_output_get_user_data(wl_output);
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = noop,
};

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct menu *menu = data;
	menu->width = width;
	menu->height = height;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct menu *menu = data;
	menu->run = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
	struct output *output = data;
	output->scale = factor;
}

static void output_name(void *data, struct wl_output *wl_output, const char *name) {
	struct output *output = data;
	struct menu *menu = output->menu;
	char *outname = menu->output_name;
	if (!menu->output && outname && strcmp(outname, name) == 0) {
		menu->output = output;
	}
}

static const struct wl_output_listener output_listener = {
	.geometry = noop,
	.mode = noop,
	.done = noop,
	.scale = output_scale,
	.name = output_name,
	.description = noop,
};

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	struct menu *menu = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		menu->run = false;
		menu->failure = true;
		return;
	}
	char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_shm == MAP_FAILED) {
		close(fd);
		menu->run = false;
		menu->failure = true;
		return;
	}
	menu->xkb_keymap = xkb_keymap_new_from_string(menu->xkb_context,
		map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	munmap(map_shm, size);
	close(fd);
	menu->xkb_state = xkb_state_new(menu->xkb_keymap);
}

static void keypress(struct menu *menu, enum wl_keyboard_key_state key_state,
		xkb_keysym_t sym) {
	if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}

	bool ctrl = xkb_state_mod_name_is_active(menu->xkb_state,
			XKB_MOD_NAME_CTRL,
			XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);
	bool meta = xkb_state_mod_name_is_active(menu->xkb_state,
			XKB_MOD_NAME_ALT,
			XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);
	bool shift = xkb_state_mod_name_is_active(menu->xkb_state,
			XKB_MOD_NAME_SHIFT,
			XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

	size_t len = strlen(menu->input);

	if (ctrl) {
		// Emacs-style line editing bindings
		switch (sym) {
		case XKB_KEY_a:
			sym = XKB_KEY_Home;
			break;
		case XKB_KEY_b:
			sym = XKB_KEY_Left;
			break;
		case XKB_KEY_c:
			sym = XKB_KEY_Escape;
			break;
		case XKB_KEY_d:
			sym = XKB_KEY_Delete;
			break;
		case XKB_KEY_e:
			sym = XKB_KEY_End;
			break;
		case XKB_KEY_f:
			sym = XKB_KEY_Right;
			break;
		case XKB_KEY_g:
			sym = XKB_KEY_Escape;
			break;
		case XKB_KEY_bracketleft:
			sym = XKB_KEY_Escape;
			break;
		case XKB_KEY_h:
			sym = XKB_KEY_BackSpace;
			break;
		case XKB_KEY_i:
			sym = XKB_KEY_Tab;
			break;
		case XKB_KEY_j:
		case XKB_KEY_J:
		case XKB_KEY_m:
		case XKB_KEY_M:
			sym = XKB_KEY_Return;
			ctrl = false;
			break;
		case XKB_KEY_n:
			sym = XKB_KEY_Down;
			break;
		case XKB_KEY_p:
			sym = XKB_KEY_Up;
			break;

		case XKB_KEY_k:
			// Delete right
			menu->input[menu->cursor] = '\0';
			match_items(menu);
			render_frame(menu);
			return;
		case XKB_KEY_u:
			// Delete left
			insert(menu, NULL, 0 - menu->cursor);
			match_items(menu);
			render_frame(menu);
			return;
		case XKB_KEY_w:
			// Delete word
			while (menu->cursor > 0 && menu->input[nextrune(menu, -1)] == ' ') {
				insert(menu, NULL, nextrune(menu, -1) - menu->cursor);
			}
			while (menu->cursor > 0 && menu->input[nextrune(menu, -1)] != ' ') {
				insert(menu, NULL, nextrune(menu, -1) - menu->cursor);
			}
			match_items(menu);
			render_frame(menu);
			return;
		case XKB_KEY_Y:
			// Paste clipboard
			if (!menu->offer) {
				return;
			}

			int fds[2];
			if (pipe(fds) == -1) {
				// Pipe failed
				return;
			}
			wl_data_offer_receive(menu->offer, "text/plain", fds[1]);
			close(fds[1]);

			wl_display_roundtrip(menu->display);

			while (true) {
				char buf[1024];
				ssize_t n = read(fds[0], buf, sizeof(buf));
				if (n <= 0) {
					break;
				}
				insert(menu, buf, n);
			}
			close(fds[0]);

			wl_data_offer_destroy(menu->offer);
			menu->offer = NULL;
			match_items(menu);
			render_frame(menu);
			return;
		case XKB_KEY_Left:
		case XKB_KEY_KP_Left:
			movewordedge(menu, -1);
			render_frame(menu);
			return;
		case XKB_KEY_Right:
		case XKB_KEY_KP_Right:
			movewordedge(menu, +1);
			render_frame(menu);
			return;

		case XKB_KEY_Return:
		case XKB_KEY_KP_Enter:
			break;
		default:
			return;
		}
	} else if (meta) {
		// Emacs-style line editing bindings
		switch (sym) {
		case XKB_KEY_b:
			movewordedge(menu, -1);
			render_frame(menu);
			return;
		case XKB_KEY_f:
			movewordedge(menu, +1);
			render_frame(menu);
			return;
		case XKB_KEY_g:
			sym = XKB_KEY_Home;
			break;
		case XKB_KEY_G:
			sym = XKB_KEY_End;
			break;
		case XKB_KEY_h:
			sym = XKB_KEY_Up;
			break;
		case XKB_KEY_j:
			sym = XKB_KEY_Next;
			break;
		case XKB_KEY_k:
			sym = XKB_KEY_Prior;
			break;
		case XKB_KEY_l:
			sym = XKB_KEY_Down;
			break;
		default:
			return;
		}
	}

	char buf[8];
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (shift) {
			puts(menu->input);
			fflush(stdout);
			menu->run = false;
		} else {
			char *text = menu->sel ? menu->sel->text : menu->input;
			puts(text);
			fflush(stdout);
			if (!ctrl) {
				menu->run = false;
			}
		}
		break;
	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
	case XKB_KEY_Up:
	case XKB_KEY_KP_Up:
		if (menu->sel && menu->sel->prev_match) {
			menu->sel = menu->sel->prev_match;
			render_frame(menu);
		} else if (menu->cursor > 0) {
			menu->cursor = nextrune(menu, -1);
			render_frame(menu);
		}
		break;
	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
	case XKB_KEY_Down:
	case XKB_KEY_KP_Down:
		if (menu->cursor < len) {
			menu->cursor = nextrune(menu, +1);
			render_frame(menu);
		} else if (menu->sel && menu->sel->next_match) {
			menu->sel = menu->sel->next_match;
			render_frame(menu);
		}
		break;
	case XKB_KEY_Prior:
	case XKB_KEY_KP_Prior:
		if (menu->sel && menu->sel->page->prev) {
			menu->sel = menu->sel->page->prev->first;
			render_frame(menu);
		}
		break;
	case XKB_KEY_Next:
	case XKB_KEY_KP_Next:
		if (menu->sel && menu->sel->page->next) {
			menu->sel = menu->sel->page->next->first;
			render_frame(menu);
		}
		break;
	case XKB_KEY_Home:
	case XKB_KEY_KP_Home:
		if (menu->sel == menu->matches) {
			menu->cursor = 0;
			render_frame(menu);
		} else {
			menu->sel = menu->matches;
			render_frame(menu);
		}
		break;
	case XKB_KEY_End:
	case XKB_KEY_KP_End:
		if (menu->cursor < len) {
			menu->cursor = len;
			render_frame(menu);
		} else {
			menu->sel = menu->matches_end;
			render_frame(menu);
		}
		break;
	case XKB_KEY_BackSpace:
		if (menu->cursor > 0) {
			insert(menu, NULL, nextrune(menu, -1) - menu->cursor);
			match_items(menu);
			render_frame(menu);
		}
		break;
	case XKB_KEY_Delete:
	case XKB_KEY_KP_Delete:
		if (menu->cursor == len) {
			return;
		}
		menu->cursor = nextrune(menu, +1);
		insert(menu, NULL, nextrune(menu, -1) - menu->cursor);
		match_items(menu);
		render_frame(menu);
		break;
	case XKB_KEY_Tab:
		if (!menu->sel) {
			return;
		}
		menu->cursor = strnlen(menu->sel->text, sizeof menu->input - 1);
		memcpy(menu->input, menu->sel->text, menu->cursor);
		menu->input[menu->cursor] = '\0';
		match_items(menu);
		render_frame(menu);
		break;
	case XKB_KEY_Escape:
		menu->failure = true;
		menu->run = false;
		break;
	default:
		if (xkb_keysym_to_utf8(sym, buf, 8)) {
			insert(menu, buf, strnlen(buf, 8));
			match_items(menu);
			render_frame(menu);
		}
	}
}

static void keyboard_repeat(struct menu *menu) {
	keypress(menu, menu->repeat_key_state, menu->repeat_sym);
	struct itimerspec spec = { 0 };
	spec.it_value.tv_sec = menu->repeat_period / 1000;
	spec.it_value.tv_nsec = (menu->repeat_period % 1000) * 1000000l;
	timerfd_settime(menu->repeat_timer, 0, &spec, NULL);
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t _key_state) {
	struct menu *menu = data;

	enum wl_keyboard_key_state key_state = _key_state;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(menu->xkb_state, key + 8);
	keypress(menu, key_state, sym);

	if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED && menu->repeat_period >= 0) {
		menu->repeat_key_state = key_state;
		menu->repeat_sym = sym;

		struct itimerspec spec = { 0 };
		spec.it_value.tv_sec = menu->repeat_delay / 1000;
		spec.it_value.tv_nsec = (menu->repeat_delay % 1000) * 1000000l;
		timerfd_settime(menu->repeat_timer, 0, &spec, NULL);
	} else if (key_state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		struct itimerspec spec = { 0 };
		timerfd_settime(menu->repeat_timer, 0, &spec, NULL);
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	struct menu *menu = data;
	menu->repeat_delay = delay;
	if (rate > 0) {
		menu->repeat_period = 1000 / rate;
	} else {
		menu->repeat_period = -1;
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t mods_depressed,
		uint32_t mods_latched, uint32_t mods_locked,
		uint32_t group) {
	struct menu *menu = data;
	xkb_state_update_mask(menu->xkb_state, mods_depressed, mods_latched,
			mods_locked, 0, 0, group);
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = keyboard_keymap,
	.enter = noop,
	.leave = noop,
	.key = keyboard_key,
	.modifiers = keyboard_modifiers,
	.repeat_info = keyboard_repeat_info,
};

static void seat_capabilities(void *data, struct wl_seat *seat,
		enum wl_seat_capability caps) {
	struct menu *menu = data;
	if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		struct wl_keyboard *keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, menu);
	}
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = noop,
};

static void data_device_selection(void *data, struct wl_data_device *data_device,
		struct wl_data_offer *offer) {
	struct menu *menu = data;
	menu->offer = offer;
}

static const struct wl_data_device_listener data_device_listener = {
	.data_offer = noop,
	.enter = noop,
	.leave = noop,
	.motion = noop,
	.drop = noop,
	.selection = data_device_selection,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct menu *menu = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		menu->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		menu->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		menu->seat = wl_registry_bind(registry, name, &wl_seat_interface, 4);
		wl_seat_add_listener(menu->seat, &seat_listener, menu);
	} else if (strcmp(interface, wl_data_device_manager_interface.name) == 0) {
		menu->data_device_manager = wl_registry_bind(registry, name,
				&wl_data_device_manager_interface, 3);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		menu->layer_shell = wl_registry_bind(registry, name,
				&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *output = calloc(1, sizeof(struct output));
		output->output = wl_registry_bind(registry, name,
				&wl_output_interface, 4);
		output->menu = menu;
		output->scale = 1;
		wl_output_set_user_data(output->output, output);
		wl_output_add_listener(output->output, &output_listener, output);
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = noop,
};

static void read_stdin(struct menu *menu) {
	char buf[sizeof menu->input], *p;
	struct item *item, **end;

	for(end = &menu->items; fgets(buf, sizeof buf, stdin); *end = item, end = &item->next) {
		if((p = strchr(buf, '\n'))) {
			*p = '\0';
		}
		item = malloc(sizeof *item);
		if (!item) {
			return;
		}

		item->text = strdup(buf);
		item->next = item->prev_match = item->next_match = NULL;
	}
}

static void menu_init(struct menu *menu) {
	int height = get_font_height(menu->font);
	menu->line_height = height + 3;
	menu->height = menu->line_height;
	if (menu->vertical) {
		menu->height += menu->height * menu->lines;
	}
	menu->padding = height / 2;

	menu->display = wl_display_connect(NULL);
	if (!menu->display) {
		fprintf(stderr, "wl_display_connect: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	menu->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!menu->xkb_context) {
		fprintf(stderr, "xkb_context_new: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	menu->repeat_timer = timerfd_create(CLOCK_MONOTONIC, 0);
	assert(menu->repeat_timer >= 0);

	struct wl_registry *registry = wl_display_get_registry(menu->display);
	wl_registry_add_listener(registry, &registry_listener, menu);
	wl_display_roundtrip(menu->display);
	assert(menu->compositor != NULL);
	assert(menu->shm != NULL);
	assert(menu->seat != NULL);
	assert(menu->data_device_manager != NULL);
	assert(menu->layer_shell != NULL);

	// Get data device for seat
	struct wl_data_device *data_device = wl_data_device_manager_get_data_device(
			menu->data_device_manager, menu->seat);
	wl_data_device_add_listener(data_device, &data_device_listener, menu);

	// Second roundtrip for xdg-output
	wl_display_roundtrip(menu->display);

	if (menu->output_name && !menu->output) {
		fprintf(stderr, "Output %s not found\n", menu->output_name);
		exit(EXIT_FAILURE);
	}
}

static void menu_create_surface(struct menu *menu) {
	menu->surface = wl_compositor_create_surface(menu->compositor);
	wl_surface_add_listener(menu->surface, &surface_listener, menu);
	menu->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		menu->layer_shell,
		menu->surface,
		NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		"menu"
	);
	assert(menu->layer_surface != NULL);

	uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	if (menu->bottom) {
		anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	} else {
		anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	}

	zwlr_layer_surface_v1_set_anchor(menu->layer_surface, anchor);
	zwlr_layer_surface_v1_set_size(menu->layer_surface, 0, menu->height);
	zwlr_layer_surface_v1_set_exclusive_zone(menu->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(menu->layer_surface, true);
	zwlr_layer_surface_v1_add_listener(menu->layer_surface,
		&layer_surface_listener, menu);

	wl_surface_commit(menu->surface);
	wl_display_roundtrip(menu->display);
}

static bool parse_color(const char *color, uint32_t *result) {
	if (color[0] == '#') {
		++color;
	}
	size_t len = strlen(color);
	if ((len != 6 && len != 8) || !isxdigit(color[0]) || !isxdigit(color[1])) {
		return false;
	}
	char *ptr;
	uint32_t parsed = (uint32_t)strtoul(color, &ptr, 16);
	if (*ptr != '\0') {
		return false;
	}
	*result = len == 6 ? ((parsed << 8) | 0xFF) : parsed;
	return true;
}

int main(int argc, char **argv) {
	struct menu menu = {
		.strncmp = strncmp,
		.font = "monospace 10",
		.vertical = false,
		.background = 0x222222ff,
		.foreground = 0xbbbbbbff,
		.promptbg = 0x005577ff,
		.promptfg = 0xeeeeeeff,
		.selectionbg = 0x005577ff,
		.selectionfg = 0xeeeeeeff,
		.run = true,
	};

	const char *usage =
		"Usage: wmenu [-biv] [-f font] [-l lines] [-o output] [-p prompt]\n"
		"\t[-N color] [-n color] [-M color] [-m color] [-S color] [-s color]\n";

	int opt;
	while ((opt = getopt(argc, argv, "bhivf:l:o:p:N:n:M:m:S:s:")) != -1) {
		switch (opt) {
		case 'b':
			menu.bottom = true;
			break;
		case 'i':
			menu.strncmp = strncasecmp;
			break;
		case 'v':
			puts("wmenu " VERSION);
			exit(EXIT_SUCCESS);
		case 'f':
			menu.font = optarg;
			break;
		case 'l':
			menu.vertical = true;
			menu.lines = atoi(optarg);
			break;
		case 'o':
			menu.output_name = optarg;
			break;
		case 'p':
			menu.prompt = optarg;
			break;
		case 'N':
			if (!parse_color(optarg, &menu.background)) {
				fprintf(stderr, "Invalid background color: %s", optarg);
			}
			break;
		case 'n':
			if (!parse_color(optarg, &menu.foreground)) {
				fprintf(stderr, "Invalid foreground color: %s", optarg);
			}
			break;
		case 'M':
			if (!parse_color(optarg, &menu.promptbg)) {
				fprintf(stderr, "Invalid prompt background color: %s", optarg);
			}
			break;
		case 'm':
			if (!parse_color(optarg, &menu.promptfg)) {
				fprintf(stderr, "Invalid prompt foreground color: %s", optarg);
			}
			break;
		case 'S':
			if (!parse_color(optarg, &menu.selectionbg)) {
				fprintf(stderr, "Invalid selection background color: %s", optarg);
			}
			break;
		case 's':
			if (!parse_color(optarg, &menu.selectionfg)) {
				fprintf(stderr, "Invalid selection foreground color: %s", optarg);
			}
			break;
		default:
			fprintf(stderr, "%s", usage);
			exit(EXIT_FAILURE);
		}
	}

	if (optind < argc) {
		fprintf(stderr, "%s", usage);
		exit(EXIT_FAILURE);
	}

	menu_init(&menu);
	menu_create_surface(&menu);
	render_frame(&menu);

	read_stdin(&menu);
	calc_widths(&menu);
	match_items(&menu);
	render_frame(&menu);

	struct pollfd fds[] = {
		{ wl_display_get_fd(menu.display), POLLIN },
		{ menu.repeat_timer, POLLIN },
	};
	const size_t nfds = sizeof(fds) / sizeof(*fds);

	while (menu.run) {
		errno = 0;
		do {
			if (wl_display_flush(menu.display) == -1 && errno != EAGAIN) {
				fprintf(stderr, "wl_display_flush: %s\n", strerror(errno));
				break;
			}
		} while (errno == EAGAIN);

		if (poll(fds, nfds, -1) < 0) {
			fprintf(stderr, "poll: %s\n", strerror(errno));
			break;
		}

		if (fds[0].revents & POLLIN) {
			if (wl_display_dispatch(menu.display) < 0) {
				menu.run = false;
			}
		}

		if (fds[1].revents & POLLIN) {
			keyboard_repeat(&menu);
		}
	}

	wl_display_disconnect(menu.display);

	if (menu.failure) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
