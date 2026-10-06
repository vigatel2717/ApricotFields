#include "audio/apaudiocues.h"

#include <glaze/glaze.hpp>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// ApAudio's cues (apaudiocues.h): a table of cue names, what the loaded
// banks define for each as the mixer's objects, and the sustained playings.
// Everything about sound - voices, buses, ducking, limits, ranking, fades
// out - is the mixer's, through apaudio.h alone; what's here is only what a
// cue is and what a bank says.
//
// The manifest is read strictly - an unknown or missing key is an error,
// not something skipped - so a typo in a bank fails where its author sees
// it, not as a cue that's quietly wrong.

// The manifest's shape, which glaze reads by reflection. Not in an
// anonymous namespace: glaze needs its types to have linkage.
namespace apaudio_bank_manifest
{
struct manifest_cue_t
{
    std::vector<std::string> sounds;
    std::string bus;
    std::optional<float> gain;
    std::optional<float> pitch_variation;
    std::optional<uint32_t> priority;
    std::optional<uint32_t> max_instances;
    std::optional<uint32_t> retrigger_ms;
    std::optional<bool> loop;
    std::optional<bool> exclusive;
    std::optional<uint32_t> fade_in_ms;
    std::optional<uint32_t> fade_out_ms;
};

struct manifest_bus_t
{
    std::optional<std::map<std::string, float>> duck;
};

struct manifest_t
{
    uint32_t version;
    std::map<std::string, manifest_cue_t> cues;
    std::optional<std::map<std::string, manifest_bus_t>> buses;
};
} // namespace apaudio_bank_manifest

namespace
{
using namespace apaudio_bank_manifest;

constexpr uint32_t MANIFEST_VERSION = 1;
constexpr glz::opts MANIFEST_OPTS{.error_on_unknown_keys = true, .error_on_missing_keys = true};

// What a ZIP bank may hold, besides the table's max_bank_size. These stop a
// hostile or broken archive before anything is allocated for it.
constexpr uint32_t BANK_MAX_ENTRIES = 4096;
constexpr uint64_t BANK_MAX_CENTRAL_DIRECTORY = 1u << 20;
constexpr uint64_t BANK_MAX_ENTRY_SIZE = 256ull << 20;

// The rates an APAUDIO_SOUND can have (apaudio.h, APAUDIO_SOUND_DESC).
constexpr uint32_t SOUND_RATE_MIN = 1000;
constexpr uint32_t SOUND_RATE_MAX = 768000;

// A playback rate the mixer accepts (APAUDIO_PLAY_DESC.rate).
constexpr float RATE_MIN = 0.125f;
constexpr float RATE_MAX = 8.0f;

// What a bank says about how one cue plays (apaudiocues.h, Bank format).
struct cue_settings_t
{
    float gain{1.0f};
    float pitch_variation{0.0f};
    uint32_t bus{0}; // its index in the table's buses
    uint8_t priority{128};
    uint32_t max_instances{0}; // 0: no limit of its own
    uint32_t retrigger_ms{0};
    bool loop{false};
    bool exclusive{false};
    uint32_t fade_in_ms{0};
    uint32_t fade_out_ms{0};
};

// One of a bank's sound files, decoded: mono or stereo. Held only until the
// bank is applied - the mixer copies it.
struct bank_sound_t
{
    std::string name; // its file in the bank
    APAUDIO_PCM pcm{};

    bank_sound_t() = default;
    bank_sound_t(bank_sound_t &&other) noexcept : name(std::move(other.name)), pcm(other.pcm)
    {
        other.pcm = APAUDIO_PCM{};
    }
    bank_sound_t(const bank_sound_t &) = delete;
    bank_sound_t &operator=(const bank_sound_t &) = delete;
    ~bank_sound_t()
    {
        apaudio_pcm_free(&pcm);
    }
};

struct bank_cue_t
{
    std::string name;
    cue_settings_t settings;
    // Indices into the bank's sounds: one picked at random each play. Empty:
    // defined, but plays nothing.
    std::vector<uint32_t> sounds;
};

// Everything one bank defines, read whole before any of it is applied, so a
// malformed bank loads nothing.
struct bank_t
{
    // Each file once, however many cues name it.
    std::vector<bank_sound_t> sounds;
    std::vector<bank_cue_t> cues;
    // The buses the bank has settings for, with what each ducks every bus of
    // the table to: linear, 1 for not ducked.
    std::vector<std::pair<uint32_t, std::vector<float>>> buses;
};

// Reads a bank's file named [name] whole into [out_data], or fails with why
// in [out_error].
typedef std::function<APRESULT(const std::string &name, std::vector<unsigned char> &out_data, std::string &out_error)>
    file_reader_t;

// An APAUDIO_SOUND or APAUDIO_CONCURRENCY the loaded cues hold, destroyed
// with the last cue holding it. Only the cue table holds these, and only
// the control thread changes it (banks, and destruction), so that's the
// thread they're destroyed on, as the mixer requires.
struct sound_ref_t
{
    apaudio_mixer mixer;
    APAUDIO_SOUND sound;
    sound_ref_t(apaudio_mixer mixer, APAUDIO_SOUND sound) : mixer(mixer), sound(sound)
    {
    }
    sound_ref_t(const sound_ref_t &) = delete;
    sound_ref_t &operator=(const sound_ref_t &) = delete;
    ~sound_ref_t()
    {
        apaudio_sound_destroy(mixer, sound);
    }
};

struct group_ref_t
{
    apaudio_mixer mixer;
    APAUDIO_CONCURRENCY group;
    group_ref_t(apaudio_mixer mixer, APAUDIO_CONCURRENCY group) : mixer(mixer), group(group)
    {
    }
    group_ref_t(const group_ref_t &) = delete;
    group_ref_t &operator=(const group_ref_t &) = delete;
    ~group_ref_t()
    {
        apaudio_concurrency_destroy(mixer, group);
    }
};

// A cue as loaded: what the latest bank to define it says, as the mixer's
// objects.
struct cue_t
{
    // False for a cue no loaded bank defines: playing it plays nothing.
    bool defined{false};
    cue_settings_t settings;
    // One picked at random each play. Empty: defined, but plays nothing.
    std::vector<std::shared_ptr<sound_ref_t>> sounds;
    // Its max_instances and retrigger_ms, if it has either.
    std::shared_ptr<group_ref_t> group;
};

struct bus_t
{
    std::string name;
    APAUDIO_BUS bus{0};
    APAUDIO_VIRTUAL_BEHAVIOR virtual_behavior{APAUDIO_VIRTUAL_UNSPECIFIED};
    // Its ducking of the others, from the banks: a modifier it triggers, or
    // 0. The control thread's.
    APAUDIO_MODIFIER ducking{0};
};

// A sustained playing: one apaudio_cues_stop() and exclusive crossfades can
// still end. It's named by its APAUDIO_VOICE, which the mixer never reuses,
// so an old one never stops a newer playing. A free slot's voice is 0.
struct playing_t
{
    APAUDIO_VOICE voice{0};
    uint32_t bus{0};
    bool exclusive{false};
    uint32_t fade_out_ms{0};
};

void set_error(APAUDIO_ERROR *error, const std::string &message)
{
    if (error)
        snprintf(error->message, sizeof(error->message), "%s", message.c_str());
}

bool gain_valid(float gain)
{
    return std::isfinite(gain) && gain >= 0.0f;
}
} // namespace

struct apaudio_cues_t
{
    apaudio_mixer mixer{nullptr};
    // Fixed once created, but for each bus's ducking.
    std::vector<bus_t> buses;
    uint32_t max_cues{0};
    uint32_t max_voices{0};
    uint64_t max_bank_size{0};
    uint32_t duck_fade_in_us{0};
    uint32_t duck_fade_out_us{0};
    std::atomic<uint32_t> sample_rate{0};
    // A play on a muted bus plays nothing rather than waiting for the
    // unmute (apaudio_cues_set_bus_muted()). One per bus.
    std::unique_ptr<std::atomic<bool>[]> bus_muted;

    // Cue names and what the loaded banks define for each. Names are only
    // ever added, so a cue handle - its index + 1 - never goes stale. The
    // mutex is held only for short lookups and copies, never while the
    // mixer is called, so no call waits on sound.
    std::mutex cue_mutex;
    std::unordered_map<std::string, uint32_t> cue_indices;
    std::vector<std::string> cue_names;
    std::vector<cue_t> cues;
    std::unique_ptr<std::atomic<bool>[]> cue_enabled; // max_cues of them
    std::atomic<uint32_t> cue_count{0};
    // Picks variations and pitch offsets.
    std::atomic<uint32_t> random_state{0};

    std::mutex playing_mutex;
    std::vector<playing_t> playings; // max_playings of them
};

namespace
{
bool bus_from_name(const apaudio_cues_t *cues, const std::string &name, uint32_t &out_bus)
{
    for (uint32_t bus = 0; bus < cues->buses.size(); bus++)
        if (cues->buses[bus].name == name)
        {
            out_bus = bus;
            return true;
        }
    return false;
}

// ---- Reading a bank -----------------------------------------------------------

// Decodes the WAV named [name] into [bank], once per bank however many cues
// name it. [out_sound] is its index in the bank's sounds.
APRESULT read_sound(const file_reader_t &read_file, const std::string &name,
                    std::unordered_map<std::string, uint32_t> &sounds, bank_t &bank, uint32_t &out_sound,
                    std::string &out_error)
{
    auto found = sounds.find(name);
    if (found != sounds.end())
    {
        out_sound = found->second;
        return APRESULT_OK;
    }
    std::vector<unsigned char> file;
    APRESULT result = read_file(name, file, out_error);
    if (result != APRESULT_OK)
        return result;

    bank_sound_t sound;
    sound.name = name;
    APAUDIO_ERROR error{};
    APRESULT decoded = apaudio_wav_decode(file.data(), file.size(), &sound.pcm, &error);
    if (decoded == APRESULT_OUT_OF_MEMORY)
        return APRESULT_OUT_OF_MEMORY;
    if (decoded != APRESULT_OK)
    {
        out_error = name + ": " + error.message;
        return APRESULT_CORRUPT_DATA;
    }
    // The mixer plays mono (which can come from a direction) and stereo.
    if (sound.pcm.channel_count != 1 && sound.pcm.channel_count != 2)
    {
        out_error = name + ": " + std::to_string(sound.pcm.channel_count) + " channels; a sound is mono or stereo";
        return APRESULT_CORRUPT_DATA;
    }
    if (sound.pcm.sample_rate < SOUND_RATE_MIN || sound.pcm.sample_rate > SOUND_RATE_MAX)
    {
        out_error = name + ": " + std::to_string(sound.pcm.sample_rate) + " Hz; a sound is " +
                    std::to_string(SOUND_RATE_MIN) + " to " + std::to_string(SOUND_RATE_MAX) + " Hz";
        return APRESULT_CORRUPT_DATA;
    }
    out_sound = (uint32_t)bank.sounds.size();
    bank.sounds.push_back(std::move(sound));
    sounds.emplace(name, out_sound);
    return APRESULT_OK;
}

APRESULT read_cue(const apaudio_cues_t *cues, const file_reader_t &read_file, const std::string &name,
                  const manifest_cue_t &cue, std::unordered_map<std::string, uint32_t> &sounds, bank_t &bank,
                  std::string &out_error)
{
    auto fail = [&](const std::string &why) {
        out_error = "cue " + name + ": " + why;
        return APRESULT_CORRUPT_DATA;
    };
    if (name.empty() || name.size() >= APAUDIO_CUE_NAME_SIZE)
        return fail("a name is 1 to " + std::to_string(APAUDIO_CUE_NAME_SIZE - 1) + " bytes");

    bank_cue_t definition;
    definition.name = name;
    cue_settings_t &settings = definition.settings;
    if (!bus_from_name(cues, cue.bus, settings.bus))
        return fail("no bus \"" + cue.bus + "\"");
    if (cue.gain && !gain_valid(*cue.gain))
        return fail("gain is finite and not negative");
    if (cue.pitch_variation && !(*cue.pitch_variation >= 0.0f && *cue.pitch_variation < 1.0f))
        return fail("pitch_variation is from 0 to under 1");
    if (cue.priority && *cue.priority > 255)
        return fail("priority is 0 to 255");
    settings.gain = cue.gain.value_or(settings.gain);
    settings.pitch_variation = cue.pitch_variation.value_or(settings.pitch_variation);
    settings.priority = (uint8_t)cue.priority.value_or(settings.priority);
    settings.max_instances = cue.max_instances.value_or(settings.max_instances);
    settings.retrigger_ms = cue.retrigger_ms.value_or(settings.retrigger_ms);
    settings.loop = cue.loop.value_or(settings.loop);
    settings.exclusive = cue.exclusive.value_or(settings.exclusive);
    settings.fade_in_ms = cue.fade_in_ms.value_or(settings.fade_in_ms);
    settings.fade_out_ms = cue.fade_out_ms.value_or(settings.fade_out_ms);

    for (const std::string &file : cue.sounds)
    {
        uint32_t sound;
        APRESULT result = read_sound(read_file, file, sounds, bank, sound, out_error);
        if (result != APRESULT_OK)
        {
            if (result == APRESULT_CORRUPT_DATA)
                out_error = "cue " + name + ": " + out_error;
            return result;
        }
        definition.sounds.push_back(sound);
    }
    bank.cues.push_back(std::move(definition));
    return APRESULT_OK;
}

APRESULT read_bank(const apaudio_cues_t *cues, const file_reader_t &read_file, bank_t &out_bank,
                   std::string &out_error)
{
    std::vector<unsigned char> manifest_file;
    APRESULT result = read_file("manifest.json", manifest_file, out_error);
    if (result != APRESULT_OK)
        return result;
    std::string manifest_json(manifest_file.begin(), manifest_file.end());
    manifest_t manifest{};
    if (auto parsed = glz::read<MANIFEST_OPTS>(manifest, manifest_json))
    {
        out_error = "manifest.json: " + glz::format_error(parsed, manifest_json);
        return APRESULT_CORRUPT_DATA;
    }
    if (manifest.version != MANIFEST_VERSION)
    {
        out_error = "manifest.json: version " + std::to_string(manifest.version) + ", not " +
                    std::to_string(MANIFEST_VERSION);
        return APRESULT_CORRUPT_DATA;
    }

    bank_t bank;
    std::unordered_map<std::string, uint32_t> sounds;
    for (const auto &[name, cue] : manifest.cues)
    {
        result = read_cue(cues, read_file, name, cue, sounds, bank, out_error);
        if (result != APRESULT_OK)
            return result;
    }
    if (manifest.buses)
        for (const auto &[name, settings] : *manifest.buses)
        {
            uint32_t bus;
            if (!bus_from_name(cues, name, bus))
            {
                out_error = "manifest.json: no bus \"" + name + "\"";
                return APRESULT_CORRUPT_DATA;
            }
            std::vector<float> duck(cues->buses.size(), 1.0f);
            if (settings.duck)
                for (const auto &[ducked_name, gain] : *settings.duck)
                {
                    uint32_t ducked;
                    if (!bus_from_name(cues, ducked_name, ducked) || ducked == bus || !gain_valid(gain) ||
                        gain > 1.0f)
                    {
                        out_error = "bus " + name + ": duck names another bus, with a gain from 0 to 1";
                        return APRESULT_CORRUPT_DATA;
                    }
                    duck[ducked] = gain;
                }
            bank.buses.emplace_back(bus, std::move(duck));
        }
    out_bank = std::move(bank);
    return APRESULT_OK;
}

// ---- Applying a bank ----------------------------------------------------------

// Applies [bank] over what's loaded, whole: its sounds become the mixer's,
// its cues replace the same names, its buses' ducking replaces theirs.
// APRESULT_LIMIT, applying nothing, if its new names don't fit in max_cues
// or the mixer has no room for its sounds.
APRESULT apply_bank(apaudio_cues_t *cues, const bank_t &bank, std::string &out_error)
{
    apaudio_mixer mixer = cues->mixer;
    // The mixer's objects first, so a bank it has no room for applies
    // nothing: until they're swapped in below, leaving destroys them.
    APAUDIO_ERROR error{};
    std::vector<std::shared_ptr<sound_ref_t>> sounds;
    sounds.reserve(bank.sounds.size());
    for (const bank_sound_t &file : bank.sounds)
    {
        APAUDIO_SOUND_DESC desc{};
        desc.struct_size = sizeof(desc);
        desc.samples = file.pcm.samples;
        desc.frame_count = file.pcm.frame_count;
        desc.channel_count = file.pcm.channel_count;
        desc.sample_rate = file.pcm.sample_rate;
        // Each play says its own: a file can be several cues', on different
        // buses.
        desc.virtual_behavior = APAUDIO_VIRTUAL_KILL;
        APAUDIO_SOUND sound = 0;
        APRESULT created = apaudio_sound_create(mixer, &desc, &sound, &error);
        if (created != APRESULT_OK)
        {
            out_error = "sound " + file.name + ": " + error.message;
            return created;
        }
        sounds.push_back(std::make_shared<sound_ref_t>(mixer, sound));
    }

    std::vector<std::pair<const std::string *, cue_t>> loaded;
    loaded.reserve(bank.cues.size());
    for (const bank_cue_t &definition : bank.cues)
    {
        cue_t cue;
        cue.defined = true;
        cue.settings = definition.settings;
        for (uint32_t sound : definition.sounds)
        {
            if (sound >= sounds.size())
                return APRESULT_CORRUPT_DATA;
            cue.sounds.push_back(sounds[sound]);
        }
        if (cue.settings.max_instances || cue.settings.retrigger_ms)
        {
            APAUDIO_CONCURRENCY_DESC desc{};
            desc.struct_size = sizeof(desc);
            desc.max_instances = cue.settings.max_instances ? cue.settings.max_instances : cues->max_voices;
            desc.resolution = APAUDIO_RESOLUTION_STOP_OLDEST;
            desc.retrigger_us = cue.settings.retrigger_ms > UINT32_MAX / 1000 ? UINT32_MAX
                                                                              : cue.settings.retrigger_ms * 1000;
            desc.older_instance_gain = 1.0f;
            APAUDIO_CONCURRENCY group = 0;
            APRESULT created = apaudio_concurrency_create(mixer, &desc, &group, &error);
            if (created != APRESULT_OK)
            {
                out_error = "cue " + definition.name + ": " + error.message;
                return created;
            }
            cue.group = std::make_shared<group_ref_t>(mixer, group);
        }
        loaded.emplace_back(&definition.name, std::move(cue));
    }

    // Each bus's ducking: a modifier that bus triggers, or none if it ducks
    // nothing.
    std::vector<std::pair<uint32_t, APAUDIO_MODIFIER>> ducking;
    auto drop_ducking = [&]() {
        for (const auto &[bus, modifier] : ducking)
            apaudio_modifier_destroy(mixer, modifier);
    };
    for (const auto &[bus, duck] : bank.buses)
    {
        std::vector<APAUDIO_MODIFIER_TARGET> targets;
        for (uint32_t ducked = 0; ducked < cues->buses.size(); ducked++)
            if (ducked != bus && duck[ducked] < 1.0f)
                targets.push_back({cues->buses[ducked].bus, duck[ducked], 1.0f});
        APAUDIO_MODIFIER modifier = 0;
        if (!targets.empty())
        {
            APAUDIO_MODIFIER_DESC desc{};
            desc.struct_size = sizeof(desc);
            desc.targets = targets.data();
            desc.target_count = (uint32_t)targets.size();
            desc.fade_in_us = cues->duck_fade_in_us;
            desc.fade_out_us = cues->duck_fade_out_us;
            desc.trigger = cues->buses[bus].bus;
            APRESULT created = apaudio_modifier_create(mixer, &desc, &modifier, &error);
            if (created != APRESULT_OK)
            {
                out_error = "bus " + cues->buses[bus].name + ": ducking: " + error.message;
                drop_ducking();
                return created;
            }
        }
        ducking.emplace_back(bus, modifier);
    }

    // The cues under one hold of cue_mutex, so a play sees either all of the
    // bank or none of it. The cues it replaces are left in [loaded], and so
    // let go of their sounds and groups after the lock, on this thread.
    bool fits = true;
    {
        std::lock_guard<std::mutex> lock(cues->cue_mutex);
        size_t new_names = 0;
        for (const auto &cue : loaded)
            if (cues->cue_indices.find(*cue.first) == cues->cue_indices.end())
                new_names++;
        fits = cues->cue_names.size() + new_names <= cues->max_cues;
        if (fits)
            for (auto &[name, cue] : loaded)
            {
                auto found = cues->cue_indices.find(*name);
                if (found != cues->cue_indices.end())
                {
                    std::swap(cues->cues[found->second], cue);
                    continue;
                }
                uint32_t index = (uint32_t)cues->cue_names.size();
                cues->cue_names.push_back(*name);
                cues->cues.push_back(std::move(cue));
                cues->cue_indices.emplace(*name, index);
                cues->cue_count.store(index + 1, std::memory_order_release);
            }
    }
    if (!fits)
    {
        drop_ducking();
        out_error = "more than " + std::to_string(cues->max_cues) + " cue names";
        return APRESULT_LIMIT;
    }
    for (const auto &[bus, modifier] : ducking)
    {
        apaudio_modifier_destroy(mixer, cues->buses[bus].ducking);
        cues->buses[bus].ducking = modifier;
    }
    return APRESULT_OK;
}

// Reads the bank behind [read_file] and applies it. No exception leaves.
APRESULT load_bank(apaudio_cues_t *cues, const file_reader_t &read_file, APAUDIO_ERROR *error)
{
    try
    {
        bank_t bank;
        std::string why;
        APRESULT result = read_bank(cues, read_file, bank, why);
        if (result == APRESULT_OK)
            result = apply_bank(cues, bank, why);
        if (result != APRESULT_OK)
            set_error(error, why);
        return result;
    }
    catch (const std::bad_alloc &)
    {
        set_error(error, "out of memory");
        return APRESULT_OUT_OF_MEMORY;
    }
    catch (...)
    {
        set_error(error, "the bank couldn't be read");
        return APRESULT_CORRUPT_DATA;
    }
}

// ---- ZIP banks ----------------------------------------------------------------

// An open ZIP and its codecs, closed together.
struct archive_t
{
    aparchive_codecs codecs{nullptr};
    aparchive_reader reader{nullptr};
    ~archive_t()
    {
        if (reader)
            aparchive_reader_destroy(reader);
        if (codecs)
            aparchive_codecs_destroy(codecs);
    }
};

// Reads the entry named [name] whole into [out_data].
APRESULT read_entry(aparchive_reader reader, const std::string &name, std::vector<unsigned char> &out_data,
                    std::string &out_error)
{
    uint32_t index;
    if (aparchive_reader_find(reader, name.c_str(), &index) != APRESULT_OK)
    {
        out_error = "no " + name + " in the bank";
        return APRESULT_CORRUPT_DATA;
    }
    APARCHIVE_ENTRY_INFO info{};
    info.struct_size = sizeof(info);
    if (aparchive_reader_get_entry(reader, index, &info) != APRESULT_OK || info.is_directory)
    {
        out_error = name + " isn't a file";
        return APRESULT_CORRUPT_DATA;
    }
    out_data.resize(info.uncompressed_size);
    APARCHIVE_ERROR error{};
    if (aparchive_reader_read_entry(reader, index, out_data.data(), out_data.size(), &error) != APRESULT_OK)
    {
        out_error = name + ": " + error.message;
        return APRESULT_CORRUPT_DATA;
    }
    return APRESULT_OK;
}

// ---- Playing ------------------------------------------------------------------

uint32_t next_random(apaudio_cues_t *cues)
{
    // A counter through a bit mixer: any thread, no lock, and good enough to
    // pick a variation.
    uint32_t x = cues->random_state.fetch_add(0x9E3779B9u, std::memory_order_relaxed) + 0x9E3779B9u;
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

// A cue handle's index, if it names a known cue.
bool cue_index(apaudio_cues_t *cues, APAUDIO_CUE cue, uint32_t &out_index)
{
    if (cue == 0 || cue > cues->cue_count.load(std::memory_order_acquire))
        return false;
    out_index = (uint32_t)(cue - 1);
    return true;
}

// What a play of a cue would play: which of its sounds, and how. [plays]
// is false when it plays nothing (off, undefined, muted) - which isn't a
// failure.
struct play_plan_t
{
    bool plays{false};
    cue_settings_t settings;
    APAUDIO_SOUND sound{0};
    APAUDIO_CONCURRENCY group{0};
    float gain{1.0f};
    float rate{1.0f};
    bool has_direction{false};
    APAUDIO_DIRECTION direction{0.0, 0.0};
    uint64_t start_time_ns{0};
};

APRESULT plan_play(apaudio_cues_t *cues, APAUDIO_CUE cue, const APAUDIO_CUE_PLAY_DESC *desc, play_plan_t &plan)
{
    if (!cues)
        return APRESULT_INVALID_ARGUMENT;
    uint32_t index;
    if (!cue_index(cues, cue, index))
        return APRESULT_NOT_FOUND;
    float gain = 1.0f;
    if (desc)
    {
        if (desc->struct_size < sizeof(APAUDIO_CUE_PLAY_DESC) || !gain_valid(desc->gain))
            return APRESULT_INVALID_ARGUMENT;
        gain = desc->gain;
        if (desc->direction)
        {
            if (!std::isfinite(desc->direction->azimuth) || !std::isfinite(desc->direction->elevation))
                return APRESULT_INVALID_ARGUMENT;
            plan.has_direction = true;
            plan.direction = *desc->direction;
        }
        plan.start_time_ns = desc->start_time_ns;
    }

    if (!cues->cue_enabled[index].load())
        return APRESULT_OK;
    uint32_t pick = next_random(cues);
    {
        // Only IDs leave the lock, never the cue's hold on its sounds: a
        // bank replacing the cue meanwhile makes them stale, and the mixer
        // turns a stale sound away.
        std::lock_guard<std::mutex> lock(cues->cue_mutex);
        const cue_t &definition = cues->cues[index];
        if (!definition.defined || definition.sounds.empty())
            return APRESULT_OK;
        plan.settings = definition.settings;
        plan.sound = definition.sounds[pick % definition.sounds.size()]->sound;
        plan.group = definition.group ? definition.group->group : 0;
    }
    // A muted bus plays nothing new; what's already playing there is the
    // mixer's to silence.
    if (cues->bus_muted[plan.settings.bus].load())
        return APRESULT_OK;

    plan.gain = gain * plan.settings.gain;
    if (plan.settings.pitch_variation > 0.0f)
    {
        // Evenly within the variation either side of the sound's own pitch.
        float offset = (float)(next_random(cues) >> 8) / (float)(1u << 24) * 2.0f - 1.0f;
        float rate = 1.0f + offset * plan.settings.pitch_variation;
        plan.rate = rate < RATE_MIN ? RATE_MIN : rate > RATE_MAX ? RATE_MAX : rate;
    }
    plan.plays = true;
    return APRESULT_OK;
}

// Plays [plan] on the mixer. [out_voice] is 0 if nothing was queued: the
// cue's sound was replaced meanwhile.
APRESULT play_plan(apaudio_cues_t *cues, const play_plan_t &plan, APAUDIO_VOICE &out_voice)
{
    out_voice = 0;
    const bus_t &bus = cues->buses[plan.settings.bus];
    APAUDIO_PLAY_DESC desc{};
    desc.struct_size = sizeof(desc);
    desc.gain = plan.gain;
    desc.priority = plan.settings.priority;
    desc.virtual_behavior = bus.virtual_behavior;
    desc.concurrency = plan.group;
    desc.bus = bus.bus;
    desc.rate = plan.rate;
    desc.direction = plan.has_direction ? &plan.direction : nullptr;
    desc.start_time_ns = plan.start_time_ns;
    desc.loop = plan.settings.loop;
    APRESULT played = apaudio_play(cues->mixer, plan.sound, &desc, &out_voice);
    if (played == APRESULT_LIMIT)
        return APRESULT_LIMIT;
    if (played != APRESULT_OK)
        out_voice = 0;
    return APRESULT_OK;
}

uint32_t frames_from_ms(apaudio_cues_t *cues, uint32_t ms)
{
    uint64_t frames = (uint64_t)ms * cues->sample_rate.load() / 1000;
    return frames > UINT32_MAX ? UINT32_MAX : (uint32_t)frames;
}

// Ends [playing] (playing_mutex held), over its cue's fade-out if [fade].
void end_playing(apaudio_cues_t *cues, playing_t &playing, bool fade)
{
    apaudio_voice_stop(cues->mixer, playing.voice, fade ? frames_from_ms(cues, playing.fade_out_ms) : 0);
    playing.voice = 0;
}

// The live record [voice] names (playing_mutex held), or null if it has
// ended or never named one.
playing_t *find_playing(apaudio_cues_t *cues, APAUDIO_VOICE voice)
{
    if (!voice)
        return nullptr;
    for (playing_t &record : cues->playings)
        if (record.voice == voice)
            return &record;
    return nullptr;
}
} // namespace

// ---- The C API ----------------------------------------------------------------

extern "C"
{
    APRESULT apaudio_cues_create(const APAUDIO_CUES_DESC *desc, apaudio_cues *out_cues, APAUDIO_ERROR *error)
    {
        if (out_cues)
            *out_cues = nullptr;
        if (error)
            error->message[0] = '\0';
        if (!desc || !out_cues || desc->struct_size < sizeof(APAUDIO_CUES_DESC))
            return APRESULT_INVALID_ARGUMENT;
        if (!desc->mixer || !desc->buses || !desc->bus_count || !desc->max_cues || !desc->max_playings ||
            !desc->max_voices || !desc->sample_rate)
        {
            set_error(error, "a cue table needs a mixer, buses, and sizes that aren't 0");
            return APRESULT_INVALID_ARGUMENT;
        }
        try
        {
            std::unique_ptr<apaudio_cues_t> cues = std::make_unique<apaudio_cues_t>();
            cues->mixer = desc->mixer;
            for (uint32_t i = 0; i < desc->bus_count; i++)
            {
                const APAUDIO_CUES_BUS &bus = desc->buses[i];
                uint32_t same;
                if (!bus.name || !bus.name[0] || !bus.bus || bus.virtual_behavior == APAUDIO_VIRTUAL_UNSPECIFIED ||
                    bus.virtual_behavior > APAUDIO_VIRTUAL_PAUSE || bus_from_name(cues.get(), bus.name, same))
                {
                    set_error(error, "bus " + std::to_string(i) +
                                         ": a bus has a name of its own, a mixer bus and a virtual behavior");
                    return APRESULT_INVALID_ARGUMENT;
                }
                bus_t added;
                added.name = bus.name;
                added.bus = bus.bus;
                added.virtual_behavior = bus.virtual_behavior;
                cues->buses.push_back(std::move(added));
            }
            cues->max_cues = desc->max_cues;
            cues->max_voices = desc->max_voices;
            cues->max_bank_size = desc->max_bank_size;
            cues->duck_fade_in_us = desc->duck_fade_in_us;
            cues->duck_fade_out_us = desc->duck_fade_out_us;
            cues->sample_rate.store(desc->sample_rate);
            cues->bus_muted = std::make_unique<std::atomic<bool>[]>(desc->bus_count);
            for (uint32_t i = 0; i < desc->bus_count; i++)
                cues->bus_muted[i].store(false);
            cues->cue_enabled = std::make_unique<std::atomic<bool>[]>(desc->max_cues);
            for (uint32_t i = 0; i < desc->max_cues; i++)
                cues->cue_enabled[i].store(true);
            // Reserved whole, so a name added never moves the ones before it.
            cues->cue_names.reserve(desc->max_cues);
            cues->cues.reserve(desc->max_cues);
            cues->playings.resize(desc->max_playings);
            *out_cues = cues.release();
            return APRESULT_OK;
        }
        catch (const std::bad_alloc &)
        {
            return APRESULT_OUT_OF_MEMORY;
        }
    }

    void apaudio_cues_destroy(apaudio_cues cues)
    {
        if (!cues)
            return;
        // The cues' sounds and groups go with them, on the mixer.
        cues->cues.clear();
        for (bus_t &bus : cues->buses)
            apaudio_modifier_destroy(cues->mixer, bus.ducking);
        delete cues;
    }

    void apaudio_cues_set_sample_rate(apaudio_cues cues, uint32_t sample_rate)
    {
        if (cues && sample_rate)
            cues->sample_rate.store(sample_rate);
    }

    void apaudio_cues_update(apaudio_cues cues)
    {
        if (!cues)
            return;
        std::lock_guard<std::mutex> lock(cues->playing_mutex);
        for (playing_t &playing : cues->playings)
            if (playing.voice && !apaudio_voice_is_live(cues->mixer, playing.voice))
                playing.voice = 0;
    }

    APRESULT apaudio_cues_load_bank(apaudio_cues cues, const APAUDIO_BANK_SOURCE *source, APAUDIO_ERROR *error)
    {
        if (error)
            error->message[0] = '\0';
        if (!cues || !source || !source->get_size || !source->read)
            return APRESULT_INVALID_ARGUMENT;
        uint64_t total = 0;
        return load_bank(
            cues,
            [&](const std::string &name, std::vector<unsigned char> &out_data, std::string &out_error) -> APRESULT {
                uint64_t size = 0;
                if (!source->get_size(source->user, name.c_str(), &size))
                {
                    out_error = "no " + name + " in the bank";
                    return APRESULT_CORRUPT_DATA;
                }
                if (total > cues->max_bank_size || size > cues->max_bank_size - total)
                {
                    out_error = "over " + std::to_string(cues->max_bank_size) + " bytes";
                    return APRESULT_CORRUPT_DATA;
                }
                out_data.resize((size_t)size);
                if (!source->read(source->user, name.c_str(), out_data.data(), size))
                {
                    out_error = name + ": couldn't be read";
                    return APRESULT_IO;
                }
                total += size;
                return APRESULT_OK;
            },
            error);
    }

    APRESULT apaudio_cues_load_zip_bank(apaudio_cues cues, const APARCHIVE_SOURCE *source, APAUDIO_ERROR *error)
    {
        if (error)
            error->message[0] = '\0';
        if (!cues || !source)
            return APRESULT_INVALID_ARGUMENT;
        archive_t archive;
        if (aparchive_codecs_create(&archive.codecs) != APRESULT_OK ||
            aparchive_codecs_add_deflate(archive.codecs) != APRESULT_OK)
            return APRESULT_OUT_OF_MEMORY;
        APARCHIVE_READER_DESC desc{};
        desc.struct_size = sizeof(desc);
        desc.source = *source;
        desc.codecs = archive.codecs;
        desc.strict = APARCHIVE_STRICT_NONE;
        desc.max_entry_count = BANK_MAX_ENTRIES;
        desc.max_central_directory_size = BANK_MAX_CENTRAL_DIRECTORY;
        desc.max_entry_uncompressed_size = BANK_MAX_ENTRY_SIZE;
        desc.max_total_uncompressed_size = cues->max_bank_size;
        // The size limits bound what a bank costs; silence in a WAV compresses
        // as far as DEFLATE goes, so a ratio limit would only refuse real banks.
        desc.max_compression_ratio = UINT32_MAX;
        desc.build_name_index = true;
        APARCHIVE_ERROR opening{};
        APRESULT opened = aparchive_reader_open(&desc, &archive.reader, &opening);
        if (opened == APRESULT_OUT_OF_MEMORY)
            return APRESULT_OUT_OF_MEMORY;
        if (opened == APRESULT_NOT_IMPLEMENTED)
        {
            if (error)
                snprintf(error->message, sizeof(error->message),
                         "ZIP banks can't be read until ApArchive's reader exists: %s", opening.message);
            return APRESULT_NOT_IMPLEMENTED;
        }
        if (opened != APRESULT_OK)
        {
            if (error)
                snprintf(error->message, sizeof(error->message), "not a bank: %s", opening.message);
            return APRESULT_CORRUPT_DATA;
        }
        return load_bank(
            cues,
            [&](const std::string &name, std::vector<unsigned char> &out_data, std::string &out_error) {
                return read_entry(archive.reader, name, out_data, out_error);
            },
            error);
    }

    APRESULT apaudio_cues_find(apaudio_cues cues, const char *name, APAUDIO_CUE *out_cue)
    {
        if (out_cue)
            *out_cue = 0;
        if (!cues || !name || !out_cue)
            return APRESULT_INVALID_ARGUMENT;
        size_t length = strnlen(name, APAUDIO_CUE_NAME_SIZE);
        if (length == 0 || length >= APAUDIO_CUE_NAME_SIZE)
            return APRESULT_INVALID_ARGUMENT;
        try
        {
            std::string key(name, length);
            std::lock_guard<std::mutex> lock(cues->cue_mutex);
            auto found = cues->cue_indices.find(key);
            if (found != cues->cue_indices.end())
            {
                *out_cue = (uint64_t)found->second + 1;
                return APRESULT_OK;
            }
            if (cues->cue_names.size() >= cues->max_cues)
                return APRESULT_LIMIT;
            uint32_t index = (uint32_t)cues->cue_names.size();
            cues->cue_names.push_back(key);
            cues->cues.emplace_back();
            cues->cue_indices.emplace(key, index);
            // Published last: a handle is only valid once its cue exists.
            cues->cue_count.store(index + 1, std::memory_order_release);
            *out_cue = (uint64_t)index + 1;
            return APRESULT_OK;
        }
        catch (const std::bad_alloc &)
        {
            return APRESULT_OUT_OF_MEMORY;
        }
    }

    APRESULT apaudio_cues_set_enabled(apaudio_cues cues, APAUDIO_CUE cue, bool enabled)
    {
        if (!cues)
            return APRESULT_INVALID_ARGUMENT;
        uint32_t index;
        if (!cue_index(cues, cue, index))
            return APRESULT_NOT_FOUND;
        cues->cue_enabled[index].store(enabled ? true : false);
        return APRESULT_OK;
    }

    APRESULT apaudio_cues_set_bus_muted(apaudio_cues cues, uint32_t bus_index, bool muted)
    {
        if (!cues || bus_index >= cues->buses.size())
            return APRESULT_INVALID_ARGUMENT;
        cues->bus_muted[bus_index].store(muted ? true : false);
        return APRESULT_OK;
    }

    APAUDIO_MODIFIER apaudio_cues_get_bus_ducking(apaudio_cues cues, uint32_t bus_index)
    {
        if (!cues || bus_index >= cues->buses.size())
            return 0;
        return cues->buses[bus_index].ducking;
    }

    APRESULT apaudio_cues_get_info(apaudio_cues cues, APAUDIO_CUE cue, APAUDIO_CUE_INFO *out_info)
    {
        if (!cues || !out_info || out_info->struct_size < sizeof(uint32_t))
            return APRESULT_INVALID_ARGUMENT;
        uint32_t index;
        if (!cue_index(cues, cue, index))
            return APRESULT_NOT_FOUND;
        APAUDIO_CUE_INFO info{};
        info.struct_size = out_info->struct_size;
        {
            std::lock_guard<std::mutex> lock(cues->cue_mutex);
            const cue_t &definition = cues->cues[index];
            const cue_settings_t &settings = definition.settings;
            info.defined = definition.defined;
            info.loop = settings.loop;
            info.exclusive = settings.exclusive;
            info.priority = settings.priority;
            info.sound_count = (uint32_t)definition.sounds.size();
            info.bus_index = settings.bus;
            info.gain = settings.gain;
            info.pitch_variation = settings.pitch_variation;
            info.max_instances = settings.max_instances;
            info.retrigger_ms = settings.retrigger_ms;
            info.fade_in_ms = settings.fade_in_ms;
            info.fade_out_ms = settings.fade_out_ms;
        }
        size_t size = out_info->struct_size < sizeof(info) ? out_info->struct_size : sizeof(info);
        memcpy(out_info, &info, size);
        return APRESULT_OK;
    }

    APRESULT apaudio_cues_play(apaudio_cues cues, APAUDIO_CUE cue, const APAUDIO_CUE_PLAY_DESC *desc,
                               APAUDIO_VOICE *out_voice)
    {
        if (out_voice)
            *out_voice = 0;
        play_plan_t plan;
        APRESULT result = plan_play(cues, cue, desc, plan);
        if (result != APRESULT_OK || !plan.plays)
            return result;
        APAUDIO_VOICE voice;
        result = play_plan(cues, plan, voice);
        if (out_voice)
            *out_voice = voice;
        return result;
    }

    APRESULT apaudio_cues_start(apaudio_cues cues, APAUDIO_CUE cue, const APAUDIO_CUE_PLAY_DESC *desc,
                                APAUDIO_VOICE *out_voice)
    {
        if (out_voice)
            *out_voice = 0;
        if (!out_voice)
            return APRESULT_INVALID_ARGUMENT;
        play_plan_t plan;
        APRESULT result = plan_play(cues, cue, desc, plan);
        if (result != APRESULT_OK || !plan.plays)
            return result;

        std::lock_guard<std::mutex> lock(cues->playing_mutex);
        playing_t *free_slot = nullptr;
        for (playing_t &playing : cues->playings)
            if (!playing.voice)
            {
                free_slot = &playing;
                break;
            }
        if (!free_slot)
            return APRESULT_LIMIT;

        APAUDIO_VOICE voice;
        result = play_plan(cues, plan, voice);
        if (result != APRESULT_OK || !voice)
            return result; // nothing playing, nothing to stop: [out_voice] stays 0

        // Exclusive cues (music) crossfade: the bus's other exclusive
        // playing fades out as this one starts.
        if (plan.settings.exclusive)
            for (playing_t &other : cues->playings)
                if (other.voice && other.exclusive && other.bus == plan.settings.bus)
                    end_playing(cues, other, true);

        free_slot->voice = voice;
        free_slot->bus = plan.settings.bus;
        free_slot->exclusive = plan.settings.exclusive;
        free_slot->fade_out_ms = plan.settings.fade_out_ms;
        *out_voice = voice;
        return APRESULT_OK;
    }

    void apaudio_cues_stop(apaudio_cues cues, APAUDIO_VOICE voice, bool fade)
    {
        if (!cues)
            return;
        std::lock_guard<std::mutex> lock(cues->playing_mutex);
        if (playing_t *record = find_playing(cues, voice))
            end_playing(cues, *record, fade);
    }

    bool apaudio_cues_is_playing(apaudio_cues cues, APAUDIO_VOICE voice)
    {
        if (!cues)
            return false;
        std::lock_guard<std::mutex> lock(cues->playing_mutex);
        return find_playing(cues, voice) != nullptr;
    }
}
