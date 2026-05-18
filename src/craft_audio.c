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

/* Eight-chord progression covering the A / B / A' sections of Debussy's
 * "Clair de Lune" (3rd of Suite Bergamasque). Authentic Db major
 * voicings with parsimonious voice-leading — adjacent chords share at
 * least two common tones so the pad transitions glide smoothly rather
 * than jumping.
 *
 *   0  Dbmaj9     Ab3 F4  Ab4 C5     I9 (opening colour)
 *   1  Fm7        C4  F4  Ab4 C5     iii7 parallel shadow
 *   2  Bbm7       Bb3 F4  Ab4 Db5    vi7 wistful turn
 *   3  Ebm9       Eb4 Gb4 Bb4 F5     ii9 floaty
 *   4  Ab7sus4    Ab3 Db4 Ab4 Eb5    V-sus, suspended 3rd
 *   5  Ab7        Ab3 Eb4 Gb4 C5     V7
 *   6  Gbmaj7     Bb3 Gb4 Bb4 F5     IV7 plagal lift
 *   7  Dbmaj7/F   F4  Ab4 C5  F5     I6 return
 *
 * Bass tones (lowest osc) sit at 184-349 Hz — above the drone-buzz
 * zone but rich enough to anchor the 9ths above. */
static const Chord chord_prog[] = {
    { { 207.65f, 349.23f, 415.30f, 523.25f } }, /* 0 Dbmaj9      Ab3 F4 Ab4 C5 */
    { { 261.63f, 349.23f, 415.30f, 523.25f } }, /* 1 Fm7         C4  F4 Ab4 C5 */
    { { 233.08f, 349.23f, 415.30f, 554.37f } }, /* 2 Bbm7        Bb3 F4 Ab4 Db5 */
    { { 311.13f, 369.99f, 466.16f, 698.46f } }, /* 3 Ebm9        Eb4 Gb4 Bb4 F5 */
    { { 207.65f, 277.18f, 415.30f, 622.25f } }, /* 4 Ab7sus4     Ab3 Db4 Ab4 Eb5 */
    { { 207.65f, 311.13f, 369.99f, 523.25f } }, /* 5 Ab7         Ab3 Eb4 Gb4 C5 */
    { { 233.08f, 369.99f, 466.16f, 698.46f } }, /* 6 Gbmaj7      Bb3 Gb4 Bb4 F5 */
    { { 349.23f, 415.30f, 523.25f, 698.46f } }, /* 7 Dbmaj7/F    F4  Ab4 C5 F5  */
};
#define CHORD_COUNT (int)(sizeof(chord_prog) / sizeof(chord_prog[0]))

/* Per-chord OPENING MOTIF — the iconic Clair de Lune right-hand line:
 * descending stepwise figure with one rebound, ending on a held tone.
 * Each motif spans roughly the top of the chord through to the 3rd or
 * 5th, hugging the chord's diatonic neighbours. Frequencies are
 * absolute Hz so transposition is implicit per chord row. */
#define MOTIF_NOTES 7
static const float motif_per_chord[CHORD_COUNT][MOTIF_NOTES] = {
    /* 0  Dbmaj9 — the literal opening, brief's reference pattern. */
    { 830.61f, 698.46f, 554.37f, 622.25f, 698.46f, 554.37f, 415.30f },
    /* 1  Fm7 — sits a step up, mirrors the descent with C6 lead. */
    { 1046.50f, 830.61f, 698.46f, 622.25f, 698.46f, 830.61f, 698.46f },
    /* 2  Bbm7 — wistful descent through chord+9th tones. */
    { 932.33f, 830.61f, 698.46f, 554.37f, 622.25f, 698.46f, 554.37f },
    /* 3  Ebm9 — floating around the 5th + 9th, soft contour. */
    { 698.46f, 622.25f, 554.37f, 466.16f, 622.25f, 554.37f, 466.16f },
    /* 4  Ab7sus — suspended 4th hangs, descent through sus tones. */
    { 554.37f, 466.16f, 415.30f, 622.25f, 554.37f, 415.30f, 554.37f },
    /* 5  Ab7 — dominant tension; the 3rd appears for the resolution. */
    { 622.25f, 554.37f, 466.16f, 415.30f, 466.16f, 554.37f, 523.25f },
    /* 6  Gbmaj7 — bright plagal lift, hovers around Gb-Bb-F. */
    { 932.33f, 739.99f, 698.46f, 622.25f, 698.46f, 739.99f, 698.46f },
    /* 7  Dbmaj7/F — return cadence, lands on Ab5 for the closing glow. */
    { 698.46f, 622.25f, 554.37f, 523.25f, 554.37f, 698.46f, 830.61f },
};

/* Dotted-eighth + sixteenth + eighth rhythmic cell, repeated twice
 * with a quarter resolution at the end. In sixteenth units at the
 * piece's ~60 BPM dotted-quarter: 3-1-2 3-1-2 4 = 16 sixteenths,
 * i.e. one full 9/8 bar. */
static const uint8_t motif_durs_16ths[MOTIF_NOTES] = { 3, 1, 2, 3, 1, 2, 4 };

/* Per-chord APPOGIATURA — a 2-note grace + resolution where the
 * upper neighbour resolves a step down onto a chord tone. The accent
 * is on the dissonant first note; the second is the soft landing. */
static const float appogg_per_chord[CHORD_COUNT][2] = {
    { 739.99f, 698.46f },   /* 0  Gb5→F5  over Dbmaj9 */
    { 932.33f, 830.61f },   /* 1  Bb5→Ab5 over Fm7 */
    { 932.33f, 830.61f },   /* 2  Bb5→Ab5 over Bbm7 */
    { 783.99f, 698.46f },   /* 3  G5→F5   over Ebm9 */
    { 587.33f, 554.37f },   /* 4  D5→Db5  over Ab7sus */
    { 587.33f, 554.37f },   /* 5  D5→Db5  over Ab7 (leading-tone tug) */
    { 830.61f, 739.99f },   /* 6  Ab5→Gb5 over Gbmaj7 */
    { 932.33f, 830.61f },   /* 7  Bb5→Ab5 over Dbmaj7/F */
};

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
#define MELODY_MIN_GAP_DAY    1.2f
#define MELODY_MAX_GAP_DAY    2.8f
#define MELODY_REST_PCT_DAY   0.20f

/* Day mode mix of melody event types (sum to 1.0):
 *   SCALE_RUN — long sequential pentatonic sweep (8-14 notes,
 *               ascending or descending, optionally crossing
 *               octaves). This is the Claire-de-Lune RH figure.
 *   ARPEGGIO  — short broken chord (4-6 notes).
 *   SINGLE    — one held note. */
#define DAY_PCT_SCALE     0.60f
#define DAY_PCT_ARPEGGIO  0.25f
/* Night mode: occasional scale runs for variety, but sparser. */
#define NIGHT_PCT_SCALE   0.15f

#define RUN_MAX_NOTES   16
#define RUN_NOTE_GAP    0.105f   /* 16th-note feel at ~140 BPM — sweepy */
#define ARP_NOTE_GAP    0.140f   /* slightly slower for chord arps */

typedef struct {
    int    n_notes;
    int    next_idx;
    float  next_t;
    float  note_gap;
    float  velocity;
    float  release_t;
    float  freqs[RUN_MAX_NOTES];
} RunState;

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
    RunState run;        /* active scale-run or arpeggio note queue */
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
    s_music.run.n_notes       = 0;
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

/* Short chord arpeggio — 4-6 notes through the chord tones plus an
 * octave-up doubling. Filler between scale runs. */
static void arp_build(RunState *run, const Chord *ch, uint32_t *rng) {
    uint32_t r = xs(rng);
    bool descending = (r & 1);
    bool include_octave = ((r >> 1) & 3) != 0;
    int n = 4;
    if (include_octave) n = (((r >> 3) & 1) ? 5 : 6);
    run->n_notes = n;
    run->next_idx = 0;
    run->next_t = 0.0f;
    run->note_gap = ARP_NOTE_GAP;
    run->velocity = 0.15f;
    run->release_t = 0.80f;
    for (int i = 0; i < 4; i++) {
        int idx = descending ? (3 - i) : i;
        run->freqs[i] = ch->freqs[idx];
    }
    if (n >= 5) run->freqs[4] = ch->freqs[descending ? 0 : 3] * 2.0f;
    if (n >= 6) run->freqs[5] = ch->freqs[descending ? 1 : 2] * 2.0f;
}

/* SCALE RUN — sequential pentatonic sweep on the current chord's
 * pentatonic table. Picks a random start, direction, and length
 * (8-14 notes) and walks in one-step increments, wrapping into the
 * octave above when we run off the top. The pent argument is the
 * 10-entry table for the chord we're currently sitting on. */
static void scale_build(RunState *run, const float *pent, uint32_t *rng) {
    uint32_t r = xs(rng);
    bool descending = (r & 1);
    int len = 8 + (int)((r >> 1) & 7);   /* 8..15 */
    if (len > RUN_MAX_NOTES) len = RUN_MAX_NOTES;
    run->n_notes = len;
    run->next_idx = 0;
    run->next_t = 0.0f;
    run->note_gap = RUN_NOTE_GAP;
    run->velocity = 0.20f;
    run->release_t = 0.55f;
    int max_start = descending ? (PENT_COUNT - 1) : 4;
    int start = (int)((r >> 4) % (uint32_t)(max_start + 1));
    if (descending) start = (PENT_COUNT - 1) - start;
    for (int i = 0; i < len; i++) {
        int step = descending ? -i : i;
        int idx = start + step;
        float oct_mul = 1.0f;
        while (idx >= PENT_COUNT) { idx -= PENT_COUNT; oct_mul *= 2.0f; }
        while (idx < 0)           { idx += PENT_COUNT; oct_mul *= 0.5f; }
        run->freqs[i] = pent[idx] * oct_mul;
    }
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

    /* Drive any active run (scale or arpeggio) — fire next note when
     * its time comes. */
    if (s_music.run.n_notes > 0 && s_music.t >= s_music.run.next_t) {
        int i = s_music.run.next_idx;
        float freqs1[1] = { s_music.run.freqs[i] };
        trigger_music_voice(
            MUSIC_MELODY_VOICE,
            freqs1, 1,
            s_music.run.velocity,
            /* attack */ 0.025f,
            /* sustain hold */ 0.04f,
            /* release */ s_music.run.release_t);
        s_music.run.next_idx++;
        s_music.run.next_t = s_music.t + s_music.run.note_gap;
        if (s_music.run.next_idx >= s_music.run.n_notes) {
            s_music.run.n_notes = 0;   /* sequence done */
            /* Defer next melody event so the sweep's tail rings out. */
            float span = melody_max - melody_min;
            s_music.next_note_t = s_music.t + melody_min + frand01(&s_music.rng) * span;
        }
    }
    else if (s_music.t >= s_music.next_note_t && s_music.run.n_notes == 0) {
        uint32_t r = xs(&s_music.rng);
        bool rest = (r & 0xFFFF) < (uint32_t)(melody_rest * 0x10000);
        if (!rest) {
            /* Pick event kind:
             *   day:   SCALE (60%) → ARP (25%) → SINGLE (15%)
             *   night: SCALE (15%) → SINGLE (85%)  -- no arps
             * Scale runs are the dominant Claire-de-Lune feature so
             * we want them often during the day and as occasional
             * night sparkle. */
            float pick = ((r >> 16) & 0xFFFF) / 65535.0f;
            float p_scale = is_day ? DAY_PCT_SCALE : NIGHT_PCT_SCALE;
            float p_arp   = is_day ? DAY_PCT_ARPEGGIO : 0.0f;
            const float *pent = pent_per_chord[s_music.chord_idx];
            if (pick < p_scale) {
                scale_build(&s_music.run, pent, &s_music.rng);
            } else if (pick < p_scale + p_arp) {
                arp_build(&s_music.run, &chord_prog[s_music.chord_idx],
                          &s_music.rng);
            } else {
                int note = pick_next_melody_note(s_music.last_note, &s_music.rng);
                s_music.last_note = note;
                float hz = pent[note];
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
        if (s_music.run.n_notes == 0) {
            /* Only schedule next event when a run wasn't queued —
             * the run-end path handles its own scheduling above. */
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
        /* Loudness boost + soft-clipper. The previous hard ±1 clamp
         * crackled because the chord+melody+reverb stack often pushes
         * past the |mix|>0.33 threshold after the 3× boost, slicing
         * every transient flat. x / sqrt(1 + x²) is the standard
         * tanh-shaped soft-clip — linear at small signal, saturates
         * smoothly toward ±1 at peaks. One sqrt per sample (~1 % CPU
         * at 22 050 Hz) buys clean output across the whole dynamic
         * range. */
        mix *= 3.0f;
        mix = mix / sqrtf(1.0f + mix * mix);
        /* Output scale: 32000 ≈ 98% of int16 max. */
        out[i] = (int16_t)(mix * 32000.0f);
    }
    return n;
}
