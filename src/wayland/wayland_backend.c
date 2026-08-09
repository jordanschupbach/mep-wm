#include <stdio.h>
#include <stdlib.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

struct mepwm_wayland_server {
  struct wl_display *display;
  struct wlr_backend *backend;
  struct wlr_renderer *renderer;
  struct wlr_allocator *allocator;
  struct wlr_output_layout *output_layout;
  struct wlr_scene *scene;
  struct wlr_xdg_shell *xdg_shell;
  struct wl_listener new_output;
  struct wl_listener new_xdg_surface;
};

static void handle_new_output(struct wl_listener *listener, void *data) {
  struct mepwm_wayland_server *server =
      wl_container_of(listener, server, new_output);
  struct wlr_output *output = data;
  if (!wlr_output_init_render(output, server->allocator, server->renderer)) return;

  struct wlr_output_state state;
  wlr_output_state_init(&state);
  wlr_output_state_set_enabled(&state, true);
  wlr_output_commit_state(output, &state);
  wlr_output_state_finish(&state);
  wlr_output_layout_add_auto(server->output_layout, output);
}

static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
  struct mepwm_wayland_server *server =
      wl_container_of(listener, server, new_xdg_surface);
  struct wlr_xdg_surface *surface = data;
  if (surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) return;
  wlr_scene_xdg_surface_create(&server->scene->tree, surface);
}

int mepwm_wayland_run(void) {
  struct mepwm_wayland_server server = {0};
  wlr_log_init(WLR_INFO, NULL);
  server.display = wl_display_create();
  if (!server.display) return 1;

  server.backend = wlr_backend_autocreate(wl_display_get_event_loop(server.display), NULL);
  server.renderer = server.backend ? wlr_renderer_autocreate(server.backend) : NULL;
  server.allocator = server.renderer ? wlr_allocator_autocreate(server.backend, server.renderer) : NULL;
  if (!server.allocator) {
    wl_display_destroy(server.display);
    return 1;
  }

  wlr_renderer_init_wl_display(server.renderer, server.display);
  wlr_compositor_create(server.display, 6, server.renderer);
  wlr_data_device_manager_create(server.display);
  server.output_layout = wlr_output_layout_create(server.display);
  server.scene = wlr_scene_create();
  wlr_scene_attach_output_layout(server.scene, server.output_layout);
  server.xdg_shell = wlr_xdg_shell_create(server.display, 6);
  if (!server.output_layout || !server.scene || !server.xdg_shell) {
    wl_display_destroy(server.display);
    return 1;
  }

  server.new_output.notify = handle_new_output;
  wl_signal_add(&server.backend->events.new_output, &server.new_output);
  server.new_xdg_surface.notify = handle_new_xdg_surface;
  wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

  const char *socket = wl_display_add_socket_auto(server.display);
  if (!socket || !wlr_backend_start(server.backend)) {
    wl_display_destroy(server.display);
    return 1;
  }
  setenv("WAYLAND_DISPLAY", socket, 1);
  fprintf(stderr, "mepwm: running Wayland display %s\n", socket);
  wl_display_run(server.display);
  wl_display_destroy(server.display);
  return 0;
}
