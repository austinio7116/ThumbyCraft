#!/usr/bin/env python3
"""Convert the Clair de Lune MIDI into a C CDLNote[] array.

Strategy:
- Group simultaneous notes (within 50ms window) into chord events.
- For each event, split by pitch: LH = notes < C4 (MIDI 60), RH = >=.
- Emit one CDLNote per voice that fires, with up to 4 simultaneous Hz.
- Duration = median note duration in the group.
- Velocity = average velocity in the group, normalised to ~0.10-0.18.
"""
import os
import mido
from statistics import median, mean
from collections import defaultdict

# The source MIDI lives alongside this script. Regenerate with:
#   cd tools && python3 cdl_to_c.py > ../src/craft_audio_cdl_data.h.body
# then wrap with the include guard (see existing header).
MID = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                   "debussy-clair-de-lune.mid")
mid = mido.MidiFile(MID)
TPB = mid.ticks_per_beat

# Global tempo factor. The MIDI plays at ~80 BPM dotted-quarter; the
# real Andante très expressif marking is closer to 50-55. Stretching
# everything by 1.3× gives a more contemplative pace without dragging.
TIME_STRETCH = 1.3

# Collect notes
notes = []
for ti, track in enumerate(mid.tracks):
    abs_sec = 0.0
    local_tempo = 500000
    active = {}
    for msg in track:
        delta_sec = mido.tick2second(msg.time, TPB, local_tempo)
        abs_sec += delta_sec
        if msg.type == 'set_tempo':
            local_tempo = msg.tempo
        elif msg.type == 'note_on' and msg.velocity > 0:
            active[(msg.channel, msg.note)] = (abs_sec, msg.velocity)
        elif msg.type == 'note_off' or (msg.type == 'note_on' and msg.velocity == 0):
            k = (msg.channel, msg.note)
            if k in active:
                t0, v = active.pop(k)
                notes.append((t0, abs_sec - t0, msg.note, v))

# Apply global time stretch — slows the piece down while preserving
# the relative rhythm exactly.
notes = [(t * TIME_STRETCH, d * TIME_STRETCH, n, v) for (t, d, n, v) in notes]
notes.sort()
print(f"// Source: {MID} — {len(notes)} notes parsed")

# Group by start time — tight window so "rolled chord" arrivals (notes
# played 10-30 ms apart) stay separated and the original rhythm is
# preserved. The old 50 ms window was collapsing rolled chords into
# simultaneous stabs and the user noticed.
EPS = 0.015
groups = []
i = 0
while i < len(notes):
    t0 = notes[i][0]
    g = []
    while i < len(notes) and notes[i][0] - t0 < EPS:
        g.append(notes[i])
        i += 1
    groups.append(g)

print(f"// {len(groups)} simultaneous groups")

# Convert to (t, voice, freqs[], dur, vel)
LH_THRESHOLD = 60  # MIDI note number — C4. Anything below this is LH.

def hz(n):
    return 440.0 * (2 ** ((n - 69) / 12.0))

# The Thumby Color's PWM speaker rolls off heavily below ~280 Hz.
# Notes pitched lower than this aren't physically reproducible at
# audible volume. Octave-shift them upward until they land in the
# speaker's passband. Chord interval relationships are preserved
# because every chord tone gets the same shift if it falls below
# the threshold, and crucially MORE THAN ONE octave shift can apply
# for very low notes (e.g. Db2 → Db3 → Db4).
SPEAKER_HZ_THRESHOLD = 280.0
def speaker_safe_hz(h):
    while h < SPEAKER_HZ_THRESHOLD:
        h *= 2.0
    return h

events = []
for g in groups:
    t_start = g[0][0]
    lh_notes = sorted([n for n in g if n[2] < LH_THRESHOLD], key=lambda n: n[2])
    rh_notes = sorted([n for n in g if n[2] >= LH_THRESHOLD], key=lambda n: n[2])

    for voice_idx, group_notes in [(0, lh_notes), (1, rh_notes)]:
        if not group_notes:
            continue
        # Cap to 4 simultaneous notes per event
        group_notes = group_notes[:4]
        freqs = [hz(n[2]) for n in group_notes]
        dur = float(median(n[1] for n in group_notes))
        vel_midi = mean(n[3] for n in group_notes)
        # Normalise MIDI velocity to 0.05-0.22 range. The post-mix
        # soft-clipper has been removed in favour of a simple linear
        # sum + hard clamp, so we can run hotter without IMD; the
        # ceiling is bounded by how often the hard clamp activates.
        vel = 0.05 + (vel_midi / 127.0) * 0.17
        events.append((t_start, voice_idx, freqs, dur, vel))

# Sort by time
events.sort(key=lambda e: e[0])

# Emit C struct entries
print(f"\n// {len(events)} note events (LH + RH split)")
print(f"// Total piece duration: {notes[-1][0] + notes[-1][1]:.2f} sec\n")

print("static const CDLNote cdl_seq[] = {")
for (t, v, freqs, dur, vel) in events:
    # Pad freqs to 4
    pad = freqs + [0.0] * (4 - len(freqs))
    f_str = ", ".join(f"{f:8.2f}f" for f in pad)
    # Piano-like envelope. Attack is fast (hammer strike), sustain
    # spans the note's gate-on time, then a long release tail to
    # mimic sustain-pedal decay. Releases pushed up to 4-7 s so notes
    # ring out across subsequent triggers via the reverb tail rather
    # than dying immediately.
    if dur < 0.30:
        attack = 0.015
        sustain = max(dur - 0.04, 0.02)
        release = 3.50
    elif dur < 1.0:
        attack = 0.030
        sustain = dur - 0.05
        release = 5.00
    else:
        attack = 0.050
        sustain = dur - 0.08
        release = 7.00
    print(f"    {{ {t:7.3f}f, {{ {f_str} }}, {len(freqs)}, {v}, "
          f"{vel:.3f}f, {attack:.3f}f, {sustain:.3f}f, {release:.3f}f }},")
print("};")
print(f"#define CDL_SEQ_LEN     {len(events)}")
total_dur = notes[-1][0] + notes[-1][1]
print(f"#define CDL_SEQ_PERIOD  {total_dur:.2f}f")

# --- Sections -------------------------------------------------------
# Cut on left-hand (voice=0) onset gaps: when the bass disappears for
# at least LH_GAP_THRESHOLD seconds, that marks a harmonic phrase
# boundary. RH events that sit inside such a gap are included in the
# section that PRECEDES the gap — they're the RH-solo tail of that
# phrase (e.g. Debussy's tranquille middle interlude).
LH_GAP_THRESHOLD = 2.0

lh_idx_t = [(i, e[0]) for i, e in enumerate(events) if e[1] == 0]
boundaries = [0]
for k in range(1, len(lh_idx_t)):
    prev_t = lh_idx_t[k-1][1]
    cur_i, cur_t = lh_idx_t[k]
    if cur_t - prev_t >= LH_GAP_THRESHOLD:
        boundaries.append(cur_i)
boundaries.append(len(events))

sections = []
for k in range(len(boundaries) - 1):
    a = boundaries[k]
    b = boundaries[k+1]
    sections.append((a, b, events[a][0], events[b-1][0]))

print(f"\n// {len(sections)} sections (cut on LH-onset gaps ≥ {LH_GAP_THRESHOLD}s)")
print("static const CDLSection cdl_sections[] = {")
for (a, b, t0, t1) in sections:
    print(f"    {{ {a:4d}, {b:4d}, {t0:7.3f}f, {t1:7.3f}f }},")
print("};")
print(f"#define CDL_SECTION_COUNT {len(sections)}")
