/* 32-bit MSACM subset for FMOD's compressed WAV streams. */
#include "host.h"

#define ACMERR_NOTPOSSIBLE 0x200u
#define ACMERR_UNPREPARED 0x203u
#define ACMERR_INVALIDSIZE 0x214u
#define ACM_STREAM_PREPARED 0x10000u

typedef struct acm_stream {
    uint32_t id, tag, channels, source_align, samples_per_block;
    struct acm_stream *next;
} acm_stream;

static acm_stream *streams;
static pthread_mutex_t streams_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t next_stream_id = 0xAC100000u;

static const int16_t ima_steps[89] = {
    7,8,9,10,11,12,13,14,16,17,19,21,23,25,28,31,34,37,41,45,
    50,55,60,66,73,80,88,97,107,118,130,143,157,173,190,209,230,
    253,279,307,337,371,408,449,494,544,598,658,724,796,876,963,
    1060,1166,1282,1411,1552,1707,1878,2066,2272,2499,2749,3024,
    3327,3660,4026,4428,4871,5358,5894,6484,7132,7845,8630,9493,
    10442,11487,12635,13899,15289,16818,18500,20350,22385,24623,
    27086,29794,32767
};
static const int8_t ima_indices[16] = {-1,-1,-1,-1,2,4,6,8,-1,-1,-1,-1,2,4,6,8};
static const int16_t ms_coeff1[7] = {256,512,0,192,240,460,392};
static const int16_t ms_coeff2[7] = {0,-256,0,64,0,-208,-232};
static const int16_t ms_adapt[16] = {230,230,230,230,307,409,512,614,768,614,512,409,307,230,230,230};

static int16_t clamp16(int32_t value)
{
    return (int16_t)(value > 32767 ? 32767 : value < -32768 ? -32768 : value);
}

static acm_stream *stream_find(uint32_t id)
{
    for (acm_stream *stream = streams; stream; stream = stream->next)
        if (stream->id == id) return stream;
    return NULL;
}

static int format_values(uint32_t address, uint32_t *tag, uint32_t *channels, uint32_t *align, uint32_t *samples)
{
    if (!address || !void_vm_readable(address, 18) || (MEM16(address + 16) >= 2 && !void_vm_readable(address, 20))) return 0;
    uint32_t format = MEM16(address), count = MEM16(address + 2), block = MEM16(address + 12);
    if ((format != 2 && format != 0x11) || (count != 1 && count != 2) || block < (format == 2 ? 7u : 4u) * count) return 0;
    uint32_t computed = format == 2 ? 2 * (block - 7 * count) / count + 2 : 2 * (block - 4 * count) / count + 1;
    uint32_t declared = MEM16(address + 16) >= 2 ? MEM16(address + 18) : 0;
    if (!computed || (declared && declared > computed)) return 0;
    if (declared) computed = declared;
    if (tag) *tag = format;
    if (channels) *channels = count;
    if (align) *align = block;
    if (samples) *samples = computed;
    return 1;
}

WINAPI_EXPORT("msacm32.dll", acmFormatSuggest, 5)
{
    uint32_t source = ARG(1), destination = ARG(2), destination_size = ARG(3);
    if (!source || !destination || destination_size < 18 || !void_vm_readable(source, 18) ||
        !void_vm_writable(destination, 18)) return 11;
    if (!format_values(source, NULL, NULL, NULL, NULL) || MEM16(destination) != 1) return ACMERR_NOTPOSSIBLE;
    uint32_t channels = MEM16(source + 2), rate = MEM32(source + 4);
    MEM16(destination) = 1;
    MEM16(destination + 2) = (uint16_t)channels;
    MEM32(destination + 4) = rate;
    MEM32(destination + 8) = rate * channels * 2;
    MEM16(destination + 12) = (uint16_t)(channels * 2);
    MEM16(destination + 14) = 16;
    MEM16(destination + 16) = 0;
    return 0;
}

WINAPI_EXPORT("msacm32.dll", acmStreamOpen, 8)
{
    uint32_t out = ARG(0), source = ARG(2), destination = ARG(3);
    if (!out || !void_vm_writable(out, 4)) return 11;
    MEM32(out) = 0;
    if (ARG(4) || !format_values(source, NULL, NULL, NULL, NULL) || !void_vm_readable(destination, 18) ||
        MEM16(destination) != 1 || MEM16(destination + 14) != 16 ||
        MEM16(destination + 2) != MEM16(source + 2) || MEM32(destination + 4) != MEM32(source + 4)) return ACMERR_NOTPOSSIBLE;
    acm_stream *stream = (acm_stream *)calloc(1, sizeof *stream);
    if (!stream) return 8;
    stream->id = next_stream_id;
    next_stream_id += 16;
    stream->tag = MEM16(source);
    stream->channels = MEM16(source + 2);
    stream->source_align = MEM16(source + 12);
    format_values(source, NULL, NULL, NULL, &stream->samples_per_block);
    pthread_mutex_lock(&streams_lock);
    stream->next = streams;
    streams = stream;
    pthread_mutex_unlock(&streams_lock);
    MEM32(out) = stream->id;
    return 0;
}

WINAPI_EXPORT("msacm32.dll", acmStreamClose, 2)
{
    pthread_mutex_lock(&streams_lock);
    for (acm_stream **link = &streams; *link; link = &(*link)->next) {
        if ((*link)->id == ARG(0)) {
            acm_stream *stream = *link;
            *link = stream->next;
            free(stream);
            pthread_mutex_unlock(&streams_lock);
            return 0;
        }
    }
    pthread_mutex_unlock(&streams_lock);
    return 10;
}

WINAPI_EXPORT("msacm32.dll", acmStreamSize, 4)
{
    acm_stream *stream = stream_find(ARG(0));
    uint32_t out = ARG(2), bytes = ARG(1);
    if (!stream || !out || !void_vm_writable(out, 4)) return 10;
    uint32_t blocks = bytes / stream->source_align;
    if (ARG(3) & 2) return 11;
    if (ARG(3) & 1) {
        uint64_t frames = (uint64_t)blocks * stream->samples_per_block;
        uint64_t output = frames ? ((frames * stream->channels * 2 + stream->source_align - 1) / stream->source_align) * stream->source_align : 0;
        MEM32(out) = output > UINT32_MAX ? UINT32_MAX : (uint32_t)output;
    } else {
        uint64_t output = (uint64_t)blocks * stream->samples_per_block * stream->channels * 2;
        MEM32(out) = output > UINT32_MAX ? UINT32_MAX : (uint32_t)output;
    }
    return 0;
}

static int stream_header(acm_stream *stream, uint32_t header, int write)
{
    if (!stream || !header || !void_vm_readable(header, 44) || MEM32(header) < 44) return 0;
    uint32_t source = MEM32(header + 12), source_bytes = MEM32(header + 16);
    uint32_t destination = MEM32(header + 28), destination_bytes = MEM32(header + 32);
    if ((!source && source_bytes) || (!destination && destination_bytes)) return 0;
    if (!void_vm_readable(source, source_bytes)) return 0;
    if (write && !void_vm_writable(destination, destination_bytes)) return 0;
    return 1;
}

WINAPI_EXPORT("msacm32.dll", acmStreamPrepareHeader, 3)
{
    acm_stream *stream = stream_find(ARG(0));
    if (!stream_header(stream, ARG(1), 1)) return 11;
    if (ARG(2)) return 11;
    MEM32(ARG(1) + 4) |= ACM_STREAM_PREPARED;
    MEM32(ARG(1) + 20) = 0;
    MEM32(ARG(1) + 36) = 0;
    return 0;
}

WINAPI_EXPORT("msacm32.dll", acmStreamUnprepareHeader, 3)
{
    acm_stream *stream = stream_find(ARG(0));
    if (!stream_header(stream, ARG(1), 0)) return 11;
    if (ARG(2)) return 11;
    MEM32(ARG(1) + 4) &= ~ACM_STREAM_PREPARED;
    return 0;
}

static void decode_ima(acm_stream *stream, uint32_t source, uint32_t destination)
{
    for (uint32_t channel = 0; channel < stream->channels; ++channel) {
        uint32_t header = source + channel * 4;
        int32_t predictor = (int16_t)MEM16(header);
        int index = MEM8(header + 2);
        if (index < 0 || index > 88) index = 0;
        MEM16(destination + channel * 2) = (uint16_t)(int16_t)predictor;
        if (stream->channels == 1) {
            for (uint32_t offset = 4, frame = 1; offset < stream->source_align && frame < stream->samples_per_block; ++offset) {
                uint8_t byte = MEM8(source + offset);
                for (unsigned part = 0; part < 2 && frame < stream->samples_per_block; ++part, ++frame) {
                    int nibble = part ? byte & 15 : byte >> 4;
                    int step = ima_steps[index];
                    int difference = ((nibble & 7) * 2 + 1) * step / 8;
                    predictor = clamp16(predictor + (nibble & 8 ? -difference : difference));
                    MEM16(destination + frame * 2) = (uint16_t)(int16_t)predictor;
                    index += ima_indices[nibble];
                    if (index < 0) index = 0;
                    if (index > 88) index = 88;
                }
            }
        } else {
            uint32_t groups = (stream->source_align - 8) / 8;
            for (uint32_t group = 0, frame = 1; group < groups && frame < stream->samples_per_block; ++group) {
                uint32_t address = source + 8 + group * 8 + channel * 4;
                for (unsigned byte_index = 0; byte_index < 4 && frame < stream->samples_per_block; ++byte_index) {
                    uint8_t byte = MEM8(address + byte_index);
                    for (unsigned part = 0; part < 2 && frame < stream->samples_per_block; ++part, ++frame) {
                        int nibble = part ? byte & 15 : byte >> 4;
                        int step = ima_steps[index];
                        int difference = ((nibble & 7) * 2 + 1) * step / 8;
                        predictor = clamp16(predictor + (nibble & 8 ? -difference : difference));
                        MEM16(destination + (frame * 2 + channel) * 2) = (uint16_t)(int16_t)predictor;
                        index += ima_indices[nibble];
                        if (index < 0) index = 0;
                        if (index > 88) index = 88;
                    }
                }
            }
        }
    }
}

static int32_t ms_nibble(acm_stream *stream, uint32_t source, uint32_t channel, int nibble, int32_t *sample1, int32_t *sample2, int32_t *delta)
{
    uint32_t predictor_index = MEM8(source + channel);
    if (predictor_index >= 7) predictor_index = 0;
    int32_t prediction = (*sample1 * ms_coeff1[predictor_index] + *sample2 * ms_coeff2[predictor_index]) >> 8;
    if (nibble >= 8) nibble -= 16;
    int32_t sample = clamp16(prediction + nibble * *delta);
    *sample2 = *sample1;
    *sample1 = sample;
    *delta = (ms_adapt[nibble & 15] * *delta) / 256;
    if (*delta < 16) *delta = 16;
    return sample;
}

static void decode_ms(acm_stream *stream, uint32_t source, uint32_t destination)
{
    int32_t sample1[2] = {0}, sample2[2] = {0}, delta[2] = {0};
    for (uint32_t channel = 0; channel < stream->channels; ++channel) {
        if (stream->channels == 1) {
            delta[channel] = (int16_t)MEM16(source + 1);
            sample1[channel] = (int16_t)MEM16(source + 3);
            sample2[channel] = (int16_t)MEM16(source + 5);
        } else {
            delta[channel] = (int16_t)MEM16(source + 2 + channel * 2);
            sample1[channel] = (int16_t)MEM16(source + 6 + channel * 2);
            sample2[channel] = (int16_t)MEM16(source + 10 + channel * 2);
        }
        MEM16(destination + channel * 2) = (uint16_t)(int16_t)sample2[channel];
        MEM16(destination + (stream->channels + channel) * 2) = (uint16_t)(int16_t)sample1[channel];
    }
    uint32_t data = source + 7 * stream->channels, data_bytes = stream->source_align - 7 * stream->channels;
    if (stream->channels == 2) {
        for (uint32_t index = 0; index < data_bytes; ++index) {
            uint32_t frame = 2 + index;
            if (frame >= stream->samples_per_block) break;
            uint8_t byte = MEM8(data + index);
            int32_t left = ms_nibble(stream, source, 0, byte >> 4, &sample1[0], &sample2[0], &delta[0]);
            int32_t right = ms_nibble(stream, source, 1, byte & 15, &sample1[1], &sample2[1], &delta[1]);
            MEM16(destination + frame * 4) = (uint16_t)(int16_t)left;
            MEM16(destination + frame * 4 + 2) = (uint16_t)(int16_t)right;
        }
        return;
    }
    for (uint32_t index = 0; index < data_bytes * 2; ++index) {
        uint32_t frame = 2 + index / stream->channels;
        if (frame >= stream->samples_per_block) break;
        uint32_t channel = index % stream->channels;
        uint8_t byte = MEM8(data + index / 2);
        int nibble = (index & 1) ? byte & 15 : byte >> 4;
        int32_t sample = ms_nibble(stream, source, channel, nibble, &sample1[channel], &sample2[channel], &delta[channel]);
        MEM16(destination + (frame * stream->channels + channel) * 2) = (uint16_t)(int16_t)sample;
    }
}

WINAPI_EXPORT("msacm32.dll", acmStreamConvert, 3)
{
    acm_stream *stream = stream_find(ARG(0));
    uint32_t header = ARG(1);
    if (!stream_header(stream, header, 1)) return 11;
    if (!(MEM32(header + 4) & ACM_STREAM_PREPARED)) return ACMERR_UNPREPARED;
    if (ARG(2) & ~0x34u) return 11;
    uint32_t source = MEM32(header + 12), source_bytes = MEM32(header + 16);
    uint32_t destination = MEM32(header + 28), destination_bytes = MEM32(header + 32);
    uint32_t blocks = source_bytes / stream->source_align;
    uint32_t frames = destination_bytes / (stream->channels * 2);
    if (frames > blocks * stream->samples_per_block) frames = blocks * stream->samples_per_block;
    blocks = frames / stream->samples_per_block;
    if (frames && !blocks) return ACMERR_INVALIDSIZE;
    uint32_t written = 0;
    for (uint32_t block = 0; block < blocks; ++block) {
        uint32_t count = frames - written;
        if (count > stream->samples_per_block) count = stream->samples_per_block;
        if (!count) break;
        if (stream->tag == 0x11) decode_ima(stream, source + block * stream->source_align,
            destination + written * stream->channels * 2);
        else decode_ms(stream, source + block * stream->source_align,
            destination + written * stream->channels * 2);
        written += count;
    }
    MEM32(header + 20) = blocks * stream->source_align;
    MEM32(header + 36) = written * stream->channels * 2;
    return 0;
}
