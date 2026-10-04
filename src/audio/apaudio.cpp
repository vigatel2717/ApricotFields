#include "audio/apaudio.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <math.h>
#include <new>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ApAudio's mixer. Three kinds of thread meet here (apaudio.h, Conventions),
// and none of them ever waits for another:
//   - Plays are records taken from a lock-free free list and pushed onto a
//     lock-free queue, which each render empties.
//   - Stops and changes - to voices, buses and modifiers - are single atomic
//     words on the thing they change ("tagged words" below), so they need no
//     queue space and the latest wins.
//   - What the control thread builds - the bus tree, modifiers, concurrency
//     groups, and apaudio_mixer_update()'s ranking - is posted to the audio
//     thread as whole copies through a triple buffer; what the audio thread
//     knows that ranking needs is published back in atomics.
// The audio thread never allocates or frees: everything is sized at creation.
//
// Where this differs from APAUDIO_TODO.md's implementation notes:
//   - A voice's direction is one word like its other settings, but as its
//     generation plus quantized angles rather than two floats: the generation
//     is what keeps a stale ID off the record's next voice.
//   - Ranking is posted through three buffers, not two, so neither thread
//     waits when the other is slow or stopped.
//   - There are max_fading_voices more voice records than max_voices +
//     queue_capacity: a voice ended to make room keeps its record while it
//     fades.
//   - Sinc keeps no history: sounds are whole in memory, so it reads them
//     directly.

// ---- Errors -------------------------------------------------------------------

static void clear_error(APAUDIO_ERROR *error)
{
    if (error)
        error->message[0] = '\0';
}

// Records [format] in [error] (if any) and returns [result].
static APRESULT fail(APAUDIO_ERROR *error, APRESULT result, const char *format, ...)
{
    if (error)
    {
        va_list args;
        va_start(args, format);
        vsnprintf(error->message, sizeof(error->message), format, args);
        va_end(args);
    }
    return result;
}

// ---- WAV ----------------------------------------------------------------------

// RIFF WAVE, little-endian throughout. Lenient where real files are sloppy
// and nothing is at risk - a RIFF size that disagrees with the file, a data
// chunk running past its end (a writer that never went back to fill the
// size in), a trailing partial frame - and strict about the format itself.

static uint16_t read_u16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

#define WAV_FORMAT_PCM 0x0001
#define WAV_FORMAT_IEEE_FLOAT 0x0003
#define WAV_FORMAT_EXTENSIBLE 0xFFFE

// KSDATAFORMAT_SUBTYPE_* after its first two bytes, which are the format tag.
static const uint8_t wav_subformat_tail[14] = {0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x80,
                                               0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};

static float wav_sample(const uint8_t *p, uint16_t format, uint32_t bytes)
{
    if (format == WAV_FORMAT_IEEE_FLOAT)
    {
        if (bytes == 4)
        {
            uint32_t bits = read_u32(p);
            float value;
            memcpy(&value, &bits, sizeof(value));
            return value;
        }
        uint64_t bits = (uint64_t)read_u32(p) | ((uint64_t)read_u32(p + 4) << 32);
        double value;
        memcpy(&value, &bits, sizeof(value));
        return (float)value;
    }
    switch (bytes)
    {
    case 1:
        return ((int)p[0] - 128) / 128.0f; // 8-bit PCM is unsigned
    case 2:
        return (int16_t)read_u16(p) / 32768.0f;
    case 3:
        // Sign-extend from the top byte.
        return (int32_t)(((uint32_t)p[0] << 8) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 24)) / 2147483648.0f;
    default:
        return (float)((int32_t)read_u32(p) / 2147483648.0);
    }
}

static APRESULT wav_decode(const uint8_t *data, uint64_t size, APAUDIO_PCM *out_pcm, APAUDIO_ERROR *error)
{
    if (size >= 4 && memcmp(data, "RF64", 4) == 0)
        return fail(error, APRESULT_UNSUPPORTED, "RF64 (WAV over 4 GB) isn't supported");
    if (size < 12 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        return fail(error, APRESULT_CORRUPT_DATA, "not a RIFF WAVE file");

    const uint8_t *fmt = nullptr;
    uint32_t fmt_size = 0;
    const uint8_t *samples = nullptr;
    uint64_t samples_size = 0;
    uint64_t offset = 12;
    while (offset + 8 <= size && !(fmt && samples))
    {
        const uint8_t *chunk = data + offset;
        uint64_t chunk_size = read_u32(chunk + 4);
        uint64_t available = size - offset - 8;
        if (memcmp(chunk, "fmt ", 4) == 0 && !fmt)
        {
            if (chunk_size > available)
                return fail(error, APRESULT_CORRUPT_DATA, "the fmt chunk runs past the end of the file");
            fmt = chunk + 8;
            fmt_size = (uint32_t)chunk_size;
        }
        else if (memcmp(chunk, "data", 4) == 0 && !samples)
        {
            samples = chunk + 8;
            samples_size = chunk_size < available ? chunk_size : available;
        }
        offset += 8 + chunk_size + (chunk_size & 1); // chunks are padded to an even size
    }
    if (!fmt)
        return fail(error, APRESULT_CORRUPT_DATA, "no fmt chunk");
    if (!samples)
        return fail(error, APRESULT_CORRUPT_DATA, "no data chunk");
    if (fmt_size < 16)
        return fail(error, APRESULT_CORRUPT_DATA, "the fmt chunk is %u bytes, under 16", fmt_size);

    uint16_t format = read_u16(fmt);
    uint32_t channel_count = read_u16(fmt + 2);
    uint32_t sample_rate = read_u32(fmt + 4);
    uint32_t block_align = read_u16(fmt + 12);
    uint32_t bits = read_u16(fmt + 14);
    if (format == WAV_FORMAT_EXTENSIBLE)
    {
        if (fmt_size < 40)
            return fail(error, APRESULT_CORRUPT_DATA, "an extensible fmt chunk is %u bytes, under 40", fmt_size);
        const uint8_t *subformat = fmt + 24;
        if (memcmp(subformat + 2, wav_subformat_tail, sizeof(wav_subformat_tail)) != 0)
            return fail(error, APRESULT_UNSUPPORTED, "an extensible WAV of a non-standard subformat");
        format = read_u16(subformat);
    }
    if (format != WAV_FORMAT_PCM && format != WAV_FORMAT_IEEE_FLOAT)
        return fail(error, APRESULT_UNSUPPORTED, "WAV format 0x%04X: only PCM and IEEE float are supported", format);
    bool bits_supported = format == WAV_FORMAT_PCM ? (bits == 8 || bits == 16 || bits == 24 || bits == 32)
                                                   : (bits == 32 || bits == 64);
    if (!bits_supported)
        return fail(error, APRESULT_UNSUPPORTED, "%u-bit %s samples aren't supported", bits,
                    format == WAV_FORMAT_PCM ? "PCM" : "float");
    if (channel_count == 0 || sample_rate == 0)
        return fail(error, APRESULT_CORRUPT_DATA, "%u channels at %u Hz", channel_count, sample_rate);
    uint32_t sample_bytes = bits / 8;
    if (block_align != channel_count * sample_bytes)
        return fail(error, APRESULT_CORRUPT_DATA, "a block align of %u, not %u", block_align,
                    channel_count * sample_bytes);

    uint64_t frame_count = samples_size / block_align; // a trailing partial frame is dropped
    uint64_t sample_count = frame_count * channel_count;
    if (sample_count > SIZE_MAX / sizeof(float))
        return fail(error, APRESULT_OUT_OF_MEMORY, "%llu samples don't fit in memory",
                    (unsigned long long)sample_count);
    float *decoded = nullptr;
    if (sample_count)
    {
        decoded = (float *)malloc((size_t)sample_count * sizeof(float));
        if (!decoded)
            return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for %llu samples",
                        (unsigned long long)sample_count);
        for (uint64_t i = 0; i < sample_count; i++)
            decoded[i] = wav_sample(samples + i * sample_bytes, format, sample_bytes);
    }
    out_pcm->samples = decoded;
    out_pcm->frame_count = frame_count;
    out_pcm->channel_count = channel_count;
    out_pcm->sample_rate = sample_rate;
    return APRESULT_OK;
}

// ---- Struct versions ----------------------------------------------------------

// The size of each versioned struct's first version: up to the end of its
// last original field. Frozen - when a field is added, these don't change.

#define APAUDIO_V1_SIZE(type, last_field) (offsetof(type, last_field) + sizeof(((type *)nullptr)->last_field))

constexpr size_t APAUDIO_MIXER_DESC_V1_SIZE = APAUDIO_V1_SIZE(APAUDIO_MIXER_DESC, gain);
constexpr size_t APAUDIO_BUS_DESC_V1_SIZE = APAUDIO_V1_SIZE(APAUDIO_BUS_DESC, rate);
constexpr size_t APAUDIO_MODIFIER_DESC_V1_SIZE = APAUDIO_V1_SIZE(APAUDIO_MODIFIER_DESC, trigger);
constexpr size_t APAUDIO_CONCURRENCY_DESC_V1_SIZE = APAUDIO_V1_SIZE(APAUDIO_CONCURRENCY_DESC, older_instance_gain);
constexpr size_t APAUDIO_SOUND_DESC_V1_SIZE = APAUDIO_V1_SIZE(APAUDIO_SOUND_DESC, bus);
constexpr size_t APAUDIO_PLAY_DESC_V1_SIZE = APAUDIO_V1_SIZE(APAUDIO_PLAY_DESC, loop);

// Copies a caller's versioned input struct into [out] at the current version:
// the fields its struct_size covers, zeros past them. False if it's smaller
// than the struct's first version.
template <typename T> static bool read_versioned(const T *in, size_t v1_size, T &out)
{
    if (!in || in->struct_size < v1_size)
        return false;
    memset(&out, 0, sizeof(T));
    memcpy(&out, in, in->struct_size < sizeof(T) ? in->struct_size : sizeof(T));
    out.struct_size = (uint32_t)sizeof(T);
    return true;
}

// ---- Small helpers ------------------------------------------------------------

constexpr float PI_F = 3.14159265358979323846f;
constexpr float RATE_MIN = 0.125f;
constexpr float RATE_MAX = 8.0f;
constexpr uint32_t SOUND_RATE_MIN = 1000;
constexpr uint32_t SOUND_RATE_MAX = 768000;
// How much more audible a voice must be to take a real slot from one of its
// own priority: 3 dB, so near-equal voices don't trade places every update.
constexpr float TIE_MARGIN = 1.4125375f;
// A source position: frames in the top 32 bits' worth, 32 bits of fraction.
constexpr uint64_t FIXED_ONE = 1ull << 32;

// Zeroed, default-constructed [count] of T, or null. Freed with free(): every
// T here is trivially destructible.
template <typename T> static T *alloc_array(size_t count)
{
    T *array = static_cast<T *>(calloc(count ? count : 1, sizeof(T)));
    if (!array)
        return nullptr;
    for (size_t i = 0; i < count; i++)
        new (array + i) T();
    return array;
}

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static float bits_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static bool valid_gain(float gain)
{
    return isfinite(gain) && gain >= 0.0f;
}

static bool valid_rate(float rate)
{
    return isfinite(rate) && rate >= RATE_MIN && rate <= RATE_MAX;
}

static float clamp_float(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

// Degrees, into [0, 360).
static float wrap360(float degrees)
{
    degrees = fmodf(degrees, 360.0f);
    if (degrees < 0.0f)
        degrees += 360.0f;
    return degrees >= 360.0f ? 0.0f : degrees;
}

// Degrees, into (-180, 180].
static float wrap180(float degrees)
{
    degrees = wrap360(degrees);
    return degrees > 180.0f ? degrees - 360.0f : degrees;
}

// ---- IDs ----------------------------------------------------------------------

// Every ID - sound, voice, bus, modifier, concurrency group - is a slot's
// index plus one in the low 32 bits (so it's never 0) and the slot's
// generation, which starts at 1, in the high 32.

static uint64_t make_id(uint32_t index, uint32_t generation)
{
    return ((uint64_t)generation << 32) | (uint64_t)(index + 1);
}

static bool split_id(uint64_t id, uint32_t slot_count, uint32_t &index, uint32_t &generation)
{
    uint32_t low = (uint32_t)id;
    if (low == 0 || low > slot_count)
        return false;
    index = low - 1;
    generation = (uint32_t)(id >> 32);
    return generation != 0;
}

static uint32_t next_generation(uint32_t generation)
{
    return generation + 1 == 0 ? 1 : generation + 1;
}

// ---- Tagged words -------------------------------------------------------------

// A request recorded on a voice, bus or modifier from any thread: one atomic
// word holding the slot's generation (high 32 bits) and a 32-bit payload. A
// writer only replaces a word of the generation its ID names, so a stale ID
// never reaches the slot's next occupant, and the word never needs queue
// space: the latest value wins.

// A payload no valid float setting has: "nothing new".
constexpr uint32_t FLOAT_NONE = 0x7FC00001u;

static uint64_t tagged(uint32_t generation, uint32_t payload)
{
    return ((uint64_t)generation << 32) | payload;
}

// Any thread. False if the word isn't [generation]'s any more.
static bool tagged_store(std::atomic<uint64_t> &word, uint32_t generation, uint32_t payload)
{
    uint64_t old = word.load(std::memory_order_relaxed);
    while ((uint32_t)(old >> 32) == generation)
    {
        if (word.compare_exchange_weak(old, tagged(generation, payload), std::memory_order_release,
                                       std::memory_order_relaxed))
            return true;
    }
    return false;
}

// Audio thread. Takes [generation]'s payload if it isn't [none], leaving
// [none]. One attempt: a value written meanwhile is taken by the next render.
static bool tagged_take(std::atomic<uint64_t> &word, uint32_t generation, uint32_t none, uint32_t &payload)
{
    uint64_t old = word.load(std::memory_order_acquire);
    if ((uint32_t)(old >> 32) != generation || (uint32_t)old == none)
        return false;
    if (!word.compare_exchange_strong(old, tagged(generation, none), std::memory_order_acquire,
                                      std::memory_order_relaxed))
        return false;
    payload = (uint32_t)old;
    return true;
}

// A stop: bit 31 set, the release in frames below it.
constexpr uint32_t STOP_REQUESTED = 0x80000000u;
constexpr uint32_t STOP_FRAMES_MASK = 0x7FFFFFFFu;

// A direction: azimuth as 16 bits of a turn, elevation as 14 bits of
// [-90, 90] degrees - both far finer than panning can be heard.
constexpr uint32_t DIRECTION_CHANGED = 0x80000000u;
constexpr uint32_t DIRECTION_PRESENT = 0x40000000u;

static uint32_t pack_direction(const APAUDIO_DIRECTION *direction)
{
    if (!direction)
        return DIRECTION_CHANGED;
    double turns = direction->azimuth / (2.0 * 3.14159265358979323846);
    turns -= floor(turns);
    uint32_t azimuth = (uint32_t)lround(turns * 65536.0) & 0xFFFFu;
    double up = direction->elevation / 3.14159265358979323846 + 0.5;
    up = up < 0.0 ? 0.0 : (up > 1.0 ? 1.0 : up);
    uint32_t elevation = (uint32_t)lround(up * 16383.0);
    return DIRECTION_CHANGED | DIRECTION_PRESENT | (elevation << 16) | azimuth;
}

// A bus's mute or pause: bit 31 changed, bit 30 the value, the fade in
// microseconds below.
constexpr uint32_t SWITCH_CHANGED = 0x80000000u;
constexpr uint32_t SWITCH_ON = 0x40000000u;
constexpr uint32_t SWITCH_FADE_MASK = 0x3FFFFFFFu;

// ---- Speaker layouts ----------------------------------------------------------

// Panning is by layer: speakers at ear height form a ring, and so do the ones
// above and below it. A direction is panned between the two speakers either
// side of it in a ring (constant power), and between the two layers either
// side of its elevation.

enum
{
    LAYER_BOTTOM,
    LAYER_EAR,
    LAYER_UPPER,
    LAYER_TOP,
    LAYER_COUNT
};

static const float layer_elevation[LAYER_COUNT] = {-45.0f, 0.0f, 45.0f, 90.0f};

struct Ring
{
    uint32_t first; // into Layout::ring_channel / ring_offset
    uint32_t count;
    float start; // the first speaker's azimuth, degrees
    float span;  // from the first speaker round to the last
    // The speakers leave an arc of 180 degrees or more empty (stereo's whole
    // rear): directions in it are folded back, mirrored, since they can't be
    // placed there.
    bool gap;
};

struct Layout
{
    uint32_t channel_count;
    SPUDAUDIO_CHANNEL_POSITION positions[SPUDAUDIO_MAX_CHANNELS];
    Ring rings[LAYER_COUNT];
    uint8_t ring_channel[SPUDAUDIO_MAX_CHANNELS];
    float ring_offset[SPUDAUDIO_MAX_CHANNELS]; // degrees clockwise of the ring's start, ascending
    int front_left, front_right, front_centre, mono; // channels, or -1
};

// Where a speaker is: its layer and azimuth. False for a channel that takes
// no panned sound (LFE, NA, AUX).
static bool speaker_place(SPUDAUDIO_CHANNEL_POSITION position, int &layer, float &azimuth)
{
    struct Place
    {
        SPUDAUDIO_CHANNEL_POSITION position;
        int layer;
        float azimuth;
    };
    static const Place places[] = {
        {SPUDAUDIO_CHANNEL_POSITION_MONO, LAYER_EAR, 0.0f},      {SPUDAUDIO_CHANNEL_POSITION_FL, LAYER_EAR, -30.0f},
        {SPUDAUDIO_CHANNEL_POSITION_FR, LAYER_EAR, 30.0f},       {SPUDAUDIO_CHANNEL_POSITION_FC, LAYER_EAR, 0.0f},
        {SPUDAUDIO_CHANNEL_POSITION_RL, LAYER_EAR, -135.0f},     {SPUDAUDIO_CHANNEL_POSITION_RR, LAYER_EAR, 135.0f},
        {SPUDAUDIO_CHANNEL_POSITION_FLC, LAYER_EAR, -15.0f},     {SPUDAUDIO_CHANNEL_POSITION_FRC, LAYER_EAR, 15.0f},
        {SPUDAUDIO_CHANNEL_POSITION_RC, LAYER_EAR, 180.0f},      {SPUDAUDIO_CHANNEL_POSITION_SL, LAYER_EAR, -90.0f},
        {SPUDAUDIO_CHANNEL_POSITION_SR, LAYER_EAR, 90.0f},       {SPUDAUDIO_CHANNEL_POSITION_TC, LAYER_TOP, 0.0f},
        {SPUDAUDIO_CHANNEL_POSITION_TFL, LAYER_UPPER, -45.0f},   {SPUDAUDIO_CHANNEL_POSITION_TFC, LAYER_UPPER, 0.0f},
        {SPUDAUDIO_CHANNEL_POSITION_TFR, LAYER_UPPER, 45.0f},    {SPUDAUDIO_CHANNEL_POSITION_TRL, LAYER_UPPER, -135.0f},
        {SPUDAUDIO_CHANNEL_POSITION_TRC, LAYER_UPPER, 180.0f},   {SPUDAUDIO_CHANNEL_POSITION_TRR, LAYER_UPPER, 135.0f},
        {SPUDAUDIO_CHANNEL_POSITION_RLC, LAYER_EAR, -160.0f},    {SPUDAUDIO_CHANNEL_POSITION_RRC, LAYER_EAR, 160.0f},
        {SPUDAUDIO_CHANNEL_POSITION_FLW, LAYER_EAR, -60.0f},     {SPUDAUDIO_CHANNEL_POSITION_FRW, LAYER_EAR, 60.0f},
        {SPUDAUDIO_CHANNEL_POSITION_FLH, LAYER_UPPER, -30.0f},   {SPUDAUDIO_CHANNEL_POSITION_FCH, LAYER_UPPER, 5.0f},
        {SPUDAUDIO_CHANNEL_POSITION_FRH, LAYER_UPPER, 30.0f},    {SPUDAUDIO_CHANNEL_POSITION_TFLC, LAYER_UPPER, -20.0f},
        {SPUDAUDIO_CHANNEL_POSITION_TFRC, LAYER_UPPER, 20.0f},   {SPUDAUDIO_CHANNEL_POSITION_TSL, LAYER_UPPER, -90.0f},
        {SPUDAUDIO_CHANNEL_POSITION_TSR, LAYER_UPPER, 90.0f},    {SPUDAUDIO_CHANNEL_POSITION_BC, LAYER_BOTTOM, 0.0f},
        {SPUDAUDIO_CHANNEL_POSITION_BLC, LAYER_BOTTOM, -30.0f},  {SPUDAUDIO_CHANNEL_POSITION_BRC, LAYER_BOTTOM, 30.0f},
    };
    for (const Place &place : places)
    {
        if (place.position == position)
        {
            layer = place.layer;
            azimuth = place.azimuth;
            return true;
        }
    }
    return false;
}

static bool known_position(SPUDAUDIO_CHANNEL_POSITION position)
{
    return (position >= SPUDAUDIO_CHANNEL_POSITION_MONO && position <= SPUDAUDIO_CHANNEL_POSITION_BRC) ||
           (position >= SPUDAUDIO_CHANNEL_POSITION_NA && position <= SPUDAUDIO_CHANNEL_POSITION_AUX_LAST);
}

// Checks [format] and works out its layout's rings.
static APRESULT layout_build(const APAUDIO_FORMAT &format, uint32_t max_frames, Layout &layout, APAUDIO_ERROR *error)
{
    if (format.sample_format < SPUDAUDIO_SAMPLE_FORMAT_S16 || format.sample_format > SPUDAUDIO_SAMPLE_FORMAT_F32)
        return fail(error, APRESULT_INVALID_ARGUMENT, "sample format %u isn't one the mixer writes", format.sample_format);
    if (format.sample_rate < SOUND_RATE_MIN || format.sample_rate > SOUND_RATE_MAX)
        return fail(error, APRESULT_INVALID_ARGUMENT, "a sample rate of %u Hz, outside %u to %u", format.sample_rate,
                    SOUND_RATE_MIN, SOUND_RATE_MAX);
    if (format.channel_count == 0 || format.channel_count > SPUDAUDIO_MAX_CHANNELS)
        return fail(error, APRESULT_INVALID_ARGUMENT, "%u channels, outside 1 to %d", format.channel_count,
                    SPUDAUDIO_MAX_CHANNELS);
    if (!format.positions)
        return fail(error, APRESULT_INVALID_ARGUMENT, "format.positions is null: the mixer needs a full channel layout");
    if (max_frames == 0)
        return fail(error, APRESULT_INVALID_ARGUMENT, "max_frames is 0");

    memset(&layout, 0, sizeof(layout));
    layout.channel_count = format.channel_count;
    layout.front_left = layout.front_right = layout.front_centre = layout.mono = -1;
    for (uint32_t c = 0; c < format.channel_count; c++)
    {
        SPUDAUDIO_CHANNEL_POSITION position = format.positions[c];
        if (!known_position(position))
            return fail(error, APRESULT_INVALID_ARGUMENT, "channel %u has no position (%u): the mixer needs a full layout",
                        c, position);
        if (position == SPUDAUDIO_CHANNEL_POSITION_MONO && format.channel_count != 1)
            return fail(error, APRESULT_INVALID_ARGUMENT, "channel %u is MONO in a %u-channel layout", c,
                        format.channel_count);
        for (uint32_t other = 0; other < c; other++)
        {
            if (position != SPUDAUDIO_CHANNEL_POSITION_NA && format.positions[other] == position)
                return fail(error, APRESULT_INVALID_ARGUMENT, "channels %u and %u have the same position (%u)", other, c,
                            position);
        }
        layout.positions[c] = position;
        if (position == SPUDAUDIO_CHANNEL_POSITION_FL)
            layout.front_left = (int)c;
        else if (position == SPUDAUDIO_CHANNEL_POSITION_FR)
            layout.front_right = (int)c;
        else if (position == SPUDAUDIO_CHANNEL_POSITION_FC)
            layout.front_centre = (int)c;
        else if (position == SPUDAUDIO_CHANNEL_POSITION_MONO)
            layout.mono = (int)c;
    }

    uint32_t used = 0;
    for (int layer = 0; layer < LAYER_COUNT; layer++)
    {
        // This layer's speakers, sorted by azimuth.
        float azimuths[SPUDAUDIO_MAX_CHANNELS];
        uint8_t channels[SPUDAUDIO_MAX_CHANNELS];
        uint32_t count = 0;
        for (uint32_t c = 0; c < format.channel_count; c++)
        {
            int speaker_layer;
            float azimuth;
            if (!speaker_place(layout.positions[c], speaker_layer, azimuth) || speaker_layer != layer)
                continue;
            azimuth = wrap360(azimuth);
            uint32_t at = count++;
            while (at > 0 && azimuths[at - 1] > azimuth)
            {
                azimuths[at] = azimuths[at - 1];
                channels[at] = channels[at - 1];
                at--;
            }
            azimuths[at] = azimuth;
            channels[at] = (uint8_t)c;
        }

        Ring &ring = layout.rings[layer];
        ring.first = used;
        ring.count = count;
        if (count == 0)
            continue;
        // The widest empty arc decides where the ring starts.
        uint32_t start = 0;
        float widest = 360.0f;
        if (count > 1)
        {
            widest = 0.0f;
            for (uint32_t i = 0; i < count; i++)
            {
                float arc = i + 1 < count ? azimuths[i + 1] - azimuths[i] : azimuths[0] + 360.0f - azimuths[i];
                if (arc > widest)
                {
                    widest = arc;
                    start = (i + 1) % count;
                }
            }
        }
        ring.gap = widest >= 179.99f;
        if (!ring.gap)
            start = 0;
        ring.start = azimuths[start];
        ring.span = ring.gap ? 360.0f - widest : 360.0f;
        for (uint32_t i = 0; i < count; i++)
        {
            uint32_t from = (start + i) % count;
            layout.ring_channel[used] = channels[from];
            layout.ring_offset[used] = wrap360(azimuths[from] - ring.start);
            used++;
        }
    }
    return APRESULT_OK;
}

// Adds [weight] of a sound at [azimuth] to the ring's speakers.
static void ring_pan(const Layout &layout, const Ring &ring, float azimuth, float weight, float *gains)
{
    const uint8_t *channels = layout.ring_channel + ring.first;
    const float *offsets = layout.ring_offset + ring.first;
    if (ring.count == 1)
    {
        gains[channels[0]] += weight;
        return;
    }
    float at; // degrees clockwise of the ring's start
    if (ring.gap)
    {
        // Mirror what's behind the speakers to the front of them, then spread
        // the half-circle that leaves across the speakers' span - on stereo,
        // ordinary panning, hard left and right at 90 degrees.
        float half = ring.span * 0.5f;
        float off_centre = wrap180(azimuth - (ring.start + half));
        if (off_centre > 90.0f)
            off_centre = 180.0f - off_centre;
        else if (off_centre < -90.0f)
            off_centre = -180.0f - off_centre;
        at = clamp_float(half + off_centre * (half / 90.0f), 0.0f, ring.span);
    }
    else
    {
        at = wrap360(azimuth - ring.start);
    }
    uint32_t i = 0;
    while (i + 1 < ring.count && at >= offsets[i + 1])
        i++;
    uint32_t j;
    float next;
    if (i + 1 < ring.count)
    {
        j = i + 1;
        next = offsets[j];
    }
    else if (ring.gap)
    {
        gains[channels[i]] += weight;
        return;
    }
    else
    {
        j = 0;
        next = 360.0f;
    }
    float arc = next - offsets[i];
    float t = arc > 1e-4f ? clamp_float((at - offsets[i]) / arc, 0.0f, 1.0f) : 0.5f;
    gains[channels[i]] += weight * cosf(t * PI_F * 0.5f);
    gains[channels[j]] += weight * sinf(t * PI_F * 0.5f);
}

// Adds a sound from [azimuth], [elevation] (degrees) to [gains], one per
// channel.
static void direction_pan(const Layout &layout, float azimuth, float elevation, float *gains)
{
    int below = -1, above = -1;
    for (int layer = 0; layer < LAYER_COUNT; layer++)
    {
        if (layout.rings[layer].count == 0)
            continue;
        if (layer_elevation[layer] <= elevation)
            below = layer;
        if (layer_elevation[layer] >= elevation && above < 0)
            above = layer;
    }
    if (below < 0)
        below = above;
    if (above < 0)
        above = below;
    if (below < 0)
        return; // nothing but LFE, NA and AUX
    if (below == above)
    {
        ring_pan(layout, layout.rings[below], azimuth, 1.0f, gains);
        return;
    }
    float t = (elevation - layer_elevation[below]) / (layer_elevation[above] - layer_elevation[below]);
    ring_pan(layout, layout.rings[below], azimuth, cosf(t * PI_F * 0.5f), gains);
    ring_pan(layout, layout.rings[above], azimuth, sinf(t * PI_F * 0.5f), gains);
}

// ---- Objects --------------------------------------------------------------------

// A value moving to a target at a fixed speed: a bus's gain, rate, mute and
// pause. Audio thread.
struct Fader
{
    float value{1.0f};
    float target{1.0f};
    float step{0.0f}; // per frame; 0 jumps

    void reset(float to)
    {
        value = target = to;
        step = 0.0f;
    }
    void set(float to, uint64_t frames)
    {
        target = to;
        step = frames ? fabsf(to - value) / (float)frames : 0.0f;
    }
    void advance(uint32_t frames)
    {
        if (value == target)
            return;
        float move = step * (float)frames;
        if (step <= 0.0f || fabsf(target - value) <= move)
            value = target;
        else
            value += target > value ? move : -move;
    }
};

enum : uint8_t
{
    SOUND_FREE,
    SOUND_LIVE,
    SOUND_RETIRED, // destroyed, waiting for the audio thread to be done with it
};

struct Sound
{
    // The ID that plays this slot's sound; 0 when it has none. What every
    // thread checks an APAUDIO_SOUND against.
    std::atomic<uint64_t> live_id{0};

    // Control thread.
    uint8_t state{SOUND_FREE};
    uint32_t generation{0};
    uint64_t retire_sequence{0}; // the render sequence that frees it

    // Written before live_id, and unchanged until the slot is collected.
    float *samples{nullptr};
    uint64_t frame_count{0};
    uint32_t channel_count{0};
    uint32_t sample_rate{0};
    float loudness{0.0f}; // RMS
    APAUDIO_VIRTUAL_BEHAVIOR virtual_behavior{0};
    APAUDIO_CONCURRENCY concurrency{0};
    APAUDIO_BUS bus{0};
};

enum : uint8_t
{
    VOICE_FREE,
    VOICE_WAITING, // for its start time
    VOICE_REAL,    // mixing, in a slot
    VOICE_VIRTUAL,
};

// What the audio thread tells apaudio_mixer_update() about a voice.
enum : uint32_t
{
    PUBLISHED_REAL = 1,      // holds a real slot
    PUBLISHED_ENDING = 2,    // releasing, or fading out to end
    PUBLISHED_HELD = 4,      // on a muted or paused bus
    PUBLISHED_WAITING = 8,   // not started
    PUBLISHED_SETTLED = 16,  // real for min_real_us
};

struct Voice
{
    // Shared.
    std::atomic<uint32_t> next{0}; // the free list's or the play queue's link: index + 1
    std::atomic<uint64_t> stop_word{0};
    std::atomic<uint64_t> gain_word{0};
    std::atomic<uint64_t> rate_word{0};
    std::atomic<uint64_t> direction_word{0};
    // For ranking: the voice's ID (0 when it has none), and its score,
    // priority and PUBLISHED_ flags.
    std::atomic<uint64_t> published_id{0};
    std::atomic<uint64_t> published_info{0};
    // The voice's ID from apaudio_play() until the audio thread frees its
    // record; 0 otherwise. apaudio_voice_is_live()'s.
    std::atomic<uint64_t> live_id{0};

    // apaudio_play()'s, written before the voice is queued; then the audio
    // thread's, which keeps them up to date with the voice's changes.
    uint32_t generation{0};
    uint64_t id{0};
    uint64_t sound_id{0};
    float gain{1.0f};
    float rate{1.0f};
    uint8_t priority{0};
    APAUDIO_VIRTUAL_BEHAVIOR behavior{0};
    APAUDIO_CONCURRENCY concurrency{0};
    APAUDIO_BUS bus{0};
    bool has_direction{false};
    float azimuth{0.0f}; // degrees
    float elevation{0.0f};
    uint64_t start_time_ns{0};
    bool loop{false};

    // Audio thread.
    uint8_t state{VOICE_FREE};
    bool dead{false};           // ended; swept off the active list next
    bool logical{false};        // counts towards max_voices
    bool stop_seen{false};
    bool releasing{false};      // stopped, fading out to end
    bool fading_out{false};     // lost its real slot: fading in a fading slot
    bool end_after_fade{false}; // ...to end, not to go virtual
    bool frozen{false};         // virtual and keeping its place whatever its behavior
    bool cut{false};            // its sound was destroyed: fades across this render, then ends
    bool snap{true};            // nothing mixed yet to ramp from
    bool pan_changed{false};
    bool ended{false}; // played to its sound's end
    const Sound *sound{nullptr};
    uint32_t bus_index{0};
    uint32_t bus_generation{0};
    uint32_t group_index{0};
    uint32_t group_generation{0}; // 0: no group
    int32_t slot{-1};
    uint32_t start_offset{0}; // into the render it starts in
    uint64_t position{0};     // source frames
    uint32_t fraction{0};     // of a source frame, 32 bits
    uint64_t step{FIXED_ONE}; // source frames per output frame, as last mixed
    float applied_gain{0.0f}; // gain x group x buses, as last mixed
    float group_gain{1.0f};
    float envelope{1.0f}; // fades and releases
    float envelope_step{0.0f};
    float score{0.0f}; // audibility
    uint64_t serial{0}; // start order
    uint64_t real_frames{0};
    uint64_t fade_serial{0};
};

struct Bus
{
    // Shared: any thread's requests, and what the audio thread reports.
    std::atomic<uint64_t> gain_word{0};
    std::atomic<uint64_t> gain_fade_word{0};
    std::atomic<uint64_t> rate_word{0};
    std::atomic<uint64_t> rate_fade_word{0};
    std::atomic<uint64_t> mute_word{0};
    std::atomic<uint64_t> pause_word{0};
    std::atomic<uint8_t> audible{0}; // real voices above the threshold, here or below

    // Audio thread.
    uint32_t generation{0};
    Fader gain, rate, mute, pause; // mute and pause: 1 open, 0 shut
    float modifier_gain{1.0f};
    float modifier_rate{1.0f};
    float effective_gain{1.0f}; // with every bus above it, at the render's end
    float effective_rate{1.0f};
    bool held{false}; // shut, or under a bus that is
    bool has_audible{false};
};

struct Modifier
{
    std::atomic<uint64_t> active_word{0}; // any thread: generation, and bit 0 active
    std::atomic<uint8_t> triggered{0};    // apaudio_mixer_update()'s

    // Audio thread.
    uint32_t generation{0};
    float amount{0.0f}; // 0 no effect .. 1 all of it
};

struct GroupState // audio thread
{
    uint32_t generation{0};
    bool started{false};
    uint64_t last_start{0}; // on the mixer's frame clock
};

// What the control thread decides and the audio thread reads: the bus tree,
// modifiers and concurrency groups. The control thread edits its own copy and
// posts whole copies; the audio thread takes the latest at each render.

struct BusConfig
{
    uint32_t generation;
    bool live;
    // A live bus's parent. A destroyed one's: where its voices go.
    uint32_t parent;
    float gain, rate; // its desc's
};

struct ModifierTarget
{
    uint32_t bus;
    float gain, rate;
};

struct ModifierConfig
{
    uint32_t generation;
    bool live; // false: destroyed, fading out
    bool has_trigger;
    uint32_t trigger;
    uint32_t fade_in_us, fade_out_us;
    uint32_t target_count;
};

struct GroupConfig
{
    uint32_t generation;
    bool live;
    uint32_t max_instances;
    APAUDIO_RESOLUTION resolution;
    uint32_t retrigger_us;
    float older_instance_gain;
};

struct Config
{
    BusConfig *buses{nullptr};
    uint32_t *order{nullptr}; // live buses, parents first
    uint32_t order_count{0};
    ModifierConfig *modifiers{nullptr};
    ModifierTarget *targets{nullptr}; // bus_count per modifier
    GroupConfig *groups{nullptr};
};

// One writer posting whole values to one reader, neither ever waiting: three
// buffers - the writer's, the reader's, and the latest one posted.
struct TripleBuffer
{
    static constexpr uint32_t FRESH = 4;
    std::atomic<uint32_t> posted{1};
    uint32_t write{0};
    uint32_t read{2};

    void post()
    {
        write = posted.exchange(write | FRESH, std::memory_order_acq_rel) & 3;
    }
    bool take()
    {
        if (!(posted.load(std::memory_order_relaxed) & FRESH))
            return false;
        read = posted.exchange(read, std::memory_order_acq_rel) & 3;
        return true;
    }
};

struct RankEntry // apaudio_mixer_update()'s
{
    uint32_t index;
    uint32_t generation;
    uint8_t priority;
    bool real;
    float score;
    float key;
};

// Windowed sinc: SINC_ZEROS zero crossings each side, SINC_RESOLUTION table
// entries per crossing. Playing faster than the mixer's rate stretches it (a
// lower cutoff), up to SINC_MAX_STRETCH.
constexpr int SINC_ZEROS = 8;
constexpr int SINC_RESOLUTION = 256;
constexpr float SINC_MAX_STRETCH = 16.0f;

struct apaudio_mixer_t
{
#ifdef _DEBUG
    const char *debug_name{nullptr};
#endif
    // Fixed at creation.
    uint32_t max_voices{0};
    uint32_t max_real_voices{0};
    uint32_t max_fading_voices{0};
    uint32_t max_sounds{0};
    uint32_t max_groups{0};
    uint32_t bus_count{0}; // max_buses and the master, slot 0
    uint32_t max_modifiers{0};
    uint32_t queue_capacity{0};
    uint32_t record_count{0}; // voices: max_voices + queue_capacity + max_fading_voices
    uint32_t slot_count{0};   // mixing slots: max_real_voices + max_fading_voices
    float virtual_threshold{0.0f};
    uint32_t transition_fade_us{0};
    uint32_t min_real_us{0};
    APAUDIO_RESAMPLER resampler{0};
    float *sinc{nullptr};

    // The format: the control thread's, changed only while nothing renders.
    SPUDAUDIO_SAMPLE_FORMAT sample_format{0};
    uint32_t sample_rate{0};
    uint32_t channel_count{0};
    bool interleaved{true};
    uint32_t max_frames{0};
    Layout layout{};
    float *mix{nullptr};         // [channel_count][max_frames]
    float *source{nullptr};      // [2][max_frames]: one voice, resampled
    float *pan{nullptr};         // [slot_count][2][channel_count]: each slot's speaker gains
    float *pan_scratch{nullptr}; // [2][channel_count]

    Voice *voices{nullptr};
    Sound *sounds{nullptr};
    Bus *buses{nullptr};
    Modifier *modifiers{nullptr};
    GroupState *groups{nullptr};

    // Shared.
    std::atomic<uint64_t> free_head{0};    // free voice records: a count against ABA, and index + 1
    std::atomic<uint32_t> pending_head{0}; // plays queued for the next render: index + 1
    std::atomic<uint32_t> queued{0};
    // Odd while rendering. A destroyed sound is freed once a render that
    // began after its ID went stale has finished.
    std::atomic<uint64_t> render_sequence{0};
    Config configs[3];
    TripleBuffer config_buffer;
    uint64_t *decisions[3]{nullptr, nullptr, nullptr}; // per voice record: generation, and DECISION_
    TripleBuffer decision_buffer;

    // Control thread.
    Config shadow;
    uint32_t sound_cursor{0}, bus_cursor{0}, modifier_cursor{0}, group_cursor{0};
    RankEntry *rank{nullptr};

    // Audio thread.
    const Config *config{nullptr};
    uint32_t *active{nullptr}; // voice records in use
    uint32_t active_count{0};
    uint32_t logical_count{0};
    uint32_t real_count{0};
    uint32_t fading_count{0};
    uint32_t *free_slots{nullptr};
    uint32_t free_slot_count{0};
    uint64_t clock{0}; // frames rendered
    uint64_t next_serial{1};
};

typedef apaudio_mixer_t Mixer;

constexpr uint64_t DECISION_REAL = 1;
constexpr uint64_t DECISION_VIRTUAL = 2;

static uint64_t frames_from_us(const Mixer *m, uint64_t us)
{
    return (us * m->sample_rate + 500000) / 1000000;
}

// ---- Voice records ------------------------------------------------------------

// Any thread.
static void free_push(Mixer *m, uint32_t index)
{
    uint64_t old = m->free_head.load(std::memory_order_relaxed);
    for (;;)
    {
        m->voices[index].next.store((uint32_t)old, std::memory_order_relaxed);
        uint64_t head = (((old >> 32) + 1) << 32) | (uint64_t)(index + 1);
        if (m->free_head.compare_exchange_weak(old, head, std::memory_order_release, std::memory_order_relaxed))
            return;
    }
}

// Any thread.
static bool free_pop(Mixer *m, uint32_t &index)
{
    uint64_t old = m->free_head.load(std::memory_order_acquire);
    for (;;)
    {
        uint32_t first = (uint32_t)old;
        if (first == 0)
            return false;
        uint32_t next = m->voices[first - 1].next.load(std::memory_order_relaxed);
        uint64_t head = (((old >> 32) + 1) << 32) | next;
        if (m->free_head.compare_exchange_weak(old, head, std::memory_order_acquire, std::memory_order_acquire))
        {
            index = first - 1;
            return true;
        }
    }
}

// ---- Config (control thread) -------------------------------------------------

static bool config_alloc(const Mixer *m, Config &config)
{
    config.buses = alloc_array<BusConfig>(m->bus_count);
    config.order = alloc_array<uint32_t>(m->bus_count);
    config.modifiers = alloc_array<ModifierConfig>(m->max_modifiers);
    config.targets = alloc_array<ModifierTarget>((size_t)m->max_modifiers * m->bus_count);
    config.groups = alloc_array<GroupConfig>(m->max_groups);
    return config.buses && config.order && config.modifiers && config.targets && config.groups;
}

static void config_free(Config &config)
{
    free(config.buses);
    free(config.order);
    free(config.modifiers);
    free(config.targets);
    free(config.groups);
    config = Config{};
}

// Posts the control thread's config to the audio thread.
static void config_post(Mixer *m)
{
    Config &from = m->shadow;
    // Live buses, each after its parent.
    from.order[0] = 0;
    from.order_count = 1;
    for (uint32_t at = 0; at < from.order_count; at++)
    {
        for (uint32_t b = 1; b < m->bus_count; b++)
        {
            if (from.buses[b].live && from.buses[b].parent == from.order[at])
                from.order[from.order_count++] = b;
        }
    }
    Config &to = m->configs[m->config_buffer.write];
    memcpy(to.buses, from.buses, m->bus_count * sizeof(BusConfig));
    memcpy(to.order, from.order, m->bus_count * sizeof(uint32_t));
    to.order_count = from.order_count;
    memcpy(to.modifiers, from.modifiers, m->max_modifiers * sizeof(ModifierConfig));
    memcpy(to.targets, from.targets, (size_t)m->max_modifiers * m->bus_count * sizeof(ModifierTarget));
    memcpy(to.groups, from.groups, m->max_groups * sizeof(GroupConfig));
    m->config_buffer.post();
}

// The live bus [bus] names, or 0 for the master. False if it's stale.
static bool find_bus(const Mixer *m, APAUDIO_BUS bus, uint32_t &index)
{
    if (bus == 0)
    {
        index = 0;
        return true;
    }
    uint32_t generation;
    return split_id(bus, m->bus_count, index, generation) && m->shadow.buses[index].live &&
           m->shadow.buses[index].generation == generation;
}

// ---- Sounds (control thread) ---------------------------------------------------

// Frees destroyed sounds the audio thread is done with.
static void collect_sounds(Mixer *m)
{
    uint64_t sequence = m->render_sequence.load();
    for (uint32_t i = 0; i < m->max_sounds; i++)
    {
        Sound &sound = m->sounds[i];
        if (sound.state != SOUND_RETIRED || sequence < sound.retire_sequence)
            continue;
        free(sound.samples);
        sound.samples = nullptr;
        sound.state = SOUND_FREE;
    }
}

// ---- Format buffers -------------------------------------------------------------

struct FormatBuffers
{
    float *mix{nullptr};
    float *source{nullptr};
    float *pan{nullptr};
    float *pan_scratch{nullptr};
};

static void format_buffers_free(FormatBuffers &buffers)
{
    free(buffers.mix);
    free(buffers.source);
    free(buffers.pan);
    free(buffers.pan_scratch);
    buffers = FormatBuffers{};
}

static bool format_buffers_alloc(const Mixer *m, uint32_t channel_count, uint32_t max_frames, FormatBuffers &buffers)
{
    buffers.mix = alloc_array<float>((size_t)channel_count * max_frames);
    buffers.source = alloc_array<float>((size_t)2 * max_frames);
    buffers.pan = alloc_array<float>((size_t)m->slot_count * 2 * channel_count);
    buffers.pan_scratch = alloc_array<float>((size_t)2 * channel_count);
    if (buffers.mix && buffers.source && buffers.pan && buffers.pan_scratch)
        return true;
    format_buffers_free(buffers);
    return false;
}

static void format_apply(Mixer *m, const APAUDIO_FORMAT &format, uint32_t max_frames, const Layout &layout,
                         FormatBuffers &buffers)
{
    free(m->mix);
    free(m->source);
    free(m->pan);
    free(m->pan_scratch);
    m->mix = buffers.mix;
    m->source = buffers.source;
    m->pan = buffers.pan;
    m->pan_scratch = buffers.pan_scratch;
    buffers = FormatBuffers{};
    m->sample_format = format.sample_format;
    m->sample_rate = format.sample_rate;
    m->channel_count = format.channel_count;
    m->interleaved = format.interleaved;
    m->max_frames = max_frames;
    m->layout = layout;
}

static float *sinc_table_build()
{
    int size = SINC_ZEROS * SINC_RESOLUTION;
    float *table = alloc_array<float>((size_t)size + 2);
    if (!table)
        return nullptr;
    // Kaiser window, beta 9: about -90 dB of stopband.
    auto bessel = [](double x) {
        double sum = 1.0, term = 1.0;
        for (int k = 1; k < 32; k++)
        {
            term *= (x / (2.0 * k)) * (x / (2.0 * k));
            sum += term;
        }
        return sum;
    };
    const double beta = 9.0;
    const double pi = 3.14159265358979323846;
    for (int i = 0; i < size; i++)
    {
        double x = (double)i / SINC_RESOLUTION;
        double sinc = i == 0 ? 1.0 : sin(pi * x) / (pi * x);
        double edge = x / SINC_ZEROS;
        table[i] = (float)(sinc * bessel(beta * sqrt(1.0 - edge * edge)) / bessel(beta));
    }
    return table; // the two entries past the end stay 0
}

// ---- Rendering (audio thread) ---------------------------------------------------

static uint64_t transition_frames(const Mixer *m)
{
    return frames_from_us(m, m->transition_fade_us);
}

static void release_slot(Mixer *m, Voice &v)
{
    if (v.slot < 0)
        return;
    m->free_slots[m->free_slot_count++] = (uint32_t)v.slot;
    v.slot = -1;
    if (v.fading_out)
        m->fading_count--;
    else
        m->real_count--;
    v.fading_out = false;
}

// Ends a voice. Its record is swept off the active list, and freed, later in
// the render.
static void end_voice(Mixer *m, Voice &v)
{
    if (v.dead)
        return;
    release_slot(m, v);
    if (v.logical)
        m->logical_count--;
    v.logical = false;
    v.dead = true;
}

static void make_virtual(Mixer *m, Voice &v, bool frozen)
{
    release_slot(m, v);
    v.state = VOICE_VIRTUAL;
    v.end_after_fade = false;
    v.envelope = 0.0f;
    v.envelope_step = 0.0f;
    v.frozen = frozen;
}

// A fade that has run its course, or is being cut short.
static void finish_fade(Mixer *m, Voice &v)
{
    if (v.releasing || v.end_after_fade || !v.fading_out)
        end_voice(m, v);
    else
        make_virtual(m, v, false);
}

// Takes a real voice's slot from it. It fades out in a fading slot - to
// virtual, or to its end if [kill], it's stopping, or its behavior is KILL -
// so whatever takes its place starts at once.
static void demote(Mixer *m, Voice &v, bool kill)
{
    bool ends = kill || v.releasing || v.behavior == APAUDIO_VIRTUAL_KILL;
    uint64_t fade = transition_frames(m);
    if (m->max_fading_voices == 0 || fade == 0 || v.envelope <= 0.0f)
    {
        if (ends)
            end_voice(m, v);
        else
            make_virtual(m, v, false);
        return;
    }
    if (m->fading_count == m->max_fading_voices)
    {
        // Every fading slot is in use: the oldest fade is cut short.
        Voice *oldest = nullptr;
        for (uint32_t i = 0; i < m->active_count; i++)
        {
            Voice &other = m->voices[m->active[i]];
            if (!other.dead && other.fading_out && (!oldest || other.fade_serial < oldest->fade_serial))
                oldest = &other;
        }
        if (oldest)
            finish_fade(m, *oldest);
    }
    m->real_count--;
    m->fading_count++;
    v.fading_out = true;
    v.end_after_fade = ends;
    v.fade_serial = m->next_serial++;
    if (ends && v.logical)
    {
        v.logical = false;
        m->logical_count--;
    }
    float step = -1.0f / (float)fade;
    if (!(v.envelope_step < step))
        v.envelope_step = step;
}

// Ends a voice to make room: at once if it isn't sounding, with a fade if it
// is.
static void kill_voice(Mixer *m, Voice &v)
{
    if (v.state != VOICE_REAL)
        end_voice(m, v);
    else if (!v.fading_out)
        demote(m, v, true);
    else
    {
        v.end_after_fade = true;
        if (v.logical)
        {
            v.logical = false;
            m->logical_count--;
        }
    }
}

// Stops a voice, fading out over [release] frames.
static void stop_voice(Mixer *m, Voice &v, uint64_t release)
{
    if (v.state != VOICE_REAL || release == 0 || v.envelope <= 0.0f)
    {
        end_voice(m, v);
        return;
    }
    v.releasing = true;
    float step = -v.envelope / (float)release;
    if (!(v.envelope_step < step))
        v.envelope_step = step;
}

static void take_slot(Mixer *m, Voice &v)
{
    v.slot = (int32_t)m->free_slots[--m->free_slot_count];
    m->real_count++;
    v.state = VOICE_REAL;
    v.snap = true;
    v.real_frames = 0;
    v.frozen = false;
}

// Source frames per output frame, 32.32.
static uint64_t voice_step(const Mixer *m, const Voice &v)
{
    float rate = clamp_float(v.rate * m->buses[v.bus_index].effective_rate, RATE_MIN, RATE_MAX);
    return (uint64_t)((double)v.sound->sample_rate * (double)rate / (double)m->sample_rate * 4294967296.0 + 0.5);
}

// Gives a virtual voice a real slot, fading in.
static void promote(Mixer *m, Voice &v)
{
    uint64_t fade = transition_frames(m);
    if (v.behavior == APAUDIO_VIRTUAL_RESTART)
    {
        v.position = 0;
        v.fraction = 0;
    }
    else if (v.behavior == APAUDIO_VIRTUAL_CONTINUE && !v.loop)
    {
        // Not worth coming back for less than a fade.
        double left = (double)(v.sound->frame_count - v.position) * 4294967296.0 / (double)voice_step(m, v);
        if (left < (double)fade)
        {
            end_voice(m, v);
            return;
        }
    }
    take_slot(m, v);
    v.envelope = fade ? 0.0f : 1.0f;
    v.envelope_step = fade ? 1.0f / (float)fade : 0.0f;
}

// Follows a voice's bus to a live one: a destroyed bus's voices move to its
// parent, and a bus that can't be traced any more is the master.
static void resolve_bus(const Mixer *m, Voice &v)
{
    const BusConfig *buses = m->config->buses;
    for (uint32_t hop = 0; hop < m->bus_count; hop++)
    {
        const BusConfig &bus = buses[v.bus_index];
        if (bus.generation == v.bus_generation && bus.live)
            return;
        if (bus.generation != v.bus_generation)
            break;
        v.bus_index = bus.parent;
        v.bus_generation = buses[bus.parent].generation;
    }
    v.bus_index = 0;
    v.bus_generation = buses[0].generation;
}

static void score_voice(const Mixer *m, Voice &v)
{
    v.score = v.gain * v.group_gain * m->buses[v.bus_index].effective_gain * v.sound->loudness;
}

static bool audible_enough(const Mixer *m, float score)
{
    return m->virtual_threshold <= 0.0f || score >= m->virtual_threshold;
}

// Applies the latest apaudio_voice_set_*() values.
static void take_changes(Voice &v)
{
    uint32_t payload;
    if (tagged_take(v.gain_word, v.generation, FLOAT_NONE, payload))
        v.gain = bits_float(payload);
    if (tagged_take(v.rate_word, v.generation, FLOAT_NONE, payload))
        v.rate = bits_float(payload);
    if (tagged_take(v.direction_word, v.generation, 0, payload))
    {
        v.has_direction = (payload & DIRECTION_PRESENT) != 0;
        v.azimuth = (float)(payload & 0xFFFFu) * (360.0f / 65536.0f);
        v.elevation = ((float)((payload >> 16) & 0x3FFFu) / 16383.0f - 0.5f) * 180.0f;
        v.pan_changed = true;
    }
}

static bool stop_requested(const Voice &v, uint64_t &release)
{
    uint64_t word = v.stop_word.load(std::memory_order_acquire);
    if ((uint32_t)(word >> 32) != v.generation || !(word & STOP_REQUESTED))
        return false;
    release = word & STOP_FRAMES_MASK;
    return true;
}

// The frame of this render a voice starts on. False if it starts later.
static bool start_offset(const Mixer *m, const Voice &v, uint32_t frames, bool host_valid, uint64_t host_ns,
                         uint32_t &offset)
{
    offset = 0;
    if (v.start_time_ns == 0 || !host_valid || v.start_time_ns <= host_ns)
        return true;
    // The nearest frame.
    double ahead = (double)(v.start_time_ns - host_ns) * (double)m->sample_rate / 1e9 + 0.5;
    if (ahead >= (double)frames)
        return false;
    offset = (uint32_t)ahead;
    return true;
}

static bool in_group(const Voice &v, uint32_t index, uint32_t generation)
{
    return !v.dead && v.group_generation == generation && v.group_index == index && v.state != VOICE_WAITING &&
           !v.releasing && !(v.fading_out && v.end_after_fade);
}

// Applies the voice's concurrency group's rules as it starts. False if they
// drop it.
static bool group_admit(Mixer *m, Voice &v, uint64_t now)
{
    v.group_generation = 0;
    APAUDIO_CONCURRENCY id = v.concurrency ? v.concurrency : v.sound->concurrency;
    uint32_t index, generation;
    if (!split_id(id, m->max_groups, index, generation))
        return true;
    const GroupConfig &group = m->config->groups[index];
    if (!group.live || group.generation != generation)
        return true; // destroyed: plays with no group
    GroupState &state = m->groups[index];
    if (state.generation != generation)
    {
        state.generation = generation;
        state.started = false;
    }
    if (group.retrigger_us && state.started && now - state.last_start < frames_from_us(m, group.retrigger_us))
        return false;

    uint32_t count = 0;
    Voice *oldest = nullptr, *quietest = nullptr, *lowest = nullptr;
    for (uint32_t i = 0; i < m->active_count; i++)
    {
        Voice &other = m->voices[m->active[i]];
        if (&other == &v || !in_group(other, index, generation))
            continue;
        count++;
        if (!oldest || other.serial < oldest->serial)
            oldest = &other;
        if (!quietest || other.score < quietest->score)
            quietest = &other;
        if (!lowest || other.priority < lowest->priority ||
            (other.priority == lowest->priority && other.serial < lowest->serial))
            lowest = &other;
    }
    if (count >= group.max_instances)
    {
        Voice *victim = nullptr;
        switch (group.resolution)
        {
        case APAUDIO_RESOLUTION_STOP_OLDEST:
            victim = oldest;
            break;
        case APAUDIO_RESOLUTION_STOP_QUIETEST:
            victim = quietest;
            break;
        case APAUDIO_RESOLUTION_STOP_LOWEST_PRIORITY:
            victim = lowest && lowest->priority <= v.priority ? lowest : nullptr;
            break;
        default:
            break;
        }
        if (!victim)
            return false;
        stop_voice(m, *victim, transition_frames(m));
    }
    if (group.older_instance_gain < 1.0f)
    {
        for (uint32_t i = 0; i < m->active_count; i++)
        {
            Voice &other = m->voices[m->active[i]];
            if (&other != &v && in_group(other, index, generation))
                other.group_gain *= group.older_instance_gain;
        }
    }
    state.started = true;
    state.last_start = now;
    v.group_index = index;
    v.group_generation = generation;
    return true;
}

// Makes room for one more logical voice. False if every voice there is
// outranks the new one.
static bool ensure_capacity(Mixer *m, const Voice &v)
{
    if (m->logical_count < m->max_voices)
        return true;
    Voice *weakest = nullptr;
    bool weakest_real = false;
    for (uint32_t i = 0; i < m->active_count; i++)
    {
        Voice &other = m->voices[m->active[i]];
        if (other.dead || !other.logical)
            continue;
        bool real = other.state == VOICE_REAL && !other.fading_out;
        bool weaker = !weakest || (real != weakest_real ? !real
                                   : other.priority != weakest->priority ? other.priority < weakest->priority
                                                                         : other.score < weakest->score);
        if (weaker)
        {
            weakest = &other;
            weakest_real = real;
        }
    }
    if (!weakest || v.priority < weakest->priority)
        return false;
    kill_voice(m, *weakest);
    return true;
}

// Starts a voice on frame [offset] of this render: real if it earns a slot,
// else virtual - which ends it if its behavior is KILL.
static void begin_voice(Mixer *m, Voice &v, uint32_t offset)
{
    v.serial = m->next_serial++;
    APAUDIO_BUS bus_id = v.bus ? v.bus : v.sound->bus;
    if (!split_id(bus_id, m->bus_count, v.bus_index, v.bus_generation))
    {
        v.bus_index = 0;
        v.bus_generation = m->config->buses[0].generation;
    }
    resolve_bus(m, v);
    score_voice(m, v);
    bool held = m->buses[v.bus_index].held;
    if (!held && audible_enough(m, v.score))
    {
        if (m->real_count >= m->max_real_voices)
        {
            // The lowest-ranked real voice gives way if this one outranks it.
            Voice *lowest = nullptr;
            for (uint32_t i = 0; i < m->active_count; i++)
            {
                Voice &other = m->voices[m->active[i]];
                if (other.dead || &other == &v || other.state != VOICE_REAL || other.fading_out || other.releasing)
                    continue;
                if (!lowest || other.priority < lowest->priority ||
                    (other.priority == lowest->priority && other.score < lowest->score))
                    lowest = &other;
            }
            bool settled = lowest && lowest->real_frames >= frames_from_us(m, m->min_real_us);
            if (lowest && (v.priority > lowest->priority ||
                           (v.priority == lowest->priority && settled && v.score > lowest->score * TIE_MARGIN)))
                demote(m, *lowest, false);
        }
        if (m->real_count < m->max_real_voices)
        {
            take_slot(m, v);
            v.envelope = 1.0f;
            v.envelope_step = 0.0f;
            v.start_offset = offset;
            return;
        }
    }
    if (v.behavior == APAUDIO_VIRTUAL_KILL)
    {
        end_voice(m, v);
        return;
    }
    v.state = VOICE_VIRTUAL;
    v.frozen = held;
}

// Takes a queued play. False if it's dropped before it exists.
static bool admit_voice(Mixer *m, uint32_t index, uint32_t frames, bool host_valid, uint64_t host_ns)
{
    Voice &v = m->voices[index];
    v.state = VOICE_WAITING;
    v.dead = v.logical = v.stop_seen = v.releasing = v.fading_out = v.end_after_fade = false;
    v.frozen = v.cut = v.pan_changed = v.ended = false;
    v.snap = true;
    v.group_generation = 0;
    v.slot = -1;
    v.start_offset = 0;
    v.position = 0;
    v.fraction = 0;
    v.applied_gain = 0.0f;
    v.group_gain = 1.0f;
    v.envelope = 1.0f;
    v.envelope_step = 0.0f;
    v.score = 0.0f;
    v.real_frames = 0;

    // The sound may have been destroyed since apaudio_play() checked.
    uint32_t sound_index, sound_generation;
    if (!split_id(v.sound_id, m->max_sounds, sound_index, sound_generation))
        return false;
    Sound &sound = m->sounds[sound_index];
    if (sound.live_id.load() != v.sound_id || sound.frame_count == 0)
        return false; // an empty sound has ended before it starts
    v.sound = &sound;
    uint64_t release;
    if (stop_requested(v, release))
        return false; // stopped before it started
    take_changes(v);
    if (v.behavior == APAUDIO_VIRTUAL_UNSPECIFIED)
        v.behavior = sound.virtual_behavior;

    uint32_t offset;
    bool starts = start_offset(m, v, frames, host_valid, host_ns, offset);
    if (starts && !group_admit(m, v, m->clock + offset))
        return false;
    if (!ensure_capacity(m, v))
        return false;
    m->active[m->active_count++] = index;
    v.logical = true;
    m->logical_count++;
    if (starts)
        begin_voice(m, v, offset);
    return true;
}

static void start_pending(Mixer *m, uint32_t frames, bool host_valid, uint64_t host_ns)
{
    // The queue is a stack: reverse it into the order the plays were made.
    uint32_t stack = m->pending_head.exchange(0, std::memory_order_acquire);
    uint32_t head = 0;
    while (stack)
    {
        Voice &v = m->voices[stack - 1];
        uint32_t next = v.next.load(std::memory_order_relaxed);
        v.next.store(head, std::memory_order_relaxed);
        head = stack;
        stack = next;
    }
    while (head)
    {
        uint32_t index = head - 1;
        head = m->voices[index].next.load(std::memory_order_relaxed);
        if (!admit_voice(m, index, frames, host_valid, host_ns))
        {
            m->voices[index].live_id.store(0, std::memory_order_release);
            free_push(m, index);
        }
        m->queued.fetch_sub(1, std::memory_order_release);
    }
}

// Frees the records of voices that have ended.
static void sweep(Mixer *m)
{
    for (uint32_t i = 0; i < m->active_count;)
    {
        uint32_t index = m->active[i];
        Voice &v = m->voices[index];
        if (!v.dead)
        {
            i++;
            continue;
        }
        m->active[i] = m->active[--m->active_count];
        v.published_id.store(0, std::memory_order_release);
        v.live_id.store(0, std::memory_order_release);
        v.state = VOICE_FREE;
        free_push(m, index);
    }
}

// Takes the buses' and modifiers' latest requests and moves every fade on by
// [frames], leaving each bus's effective gain and rate as of the render's end.
static void update_buses(Mixer *m, uint32_t frames)
{
    const Config &config = *m->config;
    for (uint32_t i = 0; i < config.order_count; i++)
    {
        uint32_t index = config.order[i];
        const BusConfig &desc = config.buses[index];
        Bus &bus = m->buses[index];
        if (bus.generation != desc.generation)
        {
            bus.generation = desc.generation;
            bus.gain.reset(desc.gain);
            bus.rate.reset(desc.rate);
            bus.mute.reset(1.0f);
            bus.pause.reset(1.0f);
        }
        uint32_t payload, fade;
        if (tagged_take(bus.gain_word, bus.generation, FLOAT_NONE, payload))
        {
            fade = (uint32_t)bus.gain_fade_word.load(std::memory_order_relaxed);
            bus.gain.set(bits_float(payload), frames_from_us(m, fade));
        }
        if (tagged_take(bus.rate_word, bus.generation, FLOAT_NONE, payload))
        {
            fade = (uint32_t)bus.rate_fade_word.load(std::memory_order_relaxed);
            bus.rate.set(bits_float(payload), frames_from_us(m, fade));
        }
        if (tagged_take(bus.mute_word, bus.generation, 0, payload))
            bus.mute.set(payload & SWITCH_ON ? 0.0f : 1.0f, frames_from_us(m, payload & SWITCH_FADE_MASK));
        if (tagged_take(bus.pause_word, bus.generation, 0, payload))
            bus.pause.set(payload & SWITCH_ON ? 0.0f : 1.0f, frames_from_us(m, payload & SWITCH_FADE_MASK));
        bus.modifier_gain = 1.0f;
        bus.modifier_rate = 1.0f;
    }

    for (uint32_t i = 0; i < m->max_modifiers; i++)
    {
        const ModifierConfig &desc = config.modifiers[i];
        if (desc.generation == 0)
            continue;
        Modifier &modifier = m->modifiers[i];
        if (modifier.generation != desc.generation)
        {
            modifier.generation = desc.generation;
            modifier.amount = 0.0f;
        }
        bool active = false;
        if (desc.live)
        {
            uint64_t word = modifier.active_word.load(std::memory_order_relaxed);
            active = ((uint32_t)(word >> 32) == desc.generation && (word & 1)) ||
                     (desc.has_trigger && modifier.triggered.load(std::memory_order_relaxed));
        }
        uint64_t fade = frames_from_us(m, active ? desc.fade_in_us : desc.fade_out_us);
        float move = fade ? (float)frames / (float)fade : 1.0f;
        modifier.amount = active ? fminf(1.0f, modifier.amount + move) : fmaxf(0.0f, modifier.amount - move);
        if (modifier.amount <= 0.0f)
            continue;
        const ModifierTarget *targets = config.targets + (size_t)i * m->bus_count;
        for (uint32_t t = 0; t < desc.target_count; t++)
        {
            Bus &bus = m->buses[targets[t].bus];
            bus.modifier_gain *= 1.0f + (targets[t].gain - 1.0f) * modifier.amount;
            bus.modifier_rate *= 1.0f + (targets[t].rate - 1.0f) * modifier.amount;
        }
    }

    for (uint32_t i = 0; i < config.order_count; i++)
    {
        uint32_t index = config.order[i];
        Bus &bus = m->buses[index];
        // Shut for the whole of this render: its last fade has finished.
        bus.held = (bus.mute.target == 0.0f && bus.mute.value == 0.0f) ||
                   (bus.pause.target == 0.0f && bus.pause.value == 0.0f);
        bus.gain.advance(frames);
        bus.rate.advance(frames);
        bus.mute.advance(frames);
        bus.pause.advance(frames);
        bus.effective_gain = bus.gain.value * bus.mute.value * bus.pause.value * bus.modifier_gain;
        bus.effective_rate = bus.rate.value * bus.modifier_rate;
        bus.has_audible = false;
        if (index != 0)
        {
            const Bus &parent = m->buses[config.buses[index].parent];
            bus.effective_gain *= parent.effective_gain;
            bus.effective_rate *= parent.effective_rate;
            bus.held = bus.held || parent.held;
        }
    }
}

// A voice already playing: what changed since the last render.
static void prepare_voice(Mixer *m, Voice &v, const uint64_t *decisions, uint32_t index)
{
    if (v.sound->live_id.load() != v.sound_id)
    {
        // Its sound was destroyed. One that's sounding fades across this
        // render rather than clicking; the sound's memory outlives it.
        if (v.state != VOICE_REAL)
        {
            end_voice(m, v);
            return;
        }
        v.cut = true;
    }
    uint64_t release;
    if (!v.stop_seen && stop_requested(v, release))
    {
        v.stop_seen = true;
        stop_voice(m, v, release);
        if (v.dead)
            return;
    }
    if (!v.releasing)
        take_changes(v);
    if (v.state == VOICE_WAITING)
        return;

    resolve_bus(m, v);
    score_voice(m, v);
    if (m->buses[v.bus_index].held)
    {
        // Muted or paused: it keeps its place, whatever its behavior, until
        // the bus opens again.
        if (v.state == VOICE_REAL)
        {
            if (v.releasing || v.end_after_fade)
                end_voice(m, v);
            else
                make_virtual(m, v, true);
        }
        else
        {
            v.frozen = true;
        }
        return;
    }
    if (!decisions || v.releasing || (uint32_t)(decisions[index] >> 32) != v.generation)
        return;
    if ((decisions[index] & 3) != DECISION_VIRTUAL)
        return;
    if (v.state == VOICE_REAL)
    {
        if (!v.fading_out)
            demote(m, v, false);
    }
    else if (v.behavior == APAUDIO_VIRTUAL_KILL)
    {
        end_voice(m, v);
    }
    else
    {
        v.frozen = false;
    }
}

// The voices the last update ranked real, now that the ones it ranked virtual
// have given up their slots.
static void promote_voice(Mixer *m, Voice &v, uint64_t decision)
{
    if (v.dead || v.releasing || (uint32_t)(decision >> 32) != v.generation || (decision & 3) != DECISION_REAL)
        return;
    if (m->buses[v.bus_index].held || m->real_count >= m->max_real_voices)
        return;
    if (v.state == VOICE_VIRTUAL)
    {
        promote(m, v);
    }
    else if (v.state == VOICE_REAL && v.fading_out && !v.end_after_fade)
    {
        // Still fading out from last time: turn it round.
        uint64_t fade = transition_frames(m);
        m->fading_count--;
        m->real_count++;
        v.fading_out = false;
        v.envelope_step = fade ? 1.0f / (float)fade : 1.0f;
    }
}

// A voice's speaker gains: [channel_count] per source channel.
static void voice_pan(const Mixer *m, const Voice &v, float *gains)
{
    const Layout &layout = m->layout;
    uint32_t channels = m->channel_count;
    uint32_t source_channels = v.sound->channel_count;
    memset(gains, 0, (size_t)source_channels * channels * sizeof(float));
    if (source_channels == 1)
    {
        if (v.has_direction)
            direction_pan(layout, v.azimuth, v.elevation, gains);
        else if (layout.front_centre >= 0)
            gains[layout.front_centre] = 1.0f;
        else if (layout.front_left >= 0 && layout.front_right >= 0)
            gains[layout.front_left] = gains[layout.front_right] = 0.70710678f;
        else
            direction_pan(layout, 0.0f, 0.0f, gains);
        return;
    }
    float *left = gains, *right = gains + channels;
    if (layout.mono >= 0)
    {
        left[layout.mono] = right[layout.mono] = 0.5f;
        return;
    }
    if (layout.front_left >= 0)
        left[layout.front_left] = 1.0f;
    else
        direction_pan(layout, -30.0f, 0.0f, left);
    if (layout.front_right >= 0)
        right[layout.front_right] = 1.0f;
    else
        direction_pan(layout, 30.0f, 0.0f, right);
}

// One frame of [sound] at [position] + [fraction], band-limited.
static inline void sinc_frame(const float *table, const Sound &sound, bool loop, uint64_t position, uint32_t fraction,
                              float stretch, float *out)
{
    float t = (float)fraction * (1.0f / 4294967296.0f);
    int half = (int)ceilf((float)SINC_ZEROS * stretch);
    float to_table = (float)SINC_RESOLUTION / stretch;
    const float limit = (float)(SINC_ZEROS * SINC_RESOLUTION);
    int64_t count = (int64_t)sound.frame_count;
    uint32_t channels = sound.channel_count;
    const float *samples = sound.samples;
    float sum[2] = {0.0f, 0.0f};
    for (int k = -half + 1; k <= half; k++)
    {
        float at = fabsf((float)k - t) * to_table;
        if (at >= limit)
            continue;
        int entry = (int)at;
        float weight = table[entry] + (table[entry + 1] - table[entry]) * (at - (float)entry);
        int64_t frame = (int64_t)position + k;
        if (frame < 0 || frame >= count)
        {
            if (!loop)
                continue;
            frame %= count;
            if (frame < 0)
                frame += count;
        }
        sum[0] += weight * samples[frame * channels];
        if (channels == 2)
            sum[1] += weight * samples[frame * channels + 1];
    }
    out[0] = sum[0] / stretch;
    out[1] = sum[1] / stretch;
}

// Resamples the voice's next [count] frames into m->source, its rate gliding
// from the last render's to [step_end], and moves its place on. Returns how
// many frames it had before its sound ended.
static uint32_t render_source(Mixer *m, Voice &v, uint32_t count, uint64_t step_end)
{
    const Sound &sound = *v.sound;
    const float *samples = sound.samples;
    uint64_t length = sound.frame_count;
    uint32_t channels = sound.channel_count;
    float *out0 = m->source;
    float *out1 = m->source + m->max_frames;
    uint64_t position = v.position;
    uint32_t fraction = v.fraction;
    uint64_t step = v.step;
    int64_t glide = ((int64_t)step_end - (int64_t)step) / (int64_t)count;

    // A voice at its sound's own rate is copied, not resampled.
    bool copy = step == FIXED_ONE && step_end == FIXED_ONE && fraction == 0;
    bool sinc = !copy && m->resampler == APAUDIO_RESAMPLER_SINC;
    float stretch = 1.0f;
    uint64_t end = length;
    if (sinc)
    {
        uint64_t fastest = step > step_end ? step : step_end;
        stretch = clamp_float((float)((double)fastest / 4294967296.0), 1.0f, SINC_MAX_STRETCH);
        if (!v.loop)
            end += (uint64_t)ceilf((float)SINC_ZEROS * stretch); // the filter's tail
    }

    uint32_t produced = count;
    for (uint32_t i = 0; i < count; i++)
    {
        if (v.loop)
        {
            if (position >= length)
                position %= length;
        }
        else if (position >= end)
        {
            produced = i;
            break;
        }
        if (copy)
        {
            out0[i] = samples[position * channels];
            if (channels == 2)
                out1[i] = samples[position * channels + 1];
        }
        else if (sinc)
        {
            float frame[2];
            sinc_frame(m->sinc, sound, v.loop, position, fraction, stretch, frame);
            out0[i] = frame[0];
            out1[i] = frame[1];
        }
        else
        {
            uint64_t after = position + 1 < length ? position + 1 : 0;
            bool silent_after = position + 1 >= length && !v.loop;
            float t = (float)fraction * (1.0f / 4294967296.0f);
            float a = samples[position * channels];
            float b = silent_after ? 0.0f : samples[after * channels];
            out0[i] = a + (b - a) * t;
            if (channels == 2)
            {
                a = samples[position * channels + 1];
                b = silent_after ? 0.0f : samples[after * channels + 1];
                out1[i] = a + (b - a) * t;
            }
        }
        step = (uint64_t)((int64_t)step + glide);
        uint64_t moved = (uint64_t)fraction + (step & 0xFFFFFFFFull);
        position += (step >> 32) + (moved >> 32);
        fraction = (uint32_t)moved;
    }
    for (uint32_t i = produced; i < count; i++)
    {
        out0[i] = 0.0f;
        out1[i] = 0.0f;
    }
    v.position = position;
    v.fraction = fraction;
    v.step = step_end;
    v.ended = !v.loop && (produced < count || position >= end);
    return produced;
}

// Mixes a real voice's part of this render.
static void mix_voice(Mixer *m, Voice &v, uint32_t frames)
{
    uint32_t channels = m->channel_count;
    uint32_t source_channels = v.sound->channel_count;
    uint32_t offset = v.start_offset;
    uint32_t count = frames - offset;
    v.start_offset = 0;

    float gain = v.cut ? 0.0f : v.gain * v.group_gain * m->buses[v.bus_index].effective_gain;
    uint64_t step = voice_step(m, v);
    float *pan = m->pan + (size_t)v.slot * 2 * channels;
    float *pan_end = pan;
    if (v.snap)
    {
        // Nothing mixed yet to ramp from.
        v.applied_gain = gain;
        v.step = step;
        voice_pan(m, v, pan);
    }
    else if (v.pan_changed)
    {
        pan_end = m->pan_scratch;
        voice_pan(m, v, pan_end);
    }
    v.snap = false;
    v.pan_changed = false;

    uint32_t produced = render_source(m, v, count, step);

    // Gain and envelope, ramped across the render.
    float gain_step = (gain - v.applied_gain) / (float)count;
    for (uint32_t c = 0; c < source_channels; c++)
    {
        float *source = m->source + (size_t)c * m->max_frames;
        for (uint32_t i = 0; i < produced; i++)
        {
            float envelope = clamp_float(v.envelope + v.envelope_step * (float)(i + 1), 0.0f, 1.0f);
            source[i] *= (v.applied_gain + gain_step * (float)(i + 1)) * envelope;
        }
    }
    v.applied_gain = gain;
    v.envelope = clamp_float(v.envelope + v.envelope_step * (float)count, 0.0f, 1.0f);

    for (uint32_t c = 0; c < source_channels; c++)
    {
        const float *source = m->source + (size_t)c * m->max_frames;
        for (uint32_t o = 0; o < channels; o++)
        {
            float from = pan[c * channels + o];
            float to = pan_end[c * channels + o];
            if (from == 0.0f && to == 0.0f)
                continue;
            float *out = m->mix + (size_t)o * m->max_frames + offset;
            if (from == to)
            {
                for (uint32_t i = 0; i < produced; i++)
                    out[i] += source[i] * from;
            }
            else
            {
                float pan_step = (to - from) / (float)count;
                for (uint32_t i = 0; i < produced; i++)
                    out[i] += source[i] * (from + pan_step * (float)(i + 1));
            }
        }
    }
    if (pan_end != pan)
        memcpy(pan, pan_end, (size_t)source_channels * channels * sizeof(float));
    v.real_frames += count;

    if (v.ended || v.cut)
        end_voice(m, v);
    else if (v.envelope_step < 0.0f && v.envelope <= 0.0f)
        finish_fade(m, v);
    else if (v.envelope_step > 0.0f && v.envelope >= 1.0f)
        v.envelope_step = 0.0f;
}

// A virtual voice's place moves on only if it's CONTINUE, and isn't held.
static void advance_virtual(Mixer *m, Voice &v, uint32_t frames)
{
    if (v.frozen || v.behavior != APAUDIO_VIRTUAL_CONTINUE)
        return;
    uint64_t length = v.sound->frame_count;
    uint64_t step = voice_step(m, v);
    uint64_t moved = (uint64_t)v.fraction + (step & 0xFFFFFFFFull) * frames;
    v.position += (step >> 32) * frames + (moved >> 32);
    v.fraction = (uint32_t)moved;
    v.step = step;
    if (v.loop)
        v.position %= length;
    else if (v.position >= length)
        end_voice(m, v);
}

// Tells apaudio_mixer_update() where every voice and bus stands.
static void publish(Mixer *m)
{
    uint64_t settle = frames_from_us(m, m->min_real_us);
    for (uint32_t i = 0; i < m->active_count; i++)
    {
        Voice &v = m->voices[m->active[i]];
        uint32_t flags = 0;
        bool real = v.state == VOICE_REAL && !v.fading_out;
        if (real)
            flags |= PUBLISHED_REAL;
        if (v.releasing || (v.fading_out && v.end_after_fade))
            flags |= PUBLISHED_ENDING;
        if (v.state == VOICE_WAITING)
            flags |= PUBLISHED_WAITING;
        else if (m->buses[v.bus_index].held)
            flags |= PUBLISHED_HELD;
        if (v.real_frames >= settle)
            flags |= PUBLISHED_SETTLED;
        if (real && !v.releasing && v.score > m->virtual_threshold)
            m->buses[v.bus_index].has_audible = true;
        v.published_info.store((uint64_t)float_bits(v.score) | ((uint64_t)v.priority << 32) | ((uint64_t)flags << 40),
                               std::memory_order_relaxed);
        v.published_id.store(v.id, std::memory_order_release);
    }
    const Config &config = *m->config;
    for (uint32_t i = config.order_count; i-- > 0;)
    {
        uint32_t index = config.order[i];
        Bus &bus = m->buses[index];
        if (bus.has_audible && index != 0)
            m->buses[config.buses[index].parent].has_audible = true;
        bus.audible.store(bus.has_audible ? 1 : 0, std::memory_order_relaxed);
    }
}

// Mixes [frames] frames (at most max_frames) into m->mix.
static void render_block(Mixer *m, uint32_t frames, bool host_valid, uint64_t host_ns)
{
    if (m->config_buffer.take())
        m->config = &m->configs[m->config_buffer.read];
    update_buses(m, frames);
    const uint64_t *decisions = m->decision_buffer.take() ? m->decisions[m->decision_buffer.read] : nullptr;

    for (uint32_t i = 0; i < m->active_count; i++)
    {
        Voice &v = m->voices[m->active[i]];
        if (!v.dead)
            prepare_voice(m, v, decisions, m->active[i]);
    }
    if (decisions)
    {
        for (uint32_t i = 0; i < m->active_count; i++)
            promote_voice(m, m->voices[m->active[i]], decisions[m->active[i]]);
    }
    for (uint32_t i = 0; i < m->active_count; i++)
    {
        Voice &v = m->voices[m->active[i]];
        uint32_t offset;
        if (v.dead || v.state != VOICE_WAITING || !start_offset(m, v, frames, host_valid, host_ns, offset))
            continue;
        if (group_admit(m, v, m->clock + offset))
            begin_voice(m, v, offset);
        else
            end_voice(m, v);
    }
    start_pending(m, frames, host_valid, host_ns);
    sweep(m);

    for (uint32_t c = 0; c < m->channel_count; c++)
        memset(m->mix + (size_t)c * m->max_frames, 0, frames * sizeof(float));
    for (uint32_t i = 0; i < m->active_count; i++)
    {
        Voice &v = m->voices[m->active[i]];
        if (v.dead)
            continue;
        if (v.state == VOICE_REAL)
            mix_voice(m, v, frames);
        else if (v.state == VOICE_VIRTUAL)
            advance_virtual(m, v, frames);
    }
    sweep(m);
    publish(m);
    m->clock += frames;
}

// [value] as a signed integer of [bits] bits, clamped.
static int32_t quantize(float value, int bits)
{
    if (!(value == value))
        return 0;
    double scale = (double)(1ull << (bits - 1));
    double scaled = (double)value * scale;
    if (scaled >= scale - 1.0)
        return (int32_t)(scale - 1.0);
    if (scaled <= -scale)
        return (int32_t)(-scale);
    return (int32_t)lrint(scaled);
}

// Writes [frames] frames of m->mix into the caller's buffer(s), from frame
// [offset] of them, in the mixer's format.
static void write_output(const Mixer *m, void *frames_out, uint32_t offset, uint32_t frames)
{
    uint32_t channels = m->channel_count;
    size_t bytes = m->sample_format == SPUDAUDIO_SAMPLE_FORMAT_S16          ? 2
                   : m->sample_format == SPUDAUDIO_SAMPLE_FORMAT_S24_PACKED ? 3
                                                                            : 4;
    for (uint32_t c = 0; c < channels; c++)
    {
        const float *mix = m->mix + (size_t)c * m->max_frames;
        uint8_t *out;
        size_t stride;
        if (m->interleaved)
        {
            out = (uint8_t *)frames_out + ((size_t)offset * channels + c) * bytes;
            stride = channels * bytes;
        }
        else
        {
            out = (uint8_t *)((void **)frames_out)[c] + (size_t)offset * bytes;
            stride = bytes;
        }
        for (uint32_t i = 0; i < frames; i++, out += stride)
        {
            switch (m->sample_format)
            {
            case SPUDAUDIO_SAMPLE_FORMAT_S16: {
                int16_t sample = (int16_t)quantize(mix[i], 16);
                memcpy(out, &sample, 2);
                break;
            }
            case SPUDAUDIO_SAMPLE_FORMAT_S24_PACKED: {
                uint32_t sample = (uint32_t)quantize(mix[i], 24);
                if (std::endian::native == std::endian::little)
                {
                    out[0] = (uint8_t)sample;
                    out[1] = (uint8_t)(sample >> 8);
                    out[2] = (uint8_t)(sample >> 16);
                }
                else
                {
                    out[0] = (uint8_t)(sample >> 16);
                    out[1] = (uint8_t)(sample >> 8);
                    out[2] = (uint8_t)sample;
                }
                break;
            }
            case SPUDAUDIO_SAMPLE_FORMAT_S24_32_MSB: {
                int32_t sample = (int32_t)((uint32_t)quantize(mix[i], 24) << 8);
                memcpy(out, &sample, 4);
                break;
            }
            case SPUDAUDIO_SAMPLE_FORMAT_S24_32_LSB: {
                int32_t sample = quantize(mix[i], 24);
                memcpy(out, &sample, 4);
                break;
            }
            case SPUDAUDIO_SAMPLE_FORMAT_S32: {
                int32_t sample = quantize(mix[i], 32);
                memcpy(out, &sample, 4);
                break;
            }
            default:
                memcpy(out, &mix[i], 4);
                break;
            }
        }
    }
}

// ---- Control-thread helpers ----------------------------------------------------

// A free slot of [count], looking from [cursor] on so the slot freed longest
// ago is reused first. [count] if there's none.
template <typename Free> static uint32_t find_slot(uint32_t first, uint32_t count, uint32_t &cursor, Free is_free)
{
    for (uint32_t i = 0; i < count - first; i++)
    {
        uint32_t index = first + (cursor + i) % (count - first);
        if (is_free(index))
        {
            cursor = (index - first + 1) % (count - first);
            return index;
        }
    }
    return count;
}

static void mixer_free(Mixer *m)
{
    if (!m)
        return;
    if (m->sounds)
    {
        for (uint32_t i = 0; i < m->max_sounds; i++)
            free(m->sounds[i].samples);
    }
    free(m->sinc);
    free(m->mix);
    free(m->source);
    free(m->pan);
    free(m->pan_scratch);
    free(m->voices);
    free(m->sounds);
    free(m->buses);
    free(m->modifiers);
    free(m->groups);
    for (Config &config : m->configs)
        config_free(config);
    config_free(m->shadow);
    for (uint64_t *decisions : m->decisions)
        free(decisions);
    free(m->rank);
    free(m->active);
    free(m->free_slots);
    delete m;
}

// Records a bus's gain or rate request: the fade first, so the value never
// arrives without one.
static APRESULT bus_set_value(Mixer *m, APAUDIO_BUS bus, std::atomic<uint64_t> Bus::*value_word,
                              std::atomic<uint64_t> Bus::*fade_word, float value, uint32_t fade_us)
{
    uint32_t index, generation;
    if (!split_id(bus, m->bus_count, index, generation))
        return APRESULT_NOT_FOUND;
    Bus &slot = m->buses[index];
    if (!tagged_store(slot.*fade_word, generation, fade_us) ||
        !tagged_store(slot.*value_word, generation, float_bits(value)))
        return APRESULT_NOT_FOUND;
    return APRESULT_OK;
}

static APRESULT bus_set_switch(Mixer *m, APAUDIO_BUS bus, std::atomic<uint64_t> Bus::*word, bool on, uint32_t fade_us)
{
    uint32_t index, generation;
    if (!split_id(bus, m->bus_count, index, generation))
        return APRESULT_NOT_FOUND;
    uint32_t fade = fade_us > SWITCH_FADE_MASK ? SWITCH_FADE_MASK : fade_us;
    if (!tagged_store(m->buses[index].*word, generation, SWITCH_CHANGED | (on ? SWITCH_ON : 0) | fade))
        return APRESULT_NOT_FOUND;
    return APRESULT_OK;
}

static void bus_words_reset(Bus &bus, uint32_t generation)
{
    bus.gain_word.store(tagged(generation, FLOAT_NONE), std::memory_order_release);
    bus.gain_fade_word.store(tagged(generation, 0), std::memory_order_release);
    bus.rate_word.store(tagged(generation, FLOAT_NONE), std::memory_order_release);
    bus.rate_fade_word.store(tagged(generation, 0), std::memory_order_release);
    bus.mute_word.store(tagged(generation, 0), std::memory_order_release);
    bus.pause_word.store(tagged(generation, 0), std::memory_order_release);
}

extern "C"
{
    // ---- Mixer --------------------------------------------------------------

    APRESULT apaudio_mixer_create(const APAUDIO_MIXER_DESC *desc, apaudio_mixer *out_mixer, APAUDIO_ERROR *error)
    {
        clear_error(error);
        if (out_mixer)
            *out_mixer = nullptr;
        if (!desc || !out_mixer)
            return fail(error, APRESULT_INVALID_ARGUMENT, "desc or out_mixer is null");
        APAUDIO_MIXER_DESC d;
        if (!read_versioned(desc, APAUDIO_MIXER_DESC_V1_SIZE, d))
            return fail(error, APRESULT_INVALID_ARGUMENT, "desc's struct_size is too small");
        Layout layout;
        if (APRESULT result = layout_build(d.format, d.max_frames, layout, error))
            return result;
        if (d.max_voices == 0 || d.max_real_voices == 0 || d.max_real_voices > d.max_voices)
            return fail(error, APRESULT_INVALID_ARGUMENT, "max_voices is %u and max_real_voices %u: both at least 1, and "
                        "max_real_voices at most max_voices", d.max_voices, d.max_real_voices);
        if (d.max_sounds == 0 || d.queue_capacity == 0)
            return fail(error, APRESULT_INVALID_ARGUMENT, "max_sounds or queue_capacity is 0");
        if (!valid_gain(d.virtual_threshold) || !valid_gain(d.gain))
            return fail(error, APRESULT_INVALID_ARGUMENT, "virtual_threshold or gain isn't finite and >= 0");
        if (d.resampler != APAUDIO_RESAMPLER_LINEAR && d.resampler != APAUDIO_RESAMPLER_SINC)
            return fail(error, APRESULT_INVALID_ARGUMENT, "resampler %u isn't LINEAR or SINC", d.resampler);
        uint64_t records = (uint64_t)d.max_voices + d.queue_capacity + d.max_fading_voices;
        uint64_t slots = (uint64_t)d.max_real_voices + d.max_fading_voices;
        if (records >= 0x7FFFFFFFull || d.max_sounds == UINT32_MAX || d.max_buses == UINT32_MAX)
            return fail(error, APRESULT_INVALID_ARGUMENT, "more voices, sounds or buses than the mixer can index");

        Mixer *m = new (std::nothrow) Mixer();
        if (!m)
            return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the mixer");
        m->max_voices = d.max_voices;
        m->max_real_voices = d.max_real_voices;
        m->max_fading_voices = d.max_fading_voices;
        m->max_sounds = d.max_sounds;
        m->max_groups = d.max_concurrency_groups;
        m->bus_count = d.max_buses + 1;
        m->max_modifiers = d.max_modifiers;
        m->queue_capacity = d.queue_capacity;
        m->record_count = (uint32_t)records;
        m->slot_count = (uint32_t)slots;
        m->virtual_threshold = d.virtual_threshold;
        m->transition_fade_us = d.transition_fade_us;
        m->min_real_us = d.min_real_us;
        m->resampler = d.resampler;

        FormatBuffers buffers;
        bool allocated = format_buffers_alloc(m, d.format.channel_count, d.max_frames, buffers);
        if (allocated)
            format_apply(m, d.format, d.max_frames, layout, buffers);
        m->voices = alloc_array<Voice>(m->record_count);
        m->sounds = alloc_array<Sound>(m->max_sounds);
        m->buses = alloc_array<Bus>(m->bus_count);
        m->modifiers = alloc_array<Modifier>(m->max_modifiers);
        m->groups = alloc_array<GroupState>(m->max_groups);
        m->rank = alloc_array<RankEntry>(m->record_count);
        m->active = alloc_array<uint32_t>(m->record_count);
        m->free_slots = alloc_array<uint32_t>(m->slot_count);
        allocated = allocated && m->voices && m->sounds && m->buses && m->modifiers && m->groups && m->rank &&
                    m->active && m->free_slots && config_alloc(m, m->shadow);
        for (int i = 0; i < 3 && allocated; i++)
        {
            m->decisions[i] = alloc_array<uint64_t>(m->record_count);
            allocated = m->decisions[i] && config_alloc(m, m->configs[i]);
        }
        if (allocated && m->resampler == APAUDIO_RESAMPLER_SINC)
            allocated = (m->sinc = sinc_table_build()) != nullptr;
        if (!allocated)
        {
            mixer_free(m);
            return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the mixer's voices, sounds and buffers");
        }

        for (uint32_t i = m->record_count; i-- > 0;)
            free_push(m, i);
        for (uint32_t i = 0; i < m->slot_count; i++)
            m->free_slots[m->free_slot_count++] = m->slot_count - 1 - i;

        // The master bus: slot 0, never destroyed.
        m->shadow.buses[0] = BusConfig{1, true, 0, d.gain, 1.0f};
        bus_words_reset(m->buses[0], 1);
        for (int i = 0; i < 3; i++)
            config_post(m);
        m->config_buffer.take();
        m->config = &m->configs[m->config_buffer.read];

        *out_mixer = m;
        return APRESULT_OK;
    }

    void apaudio_mixer_destroy(apaudio_mixer mixer)
    {
        mixer_free(mixer);
    }

    APRESULT apaudio_mixer_set_format(apaudio_mixer mixer, const APAUDIO_FORMAT *format, uint32_t max_frames,
                                      APAUDIO_ERROR *error)
    {
        clear_error(error);
        if (!mixer || !format)
            return fail(error, APRESULT_INVALID_ARGUMENT, "mixer or format is null");
        Layout layout;
        if (APRESULT result = layout_build(*format, max_frames, layout, error))
            return result;
        FormatBuffers buffers;
        if (!format_buffers_alloc(mixer, format->channel_count, max_frames, buffers))
            return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for the mixer's buffers");

        // Nothing is rendering, so the audio thread's state is this thread's
        // to touch: fades in progress keep their length at the new rate, and
        // playing voices carry on, panned afresh for the new layout.
        float rescale = (float)mixer->sample_rate / (float)format->sample_rate;
        format_apply(mixer, *format, max_frames, layout, buffers);
        for (uint32_t i = 0; i < mixer->active_count; i++)
        {
            Voice &v = mixer->voices[mixer->active[i]];
            v.envelope_step *= rescale;
            v.snap = true; // re-pan, and take the new rate's step
        }
        for (uint32_t i = 0; i < mixer->bus_count; i++)
        {
            Bus &bus = mixer->buses[i];
            bus.gain.step *= rescale;
            bus.rate.step *= rescale;
            bus.mute.step *= rescale;
            bus.pause.step *= rescale;
        }
        collect_sounds(mixer);
        return APRESULT_OK;
    }

    void apaudio_mixer_update(apaudio_mixer mixer)
    {
        if (!mixer)
            return;
        Mixer *m = mixer;
        collect_sounds(m);

        // Triggered modifiers: active while their bus has audible voices.
        for (uint32_t i = 0; i < m->max_modifiers; i++)
        {
            const ModifierConfig &modifier = m->shadow.modifiers[i];
            if (modifier.live && modifier.has_trigger)
                m->modifiers[i].triggered.store(m->buses[modifier.trigger].audible.load(std::memory_order_relaxed),
                                                std::memory_order_relaxed);
        }

        // Rank the voices: priority, then audibility. A voice holding a real
        // slot keeps it against a near-equal one, and against any of its own
        // priority until it has had it for min_real_us.
        uint64_t *decisions = m->decisions[m->decision_buffer.write];
        memset(decisions, 0, m->record_count * sizeof(uint64_t));
        uint32_t count = 0;
        uint32_t capacity = m->max_real_voices;
        for (uint32_t i = 0; i < m->record_count; i++)
        {
            Voice &v = m->voices[i];
            uint64_t id = v.published_id.load(std::memory_order_acquire);
            if (id == 0)
                continue;
            uint64_t info = v.published_info.load(std::memory_order_relaxed);
            if (v.published_id.load(std::memory_order_acquire) != id)
                continue; // ended, or replaced, as it was read
            uint32_t flags = (uint32_t)(info >> 40);
            if (flags & (PUBLISHED_WAITING | PUBLISHED_HELD))
                continue;
            bool real = (flags & PUBLISHED_REAL) != 0;
            if (flags & PUBLISHED_ENDING)
            {
                // On its way out, keeping its slot until it's gone.
                if (real && capacity > 0)
                    capacity--;
                continue;
            }
            RankEntry &entry = m->rank[count++];
            entry.index = i;
            entry.generation = (uint32_t)(id >> 32);
            entry.priority = (uint8_t)(info >> 32);
            entry.real = real;
            entry.score = bits_float((uint32_t)info);
            entry.key = !real ? entry.score : (flags & PUBLISHED_SETTLED) ? entry.score * TIE_MARGIN : INFINITY;
        }
        std::sort(m->rank, m->rank + count, [](const RankEntry &a, const RankEntry &b) {
            if (a.priority != b.priority)
                return a.priority > b.priority;
            if (a.key != b.key)
                return a.key > b.key;
            if (a.real != b.real)
                return a.real;
            return a.index < b.index;
        });
        uint32_t taken = 0;
        for (uint32_t i = 0; i < count; i++)
        {
            const RankEntry &entry = m->rank[i];
            float score = entry.real ? entry.score * TIE_MARGIN : entry.score;
            bool real = taken < capacity && audible_enough(m, score);
            if (real)
                taken++;
            decisions[entry.index] = tagged(entry.generation, (uint32_t)(real ? DECISION_REAL : DECISION_VIRTUAL));
        }
        m->decision_buffer.post();
    }

    // ---- Rendering ----------------------------------------------------------

    void apaudio_mixer_render(apaudio_mixer mixer, void *frames, uint32_t frame_count,
                              const SPUDAUDIO_CALLBACK_INFO *info)
    {
        if (!mixer || !frames || frame_count == 0)
            return;
        Mixer *m = mixer;
        m->render_sequence.fetch_add(1);
        bool host_valid = info && (info->flags & SPUDAUDIO_CALLBACK_FLAG_HOST_TIME_VALID);
        // More than max_frames is a caller error, but one that costs nothing
        // to render correctly: in parts.
        for (uint32_t offset = 0; offset < frame_count;)
        {
            uint32_t part = frame_count - offset < m->max_frames ? frame_count - offset : m->max_frames;
            uint64_t host_ns = host_valid ? info->host_time_ns + (uint64_t)offset * 1000000000ull / m->sample_rate : 0;
            render_block(m, part, host_valid, host_ns);
            write_output(m, frames, offset, part);
            offset += part;
        }
        m->render_sequence.fetch_add(1);
    }

    void apaudio_mixer_stream_callback(spudaudio_stream stream, void *frames, uint32_t frame_count,
                                       const SPUDAUDIO_CALLBACK_INFO *info, void *user_data)
    {
        (void)stream;
        apaudio_mixer_render((apaudio_mixer)user_data, frames, frame_count, info);
    }

    // ---- Buses --------------------------------------------------------------

    APAUDIO_BUS apaudio_mixer_get_master_bus(apaudio_mixer mixer)
    {
        return mixer ? make_id(0, 1) : 0;
    }

    APRESULT apaudio_bus_create(apaudio_mixer mixer, const APAUDIO_BUS_DESC *desc, APAUDIO_BUS *out_bus,
                                APAUDIO_ERROR *error)
    {
        clear_error(error);
        if (out_bus)
            *out_bus = 0;
        if (!mixer || !desc || !out_bus)
            return fail(error, APRESULT_INVALID_ARGUMENT, "mixer, desc or out_bus is null");
        Mixer *m = mixer;
        APAUDIO_BUS_DESC d;
        if (!read_versioned(desc, APAUDIO_BUS_DESC_V1_SIZE, d))
            return fail(error, APRESULT_INVALID_ARGUMENT, "desc's struct_size is too small");
        if (!valid_gain(d.gain) || !valid_rate(d.rate))
            return fail(error, APRESULT_INVALID_ARGUMENT, "a gain of %g (finite, >= 0) or a rate of %g (%g to %g)",
                        (double)d.gain, (double)d.rate, (double)RATE_MIN, (double)RATE_MAX);
        uint32_t parent;
        if (!find_bus(m, d.parent, parent))
            return fail(error, APRESULT_NOT_FOUND, "the parent bus was destroyed");
        Config &config = m->shadow;
        uint32_t index = m->bus_count > 1 ? find_slot(1, m->bus_count, m->bus_cursor,
                                                      [&](uint32_t i) { return !config.buses[i].live; })
                                          : m->bus_count;
        if (index == m->bus_count)
            return fail(error, APRESULT_LIMIT, "all %u buses are in use", m->bus_count - 1);
        uint32_t generation = next_generation(config.buses[index].generation);
        config.buses[index] = BusConfig{generation, true, parent, d.gain, d.rate};
        m->buses[index].audible.store(0, std::memory_order_relaxed);
        bus_words_reset(m->buses[index], generation);
        config_post(m);
        *out_bus = make_id(index, generation);
        return APRESULT_OK;
    }

    void apaudio_bus_destroy(apaudio_mixer mixer, APAUDIO_BUS bus)
    {
        uint32_t index;
        if (!mixer || bus == 0 || !find_bus(mixer, bus, index) || index == 0)
            return;
        Mixer *m = mixer;
        Config &config = m->shadow;
        uint32_t parent = config.buses[index].parent;
        config.buses[index].live = false;
        bus_words_reset(m->buses[index], 0); // no ID names generation 0: every setter is turned away
        // Its children - and the voices of buses destroyed earlier that were
        // sent to it - move to its parent.
        for (uint32_t b = 1; b < m->bus_count; b++)
        {
            if (b != index && config.buses[b].parent == index)
                config.buses[b].parent = parent;
        }
        for (uint32_t i = 0; i < m->max_modifiers; i++)
        {
            ModifierConfig &modifier = config.modifiers[i];
            ModifierTarget *targets = config.targets + (size_t)i * m->bus_count;
            for (uint32_t t = 0; t < modifier.target_count;)
            {
                if (targets[t].bus == index)
                    targets[t] = targets[--modifier.target_count];
                else
                    t++;
            }
            if (modifier.has_trigger && modifier.trigger == index)
            {
                modifier.has_trigger = false;
                m->modifiers[i].triggered.store(0, std::memory_order_relaxed);
            }
        }
        config_post(m);
    }

    APRESULT apaudio_bus_set_parent(apaudio_mixer mixer, APAUDIO_BUS bus, APAUDIO_BUS parent)
    {
        if (!mixer)
            return APRESULT_INVALID_ARGUMENT;
        Mixer *m = mixer;
        uint32_t index, parent_index;
        if (bus == 0 || !find_bus(m, bus, index) || !find_bus(m, parent, parent_index))
            return APRESULT_NOT_FOUND;
        if (index == 0)
            return APRESULT_INVALID_ARGUMENT; // the master stays the root
        for (uint32_t at = parent_index;; at = m->shadow.buses[at].parent)
        {
            if (at == index)
                return APRESULT_INVALID_ARGUMENT; // [parent] is [bus], or under it
            if (at == 0)
                break;
        }
        m->shadow.buses[index].parent = parent_index;
        config_post(m);
        return APRESULT_OK;
    }

    APRESULT apaudio_bus_set_gain(apaudio_mixer mixer, APAUDIO_BUS bus, float gain, uint32_t fade_us)
    {
        if (!mixer || !valid_gain(gain))
            return APRESULT_INVALID_ARGUMENT;
        return bus_set_value(mixer, bus, &Bus::gain_word, &Bus::gain_fade_word, gain, fade_us);
    }

    APRESULT apaudio_bus_set_rate(apaudio_mixer mixer, APAUDIO_BUS bus, float rate, uint32_t fade_us)
    {
        if (!mixer || !valid_rate(rate))
            return APRESULT_INVALID_ARGUMENT;
        return bus_set_value(mixer, bus, &Bus::rate_word, &Bus::rate_fade_word, rate, fade_us);
    }

    APRESULT apaudio_bus_set_muted(apaudio_mixer mixer, APAUDIO_BUS bus, bool muted, uint32_t fade_us)
    {
        if (!mixer)
            return APRESULT_INVALID_ARGUMENT;
        return bus_set_switch(mixer, bus, &Bus::mute_word, muted, fade_us);
    }

    APRESULT apaudio_bus_set_paused(apaudio_mixer mixer, APAUDIO_BUS bus, bool paused, uint32_t fade_us)
    {
        if (!mixer)
            return APRESULT_INVALID_ARGUMENT;
        return bus_set_switch(mixer, bus, &Bus::pause_word, paused, fade_us);
    }

    // ---- Modifiers ----------------------------------------------------------

    APRESULT apaudio_modifier_create(apaudio_mixer mixer, const APAUDIO_MODIFIER_DESC *desc,
                                     APAUDIO_MODIFIER *out_modifier, APAUDIO_ERROR *error)
    {
        clear_error(error);
        if (out_modifier)
            *out_modifier = 0;
        if (!mixer || !desc || !out_modifier)
            return fail(error, APRESULT_INVALID_ARGUMENT, "mixer, desc or out_modifier is null");
        Mixer *m = mixer;
        APAUDIO_MODIFIER_DESC d;
        if (!read_versioned(desc, APAUDIO_MODIFIER_DESC_V1_SIZE, d))
            return fail(error, APRESULT_INVALID_ARGUMENT, "desc's struct_size is too small");
        if (d.target_count > 0 && !d.targets)
            return fail(error, APRESULT_INVALID_ARGUMENT, "targets is null with a target_count of %u", d.target_count);
        if (d.target_count > m->bus_count)
            return fail(error, APRESULT_INVALID_ARGUMENT, "%u targets for %u buses: at most one per bus", d.target_count,
                        m->bus_count);
        Config &config = m->shadow;
        uint32_t index = m->max_modifiers ? find_slot(0, m->max_modifiers, m->modifier_cursor,
                                                      [&](uint32_t i) { return !config.modifiers[i].live; })
                                          : 0;
        if (index == m->max_modifiers)
            return fail(error, APRESULT_LIMIT, "all %u modifiers are in use", m->max_modifiers);
        // Checked, and built, in the free slot: nothing reads it until it's live.
        ModifierTarget *targets = config.targets + (size_t)index * m->bus_count;
        for (uint32_t t = 0; t < d.target_count; t++)
        {
            const APAUDIO_MODIFIER_TARGET &target = d.targets[t];
            uint32_t bus;
            if (target.bus == 0 || !find_bus(m, target.bus, bus))
                return fail(error, APRESULT_NOT_FOUND, "target %u's bus is 0 or was destroyed", t);
            if (!valid_gain(target.gain) || !valid_rate(target.rate))
                return fail(error, APRESULT_INVALID_ARGUMENT, "target %u has a gain of %g (finite, >= 0) or a rate of %g "
                            "(%g to %g)", t, (double)target.gain, (double)target.rate, (double)RATE_MIN,
                            (double)RATE_MAX);
            for (uint32_t other = 0; other < t; other++)
            {
                if (targets[other].bus == bus)
                    return fail(error, APRESULT_INVALID_ARGUMENT, "targets %u and %u name the same bus", other, t);
            }
            targets[t] = ModifierTarget{bus, target.gain, target.rate};
        }
        uint32_t trigger = 0;
        if (d.trigger != 0 && !find_bus(m, d.trigger, trigger))
            return fail(error, APRESULT_NOT_FOUND, "the trigger bus was destroyed");
        uint32_t generation = next_generation(config.modifiers[index].generation);
        config.modifiers[index] =
            ModifierConfig{generation, true, d.trigger != 0, trigger, d.fade_in_us, d.fade_out_us, d.target_count};
        m->modifiers[index].triggered.store(0, std::memory_order_relaxed);
        m->modifiers[index].active_word.store(tagged(generation, 0), std::memory_order_release);
        config_post(m);
        *out_modifier = make_id(index, generation);
        return APRESULT_OK;
    }

    void apaudio_modifier_destroy(apaudio_mixer mixer, APAUDIO_MODIFIER modifier)
    {
        uint32_t index, generation;
        if (!mixer || !split_id(modifier, mixer->max_modifiers, index, generation))
            return;
        ModifierConfig &config = mixer->shadow.modifiers[index];
        if (!config.live || config.generation != generation)
            return;
        // Kept, not live, so the audio thread fades its effect out; the slot
        // is the last to be reused.
        config.live = false;
        mixer->modifiers[index].active_word.store(0, std::memory_order_release);
        mixer->modifiers[index].triggered.store(0, std::memory_order_relaxed);
        config_post(mixer);
    }

    APRESULT apaudio_modifier_set_active(apaudio_mixer mixer, APAUDIO_MODIFIER modifier, bool active)
    {
        if (!mixer)
            return APRESULT_INVALID_ARGUMENT;
        uint32_t index, generation;
        if (!split_id(modifier, mixer->max_modifiers, index, generation) ||
            !tagged_store(mixer->modifiers[index].active_word, generation, active ? 1 : 0))
            return APRESULT_NOT_FOUND;
        return APRESULT_OK;
    }

    // ---- Concurrency groups -------------------------------------------------

    APRESULT apaudio_concurrency_create(apaudio_mixer mixer, const APAUDIO_CONCURRENCY_DESC *desc,
                                        APAUDIO_CONCURRENCY *out_group, APAUDIO_ERROR *error)
    {
        clear_error(error);
        if (out_group)
            *out_group = 0;
        if (!mixer || !desc || !out_group)
            return fail(error, APRESULT_INVALID_ARGUMENT, "mixer, desc or out_group is null");
        Mixer *m = mixer;
        APAUDIO_CONCURRENCY_DESC d;
        if (!read_versioned(desc, APAUDIO_CONCURRENCY_DESC_V1_SIZE, d))
            return fail(error, APRESULT_INVALID_ARGUMENT, "desc's struct_size is too small");
        if (d.max_instances == 0)
            return fail(error, APRESULT_INVALID_ARGUMENT, "max_instances is 0");
        if (d.resolution < APAUDIO_RESOLUTION_REJECT_NEW || d.resolution > APAUDIO_RESOLUTION_STOP_LOWEST_PRIORITY)
            return fail(error, APRESULT_INVALID_ARGUMENT, "resolution %u isn't one of APAUDIO_RESOLUTION", d.resolution);
        if (!(d.older_instance_gain > 0.0f && d.older_instance_gain <= 1.0f))
            return fail(error, APRESULT_INVALID_ARGUMENT, "an older_instance_gain of %g, outside (0, 1]",
                        (double)d.older_instance_gain);
        Config &config = m->shadow;
        uint32_t index = m->max_groups ? find_slot(0, m->max_groups, m->group_cursor,
                                                   [&](uint32_t i) { return !config.groups[i].live; })
                                       : 0;
        if (index == m->max_groups)
            return fail(error, APRESULT_LIMIT, "all %u concurrency groups are in use", m->max_groups);
        uint32_t generation = next_generation(config.groups[index].generation);
        config.groups[index] =
            GroupConfig{generation, true, d.max_instances, d.resolution, d.retrigger_us, d.older_instance_gain};
        config_post(m);
        *out_group = make_id(index, generation);
        return APRESULT_OK;
    }

    void apaudio_concurrency_destroy(apaudio_mixer mixer, APAUDIO_CONCURRENCY group)
    {
        uint32_t index, generation;
        if (!mixer || !split_id(group, mixer->max_groups, index, generation))
            return;
        GroupConfig &config = mixer->shadow.groups[index];
        if (!config.live || config.generation != generation)
            return;
        config.live = false;
        config_post(mixer);
    }

    // ---- Sounds -------------------------------------------------------------

    APRESULT apaudio_sound_create(apaudio_mixer mixer, const APAUDIO_SOUND_DESC *desc, APAUDIO_SOUND *out_sound,
                                  APAUDIO_ERROR *error)
    {
        clear_error(error);
        if (out_sound)
            *out_sound = 0;
        if (!mixer || !desc || !out_sound)
            return fail(error, APRESULT_INVALID_ARGUMENT, "mixer, desc or out_sound is null");
        Mixer *m = mixer;
        APAUDIO_SOUND_DESC d;
        if (!read_versioned(desc, APAUDIO_SOUND_DESC_V1_SIZE, d))
            return fail(error, APRESULT_INVALID_ARGUMENT, "desc's struct_size is too small");
        if (d.channel_count != 1 && d.channel_count != 2)
            return fail(error, APRESULT_UNSUPPORTED, "a sound of %u channels: only mono and stereo play",
                        d.channel_count);
        if (d.frame_count > 0 && !d.samples)
            return fail(error, APRESULT_INVALID_ARGUMENT, "samples is null with a frame_count of %llu",
                        (unsigned long long)d.frame_count);
        if (d.sample_rate < SOUND_RATE_MIN || d.sample_rate > SOUND_RATE_MAX)
            return fail(error, APRESULT_INVALID_ARGUMENT, "a sample rate of %u Hz, outside %u to %u", d.sample_rate,
                        SOUND_RATE_MIN, SOUND_RATE_MAX);
        if (d.virtual_behavior < APAUDIO_VIRTUAL_KILL || d.virtual_behavior > APAUDIO_VIRTUAL_PAUSE)
            return fail(error, APRESULT_INVALID_ARGUMENT, "virtual_behavior %u isn't one a sound can have",
                        d.virtual_behavior);
        if (d.frame_count > (uint64_t)INT64_MAX / 8 || d.frame_count * d.channel_count > SIZE_MAX / sizeof(float))
            return fail(error, APRESULT_OUT_OF_MEMORY, "%llu frames don't fit in memory",
                        (unsigned long long)d.frame_count);

        collect_sounds(m);
        uint32_t index = find_slot(0, m->max_sounds, m->sound_cursor,
                                   [&](uint32_t i) { return m->sounds[i].state == SOUND_FREE; });
        if (index == m->max_sounds)
            return fail(error, APRESULT_LIMIT, "all %u sounds are in use, or destroyed and waiting for a render to "
                        "let go of them", m->max_sounds);
        Sound &sound = m->sounds[index];
        size_t sample_count = (size_t)(d.frame_count * d.channel_count);
        float *samples = nullptr;
        double squares = 0.0;
        if (sample_count)
        {
            samples = static_cast<float *>(malloc(sample_count * sizeof(float)));
            if (!samples)
                return fail(error, APRESULT_OUT_OF_MEMORY, "no memory for %llu samples",
                            (unsigned long long)sample_count);
            memcpy(samples, d.samples, sample_count * sizeof(float));
            for (size_t i = 0; i < sample_count; i++)
                squares += (double)samples[i] * (double)samples[i];
        }
        sound.samples = samples;
        sound.frame_count = d.frame_count;
        sound.channel_count = d.channel_count;
        sound.sample_rate = d.sample_rate;
        sound.loudness = sample_count ? (float)sqrt(squares / (double)sample_count) : 0.0f;
        sound.virtual_behavior = d.virtual_behavior;
        sound.concurrency = d.concurrency;
        sound.bus = d.bus;
        sound.generation = next_generation(sound.generation);
        sound.state = SOUND_LIVE;
        uint64_t id = make_id(index, sound.generation);
        sound.live_id.store(id);
        *out_sound = id;
        return APRESULT_OK;
    }

    void apaudio_sound_destroy(apaudio_mixer mixer, APAUDIO_SOUND sound)
    {
        uint32_t index, generation;
        if (!mixer || !split_id(sound, mixer->max_sounds, index, generation))
            return;
        Sound &slot = mixer->sounds[index];
        if (slot.state != SOUND_LIVE || slot.generation != generation)
            return;
        // Stale from here. A render that began before this may still be
        // using the sound, so its memory waits for one that began after to
        // finish.
        slot.live_id.store(0);
        uint64_t sequence = mixer->render_sequence.load();
        slot.retire_sequence = sequence + 2 + (sequence & 1);
        slot.state = SOUND_RETIRED;
        collect_sounds(mixer);
    }

    // ---- Voices -------------------------------------------------------------

    APRESULT apaudio_play(apaudio_mixer mixer, APAUDIO_SOUND sound, const APAUDIO_PLAY_DESC *desc,
                          APAUDIO_VOICE *out_voice)
    {
        if (out_voice)
            *out_voice = 0;
        if (!mixer || !desc)
            return APRESULT_INVALID_ARGUMENT;
        Mixer *m = mixer;
        APAUDIO_PLAY_DESC d;
        if (!read_versioned(desc, APAUDIO_PLAY_DESC_V1_SIZE, d))
            return APRESULT_INVALID_ARGUMENT;
        if (!valid_gain(d.gain) || !valid_rate(d.rate) || d.virtual_behavior > APAUDIO_VIRTUAL_PAUSE)
            return APRESULT_INVALID_ARGUMENT;
        if (d.direction && (!isfinite(d.direction->azimuth) || !isfinite(d.direction->elevation)))
            return APRESULT_INVALID_ARGUMENT;
        uint32_t sound_index, sound_generation;
        if (!split_id(sound, m->max_sounds, sound_index, sound_generation) ||
            m->sounds[sound_index].live_id.load() != sound)
            return APRESULT_NOT_FOUND;

        // A place in the queue, then a record: with queue_capacity plays
        // waiting at most, there's always one.
        if (m->queued.fetch_add(1, std::memory_order_acquire) >= m->queue_capacity)
        {
            m->queued.fetch_sub(1, std::memory_order_release);
            return APRESULT_LIMIT;
        }
        uint32_t index;
        if (!free_pop(m, index))
        {
            m->queued.fetch_sub(1, std::memory_order_release);
            return APRESULT_LIMIT;
        }
        Voice &v = m->voices[index];
        v.generation = next_generation(v.generation);
        v.id = make_id(index, v.generation);
        v.sound_id = sound;
        v.gain = d.gain;
        v.rate = d.rate;
        v.priority = d.priority;
        v.behavior = d.virtual_behavior;
        v.concurrency = d.concurrency;
        v.bus = d.bus;
        v.has_direction = d.direction != nullptr;
        if (d.direction)
        {
            v.azimuth = wrap360((float)(fmod(d.direction->azimuth, 2.0 * 3.14159265358979323846) * (180.0 / 3.14159265358979323846)));
            v.elevation = clamp_float((float)(d.direction->elevation * (180.0 / 3.14159265358979323846)), -90.0f, 90.0f);
        }
        v.start_time_ns = d.start_time_ns;
        v.loop = d.loop;
        v.stop_word.store(tagged(v.generation, 0), std::memory_order_relaxed);
        v.gain_word.store(tagged(v.generation, FLOAT_NONE), std::memory_order_relaxed);
        v.rate_word.store(tagged(v.generation, FLOAT_NONE), std::memory_order_relaxed);
        v.direction_word.store(tagged(v.generation, 0), std::memory_order_relaxed);
        v.live_id.store(v.id, std::memory_order_release);
        if (out_voice)
            *out_voice = v.id;

        uint32_t head = m->pending_head.load(std::memory_order_relaxed);
        do
        {
            v.next.store(head, std::memory_order_relaxed);
        } while (!m->pending_head.compare_exchange_weak(head, index + 1, std::memory_order_release,
                                                         std::memory_order_relaxed));
        return APRESULT_OK;
    }

    APRESULT apaudio_voice_stop(apaudio_mixer mixer, APAUDIO_VOICE voice, uint32_t release_frames)
    {
        if (!mixer)
            return APRESULT_INVALID_ARGUMENT;
        uint32_t index, generation;
        if (!split_id(voice, mixer->record_count, index, generation))
            return APRESULT_OK;
        // The first stop stands: one already asked for isn't replaced.
        std::atomic<uint64_t> &word = mixer->voices[index].stop_word;
        uint32_t release = release_frames > STOP_FRAMES_MASK ? STOP_FRAMES_MASK : release_frames;
        uint64_t old = word.load(std::memory_order_relaxed);
        while ((uint32_t)(old >> 32) == generation && !(old & STOP_REQUESTED))
        {
            if (word.compare_exchange_weak(old, tagged(generation, STOP_REQUESTED | release),
                                           std::memory_order_release, std::memory_order_relaxed))
                break;
        }
        return APRESULT_OK;
    }

    bool apaudio_voice_is_live(apaudio_mixer mixer, APAUDIO_VOICE voice)
    {
        uint32_t index, generation;
        if (!mixer || !split_id(voice, mixer->record_count, index, generation))
            return false;
        return mixer->voices[index].live_id.load(std::memory_order_acquire) == voice;
    }

    APRESULT apaudio_voice_set_gain(apaudio_mixer mixer, APAUDIO_VOICE voice, float gain)
    {
        if (!mixer || !valid_gain(gain))
            return APRESULT_INVALID_ARGUMENT;
        uint32_t index, generation;
        if (split_id(voice, mixer->record_count, index, generation))
            tagged_store(mixer->voices[index].gain_word, generation, float_bits(gain));
        return APRESULT_OK;
    }

    APRESULT apaudio_voice_set_direction(apaudio_mixer mixer, APAUDIO_VOICE voice, const APAUDIO_DIRECTION *direction)
    {
        if (!mixer || (direction && (!isfinite(direction->azimuth) || !isfinite(direction->elevation))))
            return APRESULT_INVALID_ARGUMENT;
        uint32_t index, generation;
        if (split_id(voice, mixer->record_count, index, generation))
            tagged_store(mixer->voices[index].direction_word, generation, pack_direction(direction));
        return APRESULT_OK;
    }

    APRESULT apaudio_voice_set_rate(apaudio_mixer mixer, APAUDIO_VOICE voice, float rate)
    {
        if (!mixer || !valid_rate(rate))
            return APRESULT_INVALID_ARGUMENT;
        uint32_t index, generation;
        if (split_id(voice, mixer->record_count, index, generation))
            tagged_store(mixer->voices[index].rate_word, generation, float_bits(rate));
        return APRESULT_OK;
    }

    // ---- WAV ----------------------------------------------------------------

    APRESULT apaudio_wav_decode(const void *data, uint64_t size, APAUDIO_PCM *out_pcm, APAUDIO_ERROR *error)
    {
        clear_error(error);
        if (out_pcm)
            *out_pcm = APAUDIO_PCM{};
        if ((!data && size != 0) || !out_pcm)
            return fail(error, APRESULT_INVALID_ARGUMENT, "data or out_pcm is null");
        return wav_decode(static_cast<const uint8_t *>(data), size, out_pcm, error);
    }

    // apaudio_wav_decode allocates samples with malloc.
    void apaudio_pcm_free(APAUDIO_PCM *pcm)
    {
        if (!pcm)
            return;
        free(pcm->samples);
        *pcm = APAUDIO_PCM{};
    }
}
