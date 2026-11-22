/* kernel/drivers/keyboard.c
 * LumaOS 5.0 - PS/2 Keyboard Driver
 *
 * STAGE1-KB changes:
 *   KB-1: Added E0 extended-key prefix handling.
 *         Modern PS/2 keyboards send E0 xx for arrow keys, Insert,
 *         Delete, Home, End, PgUp, PgDn, and the right Ctrl/Alt keys.
 *         The old driver only handled the numeric-keypad aliases (0x48
 *         etc.) which work on some BIOS setups but not in QEMU with a
 *         modern keyboard model.
 *   KB-2: Right Ctrl and Right Alt now also set ctrl_held / alt_held.
 *   KB-3: Num-lock state tracked; keypad keys produce digits when
 *         Num-Lock is on, navigation codes when off.
 *   KB-4: Ctrl+C / Ctrl+L produce dedicated codes KEY_CTRL_C /
 *         KEY_CTRL_L so the shell can act on them.
 *   KB-5: keyboard_clear_buffer() is now interrupt-safe via pushf/cli.
 */

#include "../core/kernel.h"
#include "keyboard.h"

/* ===== Ports ===== */
#define KB_DATA  0x60
#define KB_STAT  0x64

/* ===== Ring buffer ===== */
#define KB_BUF_SIZE 256
static unsigned char    kb_buf[KB_BUF_SIZE];
static volatile int     kb_head  = 0;
static volatile int     kb_tail  = 0;
static volatile int     kb_count = 0;

/* ===== Modifier state ===== */
static volatile int shift_held = 0;
static volatile int ctrl_held  = 0;
static volatile int ctrl_c_flag = 0;     /* DAY56: 有程序在跑时按下了 Ctrl+C（由 irq_handler 取走并终止程序） */
extern uint32_t elf_running;
static volatile int alt_held   = 0;
static volatile int caps_lock  = 0;
static volatile int num_lock   = 1;   /* Num-Lock on by default */
static volatile int e0_prefix  = 0;   /* KB-1: received 0xE0 last byte */

/* ===== Make-code constants ===== */
#define SC_LSHIFT  0x2A
#define SC_RSHIFT  0x36
#define SC_LCTRL   0x1D   /* also E0 1D = Right Ctrl */
#define SC_LALT    0x38   /* also E0 38 = Right Alt  */
#define SC_CAPS    0x3A
#define SC_NUMLOCK 0x45
#define SC_F1      0x3B
#define SC_F2      0x3C
#define SC_F3      0x3D
#define SC_F4      0x3E
#define SC_F5      0x3F
#define SC_F6      0x40
#define SC_F7      0x41
#define SC_F8      0x42
#define SC_F9      0x43
#define SC_F10     0x44
#define SC_F11     0x57
#define SC_F12     0x58

/* Numeric-keypad codes (sent WITHOUT E0 prefix) */
#define SC_KP7     0x47   /* Home  / 7 */
#define SC_KP8     0x48   /* Up    / 8 */
#define SC_KP9     0x49   /* PgUp  / 9 */
#define SC_KP4     0x4B   /* Left  / 4 */
#define SC_KP5     0x4C   /* 5 */
#define SC_KP6     0x4D   /* Right / 6 */
#define SC_KP1     0x4F   /* End   / 1 */
#define SC_KP2     0x50   /* Down  / 2 */
#define SC_KP3     0x51   /* PgDn  / 3 */
#define SC_KP0     0x52   /* Ins   / 0 */
#define SC_KPDOT   0x53   /* Del   / . */

/* Extended (E0-prefixed) navigation codes */
#define SC_E0_UP    0x48
#define SC_E0_DOWN  0x50
#define SC_E0_LEFT  0x4B
#define SC_E0_RIGHT 0x4D
#define SC_E0_HOME  0x47
#define SC_E0_END   0x4F
#define SC_E0_PGUP  0x49
#define SC_E0_PGDN  0x51
#define SC_E0_INS   0x52
#define SC_E0_DEL   0x53

/* ===== Keymap: scancode → ASCII (no shift) ===== */
static const unsigned char keymap_normal[128] = {
/*00*/ 0,    0x1B,'1', '2', '3', '4', '5', '6',
/*08*/ '7', '8', '9', '0', '-', '=', '\b','\t',
/*10*/ 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i',
/*18*/ 'o', 'p', '[', ']', '\r',0,   'a', 's',
/*20*/ 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';',
/*28*/ '\'','`', 0,   '\\','z', 'x', 'c', 'v',
/*30*/ 'b', 'n', 'm', ',', '.', '/', 0,   '*',
/*38*/ 0,   ' ', 0,   0,   0,   0,   0,   0,
/*40*/ 0,   0,   0,   0,   0,   0,   0,   '7',
/*48*/ '8', '9', '-', '4', '5', '6', '+', '1',
/*50*/ '2', '3', '0', '.', 0,   0,   0,   0,
/*58*/ 0,   0,   0,   0,   0,   0,   0,   0,
/*60*/ 0,   0,   0,   0,   0,   0,   0,   0,
/*68*/ 0,   0,   0,   0,   0,   0,   0,   0,
/*70*/ 0,   0,   0,   0,   0,   0,   0,   0,
/*78*/ 0,   0,   0,   0,   0,   0,   0,   0,
};

/* ===== Keymap: scancode → ASCII (shift) ===== */
static const unsigned char keymap_shift[128] = {
/*00*/ 0,    0x1B,'!', '@', '#', '$', '%', '^',
/*08*/ '&', '*', '(', ')', '_', '+', '\b','\t',
/*10*/ 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I',
/*18*/ 'O', 'P', '{', '}', '\r',0,   'A', 'S',
/*20*/ 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':',
/*28*/ '"', '~', 0,   '|', 'Z', 'X', 'C', 'V',
/*30*/ 'B', 'N', 'M', '<', '>', '?', 0,   '*',
/*38*/ 0,   ' ', 0,   0,   0,   0,   0,   0,
/*40*/ 0,   0,   0,   0,   0,   0,   0,   '7',
/*48*/ '8', '9', '-', '4', '5', '6', '+', '1',
/*50*/ '2', '3', '0', '.', 0,   0,   0,   0,
/*58*/ 0,   0,   0,   0,   0,   0,   0,   0,
/*60*/ 0,   0,   0,   0,   0,   0,   0,   0,
/*68*/ 0,   0,   0,   0,   0,   0,   0,   0,
/*70*/ 0,   0,   0,   0,   0,   0,   0,   0,
/*78*/ 0,   0,   0,   0,   0,   0,   0,   0,
};

/* ===== Buffer helpers ===== */
static void buf_push(unsigned char key)
{
    if (kb_count >= KB_BUF_SIZE) return;
    kb_buf[kb_head] = key;
    kb_head = (kb_head + 1) % KB_BUF_SIZE;
    kb_count++;
}

/* ===== IRQ1 handler ===== */
void keyboard_irq_handler(void)
{
    if (!(inb(KB_STAT) & 0x01)) return;
    unsigned char sc = inb(KB_DATA);

    /* KB-1: Catch E0 prefix byte */
    if (sc == 0xE0) {
        e0_prefix = 1;
        return;
    }
    /* Ignore E1 (Pause/Break multi-byte sequence) */
    if (sc == 0xE1) {
        e0_prefix = 0;
        return;
    }

    int release = (sc & 0x80);
    unsigned char make = sc & 0x7F;

    /* ---- E0-prefixed extended keys ---- */
    if (e0_prefix) {
        e0_prefix = 0;
        if (release) {
            /* Release of extended modifier */
            if (make == SC_LCTRL) ctrl_held = 0;
            if (make == SC_LALT)  alt_held  = 0;
            return;
        }
        /* Extended modifier make */
        if (make == SC_LCTRL) { ctrl_held = 1; return; }
        if (make == SC_LALT)  { alt_held  = 1; return; }

        /* Navigation keys (E0-prefixed) */
        switch (make) {
        case SC_E0_UP:    buf_push(KEY_UP);    return;
        case SC_E0_DOWN:  buf_push(KEY_DOWN);  return;
        case SC_E0_LEFT:  buf_push(KEY_LEFT);  return;
        case SC_E0_RIGHT: buf_push(KEY_RIGHT); return;
        case SC_E0_HOME:  buf_push(KEY_HOME);  return;
        case SC_E0_END:   buf_push(KEY_END);   return;
        case SC_E0_PGUP:  buf_push(KEY_PGUP);  return;
        case SC_E0_PGDN:  buf_push(KEY_PGDN);  return;
        case SC_E0_INS:   buf_push(KEY_INS);   return;
        case SC_E0_DEL:   buf_push(KEY_DEL);   return;
        default:          return;   /* ignore unknown extended */
        }
    }

    /* ---- Normal (non-E0) keys ---- */
    if (release) {
        if (make == SC_LSHIFT || make == SC_RSHIFT) shift_held = 0;
        if (make == SC_LCTRL)                       ctrl_held  = 0;
        if (make == SC_LALT)                        alt_held   = 0;
        return;
    }

    /* Modifiers & toggles */
    switch (make) {
    case SC_LSHIFT: case SC_RSHIFT: shift_held = 1; return;
    case SC_LCTRL:                  ctrl_held  = 1; return;
    case SC_LALT:                   alt_held   = 1; return;
    case SC_CAPS:    caps_lock = !caps_lock;         return;
    case SC_NUMLOCK: num_lock  = !num_lock;          return;

    /* Function keys */
    case SC_F1:  buf_push(KEY_F1);  return;
    case SC_F2:  buf_push(KEY_F2);  return;
    case SC_F3:  buf_push(KEY_F3);  return;
    case SC_F4:  buf_push(KEY_F4);  return;
    case SC_F5:  buf_push(KEY_F5);  return;
    case SC_F6:  buf_push(KEY_F6);  return;
    case SC_F7:  buf_push(KEY_F7);  return;
    case SC_F8:  buf_push(KEY_F8);  return;
    case SC_F9:  buf_push(KEY_F9);  return;
    case SC_F10: buf_push(KEY_F10); return;
    case SC_F11: buf_push(KEY_F11); return;
    case SC_F12: buf_push(KEY_F12); return;

    /* Numeric keypad — depends on Num-Lock */
    case SC_KP8: buf_push(num_lock ? '8' : KEY_UP);    return;
    case SC_KP2: buf_push(num_lock ? '2' : KEY_DOWN);  return;
    case SC_KP4: buf_push(num_lock ? '4' : KEY_LEFT);  return;
    case SC_KP6: buf_push(num_lock ? '6' : KEY_RIGHT); return;
    case SC_KP7: buf_push(num_lock ? '7' : KEY_HOME);  return;
    case SC_KP1: buf_push(num_lock ? '1' : KEY_END);   return;
    case SC_KP9: buf_push(num_lock ? '9' : KEY_PGUP);  return;
    case SC_KP3: buf_push(num_lock ? '3' : KEY_PGDN);  return;
    case SC_KP0: buf_push(num_lock ? '0' : KEY_INS);   return;
    case SC_KPDOT: buf_push(num_lock ? '.' : KEY_DEL); return;

    default: break;
    }

    /* Printable character path */
    if (make >= 128) return;

    /* KB-4: Ctrl combos */
    if (ctrl_held) {
        unsigned char base = keymap_normal[make];
        if (base == 'c') { if (elf_running) { ctrl_c_flag = 1; return; } buf_push(KEY_CTRL_C); return; }
        if (base == 'l') { buf_push(KEY_CTRL_L); return; }
        if (base == 'a') { buf_push(KEY_CTRL_A); return; }
        if (base == 'e') { buf_push(KEY_CTRL_E); return; }
        if (base == 'u') { buf_push(KEY_CTRL_U); return; }
        if (base == 'k') { buf_push(KEY_CTRL_K); return; }
        /* Other Ctrl+key: generate ASCII control code (e.g. Ctrl+A = 0x01) */
        if (base >= 'a' && base <= 'z') {
            buf_push((unsigned char)(base - 'a' + 1));
            return;
        }
        return;
    }

    /* Normal ASCII */
    int effective_shift = shift_held;
    if (caps_lock) {
        unsigned char base = keymap_normal[make];
        if (base >= 'a' && base <= 'z') effective_shift = !effective_shift;
    }
    unsigned char ascii = effective_shift ? keymap_shift[make] : keymap_normal[make];
    if (ascii) buf_push(ascii);
}

/* ===== Public API ===== */
void keyboard_init(void)
{
    kb_head = 0; kb_tail = 0; kb_count = 0;
    shift_held = 0; ctrl_held = 0; alt_held = 0;
    caps_lock  = 0; num_lock  = 1; e0_prefix = 0;
    /* Flush stale bytes */
    while (inb(KB_STAT) & 0x01) inb(KB_DATA);
    pic_enable_irq(1);
}

/* Blocking read */
char keyboard_getchar(void)
{
    while (kb_count == 0) asm volatile ("pause");
    unsigned long fl;
    asm volatile ("pushf; pop %0; cli" : "=r"(fl));
    unsigned char key = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    kb_count--;
    if (fl & 0x200) asm volatile ("sti");
    return (char)key;
}

/* Non-blocking read */
unsigned char keyboard_read(void)
{
    if (kb_count == 0) return 0;
    unsigned long fl;
    asm volatile ("pushf; pop %0; cli" : "=r"(fl));
    unsigned char key = kb_buf[kb_tail];
    kb_tail = (kb_tail + 1) % KB_BUF_SIZE;
    kb_count--;
    if (fl & 0x200) asm volatile ("sti");
    return key;
}

int keyboard_available(void) { return kb_count; }

/* KB-5: interrupt-safe clear */
void keyboard_clear_buffer(void)
{
    unsigned long fl;
    asm volatile ("pushf; pop %0; cli" : "=r"(fl));
    kb_head = 0; kb_tail = 0; kb_count = 0;
    if (fl & 0x200) asm volatile ("sti");
}

/* Modifier queries */
int keyboard_shift_held(void) { return shift_held; }
int keyboard_ctrl_held(void)  { return ctrl_held;  }
int keyboard_alt_held(void)   { return alt_held;   }
int keyboard_caps_lock(void)  { return caps_lock;  }
int keyboard_num_lock(void)   { return num_lock;   }

/* DAY56: irq_handler 在键盘中断后调用：刚才是不是有程序在跑时按了 Ctrl+C？（读后清除） */
int keyboard_take_ctrl_c(void)
{
    int f = ctrl_c_flag;
    ctrl_c_flag = 0;
    return f;
}
