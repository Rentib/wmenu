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
#include "xdg-output-unstable-v1-client-protocol.h"

struct menu_item {
	char *text;
	int width;
	struct menu_item *next;         // traverses all items
	struct menu_item *left, *right; // traverses matching items
};

struct output {
	struct menu_state *menu;
	struct wl_output *output;
	struct zxdg_output_v1 *xdg_output;
	int32_t scale;
};

struct menu_state {
	struct output *output;
	char *output_name;

	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_keyboard *keyboard;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zxdg_output_manager_v1 *output_manager;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer_surface;

	struct xkb_context *xkb_context;
	struct xkb_keymap *xkb_keymap;
	struct xkb_state *xkb_state;

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
	int (*fstrncmp)(const char *, const char *, size_t);
	char *font;
	bool vertical;
	int lines;
	char *prompt;
	uint32_t background, foreground;
	uint32_t promptbg, promptfg;
	uint32_t selectionbg, selectionfg;

	char text[BUFSIZ];
	size_t cursor;

	int repeat_timer;
	int repeat_delay;
	int repeat_period;
	enum wl_keyboard_key_state repeat_key_state;
	xkb_keysym_t repeat_sym;

	bool run;
	bool failure;

	struct menu_item *items;
	struct menu_item *matches;
	struct menu_item *selection;
	struct menu_item *leftmost, *rightmost;
};

static void cairo_set_source_u32(cairo_t *cairo, uint32_t color) {
	cairo_set_source_rgba(cairo,
			(color >> (3*8) & 0xFF) / 255.0,
			(color >> (2*8) & 0xFF) / 255.0,
			(color >> (1*8) & 0xFF) / 255.0,
			(color >> (0*8) & 0xFF) / 255.0);
}

static void insert(struct menu_state *state, const char *s, ssize_t n);
static void match(struct menu_state *state);
static size_t nextrune(struct menu_state *state, int incr);

int render_text(struct menu_state *state, cairo_t *cairo, const char *str,
		int x, int y, int width, int height,
		uint32_t foreground, uint32_t background,
		int left_padding, int right_padding) {

	int text_width, text_height;
	get_text_size(cairo, state->font, &text_width, &text_height, NULL, 1, str);
	int text_y = (state->line_height / 2.0) - (text_height / 2.0);

	if (background) {
		int bg_width = text_width + left_padding + right_padding;
		cairo_set_source_u32(cairo, background);
		cairo_rectangle(cairo, x, y, bg_width, height);
		cairo_fill(cairo);
	}

	cairo_move_to(cairo, x + left_padding, y + text_y);
	cairo_set_source_u32(cairo, foreground);
	pango_printf(cairo, state->font, 1, str);

	return x + text_width + left_padding + right_padding;
}

int render_horizontal_item(struct menu_state *state, cairo_t *cairo, const char *str,
		int x, int y, int width, int height,
		uint32_t foreground, uint32_t background,
		int left_padding, int right_padding) {

	int text_width, text_height;
	get_text_size(cairo, state->font, &text_width, &text_height, NULL, 1, str);
	int text_y = (state->line_height / 2.0) - (text_height / 2.0);

	if (x + left_padding + text_width > width) {
		return -1;
	} else {
		if (background) {
			int bg_width = text_width + left_padding + right_padding;
			cairo_set_source_u32(cairo, background);
			cairo_rectangle(cairo, x, y, bg_width, height);
			cairo_fill(cairo);
		}

		cairo_move_to(cairo, x + left_padding, y + text_y);
		cairo_set_source_u32(cairo, foreground);
		pango_printf(cairo, state->font, 1, str);
	}

	return x + text_width + left_padding + right_padding;
}

void render_vertical_item(struct menu_state *state, cairo_t *cairo, const char *str,
		int x, int y, int width, int height,
		uint32_t foreground, uint32_t background,
		int left_padding) {

	int text_height;
	get_text_size(cairo, state->font, NULL, &text_height, NULL, 1, str);
	int text_y = (state->line_height / 2.0) - (text_height / 2.0);

	if (background) {
		int bg_width = state->width - x;
		cairo_set_source_u32(cairo, background);
		cairo_rectangle(cairo, x, y, bg_width, height);
		cairo_fill(cairo);
	}

	cairo_move_to(cairo, x + left_padding, y + text_y);
	cairo_set_source_u32(cairo, foreground);
	pango_printf(cairo, state->font, 1, str);
}

void scroll_matches(struct menu_state *state) {
	if (!state->matches) {
		return;
	}

	if (state->vertical) {
		if (state->leftmost == NULL) {
			state->leftmost = state->matches;
			if (state->rightmost == NULL) {
				int offs = 0;
				struct menu_item *item;
				for (item = state->matches; item->left != state->selection; item = item->right) {
					offs += state->line_height;
					if (offs >= state->height) {
						state->leftmost = item->left;
						offs = state->height - offs;
					}
				}
			} else {
				int offs = 0;
				struct menu_item *item;
				for (item = state->rightmost; item; item = item->left) {
					offs += state->line_height;
					if (offs >= state->height) {
						state->leftmost = item->right;
						break;
					}
				}
			}
		}
		if (state->rightmost == NULL) {
			state->rightmost = state->matches;
			int offs = 0;
			struct menu_item *item;
			for (item = state->leftmost; item; item = item->right) {
				offs += state->line_height;
				if (offs >= state->height) {
					break;
				}
				state->rightmost = item;
			}
		}
	} else {
		// Calculate available space
		int padding = state->padding;
		int width = state->width - state->inputw - state->promptw
			- state->left_arrow - state->right_arrow;
		if (state->leftmost == NULL) {
			state->leftmost = state->matches;
			if (state->rightmost == NULL) {
				int offs = 0;
				struct menu_item *item;
				for (item = state->matches; item->left != state->selection; item = item->right) {
					offs += item->width + 2 * padding;
					if (offs >= width) {
						state->leftmost = item->left;
						offs = width - offs;
					}
				}
			} else {
				int offs = 0;
				struct menu_item *item;
				for (item = state->rightmost; item; item = item->left) {
					offs += item->width + 2 * padding;
					if (offs >= width) {
						state->leftmost = item->right;
						break;
					}
				}
			}
		}
		if (state->rightmost == NULL) {
			state->rightmost = state->matches;
			int offs = 0;
			struct menu_item *item;
			for (item = state->leftmost; item; item = item->right) {
				offs += item->width + 2 * padding;
				if (offs >= width) {
					break;
				}
				state->rightmost = item;
			}
		}
	}
}

void render_to_cairo(struct menu_state *state, cairo_t *cairo) {
	int width = state->width;
	int padding = state->padding;


	cairo_set_operator(cairo, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_u32(cairo, state->background);
	cairo_paint(cairo);

	int x = 0;

	// Draw prompt
	if (state->prompt) {
		state->promptw = render_text(state, cairo, state->prompt,
				0, 0, state->width, state->line_height,
				state->promptfg, state->promptbg,
				padding, padding/2);
		x += state->promptw;
	}

	// Draw background
	cairo_set_source_u32(cairo, state->background);
	cairo_rectangle(cairo, x, 0, 300, state->height);
	cairo_fill(cairo);

	// Draw input
	render_text(state, cairo, state->text,
			x, 0, state->width, state->line_height,
			state->foreground, 0, padding, padding);

	// Draw cursor
	{
		int cursor_width = 2;
		int cursor_margin = 2;
		int cursor_pos = x + padding
			+ text_width(cairo, state->font, state->text)
			- text_width(cairo, state->font, &state->text[state->cursor])
			- cursor_width / 2;
		cairo_rectangle(cairo, cursor_pos, cursor_margin, cursor_width,
				state->line_height - 2 * cursor_margin);
		cairo_fill(cairo);
	}

	if (!state->matches) {
		return;
	}

	if (state->vertical) {
		// Draw matches vertically
		int y = state->line_height;
		struct menu_item *item;
		for (item = state->leftmost; item; item = item->right) {
			uint32_t bg_color = state->selection == item ? state->selectionbg : state->background;
			uint32_t fg_color = state->selection == item ? state->selectionfg : state->foreground;
			render_vertical_item(state, cairo, item->text,
				x, y, width, state->line_height,
				fg_color, bg_color, padding);
			y += state->line_height;
			if (y >= state->height) {
				break;
			}
		}
	} else {
		// Leave room for input
		x += state->inputw;

		// Calculate scroll indicator widths
		state->left_arrow = text_width(cairo, state->font, "<") + 2 * padding;
		state->right_arrow = text_width(cairo, state->font, ">") + 2 * padding;

		// Remember scroll indicator position
		int left_arrow_pos = x + padding;
		x += state->left_arrow;

		// Draw matches horizontally
		bool scroll_right = false;
		struct menu_item *item;
		for (item = state->leftmost; item; item = item->right) {
			uint32_t bg_color = state->selection == item ? state->selectionbg : state->background;
			uint32_t fg_color = state->selection == item ? state->selectionfg : state->foreground;
			x = render_horizontal_item(state, cairo, item->text,
				x, 0, width - state->right_arrow, state->line_height,
				fg_color, bg_color, padding, padding);
			if (x == -1) {
				scroll_right = true;
				break;
			}
		}

		// Draw left scroll indicator if necessary
		if (state->leftmost != state->matches) {
			cairo_move_to(cairo, left_arrow_pos, 0);
			pango_printf(cairo, state->font, 1, "<");
		}

		// Draw right scroll indicator if necessary
		if (scroll_right) {
			cairo_move_to(cairo, width - state->right_arrow + padding, 0);
			pango_printf(cairo, state->font, 1, ">");
		}
	}
}

void render_frame(struct menu_state *state) {
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

	render_to_cairo(state, cairo);

	int scale = state->output ? state->output->scale : 1;
	state->current = get_next_buffer(state->shm,
		state->buffers, state->width, state->height, scale);
	if (!state->current) {
		goto cleanup;
	}

	cairo_t *shm = state->current->cairo;
	cairo_save(shm);
	cairo_set_operator(shm, CAIRO_OPERATOR_CLEAR);
	cairo_paint(shm);
	cairo_restore(shm);
	cairo_set_source_surface(shm, recorder, 0, 0);
	cairo_paint(shm);

	wl_surface_attach(state->surface, state->current->buffer, 0, 0);
	wl_surface_damage(state->surface, 0, 0, state->width, state->height);
	wl_surface_commit(state->surface);

cleanup:
	cairo_destroy(cairo);
}

static void noop() {
	// Do nothing
}

static void surface_enter(void *data, struct wl_surface *surface,
		struct wl_output *wl_output) {
	struct menu_state *state = data;
	state->output = wl_output_get_user_data(wl_output);
	wl_surface_set_buffer_scale(state->surface, state->output->scale);
	wl_surface_commit(state->surface);
	render_frame(state);
}

static const struct wl_surface_listener surface_listener = {
	.enter = surface_enter,
	.leave = noop,
};

static void layer_surface_configure(void *data,
		struct zwlr_layer_surface_v1 *surface,
		uint32_t serial, uint32_t width, uint32_t height) {
	struct menu_state *state = data;
	state->width = width;
	state->height = height;
	zwlr_layer_surface_v1_ack_configure(surface, serial);
}

static void layer_surface_closed(void *data,
		struct zwlr_layer_surface_v1 *surface) {
	struct menu_state *state = data;
	state->run = false;
}

struct zwlr_layer_surface_v1_listener layer_surface_listener = {
	.configure = layer_surface_configure,
	.closed = layer_surface_closed,
};

static void output_scale(void *data, struct wl_output *wl_output, int32_t factor) {
	struct output *output = data;
	output->scale = factor;
}

static void output_name(void *data, struct zxdg_output_v1 *xdg_output,
		const char *name) {
	struct output *output = data;
	struct menu_state *state = output->menu;
	char *outname = state->output_name;
	if (!state->output && outname && strcmp(outname, name) == 0) {
		state->output = output;
	}
}

struct wl_output_listener output_listener = {
	.geometry = noop,
	.mode = noop,
	.done = noop,
	.scale = output_scale,
};

static void keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t format, int32_t fd, uint32_t size) {
	struct menu_state *state = data;
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		state->run = false;
		state->failure = true;
		return;
	}
	char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	if (map_shm == MAP_FAILED) {
		close(fd);
		state->run = false;
		state->failure = true;
		return;
	}
	state->xkb_keymap = xkb_keymap_new_from_string(state->xkb_context,
		map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	munmap(map_shm, size);
	close(fd);
	state->xkb_state = xkb_state_new(state->xkb_keymap);
}

void keypress(struct menu_state *state, enum wl_keyboard_key_state key_state,
		xkb_keysym_t sym) {
	if (key_state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}

	bool ctrl = xkb_state_mod_name_is_active(state->xkb_state,
			XKB_MOD_NAME_CTRL,
			XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);
	bool shift = xkb_state_mod_name_is_active(state->xkb_state,
			XKB_MOD_NAME_SHIFT,
			XKB_STATE_MODS_DEPRESSED | XKB_STATE_MODS_LATCHED);

	size_t len = strlen(state->text);

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
			state->text[state->cursor] = '\0';
			match(state);
			render_frame(state);
			return;
		case XKB_KEY_u:
			// Delete left
			insert(state, NULL, 0 - state->cursor);
			render_frame(state);
			return;
		case XKB_KEY_w:
			// Delete word
			while (state->cursor > 0 && state->text[nextrune(state, -1)] == ' ') {
				insert(state, NULL, nextrune(state, -1) - state->cursor);
			}
			while (state->cursor > 0 && state->text[nextrune(state, -1)] != ' ') {
				insert(state, NULL, nextrune(state, -1) - state->cursor);
			}
			render_frame(state);
			return;
		case XKB_KEY_Left:
		case XKB_KEY_KP_Left:
			// Move to beginning of word
			while (state->cursor > 0 && state->text[nextrune(state, -1)] == ' ') {
				state->cursor = nextrune(state, -1);
			}
			while (state->cursor > 0 && state->text[nextrune(state, -1)] != ' ') {
				state->cursor = nextrune(state, -1);
			}
			render_frame(state);
			return;
		case XKB_KEY_Right:
		case XKB_KEY_KP_Right:
			// Move to end of word
			while (state->cursor < len && state->text[state->cursor] == ' ') {
				state->cursor = nextrune(state, +1);
			}
			while (state->cursor < len && state->text[state->cursor] != ' ') {
				state->cursor = nextrune(state, +1);
			}
			render_frame(state);
			return;
		}
	}

	char buf[8];
	switch (sym) {
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		if (shift) {
			puts(state->text);
			fflush(stdout);
			state->run = false;
		} else {
			char *text = state->selection ? state->selection->text
				: state->text;
			puts(text);
			fflush(stdout);
			if (!ctrl) {
				state->run = false;
			}
		}
		break;
	case XKB_KEY_Left:
	case XKB_KEY_KP_Left:
		if (state->vertical) {
			break;
		}
		if (state->cursor && (!state->selection || !state->selection->left)) {
			state->cursor = nextrune(state, -1);
			render_frame(state);
		}
		if (state->selection && state->selection->left) {
			if (state->selection == state->leftmost) {
				state->rightmost = state->selection->left;
				state->leftmost = NULL;
			}
			state->selection = state->selection->left;
			scroll_matches(state);
			render_frame(state);
		}
		break;
	case XKB_KEY_Right:
	case XKB_KEY_KP_Right:
		if (state->vertical) {
			break;
		}
		if (state->cursor < len) {
			state->cursor = nextrune(state, +1);
			render_frame(state);
		} else if (state->cursor == len) {
			if (state->selection && state->selection->right) {
				if (state->selection == state->rightmost) {
					state->leftmost = state->selection->right;
					state->rightmost = NULL;
				}
				state->selection = state->selection->right;
				scroll_matches(state);
				render_frame(state);
			}
		}
		break;
	case XKB_KEY_Up:
	case XKB_KEY_KP_Up:
		if (!state->vertical) {
			break;
		}
		if (state->cursor && (!state->selection || !state->selection->left)) {
			state->cursor = nextrune(state, -1);
			render_frame(state);
		}
		if (state->selection && state->selection->left) {
			if (state->selection == state->leftmost) {
				state->rightmost = state->selection->left;
				state->leftmost = NULL;
			}
			state->selection = state->selection->left;
			scroll_matches(state);
			render_frame(state);
		}
		break;
	case XKB_KEY_Down:
	case XKB_KEY_KP_Down:
		if (!state->vertical) {
			break;
		}
		if (state->cursor < len) {
			state->cursor = nextrune(state, +1);
			render_frame(state);
		} else if (state->cursor == len) {
			if (state->selection && state->selection->right) {
				if (state->selection == state->rightmost) {
					state->leftmost = state->selection->right;
					state->rightmost = NULL;
				}
				state->selection = state->selection->right;
				scroll_matches(state);
				render_frame(state);
			}
		}
		break;
	case XKB_KEY_Page_Up:
	case XKB_KEY_KP_Page_Up:
		if (state->leftmost && state->leftmost->left) {
			state->rightmost = state->leftmost->left;
			state->leftmost = NULL;
			scroll_matches(state);
			state->selection = state->leftmost;
			render_frame(state);
		}
		break;
	case XKB_KEY_Page_Down:
	case XKB_KEY_KP_Page_Down:
		if (state->rightmost && state->rightmost->right) {
			state->leftmost = state->rightmost->right;
			state->rightmost = NULL;
			state->selection = state->leftmost;
			scroll_matches(state);
			render_frame(state);
		}
		break;
	case XKB_KEY_Home:
	case XKB_KEY_KP_Home:
		if (state->selection == state->matches) {
			if (state->cursor != 0) {
				state->cursor = 0;
				render_frame(state);
			}
		} else {
			state->selection = state->matches;
			state->leftmost = state->matches;
			state->rightmost = NULL;
			scroll_matches(state);
			render_frame(state);
		}
		break;
	case XKB_KEY_End:
	case XKB_KEY_KP_End:
		if (state->cursor < len) {
			state->cursor = len;
			render_frame(state);
		} else {
			if (!state->selection || !state->selection->right) {
				return;
			}
			while (state->selection && state->selection->right) {
				state->selection = state->selection->right;
			}
			state->leftmost = NULL;
			state->rightmost = state->selection;
			scroll_matches(state);
			render_frame(state);
		}
		break;
	case XKB_KEY_BackSpace:
		if (state->cursor > 0) {
			insert(state, NULL, nextrune(state, -1) - state->cursor);
			render_frame(state);
		}
		break;
	case XKB_KEY_Delete:
	case XKB_KEY_KP_Delete:
		if (state->cursor == len) {
			return;
		}
		state->cursor = nextrune(state, +1);
		insert(state, NULL, nextrune(state, -1) - state->cursor);
		render_frame(state);
		break;
	case XKB_KEY_Tab:
		if (!state->selection) {
			return;
		}
		strncpy(state->text, state->selection->text, sizeof state->text);
		state->cursor = strlen(state->text);
		match(state);
		render_frame(state);
		break;
	case XKB_KEY_Escape:
		state->failure = true;
		state->run = false;
		break;
	default:
		if (xkb_keysym_to_utf8(sym, buf, 8)) {
			insert(state, buf, strnlen(buf, 8));
			render_frame(state);
		}
	}
}

void keyboard_repeat(struct menu_state *state) {
	keypress(state, state->repeat_key_state, state->repeat_sym);
	struct itimerspec spec = { 0 };
	spec.it_value.tv_sec = state->repeat_period / 1000;
	spec.it_value.tv_nsec = (state->repeat_period % 1000) * 1000000l;
	timerfd_settime(state->repeat_timer, 0, &spec, NULL);
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t _key_state) {
	struct menu_state *state = data;

	enum wl_keyboard_key_state key_state = _key_state;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(state->xkb_state, key + 8);
	keypress(state, key_state, sym);

	if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED && state->repeat_period >= 0) {
		state->repeat_key_state = key_state;
		state->repeat_sym = sym;

		struct itimerspec spec = { 0 };
		spec.it_value.tv_sec = state->repeat_delay / 1000;
		spec.it_value.tv_nsec = (state->repeat_delay % 1000) * 1000000l;
		timerfd_settime(state->repeat_timer, 0, &spec, NULL);
	} else if (key_state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		struct itimerspec spec = { 0 };
		timerfd_settime(state->repeat_timer, 0, &spec, NULL);
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	struct menu_state *state = data;
	state->repeat_delay = delay;
	if (rate > 0) {
		state->repeat_period = 1000 / rate;
	} else {
		state->repeat_period = -1;
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *keyboard,
		uint32_t serial, uint32_t mods_depressed,
		uint32_t mods_latched, uint32_t mods_locked,
		uint32_t group) {
	struct menu_state *state = data;
	xkb_state_update_mask(state->xkb_state, mods_depressed, mods_latched,
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
	struct menu_state *state = data;
	if (caps & WL_SEAT_CAPABILITY_KEYBOARD) {
		state->keyboard = wl_seat_get_keyboard(seat);
		wl_keyboard_add_listener(state->keyboard, &keyboard_listener, state);
	}
}

const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = noop,
};

struct zxdg_output_v1_listener xdg_output_listener = {
	.logical_position = noop,
	.logical_size = noop,
	.done = noop,
	.name = output_name,
	.description = noop,
};

static void handle_global(void *data, struct wl_registry *registry,
		uint32_t name, const char *interface, uint32_t version) {
	struct menu_state *state = data;
	if (strcmp(interface, wl_compositor_interface.name) == 0) {
		state->compositor = wl_registry_bind(registry, name,
				&wl_compositor_interface, 4);
	} else if (strcmp(interface, wl_shm_interface.name) == 0) {
		state->shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
	} else if (strcmp(interface, wl_seat_interface.name) == 0) {
		struct wl_seat *seat = wl_registry_bind(registry, name,
				&wl_seat_interface, 4);
		wl_seat_add_listener(seat, &seat_listener, state);
	} else if (strcmp(interface, zwlr_layer_shell_v1_interface.name) == 0) {
		state->layer_shell = wl_registry_bind(registry, name,
				&zwlr_layer_shell_v1_interface, 1);
	} else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
		state->output_manager = wl_registry_bind(registry, name,
			&zxdg_output_manager_v1_interface, 3);
	} else if (strcmp(interface, wl_output_interface.name) == 0) {
		struct output *output = calloc(1, sizeof(struct output));
		output->output = wl_registry_bind(registry, name,
				&wl_output_interface, 3);
		output->menu = state;
		output->scale = 1;
		wl_output_set_user_data(output->output, output);
		wl_output_add_listener(output->output, &output_listener, output);
		if (state->output_manager != NULL) {
                       output->xdg_output = zxdg_output_manager_v1_get_xdg_output(
                               state->output_manager, output->output);
                       zxdg_output_v1_add_listener(output->xdg_output,
                               &xdg_output_listener, output);
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = handle_global,
	.global_remove = noop,
};

void insert(struct menu_state *state, const char *s, ssize_t n) {
	if (strlen(state->text) + n > sizeof state->text - 1) {
		return;
	}
	memmove(state->text + state->cursor + n, state->text + state->cursor,
			sizeof state->text - state->cursor - MAX(n, 0));
	if (n > 0) {
		memcpy(state->text + state->cursor, s, n);
	}
	state->cursor += n;
	match(state);
}

char * fstrstr(struct menu_state *state, const char *s, const char *sub) {
	size_t len;

	for(len = strlen(sub); *s; s++)
		if(!state->fstrncmp(s, sub, len))
			return (char *)s;
	return NULL;
}

void append_item(struct menu_item *item, struct menu_item **list, struct menu_item **last) {
	if(!*last)
		*list = item;
	else
		(*last)->right = item;
	item->left = *last;
	item->right = NULL;
	*last = item;
}

void match(struct menu_state *state) {
	struct menu_item *item, *itemend, *lexact, *lprefix, *lsubstr, *exactend, *prefixend, *substrend;

	state->matches = NULL;
	state->leftmost = NULL;
	size_t len = strlen(state->text);
	state->matches = lexact = lprefix = lsubstr = itemend = exactend = prefixend = substrend = NULL;
	for (item = state->items; item; item = item->next) {
		if (!state->fstrncmp(state->text, item->text, len + 1)) {
			append_item(item, &lexact, &exactend);
		} else if (!state->fstrncmp(state->text, item->text, len)) {
			append_item(item, &lprefix, &prefixend);
		} else if (fstrstr(state, item->text, state->text)) {
			append_item(item, &lsubstr, &substrend);
		}
	}

	if (lexact) {
		state->matches = lexact;
		itemend = exactend;
	}
	if (lprefix) {
		if (itemend) {
			itemend->right = lprefix;
			lprefix->left = itemend;
		} else {
			state->matches = lprefix;
		}
		itemend = prefixend;
	}
	if (lsubstr) {
		if (itemend) {
			itemend->right = lsubstr;
			lsubstr->left = itemend;
			itemend = substrend;
		} else {
			state->matches = lsubstr;
		}
	}
	state->selection = state->matches;
	state->leftmost = state->matches;
	state->rightmost = NULL;
	scroll_matches(state);
}

size_t nextrune(struct menu_state *state, int incr) {
	size_t n, len;

	len = strlen(state->text);
	for(n = state->cursor + incr; n < len && (state->text[n] & 0xc0) == 0x80; n += incr);
	return n;
}

void read_stdin(struct menu_state *state) {
	char buf[sizeof state->text], *p;
	struct menu_item *item, **end;

	for(end = &state->items; fgets(buf, sizeof buf, stdin); *end = item, end = &item->next) {
		if((p = strchr(buf, '\n'))) {
			*p = '\0';
		}
		item = malloc(sizeof *item);
		if (!item) {
			return;
		}

		item->text = strdup(buf);
		item->next = item->left = item->right = NULL;

		cairo_t *cairo = state->current->cairo;
		item->width = text_width(cairo, state->font, item->text);
		if (item->width > state->inputw) {
			state->inputw = item->width;
		}
	}
}

static void menu_init(struct menu_state *state) {
	int height = get_font_height(state->font);
	state->line_height = height + 3;
	state->height = state->line_height;
	if (state->vertical) {
		state->height += state->height * state->lines;
	}
	state->padding = height / 2;

	state->display = wl_display_connect(NULL);
	if (!state->display) {
		fprintf(stderr, "wl_display_connect: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	state->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (!state->xkb_context) {
		fprintf(stderr, "xkb_context_new: %s\n", strerror(errno));
		exit(EXIT_FAILURE);
	}

	state->repeat_timer = timerfd_create(CLOCK_MONOTONIC, 0);
	assert(state->repeat_timer >= 0);

	struct wl_registry *registry = wl_display_get_registry(state->display);
	wl_registry_add_listener(registry, &registry_listener, state);
	wl_display_roundtrip(state->display);
	assert(state->compositor != NULL);
	assert(state->layer_shell != NULL);
	assert(state->shm != NULL);
	assert(state->output_manager != NULL);

	// Second roundtrip for xdg-output
	wl_display_roundtrip(state->display);

	if (state->output_name && !state->output) {
		fprintf(stderr, "Output %s not found\n", state->output_name);
		exit(EXIT_FAILURE);
	}
}

static void menu_create_surface(struct menu_state *state) {
	state->surface = wl_compositor_create_surface(state->compositor);
	wl_surface_add_listener(state->surface, &surface_listener, state);
	state->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		state->layer_shell,
		state->surface,
		NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		"menu"
	);
	assert(state->layer_surface != NULL);

	uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	if (state->bottom) {
		anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	} else {
		anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	}

	zwlr_layer_surface_v1_set_anchor(state->layer_surface, anchor);
	zwlr_layer_surface_v1_set_size(state->layer_surface, 0, state->height);
	zwlr_layer_surface_v1_set_exclusive_zone(state->layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(state->layer_surface, true);
	zwlr_layer_surface_v1_add_listener(state->layer_surface,
		&layer_surface_listener, state);

	wl_surface_commit(state->surface);
	wl_display_roundtrip(state->display);
}

bool parse_color(const char *color, uint32_t *result) {
	if (color[0] == '#') {
		++color;
	}
	int len = strlen(color);
	if ((len != 6 && len != 8) || !isxdigit(color[0]) || !isxdigit(color[1])) {
		return false;
	}
	char *ptr;
	uint32_t parsed = strtoul(color, &ptr, 16);
	if (*ptr != '\0') {
		return false;
	}
	*result = len == 6 ? ((parsed << 8) | 0xFF) : parsed;
	return true;
}

int main(int argc, char **argv) {
	struct menu_state state = {
		.fstrncmp = strncmp,
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
			state.bottom = true;
			break;
		case 'i':
			state.fstrncmp = strncasecmp;
			break;
		case 'v':
			puts("wmenu " VERSION);
			exit(EXIT_SUCCESS);
		case 'f':
			state.font = optarg;
			break;
		case 'l':
			state.vertical = true;
			state.lines = atoi(optarg);
			break;
		case 'o':
			state.output_name = optarg;
			break;
		case 'p':
			state.prompt = optarg;
			break;
		case 'N':
			if (!parse_color(optarg, &state.background)) {
				fprintf(stderr, "Invalid background color: %s", optarg);
			}
			break;
		case 'n':
			if (!parse_color(optarg, &state.foreground)) {
				fprintf(stderr, "Invalid foreground color: %s", optarg);
			}
			break;
		case 'M':
			if (!parse_color(optarg, &state.promptbg)) {
				fprintf(stderr, "Invalid prompt background color: %s", optarg);
			}
			break;
		case 'm':
			if (!parse_color(optarg, &state.promptfg)) {
				fprintf(stderr, "Invalid prompt foreground color: %s", optarg);
			}
			break;
		case 'S':
			if (!parse_color(optarg, &state.selectionbg)) {
				fprintf(stderr, "Invalid selection background color: %s", optarg);
			}
			break;
		case 's':
			if (!parse_color(optarg, &state.selectionfg)) {
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

	menu_init(&state);
	menu_create_surface(&state);
	render_frame(&state);

	read_stdin(&state);
	match(&state);

	struct pollfd fds[] = {
		{ wl_display_get_fd(state.display), POLLIN },
		{ state.repeat_timer, POLLIN },
	};
	const int nfds = sizeof(fds) / sizeof(*fds);

	while (state.run) {
		errno = 0;
		do {
			if (wl_display_flush(state.display) == -1 && errno != EAGAIN) {
				fprintf(stderr, "wl_display_flush: %s\n", strerror(errno));
				break;
			}
		} while (errno == EAGAIN);

		if (poll(fds, nfds, -1) < 0) {
			fprintf(stderr, "poll: %s\n", strerror(errno));
			break;
		}

		if (fds[0].revents & POLLIN) {
			if (wl_display_dispatch(state.display) < 0) {
				state.run = false;
			}
		}

		if (fds[1].revents & POLLIN) {
			keyboard_repeat(&state);
		}
	}

	wl_display_disconnect(state.display);

	if (state.failure) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
