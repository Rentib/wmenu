#define _POSIX_C_SOURCE 200809L
#include <assert.h>
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

#include "menu.h"
#include "render.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static void noop() {
	// Do nothing
}

static void surface_enter(void *data, struct wl_surface *surface,
		struct wl_output *wl_output) {
	struct menu *menu = data;
	struct output *output = wl_output_get_user_data(wl_output);
	menu->output = output;
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
	menu->exit = true;
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
	output->name = name;

	struct menu *menu = output->menu;
	if (menu->output_name && strcmp(menu->output_name, name) == 0) {
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
	assert(format == WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1);
	struct keyboard *keyboard = data;

	char *map_shm = mmap(NULL, size, PROT_READ, MAP_SHARED, fd, 0);
	assert (map_shm != MAP_FAILED);

	struct xkb_keymap *keymap = xkb_keymap_new_from_string(keyboard->xkb_context,
		map_shm, XKB_KEYMAP_FORMAT_TEXT_V1, 0);
	munmap(map_shm, size);
	close(fd);

	keyboard->xkb_state = xkb_state_new(keymap);
}

static void keyboard_repeat(struct keyboard *keyboard) {
	menu_keypress(keyboard->menu, keyboard->repeat_key_state, keyboard->repeat_sym);
	struct itimerspec spec = { 0 };
	spec.it_value.tv_sec = keyboard->repeat_period / 1000;
	spec.it_value.tv_nsec = (keyboard->repeat_period % 1000) * 1000000l;
	timerfd_settime(keyboard->repeat_timer, 0, &spec, NULL);
}

static void keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t time, uint32_t key, uint32_t _key_state) {
	struct keyboard *keyboard = data;

	enum wl_keyboard_key_state key_state = _key_state;
	xkb_keysym_t sym = xkb_state_key_get_one_sym(keyboard->xkb_state, key + 8);
	menu_keypress(keyboard->menu, key_state, sym);

	if (key_state == WL_KEYBOARD_KEY_STATE_PRESSED && keyboard->repeat_period >= 0) {
		keyboard->repeat_key_state = key_state;
		keyboard->repeat_sym = sym;

		struct itimerspec spec = { 0 };
		spec.it_value.tv_sec = keyboard->repeat_delay / 1000;
		spec.it_value.tv_nsec = (keyboard->repeat_delay % 1000) * 1000000l;
		timerfd_settime(keyboard->repeat_timer, 0, &spec, NULL);
	} else if (key_state == WL_KEYBOARD_KEY_STATE_RELEASED) {
		struct itimerspec spec = { 0 };
		timerfd_settime(keyboard->repeat_timer, 0, &spec, NULL);
	}
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
		int32_t rate, int32_t delay) {
	struct keyboard *keyboard = data;
	keyboard->repeat_delay = delay;
	if (rate > 0) {
		keyboard->repeat_period = 1000 / rate;
	} else {
		keyboard->repeat_period = -1;
	}
}

static void keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
		uint32_t serial, uint32_t mods_depressed,
		uint32_t mods_latched, uint32_t mods_locked,
		uint32_t group) {
	struct keyboard *keyboard = data;
	xkb_state_update_mask(keyboard->xkb_state, mods_depressed, mods_latched,
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
		wl_keyboard_add_listener(keyboard, &keyboard_listener, menu->keyboard);
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

static void keyboard_init(struct keyboard *keyboard, struct menu *menu) {
	keyboard->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	assert(keyboard->xkb_context != NULL);
	keyboard->repeat_timer = timerfd_create(CLOCK_MONOTONIC, 0);
	assert(keyboard->repeat_timer >= 0);
	keyboard->menu = menu;
}

static void create_surface(struct menu *menu) {
	menu->display = wl_display_connect(NULL);
	if (!menu->display) {
		fprintf(stderr, "Failed to connect to display.\n");
		exit(EXIT_FAILURE);
	}

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

	menu->surface = wl_compositor_create_surface(menu->compositor);
	wl_surface_add_listener(menu->surface, &surface_listener, menu);

	struct zwlr_layer_surface_v1 *layer_surface = zwlr_layer_shell_v1_get_layer_surface(
		menu->layer_shell,
		menu->surface,
		menu->output ? menu->output->output : NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP,
		"menu"
	);
	assert(layer_surface != NULL);

	uint32_t anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
	if (menu->bottom) {
		anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
	} else {
		anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
	}

	zwlr_layer_surface_v1_set_anchor(layer_surface, anchor);
	zwlr_layer_surface_v1_set_size(layer_surface, 0, menu->height);
	zwlr_layer_surface_v1_set_exclusive_zone(layer_surface, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(layer_surface, true);
	zwlr_layer_surface_v1_add_listener(layer_surface, &layer_surface_listener, menu);

	wl_surface_commit(menu->surface);
	wl_display_roundtrip(menu->display);
}

int main(int argc, char *argv[]) {
	struct menu *menu = calloc(1, sizeof(struct menu));
	menu_init(menu, argc, argv);

	struct keyboard *keyboard = calloc(1, sizeof(struct keyboard));
	keyboard_init(keyboard, menu);
	menu->keyboard = keyboard;

	create_surface(menu);
	render_menu(menu);

	read_menu_items(menu);
	render_menu(menu);

	struct pollfd fds[] = {
		{ wl_display_get_fd(menu->display), POLLIN },
		{ keyboard->repeat_timer, POLLIN },
	};
	const size_t nfds = sizeof(fds) / sizeof(*fds);

	while (!menu->exit) {
		errno = 0;
		do {
			if (wl_display_flush(menu->display) == -1 && errno != EAGAIN) {
				fprintf(stderr, "wl_display_flush: %s\n", strerror(errno));
				break;
			}
		} while (errno == EAGAIN);

		if (poll(fds, nfds, -1) < 0) {
			fprintf(stderr, "poll: %s\n", strerror(errno));
			break;
		}

		if (fds[0].revents & POLLIN) {
			if (wl_display_dispatch(menu->display) < 0) {
				menu->exit = true;
			}
		}

		if (fds[1].revents & POLLIN) {
			keyboard_repeat(keyboard);
		}
	}

	wl_display_disconnect(menu->display);

	if (menu->failure) {
		return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
