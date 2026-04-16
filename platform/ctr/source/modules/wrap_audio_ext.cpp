#include <modules/audio/wrap_audio.hpp>
#include <common/luax.hpp>
#include <objects/data/sounddata/sounddata.hpp>

#include <objects/recordingdevice_ext.hpp>

#include <3ds.h>
#include <malloc.h> // memalign

using namespace love;

// clang-format off
static int w_RecordingDevice_start(lua_State* L)
{
    auto* self       = luax::CheckType<RecordingDevice3DS>(L, 1);
    int   samples    = (int)luaL_optinteger(L, 2, 8192);
    int   sampleRate = (int)luaL_optinteger(L, 3, 16000);
    int   bitDepth   = (int)luaL_optinteger(L, 4, 16);
    int   channels   = (int)luaL_optinteger(L, 5, 1);

    luax::PushBoolean(L, self->start(samples, sampleRate, bitDepth, channels));
    return 1;
}

static int w_RecordingDevice_stop(lua_State* L)
{
    auto* self = luax::CheckType<RecordingDevice3DS>(L, 1);
    luax::PushBoolean(L, self->stop());
    return 1;
}

static int w_RecordingDevice_getData(lua_State* L)
{
    auto* self = luax::CheckType<RecordingDevice3DS>(L, 1);

    SoundData* data = nullptr;
    luax::CatchException(L, [&]() { data = self->getData(); });

    if (data != nullptr)
    {
        luax::PushType(L, data);
        data->Release();
    }
    else
    {
        lua_pushnil(L);
    }
    return 1;
}

static int w_RecordingDevice_isRecording(lua_State* L)
{
    auto* self = luax::CheckType<RecordingDevice3DS>(L, 1);
    luax::PushBoolean(L, self->isRecording());
    return 1;
}

static int w_RecordingDevice_getName(lua_State* L)
{
    auto* self = luax::CheckType<RecordingDevice3DS>(L, 1);
    lua_pushstring(L, self->getName());
    return 1;
}

static int w_RecordingDevice_getSampleCount(lua_State* L)
{
    auto* self = luax::CheckType<RecordingDevice3DS>(L, 1);
    lua_pushinteger(L, self->getSampleCount());
    return 1;
}

static int w_RecordingDevice_getSampleRate(lua_State* L)
{
    auto* self = luax::CheckType<RecordingDevice3DS>(L, 1);
    lua_pushinteger(L, self->getSampleRate());
    return 1;
}

static int w_RecordingDevice_getBitDepth(lua_State* L)
{
    auto* self = luax::CheckType<RecordingDevice3DS>(L, 1);
    lua_pushinteger(L, self->getBitDepth());
    return 1;
}

static int w_RecordingDevice_getChannelCount(lua_State* L)
{
    auto* self = luax::CheckType<RecordingDevice3DS>(L, 1);
    lua_pushinteger(L, self->getChannelCount());
    return 1;
}
// clang-format on

static int w_getRecordingDevices(lua_State* L)
{
    // Probe the MIC service to check hardware availability.
    // The buffer must be 0x1000-aligned regular heap memory (not linear memory).
    const u32 probeSize = 0x30000;
    u8*       probeBuf  = static_cast<u8*>(memalign(0x1000, probeSize));

    if (probeBuf == nullptr)
    {
        lua_newtable(L);
        lua_pushstring(L, "memalign failed (out of memory)");
        return 2;
    }

    Result res = micInit(probeBuf, probeSize);
    if (R_SUCCEEDED(res))
        micExit();
    free(probeBuf);

    lua_newtable(L);

    if (R_FAILED(res))
    {
        char buf[80];
        snprintf(buf, sizeof(buf),
                 "micInit failed: 0x%08lX (level=%ld desc=%ld module=%ld)",
                 (unsigned long)res,
                 (long)R_LEVEL(res),
                 (long)R_DESCRIPTION(res),
                 (long)R_MODULE(res));
        lua_pushstring(L, buf);
        return 2;
    }

    luax::CatchException(L, [&]() {
        auto* device = new RecordingDevice3DS();
        luax::PushType(L, device);
        device->Release();
    });
    lua_rawseti(L, -2, 1);

    return 1;
}

// clang-format off
static constexpr luaL_Reg recordingDeviceMethods[] =
{
    { "start",           w_RecordingDevice_start           },
    { "stop",            w_RecordingDevice_stop            },
    { "getData",         w_RecordingDevice_getData         },
    { "isRecording",     w_RecordingDevice_isRecording     },
    { "getName",         w_RecordingDevice_getName         },
    { "getSampleCount",  w_RecordingDevice_getSampleCount  },
    { "getSampleRate",   w_RecordingDevice_getSampleRate   },
    { "getBitDepth",     w_RecordingDevice_getBitDepth     },
    { "getChannelCount", w_RecordingDevice_getChannelCount },
};
// clang-format on

/**
 * Called from Wrap_Audio::Register() after luax::RegisterModule().
 * The love.audio table is on top of the Lua stack at that point.
 *
 * Registers the RecordingDevice3DS type metatable and adds
 * love.audio.getRecordingDevices to the module table.
 */
int Wrap_Audio_Ext_Register(lua_State* L)
{
    luax::RegisterType(L, &RecordingDevice3DS::type,
                       std::span<const luaL_Reg>(recordingDeviceMethods));

    lua_pushcfunction(L, w_getRecordingDevices);
    lua_setfield(L, -2, "getRecordingDevices");

    return 0;
}
