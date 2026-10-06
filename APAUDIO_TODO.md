# ApAudio TODO

What `include/audio/apaudio.h` gets wrong, and what it lacks compared with a good
audio framework (FMOD, Wwise, miniaudio, XAudio2, AVAudioEngine). ApAudio is in the
build (`CMakeLists.txt`, with headless tests in `tests/apaudio_tests.c`), and
`src/audio/apaudio.cpp` implements the whole header: the "For the implementation"
notes below describe what it does, except where it says otherwise at the top of
that file. `apricotfields.h` doesn't include it yet. Its first caller is trellislib's `include/trellisaudio.h` (see
`trellislib/docs/audio.md`), which still plays through its silent output.

Items 1-4, the design bugs that were cheap to fix before implementing and
expensive after, are fixed in the header, and so are items 5, 6 and 11 (6 and 11 as
one design, sized for Apricot later becoming a game engine). Buses (item 10) are the
remaining missing feature that would force API changes later, so design them in now
even if they're built later. Everything else can be added without breaking callers.

Cues and sound banks - names over the mixer, defined by a manifest and its WAVs - are
`include/audio/apaudiocues.h` (`src/audio/apaudiocues.cpp`), moved down from trellislib
so any application on Apricot has them. It has no tests of its own yet: trellislib's
audio tests drive it.

## Wrong things: design bugs in the draft

### 1. A lost stop can leave a looping sound playing forever - fixed

`apaudio_voice_stop` went through the same fixed-size queue as `apaudio_play`, and
returned `APRESULT_LIMIT` (the request dropped) when the queue was full. With
`loop = true`, a dropped stop meant the sound never ended.

- **Fixed in the header:** a voice exists from the moment `apaudio_play` returns
  it, and holds its own stop request. The mixer allocates `max_voices +
  queue_capacity` voice records at creation, so an accepted play always has one.
  `apaudio_voice_stop` sets the request on the record instead of queueing it, so it
  never needs queue space and never fails for a valid mixer. `queue_capacity` now
  counts plays only. A voice stopped before the audio thread starts it never
  sounds.
- **For the implementation:** the stop request and its `release_frames` go in one
  atomic word on the record. The voice ID encodes the record's index plus a
  generation count, so the lookup is a direct index and a stale ID never stops the
  record's next voice. The audio thread returns finished records to a lock-free
  free list.

### 2. Playing and destroying the same sound can crash - fixed

`apaudio_play(sound, ...)` was allowed from any thread, and `apaudio_sound_destroy`
freed the handle at once on the control thread. A play running at the same moment
used freed memory.

- **Fixed in the header:** sounds are generational IDs (`APAUDIO_SOUND`), like
  `APAUDIO_VOICE`: a slot in a table sized by `APAUDIO_MIXER_DESC.max_sounds`, plus
  the slot's reuse count. Destroying a sound bumps the count, so its ID is stale at
  once and `apaudio_play` returns `APRESULT_NOT_FOUND`. The audio thread re-checks
  the ID as it starts a voice, which catches a play that raced the destroy. Memory
  is freed, and the slot reused, only after the audio thread confirms it's done.
  `apaudio_play` and `apaudio_sound_destroy` now take the mixer.
- **Rejected alternatives:** a documented "don't play while destroying" rule only
  moves the bug to the caller. Reference counting doesn't close the race by itself:
  a thread releasing the last reference while another starts a play still frees
  memory under it.
- **Also fixed in trellislib's `include/trellisaudio.h`:** `trellis_sound` is a
  `{bits}` handle like the model's, and `trellis_sound_play` returns
  `TRELLIS_ERROR_STALE_HANDLE` for a destroyed sound.

### 3. Resampling at load time - fixed

`apaudio_sound_create` converted each sound to the mixer's sample rate once, "never
while playing". Serious mixers keep sounds at their original rate and resample each
voice as it plays. Converting at load:

- tied every sound to one device's rate, so a device change at another rate forced
  the caller to rebuild every sound from samples it kept;
- made pitch and playback-rate changes impossible.

- **Fixed in the header:** sounds keep their own rate (1000 to 768000 Hz) and each
  voice is resampled as it plays. `APAUDIO_RESAMPLER` moved from the sound desc to
  `APAUDIO_MIXER_DESC`; `SINC` band-limits when playing faster, so it doesn't alias.
- **Playback rate:** `APAUDIO_PLAY_DESC.rate` (pitch and speed together), in
  `[0.125, 8]`. Zero is rejected, so a zeroed desc doesn't silently pick a rate.
  Changing it while playing is item 5.
- **Device change:** new `apaudio_mixer_set_format()` changes the mixer's output
  format and `max_frames` between the old stream stopping and the new one starting.
  Sounds and handles are kept, and playing voices carry on from where they were.
  trellislib's `include/trellisaudio.h` now says it moves the stream itself, keeping
  every sound.
- **For the implementation:** keep each voice's position as a fixed-point
  fraction of source frames (e.g. 32.32), so long loops don't drift. Sinc
  needs a few frames of history per voice, sized from `max_voices` at creation.

### 4. `APAUDIO_MIXER_DESC` breaks the `struct_size` versioning scheme - fixed

It embedded `SPUDAUDIO_FORMAT`, whose size SpudLib controls. If SpudLib added a
field, every later ApAudio field would move, which is exactly what `struct_size`
exists to prevent.

- **Fixed in the header:** a new frozen `APAUDIO_FORMAT` holds only what the mixer
  needs: sample format, rate, channel count, interleaved, and a *pointer* to
  `channel_count` channel positions (copied by the call). It never grows, so
  versioned structs may hold it by value. `APAUDIO_MIXER_DESC.format` and
  `apaudio_mixer_set_format()` both take it, and the conventions now state the
  rule: no versioned struct holds another module's struct by value.
- **Converting:** `static inline apaudio_format_from_spudaudio()` builds one from a
  `SPUDAUDIO_FORMAT`. Being inline, it compiles against the caller's
  `spudaudio.h`, so it reads the caller's struct as the caller built it.
- **Rejected:** a pointer to `SPUDAUDIO_FORMAT` (it has no `struct_size`, so an
  older caller's smaller struct would be over-read); copying `positions[64]` (ties
  ApAudio's layout to `SPUDAUDIO_MAX_CHANNELS`); having the mixer read the stream's
  config itself (the stream needs the mixer as user data at creation, and it would
  break device-free tests and caller-chosen layouts).
- **Precedent:** Vulkan never grows a struct; CoreAudio keeps
  `AudioStreamBasicDescription` frozen and passes `AudioChannelLayout` by pointer.

### 5. Voice settings are fixed once a sound starts - fixed

Gain and direction were only set in `APAUDIO_PLAY_DESC`. If the camera orbits during
a 2-second sound, its direction goes stale.

- **Fixed in the header:** `apaudio_voice_set_gain`, `apaudio_voice_set_direction`
  and `apaudio_voice_set_rate`, from any thread. Like stops (item 1), a change is
  recorded on the voice itself, not queued, so it never needs queue space and
  can't be lost. Each setting holds only its latest value: changes between two
  renders coalesce, and the next render ramps to the last one (gain and pan
  across the render, rate as a glide), so nothing clicks or zippers. A voice
  changed before it starts starts with the new value; a stopping voice keeps the
  values it's fading out with.
- **For the implementation:** each setting is one atomic word on the voice record
  plus a "changed" bit. Gain and rate are single floats. The direction packs into
  one 64-bit word as two floats (azimuth, elevation), which is ample precision for
  panning and avoids a lock or a sequence counter. The audio thread swaps each
  changed word out once per render and ramps from the voice's current smoothed
  value.
- **Not done in trellislib:** `trellis_sound_play` doesn't return a voice yet, so
  hosts can't stop or change a playing sound. That's needed for a sound to follow
  the camera, and is the trellislib follow-up.

### 6. Stolen voices are cut off instantly - fixed, with item 11

`APAUDIO_VOICE_STEAL_OLDEST` cut the old voice dead, which clicks. Stealing by age
was also a weak policy.

- **Fixed in the header, together with item 11,** as one prioritization system in
  the FMOD/Wwise/Unreal mould. There's no separate voice stealing any more:
  - **Logical vs real voices.** `max_voices` logical voices (the `APAUDIO_VOICE`
    IDs, cheap records) and `max_real_voices` mixing slots (the CPU budget).
  - **Ranking:** `APAUDIO_PLAY_DESC.priority` (0-255) first, then audibility: the
    voice's gain times its sound's loudness, measured once at creation. A voice
    never loses its slot to a lower priority. `virtual_threshold` makes very quiet
    voices virtual even with slots free. Ties within a few dB, and `min_real_us`,
    stop near-equal voices from swapping back and forth.
  - **Fades on every transition** (`transition_fade_us`). A pushed-out voice fades
    in one of `max_fading_voices` extra slots, so its replacement starts at once.
  - **`APAUDIO_VIRTUAL_BEHAVIOR`** per sound, overridable per play: `KILL` (UI
    sounds and one-shots), `CONTINUE`, `RESTART`, `PAUSE`.
  - **Concurrency groups** (`APAUDIO_CONCURRENCY`, generational IDs) replace
    per-sound limits: shared by many sounds, with `max_instances`, a
    `resolution` (`REJECT_NEW`, `STOP_OLDEST`, `STOP_QUIETEST`,
    `STOP_LOWEST_PRIORITY`), `retrigger_us` and `older_instance_gain`. A group's
    limit is a design rule that stops voices; the real-voice limit is a budget
    that only virtualizes them.
  - **`apaudio_mixer_update()`** on the control thread, once a frame, does the
    ranking. A new play doesn't wait for it: the render's fast path starts it in a
    free slot, or in place of the lowest-ranked real voice it outranks.
- **trellislib** (`include/trellisaudio.h`, `docs/audio.md`) uses a fixed subset:
  every sound `KILL`, a priority on plays, and `max_instances` / `retrigger_ms` per
  sound, which it turns into one group per sound with `STOP_OLDEST`.
- **For the implementation:** start simple. Rank by sorting a few dozen voices;
  build `KILL` and `CONTINUE`, and `REJECT_NEW`, `STOP_OLDEST` and
  `STOP_QUIETEST`, first. The real-voice set the update decides is posted to the
  audio thread through a lock-free double buffer. The fast path uses the scores
  from the last update.

### 7. The integer output path is crude

Integer formats are hard-clipped with no limiter and no dither. A dozen overlapping
clicks can easily sum past 1.0.

- **Fix:** dither (TPDF) when converting to 16-bit, and an optional master limiter
  or soft clip.

### 8. Folding rear sounds to the front is misleading on stereo

On a stereo layout a sound behind the listener is mirrored to the front, so a wall
placed behind the camera sounds like one in front. For the accessibility use
(Equate), that's actively wrong.

- **Fix:** a "behind" cue, such as a gentle low-pass filter (needs item 12), or at
  least a flag saying the direction was folded.

### 9. Smaller problems

- **Freed sound memory is reclaimed "by a later control call",** so an idle app can
  hold it indefinitely. An explicit `apaudio_mixer_collect()` is clearer.
- **A play that ends before sounding is invisible to the caller.** A play dropped
  by its group's `REJECT_NEW` or `retrigger_us`, or killed for lack of a real slot,
  still got a voice ID from `apaudio_play`, and the caller can't find out. Item 13's
  events ("voice killed", "voice went virtual") are the fix.

## Real gaps: things a good framework has

### 10. Buses (submix groups)

No way to group voices (UI, feedback, ambience) with their own gain and mute, and no
ducking (automatically lowering one group while another plays). Design this in now:
retrofitting it changes `apaudio_play`.

### 11. Voice priority and virtual voices - fixed, with item 6

There was no priority, and no virtual voices: tracking sounds too quiet to hear
without mixing them, so they come back correctly. Both are now part of item 6's
prioritization system.

### 12. Effects

No filters, reverb, compressor or limiter. A per-voice low-pass filter is the
minimum: it gives occlusion and "behind" cues (item 8).

### 13. Events from playback

No voice-finished or loop-point notifications (delivered off the audio thread), and
no way to ask how far a voice has played. `apaudio_voice_is_live()` is the polled
part of this: whether a voice has ended, which trellislib's playings need
(`trellis_audio_is_playing()`). Why it ended, and events, are still missing.

### 14. Monitoring

No peak levels, active voice count, audio-thread CPU time or glitch counts.

### 15. Streaming and compressed formats

Sounds are whole, in memory, and WAV only. Fine for clicks, not for anything longer.
Needs streamed voices and FLAC, Opus and Vorbis decoders.

### 16. Spatial audio beyond angles at play time

- **World-space sources and a listener:** frameworks place sources and a listener in
  3D, with distance attenuation curves, directional cones and doppler. Angles are
  enough for Trellis's placement sound, but a moving camera needs updates (item 5).
  Items 6 and 11 made this the next game-engine step: audibility should include
  distance attenuation, `apaudio_mixer_update()` is where listener and emitter maths
  goes, and it enables a `STOP_FARTHEST` resolution and distance-based priority
  offsets. The voice record should leave room for a world position alongside its
  direction.
- **Binaural (HRTF) rendering:** elevation can't be heard on ordinary stereo, and most
  users listen on headphones or laptop speakers.
- **Ambisonics,** for many simultaneous sources.

## Platform behavior that matters for Trellis

Not ApAudio's to decide, but nothing in the design prompts its callers to handle it.

- **System UI sound conventions.** On macOS, UI sounds should normally go to the
  system sounds (alert) device (`SPUDAUDIO_DEFAULT_ROLE_SYSTEM_SOUNDS`) and respect
  "Play user interface sound effects". That's trellislib's or the host's choice.
- **iOS audio session.** On the Apple roadmap, iOS needs an `AVAudioSession`
  category, or sounds are silenced by the ringer switch and fight with other apps'
  audio. Nothing in SpudAudio or ApAudio models that yet.
- **Following the default device** when it changes is the caller's job by design.
  It's blocked on SpudAudio's CoreAudio device events
  (`spudaudio_set_device_event_callback` returns `SPUDRESULT_NOT_IMPLEMENTED_YET`).
