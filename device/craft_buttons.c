/*
 * ThumbyCraft — Thumby Color button reader.
 *
 * No diagonal coalescing or trigger-as-corner mapping here —
 * ThumbyCraft uses LB/RB as proper distinct buttons (strafe), and
 * an FPS-style game can't tolerate sticky direction smear.
 */
#include "craft_buttons.h"

#include "pico/stdlib.h"
#include "hardware/gpio.h"

#define BTN_LEFT_GP   0
#define BTN_UP_GP     1
#define BTN_RIGHT_GP  2
#define BTN_DOWN_GP   3
#define BTN_LB_GP     6
#define BTN_A_GP     21
#define BTN_RB_GP    22
#define BTN_B_GP     25
#define BTN_MENU_GP  26

static void init_pull_up(uint pin) {
    gpio_init(pin);
    gpio_set_dir(pin, GPIO_IN);
    gpio_pull_up(pin);
}

void craft_buttons_init(void) {
    init_pull_up(BTN_LEFT_GP);
    init_pull_up(BTN_UP_GP);
    init_pull_up(BTN_RIGHT_GP);
    init_pull_up(BTN_DOWN_GP);
    init_pull_up(BTN_LB_GP);
    init_pull_up(BTN_A_GP);
    init_pull_up(BTN_RB_GP);
    init_pull_up(BTN_B_GP);
    init_pull_up(BTN_MENU_GP);
}

void craft_buttons_read(CraftRawButtons *out) {
    out->left  = !gpio_get(BTN_LEFT_GP);
    out->right = !gpio_get(BTN_RIGHT_GP);
    out->up    = !gpio_get(BTN_UP_GP);
    out->down  = !gpio_get(BTN_DOWN_GP);
    out->a     = !gpio_get(BTN_A_GP);
    out->b     = !gpio_get(BTN_B_GP);
    out->lb    = !gpio_get(BTN_LB_GP);
    out->rb    = !gpio_get(BTN_RB_GP);
    out->menu  = !gpio_get(BTN_MENU_GP);
}
