/*
 * NIDE (NIDE Isn't a Desktop) - a small wlroots-based Wayland compositor.
 *
 * - Floating windows (not tiling), with no compositor-side decorations.
 * - Keybindings are configured in Lua, with hot reload.
 * - Super+W launches a terminal, Super+C closes the focused window.
 */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include <wlr/backend.h>
#include <wlr/backend/x11.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

#define NIDE_VERSION "0.2.0"
#define MAX_BINDS 64

#define NIDE_MOD_MASK \
	(WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

enum nide_action {
	NIDE_ACTION_SPAWN,
	NIDE_ACTION_CLOSE,
	NIDE_ACTION_QUIT,
};

enum nide_cursor_mode {
	NIDE_CURSOR_PASSTHROUGH,
	NIDE_CURSOR_MOVE,
	NIDE_CURSOR_RESIZE,
};

struct nide_bind {
	uint32_t mods;
	xkb_keysym_t sym;
	enum nide_action action;
	char command[512];
	char combo[64];
};

struct nide_config {
	struct nide_bind binds[MAX_BINDS];
	int nbinds;
};

struct nide_server;
struct nide_toplevel;
struct nide_decoration;

struct nide_server {
	struct wl_display *display;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;
	struct wl_list toplevels;
	struct nide_toplevel *focused;

	struct wl_listener new_decoration;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_shape;
	struct wl_listener pointer_focus_change;
	struct wl_listener request_set_selection;
	struct wl_listener request_set_primary_selection;
	struct wl_list keyboards;

	enum nide_cursor_mode cursor_mode;
	struct nide_toplevel *grabbed_toplevel;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	struct nide_config config;
	int cascade;

	char config_path[PATH_MAX];
	char config_name[NAME_MAX + 1];
	int config_watch_fd;
	int config_watch_wd;
	struct wl_event_source *config_watch;

	struct wl_event_source *sigint;
	struct wl_event_source *sigterm;
};

struct nide_output {
	struct wl_list link;
	struct nide_server *server;
	struct wlr_output *wlr_output;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
};

struct nide_toplevel {
	struct wl_list link;
	struct nide_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	bool mapped;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
	struct nide_decoration *decoration;
};

struct nide_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct nide_keyboard {
	struct wl_list link;
	struct nide_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

struct nide_decoration {
	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct nide_toplevel *toplevel;
	bool applied;
	struct wl_listener request_mode;
	struct wl_listener destroy;
};

static void decoration_apply(struct nide_toplevel *toplevel);

/* ------------------------------------------------------------------------- */
/* Lua configuration                                                         */
/* ------------------------------------------------------------------------- */

static bool parse_combo(const char *combo, uint32_t *mods_out,
		xkb_keysym_t *sym_out) {
	char buf[128];
	char *parts[8];
	int nparts = 0;

	if (combo == NULL || strlen(combo) >= sizeof(buf)) {
		return false;
	}
	snprintf(buf, sizeof(buf), "%s", combo);

	char *saveptr = NULL;
	for (char *tok = strtok_r(buf, "+", &saveptr); tok != NULL;
			tok = strtok_r(NULL, "+", &saveptr)) {
		if (nparts >= (int)(sizeof(parts) / sizeof(parts[0]))) {
			return false;
		}
		parts[nparts++] = tok;
	}
	if (nparts == 0) {
		return false;
	}

	uint32_t mods = 0;
	for (int i = 0; i < nparts - 1; i++) {
		if (strcasecmp(parts[i], "shift") == 0) {
			mods |= WLR_MODIFIER_SHIFT;
		} else if (strcasecmp(parts[i], "ctrl") == 0 ||
				strcasecmp(parts[i], "control") == 0) {
			mods |= WLR_MODIFIER_CTRL;
		} else if (strcasecmp(parts[i], "alt") == 0 ||
				strcasecmp(parts[i], "mod1") == 0) {
			mods |= WLR_MODIFIER_ALT;
		} else if (strcasecmp(parts[i], "super") == 0 ||
				strcasecmp(parts[i], "win") == 0 ||
				strcasecmp(parts[i], "logo") == 0 ||
				strcasecmp(parts[i], "mod4") == 0) {
			mods |= WLR_MODIFIER_LOGO;
		} else {
			return false;
		}
	}

	xkb_keysym_t sym = xkb_keysym_from_name(parts[nparts - 1],
		XKB_KEYSYM_CASE_INSENSITIVE);
	if (sym == XKB_KEY_NoSymbol) {
		return false;
	}

	*mods_out = mods;
	*sym_out = sym;
	return true;
}

static bool bind_set(struct nide_config *cfg, const char *combo,
		const char *action, const char *arg, char *err, size_t errsz) {
	uint32_t mods;
	xkb_keysym_t sym;

	if (!parse_combo(combo, &mods, &sym)) {
		snprintf(err, errsz, "invalid key combination: '%s'", combo);
		return false;
	}

	enum nide_action act;
	const char *command = "";
	if (strcmp(action, "spawn") == 0) {
		act = NIDE_ACTION_SPAWN;
		if (arg == NULL || *arg == '\0') {
			snprintf(err, errsz, "spawn action requires a command");
			return false;
		}
		command = arg;
	} else if (strcmp(action, "close") == 0) {
		act = NIDE_ACTION_CLOSE;
	} else if (strcmp(action, "quit") == 0) {
		act = NIDE_ACTION_QUIT;
	} else {
		snprintf(err, errsz,
			"unknown action: '%s' (use spawn/close/quit)", action);
		return false;
	}

	struct nide_bind *slot = NULL;
	for (int i = 0; i < cfg->nbinds; i++) {
		if (cfg->binds[i].mods == mods && cfg->binds[i].sym == sym) {
			slot = &cfg->binds[i];
			break;
		}
	}
	if (slot == NULL) {
		if (cfg->nbinds >= MAX_BINDS) {
			snprintf(err, errsz, "too many keybindings (maximum %d)",
				MAX_BINDS);
			return false;
		}
		slot = &cfg->binds[cfg->nbinds++];
	}

	memset(slot, 0, sizeof(*slot));
	slot->mods = mods;
	slot->sym = sym;
	slot->action = act;
	snprintf(slot->command, sizeof(slot->command), "%s", command);
	snprintf(slot->combo, sizeof(slot->combo), "%s", combo);
	return true;
}

static void config_defaults(struct nide_config *cfg) {
	char err[256];
	bind_set(cfg, "super+w", "spawn", "foot", err, sizeof(err));
	bind_set(cfg, "super+c", "close", NULL, err, sizeof(err));
	bind_set(cfg, "super+shift+q", "quit", NULL, err, sizeof(err));
}

static int l_nide_bind(lua_State *L) {
	struct nide_config *cfg = lua_touserdata(L, lua_upvalueindex(1));
	if (cfg == NULL) {
		return luaL_error(L, "nide.bind: invalid state");
	}

	const char *combo = luaL_checkstring(L, 1);
	const char *action = luaL_checkstring(L, 2);
	const char *arg = lua_isnoneornil(L, 3) ? NULL : luaL_checkstring(L, 3);

	char err[256];
	if (!bind_set(cfg, combo, action, arg, err, sizeof(err))) {
		return luaL_error(L, "%s", err);
	}
	return 0;
}

static int l_nide_log(lua_State *L) {
	int n = lua_gettop(L);
	luaL_Buffer buf;
	luaL_buffinit(L, &buf);

	for (int i = 1; i <= n; i++) {
		size_t len = 0;
		const char *str = luaL_tolstring(L, i, &len);
		if (i > 1) {
			luaL_addchar(&buf, ' ');
		}
		luaL_addlstring(&buf, str, len);
		lua_pop(L, 1);
	}

	luaL_pushresult(&buf);
	wlr_log(WLR_INFO, "config: %s", lua_tostring(L, -1));
	return 0;
}

static bool config_load(const char *path, struct nide_config *cfg,
		char *err, size_t errsz) {
	lua_State *L = luaL_newstate();
	if (L == NULL) {
		snprintf(err, errsz, "failed to create Lua state");
		return false;
	}
	luaL_openlibs(L);

	lua_newtable(L);

	lua_pushlightuserdata(L, cfg);
	lua_pushcclosure(L, l_nide_bind, 1);
	lua_setfield(L, -2, "bind");

	lua_pushcfunction(L, l_nide_log);
	lua_setfield(L, -2, "log");

	lua_pushstring(L, NIDE_VERSION);
	lua_setfield(L, -2, "version");

	lua_setglobal(L, "nide");

	if (luaL_loadfile(L, path) != LUA_OK ||
			lua_pcall(L, 0, 0, 0) != LUA_OK) {
		const char *msg = lua_tostring(L, -1);
		snprintf(err, errsz, "%s", msg != NULL ? msg : "Lua error");
		lua_close(L);
		return false;
	}

	lua_close(L);
	return true;
}

static bool config_find(const char *cli_path, char *path, size_t pathsz,
		bool *required) {
	*required = true;

	if (cli_path != NULL) {
		snprintf(path, pathsz, "%s", cli_path);
		return true;
	}

	const char *env = getenv("NIDE_CONFIG");
	if (env != NULL && *env != '\0') {
		snprintf(path, pathsz, "%s", env);
		return true;
	}

	*required = false;

	const char *xdg = getenv("XDG_CONFIG_HOME");
	if (xdg != NULL && *xdg != '\0') {
		snprintf(path, pathsz, "%s/nide/config.lua", xdg);
		if (access(path, R_OK) == 0) {
			return true;
		}
	}

	const char *home = getenv("HOME");
	if (home != NULL && *home != '\0') {
		snprintf(path, pathsz, "%s/.config/nide/config.lua", home);
		if (access(path, R_OK) == 0) {
			return true;
		}
	}

	snprintf(path, pathsz, "config.lua");
	if (access(path, R_OK) == 0) {
		return true;
	}

	path[0] = '\0';
	return false;
}

/* ------------------------------------------------------------------------- */
/* Hot reload config (inotify)                                               */
/* ------------------------------------------------------------------------- */

static void config_watch_close(struct nide_server *server) {
	if (server->config_watch != NULL) {
		wl_event_source_remove(server->config_watch);
		server->config_watch = NULL;
	}
	if (server->config_watch_fd >= 0) {
		if (server->config_watch_wd >= 0) {
			inotify_rm_watch(server->config_watch_fd,
				server->config_watch_wd);
			server->config_watch_wd = -1;
		}
		close(server->config_watch_fd);
		server->config_watch_fd = -1;
	}
}

static void config_reload(struct nide_server *server) {
	struct nide_config config = {0};
	char err[512];

	if (!config_load(server->config_path, &config, err, sizeof(err))) {
		wlr_log(WLR_ERROR,
			"failed to reload config %s: %s (keeping previous config)",
			server->config_path, err);
		return;
	}
	if (config.nbinds == 0) {
		wlr_log(WLR_ERROR,
			"config %s has no keybindings, ignoring change",
			server->config_path);
		return;
	}

	server->config = config;
	wlr_log(WLR_INFO, "config reloaded: %s (%d keybindings)",
		server->config_path, config.nbinds);
}

static int config_watch_event(int fd, uint32_t mask, void *data) {
	struct nide_server *server = data;
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	bool reload = false;
	bool deleted = false;
	bool ignored = false;

	(void)mask;

	for (;;) {
		ssize_t len = read(fd, buf, sizeof(buf));
		if (len < 0) {
			if (errno == EINTR) {
				continue;
			}
			if (errno != EAGAIN && errno != EWOULDBLOCK) {
				wlr_log(WLR_ERROR, "failed to read inotify: %s",
					strerror(errno));
			}
			break;
		}
		if (len == 0) {
			break;
		}

		for (char *ptr = buf; ptr < buf + len; ) {
			struct inotify_event *event = (struct inotify_event *)ptr;

			if (event->mask & IN_Q_OVERFLOW) {
				reload = true;
			} else if (event->len > 0 &&
					strcmp(event->name, server->config_name) == 0) {
				if (event->mask & (IN_CLOSE_WRITE | IN_MOVED_TO)) {
					reload = true;
				}
				if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
					deleted = true;
				}
			}
			if (event->mask & IN_IGNORED) {
				ignored = true;
			}

			ptr += sizeof(struct inotify_event) + event->len;
		}
	}

	if (reload) {
		config_reload(server);
	} else if (deleted) {
		wlr_log(WLR_INFO,
			"config %s deleted/moved; keeping previous config",
			server->config_path);
	} else if (ignored) {
		wlr_log(WLR_ERROR, "stopped watching config %s",
			server->config_path);
	}

	return 0;
}

static void config_watch_init(struct nide_server *server, const char *path) {
	char dir[PATH_MAX];
	const char *name = path;
	const char *slash = NULL;

	if (path == NULL || *path == '\0') {
		return;
	}

	snprintf(server->config_path, sizeof(server->config_path), "%s", path);

	slash = strrchr(path, '/');
	if (slash == NULL) {
		snprintf(dir, sizeof(dir), ".");
	} else if (slash == path) {
		snprintf(dir, sizeof(dir), "/");
		name = slash + 1;
	} else {
		size_t len = (size_t)(slash - path);
		if (len >= sizeof(dir)) {
			wlr_log(WLR_ERROR,
				"config path is too long, hot reload disabled");
			return;
		}
		memcpy(dir, path, len);
		dir[len] = '\0';
		name = slash + 1;
	}

	if (*name == '\0' || strlen(name) > NAME_MAX) {
		wlr_log(WLR_ERROR, "invalid config file name, hot reload disabled");
		return;
	}
	snprintf(server->config_name, sizeof(server->config_name), "%s", name);

	int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (fd < 0) {
		wlr_log(WLR_ERROR, "inotify failed, hot reload disabled: %s",
			strerror(errno));
		return;
	}

	int wd = inotify_add_watch(fd, dir,
		IN_CLOSE_WRITE | IN_MOVED_TO |
		IN_DELETE | IN_MOVED_FROM | IN_ONLYDIR);
	if (wd < 0) {
		wlr_log(WLR_ERROR, "failed to watch config directory %s: %s",
			dir, strerror(errno));
		close(fd);
		return;
	}

	struct wl_event_loop *loop = wl_display_get_event_loop(server->display);
	struct wl_event_source *source = wl_event_loop_add_fd(loop, fd,
		WL_EVENT_READABLE, config_watch_event, server);
	if (source == NULL) {
		wlr_log(WLR_ERROR, "failed to register hot reload with the event loop");
		inotify_rm_watch(fd, wd);
		close(fd);
		return;
	}

	server->config_watch_fd = fd;
	server->config_watch_wd = wd;
	server->config_watch = source;
	wlr_log(WLR_INFO, "hot reload active for %s", server->config_path);
}

/* ------------------------------------------------------------------------- */
/* Utilities                                                                 */
/* ------------------------------------------------------------------------- */

static void spawn_command(const char *cmd) {
	if (cmd == NULL || *cmd == '\0') {
		return;
	}

	wlr_log(WLR_INFO, "running: %s", cmd);
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

static void reset_cursor_mode(struct nide_server *server) {
	server->cursor_mode = NIDE_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static struct nide_toplevel *desktop_toplevel_at(struct nide_server *server,
		double lx, double ly, struct wlr_surface **surface,
		double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, lx, ly, sx, sy);
	if (node == NULL || node->type != WLR_SCENE_NODE_BUFFER) {
		return NULL;
	}

	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (scene_surface == NULL) {
		return NULL;
	}

	*surface = scene_surface->surface;

	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree != NULL ? tree->node.data : NULL;
}

static void focus_toplevel(struct nide_toplevel *toplevel) {
	if (toplevel == NULL) {
		return;
	}

	struct nide_server *server = toplevel->server;
	if (server->focused == toplevel) {
		return;
	}

	if (server->focused != NULL) {
		wlr_xdg_toplevel_set_activated(server->focused->xdg_toplevel, false);
	}

	server->focused = toplevel;
	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->toplevels, &toplevel->link);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(server->seat,
			toplevel->xdg_toplevel->base->surface,
			keyboard->keycodes, keyboard->num_keycodes,
			&keyboard->modifiers);
	}
}

static void clear_keyboard_focus(struct nide_server *server) {
	if (server->focused != NULL) {
		wlr_xdg_toplevel_set_activated(server->focused->xdg_toplevel, false);
		server->focused = NULL;
	}
	wlr_seat_keyboard_notify_clear_focus(server->seat);
}

static void focus_next_toplevel(struct nide_server *server) {
	if (wl_list_empty(&server->toplevels)) {
		clear_keyboard_focus(server);
		return;
	}

	struct nide_toplevel *next =
		wl_container_of(server->toplevels.next, next, link);
	focus_toplevel(next);
}

static void place_toplevel(struct nide_toplevel *toplevel) {
	struct nide_server *server = toplevel->server;
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);

	struct wlr_box box = {0};
	wlr_output_layout_get_box(server->output_layout, output, &box);
	if (box.width <= 0 || box.height <= 0) {
		box = (struct wlr_box){ .x = 0, .y = 0, .width = 1024, .height = 768 };
	}

	struct wlr_box *geo = &toplevel->xdg_toplevel->base->geometry;
	int width = geo->width > 0 ? geo->width : 800;
	int height = geo->height > 0 ? geo->height : 600;

	int offset = (server->cascade++ % 8) * 32;
	int x = box.x + (box.width - width) / 2 + offset;
	int y = box.y + (box.height - height) / 2 + offset;

	int min_x = box.x + 8;
	int min_y = box.y + 8;
	int max_x = box.x + box.width - width - 8;
	int max_y = box.y + box.height - height - 8;
	if (max_x < min_x) {
		max_x = min_x;
	}
	if (max_y < min_y) {
		max_y = min_y;
	}

	if (x < min_x) {
		x = min_x;
	} else if (x > max_x) {
		x = max_x;
	}
	if (y < min_y) {
		y = min_y;
	} else if (y > max_y) {
		y = max_y;
	}

	wlr_scene_node_set_position(&toplevel->scene_tree->node, x, y);
	wlr_log(WLR_DEBUG, "window placement: %dx%d+%d+%d (geometry %dx%d+%d+%d)",
		width, height, x, y, geo->width, geo->height, geo->x, geo->y);
}

static void begin_interactive(struct nide_toplevel *toplevel,
		enum nide_cursor_mode mode, uint32_t edges) {
	struct nide_server *server = toplevel->server;

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == NIDE_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;

		double border_x = (toplevel->scene_tree->node.x + geo_box->x) +
			((edges & WLR_EDGE_RIGHT) ? geo_box->width : 0);
		double border_y = (toplevel->scene_tree->node.y + geo_box->y) +
			((edges & WLR_EDGE_BOTTOM) ? geo_box->height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = *geo_box;
		server->grab_geobox.x += toplevel->scene_tree->node.x;
		server->grab_geobox.y += toplevel->scene_tree->node.y;
		server->resize_edges = edges;
	}
}

static void process_cursor_move(struct nide_server *server) {
	struct nide_toplevel *toplevel = server->grabbed_toplevel;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		server->cursor->x - server->grab_x,
		server->cursor->y - server->grab_y);
}

static void process_cursor_resize(struct nide_server *server) {
	struct nide_toplevel *toplevel = server->grabbed_toplevel;
	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;

	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) {
			new_top = new_bottom - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) {
			new_bottom = new_top + 1;
		}
	}
	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) {
			new_left = new_right - 1;
		}
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) {
			new_right = new_left + 1;
		}
	}

	struct wlr_box *geo_box = &toplevel->xdg_toplevel->base->geometry;
	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		new_left - geo_box->x, new_top - geo_box->y);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
		new_right - new_left, new_bottom - new_top);
}

static void process_cursor_motion(struct nide_server *server, uint32_t time) {
	if (server->cursor_mode == NIDE_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	}
	if (server->cursor_mode == NIDE_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct nide_toplevel *toplevel = desktop_toplevel_at(server,
		server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	if (toplevel == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}

	if (surface != NULL) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

/* ------------------------------------------------------------------------- */
/* Input handlers                                                            */
/* ------------------------------------------------------------------------- */

static void keyboard_handle_modifiers(struct wl_listener *listener, void *data) {
	struct nide_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	struct nide_server *server = keyboard->server;

	wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void run_bind(struct nide_server *server, struct nide_bind *bind) {
	switch (bind->action) {
	case NIDE_ACTION_SPAWN:
		spawn_command(bind->command);
		break;
	case NIDE_ACTION_CLOSE:
		if (server->focused != NULL) {
			wlr_log(WLR_INFO, "closing window: %s",
				server->focused->xdg_toplevel->title != NULL
					? server->focused->xdg_toplevel->title : "(untitled)");
			wlr_xdg_toplevel_send_close(server->focused->xdg_toplevel);
		}
		break;
	case NIDE_ACTION_QUIT:
		wl_display_terminate(server->display);
		break;
	}
}

static bool handle_keybinding(struct nide_server *server, xkb_keysym_t sym,
		uint32_t modifiers) {
	uint32_t mods = modifiers & NIDE_MOD_MASK;

	for (int i = 0; i < server->config.nbinds; i++) {
		struct nide_bind *bind = &server->config.binds[i];
		if (bind->mods != mods) {
			continue;
		}
		if (xkb_keysym_to_lower(bind->sym) != xkb_keysym_to_lower(sym)) {
			continue;
		}
		run_bind(server, bind);
		return true;
	}
	return false;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct nide_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct nide_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms = NULL;
	int nsyms = xkb_state_key_get_syms(keyboard->wlr_keyboard->xkb_state,
		keycode, &syms);


	bool handled = false;
	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
		for (int i = 0; i < nsyms; i++) {
			if (handle_keybinding(server, syms[i], modifiers)) {
				handled = true;
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat, event->time_msec,
			event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct nide_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);

	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void server_new_keyboard(struct nide_server *server,
		struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct nide_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	if (keyboard == NULL) {
		return;
	}
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	if (context != NULL) {
		struct xkb_keymap *keymap = xkb_keymap_new_from_names(context, NULL,
			XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (keymap != NULL) {
			wlr_keyboard_set_keymap(wlr_keyboard, keymap);
			xkb_keymap_unref(keymap);
		}
		xkb_context_unref(context);
	}
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&device->events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);
}

static void server_new_pointer(struct nide_server *server,
		struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;

	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_shape(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, request_set_shape);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;

	if (event->device_type !=
			WLR_CURSOR_SHAPE_MANAGER_V1_DEVICE_TYPE_POINTER) {
		return;
	}
	if (server->seat->pointer_state.focused_client != event->seat_client) {
		return;
	}

	const char *name = wlr_cursor_shape_v1_name(event->shape);
	if (name != NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, name);
	}
}

static void seat_pointer_focus_change(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, pointer_focus_change);
	struct wlr_seat_pointer_focus_change_event *event = data;

	if (event->new_surface == NULL) {
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr, "default");
	}
}

static void seat_request_set_selection(struct wl_listener *listener,
		void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void seat_request_set_primary_selection(struct wl_listener *listener,
		void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *event = data;
	wlr_seat_set_primary_selection(server->seat, event->source, event->serial);
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;

	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(struct wl_listener *listener,
		void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;

	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static bool super_is_held(struct nide_server *server) {
	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
	return keyboard != NULL &&
		(wlr_keyboard_get_modifiers(keyboard) & WLR_MODIFIER_LOGO);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server->cursor_mode != NIDE_CURSOR_PASSTHROUGH) {
			reset_cursor_mode(server);
			return;
		}
		wlr_seat_pointer_notify_button(server->seat, event->time_msec,
			event->button, event->state);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct nide_toplevel *toplevel = desktop_toplevel_at(server,
		server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	if (super_is_held(server) && toplevel != NULL) {
		focus_toplevel(toplevel);
		if (event->button == BTN_LEFT) {
			begin_interactive(toplevel, NIDE_CURSOR_MOVE, 0);
			return;
		}
		if (event->button == BTN_RIGHT) {
			begin_interactive(toplevel, NIDE_CURSOR_RESIZE,
				WLR_EDGE_BOTTOM | WLR_EDGE_RIGHT);
			return;
		}
	}

	wlr_seat_pointer_notify_button(server->seat, event->time_msec,
		event->button, event->state);
	if (toplevel != NULL) {
		focus_toplevel(toplevel);
	} else {
		clear_keyboard_focus(server);
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;

	wlr_seat_pointer_notify_axis(server->seat, event->time_msec,
		event->orientation, event->delta, event->delta_discrete,
		event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, cursor_frame);
	wlr_seat_pointer_notify_frame(server->seat);
}

/* ------------------------------------------------------------------------- */
/* Output                                                                    */
/* ------------------------------------------------------------------------- */

static void output_frame(struct wl_listener *listener, void *data) {
	struct nide_output *output = wl_container_of(listener, output, frame);
	struct nide_server *server = output->server;
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(server->scene, output->wlr_output);

	if (scene_output != NULL) {
		wlr_scene_output_commit(scene_output, NULL);
	}

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	if (scene_output != NULL) {
		wlr_scene_output_send_frame_done(scene_output, &now);
	}
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct nide_output *output =
		wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct nide_output *output = wl_container_of(listener, output, destroy);

	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	}
	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	struct nide_output *output = calloc(1, sizeof(*output));
	if (output == NULL) {
		return;
	}
	output->wlr_output = wlr_output;
	output->server = server;

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *layout_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout,
		layout_output, scene_output);

	if (wlr_output_is_x11(wlr_output)) {
		wlr_x11_output_set_title(wlr_output, "NIDE");
	}
}

/* ------------------------------------------------------------------------- */
/* xdg-shell                                                                 */
/* ------------------------------------------------------------------------- */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct nide_toplevel *toplevel = wl_container_of(listener, toplevel, map);
	struct nide_server *server = toplevel->server;

	toplevel->mapped = true;
	wl_list_insert(&server->toplevels, &toplevel->link);
	place_toplevel(toplevel);
	focus_toplevel(toplevel);
	decoration_apply(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct nide_toplevel *toplevel = wl_container_of(listener, toplevel, unmap);
	struct nide_server *server = toplevel->server;

	if (!toplevel->mapped) {
		return;
	}
	toplevel->mapped = false;

	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}

	wl_list_remove(&toplevel->link);

	if (server->focused == toplevel) {
		server->focused = NULL;
		focus_next_toplevel(server);
	}
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct nide_toplevel *toplevel =
		wl_container_of(listener, toplevel, commit);

	if (toplevel->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
	}
}

static void xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct nide_toplevel *toplevel =
		wl_container_of(listener, toplevel, destroy);
	struct nide_server *server = toplevel->server;

	if (toplevel->mapped) {
		toplevel->mapped = false;
		wl_list_remove(&toplevel->link);
	}
	if (server->focused == toplevel) {
		server->focused = NULL;
		focus_next_toplevel(server);
	}
	if (server->grabbed_toplevel == toplevel) {
		reset_cursor_mode(server);
	}
	if (toplevel->decoration != NULL) {
		toplevel->decoration->toplevel = NULL;
		toplevel->decoration = NULL;
	}

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);

	free(toplevel);
}

static void xdg_toplevel_request_move(struct wl_listener *listener,
		void *data) {
	struct nide_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_move);
	begin_interactive(toplevel, NIDE_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(struct wl_listener *listener,
		void *data) {
	struct wlr_xdg_toplevel_resize_event *event = data;
	struct nide_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_resize);
	begin_interactive(toplevel, NIDE_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(struct wl_listener *listener,
		void *data) {
	struct nide_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener,
		void *data) {
	struct nide_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	if (toplevel->xdg_toplevel->base->initialized) {
		wlr_xdg_surface_schedule_configure(toplevel->xdg_toplevel->base);
	}
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct nide_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (toplevel == NULL) {
		return;
	}
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->scene_tree = wlr_scene_xdg_surface_create(
		&server->scene->tree, xdg_toplevel->base);
	toplevel->scene_tree->node.data = toplevel;
	xdg_toplevel->base->data = toplevel->scene_tree;

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize,
		&toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize,
		&toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen,
		&toplevel->request_fullscreen);
}

static void xdg_popup_commit(struct wl_listener *listener, void *data) {
	struct nide_popup *popup = wl_container_of(listener, popup, commit);
	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void xdg_popup_destroy(struct wl_listener *listener, void *data) {
	struct nide_popup *popup = wl_container_of(listener, popup, destroy);
	wl_list_remove(&popup->commit.link);
	wl_list_remove(&popup->destroy.link);
	free(popup);
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct wlr_xdg_popup *xdg_popup = data;

	struct nide_popup *popup = calloc(1, sizeof(*popup));
	if (popup == NULL) {
		return;
	}
	popup->xdg_popup = xdg_popup;

	struct wlr_xdg_surface *parent =
		wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
	if (parent == NULL || parent->data == NULL) {
		free(popup);
		return;
	}
	struct wlr_scene_tree *parent_tree = parent->data;
	xdg_popup->base->data =
		wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);

	popup->commit.notify = xdg_popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = xdg_popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* ------------------------------------------------------------------------- */
/* Decorations: the compositor draws no title bar or border at all.          */
/* ------------------------------------------------------------------------- */

static void decoration_apply(struct nide_toplevel *toplevel) {
	if (toplevel == NULL || toplevel->decoration == NULL) {
		return;
	}

	struct nide_decoration *deco = toplevel->decoration;
	if (deco->applied ||
			!deco->decoration->toplevel->base->initialized) {
		return;
	}

	/*
	 * Request server-side mode, but do not draw any decorations.
	 * This prevents clients from adding their own CSD, so windows stay
	 * completely plain with no title bar or buttons. Close windows with
	 * Super+C, move them with Super+Left-click.
	 */
	wlr_xdg_toplevel_decoration_v1_set_mode(deco->decoration,
		WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	deco->applied = true;
}

static void decoration_request_mode(struct wl_listener *listener, void *data) {
	struct nide_decoration *deco =
		wl_container_of(listener, deco, request_mode);
	deco->applied = false;
	decoration_apply(deco->toplevel);
}

static void decoration_destroy(struct wl_listener *listener, void *data) {
	struct nide_decoration *deco =
		wl_container_of(listener, deco, destroy);
	if (deco->toplevel != NULL && deco->toplevel->decoration == deco) {
		deco->toplevel->decoration = NULL;
	}
	wl_list_remove(&deco->request_mode.link);
	wl_list_remove(&deco->destroy.link);
	free(deco);
}

static void server_new_decoration(struct wl_listener *listener, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *decoration = data;

	struct nide_decoration *deco = calloc(1, sizeof(*deco));
	if (deco == NULL) {
		return;
	}
	deco->decoration = decoration;

	struct wlr_scene_tree *scene_tree = decoration->toplevel->base->data;
	if (scene_tree != NULL) {
		deco->toplevel = scene_tree->node.data;
	}
	if (deco->toplevel != NULL) {
		deco->toplevel->decoration = deco;
	}

	deco->request_mode.notify = decoration_request_mode;
	wl_signal_add(&decoration->events.request_mode, &deco->request_mode);
	deco->destroy.notify = decoration_destroy;
	wl_signal_add(&decoration->events.destroy, &deco->destroy);

	decoration_apply(deco->toplevel);
}

/* ------------------------------------------------------------------------- */
/* Startup / shutdown                                                        */
/* ------------------------------------------------------------------------- */

static int handle_signal(int signo, void *data) {
	struct nide_server *server = data;
	wl_display_terminate(server->display);
	return 0;
}

static void print_bindings(const struct nide_config *cfg) {
	printf("Keybindings (%d):\n", cfg->nbinds);
	for (int i = 0; i < cfg->nbinds; i++) {
		const struct nide_bind *bind = &cfg->binds[i];
		const char *action = "?";
		switch (bind->action) {
		case NIDE_ACTION_SPAWN: action = "spawn"; break;
		case NIDE_ACTION_CLOSE: action = "close"; break;
		case NIDE_ACTION_QUIT: action = "quit"; break;
		}
		if (bind->action == NIDE_ACTION_SPAWN) {
			printf("  %-20s -> %s %s\n", bind->combo, action, bind->command);
		} else {
			printf("  %-20s -> %s\n", bind->combo, action);
		}
	}
}

static void usage(FILE *stream, const char *prog) {
	fprintf(stream,
		"Usage: %s [options]\n"
		"  -c, --config PATH  Lua configuration file\n"
		"  -C, --check        validate configuration and exit\n"
		"  -d, --debug        wlroots debug logging\n"
		"  -v, --version      print version\n"
		"  -h, --help         print help\n",
		prog);
}

int main(int argc, char *argv[]) {
	const char *cli_config = NULL;
	bool check_only = false;
	bool debug = false;

	static const struct option long_options[] = {
		{ "config", required_argument, NULL, 'c' },
		{ "check", no_argument, NULL, 'C' },
		{ "debug", no_argument, NULL, 'd' },
		{ "version", no_argument, NULL, 'v' },
		{ "help", no_argument, NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};

	int opt;
	while ((opt = getopt_long(argc, argv, "c:Cdvh", long_options, NULL)) != -1) {
		switch (opt) {
		case 'c':
			cli_config = optarg;
			break;
		case 'C':
			check_only = true;
			break;
		case 'd':
			debug = true;
			break;
		case 'v':
			printf("nide %s\n", NIDE_VERSION);
			return 0;
		case 'h':
			usage(stdout, argv[0]);
			return 0;
		default:
			usage(stderr, argv[0]);
			return 1;
		}
	}

	if (optind < argc) {
		usage(stderr, argv[0]);
		return 1;
	}

	wlr_log_init(debug ? WLR_DEBUG : WLR_INFO, NULL);

	char config_path[PATH_MAX] = {0};
	bool config_required = false;
	bool have_config = config_find(cli_config, config_path,
		sizeof(config_path), &config_required);

	struct nide_config config = {0};
	if (have_config) {
		char err[512];
		if (!config_load(config_path, &config, err, sizeof(err))) {
			fprintf(stderr, "nide: failed to load config %s: %s\n",
				config_path, err);
			return 1;
		}
	} else {
		if (config_required) {
			fprintf(stderr, "nide: config not found: %s\n",
				config_path);
			return 1;
		}
		config_defaults(&config);
		wlr_log(WLR_INFO, "config not found, using default keybindings");
	}

	if (config.nbinds == 0) {
		wlr_log(WLR_ERROR, "config has no keybindings");
	}

	if (check_only) {
		printf("Config: %s\n", have_config ? config_path : "(default)");
		print_bindings(&config);
		return 0;
	}

	struct nide_server server = {0};
	server.config = config;
	server.cursor_mode = NIDE_CURSOR_PASSTHROUGH;
	server.config_watch_fd = -1;
	server.config_watch_wd = -1;

	wl_list_init(&server.outputs);
	wl_list_init(&server.toplevels);
	wl_list_init(&server.keyboards);

	signal(SIGCHLD, SIG_IGN);

	server.display = wl_display_create();
	if (server.display == NULL) {
		wlr_log(WLR_ERROR, "failed to create Wayland display");
		return 1;
	}

	server.backend = wlr_backend_autocreate(
		wl_display_get_event_loop(server.display), NULL);
	if (server.backend == NULL) {
		wlr_log(WLR_ERROR, "failed to create backend");
		wl_display_destroy(server.display);
		return 1;
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (server.renderer == NULL) {
		wlr_log(WLR_ERROR, "failed to create renderer");
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.display);
		return 1;
	}
	wlr_renderer_init_wl_display(server.renderer, server.display);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (server.allocator == NULL) {
		wlr_log(WLR_ERROR, "failed to create allocator");
		wlr_renderer_destroy(server.renderer);
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.display);
		return 1;
	}

	wlr_compositor_create(server.display, 5, server.renderer);
	wlr_subcompositor_create(server.display);
	wlr_data_device_manager_create(server.display);
	wlr_primary_selection_v1_device_manager_create(server.display);

	server.output_layout = wlr_output_layout_create(server.display);
	wlr_xdg_output_manager_v1_create(server.display, server.output_layout);

	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
		server.output_layout);

	server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel,
		&server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	struct wlr_xdg_decoration_manager_v1 *deco_manager =
		wlr_xdg_decoration_manager_v1_create(server.display);
	server.new_decoration.notify = server_new_decoration;
	wl_signal_add(&deco_manager->events.new_toplevel_decoration,
		&server.new_decoration);

	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	struct wlr_cursor_shape_manager_v1 *cursor_shape_mgr =
		wlr_cursor_shape_manager_v1_create(server.display, 1);
	server.request_set_shape.notify = seat_request_set_shape;
	wl_signal_add(&cursor_shape_mgr->events.request_set_shape,
		&server.request_set_shape);

	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute,
		&server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	server.seat = wlr_seat_create(server.display, "seat0");
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor,
		&server.request_cursor);
	server.pointer_focus_change.notify = seat_pointer_focus_change;
	wl_signal_add(&server.seat->pointer_state.events.focus_change,
		&server.pointer_focus_change);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection,
		&server.request_set_selection);
	server.request_set_primary_selection.notify =
		seat_request_set_primary_selection;
	wl_signal_add(&server.seat->events.request_set_primary_selection,
		&server.request_set_primary_selection);

	const char *socket = wl_display_add_socket_auto(server.display);
	if (socket == NULL) {
		wlr_log(WLR_ERROR, "failed to create Wayland socket");
		wlr_xcursor_manager_destroy(server.cursor_mgr);
		wlr_cursor_destroy(server.cursor);
		wlr_scene_node_destroy(&server.scene->tree.node);
		wlr_allocator_destroy(server.allocator);
		wlr_renderer_destroy(server.renderer);
		wlr_backend_destroy(server.backend);
		wl_display_destroy(server.display);
		return 1;
	}

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		wl_display_destroy(server.display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "nide", true);

	struct wl_event_loop *loop = wl_display_get_event_loop(server.display);
	server.sigint = wl_event_loop_add_signal(loop, SIGINT, handle_signal, &server);
	server.sigterm = wl_event_loop_add_signal(loop, SIGTERM, handle_signal, &server);

	config_watch_init(&server, config_path);

	wlr_log(WLR_INFO, "NIDE running on WAYLAND_DISPLAY=%s", socket);
	wlr_log(WLR_INFO, "%d keybindings active", server.config.nbinds);

	wl_display_run(server.display);

	wl_display_destroy_clients(server.display);

	config_watch_close(&server);

	wl_event_source_remove(server.sigint);
	wl_event_source_remove(server.sigterm);

	wl_list_remove(&server.new_xdg_toplevel.link);
	wl_list_remove(&server.new_xdg_popup.link);
	wl_list_remove(&server.new_decoration.link);
	wl_list_remove(&server.new_output.link);

	wl_list_remove(&server.cursor_motion.link);
	wl_list_remove(&server.cursor_motion_absolute.link);
	wl_list_remove(&server.cursor_button.link);
	wl_list_remove(&server.cursor_axis.link);
	wl_list_remove(&server.cursor_frame.link);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.request_cursor.link);
	wl_list_remove(&server.request_set_shape.link);
	wl_list_remove(&server.pointer_focus_change.link);
	wl_list_remove(&server.request_set_selection.link);
	wl_list_remove(&server.request_set_primary_selection.link);

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);

	return 0;
}
