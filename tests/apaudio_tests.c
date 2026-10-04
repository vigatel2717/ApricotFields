// Headless tests for ApAudio (include/audio/apaudio.h). No device: a mixer
// renders into plain buffers.
//
// Written in C on purpose: apaudio.h is a C front end, and this proves it
// compiles and links as one.

#include "audio/apaudio.h"
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;
static const char *current_test = "";

#define CHECK(condition)                                                                                               \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(condition))                                                                                              \
        {                                                                                                              \
            printf("FAIL %s (%s:%d): %s\n", current_test, __FILE__, __LINE__, #condition);                           \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

static const SPUDAUDIO_CHANNEL_POSITION stereo[2] = {SPUDAUDIO_CHANNEL_POSITION_FL, SPUDAUDIO_CHANNEL_POSITION_FR};

static APAUDIO_MIXER_DESC valid_mixer_desc(void)
{
    APAUDIO_MIXER_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
    desc.format.sample_rate = 48000;
    desc.format.channel_count = 2;
    desc.format.interleaved = true;
    desc.format.positions = stereo;
    desc.max_frames = 512;
    desc.max_voices = 64;
    desc.max_real_voices = 16;
    desc.max_fading_voices = 4;
    desc.transition_fade_us = 10000;
    desc.max_sounds = 32;
    desc.max_concurrency_groups = 8;
    desc.max_buses = 8;
    desc.max_modifiers = 4;
    desc.queue_capacity = 64;
    desc.resampler = APAUDIO_RESAMPLER_LINEAR;
    desc.gain = 1.0f;
    return desc;
}

static void test_format_from_spudaudio(void)
{
    current_test = "format_from_spudaudio";
    SPUDAUDIO_FORMAT spud;
    memset(&spud, 0, sizeof(spud));
    spud.sample_format = SPUDAUDIO_SAMPLE_FORMAT_S16;
    spud.sample_rate = 44100;
    spud.channel_count = 2;
    spud.positions[0] = SPUDAUDIO_CHANNEL_POSITION_FL;
    spud.positions[1] = SPUDAUDIO_CHANNEL_POSITION_FR;
    spud.interleaved = true;
    APAUDIO_FORMAT format = apaudio_format_from_spudaudio(&spud);
    CHECK(format.sample_format == SPUDAUDIO_SAMPLE_FORMAT_S16);
    CHECK(format.sample_rate == 44100);
    CHECK(format.channel_count == 2);
    CHECK(format.interleaved);
    // Points into the SpudAudio format rather than copying.
    CHECK(format.positions == spud.positions);
}

// Every call turns a null mixer or a null required pointer away, and clears
// its out-parameters first.
static void test_null_arguments(void)
{
    current_test = "null_arguments";
    APAUDIO_ERROR error;
    APAUDIO_MIXER_DESC desc = valid_mixer_desc();

    apaudio_mixer mixer = (apaudio_mixer)(uintptr_t)1; // garbage, must be cleared
    CHECK(apaudio_mixer_create(NULL, &mixer, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(mixer == NULL);
    CHECK(error.message[0] != '\0');
    CHECK(apaudio_mixer_create(&desc, NULL, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_mixer_set_format(NULL, &desc.format, 512, NULL) == APRESULT_INVALID_ARGUMENT);

    APAUDIO_BUS_DESC bus_desc;
    memset(&bus_desc, 0, sizeof(bus_desc));
    bus_desc.struct_size = sizeof(bus_desc);
    bus_desc.gain = 1.0f;
    bus_desc.rate = 1.0f;
    APAUDIO_BUS bus = 99;
    CHECK(apaudio_bus_create(NULL, &bus_desc, &bus, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(bus == 0);
    CHECK(apaudio_bus_set_parent(NULL, 1, 0) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_bus_set_gain(NULL, 1, 1.0f, 0) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_bus_set_rate(NULL, 1, 1.0f, 0) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_bus_set_muted(NULL, 1, true, 0) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_bus_set_paused(NULL, 1, true, 0) == APRESULT_INVALID_ARGUMENT);

    APAUDIO_MODIFIER modifier = 99;
    APAUDIO_MODIFIER_DESC modifier_desc;
    memset(&modifier_desc, 0, sizeof(modifier_desc));
    modifier_desc.struct_size = sizeof(modifier_desc);
    CHECK(apaudio_modifier_create(NULL, &modifier_desc, &modifier, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(modifier == 0);
    CHECK(apaudio_modifier_set_active(NULL, 1, true) == APRESULT_INVALID_ARGUMENT);

    APAUDIO_CONCURRENCY group = 99;
    APAUDIO_CONCURRENCY_DESC group_desc;
    memset(&group_desc, 0, sizeof(group_desc));
    group_desc.struct_size = sizeof(group_desc);
    CHECK(apaudio_concurrency_create(NULL, &group_desc, &group, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(group == 0);

    APAUDIO_SOUND sound = 99;
    APAUDIO_SOUND_DESC sound_desc;
    memset(&sound_desc, 0, sizeof(sound_desc));
    sound_desc.struct_size = sizeof(sound_desc);
    CHECK(apaudio_sound_create(NULL, &sound_desc, &sound, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(sound == 0);

    APAUDIO_PLAY_DESC play;
    memset(&play, 0, sizeof(play));
    play.struct_size = sizeof(play);
    play.gain = 1.0f;
    play.rate = 1.0f;
    APAUDIO_VOICE voice = 99;
    CHECK(apaudio_play(NULL, 1, &play, &voice) == APRESULT_INVALID_ARGUMENT);
    CHECK(voice == 0);
    // A stop only fails for a null mixer.
    CHECK(apaudio_voice_stop(NULL, 1, 0) == APRESULT_INVALID_ARGUMENT);
    CHECK(!apaudio_voice_is_live(NULL, 1));
    CHECK(apaudio_voice_set_gain(NULL, 1, 1.0f) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_voice_set_direction(NULL, 1, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_voice_set_rate(NULL, 1, 1.0f) == APRESULT_INVALID_ARGUMENT);

    APAUDIO_PCM pcm;
    CHECK(apaudio_wav_decode(NULL, 16, &pcm, NULL) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_wav_decode("RIFF", 4, NULL, NULL) == APRESULT_INVALID_ARGUMENT);

    // Calls returning nothing accept a null mixer and stale or 0 IDs.
    apaudio_mixer_destroy(NULL);
    apaudio_mixer_update(NULL);
    apaudio_bus_destroy(NULL, 0);
    apaudio_modifier_destroy(NULL, 0);
    apaudio_concurrency_destroy(NULL, 0);
    apaudio_sound_destroy(NULL, 0);
    apaudio_pcm_free(NULL);
}

static void test_pcm_free(void)
{
    current_test = "pcm_free";
    APAUDIO_PCM pcm;
    pcm.samples = (float *)malloc(4 * sizeof(float));
    pcm.frame_count = 2;
    pcm.channel_count = 2;
    pcm.sample_rate = 48000;
    apaudio_pcm_free(&pcm);
    CHECK(pcm.samples == NULL);
    CHECK(pcm.frame_count == 0);
    CHECK(pcm.channel_count == 0);
    CHECK(pcm.sample_rate == 0);
    apaudio_pcm_free(&pcm); // freeing twice is harmless once cleared
}

/* Builds a WAV in [out]: a fmt chunk of [fmt_size] bytes from [fmt], a
 * 1-byte junk chunk (odd, so padded) between it and the data, and [data].
 * Returns its size. */
static size_t make_wav(unsigned char *out, const unsigned char *fmt, uint32_t fmt_size, const void *data,
                       uint32_t data_size)
{
    size_t n = 0;
    memcpy(out + n, "RIFF\0\0\0\0WAVE", 12);
    n += 12;
    memcpy(out + n, "fmt ", 4);
    out[n + 4] = (unsigned char)fmt_size;
    out[n + 5] = out[n + 6] = out[n + 7] = 0;
    memcpy(out + n + 8, fmt, fmt_size);
    n += 8 + fmt_size;
    memcpy(out + n, "junk\1\0\0\0x\0", 10);
    n += 10;
    memcpy(out + n, "data", 4);
    out[n + 4] = (unsigned char)data_size;
    out[n + 5] = (unsigned char)(data_size >> 8);
    out[n + 6] = out[n + 7] = 0;
    memcpy(out + n + 8, data, data_size);
    n += 8 + data_size;
    uint32_t riff = (uint32_t)(n - 8);
    out[4] = (unsigned char)riff;
    out[5] = (unsigned char)(riff >> 8);
    return n;
}

/* A plain fmt chunk. */
static void make_fmt(unsigned char *fmt, uint16_t format, uint16_t channels, uint32_t rate, uint16_t bits)
{
    uint16_t align = (uint16_t)(channels * bits / 8);
    uint32_t byte_rate = rate * align;
    unsigned char f[16] = {(unsigned char)format, (unsigned char)(format >> 8), (unsigned char)channels, 0,
                           (unsigned char)rate, (unsigned char)(rate >> 8), (unsigned char)(rate >> 16), 0,
                           (unsigned char)byte_rate, (unsigned char)(byte_rate >> 8),
                           (unsigned char)(byte_rate >> 16), (unsigned char)(byte_rate >> 24),
                           (unsigned char)align, 0, (unsigned char)bits, 0};
    memcpy(fmt, f, 16);
}

static int near(float a, float b)
{
    return fabsf(a - b) < 1e-6f;
}

static void test_wav(void)
{
    current_test = "wav";
    unsigned char wav[256];
    unsigned char fmt[40];
    APAUDIO_PCM pcm;
    APAUDIO_ERROR error;

    /* 16-bit stereo: two frames, plus a stray byte that isn't a whole frame. */
    static const unsigned char pcm16[] = {0x00, 0x00, 0x00, 0x40, 0x00, 0x80, 0xFF, 0x7F, 0x12};
    make_fmt(fmt, 1, 2, 48000, 16);
    size_t size = make_wav(wav, fmt, 16, pcm16, sizeof(pcm16));
    CHECK(apaudio_wav_decode(wav, size, &pcm, &error) == APRESULT_OK);
    CHECK(pcm.frame_count == 2 && pcm.channel_count == 2 && pcm.sample_rate == 48000);
    CHECK(pcm.samples && near(pcm.samples[0], 0.0f) && near(pcm.samples[1], 0.5f));
    CHECK(near(pcm.samples[2], -1.0f) && near(pcm.samples[3], 32767.0f / 32768.0f));
    apaudio_pcm_free(&pcm);

    /* 8-bit is unsigned. */
    static const unsigned char pcm8[] = {0x80, 0x00, 0xC0};
    make_fmt(fmt, 1, 1, 22050, 8);
    size = make_wav(wav, fmt, 16, pcm8, sizeof(pcm8));
    CHECK(apaudio_wav_decode(wav, size, &pcm, &error) == APRESULT_OK);
    CHECK(pcm.frame_count == 3 && near(pcm.samples[0], 0.0f) && near(pcm.samples[1], -1.0f) &&
          near(pcm.samples[2], 0.5f));
    apaudio_pcm_free(&pcm);

    /* 24-bit, sign-extended. */
    static const unsigned char pcm24[] = {0x00, 0x00, 0xC0, 0x00, 0x00, 0x40};
    make_fmt(fmt, 1, 1, 44100, 24);
    size = make_wav(wav, fmt, 16, pcm24, sizeof(pcm24));
    CHECK(apaudio_wav_decode(wav, size, &pcm, &error) == APRESULT_OK);
    CHECK(pcm.frame_count == 2 && near(pcm.samples[0], -0.5f) && near(pcm.samples[1], 0.5f));
    apaudio_pcm_free(&pcm);

    /* 32-bit float, through WAVE_FORMAT_EXTENSIBLE. */
    float floats[2] = {0.25f, -0.75f};
    make_fmt(fmt, 0xFFFE, 1, 96000, 32);
    static const unsigned char extensible[24] = {22, 0, 32, 0, 4, 0, 0, 0, 0x03, 0x00, 0x00, 0x00,
                                                 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA,
                                                 0x00, 0x38, 0x9B, 0x71};
    memcpy(fmt + 16, extensible, sizeof(extensible));
    size = make_wav(wav, fmt, 40, floats, sizeof(floats));
    CHECK(apaudio_wav_decode(wav, size, &pcm, &error) == APRESULT_OK);
    CHECK(pcm.frame_count == 2 && pcm.sample_rate == 96000 && near(pcm.samples[0], 0.25f) &&
          near(pcm.samples[1], -0.75f));
    apaudio_pcm_free(&pcm);

    /* A data chunk claiming more than the file holds is read to the end. */
    make_fmt(fmt, 1, 1, 48000, 16);
    size = make_wav(wav, fmt, 16, pcm16, 8);
    wav[size - 8 - 4] = 0xFF; /* the data chunk's size: 255 bytes, 8 present */
    CHECK(apaudio_wav_decode(wav, size, &pcm, &error) == APRESULT_OK);
    CHECK(pcm.frame_count == 4);
    apaudio_pcm_free(&pcm);

    /* An empty data chunk is an empty sound. */
    size = make_wav(wav, fmt, 16, pcm16, 0);
    CHECK(apaudio_wav_decode(wav, size, &pcm, &error) == APRESULT_OK);
    CHECK(pcm.frame_count == 0 && pcm.channel_count == 1);
    apaudio_pcm_free(&pcm);

    /* Compressed encodings and odd sizes are unsupported. */
    make_fmt(fmt, 0x0002, 1, 48000, 4); /* MS ADPCM */
    size = make_wav(wav, fmt, 16, pcm16, 4);
    CHECK(apaudio_wav_decode(wav, size, &pcm, &error) == APRESULT_UNSUPPORTED);
    CHECK(pcm.samples == NULL && strstr(error.message, "0x0002") != NULL);
    make_fmt(fmt, 3, 1, 48000, 16); /* 16-bit float */
    size = make_wav(wav, fmt, 16, pcm16, 4);
    CHECK(apaudio_wav_decode(wav, size, &pcm, &error) == APRESULT_UNSUPPORTED);
    CHECK(apaudio_wav_decode("RF64\0\0\0\0WAVE", 12, &pcm, &error) == APRESULT_UNSUPPORTED);

    /* Broken files. */
    CHECK(apaudio_wav_decode("RIFF\4\0\0\0WAVE", 12, &pcm, &error) == APRESULT_CORRUPT_DATA);
    CHECK(strstr(error.message, "fmt") != NULL);
    CHECK(apaudio_wav_decode("RIFX\4\0\0\0WAVE", 12, &pcm, &error) == APRESULT_CORRUPT_DATA);
    CHECK(apaudio_wav_decode("", 0, &pcm, &error) == APRESULT_CORRUPT_DATA);
    make_fmt(fmt, 1, 2, 48000, 16);
    fmt[12] = 3; /* block align that isn't channels x sample size */
    size = make_wav(wav, fmt, 16, pcm16, 4);
    CHECK(apaudio_wav_decode(wav, size, &pcm, &error) == APRESULT_CORRUPT_DATA);
    make_fmt(fmt, 1, 1, 48000, 16);
    size = make_wav(wav, fmt, 16, pcm16, 4);
    CHECK(apaudio_wav_decode(wav, 30, &pcm, &error) == APRESULT_CORRUPT_DATA); /* cut inside fmt */
    CHECK(pcm.samples == NULL);
}

/* ---- The mixer --------------------------------------------------------- */

/* A mono sound with no direction plays equally on FL and FR. */
#define CENTRE 0.70710678f

static int close_to(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

static apaudio_mixer make_mixer(const APAUDIO_MIXER_DESC *desc)
{
    apaudio_mixer mixer = NULL;
    APAUDIO_ERROR error;
    APRESULT result = apaudio_mixer_create(desc, &mixer, &error);
    if (result != APRESULT_OK)
        printf("FAIL %s: apaudio_mixer_create: %s\n", current_test, error.message);
    CHECK(result == APRESULT_OK && mixer != NULL);
    return mixer;
}

/* A mixer whose transitions are instant, so tests see whole values. */
static APAUDIO_MIXER_DESC plain_mixer_desc(void)
{
    APAUDIO_MIXER_DESC desc = valid_mixer_desc();
    desc.transition_fade_us = 0;
    desc.max_fading_voices = 0;
    return desc;
}

static APAUDIO_SOUND make_sound(apaudio_mixer mixer, const float *samples, uint64_t frames, uint32_t channels,
                                uint32_t rate, APAUDIO_VIRTUAL_BEHAVIOR behavior, APAUDIO_CONCURRENCY group,
                                APAUDIO_BUS bus)
{
    APAUDIO_SOUND_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.samples = samples;
    desc.frame_count = frames;
    desc.channel_count = channels;
    desc.sample_rate = rate;
    desc.virtual_behavior = behavior;
    desc.concurrency = group;
    desc.bus = bus;
    APAUDIO_SOUND sound = 0;
    APAUDIO_ERROR error;
    APRESULT result = apaudio_sound_create(mixer, &desc, &sound, &error);
    if (result != APRESULT_OK)
        printf("FAIL %s: apaudio_sound_create: %s\n", current_test, error.message);
    CHECK(result == APRESULT_OK && sound != 0);
    return sound;
}

/* [frames] mono frames of 1.0 at the mixer's rate. */
static float ones[4096];

static APAUDIO_SOUND make_ones(apaudio_mixer mixer, uint64_t frames, APAUDIO_VIRTUAL_BEHAVIOR behavior)
{
    for (size_t i = 0; i < sizeof(ones) / sizeof(ones[0]); i++)
        ones[i] = 1.0f;
    return make_sound(mixer, ones, frames, 1, 48000, behavior, 0, 0);
}

static APAUDIO_PLAY_DESC play_desc(float gain)
{
    APAUDIO_PLAY_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.gain = gain;
    desc.rate = 1.0f;
    return desc;
}

static APAUDIO_VOICE play(apaudio_mixer mixer, APAUDIO_SOUND sound, float gain, uint8_t priority, bool loop)
{
    APAUDIO_PLAY_DESC desc = play_desc(gain);
    desc.priority = priority;
    desc.loop = loop;
    APAUDIO_VOICE voice = 0;
    CHECK(apaudio_play(mixer, sound, &desc, &voice) == APRESULT_OK);
    CHECK(voice != 0);
    return voice;
}

/* Interleaved stereo float output. */
static float out[2 * 2048];

static void render(apaudio_mixer mixer, uint32_t frames)
{
    for (uint32_t i = 0; i < 2 * frames; i++)
        out[i] = 99.0f; /* garbage the render must replace */
    apaudio_mixer_render(mixer, out, frames, NULL);
}

/* Every frame from [first] to [last] has [left] and [right]. */
static int frames_are(uint32_t first, uint32_t last, float left, float right)
{
    for (uint32_t i = first; i <= last; i++)
    {
        if (!close_to(out[2 * i], left) || !close_to(out[2 * i + 1], right))
        {
            printf("     frame %u is %g, %g, not %g, %g\n", i, out[2 * i], out[2 * i + 1], left, right);
            return 0;
        }
    }
    return 1;
}

static void test_mixer_create(void)
{
    current_test = "mixer_create";
    APAUDIO_ERROR error;
    apaudio_mixer mixer = NULL;
    APAUDIO_MIXER_DESC desc = valid_mixer_desc();

    static const SPUDAUDIO_CHANNEL_POSITION none[2] = {0, 0};
    static const SPUDAUDIO_CHANNEL_POSITION twice[2] = {SPUDAUDIO_CHANNEL_POSITION_FL, SPUDAUDIO_CHANNEL_POSITION_FL};
    desc.format.positions = none;
    CHECK(apaudio_mixer_create(&desc, &mixer, &error) == APRESULT_INVALID_ARGUMENT && error.message[0]);
    desc.format.positions = twice;
    CHECK(apaudio_mixer_create(&desc, &mixer, &error) == APRESULT_INVALID_ARGUMENT);
    desc.format.positions = NULL;
    CHECK(apaudio_mixer_create(&desc, &mixer, &error) == APRESULT_INVALID_ARGUMENT);

    desc = valid_mixer_desc();
    desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_UNKNOWN;
    CHECK(apaudio_mixer_create(&desc, &mixer, &error) == APRESULT_INVALID_ARGUMENT);
    desc = valid_mixer_desc();
    desc.max_real_voices = desc.max_voices + 1;
    CHECK(apaudio_mixer_create(&desc, &mixer, &error) == APRESULT_INVALID_ARGUMENT);
    desc = valid_mixer_desc();
    desc.resampler = APAUDIO_RESAMPLER_UNSPECIFIED; /* a zeroed desc chooses nothing */
    CHECK(apaudio_mixer_create(&desc, &mixer, &error) == APRESULT_INVALID_ARGUMENT);
    desc = valid_mixer_desc();
    desc.struct_size = 8;
    CHECK(apaudio_mixer_create(&desc, &mixer, &error) == APRESULT_INVALID_ARGUMENT);
    CHECK(mixer == NULL);

    desc = valid_mixer_desc();
    mixer = make_mixer(&desc);
    CHECK(apaudio_mixer_get_master_bus(mixer) != 0);
    CHECK(apaudio_mixer_get_master_bus(NULL) == 0);
    render(mixer, 64);
    CHECK(frames_are(0, 63, 0.0f, 0.0f)); /* silence when nothing plays */
    apaudio_mixer_update(mixer);
    apaudio_mixer_destroy(mixer);
}

static void test_play(void)
{
    current_test = "play";
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    apaudio_mixer mixer = make_mixer(&desc);
    APAUDIO_SOUND sound = make_ones(mixer, 100, APAUDIO_VIRTUAL_KILL);

    /* A one-shot plays its frames and ends. It's live from the play, before
     * any render, until the render that ends it. */
    APAUDIO_VOICE shot = play(mixer, sound, 1.0f, 0, false);
    CHECK(apaudio_voice_is_live(mixer, shot));
    CHECK(!apaudio_voice_is_live(mixer, 0) && !apaudio_voice_is_live(mixer, shot + 1));
    render(mixer, 64);
    CHECK(frames_are(0, 63, CENTRE, CENTRE));
    CHECK(apaudio_voice_is_live(mixer, shot));
    render(mixer, 64);
    CHECK(frames_are(0, 35, CENTRE, CENTRE));
    CHECK(frames_are(36, 63, 0.0f, 0.0f));
    CHECK(!apaudio_voice_is_live(mixer, shot));

    /* Bad plays. */
    APAUDIO_PLAY_DESC bad = play_desc(1.0f);
    APAUDIO_VOICE voice = 99;
    bad.rate = 0.0f; /* a zeroed desc picks no rate */
    CHECK(apaudio_play(mixer, sound, &bad, &voice) == APRESULT_INVALID_ARGUMENT && voice == 0);
    bad = play_desc(-1.0f);
    CHECK(apaudio_play(mixer, sound, &bad, NULL) == APRESULT_INVALID_ARGUMENT);
    bad = play_desc(1.0f);
    CHECK(apaudio_play(mixer, 0, &bad, NULL) == APRESULT_NOT_FOUND);
    CHECK(apaudio_play(mixer, sound + 1, &bad, NULL) == APRESULT_NOT_FOUND);

    /* A gain change ramps across the next render. */
    APAUDIO_SOUND longer = make_ones(mixer, 4000, APAUDIO_VIRTUAL_KILL);
    voice = play(mixer, longer, 0.5f, 0, false);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.5f * CENTRE, 0.5f * CENTRE));
    CHECK(apaudio_voice_set_gain(mixer, voice, 1.0f) == APRESULT_OK);
    CHECK(apaudio_voice_set_gain(mixer, voice, -1.0f) == APRESULT_INVALID_ARGUMENT);
    render(mixer, 16);
    CHECK(out[0] > 0.5f * CENTRE && out[0] < out[14] && close_to(out[30], CENTRE));
    render(mixer, 16);
    CHECK(frames_are(0, 15, CENTRE, CENTRE));

    /* A stop fades out over its release, and the voice is gone. */
    CHECK(apaudio_voice_stop(mixer, voice, 8) == APRESULT_OK);
    CHECK(apaudio_voice_stop(mixer, voice, 4000) == APRESULT_OK); /* already stopping: nothing */
    CHECK(apaudio_voice_is_live(mixer, voice)); /* until its fade has rendered */
    render(mixer, 16);
    CHECK(!apaudio_voice_is_live(mixer, voice));
    CHECK(close_to(out[0], CENTRE * 7.0f / 8.0f) && close_to(out[2 * 6], CENTRE / 8.0f));
    CHECK(frames_are(7, 15, 0.0f, 0.0f));
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.0f, 0.0f));
    CHECK(apaudio_voice_stop(mixer, voice, 0) == APRESULT_OK); /* finished: harmless */
    CHECK(apaudio_voice_set_gain(mixer, voice, 1.0f) == APRESULT_OK);

    /* A voice stopped before it starts never sounds; one changed before it
     * starts starts with the new value. */
    voice = play(mixer, longer, 1.0f, 0, false);
    apaudio_voice_stop(mixer, voice, 100);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.0f, 0.0f));
    CHECK(!apaudio_voice_is_live(mixer, voice));
    voice = play(mixer, longer, 1.0f, 0, false);
    apaudio_voice_set_gain(mixer, voice, 0.25f);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.25f * CENTRE, 0.25f * CENTRE));
    apaudio_voice_stop(mixer, voice, 0); /* cut */
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.0f, 0.0f));

    /* A loop plays until stopped. More frames than max_frames render too. */
    static const float pattern[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    APAUDIO_SOUND looped = make_sound(mixer, pattern, 4, 1, 48000, APAUDIO_VIRTUAL_KILL, 0, 0);
    voice = play(mixer, looped, 1.0f, 0, true);
    render(mixer, 1024);
    int repeats = 1;
    for (uint32_t i = 0; i < 1024; i++)
        repeats = repeats && close_to(out[2 * i], pattern[i % 4] * CENTRE);
    CHECK(repeats);
    apaudio_voice_stop(mixer, voice, 0);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.0f, 0.0f));

    /* The queue holds queue_capacity plays between renders. */
    APAUDIO_PLAY_DESC one = play_desc(0.01f);
    for (uint32_t i = 0; i < desc.queue_capacity; i++)
        CHECK(apaudio_play(mixer, sound, &one, NULL) == APRESULT_OK);
    CHECK(apaudio_play(mixer, sound, &one, &voice) == APRESULT_LIMIT && voice == 0);
    render(mixer, 16);
    CHECK(apaudio_play(mixer, sound, &one, NULL) == APRESULT_OK);
    apaudio_mixer_destroy(mixer);
}

static void test_stereo_and_direction(void)
{
    current_test = "stereo_and_direction";
    const double pi = 3.14159265358979323846;
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    apaudio_mixer mixer = make_mixer(&desc);

    /* A stereo sound plays on FL and FR, and ignores direction. */
    float stereo_samples[2 * 64];
    for (int i = 0; i < 64; i++)
    {
        stereo_samples[2 * i] = 0.25f;
        stereo_samples[2 * i + 1] = -0.5f;
    }
    APAUDIO_SOUND wide = make_sound(mixer, stereo_samples, 64, 2, 48000, APAUDIO_VIRTUAL_KILL, 0, 0);
    APAUDIO_DIRECTION right = {pi / 2, 0.0};
    APAUDIO_PLAY_DESC wide_play = play_desc(1.0f);
    wide_play.direction = &right;
    CHECK(apaudio_play(mixer, wide, &wide_play, NULL) == APRESULT_OK);
    render(mixer, 64);
    CHECK(frames_are(0, 63, 0.25f, -0.5f));

    /* A mono sound is panned: hard right and left at the sides, mirrored to
     * the front from behind. */
    APAUDIO_SOUND sound = make_ones(mixer, 4000, APAUDIO_VIRTUAL_KILL);
    static const struct
    {
        double azimuth;
        float left, right;
    } cases[] = {{0.0, CENTRE, CENTRE},          {1.5707963267948966, 0.0f, 1.0f},
                 {-1.5707963267948966, 1.0f, 0.0f}, {3.14159265358979323846, CENTRE, CENTRE},
                 {2.356194490192345, 0.38268343f, 0.92387953f}, {0.7853981633974483, 0.38268343f, 0.92387953f}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
    {
        APAUDIO_DIRECTION direction = {cases[i].azimuth, 0.5}; /* stereo has no height: elevation is ignored */
        APAUDIO_PLAY_DESC panned = play_desc(1.0f);
        panned.direction = &direction;
        APAUDIO_VOICE voice = 0;
        CHECK(apaudio_play(mixer, sound, &panned, &voice) == APRESULT_OK);
        render(mixer, 8);
        CHECK(frames_are(0, 7, cases[i].left, cases[i].right));
        apaudio_voice_stop(mixer, voice, 0);
    }

    /* A direction change moves the pan smoothly across the next render. */
    APAUDIO_VOICE voice = play(mixer, sound, 1.0f, 0, false);
    render(mixer, 8);
    CHECK(frames_are(0, 7, CENTRE, CENTRE));
    CHECK(apaudio_voice_set_direction(mixer, voice, &right) == APRESULT_OK);
    render(mixer, 8);
    CHECK(out[0] < CENTRE && out[1] > CENTRE && out[2 * 3] > out[2 * 6]);
    CHECK(close_to(out[2 * 7], 0.0f) && close_to(out[2 * 7 + 1], 1.0f));
    CHECK(apaudio_voice_set_direction(mixer, voice, NULL) == APRESULT_OK);
    render(mixer, 8);
    CHECK(close_to(out[2 * 7], CENTRE) && close_to(out[2 * 7 + 1], CENTRE));
    APAUDIO_DIRECTION bad = {NAN, 0.0};
    CHECK(apaudio_voice_set_direction(mixer, voice, &bad) == APRESULT_INVALID_ARGUMENT);
    apaudio_mixer_destroy(mixer);

    /* 5.1: no direction is the centre speaker alone; behind is the rears;
     * the LFE gets nothing. */
    static const SPUDAUDIO_CHANNEL_POSITION surround[6] = {
        SPUDAUDIO_CHANNEL_POSITION_FL, SPUDAUDIO_CHANNEL_POSITION_FR,  SPUDAUDIO_CHANNEL_POSITION_FC,
        SPUDAUDIO_CHANNEL_POSITION_LFE, SPUDAUDIO_CHANNEL_POSITION_RL, SPUDAUDIO_CHANNEL_POSITION_RR};
    desc.format.channel_count = 6;
    desc.format.positions = surround;
    mixer = make_mixer(&desc);
    sound = make_ones(mixer, 4000, APAUDIO_VIRTUAL_KILL);
    float six[6 * 8];
    voice = play(mixer, sound, 1.0f, 0, false);
    apaudio_mixer_render(mixer, six, 8, NULL);
    CHECK(close_to(six[2], 1.0f) && close_to(six[0], 0.0f) && close_to(six[1], 0.0f) && close_to(six[3], 0.0f) &&
          close_to(six[4], 0.0f) && close_to(six[5], 0.0f));
    apaudio_voice_stop(mixer, voice, 0);
    APAUDIO_DIRECTION behind = {pi, 0.0};
    APAUDIO_PLAY_DESC rear = play_desc(1.0f);
    rear.direction = &behind;
    CHECK(apaudio_play(mixer, sound, &rear, NULL) == APRESULT_OK);
    apaudio_mixer_render(mixer, six, 8, NULL);
    CHECK(close_to(six[4], CENTRE) && close_to(six[5], CENTRE) && close_to(six[0], 0.0f) && close_to(six[1], 0.0f) &&
          close_to(six[2], 0.0f) && close_to(six[3], 0.0f));
    apaudio_mixer_destroy(mixer);
}

static void test_resampling(void)
{
    current_test = "resampling";
    const double pi = 3.14159265358979323846;
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    apaudio_mixer mixer = make_mixer(&desc);
    float ramp[1000];
    for (int i = 0; i < 1000; i++)
        ramp[i] = (float)i / 1000.0f;

    /* Twice as fast, and half: linear interpolation. */
    APAUDIO_SOUND sound = make_sound(mixer, ramp, 1000, 1, 48000, APAUDIO_VIRTUAL_KILL, 0, 0);
    APAUDIO_PLAY_DESC fast = play_desc(1.0f);
    fast.rate = 2.0f;
    APAUDIO_VOICE voice = 0;
    CHECK(apaudio_play(mixer, sound, &fast, &voice) == APRESULT_OK);
    render(mixer, 32);
    int matches = 1;
    for (int i = 0; i < 32; i++)
        matches = matches && close_to(out[2 * i], ramp[2 * i] * CENTRE);
    CHECK(matches);
    apaudio_voice_stop(mixer, voice, 0);
    APAUDIO_PLAY_DESC slow = play_desc(1.0f);
    slow.rate = 0.5f;
    CHECK(apaudio_play(mixer, sound, &slow, &voice) == APRESULT_OK);
    render(mixer, 32);
    matches = 1;
    for (int i = 0; i < 32; i++)
        matches = matches && close_to(out[2 * i], (float)i / 2000.0f * CENTRE);
    CHECK(matches);

    /* A rate change glides: the voice carries on from where it was. */
    CHECK(apaudio_voice_set_rate(mixer, voice, 1.0f) == APRESULT_OK);
    CHECK(apaudio_voice_set_rate(mixer, voice, 9.0f) == APRESULT_INVALID_ARGUMENT);
    render(mixer, 32);
    CHECK(out[0] > 15.0f / 1000.0f * CENTRE && out[0] < 17.0f / 1000.0f * CENTRE);
    float before = out[2 * 31];
    render(mixer, 4);
    CHECK(close_to(out[2] - out[0], CENTRE / 1000.0f) && out[0] > before);
    apaudio_voice_stop(mixer, voice, 0);

    /* A sound keeps its own rate: 24 kHz plays at half speed in a 48 kHz mix. */
    APAUDIO_SOUND half = make_sound(mixer, ramp, 1000, 1, 24000, APAUDIO_VIRTUAL_KILL, 0, 0);
    voice = play(mixer, half, 1.0f, 0, false);
    render(mixer, 32);
    matches = 1;
    for (int i = 0; i < 32; i++)
        matches = matches && close_to(out[2 * i], (float)i / 2000.0f * CENTRE);
    CHECK(matches);
    APAUDIO_SOUND_DESC bad;
    memset(&bad, 0, sizeof(bad));
    bad.struct_size = sizeof(bad);
    bad.samples = ramp;
    bad.frame_count = 10;
    bad.channel_count = 1;
    bad.sample_rate = 500;
    bad.virtual_behavior = APAUDIO_VIRTUAL_KILL;
    APAUDIO_SOUND none;
    CHECK(apaudio_sound_create(mixer, &bad, &none, NULL) == APRESULT_INVALID_ARGUMENT);
    bad.sample_rate = 48000;
    bad.channel_count = 3;
    CHECK(apaudio_sound_create(mixer, &bad, &none, NULL) == APRESULT_UNSUPPORTED);
    bad.channel_count = 1;
    bad.virtual_behavior = APAUDIO_VIRTUAL_UNSPECIFIED;
    CHECK(apaudio_sound_create(mixer, &bad, &none, NULL) == APRESULT_INVALID_ARGUMENT);
    apaudio_mixer_destroy(mixer);

    /* Sinc: a 1 kHz tone at 44.1 kHz comes out a 1 kHz tone at 48 kHz, and
     * one played 4 times too fast for the mix (15 kHz -> 60 kHz, past
     * Nyquist) is filtered out rather than aliased. */
    desc.resampler = APAUDIO_RESAMPLER_SINC;
    mixer = make_mixer(&desc);
    static float tone[8820];
    for (int i = 0; i < 8820; i++)
        tone[i] = (float)sin(2.0 * pi * 1000.0 * i / 44100.0);
    sound = make_sound(mixer, tone, 8820, 1, 44100, APAUDIO_VIRTUAL_KILL, 0, 0);
    voice = play(mixer, sound, 1.0f, 0, true);
    render(mixer, 512);
    float worst = 0.0f;
    for (int i = 32; i < 512; i++)
    {
        float error = fabsf(out[2 * i] - (float)sin(2.0 * pi * 1000.0 * i / 48000.0) * CENTRE);
        worst = error > worst ? error : worst;
    }
    CHECK(worst < 2e-3f);
    apaudio_voice_stop(mixer, voice, 0);
    for (int i = 0; i < 8820; i++)
        tone[i] = (float)sin(2.0 * pi * 15000.0 * i / 44100.0);
    sound = make_sound(mixer, tone, 8820, 1, 44100, APAUDIO_VIRTUAL_KILL, 0, 0);
    APAUDIO_PLAY_DESC high = play_desc(1.0f);
    high.rate = 4.0f;
    high.loop = true;
    CHECK(apaudio_play(mixer, sound, &high, NULL) == APRESULT_OK);
    render(mixer, 512);
    worst = 0.0f;
    for (int i = 64; i < 512; i++)
        worst = fabsf(out[2 * i]) > worst ? fabsf(out[2 * i]) : worst;
    CHECK(worst < 2e-3f);
    apaudio_mixer_destroy(mixer);
}

static void test_formats(void)
{
    current_test = "formats";
    float stereo_samples[2 * 64];
    for (int i = 0; i < 64; i++)
    {
        stereo_samples[2 * i] = 2.0f; /* clips */
        stereo_samples[2 * i + 1] = -0.5f;
    }

    /* Integer formats are clamped. */
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_S16;
    apaudio_mixer mixer = make_mixer(&desc);
    APAUDIO_SOUND sound = make_sound(mixer, stereo_samples, 64, 2, 48000, APAUDIO_VIRTUAL_KILL, 0, 0);
    play(mixer, sound, 1.0f, 0, false);
    int16_t s16[2 * 8];
    apaudio_mixer_render(mixer, s16, 8, NULL);
    CHECK(s16[0] == 32767 && s16[1] == -16384 && s16[14] == 32767 && s16[15] == -16384);
    apaudio_mixer_destroy(mixer);

    desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_S24_PACKED;
    mixer = make_mixer(&desc);
    sound = make_sound(mixer, stereo_samples, 64, 2, 48000, APAUDIO_VIRTUAL_KILL, 0, 0);
    play(mixer, sound, 1.0f, 0, false);
    unsigned char s24[2 * 3 * 8];
    apaudio_mixer_render(mixer, s24, 8, NULL);
    uint32_t left = 0, right = 0;
    memcpy(&left, s24, 3);
    memcpy(&right, s24 + 3, 3);
    const uint16_t endian = 1;
    if (*(const unsigned char *)&endian == 1)
        CHECK(left == 0x7FFFFF && right == 0xC00000);
    apaudio_mixer_destroy(mixer);

    desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_S32;
    mixer = make_mixer(&desc);
    sound = make_sound(mixer, stereo_samples, 64, 2, 48000, APAUDIO_VIRTUAL_KILL, 0, 0);
    play(mixer, sound, 1.0f, 0, false);
    int32_t s32[2 * 8];
    apaudio_mixer_render(mixer, s32, 8, NULL);
    CHECK(s32[0] == INT32_MAX && s32[1] == INT32_MIN / 2);
    apaudio_mixer_destroy(mixer);

    /* Planar: one buffer per channel. */
    desc.format.sample_format = SPUDAUDIO_SAMPLE_FORMAT_F32;
    desc.format.interleaved = false;
    mixer = make_mixer(&desc);
    sound = make_sound(mixer, stereo_samples, 64, 2, 48000, APAUDIO_VIRTUAL_KILL, 0, 0);
    play(mixer, sound, 1.0f, 0, false);
    float plane_left[8], plane_right[8];
    void *planes[2] = {plane_left, plane_right};
    apaudio_mixer_render(mixer, planes, 8, NULL);
    CHECK(close_to(plane_left[0], 2.0f) && close_to(plane_left[7], 2.0f) && close_to(plane_right[0], -0.5f) &&
          close_to(plane_right[7], -0.5f));

    /* A new format: the sound is kept, and its voice carries on - here mixed
     * down, at another rate. A format the mixer can't render changes nothing. */
    static const SPUDAUDIO_CHANNEL_POSITION mono[1] = {SPUDAUDIO_CHANNEL_POSITION_MONO};
    APAUDIO_FORMAT format = desc.format;
    APAUDIO_ERROR error;
    format.channel_count = 0;
    CHECK(apaudio_mixer_set_format(mixer, &format, 64, &error) == APRESULT_INVALID_ARGUMENT && error.message[0]);
    apaudio_mixer_render(mixer, planes, 8, NULL);
    CHECK(close_to(plane_left[7], 2.0f) && close_to(plane_right[7], -0.5f));
    format.channel_count = 1;
    format.positions = mono;
    format.interleaved = true;
    format.sample_rate = 96000;
    CHECK(apaudio_mixer_set_format(mixer, &format, 64, &error) == APRESULT_OK);
    float single[64];
    apaudio_mixer_render(mixer, single, 64, NULL);
    CHECK(close_to(single[0], 0.75f) && close_to(single[63], 0.75f)); /* (2 - 0.5) / 2 */
    apaudio_mixer_render(mixer, single, 64, NULL);
    /* 16 of the sound's 64 frames went at 48 kHz; the other 48 last 96 here. */
    CHECK(close_to(single[30], 0.75f) && close_to(single[40], 0.0f));
    apaudio_mixer_destroy(mixer);
}

static void test_sounds(void)
{
    current_test = "sounds";
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    desc.max_sounds = 1;
    apaudio_mixer mixer = make_mixer(&desc);
    APAUDIO_SOUND sound = make_ones(mixer, 4000, APAUDIO_VIRTUAL_KILL);
    play(mixer, sound, 1.0f, 0, true);
    render(mixer, 16);
    CHECK(frames_are(0, 15, CENTRE, CENTRE));

    /* Destroyed: stale at once, and its voice stops at the next render -
     * faded across it, not clicked. */
    APAUDIO_PLAY_DESC again = play_desc(1.0f);
    CHECK(apaudio_play(mixer, sound, &again, NULL) == APRESULT_OK); /* queued before the destroy: never sounds */
    apaudio_sound_destroy(mixer, sound);
    apaudio_sound_destroy(mixer, sound); /* stale: nothing */
    CHECK(apaudio_play(mixer, sound, &again, NULL) == APRESULT_NOT_FOUND);

    /* Its slot isn't reusable until a render has let go of it. */
    APAUDIO_SOUND_DESC sound_desc;
    memset(&sound_desc, 0, sizeof(sound_desc));
    sound_desc.struct_size = sizeof(sound_desc);
    sound_desc.samples = ones;
    sound_desc.frame_count = 8;
    sound_desc.channel_count = 1;
    sound_desc.sample_rate = 48000;
    sound_desc.virtual_behavior = APAUDIO_VIRTUAL_KILL;
    APAUDIO_SOUND next = 99;
    APAUDIO_ERROR error;
    CHECK(apaudio_sound_create(mixer, &sound_desc, &next, &error) == APRESULT_LIMIT && next == 0);
    render(mixer, 16);
    CHECK(out[0] < CENTRE && out[0] > out[2] && close_to(out[2 * 15], 0.0f));
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.0f, 0.0f));
    CHECK(apaudio_sound_create(mixer, &sound_desc, &next, &error) == APRESULT_OK);
    /* The old ID never plays the slot's new sound. */
    CHECK(next != sound && apaudio_play(mixer, sound, &again, NULL) == APRESULT_NOT_FOUND);
    CHECK(apaudio_play(mixer, next, &again, NULL) == APRESULT_OK);
    render(mixer, 16);
    CHECK(frames_are(0, 7, CENTRE, CENTRE) && frames_are(8, 15, 0.0f, 0.0f));

    /* An empty sound is a sound; it just ends at once. */
    apaudio_sound_destroy(mixer, next);
    render(mixer, 16);
    sound_desc.samples = NULL;
    sound_desc.frame_count = 0;
    CHECK(apaudio_sound_create(mixer, &sound_desc, &next, &error) == APRESULT_OK);
    again.loop = true;
    CHECK(apaudio_play(mixer, next, &again, NULL) == APRESULT_OK);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.0f, 0.0f));
    apaudio_mixer_destroy(mixer); /* frees the sounds still alive */
}

static void test_prioritization(void)
{
    current_test = "prioritization";
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    desc.max_real_voices = 2;
    apaudio_mixer mixer = make_mixer(&desc);
    APAUDIO_SOUND keeps = make_ones(mixer, 4000, APAUDIO_VIRTUAL_CONTINUE);
    APAUDIO_SOUND kills = make_ones(mixer, 4000, APAUDIO_VIRTUAL_KILL);

    /* Two real slots: the third voice, of lower priority, starts virtual. */
    play(mixer, keeps, 0.1f, 5, true);
    APAUDIO_VOICE second = play(mixer, keeps, 0.2f, 5, true);
    play(mixer, keeps, 0.4f, 1, true);
    APAUDIO_VOICE killed = play(mixer, kills, 0.8f, 1, true); /* KILL: ends instead */
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.3f * CENTRE, 0.3f * CENTRE));
    /* A voice never loses its slot to a lower priority, however loud. */
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.3f * CENTRE, 0.3f * CENTRE));

    /* A slot comes free: the virtual voice takes it at the next update - not
     * before - and the killed one is gone for good. */
    apaudio_voice_stop(mixer, second, 0);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.1f * CENTRE, 0.1f * CENTRE));
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.5f * CENTRE, 0.5f * CENTRE));
    apaudio_voice_set_gain(mixer, killed, 1.0f); /* stale: nothing */

    /* A new play of higher priority takes the lowest-ranked real voice's
     * slot at once, without waiting for an update. */
    APAUDIO_VOICE high = play(mixer, keeps, 0.3f, 9, true);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.4f * CENTRE, 0.4f * CENTRE));
    /* Within a priority, the more audible voice wins at the update. */
    apaudio_voice_stop(mixer, high, 0);
    play(mixer, keeps, 0.05f, 5, true); /* takes the free slot */
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.15f * CENTRE, 0.15f * CENTRE));
    play(mixer, keeps, 0.6f, 5, true); /* louder, same priority */
    render(mixer, 16);
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.7f * CENTRE, 0.7f * CENTRE)); /* 0.6 and 0.1, over 0.05 and 0.4 (priority 1) */
    apaudio_mixer_destroy(mixer);

    /* Below the threshold a voice is virtual even with slots free. */
    desc.virtual_threshold = 0.01f;
    mixer = make_mixer(&desc);
    keeps = make_ones(mixer, 4000, APAUDIO_VIRTUAL_CONTINUE);
    APAUDIO_VOICE quiet = play(mixer, keeps, 0.001f, 0, true);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.0f, 0.0f));
    apaudio_voice_set_gain(mixer, quiet, 1.0f);
    render(mixer, 16);
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    CHECK(frames_are(0, 15, CENTRE, CENTRE));
    apaudio_mixer_destroy(mixer);

    /* Transitions fade: the voice pushed out fades in a fading slot while
     * its replacement plays at once; coming back, it fades in. */
    desc = valid_mixer_desc();
    desc.max_real_voices = 1;
    desc.max_fading_voices = 1;
    desc.transition_fade_us = 1000; /* 48 frames */
    mixer = make_mixer(&desc);
    keeps = make_ones(mixer, 4000, APAUDIO_VIRTUAL_PAUSE);
    play(mixer, keeps, 0.5f, 1, true);
    render(mixer, 16);
    APAUDIO_VOICE over = play(mixer, keeps, 0.25f, 2, true);
    render(mixer, 64);
    CHECK(close_to(out[0], (0.25f + 0.5f * 47.0f / 48.0f) * CENTRE));
    CHECK(close_to(out[2 * 23], (0.25f + 0.5f * 24.0f / 48.0f) * CENTRE));
    CHECK(frames_are(47, 63, 0.25f * CENTRE, 0.25f * CENTRE));
    apaudio_voice_stop(mixer, over, 0);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.0f, 0.0f));
    apaudio_mixer_update(mixer);
    render(mixer, 64);
    CHECK(close_to(out[0], 0.5f / 48.0f * CENTRE) && close_to(out[2 * 23], 0.25f * CENTRE));
    CHECK(frames_are(47, 63, 0.5f * CENTRE, 0.5f * CENTRE));
    apaudio_mixer_destroy(mixer);

    /* max_voices is a limit too: a new play ends the weakest voice, or is
     * dropped if every voice outranks it. */
    desc = plain_mixer_desc();
    desc.max_voices = 2;
    desc.max_real_voices = 2;
    mixer = make_mixer(&desc);
    keeps = make_ones(mixer, 4000, APAUDIO_VIRTUAL_CONTINUE);
    play(mixer, keeps, 0.1f, 5, true);
    play(mixer, keeps, 0.2f, 5, true);
    play(mixer, keeps, 0.4f, 1, true); /* dropped */
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.3f * CENTRE, 0.3f * CENTRE));
    play(mixer, keeps, 0.4f, 5, true); /* ends the quieter of the two */
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.6f * CENTRE, 0.6f * CENTRE));
    apaudio_mixer_destroy(mixer);
}

static APAUDIO_CONCURRENCY make_group(apaudio_mixer mixer, uint32_t max_instances, APAUDIO_RESOLUTION resolution,
                                      uint32_t retrigger_us, float older_gain)
{
    APAUDIO_CONCURRENCY_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.max_instances = max_instances;
    desc.resolution = resolution;
    desc.retrigger_us = retrigger_us;
    desc.older_instance_gain = older_gain;
    APAUDIO_CONCURRENCY group = 0;
    CHECK(apaudio_concurrency_create(mixer, &desc, &group, NULL) == APRESULT_OK && group != 0);
    return group;
}

static void test_concurrency(void)
{
    current_test = "concurrency";
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    desc.max_concurrency_groups = 2;
    apaudio_mixer mixer = make_mixer(&desc);
    make_ones(mixer, 1, APAUDIO_VIRTUAL_KILL); /* fills ones[] */

    APAUDIO_CONCURRENCY_DESC bad;
    memset(&bad, 0, sizeof(bad));
    bad.struct_size = sizeof(bad);
    APAUDIO_CONCURRENCY none = 99;
    APAUDIO_ERROR error;
    CHECK(apaudio_concurrency_create(mixer, &bad, &none, &error) == APRESULT_INVALID_ARGUMENT && none == 0);

    /* REJECT_NEW: the play over the limit is dropped. */
    APAUDIO_CONCURRENCY group = make_group(mixer, 2, APAUDIO_RESOLUTION_REJECT_NEW, 0, 1.0f);
    APAUDIO_SOUND sound = make_sound(mixer, ones, 4000, 1, 48000, APAUDIO_VIRTUAL_KILL, group, 0);
    APAUDIO_VOICE voices[3];
    for (int i = 0; i < 3; i++)
        voices[i] = play(mixer, sound, 0.1f, 0, true);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.2f * CENTRE, 0.2f * CENTRE));
    /* A destroyed group limits nothing; its voices carry on. */
    apaudio_concurrency_destroy(mixer, group);
    apaudio_concurrency_destroy(mixer, group);
    play(mixer, sound, 0.1f, 0, true);
    play(mixer, sound, 0.1f, 0, true);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.4f * CENTRE, 0.4f * CENTRE));
    apaudio_mixer_destroy(mixer);
    (void)voices;

    /* STOP_OLDEST, with each newer start turning the older voices down. */
    mixer = make_mixer(&desc);
    group = make_group(mixer, 2, APAUDIO_RESOLUTION_STOP_OLDEST, 0, 0.5f);
    sound = make_sound(mixer, ones, 4000, 1, 48000, APAUDIO_VIRTUAL_KILL, group, 0);
    play(mixer, sound, 0.8f, 0, true);
    render(mixer, 16);
    play(mixer, sound, 0.4f, 0, true);
    render(mixer, 16);
    CHECK(close_to(out[2 * 15], (0.4f + 0.4f) * CENTRE)); /* the first, halved */
    play(mixer, sound, 0.2f, 0, true);
    render(mixer, 16);
    CHECK(close_to(out[2 * 15], (0.2f + 0.2f) * CENTRE)); /* the first stopped; the second, halved */

    /* A play may name another group than its sound's. STOP_LOWEST_PRIORITY
     * only stops a voice the new one is no lower than. */
    APAUDIO_CONCURRENCY other = make_group(mixer, 1, APAUDIO_RESOLUTION_STOP_LOWEST_PRIORITY, 0, 1.0f);
    CHECK(apaudio_concurrency_create(mixer, &bad, &none, &error) == APRESULT_INVALID_ARGUMENT);
    APAUDIO_PLAY_DESC named = play_desc(0.05f);
    named.concurrency = other;
    named.priority = 5;
    named.loop = true;
    CHECK(apaudio_play(mixer, sound, &named, NULL) == APRESULT_OK);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.45f * CENTRE, 0.45f * CENTRE));
    named.priority = 4; /* lower: dropped */
    named.gain = 0.01f;
    CHECK(apaudio_play(mixer, sound, &named, NULL) == APRESULT_OK);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.45f * CENTRE, 0.45f * CENTRE));
    named.priority = 5; /* equal: replaces */
    CHECK(apaudio_play(mixer, sound, &named, NULL) == APRESULT_OK);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.41f * CENTRE, 0.41f * CENTRE));
    CHECK(apaudio_concurrency_create(mixer, &bad, &none, &error) == APRESULT_INVALID_ARGUMENT);
    bad.max_instances = 1;
    bad.resolution = APAUDIO_RESOLUTION_REJECT_NEW;
    bad.older_instance_gain = 1.0f;
    CHECK(apaudio_concurrency_create(mixer, &bad, &none, &error) == APRESULT_LIMIT && none == 0);
    apaudio_mixer_destroy(mixer);

    /* retrigger_us: a play too soon after the group's last start is dropped. */
    mixer = make_mixer(&desc);
    group = make_group(mixer, 8, APAUDIO_RESOLUTION_REJECT_NEW, 1000, 1.0f); /* 48 frames */
    sound = make_sound(mixer, ones, 4000, 1, 48000, APAUDIO_VIRTUAL_KILL, group, 0);
    play(mixer, sound, 0.1f, 0, true);
    play(mixer, sound, 0.1f, 0, true); /* same render: dropped */
    render(mixer, 32);
    play(mixer, sound, 0.1f, 0, true); /* 32 frames on: dropped */
    render(mixer, 32);
    CHECK(frames_are(0, 31, 0.1f * CENTRE, 0.1f * CENTRE));
    play(mixer, sound, 0.1f, 0, true); /* 64 frames on */
    render(mixer, 32);
    CHECK(frames_are(0, 31, 0.2f * CENTRE, 0.2f * CENTRE));
    apaudio_mixer_destroy(mixer);
}

static APAUDIO_BUS make_bus(apaudio_mixer mixer, APAUDIO_BUS parent, float gain)
{
    APAUDIO_BUS_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    desc.parent = parent;
    desc.gain = gain;
    desc.rate = 1.0f;
    APAUDIO_BUS bus = 0;
    APAUDIO_ERROR error;
    CHECK(apaudio_bus_create(mixer, &desc, &bus, &error) == APRESULT_OK && bus != 0);
    return bus;
}

static APAUDIO_VOICE play_on(apaudio_mixer mixer, APAUDIO_SOUND sound, float gain, APAUDIO_BUS bus)
{
    APAUDIO_PLAY_DESC desc = play_desc(gain);
    desc.bus = bus;
    desc.loop = true;
    APAUDIO_VOICE voice = 0;
    CHECK(apaudio_play(mixer, sound, &desc, &voice) == APRESULT_OK);
    return voice;
}

static void test_buses(void)
{
    current_test = "buses";
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    desc.max_buses = 3;
    desc.gain = 0.5f; /* the master's */
    apaudio_mixer mixer = make_mixer(&desc);
    APAUDIO_BUS master = apaudio_mixer_get_master_bus(mixer);

    /* A voice's gain is its own times every bus's on the way to the master;
     * a bus is usable the moment it's created. */
    APAUDIO_BUS music = make_bus(mixer, 0, 0.5f);
    APAUDIO_BUS strings = make_bus(mixer, music, 0.5f);
    APAUDIO_SOUND sound = make_sound(mixer, ones, 4000, 1, 48000, APAUDIO_VIRTUAL_KILL, 0, music);
    make_ones(mixer, 1, APAUDIO_VIRTUAL_KILL);
    play_on(mixer, sound, 1.0f, 0);       /* the sound's bus: music */
    play_on(mixer, sound, 1.0f, strings); /* named by the play */
    play_on(mixer, sound, 1.0f, master);  /* the master, explicitly */
    render(mixer, 16);
    CHECK(frames_are(0, 15, (0.25f + 0.125f + 0.5f) * CENTRE, (0.25f + 0.125f + 0.5f) * CENTRE));

    /* Changes ramp across the next render, or over their fade. */
    CHECK(apaudio_bus_set_gain(mixer, music, 1.0f, 0) == APRESULT_OK);
    render(mixer, 16);
    CHECK(out[0] > 0.875f * CENTRE && close_to(out[2 * 15], (0.5f + 0.25f + 0.5f) * CENTRE));
    CHECK(apaudio_bus_set_gain(mixer, master, 1.0f, 1000) == APRESULT_OK); /* 48 frames */
    render(mixer, 24);
    CHECK(close_to(out[2 * 23], 1.25f * 1.5f * CENTRE)); /* half way from 0.5 to 1 */
    render(mixer, 24);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 2.5f * CENTRE, 2.5f * CENTRE));
    CHECK(apaudio_bus_set_gain(mixer, music, -1.0f, 0) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_bus_set_rate(mixer, music, 0.0f, 0) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_bus_set_gain(mixer, 0, 1.0f, 0) == APRESULT_NOT_FOUND);

    /* Muting a bus silences every voice under it and frees their slots;
     * they keep their place - even KILL ones - and come back at the update
     * after it's unmuted. */
    CHECK(apaudio_bus_set_muted(mixer, music, true, 0) == APRESULT_OK);
    render(mixer, 16);
    CHECK(close_to(out[2 * 15], CENTRE)); /* faded across the render: only the master's voice is left */
    render(mixer, 16);
    CHECK(frames_are(0, 15, CENTRE, CENTRE));
    CHECK(apaudio_bus_set_muted(mixer, music, false, 0) == APRESULT_OK);
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    CHECK(frames_are(0, 15, CENTRE, CENTRE)); /* open, but not ranked yet */
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 2.5f * CENTRE, 2.5f * CENTRE));
    /* Pausing works the same way, from the top of the tree down. */
    CHECK(apaudio_bus_set_paused(mixer, master, true, 0) == APRESULT_OK);
    render(mixer, 16);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.0f, 0.0f));
    CHECK(apaudio_bus_set_paused(mixer, master, false, 0) == APRESULT_OK);
    render(mixer, 16);
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 2.5f * CENTRE, 2.5f * CENTRE));

    /* A bus's rate is every voice's under it. */
    CHECK(apaudio_bus_set_rate(mixer, master, 2.0f, 0) == APRESULT_OK);
    render(mixer, 16);
    CHECK(apaudio_bus_set_rate(mixer, master, 1.0f, 0) == APRESULT_OK);
    render(mixer, 16);

    /* The tree: no cycles, the master stays the root, stale IDs are turned
     * away. */
    CHECK(apaudio_bus_set_parent(mixer, music, strings) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_bus_set_parent(mixer, music, music) == APRESULT_INVALID_ARGUMENT);
    CHECK(apaudio_bus_set_parent(mixer, master, music) == APRESULT_INVALID_ARGUMENT);
    APAUDIO_BUS effects = make_bus(mixer, 0, 0.1f);
    CHECK(apaudio_bus_set_parent(mixer, strings, effects) == APRESULT_OK);
    render(mixer, 16);
    render(mixer, 16);
    CHECK(frames_are(0, 15, (1.0f + 0.05f + 1.0f) * CENTRE, (1.0f + 0.05f + 1.0f) * CENTRE));
    APAUDIO_BUS_DESC bus_desc;
    memset(&bus_desc, 0, sizeof(bus_desc));
    bus_desc.struct_size = sizeof(bus_desc);
    bus_desc.gain = 1.0f;
    bus_desc.rate = 1.0f;
    APAUDIO_BUS extra = 99;
    CHECK(apaudio_bus_create(mixer, &bus_desc, &extra, NULL) == APRESULT_LIMIT && extra == 0);

    /* Destroying a bus moves its voices and children to its parent. */
    apaudio_bus_destroy(mixer, effects);
    apaudio_bus_destroy(mixer, effects);
    apaudio_bus_destroy(mixer, master); /* nothing */
    CHECK(apaudio_bus_set_gain(mixer, effects, 1.0f, 0) == APRESULT_NOT_FOUND);
    CHECK(apaudio_bus_set_muted(mixer, effects, true, 0) == APRESULT_NOT_FOUND);
    CHECK(apaudio_bus_set_parent(mixer, effects, 0) == APRESULT_NOT_FOUND);
    CHECK(apaudio_bus_set_parent(mixer, music, effects) == APRESULT_NOT_FOUND);
    bus_desc.parent = effects;
    CHECK(apaudio_bus_create(mixer, &bus_desc, &extra, NULL) == APRESULT_NOT_FOUND);
    render(mixer, 16);
    render(mixer, 16);
    CHECK(frames_are(0, 15, (1.0f + 0.5f + 1.0f) * CENTRE, (1.0f + 0.5f + 1.0f) * CENTRE));
    apaudio_bus_destroy(mixer, strings);
    render(mixer, 16);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 3.0f * CENTRE, 3.0f * CENTRE));
    /* A stale bus on a play is the master. */
    play_on(mixer, sound, 1.0f, strings);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 4.0f * CENTRE, 4.0f * CENTRE));
    apaudio_mixer_destroy(mixer);
}

static void test_modifiers(void)
{
    current_test = "modifiers";
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    desc.max_buses = 2;
    desc.max_modifiers = 2;
    apaudio_mixer mixer = make_mixer(&desc);
    APAUDIO_BUS music = make_bus(mixer, 0, 1.0f);
    APAUDIO_BUS dialogue = make_bus(mixer, 0, 1.0f);
    APAUDIO_SOUND sound = make_ones(mixer, 4000, APAUDIO_VIRTUAL_KILL);
    play_on(mixer, sound, 0.4f, music);

    /* A snapshot scales its buses while it's active, fading each way. */
    APAUDIO_MODIFIER_TARGET target = {music, 0.5f, 1.0f};
    APAUDIO_MODIFIER_DESC modifier_desc;
    memset(&modifier_desc, 0, sizeof(modifier_desc));
    modifier_desc.struct_size = sizeof(modifier_desc);
    modifier_desc.targets = &target;
    modifier_desc.target_count = 1;
    modifier_desc.fade_in_us = 1000; /* 48 frames */
    APAUDIO_MODIFIER snapshot = 0;
    APAUDIO_ERROR error;
    CHECK(apaudio_modifier_create(mixer, &modifier_desc, &snapshot, &error) == APRESULT_OK && snapshot != 0);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.4f * CENTRE, 0.4f * CENTRE)); /* a new modifier is inactive */
    CHECK(apaudio_modifier_set_active(mixer, snapshot, true) == APRESULT_OK);
    render(mixer, 24);
    CHECK(close_to(out[2 * 23], 0.3f * CENTRE)); /* half way in */
    render(mixer, 24);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.2f * CENTRE, 0.2f * CENTRE));
    /* Modifiers multiply, so they never undo each other or the bus's own
     * gain. */
    CHECK(apaudio_bus_set_gain(mixer, music, 0.5f, 0) == APRESULT_OK);
    render(mixer, 16);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.1f * CENTRE, 0.1f * CENTRE));
    CHECK(apaudio_modifier_set_active(mixer, snapshot, false) == APRESULT_OK);
    render(mixer, 16); /* fade_out_us is 0: across this render */
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.2f * CENTRE, 0.2f * CENTRE));
    CHECK(apaudio_bus_set_gain(mixer, music, 1.0f, 0) == APRESULT_OK);
    render(mixer, 16);

    /* Bad targets. */
    APAUDIO_MODIFIER none = 99;
    APAUDIO_MODIFIER_TARGET twice[2] = {{music, 0.5f, 1.0f}, {music, 0.5f, 1.0f}};
    modifier_desc.targets = twice;
    modifier_desc.target_count = 2;
    CHECK(apaudio_modifier_create(mixer, &modifier_desc, &none, &error) == APRESULT_INVALID_ARGUMENT && none == 0);
    twice[1].bus = dialogue;
    twice[1].rate = 0.0f;
    CHECK(apaudio_modifier_create(mixer, &modifier_desc, &none, &error) == APRESULT_INVALID_ARGUMENT);
    twice[1].rate = 1.0f;
    twice[1].bus = 12345;
    CHECK(apaudio_modifier_create(mixer, &modifier_desc, &none, &error) == APRESULT_NOT_FOUND);

    /* Ducking: a triggered modifier is active while its bus has audible
     * voices, as of the last update. */
    APAUDIO_MODIFIER_TARGET duck_target = {music, 0.25f, 1.0f};
    modifier_desc.targets = &duck_target;
    modifier_desc.target_count = 1;
    modifier_desc.fade_in_us = 0;
    modifier_desc.trigger = dialogue;
    APAUDIO_MODIFIER duck = 0;
    CHECK(apaudio_modifier_create(mixer, &modifier_desc, &duck, &error) == APRESULT_OK);
    CHECK(apaudio_modifier_create(mixer, &modifier_desc, &none, &error) == APRESULT_LIMIT);
    APAUDIO_VOICE line = play_on(mixer, sound, 0.5f, dialogue);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.9f * CENTRE, 0.9f * CENTRE));
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.6f * CENTRE, 0.6f * CENTRE)); /* 0.4 x 0.25, and the line */
    apaudio_voice_stop(mixer, line, 0);
    render(mixer, 16);
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.4f * CENTRE, 0.4f * CENTRE));

    /* A destroyed modifier is stale at once, and its effect goes. */
    CHECK(apaudio_modifier_set_active(mixer, snapshot, true) == APRESULT_OK);
    render(mixer, 48);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.2f * CENTRE, 0.2f * CENTRE));
    apaudio_modifier_destroy(mixer, snapshot);
    apaudio_modifier_destroy(mixer, snapshot);
    CHECK(apaudio_modifier_set_active(mixer, snapshot, true) == APRESULT_NOT_FOUND);
    render(mixer, 16);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.4f * CENTRE, 0.4f * CENTRE));
    /* Destroying a bus drops the targets and triggers naming it. */
    apaudio_bus_destroy(mixer, music);
    apaudio_bus_destroy(mixer, dialogue);
    render(mixer, 16);
    apaudio_mixer_update(mixer);
    render(mixer, 16);
    CHECK(frames_are(0, 15, 0.4f * CENTRE, 0.4f * CENTRE));
    apaudio_mixer_destroy(mixer);
}

static void test_start_time(void)
{
    current_test = "start_time";
    APAUDIO_MIXER_DESC desc = plain_mixer_desc();
    apaudio_mixer mixer = make_mixer(&desc);
    APAUDIO_SOUND sound = make_ones(mixer, 4000, APAUDIO_VIRTUAL_KILL);
    SPUDAUDIO_CALLBACK_INFO info;
    memset(&info, 0, sizeof(info));
    info.flags = SPUDAUDIO_CALLBACK_FLAG_HOST_TIME_VALID;
    info.host_time_ns = 5000000000ull;

    /* A voice with a start time begins on the frame matching it. */
    APAUDIO_PLAY_DESC timed = play_desc(1.0f);
    timed.start_time_ns = info.host_time_ns + 208400; /* just past 10 frames */
    APAUDIO_VOICE voice = 0;
    CHECK(apaudio_play(mixer, sound, &timed, &voice) == APRESULT_OK);
    apaudio_mixer_render(mixer, out, 32, &info);
    CHECK(frames_are(0, 9, 0.0f, 0.0f) && frames_are(10, 31, CENTRE, CENTRE));
    apaudio_voice_stop(mixer, voice, 0);

    /* Later than this render: it waits, and can be stopped while it does. */
    timed.start_time_ns = info.host_time_ns + 1000000000ull;
    CHECK(apaudio_play(mixer, sound, &timed, &voice) == APRESULT_OK);
    APAUDIO_VOICE never = 0;
    CHECK(apaudio_play(mixer, sound, &timed, &never) == APRESULT_OK);
    apaudio_mixer_render(mixer, out, 32, &info);
    CHECK(frames_are(0, 31, 0.0f, 0.0f));
    apaudio_voice_stop(mixer, never, 0);
    info.host_time_ns += 999900000ull;
    apaudio_mixer_render(mixer, out, 32, &info);
    CHECK(frames_are(0, 4, 0.0f, 0.0f) && frames_are(5, 31, CENTRE, CENTRE));
    apaudio_voice_stop(mixer, voice, 0);

    /* A time already past, or no host timing: as soon as possible. */
    timed.start_time_ns = 1;
    CHECK(apaudio_play(mixer, sound, &timed, &voice) == APRESULT_OK);
    apaudio_mixer_render(mixer, out, 32, &info);
    CHECK(frames_are(0, 31, CENTRE, CENTRE));
    apaudio_voice_stop(mixer, voice, 0);
    timed.start_time_ns = info.host_time_ns + 1000000000ull;
    CHECK(apaudio_play(mixer, sound, &timed, &voice) == APRESULT_OK);
    apaudio_mixer_stream_callback(NULL, out, 32, NULL, mixer);
    CHECK(frames_are(0, 31, CENTRE, CENTRE));
    apaudio_mixer_destroy(mixer);
}

int main(void)
{
    test_format_from_spudaudio();
    test_null_arguments();
    test_pcm_free();
    test_wav();
    test_mixer_create();
    test_play();
    test_stereo_and_direction();
    test_resampling();
    test_formats();
    test_sounds();
    test_prioritization();
    test_concurrency();
    test_buses();
    test_modifiers();
    test_start_time();
    if (failures)
    {
        printf("%d check(s) failed\n", failures);
        return 1;
    }
    printf("All apaudio tests passed\n");
    return 0;
}
