/*
 * NIDE (NIDE Isn't a Desktop) - a small wlroots-based Wayland compositor.
 *
 * - Floating windows (not tiling), with no compositor-side decorations.
 * - Keybindings are configured in Lua, with hot reload.
 * - Super+W launches a terminal, Super+C closes the focused window.
 * - Super+M maximizes (with a configurable margin), Super+S toggles fullscreen.
 */

#include <ctype.h>
#include <dirent.h>
#include <drm_fourcc.h>
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

#include <cairo/cairo.h>
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>

#include <wlr/backend.h>
#include <wlr/backend/x11.h>
#include <wlr/interfaces/wlr_buffer.h>
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

#define NIDE_VERSION "0.4.0"
#define MAX_BINDS 64
#define NIDE_MAXIMIZE_MARGIN_DEFAULT 8
#define NIDE_SNAP_THRESHOLD 24
#define NIDE_SNAP_RESISTANCE 64
#define NIDE_PREVIEW_BORDER 3
#define NIDE_LAUNCHER_COLS 5
#define NIDE_LAUNCHER_ROWS 3
#define NIDE_LAUNCHER_PAGE_SIZE \
	(NIDE_LAUNCHER_COLS * NIDE_LAUNCHER_ROWS)
#define NIDE_LAUNCHER_QUERY_MAX 63
#define NIDE_LAUNCHER_ICON_CACHE 64

struct nide_launcher_icon {
	char path[512];
	cairo_surface_t *surface;
	uint64_t used;
};

#define NIDE_MOD_MASK \
	(WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

enum nide_action {
	NIDE_ACTION_SPAWN,
	NIDE_ACTION_CLOSE,
	NIDE_ACTION_MAXIMIZE,
	NIDE_ACTION_FULLSCREEN,
	NIDE_ACTION_LAUNCHER,
	NIDE_ACTION_QUIT,
};

enum nide_launcher_mode {
	NIDE_LAUNCHER_WINDOWS,
	NIDE_LAUNCHER_APPS,
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

struct nide_app {
	char name[128];
	char exec[512];
	char id[128];
	char icon[512];
	bool terminal;
};

struct nide_launcher_item {
	char name[128];
	char detail[256];
	char exec[512];
	char app_id[128];
	char icon[512];
	int count;
	bool terminal;
	struct nide_toplevel *toplevel;
};

struct nide_launcher {
	bool open;
	bool apps_loaded;
	enum nide_launcher_mode mode;
	char query[NIDE_LAUNCHER_QUERY_MAX + 1];

	struct nide_app *apps;
	int napps;
	int appcap;

	struct nide_launcher_item *items;
	int nitems;
	int itemcap;

	int *filtered;
	int nfiltered;
	int filtercap;

	int selected;
	int scroll;
	bool scrollbar_dragging;
	double scrollbar_grab;
	struct wlr_box scrollbar;
	struct wlr_box scroll_track;

	struct nide_launcher_icon icons[NIDE_LAUNCHER_ICON_CACHE];
	int nicons;
	uint64_t icon_tick;

	struct wlr_box output;
	struct wlr_box panel;
	struct wlr_box search;
	struct wlr_box tabs[2];
	struct wlr_box grid;
	int tile_w;
	int tile_h;
	int gap;
	int icon_size;
};

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
	struct wlr_box preview_box;
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
	double snap_gauge_pressure;
	struct wlr_box snap_gauge_box;
	bool snap_gauge_visible;

	struct wlr_scene_tree *launcher_tree;
	struct wlr_scene_buffer *launcher_scene_buffer;
	struct nide_launcher launcher;

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
static void launcher_init(struct nide_server *server);
static void launcher_open(struct nide_server *server);
static void launcher_close(struct nide_server *server);
static void launcher_render(struct nide_server *server);
static void launcher_build_items(struct nide_server *server);
static void launcher_handle_motion(struct nide_server *server);
static void launcher_handle_key(struct nide_server *server,
		struct nide_keyboard *keyboard,
		struct wlr_keyboard_key_event *event);
static void launcher_handle_click(struct nide_server *server, uint32_t button);
static void launcher_handle_release(struct nide_server *server);
static void launcher_handle_axis(struct nide_server *server,
		struct wlr_pointer_axis_event *event);

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
	} else if (strcmp(action, "launcher") == 0) {
		act = NIDE_ACTION_LAUNCHER;
	} else if (strcmp(action, "quit") == 0) {
		act = NIDE_ACTION_QUIT;
	} else {
		snprintf(err, errsz,
			"unknown action: '%s' "
			"(use spawn/close/maximize/fullscreen/launcher/quit)",
			action);
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
	bind_set(cfg, "super+tab", "launcher", NULL, err, sizeof(err));
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
	if (keyboard != NULL && !server->launcher.open) {
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

	toplevel->floating_geometry = box;
	toplevel->has_floating_geometry = true;
}

/*
 * Keep the saved geometry reachable. Fullscreen and snap gestures can end
 * with the window partly outside the layout, but restoring that position
 * later would put the window off-screen. This only needs to run when the
 * geometry is about to be used for a restore, not on every pointer motion.
 */
static void toplevel_clamp_floating_geometry(struct nide_toplevel *toplevel) {
	if (!toplevel->has_floating_geometry) {
		return;
	}

	struct wlr_box *box = &toplevel->floating_geometry;
	struct wlr_box layout = {0};
	wlr_output_layout_get_box(toplevel->server->output_layout, NULL,
		&layout);
	if (layout.width <= 0 || layout.height <= 0) {
		return;
	}

	int margin = 8;
	int min_x = layout.x + margin;
	int min_y = layout.y + margin;
	int max_x = layout.x + layout.width - box->width - margin;
	int max_y = layout.y + layout.height - box->height - margin;

	if (max_x < min_x) {
		max_x = min_x;
	}
	if (max_y < min_y) {
		max_y = min_y;
	}
	if (box->x < min_x) {
		box->x = min_x;
	} else if (box->x > max_x) {
		box->x = max_x;
	}
	if (box->y < min_y) {
		box->y = min_y;
	} else if (box->y > max_y) {
		box->y = max_y;
	}
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
	if (server->drag_target == NIDE_SNAP_TARGET_NONE) {
		return;
	}
	wlr_log(WLR_DEBUG, "snap preview cleared");
	server->drag_target = NIDE_SNAP_TARGET_NONE;
	wlr_scene_node_set_enabled(&server->preview_tree->node, false);
}

static void drag_gauge_hide(struct nide_server *server) {
	if (!server->snap_gauge_visible) {
		return;
	}
	server->snap_gauge_visible = false;
	wlr_scene_node_set_enabled(&server->snap_gauge_tree->node, false);
}

static void drag_overlay_hide(struct nide_server *server) {
	drag_preview_hide(server);
	drag_gauge_hide(server);
}

static void drag_preview_show(struct nide_server *server,
		enum nide_snap_target target, struct wlr_box box) {
	bool was_visible = server->drag_target != NIDE_SNAP_TARGET_NONE;
	bool raise = !was_visible || server->drag_target != target;

	if (was_visible && server->drag_target == target &&
			wlr_box_equal(&server->preview_box, &box)) {
		return;
	}

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
	server->preview_box = box;

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

	if (!was_visible) {
		wlr_scene_node_set_enabled(&server->preview_tree->node, true);
	}
	if (raise) {
		drag_overlay_raise(server);
	}
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

static enum nide_snap_target drag_snap_target(struct nide_server *server,
		struct wlr_box output) {
	int threshold = NIDE_SNAP_THRESHOLD;
	int cursor_x = (int)server->cursor->x;
	int cursor_y = (int)server->cursor->y;
	int left_dist = cursor_x - output.x;
	int right_dist = (output.x + output.width) - cursor_x;
	int top_dist = cursor_y - output.y;
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
		struct wlr_box output, enum nide_snap_target target) {
	if (target == NIDE_SNAP_TARGET_FULLSCREEN) {
		return output;
	}

	return snap_box_for_output(output, server->config.maximize_margin,
		target == NIDE_SNAP_TARGET_LEFT);
}

static void drag_preview_update(struct nide_server *server,
		struct nide_toplevel *toplevel) {
	struct wlr_box output;
	enum nide_snap_target target;

	if (!server->move_started ||
			toplevel->state != NIDE_TOPLEVEL_FLOATING) {
		drag_preview_hide(server);
		return;
	}

	output = drag_output_box(server);

	if (server->drag_suppressed == NIDE_SNAP_TARGET_LEFT ||
			server->drag_suppressed == NIDE_SNAP_TARGET_RIGHT) {
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

	target = drag_snap_target(server, output);
	if (target == server->drag_suppressed) {
		target = NIDE_SNAP_TARGET_NONE;
	}
	if (target == NIDE_SNAP_TARGET_NONE) {
		drag_preview_hide(server);
		return;
	}

	drag_preview_show(server, target,
		drag_preview_box(server, output, target));
}

static void snap_gauge_update(struct nide_server *server,
		struct nide_toplevel *toplevel, double pressure) {
	struct wlr_box output = toplevel_output_box(toplevel, NULL);
	bool left = toplevel->state == NIDE_TOPLEVEL_SNAPPED_LEFT;
	struct wlr_box box = snap_box_for_output(output,
		server->config.maximize_margin, left);
	bool was_visible = server->snap_gauge_visible;
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

	if (was_visible && server->snap_gauge_pressure == pressure &&
			wlr_box_equal(&server->snap_gauge_box, &box)) {
		return;
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

	server->snap_gauge_pressure = pressure;
	server->snap_gauge_box = box;
	if (!was_visible) {
		server->snap_gauge_visible = true;
		wlr_scene_node_set_enabled(&server->snap_gauge_tree->node, true);
		drag_overlay_raise(server);
	}
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
		toplevel_clamp_floating_geometry(toplevel);
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
		toplevel_clamp_floating_geometry(toplevel);
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
		toplevel_clamp_floating_geometry(toplevel);
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
		toplevel_clamp_floating_geometry(toplevel);
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
	toplevel_clamp_floating_geometry(toplevel);
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
	if (server->launcher.open) {
		launcher_handle_motion(server);
		return;
	}
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
/* Launcher                                                                  */
/* ------------------------------------------------------------------------- */

struct nide_cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void nide_cairo_buffer_destroy(struct wlr_buffer *buffer) {
	struct nide_cairo_buffer *cairo_buffer =
		wl_container_of(buffer, cairo_buffer, base);

	cairo_surface_destroy(cairo_buffer->surface);
	wlr_buffer_finish(buffer);
	free(cairo_buffer);
}

static bool nide_cairo_buffer_begin_data_ptr_access(struct wlr_buffer *buffer,
		uint32_t flags, void **data, uint32_t *format, size_t *stride) {
	struct nide_cairo_buffer *cairo_buffer =
		wl_container_of(buffer, cairo_buffer, base);

	cairo_surface_flush(cairo_buffer->surface);
	void *surface_data =
		cairo_image_surface_get_data(cairo_buffer->surface);
	if (surface_data == NULL) {
		return false;
	}

	*data = surface_data;
	*format = DRM_FORMAT_ARGB8888;
	*stride = (size_t)cairo_image_surface_get_stride(cairo_buffer->surface);
	return true;
}

static void nide_cairo_buffer_end_data_ptr_access(struct wlr_buffer *buffer) {
	struct nide_cairo_buffer *cairo_buffer =
		wl_container_of(buffer, cairo_buffer, base);

	cairo_surface_mark_dirty(cairo_buffer->surface);
}

static const struct wlr_buffer_impl nide_cairo_buffer_impl = {
	.destroy = nide_cairo_buffer_destroy,
	.begin_data_ptr_access = nide_cairo_buffer_begin_data_ptr_access,
	.end_data_ptr_access = nide_cairo_buffer_end_data_ptr_access,
};

static struct wlr_buffer *nide_cairo_buffer_create(
		cairo_surface_t *surface) {
	int width = cairo_image_surface_get_width(surface);
	int height = cairo_image_surface_get_height(surface);

	struct nide_cairo_buffer *buffer = calloc(1, sizeof(*buffer));
	if (buffer == NULL) {
		return NULL;
	}
	buffer->surface = surface;
	wlr_buffer_init(&buffer->base, &nide_cairo_buffer_impl, width, height);
	return &buffer->base;
}

static const uint32_t launcher_palette[] = {
	0x4f7cff, 0xe5484d, 0x30a46c, 0xf5a524,
	0x9a5cf5, 0x00a2c7, 0xe05a9a, 0x6b7f99,
};

static int launcher_clamp_int(int value, int min, int max) {
	if (value < min) {
		return min;
	}
	if (value > max) {
		return max;
	}
	return value;
}

static void launcher_set_color(cairo_t *cr, uint32_t color, double alpha) {
	cairo_set_source_rgba(cr,
		((color >> 16) & 0xff) / 255.0,
		((color >> 8) & 0xff) / 255.0,
		(color & 0xff) / 255.0,
		alpha);
}

static void launcher_rounded_rect(cairo_t *cr, double x, double y,
		double width, double height, double radius) {
	double max_radius = width < height ? width / 2.0 : height / 2.0;
	if (radius > max_radius) {
		radius = max_radius;
	}
	if (radius < 0) {
		radius = 0;
	}

	cairo_new_sub_path(cr);
	cairo_arc(cr, x + width - radius, y + radius, radius,
		-M_PI / 2.0, 0);
	cairo_arc(cr, x + width - radius, y + height - radius, radius,
		0, M_PI / 2.0);
	cairo_arc(cr, x + radius, y + height - radius, radius,
		M_PI / 2.0, M_PI);
	cairo_arc(cr, x + radius, y + radius, radius,
		M_PI, 3.0 * M_PI / 2.0);
	cairo_close_path(cr);
}

static void launcher_set_font(cairo_t *cr, double size, bool bold) {
	cairo_select_font_face(cr, "sans", CAIRO_FONT_SLANT_NORMAL,
		bold ? CAIRO_FONT_WEIGHT_BOLD : CAIRO_FONT_WEIGHT_NORMAL);
	cairo_set_font_size(cr, size);
}

static void launcher_ellipsize(cairo_t *cr, const char *text,
		double max_width, char *out, size_t outsz) {
	const char *src = text != NULL ? text : "";
	size_t len = strlen(src);
	if (len >= outsz) {
		len = outsz - 1;
	}
	memcpy(out, src, len);
	out[len] = '\0';

	cairo_text_extents_t extents;
	cairo_text_extents(cr, out, &extents);
	if (extents.x_advance <= max_width) {
		return;
	}

	while (len > 0) {
		do {
			len--;
		} while (len > 0 &&
			(((unsigned char)out[len] & 0xc0) == 0x80));
		out[len] = '\0';

		char candidate[512];
		snprintf(candidate, sizeof(candidate), "%s...", out);
		cairo_text_extents(cr, candidate, &extents);
		if (extents.x_advance <= max_width) {
			snprintf(out, outsz, "%s", candidate);
			return;
		}
	}

	snprintf(out, outsz, "...");
}

static void launcher_draw_text(cairo_t *cr, const char *text, double x,
		double y, double max_width, double size, bool bold, int align,
		uint32_t color, double alpha) {
	char buffer[512];
	launcher_set_font(cr, size, bold);
	launcher_ellipsize(cr, text, max_width, buffer, sizeof(buffer));
	launcher_set_color(cr, color, alpha);

	cairo_text_extents_t extents;
	cairo_text_extents(cr, buffer, &extents);
	if (align == 1) {
		x += (max_width - extents.x_advance) / 2.0;
	} else if (align == 2) {
		x += max_width - extents.x_advance;
	}
	cairo_move_to(cr, x, y);
	cairo_show_text(cr, buffer);
}

static uint32_t launcher_color_for_text(const char *text) {
	uint32_t hash = 2166136261u;
	for (const unsigned char *ptr = (const unsigned char *)text;
			*ptr != '\0'; ptr++) {
		hash ^= *ptr;
		hash *= 16777619u;
	}
	return launcher_palette[hash %
		(sizeof(launcher_palette) / sizeof(launcher_palette[0]))];
}

static void launcher_initials(const char *name, char out[3]) {
	int count = 0;
	bool new_word = true;

	for (const unsigned char *ptr = (const unsigned char *)name;
			*ptr != '\0' && count < 2; ptr++) {
		if (isalnum(*ptr)) {
			if (new_word) {
				out[count++] = (char)toupper(*ptr);
			}
			new_word = false;
		} else {
			new_word = true;
		}
	}
	if (count == 0) {
		out[count++] = '?';
	}
	out[count] = '\0';
}

/* ------------------------------------------------------------------------- */
/* Launcher application discovery                                            */
/* ------------------------------------------------------------------------- */

static void launcher_trim_end(char *text) {
	size_t len = strlen(text);
	while (len > 0 && isspace((unsigned char)text[len - 1])) {
		text[--len] = '\0';
	}
}

static void launcher_unescape(char *text) {
	char *out = text;
	for (char *in = text; *in != '\0'; in++) {
		if (*in == '\\' && in[1] != '\0') {
			in++;
			if (*in == 's') {
				*out++ = ' ';
			} else if (*in == 'n') {
				*out++ = '\n';
			} else if (*in == 't') {
				*out++ = '\t';
			} else {
				*out++ = *in;
			}
		} else {
			*out++ = *in;
		}
	}
	*out = '\0';
}

static bool launcher_icon_filename(const char *name, char *out, size_t outsz) {
	size_t len = strlen(name);
	if (len == 0) {
		return false;
	}
	if (len >= 4 && strcasecmp(name + len - 4, ".png") == 0) {
		snprintf(out, outsz, "%s", name);
		return true;
	}
	if ((len >= 4 && (strcasecmp(name + len - 4, ".svg") == 0 ||
			strcasecmp(name + len - 4, ".xpm") == 0)) ||
			(len >= 5 && strcasecmp(name + len - 5, ".svgz") == 0)) {
		return false;
	}
	int written = snprintf(out, outsz, "%s.png", name);
	return written > 0 && (size_t)written < outsz;
}

static bool launcher_icon_in_root(const char *root, const char *file,
		char *out, size_t outsz) {
	static const char *sizes[] = {
		"128x128", "256x256", "96x96", "64x64",
		"48x48", "192x192", "32x32", "16x16",
	};
	static const char *themes[] = {
		"icons/hicolor",
		"icons/oxygen/base",
	};

	for (size_t t = 0; t < sizeof(themes) / sizeof(themes[0]); t++) {
		for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
			int written = snprintf(out, outsz, "%s/%s/%s/apps/%s",
				root, themes[t], sizes[i], file);
			if (written > 0 && (size_t)written < outsz &&
					access(out, R_OK) == 0) {
				return true;
			}
		}
	}

	int written = snprintf(out, outsz, "%s/pixmaps/%s", root, file);
	if (written > 0 && (size_t)written < outsz &&
			access(out, R_OK) == 0) {
		return true;
	}
	return false;
}

static bool launcher_resolve_icon(const char *name, char *out, size_t outsz) {
	char file[512];
	if (!launcher_icon_filename(name, file, sizeof(file))) {
		return false;
	}

	if (strchr(file, '/') != NULL) {
		if (access(file, R_OK) != 0) {
			return false;
		}
		snprintf(out, outsz, "%s", file);
		return true;
	}

	const char *data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	char root[PATH_MAX];
	if (data_home != NULL && *data_home != '\0') {
		snprintf(root, sizeof(root), "%s", data_home);
		if (launcher_icon_in_root(root, file, out, outsz)) {
			return true;
		}
	} else if (home != NULL && *home != '\0') {
		snprintf(root, sizeof(root), "%s/.local/share", home);
		if (launcher_icon_in_root(root, file, out, outsz)) {
			return true;
		}
	}

	const char *data_dirs = getenv("XDG_DATA_DIRS");
	if (data_dirs == NULL || *data_dirs == '\0') {
		data_dirs = "/usr/local/share:/usr/share";
	}
	char dirs[8192];
	snprintf(dirs, sizeof(dirs), "%s", data_dirs);

	char *saveptr = NULL;
	for (char *dir = strtok_r(dirs, ":", &saveptr); dir != NULL;
			dir = strtok_r(NULL, ":", &saveptr)) {
		if (launcher_icon_in_root(dir, file, out, outsz)) {
			return true;
		}
	}
	return false;
}

static bool launcher_has_app_id(struct nide_server *server, const char *id) {
	struct nide_launcher *launcher = &server->launcher;
	for (int i = 0; i < launcher->napps; i++) {
		if (strcmp(launcher->apps[i].id, id) == 0) {
			return true;
		}
	}
	return false;
}

static void launcher_add_app(struct nide_server *server, const char *name,
		const char *exec, const char *id, bool terminal,
		const char *icon) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher_has_app_id(server, id)) {
		return;
	}

	if (launcher->napps >= launcher->appcap) {
		int capacity = launcher->appcap > 0 ?
			launcher->appcap * 2 : 64;
		struct nide_app *apps = realloc(launcher->apps,
			(size_t)capacity * sizeof(*apps));
		if (apps == NULL) {
			return;
		}
		launcher->apps = apps;
		launcher->appcap = capacity;
	}

	struct nide_app *app = &launcher->apps[launcher->napps++];
	snprintf(app->name, sizeof(app->name), "%s", name);
	snprintf(app->exec, sizeof(app->exec), "%s", exec);
	snprintf(app->id, sizeof(app->id), "%s", id);
	snprintf(app->icon, sizeof(app->icon), "%s",
		icon != NULL ? icon : "");
	app->terminal = terminal;
}

static void launcher_read_desktop(struct nide_server *server,
		const char *path, const char *id) {
	FILE *file = fopen(path, "r");
	if (file == NULL) {
		return;
	}

	char name[128] = {0};
	char exec[512] = {0};
	char type[32] = {0};
	char icon[256] = {0};
	bool in_entry = false;
	bool terminal = false;
	bool hidden = false;
	bool no_display = false;

	char line[1024];
	while (fgets(line, sizeof(line), file) != NULL) {
		launcher_trim_end(line);

		if (line[0] == '[') {
			if (in_entry) {
				break;
			}
			if (strcmp(line, "[Desktop Entry]") == 0) {
				in_entry = true;
			}
			continue;
		}
		if (!in_entry) {
			continue;
		}

		char *equals = strchr(line, '=');
		if (equals == NULL) {
			continue;
		}
		*equals = '\0';
		char *value = equals + 1;

		if (strcmp(line, "Name") == 0 && name[0] == '\0') {
			snprintf(name, sizeof(name), "%s", value);
		} else if (strcmp(line, "Exec") == 0 && exec[0] == '\0') {
			snprintf(exec, sizeof(exec), "%s", value);
		} else if (strcmp(line, "Type") == 0 && type[0] == '\0') {
			snprintf(type, sizeof(type), "%s", value);
		} else if (strcmp(line, "Icon") == 0 && icon[0] == '\0') {
			snprintf(icon, sizeof(icon), "%s", value);
		} else if (strcmp(line, "Terminal") == 0) {
			terminal = strcasecmp(value, "true") == 0;
		} else if (strcmp(line, "NoDisplay") == 0) {
			no_display = strcasecmp(value, "true") == 0;
		} else if (strcmp(line, "Hidden") == 0) {
			hidden = strcasecmp(value, "true") == 0;
		}
	}
	fclose(file);

	if (!in_entry || hidden || no_display ||
			name[0] == '\0' || exec[0] == '\0') {
		return;
	}
	if (type[0] != '\0' && strcmp(type, "Application") != 0) {
		return;
	}

	launcher_unescape(name);
	launcher_unescape(exec);
	launcher_unescape(icon);
	if (name[0] == '\0' || exec[0] == '\0') {
		return;
	}

	char icon_path[512] = {0};
	if (icon[0] != '\0') {
		launcher_resolve_icon(icon, icon_path, sizeof(icon_path));
	}
	launcher_add_app(server, name, exec, id, terminal, icon_path);
}

static void launcher_scan_dir(struct nide_server *server, const char *dir) {
	DIR *handle = opendir(dir);
	if (handle == NULL) {
		return;
	}

	struct dirent *entry;
	while ((entry = readdir(handle)) != NULL) {
		size_t len = strlen(entry->d_name);
		if (len <= 8 ||
				strcmp(entry->d_name + len - 8, ".desktop") != 0) {
			continue;
		}

		char path[PATH_MAX];
		int written = snprintf(path, sizeof(path), "%s/%s", dir,
			entry->d_name);
		if (written < 0 || (size_t)written >= sizeof(path)) {
			continue;
		}

		char id[128];
		size_t id_len = len - 8;
		if (id_len >= sizeof(id)) {
			continue;
		}
		memcpy(id, entry->d_name, id_len);
		id[id_len] = '\0';
		launcher_read_desktop(server, path, id);
	}
	closedir(handle);
}

static int launcher_app_compare(const void *left, const void *right) {
	const struct nide_app *a = left;
	const struct nide_app *b = right;
	return strcasecmp(a->name, b->name);
}

static void launcher_load_apps(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->apps_loaded) {
		return;
	}
	launcher->apps_loaded = true;

	const char *data_home = getenv("XDG_DATA_HOME");
	const char *home = getenv("HOME");
	char path[PATH_MAX];

	if (data_home != NULL && *data_home != '\0') {
		snprintf(path, sizeof(path), "%s/applications", data_home);
		launcher_scan_dir(server, path);
	} else if (home != NULL && *home != '\0') {
		snprintf(path, sizeof(path), "%s/.local/share/applications",
			home);
		launcher_scan_dir(server, path);
	}

	const char *data_dirs = getenv("XDG_DATA_DIRS");
	if (data_dirs == NULL || *data_dirs == '\0') {
		data_dirs = "/usr/local/share:/usr/share";
	}
	char dirs[8192];
	snprintf(dirs, sizeof(dirs), "%s", data_dirs);

	char *saveptr = NULL;
	for (char *dir = strtok_r(dirs, ":", &saveptr); dir != NULL;
			dir = strtok_r(NULL, ":", &saveptr)) {
		snprintf(path, sizeof(path), "%s/applications", dir);
		launcher_scan_dir(server, path);
	}

	qsort(launcher->apps, (size_t)launcher->napps,
		sizeof(*launcher->apps), launcher_app_compare);
	wlr_log(WLR_INFO, "launcher: loaded %d applications",
		launcher->napps);
}

/* ------------------------------------------------------------------------- */
/* Launcher items and filtering                                              */
/* ------------------------------------------------------------------------- */

static bool launcher_app_id_matches(const char *app_id,
		const char *desktop_id) {
	if (app_id == NULL || *app_id == '\0' ||
			desktop_id == NULL || *desktop_id == '\0') {
		return false;
	}
	if (strcasecmp(app_id, desktop_id) == 0) {
		return true;
	}

	const char *dot = strrchr(desktop_id, '.');
	if (dot != NULL && strcasecmp(app_id, dot + 1) == 0) {
		return true;
	}
	dot = strrchr(app_id, '.');
	if (dot != NULL && strcasecmp(dot + 1, desktop_id) == 0) {
		return true;
	}
	return false;
}

static struct nide_app *launcher_find_app(struct nide_server *server,
		const char *app_id) {
	struct nide_launcher *launcher = &server->launcher;
	for (int i = 0; i < launcher->napps; i++) {
		if (launcher_app_id_matches(app_id,
				launcher->apps[i].id)) {
			return &launcher->apps[i];
		}
	}
	return NULL;
}

static int launcher_count_app(struct nide_server *server,
		const char *desktop_id) {
	int count = 0;
	struct nide_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->mapped &&
				launcher_app_id_matches(
					toplevel->xdg_toplevel->app_id,
					desktop_id)) {
			count++;
		}
	}
	return count;
}

static void launcher_friendly_name(const char *app_id,
		char *out, size_t outsz) {
	const char *base = app_id;
	if (app_id != NULL) {
		const char *dot = strrchr(app_id, '.');
		if (dot != NULL && dot[1] != '\0') {
			base = dot + 1;
		}
	}
	if (base == NULL || *base == '\0') {
		snprintf(out, outsz, "Window");
		return;
	}

	snprintf(out, outsz, "%s", base);
	for (char *ptr = out; *ptr != '\0'; ptr++) {
		if (*ptr == '-' || *ptr == '_') {
			*ptr = ' ';
		}
	}
	if (out[0] >= 'a' && out[0] <= 'z') {
		out[0] = (char)(out[0] - 'a' + 'A');
	}
}

static void launcher_reserve_items(struct nide_server *server, int count) {
	struct nide_launcher *launcher = &server->launcher;
	if (count <= launcher->itemcap) {
		return;
	}

	int capacity = launcher->itemcap > 0 ? launcher->itemcap : 32;
	while (capacity < count) {
		capacity *= 2;
	}
	struct nide_launcher_item *items = realloc(launcher->items,
		(size_t)capacity * sizeof(*items));
	if (items == NULL) {
		return;
	}
	launcher->items = items;
	launcher->itemcap = capacity;
}

static void launcher_add_item(struct nide_server *server,
		const char *name, const char *detail, const char *exec,
		const char *app_id, const char *icon, int count, bool terminal,
		struct nide_toplevel *toplevel) {
	struct nide_launcher *launcher = &server->launcher;
	launcher_reserve_items(server, launcher->nitems + 1);
	if (launcher->nitems >= launcher->itemcap) {
		return;
	}

	struct nide_launcher_item *item =
		&launcher->items[launcher->nitems++];
	snprintf(item->name, sizeof(item->name), "%s", name);
	snprintf(item->detail, sizeof(item->detail), "%s",
		detail != NULL ? detail : "");
	snprintf(item->exec, sizeof(item->exec), "%s",
		exec != NULL ? exec : "");
	snprintf(item->app_id, sizeof(item->app_id), "%s",
		app_id != NULL ? app_id : "");
	snprintf(item->icon, sizeof(item->icon), "%s",
		icon != NULL ? icon : "");
	item->count = count;
	item->terminal = terminal;
	item->toplevel = toplevel;
}

static void launcher_filter(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	launcher->nfiltered = 0;

	if (launcher->nitems > launcher->filtercap) {
		int capacity = launcher->filtercap > 0 ?
			launcher->filtercap : 32;
		while (capacity < launcher->nitems) {
			capacity *= 2;
		}
		int *filtered = realloc(launcher->filtered,
			(size_t)capacity * sizeof(*filtered));
		if (filtered == NULL) {
			return;
		}
		launcher->filtered = filtered;
		launcher->filtercap = capacity;
	}

	for (int i = 0; i < launcher->nitems; i++) {
		struct nide_launcher_item *item = &launcher->items[i];
		bool match = launcher->query[0] == '\0' ||
			strcasestr(item->name, launcher->query) != NULL ||
			strcasestr(item->detail, launcher->query) != NULL ||
			strcasestr(item->app_id, launcher->query) != NULL;
		if (match) {
			launcher->filtered[launcher->nfiltered++] = i;
		}
	}

	launcher->scroll = 0;
	launcher->selected = 0;
}

static int launcher_total_rows(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	return (launcher->nfiltered + NIDE_LAUNCHER_COLS - 1) /
		NIDE_LAUNCHER_COLS;
}

static int launcher_max_scroll(struct nide_server *server) {
	int rows = launcher_total_rows(server);
	int max_scroll = rows - NIDE_LAUNCHER_ROWS;
	return max_scroll > 0 ? max_scroll : 0;
}

static void launcher_clamp_scroll(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	launcher->scroll = launcher_clamp_int(launcher->scroll, 0,
		launcher_max_scroll(server));
}

static void launcher_ensure_selected_visible(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->nfiltered <= 0) {
		launcher->scroll = 0;
		return;
	}

	int row = launcher->selected / NIDE_LAUNCHER_COLS;
	if (row < launcher->scroll) {
		launcher->scroll = row;
	} else if (row >= launcher->scroll + NIDE_LAUNCHER_ROWS) {
		launcher->scroll = row - NIDE_LAUNCHER_ROWS + 1;
	}
	launcher_clamp_scroll(server);
}

static void launcher_keep_selection_visible(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->nfiltered <= 0) {
		launcher->selected = 0;
		launcher->scroll = 0;
		return;
	}

	int first = launcher->scroll * NIDE_LAUNCHER_COLS;
	int last = first + NIDE_LAUNCHER_PAGE_SIZE - 1;
	if (last >= launcher->nfiltered) {
		last = launcher->nfiltered - 1;
	}
	if (launcher->selected < first) {
		launcher->selected = first;
	} else if (launcher->selected > last) {
		launcher->selected = last;
	}
}

static void launcher_select(struct nide_server *server, int index) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->nfiltered <= 0) {
		launcher->selected = 0;
		launcher->scroll = 0;
		return;
	}

	index = launcher_clamp_int(index, 0, launcher->nfiltered - 1);
	launcher->selected = index;
	launcher_ensure_selected_visible(server);
}

static void launcher_move_selection(struct nide_server *server,
		int dx, int dy) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->nfiltered <= 0) {
		return;
	}

	int index = launcher->selected;
	if (dx != 0) {
		index += dx;
		if (index < 0) {
			index = launcher->nfiltered - 1;
		} else if (index >= launcher->nfiltered) {
			index = 0;
		}
	}
	if (dy != 0) {
		index += dy * NIDE_LAUNCHER_COLS;
		index = launcher_clamp_int(index, 0, launcher->nfiltered - 1);
	}

	if (index != launcher->selected) {
		launcher_select(server, index);
		launcher_render(server);
	}
}

static void launcher_scroll_to(struct nide_server *server, int row) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->nfiltered <= 0) {
		return;
	}

	launcher->scroll = row;
	launcher_clamp_scroll(server);
	launcher_keep_selection_visible(server);
	launcher_render(server);
}

static void launcher_scroll_by(struct nide_server *server, int delta) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->nfiltered <= 0 || delta == 0) {
		return;
	}
	launcher_scroll_to(server, launcher->scroll + delta);
}

static int launcher_item_compare(const void *left, const void *right) {
	const struct nide_launcher_item *a = left;
	const struct nide_launcher_item *b = right;
	int compare = strcasecmp(a->name, b->name);
	if (compare != 0) {
		return compare;
	}
	compare = strcasecmp(a->detail, b->detail);
	if (compare != 0) {
		return compare;
	}
	return strcasecmp(a->app_id, b->app_id);
}

static void launcher_build_items(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	launcher->nitems = 0;

	if (launcher->mode == NIDE_LAUNCHER_WINDOWS) {
		struct nide_toplevel *toplevel;
		wl_list_for_each(toplevel, &server->toplevels, link) {
			if (!toplevel->mapped) {
				continue;
			}

			const char *app_id =
				toplevel->xdg_toplevel->app_id;
			const char *title =
				toplevel->xdg_toplevel->title;
			struct nide_app *app =
				launcher_find_app(server, app_id);

			char name[128];
			if (app != NULL) {
				snprintf(name, sizeof(name), "%s",
					app->name);
			} else {
				launcher_friendly_name(app_id, name,
					sizeof(name));
			}

			launcher_add_item(server, name,
				title != NULL ? title : "Untitled window",
				NULL, app_id,
				app != NULL ? app->icon : NULL,
				0, false, toplevel);
		}
	} else {
		for (int i = 0; i < launcher->napps; i++) {
			struct nide_app *app = &launcher->apps[i];
			launcher_add_item(server, app->name, "",
				app->exec, app->id, app->icon,
				launcher_count_app(server, app->id),
				app->terminal, NULL);
		}
	}

	qsort(launcher->items, (size_t)launcher->nitems,
		sizeof(*launcher->items), launcher_item_compare);
	launcher_filter(server);
}

static void launcher_set_mode(struct nide_server *server,
		enum nide_launcher_mode mode) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->mode == mode) {
		return;
	}

	launcher->mode = mode;
	launcher->scroll = 0;
	launcher->selected = 0;
	launcher->scrollbar_dragging = false;
	launcher_build_items(server);
	launcher_render(server);
}

/* ------------------------------------------------------------------------- */
/* Launcher layout and rendering                                             */
/* ------------------------------------------------------------------------- */

static struct wlr_box launcher_output_box(struct nide_server *server) {
	struct wlr_output *output = wlr_output_layout_output_at(
		server->output_layout, server->cursor->x, server->cursor->y);
	if (output == NULL && server->focused != NULL) {
		struct wlr_box geo = toplevel_geometry(server->focused);
		output = wlr_output_layout_output_at(server->output_layout,
			geo.x + geo.width / 2.0, geo.y + geo.height / 2.0);
	}
	if (output == NULL && !wl_list_empty(&server->outputs)) {
		struct nide_output *first =
			wl_container_of(server->outputs.next, first, link);
		output = first->wlr_output;
	}

	struct wlr_box box = {0};
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

static void launcher_layout(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	struct wlr_box output = launcher_output_box(server);
	launcher->output = output;

	int panel_width = output.width - 64;
	if (panel_width > 760) {
		panel_width = 760;
	}
	if (panel_width < 300) {
		panel_width = output.width - 16;
	}
	panel_width = launcher_clamp_int(panel_width, 1, output.width);

	int panel_height = output.height - 64;
	if (panel_height > 540) {
		panel_height = 540;
	}
	if (panel_height < 260) {
		panel_height = output.height - 16;
	}
	panel_height = launcher_clamp_int(panel_height, 1, output.height);

	launcher->panel = (struct wlr_box) {
		.x = output.x + (output.width - panel_width) / 2,
		.y = output.y + (output.height - panel_height) / 2,
		.width = panel_width,
		.height = panel_height,
	};

	int pad = panel_width < 560 ? 14 : 24;
	int search_height = panel_height < 420 ? 40 : 52;
	int tabs_width = panel_width < 640 ? 176 : 290;
	if (panel_width < 420) {
		tabs_width = 150;
	}
	int tab_gap = 8;
	int tab_width = (tabs_width - tab_gap) / 2;
	int search_width = panel_width - pad * 2 - tabs_width - 12;
	if (search_width < 90) {
		search_width = panel_width - pad * 2;
	}

	launcher->search = (struct wlr_box) {
		.x = pad, .y = pad,
		.width = search_width, .height = search_height,
	};
	launcher->tabs[0] = (struct wlr_box) {
		.x = panel_width - pad - tabs_width,
		.y = pad, .width = tab_width, .height = search_height,
	};
	launcher->tabs[1] = (struct wlr_box) {
		.x = launcher->tabs[0].x + tab_width + tab_gap,
		.y = pad, .width = tab_width, .height = search_height,
	};

	int grid_top = pad + search_height +
		(panel_height < 420 ? 12 : 22);
	int grid_height = panel_height - grid_top - pad;
	if (grid_height < 40) {
		grid_height = 40;
	}
	int grid_limit = panel_height - grid_top - 8;
	if (grid_limit < 1) {
		grid_limit = 1;
	}
	if (grid_height > grid_limit) {
		grid_height = grid_limit;
	}
	int gap = panel_width < 600 ? 8 : 12;
	int grid_width = panel_width - pad * 2 - 22;
	if (grid_width < 1) {
		grid_width = 1;
	}

	launcher->grid = (struct wlr_box) {
		.x = pad, .y = grid_top,
		.width = grid_width, .height = grid_height,
	};
	launcher->gap = gap;
	launcher->tile_w = (launcher->grid.width -
		(NIDE_LAUNCHER_COLS - 1) * gap) / NIDE_LAUNCHER_COLS;
	launcher->tile_h = (launcher->grid.height -
		(NIDE_LAUNCHER_ROWS - 1) * gap) / NIDE_LAUNCHER_ROWS;
	if (launcher->tile_w < 1) {
		launcher->tile_w = 1;
	}
	if (launcher->tile_h < 1) {
		launcher->tile_h = 1;
	}

	int icon_size = launcher->tile_w - 20;
	if (icon_size > launcher->tile_h - 36) {
		icon_size = launcher->tile_h - 36;
	}
	if (icon_size > 76) {
		icon_size = 76;
	}
	if (icon_size < 24) {
		icon_size = 24;
	}
	launcher->icon_size = icon_size;

	int scrollbar_x = panel_width - pad - 14;
	launcher->scrollbar = (struct wlr_box) {
		.x = scrollbar_x, .y = grid_top,
		.width = 14, .height = grid_height,
	};
	launcher->scroll_track = (struct wlr_box) {
		.x = launcher->scrollbar.x + 5, .y = grid_top,
		.width = 4, .height = grid_height,
	};
}

static void launcher_free_icons(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	for (int i = 0; i < launcher->nicons; i++) {
		if (launcher->icons[i].surface != NULL) {
			cairo_surface_destroy(launcher->icons[i].surface);
		}
		launcher->icons[i].surface = NULL;
	}
	launcher->nicons = 0;
	launcher->icon_tick = 0;
}

static cairo_surface_t *launcher_icon_cache_get(struct nide_server *server,
		const char *path) {
	struct nide_launcher *launcher = &server->launcher;
	if (path == NULL || *path == '\0') {
		return NULL;
	}

	for (int i = 0; i < launcher->nicons; i++) {
		if (strcmp(launcher->icons[i].path, path) == 0) {
			launcher->icons[i].used = ++launcher->icon_tick;
			return launcher->icons[i].surface;
		}
	}

	cairo_surface_t *surface = cairo_image_surface_create_from_png(path);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		surface = NULL;
	}

	int slot = -1;
	if (launcher->nicons < NIDE_LAUNCHER_ICON_CACHE) {
		slot = launcher->nicons++;
	} else {
		slot = 0;
		for (int i = 1; i < launcher->nicons; i++) {
			if (launcher->icons[i].used <
					launcher->icons[slot].used) {
				slot = i;
			}
		}
		if (launcher->icons[slot].surface != NULL) {
			cairo_surface_destroy(launcher->icons[slot].surface);
		}
	}

	struct nide_launcher_icon *entry = &launcher->icons[slot];
	snprintf(entry->path, sizeof(entry->path), "%s", path);
	entry->surface = surface;
	entry->used = ++launcher->icon_tick;
	return surface;
}

static bool launcher_scrollbar_thumb(struct nide_server *server,
		struct wlr_box *thumb) {
	struct nide_launcher *launcher = &server->launcher;
	int rows = launcher_total_rows(server);
	if (rows <= NIDE_LAUNCHER_ROWS) {
		thumb->width = 0;
		thumb->height = 0;
		return false;
	}

	struct wlr_box track = launcher->scroll_track;
	int height = (int)((double)track.height *
		NIDE_LAUNCHER_ROWS / rows);
	if (height < 28) {
		height = 28;
	}
	if (height > track.height) {
		height = track.height;
	}
	int max_scroll = launcher_max_scroll(server);
	double position = max_scroll > 0 ?
		(double)launcher->scroll / max_scroll : 0.0;

	*thumb = (struct wlr_box) {
		.x = track.x, .y = track.y +
			(int)((track.height - height) * position),
		.width = track.width, .height = height,
	};
	return true;
}

static void launcher_draw_item(cairo_t *cr, struct nide_server *server,
		struct nide_launcher_item *item, int slot, bool selected) {
	struct nide_launcher *launcher = &server->launcher;
	int column = slot % NIDE_LAUNCHER_COLS;
	int row = slot / NIDE_LAUNCHER_COLS;
	double x = launcher->grid.x +
		column * (launcher->tile_w + launcher->gap);
	double y = launcher->grid.y +
		row * (launcher->tile_h + launcher->gap);
	double width = launcher->tile_w;
	double icon_size = launcher->icon_size;
	double icon_x = x + (width - icon_size) / 2.0;
	double icon_y = y + 8;
	double icon_cx = icon_x + icon_size / 2.0;
	double icon_cy = icon_y + icon_size / 2.0;

	if (selected) {
		cairo_pattern_t *glow = cairo_pattern_create_radial(
			icon_cx, icon_cy, icon_size * 0.24,
			icon_cx, icon_cy, icon_size * 0.86);
		cairo_pattern_add_color_stop_rgba(glow, 0.0,
			0.42, 0.55, 1.0, 0.30);
		cairo_pattern_add_color_stop_rgba(glow, 1.0,
			0.42, 0.55, 1.0, 0.0);
		cairo_set_source(cr, glow);
		cairo_new_sub_path(cr);
		cairo_arc(cr, icon_cx, icon_cy, icon_size * 0.86,
			0, 2 * M_PI);
		cairo_fill(cr);
		cairo_pattern_destroy(glow);
	}

	cairo_surface_t *surface =
		launcher_icon_cache_get(server, item->icon);
	cairo_text_extents_t extents;
	if (surface != NULL) {
		int source_width = cairo_image_surface_get_width(surface);
		int source_height = cairo_image_surface_get_height(surface);
		int source_max = source_width > source_height ?
			source_width : source_height;
		if (source_max > 0) {
			double scale = icon_size / (double)source_max;
			double draw_width = source_width * scale;
			double draw_height = source_height * scale;
			cairo_save(cr);
			cairo_translate(cr, icon_cx - draw_width / 2.0,
				icon_cy - draw_height / 2.0);
			cairo_scale(cr, scale, scale);
			cairo_set_source_surface(cr, surface, 0, 0);
			cairo_pattern_set_filter(cairo_get_source(cr),
				CAIRO_FILTER_GOOD);
			cairo_paint(cr);
			cairo_restore(cr);
		}
	} else {
		cairo_new_sub_path(cr);
		cairo_arc(cr, icon_cx, icon_cy, icon_size / 2.0,
			0, 2 * M_PI);
		launcher_set_color(cr, launcher_color_for_text(item->name),
			0.92);
		cairo_fill(cr);

		char initials[3];
		launcher_initials(item->name, initials);
		launcher_set_font(cr, icon_size * 0.32, true);
		launcher_set_color(cr, 0xffffff, 0.96);
		cairo_text_extents(cr, initials, &extents);
		cairo_move_to(cr,
			icon_cx - extents.x_advance / 2.0,
			icon_cy - extents.height / 2.0 -
				extents.y_bearing);
		cairo_show_text(cr, initials);
	}

	if (launcher->mode == NIDE_LAUNCHER_APPS && item->count == 1) {
		double dot_x = icon_x + icon_size - 3;
		double dot_y = icon_y + 3;
		cairo_new_sub_path(cr);
		cairo_arc(cr, dot_x, dot_y, 5.0, 0, 2 * M_PI);
		launcher_set_color(cr, 0x0b1220, 0.85);
		cairo_fill(cr);
		cairo_new_sub_path(cr);
		cairo_arc(cr, dot_x, dot_y, 3.2, 0, 2 * M_PI);
		launcher_set_color(cr, 0x6c8fff, 0.95);
		cairo_fill(cr);
	} else if (launcher->mode == NIDE_LAUNCHER_APPS && item->count > 1) {
		double badge_x = icon_x + icon_size - 3;
		double badge_y = icon_y + 3;
		double badge_radius = item->count > 99 ? 11.0 : 9.0;
		cairo_new_sub_path(cr);
		cairo_arc(cr, badge_x, badge_y, badge_radius, 0, 2 * M_PI);
		launcher_set_color(cr, 0x0b1220, 0.92);
		cairo_fill_preserve(cr);
		launcher_set_color(cr, 0x6c8fff, 0.85);
		cairo_set_line_width(cr, 1.0);
		cairo_stroke(cr);

		char count_text[16];
		if (item->count > 99) {
			snprintf(count_text, sizeof(count_text), "99+");
		} else {
			snprintf(count_text, sizeof(count_text), "%d",
				item->count);
		}
		launcher_set_font(cr, item->count > 99 ? 8 : 9, true);
		cairo_text_extents(cr, count_text, &extents);
		launcher_set_color(cr, 0xffffff, 0.98);
		cairo_move_to(cr, badge_x - extents.x_advance / 2.0,
			badge_y - extents.height / 2.0 - extents.y_bearing);
		cairo_show_text(cr, count_text);
	}

	double name_y = icon_y + icon_size + 17;
	launcher_draw_text(cr, item->name, x + 4, name_y,
		width - 8, 12, false, 1,
		selected ? 0xffffff : 0xd6e0ee,
		selected ? 0.98 : 0.82);
}

static void launcher_render(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	launcher_layout(server);

	int width = launcher->panel.width;
	int height = launcher->panel.height;
	if (width < 1 || height < 1) {
		return;
	}

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return;
	}
	cairo_t *cr = cairo_create(surface);
	if (cairo_status(cr) != CAIRO_STATUS_SUCCESS) {
		cairo_destroy(cr);
		cairo_surface_destroy(surface);
		return;
	}

	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0, 0, 0, 0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	launcher_rounded_rect(cr, 0, 0, width, height, 18);
	cairo_pattern_t *panel_gradient =
		cairo_pattern_create_linear(0, 0, 0, height);
	cairo_pattern_add_color_stop_rgba(panel_gradient, 0.0,
		0.12, 0.17, 0.26, 0.91);
	cairo_pattern_add_color_stop_rgba(panel_gradient, 1.0,
		0.04, 0.07, 0.12, 0.87);
	cairo_set_source(cr, panel_gradient);
	cairo_fill_preserve(cr);
	cairo_pattern_destroy(panel_gradient);
	launcher_set_color(cr, 0xffffff, 0.14);
	cairo_set_line_width(cr, 1.2);
	cairo_stroke(cr);

	launcher_rounded_rect(cr, launcher->search.x, launcher->search.y,
		launcher->search.width, launcher->search.height, 14);
	launcher_set_color(cr, 0xffffff, 0.06);
	cairo_fill_preserve(cr);
	launcher_set_color(cr, 0xffffff, 0.12);
	cairo_set_line_width(cr, 1.0);
	cairo_stroke(cr);

	double search_center_y =
		launcher->search.y + launcher->search.height / 2.0;
	cairo_new_sub_path(cr);
	cairo_arc(cr, launcher->search.x + 21, search_center_y, 6.5,
		-M_PI / 2.0, 2 * M_PI);
	launcher_set_color(cr, 0xc3cfe0, 0.72);
	cairo_set_line_width(cr, 1.8);
	cairo_stroke(cr);
	cairo_move_to(cr, launcher->search.x + 26,
		search_center_y + 5);
	cairo_line_to(cr, launcher->search.x + 31,
		search_center_y + 10);
	cairo_stroke(cr);

	double text_x = launcher->search.x + 40;
	double text_baseline = search_center_y + 5.5;
	bool has_query = launcher->query[0] != '\0';
	launcher_draw_text(cr,
		has_query ? launcher->query : "Search apps or windows",
		text_x, text_baseline,
		launcher->search.width - 56, 15, false, 0,
		has_query ? 0xf1f6ff : 0xc3cfe0,
		has_query ? 0.96 : 0.48);

	launcher_set_font(cr, 15, false);
	cairo_text_extents_t query_extents;
	cairo_text_extents(cr, launcher->query, &query_extents);
	double caret_x = text_x + query_extents.x_advance + 2;
	if (caret_x > launcher->search.x + launcher->search.width - 14) {
		caret_x = launcher->search.x + launcher->search.width - 14;
	}
	cairo_move_to(cr, caret_x, text_baseline - 13);
	cairo_line_to(cr, caret_x, text_baseline + 4);
	launcher_set_color(cr, 0x6c8fff, 0.95);
	cairo_set_line_width(cr, 1.5);
	cairo_stroke(cr);

	for (int i = 0; i < 2; i++) {
		bool active = (i == 0 &&
			launcher->mode == NIDE_LAUNCHER_WINDOWS) ||
			(i == 1 && launcher->mode == NIDE_LAUNCHER_APPS);
		struct wlr_box tab = launcher->tabs[i];
		launcher_rounded_rect(cr, tab.x, tab.y, tab.width, tab.height,
			tab.height / 2.0);
		launcher_set_color(cr, active ? 0x4f7cff : 0xffffff,
			active ? 0.52 : 0.05);
		cairo_fill_preserve(cr);
		launcher_set_color(cr, 0xffffff, active ? 0.24 : 0.09);
		cairo_set_line_width(cr, 1.0);
		cairo_stroke(cr);

		const char *label = i == 0 ? "Open Windows" : "All Apps";
		launcher_draw_text(cr, label, tab.x + 6,
			tab.y + tab.height / 2.0 + 4,
			tab.width - 12, 11, true, 1,
			active ? 0xffffff : 0xc2cee0,
			active ? 0.98 : 0.82);
	}

	int first_index = launcher->scroll * NIDE_LAUNCHER_COLS;
	for (int slot = 0; slot < NIDE_LAUNCHER_PAGE_SIZE; slot++) {
		int index = first_index + slot;
		if (index >= launcher->nfiltered) {
			break;
		}
		struct nide_launcher_item *item =
			&launcher->items[launcher->filtered[index]];
		launcher_draw_item(cr, server, item, slot,
			index == launcher->selected);
	}

	if (launcher->nfiltered == 0) {
		launcher_draw_text(cr, "No apps or windows found",
			launcher->grid.x, launcher->grid.y +
				launcher->grid.height / 2.0 + 5,
			launcher->grid.width, 16, false, 1,
			0xc3cfe0, 0.62);
	}

	struct wlr_box thumb;
	if (launcher_scrollbar_thumb(server, &thumb)) {
		launcher_rounded_rect(cr, launcher->scroll_track.x,
			launcher->scroll_track.y,
			launcher->scroll_track.width,
			launcher->scroll_track.height,
			launcher->scroll_track.width / 2.0);
		launcher_set_color(cr, 0xffffff, 0.07);
		cairo_fill(cr);

		launcher_rounded_rect(cr, thumb.x, thumb.y,
			thumb.width, thumb.height, thumb.width / 2.0);
		launcher_set_color(cr, 0xa9bcff, 0.58);
		cairo_fill(cr);
	}

	cairo_destroy(cr);

	struct wlr_buffer *buffer = nide_cairo_buffer_create(surface);
	if (buffer == NULL) {
		cairo_surface_destroy(surface);
		return;
	}

	wlr_scene_buffer_set_buffer(server->launcher_scene_buffer, buffer);
	wlr_buffer_drop(buffer);

	wlr_scene_node_set_position(&server->launcher_scene_buffer->node,
		launcher->panel.x, launcher->panel.y);
	wlr_scene_node_set_enabled(&server->launcher_tree->node, true);
	wlr_scene_node_raise_to_top(&server->launcher_tree->node);
}

/* ------------------------------------------------------------------------- */
/* Launcher actions                                                          */
/* ------------------------------------------------------------------------- */

static void launcher_free_items(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	free(launcher->items);
	launcher->items = NULL;
	launcher->nitems = 0;
	launcher->itemcap = 0;
	free(launcher->filtered);
	launcher->filtered = NULL;
	launcher->nfiltered = 0;
	launcher->filtercap = 0;
}

static bool launcher_has_windows(struct nide_server *server) {
	struct nide_toplevel *toplevel;
	wl_list_for_each(toplevel, &server->toplevels, link) {
		if (toplevel->mapped) {
			return true;
		}
	}
	return false;
}

static void launcher_init(struct nide_server *server) {
	server->launcher_tree = wlr_scene_tree_create(&server->scene->tree);
	server->launcher_scene_buffer = wlr_scene_buffer_create(
		server->launcher_tree, NULL);
	wlr_scene_node_set_enabled(&server->launcher_tree->node, false);
}

static void launcher_open(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->open) {
		launcher_close(server);
		return;
	}

	launcher_load_apps(server);
	launcher->open = true;
	launcher->query[0] = '\0';
	launcher->scroll = 0;
	launcher->selected = 0;
	launcher->scrollbar_dragging = false;
	launcher->mode = launcher_has_windows(server) ?
		NIDE_LAUNCHER_WINDOWS : NIDE_LAUNCHER_APPS;

	if (server->cursor_mode != NIDE_CURSOR_PASSTHROUGH) {
		reset_cursor_mode(server);
	}

	launcher_build_items(server);
	wlr_seat_pointer_clear_focus(server->seat);
	wlr_seat_keyboard_notify_clear_focus(server->seat);
	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
		"default");
	launcher_render(server);
	wlr_log(WLR_DEBUG, "launcher opened (%d items)",
		launcher->nfiltered);
}

static void launcher_close(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	if (!launcher->open) {
		return;
	}

	launcher->open = false;
	launcher->scrollbar_dragging = false;
	wlr_scene_node_set_enabled(&server->launcher_tree->node, false);
	wlr_scene_buffer_set_buffer(server->launcher_scene_buffer, NULL);
	launcher_free_items(server);
	launcher_free_icons(server);

	if (server->focused != NULL && server->focused->mapped) {
		struct wlr_keyboard *keyboard =
			wlr_seat_get_keyboard(server->seat);
		if (keyboard != NULL) {
			wlr_seat_keyboard_notify_enter(server->seat,
				server->focused->xdg_toplevel->base->surface,
				keyboard->keycodes, keyboard->num_keycodes,
				&keyboard->modifiers);
		}
	}
	process_cursor_motion(server, 0);
	wlr_log(WLR_DEBUG, "launcher closed");
}

static void launcher_activate_selected(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	if (launcher->nfiltered == 0) {
		return;
	}

	struct nide_launcher_item *item =
		&launcher->items[launcher->filtered[launcher->selected]];
	if (launcher->mode == NIDE_LAUNCHER_WINDOWS &&
			item->toplevel != NULL) {
		struct nide_toplevel *toplevel = item->toplevel;
		launcher_close(server);
		focus_toplevel(toplevel);
		return;
	}

	if (item->exec[0] == '\0') {
		return;
	}

	char command[1024];
	char clean[768];
	size_t clean_len = 0;
	for (size_t i = 0; item->exec[i] != '\0' &&
			clean_len + 1 < sizeof(clean); i++) {
		if (item->exec[i] == '%' && item->exec[i + 1] != '\0') {
			i++;
			if (item->exec[i] == '%') {
				clean[clean_len++] = '%';
			}
			continue;
		}
		clean[clean_len++] = item->exec[i];
	}
	clean[clean_len] = '\0';

	char *start = clean;
	while (*start == ' ') {
		start++;
	}
	size_t start_len = strlen(start);
	while (start_len > 0 && start[start_len - 1] == ' ') {
		start[--start_len] = '\0';
	}
	if (start[0] == '\0') {
		return;
	}

	if (item->terminal) {
		snprintf(command, sizeof(command), "foot -e %s", start);
	} else {
		snprintf(command, sizeof(command), "%s", start);
	}

	launcher_close(server);
	spawn_command(command);
}

static bool launcher_is_toggle_combo(struct nide_server *server,
		xkb_keysym_t sym, uint32_t modifiers) {
	uint32_t mods = modifiers & NIDE_MOD_MASK;
	for (int i = 0; i < server->config.nbinds; i++) {
		struct nide_bind *bind = &server->config.binds[i];
		if (bind->action != NIDE_ACTION_LAUNCHER) {
			continue;
		}
		if (bind->mods == mods &&
				xkb_keysym_to_lower(bind->sym) ==
				xkb_keysym_to_lower(sym)) {
			return true;
		}
	}
	return mods == WLR_MODIFIER_LOGO && sym == XKB_KEY_Tab;
}

static void launcher_query_backspace(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	size_t len = strlen(launcher->query);
	while (len > 0) {
		len--;
		if (((unsigned char)launcher->query[len] & 0xc0) != 0x80) {
			break;
		}
	}
	launcher->query[len] = '\0';
}

static bool launcher_query_append(struct nide_server *server,
		uint32_t codepoint) {
	struct nide_launcher *launcher = &server->launcher;
	char encoded[4];
	size_t length = 0;

	if (codepoint < 0x80) {
		encoded[length++] = (char)codepoint;
	} else if (codepoint < 0x800) {
		encoded[length++] = (char)(0xc0 | (codepoint >> 6));
		encoded[length++] = (char)(0x80 | (codepoint & 0x3f));
	} else if (codepoint < 0x10000) {
		encoded[length++] = (char)(0xe0 | (codepoint >> 12));
		encoded[length++] = (char)(0x80 |
			((codepoint >> 6) & 0x3f));
		encoded[length++] = (char)(0x80 | (codepoint & 0x3f));
	} else {
		encoded[length++] = (char)(0xf0 | (codepoint >> 18));
		encoded[length++] = (char)(0x80 |
			((codepoint >> 12) & 0x3f));
		encoded[length++] = (char)(0x80 |
			((codepoint >> 6) & 0x3f));
		encoded[length++] = (char)(0x80 | (codepoint & 0x3f));
	}

	size_t current = strlen(launcher->query);
	if (current + length > NIDE_LAUNCHER_QUERY_MAX) {
		return false;
	}
	memcpy(launcher->query + current, encoded, length);
	launcher->query[current + length] = '\0';
	return true;
}

static void launcher_handle_key(struct nide_server *server,
		struct nide_keyboard *keyboard,
		struct wlr_keyboard_key_event *event) {
	if (event->state != WL_KEYBOARD_KEY_STATE_PRESSED) {
		return;
	}

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms = NULL;
	int nsyms = xkb_state_key_get_syms(
		keyboard->wlr_keyboard->xkb_state, keycode, &syms);
	if (nsyms <= 0) {
		return;
	}
	xkb_keysym_t sym = syms[0];
	uint32_t modifiers =
		wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

	if (launcher_is_toggle_combo(server, sym, modifiers)) {
		launcher_close(server);
		return;
	}

	switch (sym) {
	case XKB_KEY_Escape:
		launcher_close(server);
		return;
	case XKB_KEY_Return:
	case XKB_KEY_KP_Enter:
		launcher_activate_selected(server);
		return;
	case XKB_KEY_Tab:
		launcher_set_mode(server,
			server->launcher.mode == NIDE_LAUNCHER_WINDOWS ?
				NIDE_LAUNCHER_APPS : NIDE_LAUNCHER_WINDOWS);
		return;
	case XKB_KEY_BackSpace:
		launcher_query_backspace(server);
		launcher_filter(server);
		launcher_render(server);
		return;
	case XKB_KEY_Left:
		launcher_move_selection(server, -1, 0);
		return;
	case XKB_KEY_Right:
		launcher_move_selection(server, 1, 0);
		return;
	case XKB_KEY_Up:
		launcher_move_selection(server, 0, -1);
		return;
	case XKB_KEY_Down:
		launcher_move_selection(server, 0, 1);
		return;
	case XKB_KEY_Page_Up:
		launcher_scroll_by(server, -NIDE_LAUNCHER_ROWS);
		return;
	case XKB_KEY_Page_Down:
		launcher_scroll_by(server, NIDE_LAUNCHER_ROWS);
		return;
	default:
		break;
	}

	if ((modifiers & (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT |
			WLR_MODIFIER_LOGO)) != 0) {
		return;
	}

	uint32_t codepoint = xkb_state_key_get_utf32(
		keyboard->wlr_keyboard->xkb_state, keycode);
	if (codepoint >= 0x20 && codepoint != 0x7f &&
			codepoint < 0x110000 &&
			launcher_query_append(server, codepoint)) {
		launcher_filter(server);
		launcher_render(server);
	}
}

static int launcher_grid_index_at(struct nide_server *server,
		double x, double y) {
	struct nide_launcher *launcher = &server->launcher;
	double rel_x = x - launcher->grid.x;
	double rel_y = y - launcher->grid.y;
	if (rel_x < 0 || rel_y < 0) {
		return -1;
	}

	int column = (int)(rel_x / (launcher->tile_w + launcher->gap));
	int row = (int)(rel_y / (launcher->tile_h + launcher->gap));
	if (column < 0 || column >= NIDE_LAUNCHER_COLS ||
			row < 0 || row >= NIDE_LAUNCHER_ROWS) {
		return -1;
	}

	double inside_x = rel_x - column *
		(launcher->tile_w + launcher->gap);
	double inside_y = rel_y - row *
		(launcher->tile_h + launcher->gap);
	if (inside_x > launcher->tile_w ||
			inside_y > launcher->tile_h) {
		return -1;
	}

	int index = (launcher->scroll + row) * NIDE_LAUNCHER_COLS +
		column;
	if (index < 0 || index >= launcher->nfiltered) {
		return -1;
	}
	return index;
}

static bool launcher_point_in_box(double x, double y,
		struct wlr_box box) {
	return x >= box.x && y >= box.y &&
		x < box.x + box.width && y < box.y + box.height;
}

static int launcher_scrollbar_position(struct nide_server *server,
		double y, double grab) {
	struct nide_launcher *launcher = &server->launcher;
	struct wlr_box thumb;
	if (!launcher_scrollbar_thumb(server, &thumb)) {
		return launcher->scroll;
	}

	double usable = launcher->scroll_track.height - thumb.height;
	if (usable <= 0) {
		return 0;
	}

	double offset = y - launcher->scroll_track.y - grab;
	double progress = offset / usable;
	if (progress < 0.0) {
		progress = 0.0;
	} else if (progress > 1.0) {
		progress = 1.0;
	}

	int max_scroll = launcher_max_scroll(server);
	return (int)lround(progress * max_scroll);
}

static void launcher_handle_motion(struct nide_server *server) {
	struct nide_launcher *launcher = &server->launcher;
	double local_x = server->cursor->x - launcher->panel.x;
	double local_y = server->cursor->y - launcher->panel.y;

	if (launcher->scrollbar_dragging) {
		int row = launcher_scrollbar_position(server, local_y,
			launcher->scrollbar_grab);
		if (row != launcher->scroll) {
			launcher_scroll_to(server, row);
		}
		wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
			"default");
		return;
	}

	int index = launcher_grid_index_at(server, local_x, local_y);
	if (index >= 0 && index != launcher->selected) {
		launcher_select(server, index);
		launcher_render(server);
	}

	bool clickable = index >= 0;
	for (int i = 0; i < 2; i++) {
		if (launcher_point_in_box(local_x, local_y,
				launcher->tabs[i])) {
			clickable = true;
		}
	}
	struct wlr_box thumb;
	if (launcher_scrollbar_thumb(server, &thumb) &&
			launcher_point_in_box(local_x, local_y,
				launcher->scrollbar)) {
		clickable = true;
	}

	wlr_cursor_set_xcursor(server->cursor, server->cursor_mgr,
		clickable ? "pointer" : "default");
}

static void launcher_handle_click(struct nide_server *server,
		uint32_t button) {
	struct nide_launcher *launcher = &server->launcher;
	double local_x = server->cursor->x - launcher->panel.x;
	double local_y = server->cursor->y - launcher->panel.y;

	if (button != BTN_LEFT) {
		launcher_close(server);
		return;
	}

	if (local_x < 0 || local_y < 0 ||
			local_x >= launcher->panel.width ||
			local_y >= launcher->panel.height) {
		launcher_close(server);
		return;
	}

	for (int i = 0; i < 2; i++) {
		if (launcher_point_in_box(local_x, local_y,
				launcher->tabs[i])) {
			launcher_set_mode(server, i == 0 ?
				NIDE_LAUNCHER_WINDOWS : NIDE_LAUNCHER_APPS);
			return;
		}
	}

	struct wlr_box thumb;
	if (launcher_scrollbar_thumb(server, &thumb) &&
			launcher_point_in_box(local_x, local_y,
				launcher->scrollbar)) {
		launcher->scrollbar_dragging = true;
		if (launcher_point_in_box(local_x, local_y, thumb)) {
			launcher->scrollbar_grab = local_y - thumb.y;
		} else {
			launcher->scrollbar_grab = thumb.height / 2.0;
			int row = launcher_scrollbar_position(server, local_y,
				launcher->scrollbar_grab);
			launcher_scroll_to(server, row);
		}
		return;
	}

	int index = launcher_grid_index_at(server, local_x, local_y);
	if (index >= 0) {
		launcher_select(server, index);
		launcher_activate_selected(server);
	}
}

static void launcher_handle_release(struct nide_server *server) {
	server->launcher.scrollbar_dragging = false;
}

static void launcher_handle_axis(struct nide_server *server,
		struct wlr_pointer_axis_event *event) {
	if (event->orientation != WL_POINTER_AXIS_VERTICAL_SCROLL ||
			event->delta == 0) {
		return;
	}
	launcher_scroll_by(server, event->delta > 0 ? 1 : -1);
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
	case NIDE_ACTION_LAUNCHER:
		launcher_open(server);
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

	/*
	 * Keep the launcher reachable for existing configs that were written
	 * before the "launcher" action existed.
	 */
	if (mods == WLR_MODIFIER_LOGO &&
			xkb_keysym_to_lower(sym) == XKB_KEY_Tab) {
		launcher_open(server);
		return true;
	}
	return false;
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct nide_keyboard *keyboard = wl_container_of(listener, keyboard, key);
	struct nide_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	if (server->launcher.open) {
		launcher_handle_key(server, keyboard, event);
		return;
	}

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

	if (server->launcher.open) {
		if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
			launcher_handle_click(server, event->button);
		} else {
			launcher_handle_release(server);
		}
		return;
	}

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

	if (server->launcher.open) {
		launcher_handle_axis(server, event);
		return;
	}

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

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
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

	if (server->launcher.open) {
		launcher_render(server);
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

	if (server->launcher.open) {
		launcher_build_items(server);
		launcher_render(server);
	}
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

	if (server->launcher.open) {
		launcher_build_items(server);
		launcher_render(server);
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

	if (server->launcher.open) {
		launcher_build_items(server);
		launcher_render(server);
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

static const char *action_name(enum nide_action action) {
	switch (action) {
	case NIDE_ACTION_SPAWN: return "spawn";
	case NIDE_ACTION_CLOSE: return "close";
	case NIDE_ACTION_MAXIMIZE: return "maximize";
	case NIDE_ACTION_FULLSCREEN: return "fullscreen";
	case NIDE_ACTION_LAUNCHER: return "launcher";
	case NIDE_ACTION_QUIT: return "quit";
	}
	return "?";
}

static void print_bindings(const struct nide_config *cfg) {
	printf("Keybindings (%d):\n", cfg->nbinds);
	for (int i = 0; i < cfg->nbinds; i++) {
		const struct nide_bind *bind = &cfg->binds[i];
		const char *action = action_name(bind->action);
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
	launcher_init(&server);

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

	launcher_free_items(&server);
	free(server.launcher.apps);
	server.launcher.apps = NULL;
	server.launcher.napps = 0;
	server.launcher.appcap = 0;

	wlr_scene_node_destroy(&server.scene->tree.node);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.display);

	return 0;
}
