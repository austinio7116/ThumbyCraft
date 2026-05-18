/*
 * ThumbyCraft — procedural audio synthesis.
 *
 * Voice allocation (unchanged):
 *   0: music PAD,  1: music MELODY,  2-3: SFX pool
 *
 * Music v3 sound design (Claire-de-Lune-inspired):
 *   - PAD voice plays a full 4-note chord through ONE envelope —
 *     true voicings with maj7/m9/sus colours, not just root+octave.
 *     Bass note kept above ~195 Hz so the loudness boost doesn't
 *     clip a low sine into square-wave buzz.
 *   - Modal F-centric progression with smooth voice leading —
 *     each chord shares 2-3 notes with the next so changes glide
 *     instead of jumping.
 *   - Slow attack (3 s) on the chord — blooms rather than punches.
 *   - 20 s per chord, 2.5-5.5 s between melody notes, 55 % rest.
 *   - Reverb feedback reduced (0.40 → 0.22) so sustained chords
 *     no longer build a mud halo. Wet 0.32 → 0.28 for clarity.
 *   - Per-note velocity 0.10 (chord) / 0.16 (melody) so the 3×
 *     output boost lands the chord peak around 0.4, well under
 *     the clamp wall.
 *
 * SFX path unchanged: square/triangle/noise waveforms with simple
 * exponential gain decay. Ducks the music to 30 % for 350 ms.
 */
#include "craft_audio.h"
#include <math.h>
#include <string.h>

/* 8 voices total — 6 in the music pool (notes pick the oldest
 * released voice on trigger so previous notes can ring through),
 * 2 reserved for SFX. */
#define CRAFT_AUDIO_VOICES 8
#define MUSIC_VOICE_FIRST  0
#define MUSIC_VOICE_LAST   5
#define MUSIC_VOICE_COUNT  (MUSIC_VOICE_LAST - MUSIC_VOICE_FIRST + 1)
#define SFX_VOICE_FIRST    6
#define SFX_VOICE_LAST     7

typedef enum { W_SQR, W_TRI, W_NOISE, W_SINE } Wave;

typedef enum {
    ENV_IDLE = 0,
    ENV_ATTACK,
    ENV_SUSTAIN,
    ENV_RELEASE
} EnvStage;

/* Up to MAX_OSC oscillators per voice — pad chord voice uses 4 to
 * play a full chord through a single envelope; melody/SFX use 1. */
#define MAX_OSC 4

typedef struct {
    bool   on;
    bool   use_adsr;
    Wave   wave;

    int    n_osc;                /* 1..MAX_OSC */
    float  phase[MAX_OSC];
    float  phase_inc[MAX_OSC];

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
/* Heavy reverb — this is the engine's sustain-pedal substitute. With
 * only two music voices we can't truly hold previous notes through
 * fresh triggers, so the reverb tail does that work integratively:
 * each note's release contributes a long decaying smear that fills
 * the gaps between notes and gives the perceived sustain Debussy's
 * piano writing relies on. Feedback at 0.55 yields a ~3-4 s tail. */
#define REVERB_WET    0.45f
#define REVERB_FEED   0.55f

static float freq_to_inc(float hz) {
    return hz / (float)CRAFT_AUDIO_RATE;
}

/* --- Music: Clair de Lune note timeline -------------------------- *
 *
 * Total rewrite. The previous generator (chord pad + arpeggio figure
 * scheduler) couldn't sound like the piece because the architecture
 * was wrong: a held drone with notes on top will always feel like
 * a synth pad, never like a piano playing Clair de Lune.
 *
 * New architecture: a hand-composed timeline of NOTE EVENTS — like a
 * tracker. The first ~14 seconds of Clair de Lune (opening 4 bars in
 * Db major, 9/8 time) are written out as explicit notes with start
 * times, frequencies, durations, and velocities. The engine just
 * plays them back in order and loops.
 *
 * Two music voices:
 *   LH  — left-hand chord stabs (4 oscillators per strike).
 *   RH  — right-hand melody (1 or 2 oscillators).
 *
 * Each note in the timeline targets one voice. Voice retriggers
 * preserve oscillator phase (see trigger_music_voice) so successive
 * melody notes don't produce envelope-step clicks. */

typedef struct {
    float    t;          /* start time in seconds from sequence start */
    float    hz[4];      /* fundamental(s) — 1 for melody, 2 for octave-doubled, 4 for chord */
    uint8_t  n_hz;
    uint8_t  voice;      /* 0 = LH, 1 = RH */
    float    vel;
    float    attack;
    float    sustain;
    float    release;
} CDLNote;

/* Note timeline lives in a separate header — it's ~915 events,
 * auto-generated from the actual Clair de Lune MIDI by
 * /tmp/cdl_to_c.py. Defines cdl_seq[], CDL_SEQ_LEN, CDL_SEQ_PERIOD. */
#include "craft_audio_cdl_data.h"

typedef struct {
    bool     enabled;
    float    target_gain;
    float    cur_gain;
    float    t;            /* global time since music start (for SFX duck timing) */
    float    seq_t;        /* time within current loop iteration */
    int      next_idx;     /* next event in cdl_seq[] to fire */
    float    duck_until;
    float    sun_y;        /* unused now, kept for API stability */
    uint32_t rng;
} MusicState;

static MusicState s_music;

/* Set by the world layer each frame so the music can react to the
 * day/night cycle. Default +1 (day) so tests / host runs without a
 * world clock still produce music. */
void craft_audio_music_set_sun(float sun_y) {
    s_music.sun_y = sun_y;
}

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
    s_music.rng         = 0xA110F00Du;
    s_music.target_gain = 0.70f;
    s_music.cur_gain    = 0.0f;
    s_music.t           = 0.0f;
    s_music.seq_t       = 0.0f;
    s_music.next_idx    = 0;
    s_music.sun_y       = 1.0f;
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
    v->n_osc     = 1;
    v->phase[0]     = 0.0f;
    v->phase_inc[0] = freq_to_inc(hz);
    v->gain      = gain;
    float n      = decay_ms * 0.001f * (float)CRAFT_AUDIO_RATE;
    if (n < 1.0f) n = 1.0f;
    v->gain_dec  = expf(-7.0f / n);
    v->noise_state = 0xC0DE1234u ^ (uint32_t)(hz * 1009.0f);
    if (s_music.enabled) s_music.duck_until = s_music.t + 0.35f;
}

/* Per-material break sounds — layered transient (noise burst) plus
 * tonal body. Tunings target a percussive "hit" feel rather than
 * the buzzy beeps the previous single-voice version had. */
void craft_audio_break(BlockId blk) {
    switch (blk) {
        case BLK_STONE:
        case BLK_COBBLE:
            trigger_sfx(320.0f, W_NOISE, 0.65f,  50.0f);
            trigger_sfx(110.0f, W_NOISE, 0.45f, 180.0f);
            break;
        case BLK_COAL_ORE:
            trigger_sfx(260.0f, W_NOISE, 0.65f,  60.0f);
            trigger_sfx( 90.0f, W_NOISE, 0.45f, 200.0f);
            break;
        case BLK_DIRT:
            trigger_sfx(200.0f, W_NOISE, 0.50f,  80.0f);
            trigger_sfx( 90.0f, W_TRI,   0.35f, 180.0f);
            break;
        case BLK_GRASS:
            trigger_sfx(420.0f, W_NOISE, 0.40f,  60.0f);
            trigger_sfx(180.0f, W_TRI,   0.25f, 150.0f);
            break;
        case BLK_SAND:
            trigger_sfx(550.0f, W_NOISE, 0.45f, 140.0f);
            break;
        case BLK_WOOD:
        case BLK_PLANK:
            trigger_sfx(280.0f, W_NOISE, 0.50f,  70.0f);
            trigger_sfx(200.0f, W_SQR,   0.45f, 130.0f);
            break;
        case BLK_LEAVES:
            trigger_sfx(500.0f, W_NOISE, 0.35f,  60.0f);
            trigger_sfx(260.0f, W_TRI,   0.20f,  90.0f);
            break;
        case BLK_GLASS:
            trigger_sfx(1200.0f, W_NOISE, 0.60f,  50.0f);
            trigger_sfx( 900.0f, W_SQR,   0.50f, 200.0f);
            break;
        case BLK_WATER:
            trigger_sfx(180.0f, W_NOISE, 0.30f, 200.0f);
            trigger_sfx( 80.0f, W_TRI,   0.25f, 220.0f);
            break;
        case BLK_TORCH:
            trigger_sfx(260.0f, W_NOISE, 0.40f, 60.0f);
            break;
        default:
            trigger_sfx(220.0f, W_NOISE, 0.45f, 80.0f);
            break;
    }
}

void craft_audio_place(BlockId blk) {
    switch (blk) {
        case BLK_STONE:
        case BLK_COBBLE:
        case BLK_COAL_ORE:
            trigger_sfx(180.0f, W_NOISE, 0.40f,  70.0f);
            trigger_sfx( 90.0f, W_TRI,   0.35f, 100.0f);
            break;
        case BLK_GLASS:
            trigger_sfx(700.0f, W_TRI, 0.45f, 100.0f);
            break;
        case BLK_TORCH:
            trigger_sfx(900.0f, W_TRI, 0.45f,  60.0f);
            trigger_sfx(450.0f, W_TRI, 0.30f, 100.0f);
            break;
        default:
            trigger_sfx(260.0f, W_TRI,   0.45f, 90.0f);
            trigger_sfx(130.0f, W_NOISE, 0.20f, 80.0f);
            break;
    }
}

void craft_audio_pickaxe_ting(void) {
    /* Bright two-tone "ting" played when the player tries to mine
     * a pickaxe-required block barehanded. */
    trigger_sfx(1400.0f, W_TRI, 0.40f,  80.0f);
    trigger_sfx( 950.0f, W_TRI, 0.30f, 120.0f);
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
        s_music.seq_t    = 0.0f;
        s_music.next_idx = 0;
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

/* Trigger a music voice. n_freqs sets how many oscillators run in
 * the chord (1 = single note for melody, 4 = full pad chord).
 * Phases are staggered so the chord doesn't accumulate constructive
 * peaks at sample 0 (which would clip on the loudness boost). */
static void trigger_music_voice(int voice_idx,
                                const float *freqs, int n_freqs,
                                float velocity,
                                float attack_t, float sustain_hold_t,
                                float release_t) {
    Voice *v = &voices[voice_idx];
    if (n_freqs < 1) n_freqs = 1;
    if (n_freqs > MAX_OSC) n_freqs = MAX_OSC;
    /* Phase preservation: if the voice was already active and the
     * oscillator count matches, KEEP the running phase per osc.
     * Resetting phase on a retrigger is what produced the audible
     * "click" between successive melody notes — the sine wave jumped
     * from its current value back to zero. Continuous phase + a new
     * phase_inc gives a smooth pitch transition with no discontinuity. */
    bool same_shape = v->on && v->n_osc == n_freqs;
    v->on        = true;
    v->use_adsr  = true;
    v->wave      = W_SINE;
    v->n_osc     = n_freqs;
    for (int i = 0; i < n_freqs; i++) {
        if (!same_shape) {
            v->phase[i] = (float)i / (float)n_freqs;
        }
        v->phase_inc[i] = freq_to_inc(freqs[i]);
    }
    v->attack_coef  = env_coef_for(attack_t);
    v->release_coef = env_coef_for(release_t);
    v->velocity     = velocity;
    v->sustain_remaining = sustain_hold_t;
    v->env       = ENV_ATTACK;
    /* gain is preserved from previous note for smooth re-trigger. */
}

/* Pick a voice from the music pool to host the next note. Picks an
 * idle voice if one exists, otherwise the voice with the LOWEST
 * gain (i.e. furthest into its release tail and thus least audible
 * to steal). This is what gives the engine polyphony — previous
 * notes keep ringing on their own voices while new notes pick
 * fresh ones, exactly the way a sustain pedal lets piano strings
 * vibrate after a fresh strike. */
static int alloc_music_voice(void) {
    int   best_idx  = MUSIC_VOICE_FIRST;
    float best_score = 1e30f;
    for (int i = MUSIC_VOICE_FIRST; i <= MUSIC_VOICE_LAST; i++) {
        Voice *v = &voices[i];
        /* Idle voice wins instantly. */
        if (!v->on) return i;
        /* Otherwise prefer the voice in release with the smallest
         * gain — stealing it loses the least signal. Voices in
         * attack/sustain are kept; we score them as "loud" so the
         * search prefers releasing voices. */
        float score = (v->env == ENV_RELEASE) ? v->gain : (v->gain + 1.0f);
        if (score < best_score) {
            best_score = score;
            best_idx   = i;
        }
    }
    return best_idx;
}

static void fire_cdl_event(const CDLNote *n) {
    int voice = alloc_music_voice();
    trigger_music_voice(voice,
                        n->hz, (int)n->n_hz,
                        n->vel,
                        n->attack,
                        n->sustain,
                        n->release);
}

void craft_audio_music_tick(float dt) {
    if (!s_music.enabled) {
        s_music.cur_gain *= expf(-3.0f * dt);
        return;
    }
    s_music.t     += dt;
    s_music.seq_t += dt;

    /* Fire every event whose time has arrived. */
    while (s_music.next_idx < CDL_SEQ_LEN &&
           cdl_seq[s_music.next_idx].t <= s_music.seq_t) {
        fire_cdl_event(&cdl_seq[s_music.next_idx]);
        s_music.next_idx++;
    }

    /* Loop the sequence — wrap back to the start once the period ends. */
    if (s_music.seq_t >= CDL_SEQ_PERIOD) {
        s_music.seq_t -= CDL_SEQ_PERIOD;
        s_music.next_idx = 0;
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

    /* Waveform — sine voices may have multiple oscillators (chord
     * voicing); others are always single. */
    float s = 0.0f;
    switch (v->wave) {
        case W_SQR: s = (v->phase[0] < 0.5f) ? 1.0f : -1.0f; break;
        case W_TRI: s = (v->phase[0] < 0.5f)
                         ? (v->phase[0] * 4.0f - 1.0f)
                         : (3.0f - v->phase[0] * 4.0f);
                    break;
        case W_SINE: {
            float sum = 0.0f;
            for (int i = 0; i < v->n_osc; i++) {
                sum += sine_lookup(v->phase[i]);
                v->phase[i] += v->phase_inc[i];
                if (v->phase[i] >= 1.0f) v->phase[i] -= 1.0f;
            }
            /* Already pre-summed; no /N because per-note velocity was
             * sized for chord summing (0.10 × 4 = 0.40 peak). */
            s = sum;
            break;
        }
        case W_NOISE: {
            uint32_t r = xs(&v->noise_state);
            s = ((int32_t)(r & 0xFFFF) - 32768) / 32768.0f;
            break;
        }
    }
    /* Advance phase[0] for non-sine waveforms (sine handles its own). */
    if (v->wave != W_SINE) {
        v->phase[0] += v->phase_inc[0];
        if (v->phase[0] >= 1.0f) v->phase[0] -= 1.0f;
    }
    return s * v->gain;
}

int craft_audio_render(int16_t *out, int n) {
    float mg = s_music.cur_gain;
    for (int i = 0; i < n; i++) {
        /* Music dry mix — sum across the 6-voice pool. Idle voices
         * are still cycled through voice_sample so their release
         * tails keep advancing; voice_sample itself early-outs on
         * v->on == false. */
        float music_dry = 0.0f;
        for (int j = MUSIC_VOICE_FIRST; j <= MUSIC_VOICE_LAST; j++) {
            if (voices[j].on) music_dry += voice_sample(&voices[j]);
        }
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
        mix *= 2.4f;
        mix = mix / sqrtf(1.0f + mix * mix);
        /* Output scale: 32000 ≈ 98% of int16 max. */
        out[i] = (int16_t)(mix * 32000.0f);
    }
    return n;
}
