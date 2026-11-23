#include "com.h"

static const uint8_t IID_IUnknown_WASAPI[16] = {0,0,0,0,0,0,0,0,0xC0,0,0,0,0,0,0,0x46};
static const uint8_t IID_IMMDeviceEnumerator[16] = {0xD2,0x64,0x56,0xA9,0x14,0x96,0x35,0x4F,0xA7,0x46,0xDE,0x8D,0xB6,0x36,0x17,0xE6};
static const uint8_t IID_IAudioClient[16] = {0x4C,0xAD,0xB9,0x1C,0x32,0x4C,0xBA,0x4D,0xF5,0xC2,0x78,0xB1,0x68,0xA7,0x03,0xB2};
static const uint8_t IID_IAudioRenderClient[16] = {0xF3,0xAC,0x47,0xF9,0x8A,0x96,0xC9,0x45,0x8B,0x89,0x4A,0x0A,0x86,0xC4,0xD6,0xB7};

extern void void_audio_output(const int16_t *frames, uint32_t count);

static uint32_t unknown_query(uint32_t this_ptr, uint32_t riid, uint32_t out, const uint8_t *iid)
{
    if (!out) return E_POINTER_;
    if (void_guid_equal(riid, IID_IUnknown_WASAPI) || (iid && void_guid_equal(riid, iid))) {
        void_com_addref((void_com_object *)void_com_host(this_ptr));
        MEM32(out) = this_ptr;
        return S_OK_;
    }
    MEM32(out) = 0;
    return E_NOINTERFACE_;
}

/* ------------------------------------------------------------ enumerator */
COM_METHOD(mme, QueryInterface, 3) { return unknown_query(THIS_, ARG(1), ARG(2), IID_IMMDeviceEnumerator); }
COM_METHOD(mme, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(mme, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(mme, GetDefaultAudioEndpoint, 4)
{
    extern uint32_t bioshock_wasapi_device_create(void);
    void_log("[wasapi] GetDefaultAudioEndpoint flow=%u role=%u out=%08X", ARG(1), ARG(2), ARG(3));
    if (!ARG(3)) return E_POINTER_;
    MEM32(ARG(3)) = bioshock_wasapi_device_create();
    void_log("[wasapi] endpoint=%08X", MEM32(ARG(3)));
    return MEM32(ARG(3)) ? S_OK_ : E_OUTOFMEMORY_;
}
COM_METHOD(mme, EnumAudioEndpoints, 4)
{
    extern uint32_t bioshock_wasapi_collection_create(void);
    void_log("[wasapi] EnumAudioEndpoints out=%08X", ARG(3));
    if (!ARG(3)) return E_POINTER_;
    MEM32(ARG(3)) = bioshock_wasapi_collection_create();
    void_log("[wasapi] collection=%08X", MEM32(ARG(3)));
    return MEM32(ARG(3)) ? S_OK_ : E_OUTOFMEMORY_;
}
COM_METHOD(mme, GetDevice, 3) { return E_NOTIMPL_; }
COM_METHOD(mme, RegisterCallback, 2) { return S_OK_; }
COM_METHOD(mme, UnregisterCallback, 2) { return S_OK_; }
static const void_fn mme_methods[] = {
    mme_QueryInterface, mme_AddRef, mme_Release, mme_GetDefaultAudioEndpoint,
    mme_EnumAudioEndpoints, mme_GetDevice, mme_RegisterCallback, mme_UnregisterCallback,
};
static void_com_class mme_class = { "IMMDeviceEnumerator", 8, mme_methods, 0 };
uint32_t bioshock_mmdevice_enumerator_create(void)
{
    void_com_object *object = (void_com_object *)calloc(1, sizeof *object);
    return object ? void_com_wrap(object, &mme_class, 0) : 0;
}

COM_METHOD(dc, QueryInterface, 3) { return unknown_query(THIS_, ARG(1), ARG(2), NULL); }
COM_METHOD(dc, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(dc, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(dc, GetCount, 2) { void_log("[wasapi] collection GetCount"); MEM32(ARG(1)) = 1; return S_OK_; }
COM_METHOD(dc, Item, 3)
{
    extern uint32_t bioshock_wasapi_device_create(void);
    void_log("[wasapi] collection Item index=%u out=%08X", ARG(1), ARG(2));
    if (ARG(1) || !ARG(2)) return 0x80070057u;
    MEM32(ARG(2)) = bioshock_wasapi_device_create();
    void_log("[wasapi] collection device=%08X", MEM32(ARG(2)));
    return MEM32(ARG(2)) ? S_OK_ : E_OUTOFMEMORY_;
}
static const void_fn dc_methods[] = { dc_QueryInterface, dc_AddRef, dc_Release, dc_GetCount, dc_Item };
static void_com_class dc_class = { "IMMDeviceCollection", 5, dc_methods, 0 };
uint32_t bioshock_wasapi_collection_create(void)
{
    void_com_object *object = (void_com_object *)calloc(1, sizeof *object);
    return object ? void_com_wrap(object, &dc_class, 0) : 0;
}

/* --------------------------------------------------------------- property */
static uint32_t make_bstr(const char *text)
{
    uint32_t units = void_utf8_to_gw(text, 0, 0), block = void_galloc(4u + 2u * units);
    if (!block) return 0;
    MEM32(block) = 2u * (units - 1);
    void_utf8_to_gw(text, block + 4, units);
    return block + 4;
}
COM_METHOD(ps, QueryInterface, 3) { return unknown_query(THIS_, ARG(1), ARG(2), NULL); }
COM_METHOD(ps, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(ps, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(ps, GetCount, 2) { MEM32(ARG(1)) = 2; return S_OK_; }
COM_METHOD(ps, GetAt, 3)
{
    memset(GPTR(ARG(1)), 0, 20);
    MEM32(ARG(1) + 16) = ARG(2) == 0 ? 2 : 14;
    return S_OK_;
}
COM_METHOD(ps, GetValue, 3)
{
    void_log("[wasapi] property GetValue key=%08X variant=%08X", ARG(1), ARG(2));
    memset(GPTR(ARG(2)), 0, 16);
    MEM16(ARG(2)) = 8; /* VT_BSTR */
    MEM32(ARG(2) + 8) = make_bstr("Android Audio");
    return MEM32(ARG(2) + 8) ? S_OK_ : E_OUTOFMEMORY_;
}
static const void_fn ps_methods[] = { ps_QueryInterface, ps_AddRef, ps_Release, ps_GetCount, ps_GetAt, ps_GetValue };
static void_com_class ps_class = { "IPropertyStore", 5, ps_methods, 0 };
static uint32_t property_create(void)
{
    void_com_object *object = (void_com_object *)calloc(1, sizeof *object);
    return object ? void_com_wrap(object, &ps_class, 0) : 0;
}

/* ----------------------------------------------------------- audio client */
typedef struct {
    void_com_object com;
    uint32_t rate, channels, frames, data, playing;
} audio_client;

static void audio_destroy(void_com_object *object)
{
    audio_client *client = (audio_client *)object;
    if (client->data) void_gfree(client->data);
    free(client);
}

COM_METHOD(ac, QueryInterface, 3) { return unknown_query(THIS_, ARG(1), ARG(2), IID_IAudioClient); }
COM_METHOD(ac, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(ac, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(ac, Initialize, 7)
{
    audio_client *client = (audio_client *)void_com_host(THIS_);
    uint32_t format = ARG(5);
    if (!format || !void_vm_readable(format, 18)) return E_POINTER_;
    uint32_t tag = MEM16(format), channels = MEM16(format + 2), rate = MEM32(format + 4), bits = MEM16(format + 14);
    if ((tag != 1 && tag != 0xFFFEu) || !channels || channels > 2 || bits != 16 || rate < 8000 || rate > 96000)
        return 0x80070057u;
    client->channels = channels;
    client->rate = rate;
    client->frames = 4800;
    if (!client->data) client->data = void_galloc(client->frames * channels * 2u);
    return client->data ? S_OK_ : E_OUTOFMEMORY_;
}
COM_METHOD(ac, GetBufferSize, 2) { MEM32(ARG(1)) = ((audio_client *)void_com_host(THIS_))->frames; return S_OK_; }
COM_METHOD(ac, GetStreamLatency, 2) { MEM32(ARG(1)) = 100000; return S_OK_; }
COM_METHOD(ac, GetCurrentPadding, 2) { MEM32(ARG(1)) = 0; return S_OK_; }
COM_METHOD(ac, IsFormatSupported, 4)
{
    uint32_t format = ARG(2);
    if (!format || !void_vm_readable(format, 18)) return E_POINTER_;
    uint32_t tag = MEM16(format), channels = MEM16(format + 2), rate = MEM32(format + 4), bits = MEM16(format + 14);
    int valid = (tag == 1 || tag == 0xFFFEu) && channels >= 1 && channels <= 2 && bits == 16 && rate >= 8000 && rate <= 96000;
    if (valid) return S_OK_;
    if (ARG(3)) {
        memset(GPTR(ARG(3)), 0, 18);
        MEM16(ARG(3)) = 1;
        MEM16(ARG(3) + 2) = 2;
        MEM32(ARG(3) + 4) = 48000;
        MEM32(ARG(3) + 8) = 192000;
        MEM16(ARG(3) + 12) = 4;
        MEM16(ARG(3) + 14) = 16;
    }
    return 1; /* S_FALSE */
}
COM_METHOD(ac, GetMixFormat, 2)
{
    uint32_t out = ARG(1);
    if (!out || !void_vm_writable(out, 18)) return E_POINTER_;
    memset(GPTR(out), 0, 18);
    MEM16(out) = 1;
    MEM16(out + 2) = 2;
    MEM32(out + 4) = 48000;
    MEM32(out + 8) = 192000;
    MEM16(out + 12) = 4;
    MEM16(out + 14) = 16;
    return S_OK_;
}
COM_METHOD(ac, GetDevicePeriod, 3)
{
    if (ARG(1)) MEM32(ARG(1)) = 100000;
    if (ARG(2)) MEM32(ARG(2)) = 30000;
    return S_OK_;
}
COM_METHOD(ac, Start, 1) { ((audio_client *)void_com_host(THIS_))->playing = 1; return S_OK_; }
COM_METHOD(ac, Stop, 1) { ((audio_client *)void_com_host(THIS_))->playing = 0; return S_OK_; }
COM_METHOD(ac, Reset, 1) { ((audio_client *)void_com_host(THIS_))->playing = 0; return S_OK_; }
COM_METHOD(ac, SetEventHandle, 2) { return S_OK_; }
COM_METHOD(ac, GetService, 3)
{
    extern uint32_t bioshock_wasapi_render_create(audio_client *client);
    if (!ARG(2)) return E_POINTER_;
    if (!void_guid_equal(ARG(1), IID_IAudioRenderClient)) { MEM32(ARG(2)) = 0; return E_NOINTERFACE_; }
    MEM32(ARG(2)) = bioshock_wasapi_render_create((audio_client *)void_com_host(THIS_));
    return MEM32(ARG(2)) ? S_OK_ : E_OUTOFMEMORY_;
}
static const void_fn ac_methods[] = {
    ac_QueryInterface, ac_AddRef, ac_Release, ac_Initialize, ac_GetBufferSize, ac_GetStreamLatency,
    ac_GetCurrentPadding, ac_IsFormatSupported, ac_GetMixFormat, ac_GetDevicePeriod,
    ac_Start, ac_Stop, ac_Reset, ac_SetEventHandle, ac_GetService,
};
static void_com_class ac_class = { "IAudioClient", 15, ac_methods, 0 };
static uint32_t audio_client_create(void)
{
    audio_client *client = (audio_client *)calloc(1, sizeof *client);
    if (!client) return 0;
    uint32_t guest = void_com_wrap(&client->com, &ac_class, 0);
    client->com.destroy = audio_destroy;
    return guest;
}

/* ----------------------------------------------------------- render client */
typedef struct { void_com_object com; audio_client *client; } render_client;
COM_METHOD(rc, QueryInterface, 3) { return unknown_query(THIS_, ARG(1), ARG(2), IID_IAudioRenderClient); }
COM_METHOD(rc, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(rc, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(rc, GetBuffer, 4)
{
    render_client *render = (render_client *)void_com_host(THIS_);
    audio_client *client = render->client;
    if (!ARG(3) || !client->data) return E_POINTER_;
    uint32_t frames = ARG(1) > client->frames ? client->frames : ARG(1);
    if (!frames) return 0x80070057u;
    MEM32(ARG(3)) = client->data;
    return S_OK_;
}
COM_METHOD(rc, ReleaseBuffer, 3)
{
    render_client *render = (render_client *)void_com_host(THIS_);
    audio_client *client = render->client;
    if (!client->playing || !ARG(1)) return S_OK_;
    uint32_t frames = ARG(1) > client->frames ? client->frames : ARG(1);
    if (client->channels == 2) {
        void_audio_output((const int16_t *)GPTR(client->data), frames);
    } else {
        int16_t stereo[9600];
        if (frames > 4800) frames = 4800;
        for (uint32_t i = 0; i < frames; ++i) {
            int16_t sample = (int16_t)MEM16(client->data + 2u * i);
            stereo[2 * i] = stereo[2 * i + 1] = sample;
        }
        void_audio_output(stereo, frames);
    }
    return S_OK_;
}
static const void_fn rc_methods[] = { rc_QueryInterface, rc_AddRef, rc_Release, rc_GetBuffer, rc_ReleaseBuffer };
static void_com_class rc_class = { "IAudioRenderClient", 4, rc_methods, 0 };
uint32_t bioshock_wasapi_render_create(audio_client *client)
{
    render_client *render = (render_client *)calloc(1, sizeof *render);
    if (!render) return 0;
    render->client = client;
    return void_com_wrap(&render->com, &rc_class, 0);
}

/* ---------------------------------------------------------------- endpoint */
COM_METHOD(md, QueryInterface, 3) { return unknown_query(THIS_, ARG(1), ARG(2), NULL); }
COM_METHOD(md, AddRef, 1) { return void_com_addref((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(md, Release, 1) { return void_com_release((void_com_object *)void_com_host(THIS_)); }
COM_METHOD(md, Activate, 5)
{
    void_log("[wasapi] IMMDevice::Activate out=%08X", ARG(4));
    if (!ARG(4)) return E_POINTER_;
    if (!void_guid_equal(ARG(1), IID_IAudioClient)) { MEM32(ARG(4)) = 0; return E_NOINTERFACE_; }
    MEM32(ARG(4)) = audio_client_create();
    void_log("[wasapi] IAudioClient=%08X", MEM32(ARG(4)));
    return MEM32(ARG(4)) ? S_OK_ : E_OUTOFMEMORY_;
}
COM_METHOD(md, OpenPropertyStore, 3)
{
    void_log("[wasapi] OpenPropertyStore out=%08X", ARG(2));
    if (!ARG(2)) return E_POINTER_;
    MEM32(ARG(2)) = property_create();
    return MEM32(ARG(2)) ? S_OK_ : E_OUTOFMEMORY_;
}
static void md_GetId(void)
{
    void_log("[wasapi] IMMDevice::GetId out=%08X", ARG(1));
    if (ARG(1) == 0x110C13CCu || ARG(1) == 0x110C13E0u) {
        if (!ARG(2)) { g_eax = E_POINTER_; g_esp += 12; return; }
        memset(GPTR(ARG(2)), 0, 16);
        MEM16(ARG(2)) = 8;
        MEM32(ARG(2) + 8) = make_bstr("Android Audio");
        void_log("[wasapi] property value type=%u bstr=%08X var22c=%08X var228=%08X",
            MEM16(ARG(2)), MEM32(ARG(2) + 8), MEM32(g_ebp - 0x22Cu), MEM32(g_ebp - 0x228u));
        g_eax = MEM32(ARG(2) + 8) ? S_OK_ : E_OUTOFMEMORY_;
        g_esp += 12;
        return;
    }
    if (!ARG(1)) { g_eax = E_POINTER_; g_esp += 8; return; }
    MEM32(ARG(1)) = make_bstr("default");
    g_eax = MEM32(ARG(1)) ? S_OK_ : E_OUTOFMEMORY_;
    g_esp += 8;
}
COM_METHOD(md, GetState, 2) { MEM32(ARG(1)) = 1; return S_OK_; }
static const void_fn md_methods[] = { md_QueryInterface, md_AddRef, md_Release, md_Activate, md_OpenPropertyStore, md_GetId, md_GetState };
static void_com_class md_class = { "IMMDevice", 6, md_methods, 0 };
uint32_t bioshock_wasapi_device_create(void)
{
    void_com_object *object = (void_com_object *)calloc(1, sizeof *object);
    return object ? void_com_wrap(object, &md_class, 0) : 0;
}

WINAPI_EXPORT("ole32.dll", CoSetProxyBlanket, 8) { return S_OK_; }
WINAPI_EXPORT("ole32.dll", PropVariantClear, 1)
{
    uint32_t variant = ARG(0);
    if (!variant || !void_vm_readable(variant, 16)) return E_POINTER_;
    if (MEM16(variant) == 8 && MEM32(variant + 8)) {
        uint32_t bstr = MEM32(variant + 8);
        if (bstr >= 4) void_heap_free(void_process_heap(), bstr - 4);
    }
    memset(GPTR(variant), 0, 16);
    return S_OK_;
}
