#define _POSIX_C_SOURCE 199309L
#include <stdbool.h>
#include <stdlib.h>
#include <wayland-server.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/edges.h>
#include "log.h"
#include "sway/input/input-manager.h"
#include "sway/input/seat.h"
#include "sway/server.h"
#include "sway/tree/arrange.h"
#include "sway/tree/container.h"
#include "sway/tree/layout.h"
#include "sway/tree/view.h"

static const struct sway_view_child_impl popup_impl;

static void popup_destroy(struct sway_view_child *child) {
	if (!sway_assert(child->impl == &popup_impl,
			"Expected an xdg_shell popup")) {
		return;
	}
	struct sway_xdg_popup *popup = (struct sway_xdg_popup *)child;
	wl_list_remove(&popup->new_popup.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static const struct sway_view_child_impl popup_impl = {
	.destroy = popup_destroy,
};

static struct sway_xdg_popup *popup_create(
	struct wlr_xdg_popup *wlr_popup, struct sway_view *view);

static void popup_handle_new_popup(struct wl_listener *listener, void *data) {
	struct sway_xdg_popup *popup =
		wl_container_of(listener, popup, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	popup_create(wlr_popup, popup->child.view);
}

static void popup_handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_xdg_popup *popup = wl_container_of(listener, popup, destroy);
	view_child_destroy(&popup->child);
}

static void popup_unconstrain(struct sway_xdg_popup *popup) {
	struct sway_view *view = popup->child.view;
	struct wlr_xdg_popup *wlr_popup = popup->wlr_xdg_surface->popup;

	struct sway_container *output = container_parent(view->swayc, C_OUTPUT);

	// the output box expressed in the coordinate system of the toplevel parent
	// of the popup
	struct wlr_box output_toplevel_sx_box = {
		.x = output->x - view->x,
		.y = output->y - view->y,
		.width = output->width,
		.height = output->height,
	};

	wlr_xdg_popup_unconstrain_from_box(wlr_popup, &output_toplevel_sx_box);
}

static struct sway_xdg_popup *popup_create(
		struct wlr_xdg_popup *wlr_popup, struct sway_view *view) {
	struct wlr_xdg_surface *xdg_surface = wlr_popup->base;

	struct sway_xdg_popup *popup =
		calloc(1, sizeof(struct sway_xdg_popup));
	if (popup == NULL) {
		return NULL;
	}
	view_child_init(&popup->child, &popup_impl, view, xdg_surface->surface);
	popup->wlr_xdg_surface = xdg_surface;

	wl_signal_add(&xdg_surface->events.new_popup, &popup->new_popup);
	popup->new_popup.notify = popup_handle_new_popup;
	wl_signal_add(&xdg_surface->events.destroy, &popup->destroy);
	popup->destroy.notify = popup_handle_destroy;

	popup_unconstrain(popup);

	return popup;
}


static struct sway_xdg_shell_view *xdg_shell_view_from_view(
		struct sway_view *view) {
	if (!sway_assert(view->type == SWAY_VIEW_XDG_SHELL,
			"Expected xdg_shell view")) {
		return NULL;
	}
	return (struct sway_xdg_shell_view *)view;
}

static const char *get_string_prop(struct sway_view *view, enum sway_view_prop prop) {
	if (xdg_shell_view_from_view(view) == NULL) {
		return NULL;
	}
	switch (prop) {
	case VIEW_PROP_TITLE:
		return view->wlr_xdg_surface->toplevel->title;
	case VIEW_PROP_APP_ID:
		return view->wlr_xdg_surface->toplevel->app_id;
	default:
		return NULL;
	}
}

static uint32_t configure(struct sway_view *view, double lx, double ly,
		int width, int height) {
	struct sway_xdg_shell_view *xdg_shell_view =
		xdg_shell_view_from_view(view);
	if (xdg_shell_view == NULL) {
		return 0;
	}
	return wlr_xdg_toplevel_set_size(view->wlr_xdg_surface, width, height);
}

static void set_activated(struct sway_view *view, bool activated) {
	if (xdg_shell_view_from_view(view) == NULL) {
		return;
	}
	struct wlr_xdg_surface *surface = view->wlr_xdg_surface;
	if (surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wlr_xdg_toplevel_set_activated(surface, activated);
	}
}

static void set_tiled(struct sway_view *view, bool tiled) {
	if (xdg_shell_view_from_view(view) == NULL) {
		return;
	}
	struct wlr_xdg_surface *surface = view->wlr_xdg_surface;
	enum wlr_edges edges = WLR_EDGE_NONE;
	if (tiled) {
		edges = WLR_EDGE_LEFT | WLR_EDGE_RIGHT | WLR_EDGE_TOP |
				WLR_EDGE_BOTTOM;
	}
	wlr_xdg_toplevel_set_tiled(surface, edges);
}

static void set_fullscreen(struct sway_view *view, bool fullscreen) {
	if (xdg_shell_view_from_view(view) == NULL) {
		return;
	}
	struct wlr_xdg_surface *surface = view->wlr_xdg_surface;
	wlr_xdg_toplevel_set_fullscreen(surface, fullscreen);
}

static bool wants_floating(struct sway_view *view) {
	struct wlr_xdg_toplevel *toplevel = view->wlr_xdg_surface->toplevel;
	struct wlr_xdg_toplevel_state *state = &toplevel->current;
	return (state->min_width != 0 && state->min_height != 0
		&& state->min_width == state->max_width
		&& state->min_height == state->max_height)
		|| toplevel->parent;
}

static void for_each_surface(struct sway_view *view,
		wlr_surface_iterator_func_t iterator, void *user_data) {
	if (xdg_shell_view_from_view(view) == NULL) {
		return;
	}
	wlr_xdg_surface_for_each_surface(view->wlr_xdg_surface, iterator,
		user_data);
}

static void _close(struct sway_view *view) {
	if (xdg_shell_view_from_view(view) == NULL) {
		return;
	}
	struct wlr_xdg_surface *surface = view->wlr_xdg_surface;
	if (surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
		wlr_xdg_surface_send_close(surface);
	}
}

static void destroy(struct sway_view *view) {
	struct sway_xdg_shell_view *xdg_shell_view =
		xdg_shell_view_from_view(view);
	if (xdg_shell_view == NULL) {
		return;
	}
	free(xdg_shell_view);
}

static const struct sway_view_impl view_impl = {
	.get_string_prop = get_string_prop,
	.configure = configure,
	.set_activated = set_activated,
	.set_tiled = set_tiled,
	.set_fullscreen = set_fullscreen,
	.wants_floating = wants_floating,
	.for_each_surface = for_each_surface,
	.close = _close,
	.destroy = destroy,
};

static void handle_commit(struct wl_listener *listener, void *data) {
	struct sway_xdg_shell_view *xdg_shell_view =
		wl_container_of(listener, xdg_shell_view, commit);
	struct sway_view *view = &xdg_shell_view->view;
	struct wlr_xdg_surface *xdg_surface = view->wlr_xdg_surface;

	if (!view->swayc) {
		return;
	}

	if (view->swayc->instructions->length) {
		transaction_notify_view_ready(view, xdg_surface->configure_serial);
	}

	view_update_title(view, false);
	view_damage_from(view);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
	struct sway_xdg_shell_view *xdg_shell_view =
		wl_container_of(listener, xdg_shell_view, new_popup);
	struct wlr_xdg_popup *wlr_popup = data;
	popup_create(wlr_popup, &xdg_shell_view->view);
}

static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
	struct sway_xdg_shell_view *xdg_shell_view =
		wl_container_of(listener, xdg_shell_view, request_fullscreen);
	struct wlr_xdg_toplevel_set_fullscreen_event *e = data;
	struct wlr_xdg_surface *xdg_surface =
		xdg_shell_view->view.wlr_xdg_surface;
	struct sway_view *view = &xdg_shell_view->view;

	if (!sway_assert(xdg_surface->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL,
				"xdg_shell requested fullscreen of surface with role %i",
				xdg_surface->role)) {
		return;
	}
	if (!xdg_surface->mapped) {
		return;
	}

	view_set_fullscreen(view, e->fullscreen);

	struct sway_container *output = container_parent(view->swayc, C_OUTPUT);
	arrange_windows(output);
	transaction_commit_dirty();
}

static void handle_unmap(struct wl_listener *listener, void *data) {
	struct sway_xdg_shell_view *xdg_shell_view =
		wl_container_of(listener, xdg_shell_view, unmap);
	struct sway_view *view = &xdg_shell_view->view;

	if (!sway_assert(view->surface, "Cannot unmap unmapped view")) {
		return;
	}

	view_unmap(view);

	wl_list_remove(&xdg_shell_view->commit.link);
	wl_list_remove(&xdg_shell_view->new_popup.link);
	wl_list_remove(&xdg_shell_view->request_fullscreen.link);
}

static void handle_map(struct wl_listener *listener, void *data) {
	struct sway_xdg_shell_view *xdg_shell_view =
		wl_container_of(listener, xdg_shell_view, map);
	struct sway_view *view = &xdg_shell_view->view;
	struct wlr_xdg_surface *xdg_surface = view->wlr_xdg_surface;

	view->natural_width = view->wlr_xdg_surface->geometry.width;
	view->natural_height = view->wlr_xdg_surface->geometry.height;
	if (!view->natural_width && !view->natural_height) {
		view->natural_width = view->wlr_xdg_surface->surface->current.width;
		view->natural_height = view->wlr_xdg_surface->surface->current.height;
	}

	view_map(view, view->wlr_xdg_surface->surface);

	if (xdg_surface->toplevel->client_pending.fullscreen) {
		view_set_fullscreen(view, true);
		struct sway_container *ws = container_parent(view->swayc, C_WORKSPACE);
		arrange_windows(ws);
	} else {
		arrange_windows(view->swayc->parent);
	}
	transaction_commit_dirty();

	xdg_shell_view->commit.notify = handle_commit;
	wl_signal_add(&xdg_surface->surface->events.commit,
		&xdg_shell_view->commit);

	xdg_shell_view->new_popup.notify = handle_new_popup;
	wl_signal_add(&xdg_surface->events.new_popup,
		&xdg_shell_view->new_popup);

	xdg_shell_view->request_fullscreen.notify = handle_request_fullscreen;
	wl_signal_add(&xdg_surface->toplevel->events.request_fullscreen,
			&xdg_shell_view->request_fullscreen);
}

static void handle_destroy(struct wl_listener *listener, void *data) {
	struct sway_xdg_shell_view *xdg_shell_view =
		wl_container_of(listener, xdg_shell_view, destroy);
	struct sway_view *view = &xdg_shell_view->view;
	if (!sway_assert(view->swayc == NULL || view->swayc->destroying,
				"Tried to destroy a mapped view")) {
		return;
	}
	wl_list_remove(&xdg_shell_view->destroy.link);
	wl_list_remove(&xdg_shell_view->map.link);
	wl_list_remove(&xdg_shell_view->unmap.link);
	view->wlr_xdg_surface = NULL;
	view_destroy(view);
}

struct sway_view *view_from_wlr_xdg_surface(
		struct wlr_xdg_surface *xdg_surface) {
	return xdg_surface->data;
}

void handle_xdg_shell_surface(struct wl_listener *listener, void *data) {
	struct sway_server *server = wl_container_of(listener, server,
		xdg_shell_surface);
	struct wlr_xdg_surface *xdg_surface = data;

	if (xdg_surface->role == WLR_XDG_SURFACE_ROLE_POPUP) {
		wlr_log(WLR_DEBUG, "New xdg_shell popup");
		return;
	}

	wlr_log(WLR_DEBUG, "New xdg_shell toplevel title='%s' app_id='%s'",
		xdg_surface->toplevel->title, xdg_surface->toplevel->app_id);
	wlr_xdg_surface_ping(xdg_surface);

	struct sway_xdg_shell_view *xdg_shell_view =
		calloc(1, sizeof(struct sway_xdg_shell_view));
	if (!sway_assert(xdg_shell_view, "Failed to allocate view")) {
		return;
	}

	view_init(&xdg_shell_view->view, SWAY_VIEW_XDG_SHELL, &view_impl);
	xdg_shell_view->view.wlr_xdg_surface = xdg_surface;

	// TODO:
	// - Look up pid and open on appropriate workspace

	xdg_shell_view->map.notify = handle_map;
	wl_signal_add(&xdg_surface->events.map, &xdg_shell_view->map);

	xdg_shell_view->unmap.notify = handle_unmap;
	wl_signal_add(&xdg_surface->events.unmap, &xdg_shell_view->unmap);

	xdg_shell_view->destroy.notify = handle_destroy;
	wl_signal_add(&xdg_surface->events.destroy, &xdg_shell_view->destroy);

	xdg_surface->data = xdg_shell_view;
}
