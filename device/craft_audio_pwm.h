/*
 * ThumbyCraft — RP2350 PWM audio driver.
 *
 * Sample rate 22050 Hz (matches CRAFT_AUDIO_RATE), 12-bit PWM DAC on
 * GP23 with the audio amp enable on GP20. IRQ handler pulls one
 * sample per fire from a ring buffer; main loop refills the ring
 * each frame from craft_audio_render().
 */
#ifndef CRAFT_AUDIO_PWM_H
#define CRAFT_AUDIO_PWM_H

#include <stdint.h>

void craft_audio_pwm_init(void);
void craft_audio_pwm_push(const int16_t *samples, int n);
int  craft_audio_pwm_room(void);   /* samples free */

#endif
