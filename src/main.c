/*
 * NIDE (NIDE Isn't a Desktop) - a small wlroots-based Wayland compositor.
 *
 * - Floating windows (not tiling), with no compositor-side decorations.
 * - Keybindings are configured in Lua, with hot reload.
 * - Super+W launches a terminal, Super+C closes the focused window.
 * - Super+M maximizes (with a configurable margin), Super+S toggles fullscreen.
 */

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <math.h>
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

#define NIDE_VERSION "0.3.2"
#define MAX_BINDS 64
#define NIDE_MAXIMIZE_MARGIN_DEFAULT 8
#define NIDE_SNAP_THRESHOLD 24
#define NIDE_SNAP_RESISTANCE 64
#define NIDE_PREVIEW_BORDER 3

#define NIDE_MOD_MASK \
	(WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

enum nide_action {
	NIDE_ACTION_SPAWN,
	NIDE_ACTION_CLOSE,
	NIDE_ACTION_MAXIMIZE,
	NIDE_ACTION_FULLSCREEN,
	NIDE_ACTION_QUIT,
};

enum nide_cursor_mode {
	NIDE_CURSOR_PASSTHROUGH,
	NIDE_CURSOR_MOVE,
	NIDE_CURSOR_RESIZE,
};

enum nide_toplevel_state {
	NIDE_TOPLEVEL_FLOATING,
	NIDE_TOPLEVEL_SNAPPED_LEFT,
	NIDE_TOPLEVEL_SNAPPED_RIGHT,
	NIDE_TOPLEVEL_MAXIMIZED,
	NIDE_TOPLEVEL_FULLSCREEN,
};

enum nide_snap_target {
	NIDE_SNAP_TARGET_NONE,
	NIDE_SNAP_TARGET_LEFT,
	NIDE_SNAP_TARGET_RIGHT,
	NIDE_SNAP_TARGET_FULLSCREEN,
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
	int maximize_margin;
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
	struct wl_listener output_layout_change;

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
	enum nide_snap_target drag_target;
	enum nide_snap_target drag_suppressed;
	double move_start_x, move_start_y;
	bool move_started;

	struct wlr_scene_tree *overlay_tree;
	struct wlr_scene_tree *preview_tree;
	struct wlr_scene_tree *snap_gauge_tree;
	struct wlr_scene_rect *preview_fill;
	struct wlr_scene_rect *preview_border[4];
	struct wlr_scene_rect *snap_gauge_backdrop;
	struct wlr_scene_rect *snap_gauge_track;
	struct wlr_scene_rect *snap_gauge_fill;

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
	bool placed;
	bool pending_maximize;
	bool pending_fullscreen;
	enum nide_toplevel_state state;
	enum nide_toplevel_state restore_state;
	struct wlr_box floating_geometry;
	bool has_floating_geometry;
	bool restore_floating_pending;
	double snap_anchor_x;
	double snap_anchor_y;
	double snap_grab_x;
	double snap_grab_y;
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
static void toplevel_capture_floating(struct nide_toplevel *toplevel);
static void toplevel_reapply_state(struct nide_toplevel *toplevel);
static void drag_overlay_hide(struct nide_server *server);

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
	} else if (strcmp(action, "maximize") == 0) {
		act = NIDE_ACTION_MAXIMIZE;
	} else if (strcmp(action, "fullscreen") == 0) {
		act = NIDE_ACTION_FULLSCREEN;
	} else if (strcmp(action, "quit") == 0) {
		act = NIDE_ACTION_QUIT;
	} else {
		snprintf(err, errsz,
			"unknown action: '%s' "
			"(use spawn/close/maximize/fullscreen/quit)", action);
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
	cfg->maximize_margin = NIDE_MAXIMIZE_MARGIN_DEFAULT;
	bind_set(cfg, "super+w", "spawn", "foot", err, sizeof(err));
	bind_set(cfg, "super+c", "close", NULL, err, sizeof(err));
	bind_set(cfg, "super+m", "maximize", NULL, err, sizeof(err));
	bind_set(cfg, "super+s", "fullscreen", NULL, err, sizeof(err));
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
	cfg->maximize_margin = NIDE_MAXIMIZE_MARGIN_DEFAULT;

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

	lua_pushinteger(L, cfg->maximize_margin);
	lua_setfield(L, -2, "maximize_margin");

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

	lua_getglobal(L, "nide");
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, "maximize_margin");
		if (!lua_isnil(L, -1)) {
			if (!lua_isnumber(L, -1)) {
				snprintf(err, errsz,
					"nide.maximize_margin must be a number");
				lua_close(L);
				return false;
			}
			int margin = (int)lua_tointeger(L, -1);
			if (margin < 0 || margin > 512) {
				snprintf(err, errsz,
					"nide.maximize_margin must be between 0 and 512");
				lua_close(L);
				return false;
			}
			cfg->maximize_margin = margin;
		}
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

	int previous_margin = server->config.maximize_margin;
	server->config = config;

	if (config.maximize_margin != previous_margin) {
		struct nide_toplevel *toplevel;
		wl_list_for_each(toplevel, &server->toplevels, link) {
			if (toplevel->state == NIDE_TOPLEVEL_MAXIMIZED ||
					toplevel->state ==
						NIDE_TOPLEVEL_SNAPPED_LEFT ||
					toplevel->state ==
						NIDE_TOPLEVEL_SNAPPED_RIGHT) {
				toplevel_reapply_state(toplevel);
			}
		}
	}

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
	drag_overlay_hide(server);
	server->cursor_mode = NIDE_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
	server->move_started = false;
	server->drag_suppressed = NIDE_SNAP_TARGET_NONE;
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

static struct wlr_box toplevel_geometry(struct nide_toplevel *toplevel) {
	struct wlr_box geo = toplevel->xdg_toplevel->base->geometry;
	int width = geo.width > 0 ? geo.width : 800;
	int height = geo.height > 0 ? geo.height : 600;

	return (struct wlr_box) {
		.x = (int)toplevel->scene_tree->node.x + geo.x,
		.y = (int)toplevel->scene_tree->node.y + geo.y,
		.width = width,
		.height = height,
	};
}

static struct wlr_box toplevel_output_box(struct nide_toplevel *toplevel,
		struct wlr_output *preferred) {
	struct nide_server *server = toplevel->server;
	struct wlr_box box = {0};

	if (preferred != NULL) {
		wlr_output_layout_get_box(server->output_layout, preferred, &box);
		if (box.width > 0 && box.height > 0) {
			return box;
		}
	}

	struct wlr_box geo = toplevel_geometry(toplevel);
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, geo.x + geo.width / 2.0,
		geo.y + geo.height / 2.0);
	if (output == NULL) {
		output = wlr_output_layout_output_at(server->output_layout,
			server->cursor->x, server->cursor->y);
	}
	if (output != NULL) {
		wlr_output_layout_get_box(server->output_layout, output, &box);
	}
	if (box.width <= 0 || box.height <= 0) {
		box = (struct wlr_box) {
			.x = 0, .y = 0, .width = 1024, .height = 768,
		};
	}
	return box;
}

static struct wlr_box inset_box(struct wlr_box box, int margin) {
	int x_margin = margin * 2 < box.width ? margin : 0;
	int y_margin = margin * 2 < box.height ? margin : 0;

	box.x += x_margin;
	box.y += y_margin;
	box.width -= x_margin * 2;
	box.height -= y_margin * 2;
	return box;
}

static void toplevel_apply_box(struct nide_toplevel *toplevel,
		struct wlr_box box, bool resize) {
	struct wlr_box *geo = &toplevel->xdg_toplevel->base->geometry;

	if (box.width < 1) {
		box.width = 1;
	}
	if (box.height < 1) {
		box.height = 1;
	}

	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		box.x - geo->x, box.y - geo->y);
	if (resize) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel,
			box.width, box.height);
	}
}

static void toplevel_capture_floating(struct nide_toplevel *toplevel) {
	struct wlr_box box = toplevel_geometry(toplevel);

	/*
	 * A state change may still be waiting for the client to acknowledge the
	 * configure. Keep the saved size in that case: the current surface
	 * geometry still belongs to the previous state.
	 */
	if (toplevel->restore_floating_pending &&
			toplevel->has_floating_geometry) {
		box.width = toplevel->floating_geometry.width;
		box.height = toplevel->floating_geometry.height;
	}

	/*
	 * Keep the saved geometry reachable. Fullscreen and snap gestures can
	 * end with the window partly outside the layout, but restoring that
	 * position later would put the window off-screen.
	 */
	struct wlr_box layout = {0};
	wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
		&layout);
	if (layout.width > 0 && layout.height > 0) {
		int margin = 8;
		int min_x = layout.x + margin;
		int min_y = layout.y + margin;
		int max_x = layout.x + layout.width - box.width - margin;
		int max_y = layout.y + layout.height - box.height - margin;

		if (max_x < min_x) {
			max_x = min_x;
		}
		if (max_y < min_y) {
			max_y = min_y;
		}
		if (box.x < min_x) {
			box.x = min_x;
		} else if (box.x > max_x) {
			box.x = max_x;
		}
		if (box.y < min_y) {
			box.y = min_y;
		} else if (box.y > max_y) {
			box.y = max_y;
		}
	}

	toplevel->floating_geometry = box;
	toplevel->has_floating_geometry = true;
}

static struct wlr_box snap_box_for_output(struct wlr_box box, int margin,
		bool left) {
	int x_margin = margin * 2 < box.width ? margin : 0;
	int y_margin = margin * 2 < box.height ? margin : 0;
	int half = box.width / 2;
	int x;
	int width;

	if (left) {
		x = box.x + x_margin;
		width = half - x_margin;
	} else {
		x = box.x + half;
		width = box.width - half - x_margin;
	}

	if (width < 1) {
		width = 1;
	}

	return (struct wlr_box) {
		.x = x,
		.y = box.y + y_margin,
		.width = width,
		.height = box.height - y_margin * 2,
	};
}

static struct wlr_box toplevel_snap_box(struct nide_toplevel *toplevel) {
	struct wlr_box box = toplevel_output_box(toplevel, NULL);

	return snap_box_for_output(box,
		toplevel->server->config.maximize_margin,
		toplevel->state == NIDE_TOPLEVEL_SNAPPED_LEFT);
}

/*
 * Overlay nodes for the snap preview and the snap pressure gauge. The
 * overlay tree is raised above the windows only while it is visible.
 */
static const float preview_fill_color[4] = {
	0.066f, 0.132f, 0.22f, 0.22f,
};
static const float preview_border_color[4] = {
	0.399f, 0.684f, 0.95f, 0.95f,
};
static const float snap_gauge_backdrop_color[4] = {
	0.0f, 0.0f, 0.0f, 0.45f,
};
static const float snap_gauge_track_color[4] = {
	0.176f, 0.187f, 0.209f, 0.22f,
};
static const float snap_gauge_fill_color[4] = {
	0.399f, 0.684f, 0.95f, 0.95f,
};

static void drag_overlay_init(struct nide_server *server) {
	server->overlay_tree = wlr_scene_tree_create(&server->scene->tree);
	server->preview_tree = wlr_scene_tree_create(server->overlay_tree);
	server->snap_gauge_tree = wlr_scene_tree_create(server->overlay_tree);

	server->preview_fill = wlr_scene_rect_create(server->preview_tree,
		1, 1, preview_fill_color);
	for (size_t i = 0; i < 4; i++) {
		server->preview_border[i] = wlr_scene_rect_create(
			server->preview_tree, 1, 1, preview_border_color);
	}
	server->snap_gauge_backdrop = wlr_scene_rect_create(
		server->snap_gauge_tree, 1, 1, snap_gauge_backdrop_color);
	server->snap_gauge_track = wlr_scene_rect_create(
		server->snap_gauge_tree, 1, 1, snap_gauge_track_color);
	server->snap_gauge_fill = wlr_scene_rect_create(
		server->snap_gauge_tree, 1, 1, snap_gauge_fill_color);

	wlr_scene_node_set_enabled(&server->preview_tree->node, false);
	wlr_scene_node_set_enabled(&server->snap_gauge_tree->node, false);
}

static void drag_overlay_raise(struct nide_server *server) {
	wlr_scene_node_raise_to_top(&server->overlay_tree->node);
}

static void drag_preview_hide(struct nide_server *server) {
	if (server->drag_target != NIDE_SNAP_TARGET_NONE) {
		wlr_log(WLR_DEBUG, "snap preview cleared");
	}
	server->drag_target = NIDE_SNAP_TARGET_NONE;
	wlr_scene_node_set_enabled(&server->preview_tree->node, false);
}

static void drag_gauge_hide(struct nide_server *server) {
	wlr_scene_node_set_enabled(&server->snap_gauge_tree->node, false);
}

static void drag_overlay_hide(struct nide_server *server) {
	drag_preview_hide(server);
	drag_gauge_hide(server);
}

static void drag_preview_show(struct nide_server *server,
		enum nide_snap_target target, struct wlr_box box) {
	int border = NIDE_PREVIEW_BORDER;
	if (border * 2 > box.width) {
		border = box.width / 2;
	}
	if (border * 2 > box.height) {
		border = box.height / 2;
	}
	if (border < 1) {
		border = 1;
	}
	int inner_h = box.height - border * 2;
	if (inner_h < 1) {
		inner_h = 1;
	}

	if (server->drag_target != target) {
		const char *name = "fullscreen";
		if (target == NIDE_SNAP_TARGET_LEFT) {
			name = "left half";
		} else if (target == NIDE_SNAP_TARGET_RIGHT) {
			name = "right half";
		}
		wlr_log(WLR_DEBUG, "snap preview: %s", name);
	}
	server->drag_target = target;

	wlr_scene_node_set_position(&server->preview_fill->node, box.x, box.y);
	wlr_scene_rect_set_size(server->preview_fill, box.width, box.height);

	wlr_scene_node_set_position(&server->preview_border[0]->node,
		box.x, box.y);
	wlr_scene_rect_set_size(server->preview_border[0], box.width, border);

	wlr_scene_node_set_position(&server->preview_border[1]->node,
		box.x, box.y + box.height - border);
	wlr_scene_rect_set_size(server->preview_border[1], box.width, border);

	wlr_scene_node_set_position(&server->preview_border[2]->node,
		box.x, box.y + border);
	wlr_scene_rect_set_size(server->preview_border[2], border, inner_h);

	wlr_scene_node_set_position(&server->preview_border[3]->node,
		box.x + box.width - border, box.y + border);
	wlr_scene_rect_set_size(server->preview_border[3], border, inner_h);

	wlr_scene_node_set_enabled(&server->preview_tree->node, true);
	drag_overlay_raise(server);
}

static struct wlr_box drag_output_box(struct nide_server *server) {
	struct wlr_box box = {0};
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);

	if (output != NULL) {
		wlr_output_layout_get_box(server->output_layout, output, &box);
	}
	if ((box.width <= 0 || box.height <= 0) &&
			server->grabbed_toplevel != NULL) {
		box = toplevel_output_box(server->grabbed_toplevel, NULL);
	}
	if (box.width <= 0 || box.height <= 0) {
		box = (struct wlr_box) {
			.x = 0, .y = 0, .width = 1024, .height = 768,
		};
	}
	return box;
}

static enum nide_snap_target drag_snap_target(struct nide_server *server) {
	struct wlr_box box = drag_output_box(server);
	int threshold = NIDE_SNAP_THRESHOLD;
	double cursor_x = server->cursor->x;
	double cursor_y = server->cursor->y;
	int left_dist = (int)cursor_x - box.x;
	int right_dist = (box.x + box.width) - (int)cursor_x;
	int top_dist = (int)cursor_y - box.y;
	int best = threshold + 1;
	enum nide_snap_target target = NIDE_SNAP_TARGET_NONE;

	if (left_dist < 0) {
		left_dist = 0;
	}
	if (right_dist < 0) {
		right_dist = 0;
	}
	if (top_dist < 0) {
		top_dist = 0;
	}

	/*
	 * The pointer triggers the preview, not the window edge. This lets
	 * the user snap a large window without pushing its whole width to
	 * the screen edge. A tie favours the side edge, which is the more
	 * common gesture.
	 */
	if (left_dist <= threshold && left_dist <= best) {
		target = NIDE_SNAP_TARGET_LEFT;
		best = left_dist;
	}
	if (right_dist <= threshold && right_dist < best) {
		target = NIDE_SNAP_TARGET_RIGHT;
		best = right_dist;
	}
	if (top_dist <= threshold && top_dist < best) {
		target = NIDE_SNAP_TARGET_FULLSCREEN;
	}

	return target;
}

static struct wlr_box drag_preview_box(struct nide_server *server,
		enum nide_snap_target target) {
	struct wlr_box box = drag_output_box(server);

	if (target == NIDE_SNAP_TARGET_FULLSCREEN) {
		return box;
	}

	return snap_box_for_output(box, server->config.maximize_margin,
		target == NIDE_SNAP_TARGET_LEFT);
}

static void drag_preview_update(struct nide_server *server,
		struct nide_toplevel *toplevel) {
	enum nide_snap_target target;

	if (!server->move_started ||
			toplevel->state != NIDE_TOPLEVEL_FLOATING) {
		drag_preview_hide(server);
		return;
	}

	if (server->drag_suppressed == NIDE_SNAP_TARGET_LEFT ||
			server->drag_suppressed == NIDE_SNAP_TARGET_RIGHT) {
		struct wlr_box output = drag_output_box(server);
		int cursor_x = (int)server->cursor->x;
		bool cleared;

		if (server->drag_suppressed == NIDE_SNAP_TARGET_LEFT) {
			cleared = cursor_x - output.x > NIDE_SNAP_THRESHOLD;
		} else {
			cleared = output.x + output.width - cursor_x >
				NIDE_SNAP_THRESHOLD;
		}
		if (cleared) {
			server->drag_suppressed = NIDE_SNAP_TARGET_NONE;
		}
	}

	target = drag_snap_target(server);
	if (target == server->drag_suppressed) {
		target = NIDE_SNAP_TARGET_NONE;
	}
	if (target == NIDE_SNAP_TARGET_NONE) {
		drag_preview_hide(server);
		return;
	}

	drag_preview_show(server, target, drag_preview_box(server, target));
}

static void snap_gauge_update(struct nide_server *server,
		struct nide_toplevel *toplevel, double pressure) {
	struct wlr_box box = toplevel_snap_box(toplevel);
	struct wlr_box output = toplevel_output_box(toplevel, NULL);
	bool left = toplevel->state == NIDE_TOPLEVEL_SNAPPED_LEFT;
	int bar_w = 6;
	int gap;
	int bar_x;
	int track_h;
	int track_y;
	int fill_h;

	if (pressure < 0) {
		pressure = 0;
	}
	if (pressure > 1) {
		pressure = 1;
	}

	if (left) {
		gap = box.x - output.x;
		if (gap >= bar_w + 2) {
			bar_x = output.x + (gap - bar_w) / 2;
		} else {
			bar_x = box.x + 2;
		}
	} else {
		gap = output.x + output.width - (box.x + box.width);
		if (gap >= bar_w + 2) {
			bar_x = box.x + box.width + (gap - bar_w) / 2;
		} else {
			bar_x = box.x + box.width - bar_w - 2;
		}
	}

	track_h = box.height - 16;
	if (track_h > 180) {
		track_h = 180;
	}
	if (track_h > box.height) {
		track_h = box.height;
	}
	if (track_h < 1) {
		track_h = 1;
	}
	track_y = box.y + (box.height - track_h) / 2;

	wlr_scene_node_set_position(&server->snap_gauge_backdrop->node,
		bar_x - 2, track_y - 4);
	wlr_scene_rect_set_size(server->snap_gauge_backdrop,
		bar_w + 4, track_h + 8);

	wlr_scene_node_set_position(&server->snap_gauge_track->node,
		bar_x, track_y);
	wlr_scene_rect_set_size(server->snap_gauge_track, bar_w, track_h);

	fill_h = track_h / 4 +
		(int)((track_h - track_h / 4) * pressure);
	if (fill_h < 1) {
		fill_h = 1;
	}
	wlr_scene_node_set_position(&server->snap_gauge_fill->node,
		bar_x, track_y + (track_h - fill_h) / 2);
	wlr_scene_rect_set_size(server->snap_gauge_fill, bar_w, fill_h);

	wlr_scene_node_set_enabled(&server->snap_gauge_tree->node, true);
	drag_overlay_raise(server);
}

static void toplevel_apply_snap(struct nide_toplevel *toplevel) {
	struct wlr_box box = toplevel_snap_box(toplevel);
	uint32_t tiled = WLR_EDGE_TOP | WLR_EDGE_BOTTOM;
	if (toplevel->state == NIDE_TOPLEVEL_SNAPPED_LEFT) {
		tiled |= WLR_EDGE_LEFT;
	} else {
		tiled |= WLR_EDGE_RIGHT;
	}

	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
	wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
	wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, tiled);
	toplevel_apply_box(toplevel, box, true);

	wlr_log(WLR_DEBUG, "snapped geometry: %dx%d+%d+%d",
		box.width, box.height, box.x, box.y);
}

static void toplevel_apply_maximized(struct nide_toplevel *toplevel) {
	struct wlr_box box = inset_box(toplevel_output_box(toplevel, NULL),
		toplevel->server->config.maximize_margin);

	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
	wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, 0);
	wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, true);
	toplevel_apply_box(toplevel, box, true);

	wlr_log(WLR_DEBUG, "maximized geometry: %dx%d+%d+%d (margin %d)",
		box.width, box.height, box.x, box.y,
		toplevel->server->config.maximize_margin);
}

static void toplevel_apply_fullscreen(struct nide_toplevel *toplevel,
		struct wlr_output *output) {
	struct wlr_box box = toplevel_output_box(toplevel, output);

	wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
	wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, 0);
	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
	toplevel_apply_box(toplevel, box, true);

	wlr_log(WLR_DEBUG, "fullscreen geometry: %dx%d+%d+%d",
		box.width, box.height, box.x, box.y);
}

static void toplevel_apply_floating(struct nide_toplevel *toplevel) {
	struct wlr_box box = toplevel->floating_geometry;
	if (box.width < 1 || box.height < 1) {
		box.width = box.width > 0 ? box.width : 800;
		box.height = box.height > 0 ? box.height : 600;
		toplevel->floating_geometry = box;
	}

	toplevel->state = NIDE_TOPLEVEL_FLOATING;
	toplevel->restore_floating_pending = true;
	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
	wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
	wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, 0);
	toplevel_apply_box(toplevel, box, true);
}

static void toplevel_enter_maximized(struct nide_toplevel *toplevel) {
	if (toplevel->state == NIDE_TOPLEVEL_MAXIMIZED ||
			toplevel->state == NIDE_TOPLEVEL_FULLSCREEN) {
		return;
	}

	if (toplevel->state == NIDE_TOPLEVEL_FLOATING) {
		toplevel_capture_floating(toplevel);
	}
	toplevel->restore_state = toplevel->state;
	toplevel->state = NIDE_TOPLEVEL_MAXIMIZED;
	toplevel_apply_maximized(toplevel);

	wlr_log(WLR_DEBUG, "maximized window (margin %d)",
		toplevel->server->config.maximize_margin);
}

static void toplevel_leave_maximized(struct nide_toplevel *toplevel) {
	if (toplevel->state != NIDE_TOPLEVEL_MAXIMIZED) {
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
		return;
	}

	if (toplevel->restore_state == NIDE_TOPLEVEL_SNAPPED_LEFT ||
			toplevel->restore_state == NIDE_TOPLEVEL_SNAPPED_RIGHT) {
		toplevel->state = toplevel->restore_state;
		toplevel_apply_snap(toplevel);
	} else {
		toplevel_apply_floating(toplevel);
	}
}

static void toplevel_enter_fullscreen(struct nide_toplevel *toplevel) {
	if (toplevel->state == NIDE_TOPLEVEL_FULLSCREEN) {
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
		return;
	}

	if (toplevel->state == NIDE_TOPLEVEL_FLOATING) {
		toplevel_capture_floating(toplevel);
	}
	toplevel->restore_state = toplevel->state;
	toplevel->state = NIDE_TOPLEVEL_FULLSCREEN;

	struct wlr_output *output =
		toplevel->xdg_toplevel->requested.fullscreen_output;
	toplevel_apply_fullscreen(toplevel, output);

	wlr_log(WLR_DEBUG, "fullscreen window");
}

static void toplevel_leave_fullscreen(struct nide_toplevel *toplevel) {
	if (toplevel->state != NIDE_TOPLEVEL_FULLSCREEN) {
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
		return;
	}

	switch (toplevel->restore_state) {
	case NIDE_TOPLEVEL_MAXIMIZED:
		toplevel->state = NIDE_TOPLEVEL_MAXIMIZED;
		toplevel_apply_maximized(toplevel);
		break;
	case NIDE_TOPLEVEL_SNAPPED_LEFT:
	case NIDE_TOPLEVEL_SNAPPED_RIGHT:
		toplevel->state = toplevel->restore_state;
		toplevel_apply_snap(toplevel);
		break;
	default:
		toplevel_apply_floating(toplevel);
		break;
	}
}

static void toplevel_toggle_maximized(struct nide_toplevel *toplevel) {
	if (toplevel->state == NIDE_TOPLEVEL_FULLSCREEN) {
		if (toplevel->restore_state == NIDE_TOPLEVEL_MAXIMIZED) {
			toplevel_leave_fullscreen(toplevel);
			return;
		}

		/*
		 * Do not restore floating geometry first: the client would still
		 * report its fullscreen size and we would overwrite the saved
		 * floating size with it.
		 */
		toplevel->state = NIDE_TOPLEVEL_MAXIMIZED;
		toplevel_apply_maximized(toplevel);
		return;
	}
	if (toplevel->state == NIDE_TOPLEVEL_MAXIMIZED) {
		toplevel_leave_maximized(toplevel);
	} else {
		toplevel_enter_maximized(toplevel);
	}
}

static void toplevel_toggle_fullscreen(struct nide_toplevel *toplevel) {
	if (toplevel->state == NIDE_TOPLEVEL_FULLSCREEN) {
		toplevel_leave_fullscreen(toplevel);
	} else {
		toplevel_enter_fullscreen(toplevel);
	}
}

static void toplevel_to_floating(struct nide_toplevel *toplevel,
		bool keep_current_geometry) {
	if (toplevel->state == NIDE_TOPLEVEL_FLOATING) {
		return;
	}

	if (keep_current_geometry) {
		toplevel_capture_floating(toplevel);
		toplevel->state = NIDE_TOPLEVEL_FLOATING;
		toplevel->restore_floating_pending = false;
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
		wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, 0);
	} else {
		toplevel_apply_floating(toplevel);
	}
}

static void toplevel_break_snap(struct nide_toplevel *toplevel) {
	struct nide_server *server = toplevel->server;
	struct wlr_box box = toplevel->floating_geometry;
	struct wlr_box output = toplevel_output_box(toplevel, NULL);
	bool was_left = toplevel->state == NIDE_TOPLEVEL_SNAPPED_LEFT;
	double grab_x = toplevel->snap_grab_x;
	double grab_y = toplevel->snap_grab_y;
	int clearance = NIDE_SNAP_THRESHOLD + 2;

	if (grab_x < 0) {
		grab_x = 0;
	} else if (grab_x > 1) {
		grab_x = 1;
	}
	if (grab_y < 0) {
		grab_y = 0;
	} else if (grab_y > 1) {
		grab_y = 1;
	}

	if (box.width < 1 || box.height < 1) {
		box.width = 800;
		box.height = 600;
	}

	box.x = (int)(server->cursor->x - box.width * grab_x);
	box.y = (int)(server->cursor->y - box.height * grab_y);

	/*
	 * Move the restored window just outside the snap zone. Otherwise it
	 * would still touch the edge it came from and the drag preview would
	 * immediately snap it back.
	 */
	if (box.width <= output.width - clearance * 2) {
		if (was_left) {
			int min_x = output.x + clearance;
			if (box.x < min_x) {
				box.x = min_x;
			}
		} else {
			int max_x = output.x + output.width - clearance -
				box.width;
			if (box.x > max_x) {
				box.x = max_x;
			}
		}
	}

	toplevel->floating_geometry = box;
	toplevel->has_floating_geometry = true;
	toplevel->state = NIDE_TOPLEVEL_FLOATING;
	toplevel->restore_floating_pending = true;
	server->drag_suppressed = was_left ?
		NIDE_SNAP_TARGET_LEFT : NIDE_SNAP_TARGET_RIGHT;

	wlr_xdg_toplevel_set_tiled(toplevel->xdg_toplevel, 0);
	toplevel_apply_box(toplevel, box, true);

	server->grab_x = server->cursor->x -
		toplevel->scene_tree->node.x;
	server->grab_y = server->cursor->y -
		toplevel->scene_tree->node.y;

	wlr_log(WLR_DEBUG, "snap released: floating geometry %dx%d+%d+%d",
		box.width, box.height, box.x, box.y);
}

static void toplevel_snap(struct nide_toplevel *toplevel,
		enum nide_toplevel_state side) {
	if (toplevel->state == NIDE_TOPLEVEL_FLOATING) {
		toplevel_capture_floating(toplevel);
	}

	toplevel->state = side;
	toplevel->snap_anchor_x = toplevel->server->cursor->x;
	toplevel_apply_snap(toplevel);

	wlr_log(WLR_DEBUG, "snapped window to %s half",
		side == NIDE_TOPLEVEL_SNAPPED_LEFT ? "left" : "right");
}

static void toplevel_apply_drag_target(struct nide_toplevel *toplevel,
		enum nide_snap_target target) {
	if (toplevel->state != NIDE_TOPLEVEL_FLOATING) {
		return;
	}

	switch (target) {
	case NIDE_SNAP_TARGET_LEFT:
		toplevel_snap(toplevel, NIDE_TOPLEVEL_SNAPPED_LEFT);
		break;
	case NIDE_SNAP_TARGET_RIGHT:
		toplevel_snap(toplevel, NIDE_TOPLEVEL_SNAPPED_RIGHT);
		break;
	case NIDE_SNAP_TARGET_FULLSCREEN:
		toplevel_enter_fullscreen(toplevel);
		break;
	case NIDE_SNAP_TARGET_NONE:
		break;
	}
}

static void toplevel_reapply_state(struct nide_toplevel *toplevel) {
	switch (toplevel->state) {
	case NIDE_TOPLEVEL_SNAPPED_LEFT:
	case NIDE_TOPLEVEL_SNAPPED_RIGHT:
		toplevel_apply_snap(toplevel);
		break;
	case NIDE_TOPLEVEL_MAXIMIZED:
		toplevel_apply_maximized(toplevel);
		break;
	case NIDE_TOPLEVEL_FULLSCREEN:
		toplevel_apply_fullscreen(toplevel,
			toplevel->xdg_toplevel->requested.fullscreen_output);
		break;
	case NIDE_TOPLEVEL_FLOATING:
		break;
	}
}

static void toplevel_sync_state_geometry(struct nide_toplevel *toplevel) {
	switch (toplevel->state) {
	case NIDE_TOPLEVEL_FLOATING:
		if (toplevel->restore_floating_pending) {
			struct wlr_box *geo =
				&toplevel->xdg_toplevel->base->geometry;
			struct wlr_box box = toplevel->floating_geometry;
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
				box.x - geo->x, box.y - geo->y);
			toplevel->restore_floating_pending = false;
		}
		break;
	case NIDE_TOPLEVEL_SNAPPED_LEFT:
	case NIDE_TOPLEVEL_SNAPPED_RIGHT:
		toplevel_apply_box(toplevel, toplevel_snap_box(toplevel), false);
		break;
	case NIDE_TOPLEVEL_MAXIMIZED:
		toplevel_apply_box(toplevel,
			inset_box(toplevel_output_box(toplevel, NULL),
				toplevel->server->config.maximize_margin),
			false);
		break;
	case NIDE_TOPLEVEL_FULLSCREEN:
		toplevel_apply_box(toplevel,
			toplevel_output_box(toplevel,
				toplevel->xdg_toplevel->requested.fullscreen_output),
			false);
		break;
	}
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

	wlr_scene_node_set_position(&toplevel->scene_tree->node,
		x - geo->x, y - geo->y);
	toplevel->placed = true;
	toplevel_capture_floating(toplevel);
	wlr_log(WLR_DEBUG, "window placement: %dx%d+%d+%d (geometry %dx%d+%d+%d)",
		width, height, x, y, geo->width, geo->height, geo->x, geo->y);
}

static void begin_interactive(struct nide_toplevel *toplevel,
		enum nide_cursor_mode mode, uint32_t edges) {
	struct nide_server *server = toplevel->server;

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	drag_overlay_hide(server);
	server->drag_suppressed = NIDE_SNAP_TARGET_NONE;
	server->move_start_x = server->cursor->x;
	server->move_start_y = server->cursor->y;
	server->move_started = false;

	if (mode == NIDE_CURSOR_MOVE) {
		if (toplevel->state == NIDE_TOPLEVEL_MAXIMIZED ||
				toplevel->state == NIDE_TOPLEVEL_FULLSCREEN) {
			toplevel_to_floating(toplevel, false);
		} else if (toplevel->state == NIDE_TOPLEVEL_SNAPPED_LEFT ||
				toplevel->state == NIDE_TOPLEVEL_SNAPPED_RIGHT) {
			toplevel->snap_anchor_x = server->cursor->x;
			toplevel->snap_anchor_y = server->cursor->y;

			struct wlr_box geo = toplevel_geometry(toplevel);
			toplevel->snap_grab_x = geo.width > 0 ?
				(server->cursor->x - geo.x) / geo.width : 0.5;
			toplevel->snap_grab_y = geo.height > 0 ?
				(server->cursor->y - geo.y) / geo.height : 0.5;
			if (toplevel->snap_grab_x < 0) {
				toplevel->snap_grab_x = 0;
			} else if (toplevel->snap_grab_x > 1) {
				toplevel->snap_grab_x = 1;
			}
			if (toplevel->snap_grab_y < 0) {
				toplevel->snap_grab_y = 0;
			} else if (toplevel->snap_grab_y > 1) {
				toplevel->snap_grab_y = 1;
			}

			snap_gauge_update(server, toplevel, 0);
		}

		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		if (toplevel->state == NIDE_TOPLEVEL_MAXIMIZED ||
				toplevel->state == NIDE_TOPLEVEL_FULLSCREEN ||
				toplevel->state == NIDE_TOPLEVEL_SNAPPED_LEFT ||
				toplevel->state == NIDE_TOPLEVEL_SNAPPED_RIGHT) {
			toplevel_to_floating(toplevel, true);
		}

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

	if (!server->move_started) {
		double dx = server->cursor->x - server->move_start_x;
		double dy = server->cursor->y - server->move_start_y;

		server->move_started = dx * dx + dy * dy >= 9;
	}

	if (toplevel->state == NIDE_TOPLEVEL_SNAPPED_LEFT ||
			toplevel->state == NIDE_TOPLEVEL_SNAPPED_RIGHT) {
		bool left = toplevel->state == NIDE_TOPLEVEL_SNAPPED_LEFT;
		double away_x = left ?
			server->cursor->x - toplevel->snap_anchor_x :
			toplevel->snap_anchor_x - server->cursor->x;
		double away_y = server->cursor->y - toplevel->snap_anchor_y;
		double pressure;

		/*
		 * Pushing toward the snapped edge must never detach. Pulling
		 * inward horizontally or vertically builds up pressure until
		 * the window breaks free.
		 */
		if (away_x < 0) {
			away_x = 0;
		}
		pressure = sqrt(away_x * away_x + away_y * away_y) /
			NIDE_SNAP_RESISTANCE;

		if (pressure < 1) {
			snap_gauge_update(server, toplevel, pressure);
			return;
		}

		/*
		 * The window has been pulled past the resistance threshold.
		 * Restore its floating size and keep the pointer at the same
		 * relative position inside it.
		 */
		drag_gauge_hide(server);
		toplevel_break_snap(toplevel);
	}

	int node_x = (int)(server->cursor->x - server->grab_x);
	int node_y = (int)(server->cursor->y - server->grab_y);

	wlr_scene_node_set_position(&toplevel->scene_tree->node, node_x, node_y);
	if (toplevel->state == NIDE_TOPLEVEL_FLOATING) {
		toplevel_capture_floating(toplevel);
	}
	drag_preview_update(server, toplevel);
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

	toplevel->floating_geometry = (struct wlr_box) {
		.x = new_left,
		.y = new_top,
		.width = new_right - new_left,
		.height = new_bottom - new_top,
	};
	toplevel->has_floating_geometry = true;
	toplevel->restore_floating_pending = false;
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
	case NIDE_ACTION_MAXIMIZE:
		if (server->focused != NULL) {
			toplevel_toggle_maximized(server->focused);
		}
		break;
	case NIDE_ACTION_FULLSCREEN:
		if (server->focused != NULL) {
			toplevel_toggle_fullscreen(server->focused);
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
		if (server->cursor_mode == NIDE_CURSOR_MOVE &&
				server->grabbed_toplevel != NULL &&
				server->drag_target != NIDE_SNAP_TARGET_NONE) {
			struct nide_toplevel *toplevel = server->grabbed_toplevel;
			enum nide_snap_target target = server->drag_target;

			drag_overlay_hide(server);
			toplevel_apply_drag_target(toplevel, target);
		}
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

static void output_layout_change(struct wl_listener *listener, void *data) {
	struct nide_server *server =
		wl_container_of(listener, server, output_layout_change);
	struct nide_toplevel *toplevel;

	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->state != NIDE_TOPLEVEL_FLOATING) {
			toplevel_reapply_state(toplevel);
		}
	}
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

	if (!toplevel->placed) {
		place_toplevel(toplevel);
	}

	if (toplevel->pending_fullscreen) {
		toplevel->pending_fullscreen = false;
		toplevel->pending_maximize = false;
		toplevel_enter_fullscreen(toplevel);
	} else if (toplevel->pending_maximize) {
		toplevel->pending_maximize = false;
		toplevel_enter_maximized(toplevel);
	} else if (toplevel->state != NIDE_TOPLEVEL_FLOATING) {
		toplevel_reapply_state(toplevel);
	} else {
		toplevel_apply_floating(toplevel);
	}

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
		return;
	}

	toplevel_sync_state_geometry(toplevel);
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
	bool maximized = toplevel->xdg_toplevel->requested.maximized;

	if (!toplevel->xdg_toplevel->base->initialized) {
		toplevel->pending_maximize = maximized;
		return;
	}

	if (toplevel->state == NIDE_TOPLEVEL_FULLSCREEN) {
		/*
		 * Fullscreen takes priority. Acknowledge the request without
		 * changing the layout, so the client never waits for a
		 * configure that never arrives.
		 */
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, true);
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
		return;
	}

	if (maximized) {
		if (toplevel->state != NIDE_TOPLEVEL_MAXIMIZED) {
			toplevel_enter_maximized(toplevel);
		} else {
			wlr_xdg_toplevel_set_maximized(
				toplevel->xdg_toplevel, true);
		}
	} else if (toplevel->state == NIDE_TOPLEVEL_MAXIMIZED) {
		toplevel_leave_maximized(toplevel);
	} else {
		wlr_xdg_toplevel_set_maximized(toplevel->xdg_toplevel, false);
	}
}

static void xdg_toplevel_request_fullscreen(struct wl_listener *listener,
		void *data) {
	struct nide_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	bool fullscreen = toplevel->xdg_toplevel->requested.fullscreen;

	if (!toplevel->xdg_toplevel->base->initialized) {
		toplevel->pending_fullscreen = fullscreen;
		return;
	}

	if (fullscreen) {
		if (toplevel->state != NIDE_TOPLEVEL_FULLSCREEN) {
			toplevel_enter_fullscreen(toplevel);
		} else {
			wlr_xdg_toplevel_set_fullscreen(
				toplevel->xdg_toplevel, true);
		}
	} else if (toplevel->state == NIDE_TOPLEVEL_FULLSCREEN) {
		toplevel_leave_fullscreen(toplevel);
	} else {
		wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, false);
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
		case NIDE_ACTION_MAXIMIZE: action = "maximize"; break;
		case NIDE_ACTION_FULLSCREEN: action = "fullscreen"; break;
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
		printf("Maximize margin: %d\n", config.maximize_margin);
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
	server.output_layout_change.notify = output_layout_change;
	wl_signal_add(&server.output_layout->events.change,
		&server.output_layout_change);

	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(server.scene,
		server.output_layout);
	drag_overlay_init(&server);

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
	wl_list_remove(&server.output_layout_change.link);

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
