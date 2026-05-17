/*
 * ThumbyCraft — procedural audio synthesis.
 *
 * v1: short tones for break/place feedback + a low ambient drone.
 * Output is a stream of int16 mono samples at 22050 Hz that the
 * platform layer pushes to its audio sink (PWM on device, SDL on host).
 */
#ifndef CRAFT_AUDIO_H
#define CRAFT_AUDIO_H

#include "craft_types.h"
#include "craft_blocks.h"

#define CRAFT_AUDIO_RATE 22050

void craft_audio_init(void);

/* Trigger short SFX. Routed to the SFX voice pool (2 voices), so
 * music and SFX never compete for synth voices. */
void craft_audio_break(BlockId blk);
void craft_audio_place(BlockId blk);
void craft_audio_step(void);
void craft_audio_step_on(BlockId blk);   /* material-aware footstep */
void craft_audio_jump(void);
void craft_audio_pickaxe_ting(void);     /* "needs pickaxe" feedback */

/* Fill `out` with `n_samples` of int16 mono PCM. Always succeeds —
 * never blocks. Returns the number of samples written. */
int  craft_audio_render(int16_t *out, int n_samples);

/* Ambient drone gain 0..1 (low rumble + wind). Quiet by default. */
void craft_audio_set_ambient(float gain);

/* --- Procedural ambient music (Phase 34) ------------------------- *
 * C-major pentatonic random-walk melody over a I-vi-IV-V chord
 * progression at ~60 BPM. Uses voices 0 (pad) and 1 (melody); SFX
 * gets voices 2-3. Music ducks to ~30% during SFX events and ramps
 * back over half a second. Toggle via craft_audio_music_enable —
 * defaults off so the caller must opt in. */
void craft_audio_music_enable(bool on);
bool craft_audio_music_is_enabled(void);
void craft_audio_music_set_volume(float vol);   /* 0..1, default 0.5 */
void craft_audio_music_tick(float dt);          /* call per game frame */
void craft_audio_music_set_sun(float sun_y);    /* +1 noon, -1 midnight — picks day vs night style */

#endif
