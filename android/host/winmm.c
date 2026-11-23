#include "host.h"
#include <time.h>

#define WOM_DONE 0x3BDu
#define CALLBACK_FUNCTION 0x30000u
#define CALLBACK_EVENT 0x50000u

typedef struct {
    uint32_t handle, format, callback, instance, flags;
    uint32_t rate, channels, paused, active, running;
    uint32_t headers[64];
    unsigned head, count;
    uint64_t played;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
} wave_device;

static wave_device devices[8];
static uint32_t next_wave = 0x800001u;

extern void void_audio_output(const int16_t *frames, uint32_t count);

static void wave_callback(wave_device *device, uint32_t header)
{
    if (device->flags & CALLBACK_EVENT) {
        void_object *object = void_handle_get(device->callback, VOID_OBJ_EVENT);
        if (object) void_object_signal(object);
    }
    if (device->flags & CALLBACK_FUNCTION) {
        uint32_t args[5] = { device->handle, WOM_DONE, device->instance, header, 0 };
        void_call_guest(device->callback, 5, args, 1);
    }
}

static void *wave_worker(void *argument)
{
    wave_device *device = (wave_device *)argument;
    int16_t stereo[9600];
    while (device->running) {
        pthread_mutex_lock(&device->lock);
        while (device->running && (!device->count || device->paused)) pthread_cond_wait(&device->cond, &device->lock);
        if (!device->running) { pthread_mutex_unlock(&device->lock); break; }
        uint32_t header = device->headers[device->head];
        uint32_t data = MEM32(header), bytes = MEM32(header + 4), channels = device->channels;
        device->head = (device->head + 1) & 63;
        --device->count;
        MEM32(header + 16) |= 1u;
        pthread_mutex_unlock(&device->lock);
        uint32_t frames = channels ? bytes / (2u * channels) : 0;
        if (frames > 4800) frames = 4800;
        if (channels == 2) {
            void_audio_output((const int16_t *)GPTR(data), frames);
        } else if (channels == 1) {
            for (uint32_t i = 0; i < frames; ++i) {
                int16_t sample = (int16_t)MEM16(data + 2u * i);
                stereo[2 * i] = stereo[2 * i + 1] = sample;
            }
            void_audio_output(stereo, frames);
        }
        pthread_mutex_lock(&device->lock);
        device->played += frames;
        pthread_mutex_unlock(&device->lock);
        wave_callback(device, header);
    }
    return NULL;
}

static void wave_start(wave_device *device)
{
    if (device->running) return;
    device->running = 1;
    device->active = 1;
    pthread_create(&device->thread, NULL, wave_worker, device);
}

WINAPI_EXPORT("winmm.dll", waveOutOpen, 6)
{
    uint32_t out = ARG(0), format = ARG(2);
    if (!out || !format || !void_vm_readable(format, 18) || !void_vm_writable(out, 4)) return 1;
    MEM32(out) = 0;
    uint32_t tag = MEM16(format), channels = MEM16(format + 2), rate = MEM32(format + 4), bits = MEM16(format + 14);
    void_log("[winmm] waveOutOpen tag=%04X channels=%u rate=%u bits=%u callback=%08X flags=%08X", tag, channels, rate, bits, ARG(3), ARG(5));
    if ((tag != 1 && tag != 0xFFFEu) || bits != 16 || !channels || channels > 2 || rate < 8000 || rate > 96000) return 4;
    for (unsigned i = 0; i < 8; ++i) {
        if (devices[i].handle) continue;
        memset(&devices[i], 0, sizeof devices[i]);
        devices[i].handle = next_wave++;
        devices[i].format = tag;
        devices[i].callback = ARG(3);
        devices[i].instance = ARG(4);
        devices[i].flags = ARG(5);
        devices[i].rate = rate;
        devices[i].channels = channels;
        pthread_mutex_init(&devices[i].lock, NULL);
        pthread_cond_init(&devices[i].cond, NULL);
        MEM32(out) = devices[i].handle;
        wave_start(&devices[i]);
        return 0;
    }
    return 2;
}

WINAPI_EXPORT("winmm.dll", waveOutPrepareHeader, 3)
{
    wave_device *device = NULL;
    for (unsigned i = 0; i < 8; ++i) if (devices[i].handle == ARG(0)) device = &devices[i];
    if (!device || !ARG(1) || ARG(2) < 32 || !void_vm_readable(ARG(1), 32)) return 1;
    if (!MEM32(ARG(1)) || !MEM32(ARG(1) + 4) || !void_vm_readable(MEM32(ARG(1)), MEM32(ARG(1) + 4))) return 1;
    MEM32(ARG(1) + 16) = (MEM32(ARG(1) + 16) & ~1u) | 2u;
    return 0;
}

WINAPI_EXPORT("winmm.dll", waveOutWrite, 3)
{
    wave_device *device = NULL;
    for (unsigned i = 0; i < 8; ++i) if (devices[i].handle == ARG(0)) device = &devices[i];
    if (!device || !ARG(1) || ARG(2) < 32 || !void_vm_readable(ARG(1), 32)) return 1;
    if (!(MEM32(ARG(1) + 16) & 2u)) return 5;
    pthread_mutex_lock(&device->lock);
    if (device->count == 64) { pthread_mutex_unlock(&device->lock); return 6; }
    device->headers[(device->head + device->count) & 63] = ARG(1);
    ++device->count;
    pthread_cond_broadcast(&device->cond);
    pthread_mutex_unlock(&device->lock);
    return 0;
}

WINAPI_EXPORT("winmm.dll", waveOutUnprepareHeader, 3)
{
    if (!ARG(1) || ARG(2) < 32 || !void_vm_writable(ARG(1), 32)) return 1;
    MEM32(ARG(1) + 16) &= ~2u;
    return 0;
}

WINAPI_EXPORT("winmm.dll", waveOutGetPosition, 3)
{
    wave_device *device = NULL;
    for (unsigned i = 0; i < 8; ++i) if (devices[i].handle == ARG(0)) device = &devices[i];
    if (!device || !ARG(1) || !void_vm_writable(ARG(1), 8)) return 1;
    MEM32(ARG(1)) = 1;
    MEM32(ARG(1) + 4) = (uint32_t)device->played;
    return 0;
}

WINAPI_EXPORT("winmm.dll", waveOutPause, 1)
{
    for (unsigned i = 0; i < 8; ++i) if (devices[i].handle == ARG(0)) { devices[i].paused = 1; return 0; }
    return 1;
}
WINAPI_EXPORT("winmm.dll", waveOutRestart, 1)
{
    for (unsigned i = 0; i < 8; ++i) if (devices[i].handle == ARG(0)) {
        pthread_mutex_lock(&devices[i].lock);
        devices[i].paused = 0;
        pthread_cond_broadcast(&devices[i].cond);
        pthread_mutex_unlock(&devices[i].lock);
        return 0;
    }
    return 1;
}
WINAPI_EXPORT("winmm.dll", waveOutSetVolume, 2) { return 0; }
WINAPI_EXPORT("winmm.dll", waveOutReset, 1)
{
    for (unsigned i = 0; i < 8; ++i) if (devices[i].handle == ARG(0)) {
        pthread_mutex_lock(&devices[i].lock);
        devices[i].head = devices[i].count = 0;
        devices[i].paused = 0;
        pthread_cond_broadcast(&devices[i].cond);
        pthread_mutex_unlock(&devices[i].lock);
        return 0;
    }
    return 1;
}
WINAPI_EXPORT("winmm.dll", waveOutClose, 1)
{
    for (unsigned i = 0; i < 8; ++i) if (devices[i].handle == ARG(0)) {
        pthread_mutex_lock(&devices[i].lock);
        devices[i].running = 0;
        pthread_cond_broadcast(&devices[i].cond);
        pthread_mutex_unlock(&devices[i].lock);
        pthread_join(devices[i].thread, NULL);
        devices[i].handle = 0;
        return 0;
    }
    return 1;
}
WINAPI_EXPORT("winmm.dll", waveOutGetNumDevs, 0) { return 1; }
WINAPI_EXPORT("winmm.dll", waveOutGetDevCapsA, 3)
{
    if (!ARG(1) || ARG(2) < 48 || !void_vm_writable(ARG(1), 48)) return 1;
    memset(GPTR(ARG(1)), 0, 48);
    MEM16(ARG(1)) = 1;
    MEM16(ARG(1) + 2) = 1;
    MEM32(ARG(1) + 4) = 1;
    MEM32(ARG(1) + 8) = 0x3FFFFu;
    return 0;
}
WINAPI_EXPORT("winmm.dll", waveInGetNumDevs, 0) { return 0; }
WINAPI_EXPORT("winmm.dll", waveInOpen, 6) { return 2; }
WINAPI_EXPORT("winmm.dll", waveInClose, 1) { return 1; }
WINAPI_EXPORT("winmm.dll", waveInReset, 1) { return 1; }
WINAPI_EXPORT("winmm.dll", waveInStart, 1) { return 1; }
WINAPI_EXPORT("winmm.dll", waveInGetDevCapsA, 3) { return 2; }
WINAPI_EXPORT("winmm.dll", waveInPrepareHeader, 3) { return 2; }
WINAPI_EXPORT("winmm.dll", waveInUnprepareHeader, 3) { return 2; }
WINAPI_EXPORT("winmm.dll", waveInAddBuffer, 3) { return 2; }
WINAPI_EXPORT("winmm.dll", mciSendCommandA, 4) { return 1; }
