#include <cairo/cairo.h>

#include "render.h"

#include "menu.h"
#include "pango.h"

// Calculate text widths.
void calc_widths(struct menu *menu) {
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
static void render_to_cairo(struct menu *menu, cairo_t *cairo) {
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
	if (menu->lines > 0) {
		render_vertical_page(menu, cairo, menu->sel->page);
	} else {
		render_horizontal_page(menu, cairo, menu->sel->page);
	}
}

// Renders a single frame of the menu.
void render_menu(struct menu *menu) {
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

	render_to_cairo(menu, cairo);

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
