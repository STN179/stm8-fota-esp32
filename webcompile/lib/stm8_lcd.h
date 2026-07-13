/* ============================================================================
 *  stm8_lcd.h — bare-metal LCD glass driver for STM8L-DISCOVERY
 *  Public API (call these from your app.c):
 *
 *      void lcd_init(void);
 *      void lcd_clear(void);
 *      void lcd_write_char(unsigned char ch, unsigned char pos);   // pos 0..5
 *      void lcd_puts(const char *str);                             // 6 chars
 *      void lcd_set_bar(unsigned char bar);                        // 4 bars
 *
 *  ch may be A..Z, 0..9, '-' or space.  Anything else is blank.
 * ============================================================================ */
#ifndef STM8_LCD_H
#define STM8_LCD_H

void lcd_init(void);
void lcd_clear(void);
void lcd_write_char(unsigned char ch, unsigned char pos);
void lcd_puts(const char *str);
void lcd_set_bar(unsigned char bar);

#endif
