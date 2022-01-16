/* Taken from sway. MIT licensed */
#include <cairo.h>
#include <pango/pangocairo.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct pool_buffer {
	struct wl_buffer *buffer;
	cairo_surface_t *surface;
	cairo_t *cairo;
	PangoContext *pango;
	size_t size;
	int32_t width, height, scale;
	void *data;
	bool busy;
};

struct pool_buffer *get_next_buffer(struct wl_shm *shm,
		struct pool_buffer pool[static 2], int32_t width, int32_t height, int32_t scale);
void destroy_buffer(struct pool_buffer *buffer);
