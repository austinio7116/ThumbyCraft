/*
 * ThumbyCraft — procedural audio synthesis.
 *
 * Voice allocation (unchanged):
 *   0: music PAD,  1: music MELODY,  2-3: SFX pool
 *
 * Music v2 sound design (post-feedback iteration):
 *   - Sine waveform via 256-entry table + linear interpolation.
 *     Triangles were buzzy; sine is the cleanest tone at our
 *     22 kHz / 12-bit DAC budget and reads as bell/piano-ish.
 *   - PAD doubles root + octave-up sines, summed and halved.
 *     Adds harmonic richness without a third oscillator.
 *   - Exponential ADSR (per-sample coef) instead of linear ramp.
 *     No click on attack, smooth release tail, organic feel.
 *   - One global comb-delay reverb (~80 ms, 0.35 wet, 0.4 feedback).
 *     Cheap "in a cavern" space, ~4 KB BSS.
 *   - Slower pace: 16 s chords (was 8 s), 1.5-3 s between melody
 *     notes (was 0.5-1 s), 45 % rest probability.
 *   - Pentatonic biased to upper register for that bell-tone.
 *
 * SFX path unchanged: square/triangle/noise waveforms with simple
 * exponential gain decay. Ducks the music to 30 % for 350 ms.
 */
#include "craft_audio.h"
#include <string.h>

#define CRAFT_AUDIO_VOICES 4
#define MUSIC_PAD_VOICE    0
#define MUSIC_MELODY_VOICE 1
#define SFX_VOICE_FIRST    2
#define SFX_VOICE_LAST     3

typedef enum { W_SQR, W_TRI, W_NOISE, W_SINE } Wave;

typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_SUSTAIN,
    ENV_RELEASE
} EnvStage;

typedef struct {
    bool   on;
    bool   use_adsr;
    Wave   wave;

    float  phase;
    float  phase_inc;
    /* Optional second oscillator (octave-up for pad richness). */
    bool   dual;
    float  phase2;
    float  phase_inc2;

    float  gain;
    /* Exponential ADSR */
    EnvStage env;
    float    velocity;
    float    attack_coef;     /* per-sample multiplier */
    float    release_coef;
    float    sustain_remaining;  /* seconds until auto-release */

    /* Simple SFX decay */
    float    gain_dec;
    uint32_t noise_state;
} Voice;

static Voice voices[CRAFT_AUDIO_VOICES];
static int   sfx_rr = SFX_VOICE_FIRST;
static float ambient_gain = 0.025f;   /* gentler than v1 default */
static uint32_t ambient_state = 0x13371337;
static float    ambient_lp;

/* --- Sine table -------------------------------------------------- */
#define SINE_TABLE_BITS 8
#define SINE_TABLE_SIZE (1 << SINE_TABLE_BITS)
static float s_sine_table[SINE_TABLE_SIZE];
static bool  s_sine_ready;
static void  sine_init(void) {
    for (int i = 0; i < SINE_TABLE_SIZE; i++)
        s_sine_table[i] = sinf(6.2831853f * (float)i / (float)SINE_TABLE_SIZE);
    s_sine_ready = true;
}
static inline float sine_lookup(float phase) {
    /* phase in [0, 1) */
    float f = phase * (float)SINE_TABLE_SIZE;
    int   i = (int)f;
    float frac = f - (float)i;
    int   a = i & (SINE_TABLE_SIZE - 1);
    int   b = (a + 1) & (SINE_TABLE_SIZE - 1);
    return s_sine_table[a] * (1.0f - frac) + s_sine_table[b] * frac;
}

/* --- Delay reverb ------------------------------------------------ */
#define DELAY_SIZE   2048              /* power of 2 → 92.9 ms */
#define DELAY_MASK   (DELAY_SIZE - 1)
#define DELAY_TAP    1500              /* 68 ms — feels spacious */
static int16_t s_delay_ring[DELAY_SIZE];
static int     s_delay_pos;
#define REVERB_WET    0.32f
#define REVERB_FEED   0.40f

static float freq_to_inc(float hz) {
    return hz / (float)CRAFT_AUDIO_RATE;
}

/* --- Music state -------------------------------------------------- */

typedef struct {
    float pad_freq;          /* chord root (Hz) for the pad voice */
} Chord;

/* C major pentatonic — biased upward into the bell-tone register. */
static const float pent_freqs[10] = {
    329.63f, 392.00f, 440.00f, 523.25f, 587.33f,
    659.26f, 783.99f, 880.00f, 987.77f, 1174.66f,
    /* E4   G4      A4      C5      D5
       E5   G5      A5      B5      D6 */
};
#define PENT_COUNT 10

/* I-vi-IV-V in C major, pad plays root one octave down. */
static const Chord chord_prog[4] = {
    { 130.81f },   /* C3 — I  */
    { 220.00f },   /* A3 — vi */
    { 174.61f },   /* F3 — IV */
    { 196.00f },   /* G3 — V  */
};
#define CHORD_COUNT 4
#define CHORD_DURATION 16.0f      /* sec — twice the v1 value */
#define MELODY_MIN_GAP  1.5f
#define MELODY_MAX_GAP  3.0f
#define MELODY_REST_PCT 0.45f

typedef struct {
    bool   enabled;
    float  target_gain;
    float  cur_gain;
    float  t;
    int    chord_idx;
    int    last_chord_played;
    float  chord_t;
    float  next_note_t;
    int    last_note;
    float  duck_until;
    uint32_t rng;
} MusicState;

static MusicState s_music;

/* --- Common helpers ---------------------------------------------- */
static inline uint32_t xs(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x; return x;
}
static inline float frand01(uint32_t *s) {
    return (xs(s) & 0xFFFF) / 65535.0f;
}

void craft_audio_init(void) {
    memset(voices, 0, sizeof voices);
    sfx_rr = SFX_VOICE_FIRST;
    memset(&s_music, 0, sizeof s_music);
    s_music.rng               = 0xA110F00Du;
    s_music.target_gain       = 0.5f;
    s_music.cur_gain          = 0.0f;
    s_music.last_note         = 3;
    s_music.next_note_t       = 1.0f;
    s_music.last_chord_played = -1;
    memset(s_delay_ring, 0, sizeof s_delay_ring);
    s_delay_pos = 0;
    if (!s_sine_ready) sine_init();
}

/* --- SFX trigger -------------------------------------------------- */
static void trigger_sfx(float hz, Wave w, float gain, float decay_ms) {
    Voice *v = &voices[sfx_rr];
    sfx_rr = (sfx_rr == SFX_VOICE_LAST) ? SFX_VOICE_FIRST : sfx_rr + 1;
    v->on        = true;
    v->use_adsr  = false;
    v->wave      = w;
    v->phase     = 0.0f;
    v->phase_inc = freq_to_inc(hz);
    v->dual      = false;
    v->gain      = gain;
    float n      = decay_ms * 0.001f * (float)CRAFT_AUDIO_RATE;
    if (n < 1.0f) n = 1.0f;
    v->gain_dec  = expf(-7.0f / n);
    v->noise_state = 0xC0DE1234u ^ (uint32_t)(hz * 1009.0f);
    if (s_music.enabled) s_music.duck_until = s_music.t + 0.35f;
}

static float pitch_for(BlockId blk) {
    switch (blk) {
        case BLK_STONE:  return 180.0f;
        case BLK_COBBLE: return 165.0f;
        case BLK_DIRT:   return 110.0f;
        case BLK_GRASS:  return 130.0f;
        case BLK_SAND:   return 220.0f;
        case BLK_WOOD:   return 260.0f;
        case BLK_PLANK:  return 250.0f;
        case BLK_LEAVES: return 320.0f;
        case BLK_GLASS:  return 880.0f;
        case BLK_WATER:  return 90.0f;
        default:         return 200.0f;
    }
}
void craft_audio_break(BlockId blk) {
    float hz = pitch_for(blk);
    Wave w = (blk == BLK_STONE || blk == BLK_COBBLE) ? W_NOISE : W_SQR;
    trigger_sfx(hz, w, 0.55f, 180.0f);
    trigger_sfx(hz * 0.5f, w, 0.30f, 220.0f);
}
void craft_audio_place(BlockId blk) {
    trigger_sfx(pitch_for(blk) * 1.5f, W_TRI, 0.45f, 120.0f);
}
void craft_audio_step(void) { trigger_sfx(140.0f, W_NOISE, 0.20f, 90.0f); }
void craft_audio_jump(void) { trigger_sfx(380.0f, W_TRI, 0.35f, 140.0f); }

/* Per-material footstep tone. Sand whooshes (high noise), stone clacks
 * (mid noise), grass rustles (low noise + quick), wood is a triangle
 * thunk. Volume low so each step is felt but doesn't drown out music
 * (and the music-duck applies as for any SFX). */
void craft_audio_step_on(BlockId blk) {
    switch (blk) {
        case BLK_GRASS:
        case BLK_LEAVES:
            trigger_sfx(220.0f, W_NOISE, 0.10f, 55.0f);
            break;
        case BLK_DIRT:
            trigger_sfx(140.0f, W_NOISE, 0.10f, 70.0f);
            break;
        case BLK_STONE:
        case BLK_COBBLE:
            trigger_sfx(300.0f, W_NOISE, 0.12f, 50.0f);
            break;
        case BLK_SAND:
            trigger_sfx(380.0f, W_NOISE, 0.08f, 80.0f);
            break;
        case BLK_WOOD:
        case BLK_PLANK:
            trigger_sfx(220.0f, W_TRI,   0.08f, 50.0f);
            break;
        case BLK_GLASS:
            trigger_sfx(700.0f, W_TRI,   0.07f, 40.0f);
            break;
        default:
            trigger_sfx(180.0f, W_NOISE, 0.08f, 60.0f);
            break;
    }
}
void craft_audio_set_ambient(float g) {
    if (g < 0) g = 0;
    if (g > 1) g = 1;
    ambient_gain = g;
}

/* --- Music ------------------------------------------------------- */
void craft_audio_music_enable(bool on) {
    s_music.enabled = on;
    if (on) {
        s_music.last_chord_played = -1;
        if (!s_sine_ready) sine_init();
    }
}
bool craft_audio_music_is_enabled(void) { return s_music.enabled; }
void craft_audio_music_set_volume(float v) {
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    s_music.target_gain = v;
}

/* Exponential envelope: per-sample coef chosen so that the envelope
 * reaches ~95 % of the target after the user-supplied seconds.
 * coef = exp(-3 / (sec * RATE)). For 0.5 s @ 22050 → coef ≈ 0.99973. */
static float env_coef_for(float seconds) {
    float n = seconds * (float)CRAFT_AUDIO_RATE;
    if (n < 1.0f) n = 1.0f;
    return expf(-3.0f / n);
}

/* Trigger a music voice with sine waveform + exp ADSR. */
static void trigger_music_voice(int voice_idx, float hz, bool dual_octave,
                                float velocity,
                                float attack_t, float sustain_hold_t,
                                float release_t) {
    Voice *v = &voices[voice_idx];
    v->on        = true;
    v->use_adsr  = true;
    v->wave      = W_SINE;
    v->phase     = 0.0f;
    v->phase_inc = freq_to_inc(hz);
    v->dual      = dual_octave;
    v->phase2    = 0.5f;        /* offset 180° so peaks don't double */
    v->phase_inc2 = v->phase_inc * 2.0f;   /* one octave up */
    v->attack_coef  = env_coef_for(attack_t);
    v->release_coef = env_coef_for(release_t);
    v->velocity     = velocity;
    v->sustain_remaining = sustain_hold_t;
    v->env       = ENV_ATTACK;
    /* gain is preserved from previous note for smooth re-trigger. */
}

static int pick_next_melody_note(int last, uint32_t *rng) {
    if (last < 0) return (int)(xs(rng) % PENT_COUNT);
    uint32_t r = xs(rng);
    int delta;
    if ((r & 0x3FF) < 0x266) {           /* ~60% step */
        delta = (int)((r >> 10) & 3) - 1;          /* -1..+2 */
        if (delta == 0) delta = ((r >> 12) & 1) ? 1 : -1;
    } else if ((r & 0x3FF) < 0x366) {     /* ~25% leap */
        delta = (int)((r >> 12) & 7) - 3;          /* -3..+4 */
    } else {                              /* ~15% repeat */
        delta = 0;
    }
    int next = last + delta;
    if (next < 0) next = 0;
    if (next >= PENT_COUNT) next = PENT_COUNT - 1;
    return next;
}

void craft_audio_music_tick(float dt) {
    if (!s_music.enabled) {
        s_music.cur_gain *= expf(-3.0f * dt);
        return;
    }
    s_music.t += dt;
    s_music.chord_t += dt;

    if (s_music.chord_t >= CHORD_DURATION) {
        s_music.chord_t = 0;
        s_music.chord_idx = (s_music.chord_idx + 1) % CHORD_COUNT;
    }
    if (s_music.last_chord_played != s_music.chord_idx) {
        s_music.last_chord_played = s_music.chord_idx;
        trigger_music_voice(
            MUSIC_PAD_VOICE,
            chord_prog[s_music.chord_idx].pad_freq,
            /* dual octave */ true,
            /* velocity */ 0.38f,
            /* attack */ 1.8f,
            /* sustain hold */ CHORD_DURATION - 1.5f,
            /* release */ 2.5f);
    }

    if (s_music.t >= s_music.next_note_t) {
        uint32_t r = xs(&s_music.rng);
        bool rest = (r & 0xFFFF) < (uint32_t)(MELODY_REST_PCT * 0x10000);
        if (!rest) {
            int note = pick_next_melody_note(s_music.last_note, &s_music.rng);
            s_music.last_note = note;
            float hz = pent_freqs[note];
            trigger_music_voice(
                MUSIC_MELODY_VOICE,
                hz, /* dual */ false,
                /* velocity */ 0.28f,
                /* attack */ 0.04f,
                /* sustain hold */ 0.20f,
                /* release */ 1.4f);
        }
        float span = MELODY_MAX_GAP - MELODY_MIN_GAP;
        s_music.next_note_t = s_music.t + MELODY_MIN_GAP + frand01(&s_music.rng) * span;
    }

    float target = s_music.target_gain;
    if (s_music.t < s_music.duck_until) target *= 0.30f;
    s_music.cur_gain += (target - s_music.cur_gain) * (1.0f - expf(-5.0f * dt));
}

/* --- Voice sample ------------------------------------------------- */
static inline float voice_sample(Voice *v) {
    /* Envelope */
    if (v->use_adsr) {
        switch (v->env) {
            case ENV_ATTACK:
                v->gain = v->velocity - (v->velocity - v->gain) * v->attack_coef;
                if (v->gain > v->velocity * 0.97f) {
                    v->gain = v->velocity;
                    v->env  = ENV_SUSTAIN;
                }
                break;
            case ENV_SUSTAIN:
                v->sustain_remaining -= 1.0f / (float)CRAFT_AUDIO_RATE;
                if (v->sustain_remaining <= 0.0f) v->env = ENV_RELEASE;
                break;
            case ENV_RELEASE:
                v->gain *= v->release_coef;
                if (v->gain < 0.0005f) { v->on = false; v->gain = 0.0f; }
                break;
            default:
                v->on = false;
                v->gain = 0.0f;
                break;
        }
    } else {
        v->gain *= v->gain_dec;
        if (v->gain < 0.0005f) v->on = false;
    }

    /* Waveform */
    float s = 0.0f;
    switch (v->wave) {
        case W_SQR: s = (v->phase < 0.5f) ? 1.0f : -1.0f; break;
        case W_TRI: s = (v->phase < 0.5f)
                         ? (v->phase * 4.0f - 1.0f)
                         : (3.0f - v->phase * 4.0f);
                    break;
        case W_SINE: {
            s = sine_lookup(v->phase);
            if (v->dual) {
                s = (s + sine_lookup(v->phase2)) * 0.5f;
                v->phase2 += v->phase_inc2;
                if (v->phase2 >= 1.0f) v->phase2 -= 1.0f;
            }
            break;
        }
        case W_NOISE: {
            uint32_t r = xs(&v->noise_state);
            s = ((int32_t)(r & 0xFFFF) - 32768) / 32768.0f;
            break;
        }
    }
    v->phase += v->phase_inc;
    if (v->phase >= 1.0f) v->phase -= 1.0f;
    return s * v->gain;
}

int craft_audio_render(int16_t *out, int n) {
    float mg = s_music.cur_gain;
    for (int i = 0; i < n; i++) {
        /* Music dry mix (PAD + MELODY) — fed through reverb. */
        float music_dry = 0.0f;
        if (voices[MUSIC_PAD_VOICE].on)
            music_dry += voice_sample(&voices[MUSIC_PAD_VOICE]);
        if (voices[MUSIC_MELODY_VOICE].on)
            music_dry += voice_sample(&voices[MUSIC_MELODY_VOICE]);
        music_dry *= mg;

        /* Reverb tap. */
        int tap_idx = (s_delay_pos + DELAY_SIZE - DELAY_TAP) & DELAY_MASK;
        float wet = s_delay_ring[tap_idx] / 32768.0f;
        float feedback_sample = music_dry + wet * REVERB_FEED;
        if (feedback_sample >  1.0f) feedback_sample =  1.0f;
        if (feedback_sample < -1.0f) feedback_sample = -1.0f;
        s_delay_ring[s_delay_pos] = (int16_t)(feedback_sample * 32767.0f);
        s_delay_pos = (s_delay_pos + 1) & DELAY_MASK;

        /* SFX voices direct (no reverb). */
        float sfx_mix = 0.0f;
        for (int j = SFX_VOICE_FIRST; j <= SFX_VOICE_LAST; j++) {
            if (voices[j].on) sfx_mix += voice_sample(&voices[j]);
        }

        /* Ambient noise — quieter than v1. */
        float noise = ((int32_t)(xs(&ambient_state) & 0xFFFF) - 32768) / 32768.0f;
        ambient_lp += (noise - ambient_lp) * 0.03f;

        float mix = music_dry + wet * REVERB_WET + sfx_mix + ambient_lp * ambient_gain;
        if (mix >  1.0f) mix =  1.0f;
        if (mix < -1.0f) mix = -1.0f;
        out[i] = (int16_t)(mix * 16000.0f);
    }
    return n;
}
