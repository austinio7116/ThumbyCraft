/*
 * ThumbyCraft — GC9107 LCD driver.
 *
 * 128×128 RGB565 panel, 4-wire SPI on spi0 @ 80 MHz. The init sequence
 * is the panel-mandated GC9107 startup register flow — lifted from
 * ThumbyDOOM/ThumbyNES which lifted it from the published datasheet.
 *
 * Pin map (Thumby Color):
 *   GP18 SCK, GP19 MOSI, GP17 CS, GP16 DC, GP4 RST, GP7 BL
 */
#ifndef CRAFT_LCD_GC9107_H
#define CRAFT_LCD_GC9107_H

#include <stdint.h>

void craft_lcd_init(void);
void craft_lcd_present(const uint16_t *fb_rgb565);
void craft_lcd_wait_idle(void);
void craft_lcd_backlight(int on);

#endif
