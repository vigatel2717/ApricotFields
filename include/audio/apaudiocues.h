/*
 * Built in src/audio/apaudiocues.cpp. apricotfields.h doesn't include it
 * yet.
 */

#ifndef APAUDIOCUES_H
#define APAUDIOCUES_H

#include "apaudio.h"
#include "../archive/aparchive.h"

#if __cplusplus
extern "C" {
#endif

/*
 * ApAudio's cues: named sounds over a mixer (apaudio.h), defined by sound
 * banks. This is the event model of FMOD Studio and Wwise - code states
 * what happened ("tool.wall.placed", "footstep.gravel"), data decides how
 * it sounds.
 *
 * A cue table holds cue names and what the loaded banks define for each:
 * which sounds (one picked at random each play), gain, pitch variation,
 * bus, priority, instance limits, loop, fades. A bank is a manifest.json
 * and the WAV files it names (format below). Banks load over each other:
 * a later bank's cues replace the same names in earlier ones and leave the
 * rest, so an application ships defaults and a user's bank overrides a
 * few. Loading a bank again - its files edited - is the same thing, so
 * sounds change in a running application.
 *
 * General purpose: it knows no cue names and no bus names. The caller says
 * which of its mixer's buses banks may route to, and by what names; the
 * names cues have are whatever its banks and code agree on.
 *
 * It does no I/O of its own, like ApArchive: a bank's files come through
 * an APAUDIO_BANK_SOURCE the caller implements (a folder, files embedded
 * in the executable), or from a ZIP archive through an APARCHIVE_SOURCE.
 *
 * Threads, results, error messages and struct versions are apaudio.h's
 * (Conventions). Here, also:
 *   APRESULT_CORRUPT_DATA  a malformed bank: its manifest, a sound it
 *                          names and doesn't hold, a WAV that isn't mono
 *                          or stereo
 *   APRESULT_LIMIT         a bank whose new cue names don't fit max_cues,
 *                          or whose sounds or groups the mixer has no slot
 *                          for; a play with the mixer's queue full; a
 *                          start with max_playings already live
 *   APRESULT_NOT_FOUND     a cue that's 0 or unknown
 */

/* ---- Bank format ----------------------------------------------------- */

/*
 * manifest.json, read strictly - an unknown key, a missing required one or
 * an out-of-range value fails the bank, so a typo shows up where its
 * author sees it rather than as a cue that's quietly wrong. A malformed
 * bank loads nothing, rather than half of itself.
 *
 *   {
 *     "version": 1,
 *     "cues": {
 *       "tool.wall.placed": {
 *         "sounds": ["placed_1.wav", "placed_2.wav"],
 *         "bus": "feedback",
 *         "gain": 0.8, "pitch_variation": 0.03, "priority": 160,
 *         "max_instances": 2, "retrigger_ms": 15,
 *         "loop": false, "exclusive": false,
 *         "fade_in_ms": 0, "fade_out_ms": 0
 *       }
 *     },
 *     "buses": { "accessibility": { "duck": { "feedback": 0.3 } } }
 *   }
 *
 * Required: "version" (1), "cues", and each cue's "sounds" and "bus".
 *   sounds           files of the bank, WAV, mono or stereo; one is picked
 *                    at random each play. May be empty: the cue is
 *                    defined and plays nothing.
 *   bus              one of the table's buses, by name
 *   gain             linear, finite, >= 0 (1)
 *   pitch_variation  a random playback-rate offset each play, as a
 *                    fraction either side: 0 to under 1 (0)
 *   priority         0-255, as APAUDIO_PLAY_DESC.priority (128)
 *   max_instances, retrigger_ms
 *                    with either, the cue gets a concurrency group of its
 *                    own, STOP_OLDEST (none)
 *   loop             plays until stopped (false)
 *   exclusive        started (apaudio_cues_start()), it fades out its
 *                    bus's other exclusive playing: music (false)
 *   fade_in_ms       recorded for the caller (APAUDIO_CUE_INFO); not
 *                    applied yet (0)
 *   fade_out_ms      how long apaudio_cues_stop() fades it out (0)
 * "buses" holds settings per bus: "duck" lowers other buses, each to a
 * gain from 0 to 1, while any voice on this bus is audible. A later bank's
 * settings for a bus replace an earlier one's.
 */

/* ---- Cue table ------------------------------------------------------- */

typedef struct apaudio_cues_t *apaudio_cues;

/* A cue name's size in bytes, including the terminating NUL. */
#define APAUDIO_CUE_NAME_SIZE 64

/* One of the mixer's buses that banks may route cues to. Frozen. */
typedef struct APAUDIO_CUES_BUS {
	/* What manifests call it; copied. */
	const char *name;
	APAUDIO_BUS bus;
	/* What its cues' voices do while not real (KILL for feedback, CONTINUE
	 * for music...); required. */
	APAUDIO_VIRTUAL_BEHAVIOR virtual_behavior;
} APAUDIO_CUES_BUS;

typedef struct APAUDIO_CUES_DESC {
	uint32_t struct_size;
	/* The mixer cues play on. It outlives the table. */
	apaudio_mixer mixer;
	/* [bus_count] buses, at least one, copied. Elsewhere a bus is named by
	 * its index here. The master isn't normally one of them: everything
	 * plays under it. */
	const APAUDIO_CUES_BUS *buses;
	uint32_t bus_count;
	/* Cue names the table can hold, found or defined. Names are only ever
	 * added, which is what keeps an APAUDIO_CUE valid for the table's
	 * life. */
	uint32_t max_cues;
	/* Sustained playings (apaudio_cues_start()) live at once. */
	uint32_t max_playings;
	/* The mixer's max_voices: the instance limit of a cue that has a
	 * retrigger_ms but no max_instances. */
	uint32_t max_voices;
	/* The mixer's output rate, for fades given in milliseconds. Keep it
	 * current with apaudio_cues_set_sample_rate(). */
	uint32_t sample_rate;
	/* The most a bank's manifest and sounds may come to, in bytes,
	 * uncompressed. */
	uint64_t max_bank_size;
	/* How fast a bus's ducking comes in and lets go. */
	uint32_t duck_fade_in_us;
	uint32_t duck_fade_out_us;
} APAUDIO_CUES_DESC;

/* Thread: control. An empty table: no names, no banks. */
APRESULT apaudio_cues_create(
    const APAUDIO_CUES_DESC *desc,
    apaudio_cues *out_cues,
    APAUDIO_ERROR *error);
/* Thread: control. Destroys the table and, on its mixer, every sound,
 * concurrency group and ducking modifier its banks made. Destroy it before
 * the mixer. Voices still playing stop at the next render. */
void apaudio_cues_destroy(apaudio_cues cues);

/* Thread: any. The mixer's output rate changed
 * (apaudio_mixer_set_format()). */
void apaudio_cues_set_sample_rate(apaudio_cues cues, uint32_t sample_rate);

/* Thread: control. Forgets playings whose voice ended by itself - a sound
 * that doesn't loop played to its end, or one the mixer dropped. Call it
 * once a frame, after apaudio_mixer_update(). */
void apaudio_cues_update(apaudio_cues cues);

/* ---- Banks ----------------------------------------------------------- */

/* Where a bank's files come from, by the names its manifest uses
 * ("manifest.json" itself first). Frozen. Called only during the load
 * call, on its thread.
 *   [get_size]  the size in bytes of the file named [name]; false if the
 *               bank doesn't hold it - including a name the source won't
 *               serve, such as one leading out of a folder
 *   [read]      copies the file whole into [buffer], of the [size]
 *               get_size gave; false on any failure */
typedef struct APAUDIO_BANK_SOURCE {
	void *user;
	bool (*get_size)(void *user, const char *name, uint64_t *out_size);
	bool (*read)(void *user, const char *name, void *buffer, uint64_t size);
} APAUDIO_BANK_SOURCE;

/* Thread: control. Reads the bank behind [source] whole, decoding its
 * sounds, then applies it over what's loaded: its sounds become the
 * mixer's, its cues replace the same names, its buses' ducking replaces
 * theirs. A play sees all of the bank or none of it. Cues it replaces let
 * go of their sounds; voices playing those stop at the next render.
 *
 * Failures load nothing: APRESULT_CORRUPT_DATA (malformed, with why in
 * [error]); APRESULT_IO (the source failed reading a file it holds);
 * APRESULT_LIMIT; APRESULT_OUT_OF_MEMORY; APRESULT_INVALID_ARGUMENT. */
APRESULT apaudio_cues_load_bank(
    apaudio_cues cues,
    const APAUDIO_BANK_SOURCE *source,
    APAUDIO_ERROR *error);

/* Thread: control. As apaudio_cues_load_bank(), for a bank that's a ZIP
 * archive holding manifest.json at its root. APRESULT_NOT_IMPLEMENTED
 * until ApArchive's reader exists. */
APRESULT apaudio_cues_load_zip_bank(
    apaudio_cues cues,
    const APARCHIVE_SOURCE *archive,
    APAUDIO_ERROR *error);

/* ---- Cues ------------------------------------------------------------ */

/* A cue: a handle to a cue name - its index in the table plus one. Never
 * 0, never stale. It names the cue, not a bank's definition of it: a cue
 * no loaded bank defines is valid and silent, so cues can be found before
 * or after banks load, in any order. */
typedef uint64_t APAUDIO_CUE;

/* Thread: any. The cue named [name] (under APAUDIO_CUE_NAME_SIZE bytes),
 * adding the name if it's new. Find cues once and keep the handles:
 * playing by handle never looks a name up. APRESULT_INVALID_ARGUMENT for
 * an empty or too long name; APRESULT_LIMIT with max_cues names already
 * known. [out_cue] is 0 on failure. */
APRESULT apaudio_cues_find(apaudio_cues cues, const char *name, APAUDIO_CUE *out_cue);

/* Thread: any. Turns one cue on or off, wherever it's played from. Cues
 * are on until turned off. Turning one off stops nothing already playing.
 * The latest call wins. */
APRESULT apaudio_cues_set_enabled(apaudio_cues cues, APAUDIO_CUE cue, bool enabled);

/* Thread: any. While set, a play of a cue on the bus at [bus_index] plays
 * nothing and succeeds. It doesn't mute the bus - apaudio_bus_set_muted()
 * does, and a voice started on a muted bus waits, silent, for the unmute.
 * This is for feedback that would be wrong late: set both.
 * APRESULT_INVALID_ARGUMENT for an index past the table's buses. */
APRESULT apaudio_cues_set_bus_muted(apaudio_cues cues, uint32_t bus_index, bool muted);

/* Thread: control. The modifier ducking other buses while the bus at
 * [bus_index] sounds, from the last bank with settings for it; 0 if it
 * ducks nothing. Replaced, and the old one destroyed, by a later bank. */
APAUDIO_MODIFIER apaudio_cues_get_bus_ducking(apaudio_cues cues, uint32_t bus_index);

/* What the loaded banks define for a cue. */
typedef struct APAUDIO_CUE_INFO {
	uint32_t struct_size;
	/* False for a cue no loaded bank defines; the rest is then its
	 * defaults. */
	bool defined;
	bool loop;
	bool exclusive;
	uint8_t priority;
	uint32_t sound_count;
	uint32_t bus_index;
	float gain;
	float pitch_variation;
	uint32_t max_instances; /* 0: no limit of its own */
	uint32_t retrigger_ms;
	uint32_t fade_in_ms;
	uint32_t fade_out_ms;
} APAUDIO_CUE_INFO;

/* Thread: any. Fills [out_info] up to its struct_size, which the caller
 * sets. */
APRESULT apaudio_cues_get_info(apaudio_cues cues, APAUDIO_CUE cue, APAUDIO_CUE_INFO *out_info);

/* ---- Playing --------------------------------------------------------- */

typedef struct APAUDIO_CUE_PLAY_DESC {
	uint32_t struct_size;
	/* Times the cue's own gain: linear, finite, >= 0. */
	float gain;
	/* As APAUDIO_PLAY_DESC's; NULL for none. */
	const APAUDIO_DIRECTION *direction;
	/* As APAUDIO_PLAY_DESC's; 0 for as soon as possible. */
	uint64_t start_time_ns;
} APAUDIO_CUE_PLAY_DESC;

/* Thread: any. Never blocks on sound. Plays [cue] once: one of its sounds,
 * on its bus, with its gain, pitch variation, priority and limits. [desc]
 * may be NULL (gain 1, no direction). [out_voice] (may be NULL) is the
 * mixer's voice, or 0 when nothing was queued.
 *
 * A cue that's off, that no bank defines, that has no sounds, or whose bus
 * is muted here plays nothing and succeeds. So does one whose sound a bank
 * replaced in the instant between picking it and playing it. What the
 * mixer then does with the voice - its group's limits, real or virtual -
 * is apaudio_play()'s. */
APRESULT apaudio_cues_play(
    apaudio_cues cues,
    APAUDIO_CUE cue,
    const APAUDIO_CUE_PLAY_DESC *desc,
    APAUDIO_VOICE *out_voice);

/* Thread: any. As apaudio_cues_play(), for a sustained playing - music, a
 * loop - that the table keeps track of until it ends: apaudio_cues_stop()
 * fades it out over its cue's fade_out_ms, and an exclusive cue starting
 * fades out its bus's other exclusive playing. [out_voice] is required;
 * the playing is named by its voice. APRESULT_LIMIT also with max_playings
 * already live. */
APRESULT apaudio_cues_start(
    apaudio_cues cues,
    APAUDIO_CUE cue,
    const APAUDIO_CUE_PLAY_DESC *desc,
    APAUDIO_VOICE *out_voice);

/* Thread: any. Ends the playing [voice]: over its cue's fade_out_ms if
 * [fade], else at once. A voice that isn't a live playing does nothing. */
void apaudio_cues_stop(apaudio_cues cues, APAUDIO_VOICE voice, bool fade);

/* Thread: any. Whether [voice] is a playing that's started and not yet
 * stopped or seen to have ended. One that ends by itself is seen at the
 * next apaudio_cues_update(). A stopped playing isn't live even while it
 * fades out. */
bool apaudio_cues_is_playing(apaudio_cues cues, APAUDIO_VOICE voice);

#if __cplusplus
}
#endif

#endif // APAUDIOCUES_H
