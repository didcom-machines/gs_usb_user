/* lua_gsusb.c - Lua C binding for libgsusb.so, using the standard Lua/C
 * API (lua.h/lauxlib.h) rather than LuaJIT's FFI -- some LuaJIT builds
 * (confirmed: Teltonika RutOS's luajit2 package on RUT956, mipsel_24kc)
 * ship with FFI compiled out entirely (no ffi.cdef/ffi.load at all, not
 * just a missing module), so this is the binding strategy that actually
 * works everywhere LuaJIT's standard C API does, not just where FFI
 * happens to be enabled.
 *
 * Three userdata types, each with its own metatable and method table
 * (the standard Lua idiom for OOP -- see luaopen_gsusb() for how the
 * inheritance-free "one flat method table per type" shape is wired up):
 *
 *   ctx     (gsusb.ctx)     -- gsusb_init()'s libusb context. gsusb.init().
 *   dev     (gsusb.dev)     -- one USB device. ctx:open(vid, pid, ...).
 *   channel (gsusb.channel) -- one CAN channel on a dev. dev:channel(n).
 *
 * A channel keeps a reference to its owning dev (via LuaJIT/Lua 5.1's
 * per-userdata "environment table", see push_channel()/CHANNEL_DEV_KEY)
 * so the dev -- and the gsusb_dev it wraps -- can't be garbage collected
 * out from under a channel still in use; gsusb_channel_t objects are
 * owned by their gsusb_dev and must never be freed independently (see
 * gsusb.h), so channel userdata has no __gc of its own at all.
 *
 * Deliberately NOT bound yet (present in gsusb.h, not needed to port the
 * IOXDevice/IOXBus object model, which only needs open/configure(bitrate)/
 * start/send/recv/close): bit-timing-constant introspection, raw
 * bit-timing register access, and termination-resistor control. Add
 * them the same way (see set_bitrate()/start() for the pattern) if a
 * future need comes up -- this file isn't trying to cover 100% of
 * gsusb.h on day one, just what the port actually needs.
 *
 * Build: needs gsusb.h (../../driver/include) and LuaJIT's headers
 * (lua.h/lauxlib.h/lualib.h -- matches the standard Lua 5.1 C API).
 * Links against libgsusb.so at runtime (not statically against
 * gsusb.c/bittiming.c) -- the whole point is to use the one .so already
 * deployed, the same way the Python SDK dlopens it.
 */
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "gsusb.h"

#define GSUSB_CTX_MT "gsusb.ctx"
#define GSUSB_DEV_MT "gsusb.dev"
#define GSUSB_CHANNEL_MT "gsusb.channel"

#define CHANNEL_DEV_KEY "dev" /* key in a channel userdata's env table holding its owning dev userdata */

typedef struct {
	gsusb_dev *dev;
} dev_ud;

typedef struct {
	gsusb_channel *ch;
} channel_ud;

/* --- CAN frame <-> Lua table marshaling ---------------------------------
 *
 * {id, extended, rtr, err, data, flags, timestamp_us} -- deliberately the
 * same field names/shape as the Python SDK's Frame (gs_usb_user/sdk/python),
 * so porting FMXDevice/IOXDevice's frame handling from Python to Lua is a
 * close syntactic match, not a redesign. `data` is a Lua string (binary-
 * safe via lua_pushlstring/luaL_checklstring, not a C string), matching
 * how the Python SDK also treats frame payloads as opaque bytes.
 */

static void push_frame(lua_State *L, const gsusb_frame *f)
{
	int extended = (f->can_id & GSUSB_EFF_FLAG) != 0;
	uint32_t id = f->can_id & (extended ? GSUSB_EFF_MASK : GSUSB_SFF_MASK);

	lua_createtable(L, 0, 7);
	lua_pushinteger(L, (lua_Integer)id);
	lua_setfield(L, -2, "id");
	lua_pushboolean(L, extended);
	lua_setfield(L, -2, "extended");
	lua_pushboolean(L, (f->can_id & GSUSB_RTR_FLAG) != 0);
	lua_setfield(L, -2, "rtr");
	lua_pushboolean(L, (f->can_id & GSUSB_ERR_FLAG) != 0);
	lua_setfield(L, -2, "err");
	lua_pushlstring(L, (const char *)f->data, f->len);
	lua_setfield(L, -2, "data");
	lua_pushinteger(L, f->flags);
	lua_setfield(L, -2, "flags");
	lua_pushinteger(L, (lua_Integer)f->timestamp_us);
	lua_setfield(L, -2, "timestamp_us");
}

/* Reads a frame table at stack index `idx` into *f. Only `id` is
 * required; extended/rtr/data/flags all default to off/empty if absent,
 * so a caller building a minimal request (see fmx_send.py's send_burst()
 * for the Python-side equivalent) doesn't have to spell out every field. */
static void check_frame(lua_State *L, int idx, gsusb_frame *f)
{
	luaL_checktype(L, idx, LUA_TTABLE);
	memset(f, 0, sizeof(*f));

	lua_getfield(L, idx, "id");
	uint32_t id = (uint32_t)luaL_checkinteger(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, idx, "extended");
	int extended = lua_toboolean(L, -1);
	lua_pop(L, 1);

	lua_getfield(L, idx, "rtr");
	int rtr = lua_toboolean(L, -1);
	lua_pop(L, 1);

	f->can_id = (id & (extended ? GSUSB_EFF_MASK : GSUSB_SFF_MASK)) | (extended ? GSUSB_EFF_FLAG : 0) |
		    (rtr ? GSUSB_RTR_FLAG : 0);

	lua_getfield(L, idx, "data");
	if (!lua_isnil(L, -1)) {
		size_t len = 0;
		const char *data = luaL_checklstring(L, -1, &len);
		if (len > GSUSB_MAX_DLEN)
			luaL_error(L, "frame data too long (%d bytes, max %d)", (int)len, GSUSB_MAX_DLEN);
		memcpy(f->data, data, len);
		f->len = (uint8_t)len;
	}
	lua_pop(L, 1);

	lua_getfield(L, idx, "flags");
	if (!lua_isnil(L, -1))
		f->flags = (uint8_t)luaL_checkinteger(L, -1);
	lua_pop(L, 1);
}

/* --- error helper: nil, "msg" for every gsusb_* call that returns <0 --- */

static int push_gsusb_error(lua_State *L, const char *what, int rc)
{
	lua_pushnil(L);
	lua_pushfstring(L, "%s failed: %d", what, rc);
	return 2;
}

/* --- gsusb.ctx ------------------------------------------------------------ */

static int l_gsusb_init(lua_State *L)
{
	void *ctx = NULL;
	int rc = gsusb_init(&ctx);
	if (rc != 0)
		return push_gsusb_error(L, "gsusb_init", rc);

	void **ud = (void **)lua_newuserdata(L, sizeof(void *));
	*ud = ctx;
	luaL_getmetatable(L, GSUSB_CTX_MT);
	lua_setmetatable(L, -2);
	return 1;
}

static int l_ctx_exit(lua_State *L)
{
	void **ud = (void **)luaL_checkudata(L, 1, GSUSB_CTX_MT);
	if (*ud != NULL) {
		gsusb_exit(*ud);
		*ud = NULL; /* safe to call again (explicitly or via __gc) -- no double-free */
	}
	return 0;
}

/* ctx:open(vid, pid, [bus], [addr]) -> dev | nil, errmsg
 * bus/addr default to -1 (match any), matching gsusb_open()'s own
 * convention for "don't care" (see gsusb.h). */
static int l_ctx_open(lua_State *L)
{
	void **ctx_ud = (void **)luaL_checkudata(L, 1, GSUSB_CTX_MT);
	luaL_argcheck(L, *ctx_ud != NULL, 1, "ctx already closed (gsusb.exit() already called)");
	uint16_t vid = (uint16_t)luaL_checkinteger(L, 2);
	uint16_t pid = (uint16_t)luaL_checkinteger(L, 3);
	int bus = (int)luaL_optinteger(L, 4, -1);
	int addr = (int)luaL_optinteger(L, 5, -1);

	char errbuf[256];
	gsusb_dev *dev = gsusb_open(*ctx_ud, vid, pid, bus, addr, errbuf, sizeof(errbuf));
	if (dev == NULL) {
		lua_pushnil(L);
		lua_pushstring(L, errbuf);
		return 2;
	}

	dev_ud *ud = (dev_ud *)lua_newuserdata(L, sizeof(dev_ud));
	ud->dev = dev;
	luaL_getmetatable(L, GSUSB_DEV_MT);
	lua_setmetatable(L, -2);
	return 1;
}

static const luaL_Reg ctx_methods[] = {
	{ "open", l_ctx_open },
	{ "exit", l_ctx_exit },
	{ NULL, NULL },
};

/* --- gsusb.dev -------------------------------------------------------- */

static int l_dev_close(lua_State *L)
{
	dev_ud *ud = (dev_ud *)luaL_checkudata(L, 1, GSUSB_DEV_MT);
	if (ud->dev != NULL) {
		gsusb_close(ud->dev);
		ud->dev = NULL;
	}
	return 0;
}

static int l_dev_channel_count(lua_State *L)
{
	dev_ud *ud = (dev_ud *)luaL_checkudata(L, 1, GSUSB_DEV_MT);
	luaL_argcheck(L, ud->dev != NULL, 1, "dev already closed");
	lua_pushinteger(L, gsusb_channel_count(ud->dev));
	return 1;
}

/* dev:device_config() -> {sw_version, hw_version, channel_count} */
static int l_dev_device_config(lua_State *L)
{
	dev_ud *ud = (dev_ud *)luaL_checkudata(L, 1, GSUSB_DEV_MT);
	luaL_argcheck(L, ud->dev != NULL, 1, "dev already closed");
	const gsusb_device_config *cfg = gsusb_get_device_config(ud->dev);
	if (cfg == NULL) {
		lua_pushnil(L);
		return 1;
	}
	lua_createtable(L, 0, 3);
	lua_pushinteger(L, (lua_Integer)cfg->sw_version);
	lua_setfield(L, -2, "sw_version");
	lua_pushinteger(L, (lua_Integer)cfg->hw_version);
	lua_setfield(L, -2, "hw_version");
	lua_pushinteger(L, cfg->channel_count);
	lua_setfield(L, -2, "channel_count");
	return 1;
}

/* dev:channel(index) -> channel userdata, keeping `dev` alive via its env table */
static int l_dev_channel(lua_State *L)
{
	dev_ud *ud = (dev_ud *)luaL_checkudata(L, 1, GSUSB_DEV_MT);
	luaL_argcheck(L, ud->dev != NULL, 1, "dev already closed");
	unsigned int index = (unsigned int)luaL_checkinteger(L, 2);

	gsusb_channel *ch = gsusb_channel_get(ud->dev, index);
	if (ch == NULL) {
		lua_pushnil(L);
		lua_pushstring(L, "no such channel");
		return 2;
	}

	channel_ud *chud = (channel_ud *)lua_newuserdata(L, sizeof(channel_ud));
	chud->ch = ch;
	luaL_getmetatable(L, GSUSB_CHANNEL_MT);
	lua_setmetatable(L, -2);

	/* env table: {dev = <the dev userdata at stack index 1>} -- keeps the
	 * parent dev (and the gsusb_dev / libusb handle it owns) referenced
	 * for as long as this channel is, so it can't be GC'd/closed out
	 * from under a channel still in use. */
	lua_createtable(L, 0, 1);
	lua_pushvalue(L, 1);
	lua_setfield(L, -2, CHANNEL_DEV_KEY);
#if LUA_VERSION_NUM >= 502
	lua_setuservalue(L, -2);
#else
	lua_setfenv(L, -2);
#endif
	return 1;
}

static const luaL_Reg dev_methods[] = {
	{ "channel", l_dev_channel },
	{ "channel_count", l_dev_channel_count },
	{ "device_config", l_dev_device_config },
	{ "close", l_dev_close },
	{ NULL, NULL },
};

/* --- gsusb.channel ------------------------------------------------------ */

static channel_ud *check_channel(lua_State *L, int idx)
{
	channel_ud *ud = (channel_ud *)luaL_checkudata(L, idx, GSUSB_CHANNEL_MT);
	luaL_argcheck(L, ud->ch != NULL, idx, "channel's dev already closed");
	return ud;
}

/* ch:set_bitrate(bitrate, [sample_point]) -> true | nil, errmsg
 * sample_point 0 (the default) picks the CiA-recommended point for the
 * given bitrate automatically -- see gsusb_channel_set_bitrate()'s
 * docstring in gsusb.h. */
static int l_ch_set_bitrate(lua_State *L)
{
	channel_ud *ud = check_channel(L, 1);
	uint32_t bitrate = (uint32_t)luaL_checkinteger(L, 2);
	double sample_point = luaL_optnumber(L, 3, 0.0);

	int rc = gsusb_channel_set_bitrate(ud->ch, bitrate, sample_point);
	if (rc != 0)
		return push_gsusb_error(L, "gsusb_channel_set_bitrate", rc);
	lua_pushboolean(L, 1);
	return 1;
}

/* ch:start([mode_flags]) -> true | nil, errmsg -- mode_flags defaults to
 * 0 (normal mode); pass gsusb.MODE_LISTEN_ONLY etc, OR'd together, for
 * anything else (see the GSUSB_MODE_* constants exported below). */
static int l_ch_start(lua_State *L)
{
	channel_ud *ud = check_channel(L, 1);
	uint32_t mode_flags = (uint32_t)luaL_optinteger(L, 2, 0);

	int rc = gsusb_channel_start(ud->ch, mode_flags);
	if (rc != 0)
		return push_gsusb_error(L, "gsusb_channel_start", rc);
	lua_pushboolean(L, 1);
	return 1;
}

static int l_ch_stop(lua_State *L)
{
	channel_ud *ud = check_channel(L, 1);
	int rc = gsusb_channel_stop(ud->ch);
	if (rc != 0)
		return push_gsusb_error(L, "gsusb_channel_stop", rc);
	lua_pushboolean(L, 1);
	return 1;
}

/* ch:send(frame, [timeout_ms]) -> true | nil, errmsg */
static int l_ch_send(lua_State *L)
{
	channel_ud *ud = check_channel(L, 1);
	gsusb_frame frame;
	check_frame(L, 2, &frame);
	unsigned int timeout_ms = (unsigned int)luaL_optinteger(L, 3, 1000);

	int rc = gsusb_channel_send(ud->ch, &frame, timeout_ms);
	if (rc != 0)
		return push_gsusb_error(L, "gsusb_channel_send", rc);
	lua_pushboolean(L, 1);
	return 1;
}

/* ch:recv([timeout_ms]) -> frame | nil (timeout) | nil, errmsg (error)
 * -- the caller's own poll loop (see iox_bus.py's _dispatch()-driving
 * recv loop for the Python-SDK equivalent this mirrors) calls this
 * repeatedly with a short timeout; distinguishing "timeout, keep
 * looping" (nil, no second value) from "real error" (nil, errmsg) is
 * why those two cases return a different number of values. */
static int l_ch_recv(lua_State *L)
{
	channel_ud *ud = check_channel(L, 1);
	unsigned int timeout_ms = (unsigned int)luaL_optinteger(L, 2, 1000);

	gsusb_frame frame;
	int rc = gsusb_channel_recv(ud->ch, &frame, timeout_ms);
	if (rc < 0)
		return push_gsusb_error(L, "gsusb_channel_recv", rc);
	if (rc == 0) {
		lua_pushnil(L); /* plain timeout, not an error -- see docstring above */
		return 1;
	}
	push_frame(L, &frame);
	return 1;
}

static int l_ch_get_state(lua_State *L)
{
	channel_ud *ud = check_channel(L, 1);
	uint32_t state = 0, rxerr = 0, txerr = 0;
	int rc = gsusb_channel_get_state(ud->ch, &state, &rxerr, &txerr);
	if (rc != 0)
		return push_gsusb_error(L, "gsusb_channel_get_state", rc);
	lua_pushinteger(L, state);
	lua_pushinteger(L, rxerr);
	lua_pushinteger(L, txerr);
	return 3;
}

static int l_ch_set_identify(lua_State *L)
{
	channel_ud *ud = check_channel(L, 1);
	int on = lua_toboolean(L, 2);
	int rc = gsusb_channel_set_identify(ud->ch, on);
	if (rc != 0)
		return push_gsusb_error(L, "gsusb_channel_set_identify", rc);
	lua_pushboolean(L, 1);
	return 1;
}

static int l_ch_features(lua_State *L)
{
	channel_ud *ud = check_channel(L, 1);
	lua_pushinteger(L, (lua_Integer)gsusb_channel_features(ud->ch));
	return 1;
}

static const luaL_Reg channel_methods[] = {
	{ "set_bitrate", l_ch_set_bitrate },
	{ "start", l_ch_start },
	{ "stop", l_ch_stop },
	{ "send", l_ch_send },
	{ "recv", l_ch_recv },
	{ "get_state", l_ch_get_state },
	{ "set_identify", l_ch_set_identify },
	{ "features", l_ch_features },
	{ NULL, NULL },
};

/* --- module setup --------------------------------------------------------
 *
 * Same "userdata + __index = method table, __gc = the matching cleanup
 * call" shape for all three types except gsusb.channel, which has no
 * __gc (see the file-level comment on why: channels are owned by their
 * dev, never freed independently). */

static void make_metatable(lua_State *L, const char *name, const luaL_Reg *methods, lua_CFunction gc)
{
	luaL_newmetatable(L, name);
	lua_newtable(L);
	luaL_register(L, NULL, methods);
	lua_setfield(L, -2, "__index");
	if (gc) {
		lua_pushcfunction(L, gc);
		lua_setfield(L, -2, "__gc");
	}
	lua_pop(L, 1);
}

static const luaL_Reg gsusb_funcs[] = {
	{ "init", l_gsusb_init },
	{ NULL, NULL },
};

int luaopen_gsusb(lua_State *L)
{
	make_metatable(L, GSUSB_CTX_MT, ctx_methods, l_ctx_exit);
	make_metatable(L, GSUSB_DEV_MT, dev_methods, l_dev_close);
	make_metatable(L, GSUSB_CHANNEL_MT, channel_methods, NULL);

	lua_newtable(L);
	luaL_register(L, NULL, gsusb_funcs); /* Lua 5.1 / LuaJIT C API: register into the table on top of stack */

	/* GSUSB_MODE_* for ch:start(mode_flags) -- named the same as the C
	 * enum in gsusb.h (module-qualified: gsusb.MODE_LISTEN_ONLY etc). */
	lua_pushinteger(L, GSUSB_MODE_LISTEN_ONLY);
	lua_setfield(L, -2, "MODE_LISTEN_ONLY");
	lua_pushinteger(L, GSUSB_MODE_LOOPBACK);
	lua_setfield(L, -2, "MODE_LOOPBACK");
	lua_pushinteger(L, GSUSB_MODE_TRIPLE_SAMPLE);
	lua_setfield(L, -2, "MODE_TRIPLE_SAMPLE");
	lua_pushinteger(L, GSUSB_MODE_ONE_SHOT);
	lua_setfield(L, -2, "MODE_ONE_SHOT");
	lua_pushinteger(L, GSUSB_MODE_BERR_REPORTING);
	lua_setfield(L, -2, "MODE_BERR_REPORTING");

	return 1;
}
