/* kernel/drivers/keyboard.h
 * LumaOS 5.0 - Keyboard driver public interface
 *
 * STAGE1-KB: Added KEY_CTRL_* codes for common Ctrl combinations.
 */
#ifndef LUMA_KEYBOARD_H
#define LUMA_KEYBOARD_H

/* ===== Special key codes (returned by keyboard_read) ===== */

/* Navigation */
#define KEY_UP    0x80
#define KEY_DOWN  0x81
#define KEY_LEFT  0x82
#define KEY_RIGHT 0x83

/* Function keys */
#define KEY_F1    0x84
#define KEY_F2    0x85
#define KEY_F3    0x86
#define KEY_F4    0x87
#define KEY_F5    0x88
#define KEY_F6    0x89
#define KEY_F7    0x8A
#define KEY_F8    0x8B
#define KEY_F9    0x8C
#define KEY_F10   0x8D
#define KEY_F11   0x8E
#define KEY_F12   0x8F

/* Extended navigation */
#define KEY_HOME  0x90
#define KEY_END   0x91
#define KEY_PGUP  0x92
#define KEY_PGDN  0x93
#define KEY_DEL   0x94
#define KEY_INS   0x95

/* ASCII control codes (standard) */
#define KEY_ESC   0x1B
#define KEY_TAB   0x09
#define KEY_ENTER 0x0D
#define KEY_BS    0x08

/* Ctrl combos (KB-4) */
#define KEY_CTRL_C  0x03   /* Ctrl+C  (ASCII ETX) */
#define KEY_CTRL_L  0x0C   /* Ctrl+L  (ASCII FF  — clear screen) */
#define KEY_CTRL_A  0x01   /* Ctrl+A  (ASCII SOH — beginning of line) */
#define KEY_CTRL_E  0x05   /* Ctrl+E  (ASCII ENQ — end of line) */
#define KEY_CTRL_U  0x15   /* Ctrl+U  (ASCII NAK — kill line) */
#define KEY_CTRL_K  0x0B   /* Ctrl+K  (ASCII VT  — kill to end) */
#define KEY_CTRL_S  0x13   /* Ctrl+S  (ASCII DC3 — DAY25: save & exit the
                              * `write` Shell command's interactive
                              * multi-line editor, kernel/shell/cmd/
                              * write.c. Reaches keyboard_getchar()/
                              * keyboard_read() the same way KEY_CTRL_C
                              * etc. already do — through the generic
                              * "Ctrl + letter" fallback branch in
                              * keyboard.c's scancode handler
                              * (`'s' - 'a' + 1 == 0x13`) — this is just
                              * the first caller to need a NAMED constant
                              * for that particular combination rather
                              * than writing the literal 0x13 inline. */

/* ===== Public API ===== */
void          keyboard_init(void);
char          keyboard_getchar(void);    /* blocking  */
unsigned char keyboard_read(void);       /* non-blocking, 0 = empty */
int           keyboard_available(void);
void          keyboard_clear_buffer(void);
void          keyboard_irq_handler(void);
int           keyboard_take_ctrl_c(void);   /* 1 once if Ctrl+C was pressed while a program runs (clears the flag) */

/* Modifier state */
int keyboard_shift_held(void);
int keyboard_ctrl_held(void);
int keyboard_alt_held(void);
int keyboard_caps_lock(void);
int keyboard_num_lock(void);

#endif /* LUMA_KEYBOARD_H */
