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
#define REVERB_WET    0.28f
#define REVERB_FEED   0.22f   /* reduced — sustained chords no longer build a mud halo */

static float freq_to_inc(float hz) {
    return hz / (float)CRAFT_AUDIO_RATE;
}

/* --- Music state -------------------------------------------------- */

/* A chord is a 4-note voicing in the mid register. Keeping the bass
 * note above ~190 Hz avoids the "drone buzz" the old C3 root had —
 * fundamentals that low were clipping under the 3× loudness boost. */
typedef struct {
    float freqs[4];
} Chord;

/* Six-chord modal progression centred on F major / D minor.
 * Voicings move with shared tones between adjacent chords so each
 * change feels like a single voice gliding rather than a hard jump:
 *
 *   Fmaj9   F4  A4  C5  E5   (root colour — bright open)
 *   Am9     A3  E4  G4  B4   (shares E with prev; adds 9th gloss)
 *   Dm9     D4  F4  A4  E5   (shares F A; classic Debussy descent)
 *   Bbmaj7  Bb3 D4  F4  A4   (shares D F A; warm lift)
 *   Csus4   C4  F4  G4  C5   (open suspension; pivots key)
 *   Fmaj7add6 F4 A4 D5 E5    (return — adds 6th for resolution glow)
 *
 * Bass notes (lowest oscillator) sit at 196–349 Hz — well above the
 * drone-buzz zone. */
static const Chord chord_prog[] = {
    { { 349.23f, 440.00f, 523.25f, 659.26f } }, /* Fmaj9    F4 A4 C5 E5 */
    { { 220.00f, 329.63f, 392.00f, 493.88f } }, /* Am9      A3 E4 G4 B4 */
    { { 293.66f, 349.23f, 440.00f, 659.26f } }, /* Dm9      D4 F4 A4 E5 */
    { { 233.08f, 293.66f, 349.23f, 440.00f } }, /* Bbmaj7   Bb3 D4 F4 A4 */
    { { 261.63f, 349.23f, 392.00f, 523.25f } }, /* Csus4    C4 F4 G4 C5 */
    { { 349.23f, 440.00f, 587.33f, 659.26f } }, /* Fmaj7add6 F4 A4 D5 E5 */
};
#define CHORD_COUNT (int)(sizeof(chord_prog) / sizeof(chord_prog[0]))

/* Melody scale — F major pentatonic spanning the upper octave so
 * notes float clearly above the pad. */
static const float pent_freqs[10] = {
    349.23f, 392.00f, 440.00f, 523.25f, 587.33f,
    698.46f, 783.99f, 880.00f, 1046.50f, 1174.66f,
    /* F4   G4      A4      C5      D5
       F5   G5      A5      C6      D6 */
};
#define PENT_COUNT 10

/* Two pacing modes — switched on the world's sun position.
 *
 * NIGHT (sun_y < 0): slow, sparse, "shifting clouds" texture.
 * DAY   (sun_y > 0): faster, more melodic — adds arpeggiated runs
 *                    through the current chord like Claire-de-Lune's
 *                    right-hand figures. Chord changes are also more
 *                    frequent so the harmony breathes with motion. */
#define CHORD_DURATION_NIGHT 20.0f
#define CHORD_DURATION_DAY   12.0f
#define MELODY_MIN_GAP_NIGHT  2.5f
#define MELODY_MAX_GAP_NIGHT  5.5f
#define MELODY_REST_PCT_NIGHT 0.55f
#define MELODY_MIN_GAP_DAY    0.9f
#define MELODY_MAX_GAP_DAY    2.2f
#define MELODY_REST_PCT_DAY   0.30f
/* Day mode: 55% of melody events kick off an arpeggio rather than
 * a single note. */
#define ARPEGGIO_PCT_DAY      0.55f
#define ARPEGGIO_NOTE_GAP     0.125f   /* sec between arp notes — 16th at 120 BPM feel */
#define ARPEGGIO_MAX_NOTES    8

typedef struct {
    int    n_notes;
    int    next_idx;
    float  next_t;
    float  freqs[ARPEGGIO_MAX_NOTES];
} ArpState;

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
    float  sun_y;        /* >0 day, <0 night. Set by host each tick. */
    ArpState arp;
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
    s_music.rng               = 0xA110F00Du;
    s_music.target_gain       = 0.70f;
    s_music.cur_gain          = 0.0f;
    s_music.last_note         = 3;
    s_music.next_note_t       = 1.0f;
    s_music.last_chord_played = -1;
    s_music.sun_y             = 1.0f;   /* assume day until world sets it */
    s_music.arp.n_notes       = 0;
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
    v->on        = true;
    v->use_adsr  = true;
    v->wave      = W_SINE;
    if (n_freqs < 1) n_freqs = 1;
    if (n_freqs > MAX_OSC) n_freqs = MAX_OSC;
    v->n_osc = n_freqs;
    for (int i = 0; i < n_freqs; i++) {
        v->phase[i]     = (float)i / (float)n_freqs;  /* staggered start */
        v->phase_inc[i] = freq_to_inc(freqs[i]);
    }
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

/* Build an ascending or descending arpeggio sequence from the
 * current chord's pitches plus an octave-up doubling for that
 * sparkling Debussy lift. */
static void arp_build(ArpState *arp, const Chord *ch, uint32_t *rng) {
    uint32_t r = xs(rng);
    bool descending = (r & 1);
    bool include_octave = ((r >> 1) & 3) != 0;   /* 75% include octave */
    int n = 4;
    if (include_octave) n = (((r >> 3) & 1) ? 5 : 6);  /* 5 or 6 notes */
    arp->n_notes = n;
    arp->next_idx = 0;
    arp->next_t = 0.0f;   /* fire immediately */
    for (int i = 0; i < 4; i++) {
        int idx = descending ? (3 - i) : i;
        arp->freqs[i] = ch->freqs[idx];
    }
    /* Extra notes: re-fire top one octave up (Claire-de-Lune-style
     * "and one more on top" gesture). */
    if (n >= 5) arp->freqs[4] = ch->freqs[descending ? 0 : 3] * 2.0f;
    if (n >= 6) arp->freqs[5] = ch->freqs[descending ? 1 : 2] * 2.0f;
}

void craft_audio_music_tick(float dt) {
    if (!s_music.enabled) {
        s_music.cur_gain *= expf(-3.0f * dt);
        return;
    }
    s_music.t += dt;
    s_music.chord_t += dt;

    /* Day/night switch — picks the timing constants for this tick. */
    bool is_day = s_music.sun_y > 0.0f;
    float chord_duration = is_day ? CHORD_DURATION_DAY : CHORD_DURATION_NIGHT;
    float melody_min     = is_day ? MELODY_MIN_GAP_DAY  : MELODY_MIN_GAP_NIGHT;
    float melody_max     = is_day ? MELODY_MAX_GAP_DAY  : MELODY_MAX_GAP_NIGHT;
    float melody_rest    = is_day ? MELODY_REST_PCT_DAY : MELODY_REST_PCT_NIGHT;

    if (s_music.chord_t >= chord_duration) {
        s_music.chord_t = 0;
        s_music.chord_idx = (s_music.chord_idx + 1) % CHORD_COUNT;
    }
    if (s_music.last_chord_played != s_music.chord_idx) {
        s_music.last_chord_played = s_music.chord_idx;
        /* Pad: full 4-note chord through a single envelope. Slow
         * attack (~3 s) so the chord blooms rather than punches.
         * Per-note velocity is low because four oscillators sum;
         * the 3× output boost brings it back up cleanly. */
        trigger_music_voice(
            MUSIC_PAD_VOICE,
            chord_prog[s_music.chord_idx].freqs, 4,
            /* per-note velocity */ 0.10f,
            /* attack */ is_day ? 1.6f : 3.0f,
            /* sustain hold */ chord_duration - 2.0f,
            /* release */ is_day ? 3.0f : 4.5f);
    }

    /* Drive any active arpeggio — fire next note when its time comes. */
    if (s_music.arp.n_notes > 0 && s_music.t >= s_music.arp.next_t) {
        int i = s_music.arp.next_idx;
        float freqs1[1] = { s_music.arp.freqs[i] };
        trigger_music_voice(
            MUSIC_MELODY_VOICE,
            freqs1, 1,
            /* velocity */ 0.14f,
            /* attack */ 0.03f,
            /* sustain hold */ 0.06f,
            /* release */ 0.7f);
        s_music.arp.next_idx++;
        s_music.arp.next_t = s_music.t + ARPEGGIO_NOTE_GAP;
        if (s_music.arp.next_idx >= s_music.arp.n_notes) {
            s_music.arp.n_notes = 0;   /* sequence done */
            /* Don't immediately stack another note — defer next
             * melody event by at least the arp's natural tail. */
            float span = melody_max - melody_min;
            s_music.next_note_t = s_music.t + melody_min + frand01(&s_music.rng) * span;
        }
    }
    else if (s_music.t >= s_music.next_note_t && s_music.arp.n_notes == 0) {
        uint32_t r = xs(&s_music.rng);
        bool rest = (r & 0xFFFF) < (uint32_t)(melody_rest * 0x10000);
        if (!rest) {
            /* Day mode: chance to fire an arpeggio instead of a
             * single note. Night mode: always single sustained
             * notes for the contemplative feel. */
            bool arp = is_day &&
                       (((r >> 16) & 0xFFFF) < (uint32_t)(ARPEGGIO_PCT_DAY * 0x10000));
            if (arp) {
                arp_build(&s_music.arp, &chord_prog[s_music.chord_idx],
                          &s_music.rng);
            } else {
                int note = pick_next_melody_note(s_music.last_note, &s_music.rng);
                s_music.last_note = note;
                float hz = pent_freqs[note];
                /* Sustained single note — long release so it bleeds
                 * into the next note like sustain-pedal piano. */
                float freqs1[1] = { hz };
                trigger_music_voice(
                    MUSIC_MELODY_VOICE,
                    freqs1, 1,
                    /* velocity */ 0.16f,
                    /* attack */ 0.08f,
                    /* sustain hold */ is_day ? 0.18f : 0.30f,
                    /* release */ is_day ? 1.2f : 2.2f);
            }
        }
        if (s_music.arp.n_notes == 0) {
            /* Only schedule next event when an arp wasn't queued —
             * the arp-end path handles its own scheduling above. */
            float span = melody_max - melody_min;
            s_music.next_note_t = s_music.t + melody_min + frand01(&s_music.rng) * span;
        }
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
        /* Loudness boost — pre-clamp gain pushes quiet sections up
         * to use the full output range. Peaks just hit the clamping
         * wall (effectively a hard limiter), which is cleaner than
         * running everything at low volume on the device speaker.
         * 3.0× brings perceived volume up substantially while keeping
         * distortion confined to brief transients. */
        mix *= 3.0f;
        if (mix >  1.0f) mix =  1.0f;
        if (mix < -1.0f) mix = -1.0f;
        /* Output scale: 32000 ≈ 98% of int16 max. */
        out[i] = (int16_t)(mix * 32000.0f);
    }
    return n;
}
