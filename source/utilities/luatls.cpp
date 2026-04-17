// source/utilities/luatls.cpp
// Minimal TLS client socket exposed to Lua as the "tls" module.
//
// Lua API (mirrors the luasec interface used by lib/websocket.lua):
//
//   local tls = require("tls")
//
//   -- Connect and perform TLS handshake.
//   -- Returns a socket userdata on success, or nil + error string on failure.
//   local sock, err = tls.connect(host, port)
//
//   -- Send data. Returns bytes_sent, nil on success or nil, errmsg on failure.
//   sock:send(data)
//
//   -- Receive data.
//   --   sock:receive(n)    → read exactly n bytes
//   --   sock:receive("*l") → read a line (strips \n, keeps \r if present)
//   -- Returns data_string, nil on success or nil, errmsg on failure.
//   sock:receive(pattern)
//
//   -- Close the connection.
//   sock:close()
//
// Implementation uses mbedtls (available as 3ds-mbedtls in devkitPro).

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>

#include <cstring>
#include <string>

extern "C"
{
#include <lauxlib.h>
#include <lua.h>
}

// ---------------------------------------------------------------------------
// TLS socket context (stored as Lua userdata)
// ---------------------------------------------------------------------------

struct TLSSocket
{
    mbedtls_net_context      net;
    mbedtls_ssl_context      ssl;
    mbedtls_ssl_config       conf;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    bool                     connected;

    // Line-receive buffer
    std::string recvBuf;
};

static const char* TLS_SOCKET_MT = "tls.socket";

// ---------------------------------------------------------------------------
// Helper: format mbedtls error code as a string
// ---------------------------------------------------------------------------

static std::string mbedtlsError(int ret)
{
    char buf[256];
    mbedtls_strerror(ret, buf, sizeof(buf));
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Lua: sock:send(data)
// ---------------------------------------------------------------------------

static int tls_sock_send(lua_State* L)
{
    TLSSocket* s = (TLSSocket*)luaL_checkudata(L, 1, TLS_SOCKET_MT);
    size_t      len;
    const char* data = luaL_checklstring(L, 2, &len);

    if (!s->connected)
    {
        lua_pushnil(L);
        lua_pushstring(L, "socket is closed");
        return 2;
    }

    size_t sent = 0;
    while (sent < len)
    {
        int ret = mbedtls_ssl_write(&s->ssl,
                                    (const unsigned char*)(data + sent),
                                    len - sent);
        if (ret < 0)
        {
            lua_pushnil(L);
            lua_pushstring(L, mbedtlsError(ret).c_str());
            return 2;
        }
        sent += (size_t)ret;
    }

    lua_pushinteger(L, (lua_Integer)sent);
    lua_pushnil(L);
    return 2;
}

// ---------------------------------------------------------------------------
// Lua: sock:receive(pattern)
//   pattern = number  → read exactly that many bytes
//   pattern = "*l"    → read a line (terminated by \n)
// ---------------------------------------------------------------------------

static int tls_sock_receive(lua_State* L)
{
    TLSSocket* s = (TLSSocket*)luaL_checkudata(L, 1, TLS_SOCKET_MT);

    if (!s->connected)
    {
        lua_pushnil(L);
        lua_pushstring(L, "socket is closed");
        return 2;
    }

    // Determine mode
    bool lineMode = false;
    size_t wantBytes = 0;

    if (lua_type(L, 2) == LUA_TSTRING)
    {
        const char* pat = lua_tostring(L, 2);
        if (strcmp(pat, "*l") == 0 || strcmp(pat, "l") == 0)
            lineMode = true;
        else
        {
            lua_pushnil(L);
            lua_pushstring(L, "unsupported receive pattern");
            return 2;
        }
    }
    else
    {
        wantBytes = (size_t)luaL_checkinteger(L, 2);
    }

    if (lineMode)
    {
        // Read until \n, buffering data
        while (true)
        {
            // Check if we already have a complete line in the buffer
            size_t pos = s->recvBuf.find('\n');
            if (pos != std::string::npos)
            {
                std::string line = s->recvBuf.substr(0, pos);
                s->recvBuf.erase(0, pos + 1);
                lua_pushlstring(L, line.c_str(), line.size());
                lua_pushnil(L);
                return 2;
            }

            // Read more data
            unsigned char tmp[256];
            int ret = mbedtls_ssl_read(&s->ssl, tmp, sizeof(tmp));
            if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            {
                // Connection closed — return whatever we have
                if (!s->recvBuf.empty())
                {
                    std::string line = s->recvBuf;
                    s->recvBuf.clear();
                    lua_pushlstring(L, line.c_str(), line.size());
                    lua_pushnil(L);
                    return 2;
                }
                lua_pushnil(L);
                lua_pushstring(L, "closed");
                return 2;
            }
            if (ret < 0)
            {
                lua_pushnil(L);
                lua_pushstring(L, mbedtlsError(ret).c_str());
                return 2;
            }
            s->recvBuf.append((char*)tmp, (size_t)ret);
        }
    }
    else
    {
        // Read exactly wantBytes
        std::string result;
        result.reserve(wantBytes);

        // Drain from buffer first
        if (!s->recvBuf.empty())
        {
            size_t take = std::min(wantBytes, s->recvBuf.size());
            result.append(s->recvBuf, 0, take);
            s->recvBuf.erase(0, take);
        }

        while (result.size() < wantBytes)
        {
            size_t remaining = wantBytes - result.size();
            unsigned char tmp[512];
            size_t toRead = std::min(remaining, sizeof(tmp));
            int ret = mbedtls_ssl_read(&s->ssl, tmp, toRead);
            if (ret == 0 || ret == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY)
            {
                lua_pushnil(L);
                lua_pushstring(L, "closed");
                return 2;
            }
            if (ret < 0)
            {
                lua_pushnil(L);
                lua_pushstring(L, mbedtlsError(ret).c_str());
                return 2;
            }
            result.append((char*)tmp, (size_t)ret);
        }

        lua_pushlstring(L, result.c_str(), result.size());
        lua_pushnil(L);
        return 2;
    }
}

// ---------------------------------------------------------------------------
// Lua: sock:close()
// ---------------------------------------------------------------------------

static int tls_sock_close(lua_State* L)
{
    TLSSocket* s = (TLSSocket*)luaL_checkudata(L, 1, TLS_SOCKET_MT);
    if (s->connected)
    {
        mbedtls_ssl_close_notify(&s->ssl);
        mbedtls_net_free(&s->net);
        mbedtls_ssl_free(&s->ssl);
        mbedtls_ssl_config_free(&s->conf);
        mbedtls_ctr_drbg_free(&s->ctr_drbg);
        mbedtls_entropy_free(&s->entropy);
        s->connected = false;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// GC metamethod — clean up if not explicitly closed
// ---------------------------------------------------------------------------

static int tls_sock_gc(lua_State* L)
{
    return tls_sock_close(L);
}

// ---------------------------------------------------------------------------
// Lua: tls.connect(host, port)
// ---------------------------------------------------------------------------

static int tls_connect(lua_State* L)
{
    const char* host = luaL_checkstring(L, 1);
    int         port = (int)luaL_checkinteger(L, 2);

    // Convert port to string for mbedtls_net_connect
    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%d", port);

    // Allocate userdata
    TLSSocket* s = (TLSSocket*)lua_newuserdata(L, sizeof(TLSSocket));
    memset(s, 0, sizeof(TLSSocket));
    s->connected = false;

    luaL_getmetatable(L, TLS_SOCKET_MT);
    lua_setmetatable(L, -2);

    // Initialise mbedtls structures
    mbedtls_net_init(&s->net);
    mbedtls_ssl_init(&s->ssl);
    mbedtls_ssl_config_init(&s->conf);
    mbedtls_entropy_init(&s->entropy);
    mbedtls_ctr_drbg_init(&s->ctr_drbg);

    int ret;

    // Seed the RNG
    const char* pers = "lovepotion_tls";
    ret = mbedtls_ctr_drbg_seed(&s->ctr_drbg, mbedtls_entropy_func, &s->entropy,
                                 (const unsigned char*)pers, strlen(pers));
    if (ret != 0)
    {
        lua_pushnil(L);
        lua_pushstring(L, ("tls.connect: RNG seed failed: " + mbedtlsError(ret)).c_str());
        return 2;
    }

    // TCP connect
    ret = mbedtls_net_connect(&s->net, host, portStr, MBEDTLS_NET_PROTO_TCP);
    if (ret != 0)
    {
        lua_pushnil(L);
        lua_pushstring(L, ("tls.connect: TCP connect failed: " + mbedtlsError(ret)).c_str());
        return 2;
    }

    // SSL config — client mode, no certificate verification (3DS has no CA store)
    ret = mbedtls_ssl_config_defaults(&s->conf,
                                      MBEDTLS_SSL_IS_CLIENT,
                                      MBEDTLS_SSL_TRANSPORT_STREAM,
                                      MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0)
    {
        lua_pushnil(L);
        lua_pushstring(L, ("tls.connect: SSL config failed: " + mbedtlsError(ret)).c_str());
        return 2;
    }

    // Skip certificate verification — 3DS has no system CA store
    mbedtls_ssl_conf_authmode(&s->conf, MBEDTLS_SSL_VERIFY_NONE);
    mbedtls_ssl_conf_rng(&s->conf, mbedtls_ctr_drbg_random, &s->ctr_drbg);

    // Set up SSL context
    ret = mbedtls_ssl_setup(&s->ssl, &s->conf);
    if (ret != 0)
    {
        lua_pushnil(L);
        lua_pushstring(L, ("tls.connect: SSL setup failed: " + mbedtlsError(ret)).c_str());
        return 2;
    }

    // Set hostname for SNI
    ret = mbedtls_ssl_set_hostname(&s->ssl, host);
    if (ret != 0)
    {
        lua_pushnil(L);
        lua_pushstring(L, ("tls.connect: set hostname failed: " + mbedtlsError(ret)).c_str());
        return 2;
    }

    // Wire the net context as the I/O layer
    mbedtls_ssl_set_bio(&s->ssl, &s->net, mbedtls_net_send, mbedtls_net_recv, nullptr);

    // TLS handshake
    while ((ret = mbedtls_ssl_handshake(&s->ssl)) != 0)
    {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE)
        {
            lua_pushnil(L);
            lua_pushstring(L, ("tls.connect: TLS handshake failed: " + mbedtlsError(ret)).c_str());
            return 2;
        }
    }

    s->connected = true;

    // Return the socket userdata (already on stack), nil error
    lua_pushnil(L);
    return 2;  // sock, nil
}

// ---------------------------------------------------------------------------
// Module registration
// ---------------------------------------------------------------------------

static const luaL_Reg tls_socket_methods[] = {
    { "send",    tls_sock_send    },
    { "receive", tls_sock_receive },
    { "close",   tls_sock_close   },
    { nullptr,   nullptr          }
};

static const luaL_Reg tls_module[] = {
    { "connect", tls_connect },
    { nullptr,   nullptr     }
};

extern "C" int luaopen_tls(lua_State* L)
{
    // Create metatable for TLS socket userdata
    luaL_newmetatable(L, TLS_SOCKET_MT);

    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");  // mt.__index = mt

    lua_pushcfunction(L, tls_sock_gc);
    lua_setfield(L, -2, "__gc");

    luaL_register(L, nullptr, tls_socket_methods);
    lua_pop(L, 1);

    // Create and return the module table
    luaL_register(L, "tls", tls_module);
    return 1;
}
