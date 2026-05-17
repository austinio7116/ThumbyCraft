/*
 * ThumbyCraft — Thumby Color button reader.
 *
 * GPIO map (active low, internal pull-ups):
 *   GP0  LEFT     GP1  UP       GP2  RIGHT     GP3  DOWN
 *   GP21 A        GP25 B        GP6  LB        GP22 RB
 *   GP26 MENU
 *
 * Returns the full set of raw buttons as a struct (no PICO-8 mask
 * here — the game wants A/B/LB/RB/MENU distinguished).
 */
#ifndef CRAFT_BUTTONS_H
#define CRAFT_BUTTONS_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    bool up, down, left, right;
    bool a, b, lb, rb, menu;
} CraftRawButtons;

void craft_buttons_init(void);
void craft_buttons_read(CraftRawButtons *out);

#endif
