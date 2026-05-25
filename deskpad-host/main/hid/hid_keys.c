#include "hid_keys.h"

#include <ctype.h>
#include <stdint.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Token tables

typedef struct {
    const char *name;
    uint8_t     mod_bit;     // for modifier table — single bit
} mod_entry_t;

static const mod_entry_t MODS[] = {
    { "ctrl",   0x01 }, { "lctrl",  0x01 }, { "rctrl",  0x10 },
    { "shift",  0x02 }, { "lshift", 0x02 }, { "rshift", 0x20 },
    { "alt",    0x04 }, { "lalt",   0x04 }, { "ralt",   0x40 },
    { "win",    0x08 }, { "cmd",    0x08 }, { "meta",   0x08 }, { "lwin",   0x08 },
    { "rwin",   0x80 }, { "rcmd",   0x80 }, { "rmeta",  0x80 },
};

typedef struct {
    const char *name;
    uint8_t     code;        // HID usage ID (keyboard page 0x07)
} key_entry_t;

static const key_entry_t KEYS[] = {
    // letters
    { "a", 0x04 }, { "b", 0x05 }, { "c", 0x06 }, { "d", 0x07 }, { "e", 0x08 },
    { "f", 0x09 }, { "g", 0x0a }, { "h", 0x0b }, { "i", 0x0c }, { "j", 0x0d },
    { "k", 0x0e }, { "l", 0x0f }, { "m", 0x10 }, { "n", 0x11 }, { "o", 0x12 },
    { "p", 0x13 }, { "q", 0x14 }, { "r", 0x15 }, { "s", 0x16 }, { "t", 0x17 },
    { "u", 0x18 }, { "v", 0x19 }, { "w", 0x1a }, { "x", 0x1b }, { "y", 0x1c }, { "z", 0x1d },
    // digits (top row)
    { "1", 0x1e }, { "2", 0x1f }, { "3", 0x20 }, { "4", 0x21 }, { "5", 0x22 },
    { "6", 0x23 }, { "7", 0x24 }, { "8", 0x25 }, { "9", 0x26 }, { "0", 0x27 },
    // common named keys
    { "enter",     0x28 }, { "return",   0x28 },
    { "esc",       0x29 }, { "escape",   0x29 },
    { "backspace", 0x2a }, { "bksp",     0x2a },
    { "tab",       0x2b },
    { "space",     0x2c },
    { "minus",     0x2d }, { "-",        0x2d },
    { "equals",    0x2e }, { "=",        0x2e },
    { "lbracket",  0x2f }, { "[",        0x2f },
    { "rbracket",  0x30 }, { "]",        0x30 },
    { "backslash", 0x31 }, { "\\",       0x31 },
    { "semicolon", 0x33 }, { ";",        0x33 },
    { "quote",     0x34 }, { "'",        0x34 },
    { "grave",     0x35 }, { "`",        0x35 },
    { "comma",     0x36 }, { ",",        0x36 },
    { "period",    0x37 }, { ".",        0x37 },
    { "slash",     0x38 }, { "/",        0x38 },
    { "capslock",  0x39 },
    // F1..F12
    { "f1",  0x3a }, { "f2",  0x3b }, { "f3",  0x3c }, { "f4",  0x3d },
    { "f5",  0x3e }, { "f6",  0x3f }, { "f7",  0x40 }, { "f8",  0x41 },
    { "f9",  0x42 }, { "f10", 0x43 }, { "f11", 0x44 }, { "f12", 0x45 },
    // navigation
    { "printscreen", 0x46 }, { "prtsc", 0x46 },
    { "scrolllock",  0x47 },
    { "pause",       0x48 },
    { "insert",      0x49 }, { "ins",   0x49 },
    { "home",        0x4a },
    { "pageup",      0x4b }, { "pgup",  0x4b },
    { "delete",      0x4c }, { "del",   0x4c },
    { "end",         0x4d },
    { "pagedown",    0x4e }, { "pgdn",  0x4e },
    { "right",       0x4f }, { "left",  0x50 },
    { "down",        0x51 }, { "up",    0x52 },
};

typedef struct {
    const char *name;
    uint16_t    code;
} usage_entry_t;

static const usage_entry_t CONSUMER[] = {
    { "PLAY_PAUSE", 0x00CD },
    { "PLAY",       0x00B0 },
    { "PAUSE",      0x00B1 },
    { "STOP",       0x00B7 },
    { "NEXT_TRACK", 0x00B5 },
    { "PREV_TRACK", 0x00B6 },
    { "FAST_FWD",   0x00B3 },
    { "REWIND",     0x00B4 },
    { "VOL_UP",     0x00E9 },
    { "VOL_DOWN",   0x00EA },
    { "MUTE",       0x00E2 },
    { "EJECT",      0x00B8 },
};

static const usage_entry_t SYSTEM[] = {
    { "POWER_DOWN", 0x0081 },
    { "SLEEP",      0x0082 },
    { "WAKE_UP",    0x0083 },
};

// ---------------------------------------------------------------------------
// Helpers

static bool ieq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static uint8_t lookup_mod(const char *tok)
{
    for (size_t i = 0; i < sizeof(MODS)/sizeof(MODS[0]); i++)
        if (ieq(MODS[i].name, tok)) return MODS[i].mod_bit;
    return 0;
}

static uint8_t lookup_key(const char *tok)
{
    for (size_t i = 0; i < sizeof(KEYS)/sizeof(KEYS[0]); i++)
        if (ieq(KEYS[i].name, tok)) return KEYS[i].code;
    return 0;
}

// ---------------------------------------------------------------------------
// API

bool hid_keys_parse_chord(const char *str, uint8_t *modifiers, uint8_t *keycode)
{
    if (!str || !modifiers || !keycode) return false;
    *modifiers = 0;
    *keycode   = 0;

    // Tokenise on '+'. Walk in-place into a local copy.
    char buf[64];
    size_t n = strlen(str);
    if (n >= sizeof(buf)) return false;
    memcpy(buf, str, n + 1);

    char *tok = strtok(buf, "+");
    while (tok) {
        // strip whitespace
        while (*tok && isspace((unsigned char)*tok)) tok++;
        char *end = tok + strlen(tok);
        while (end > tok && isspace((unsigned char)end[-1])) *--end = 0;

        if (*tok) {
            uint8_t m = lookup_mod(tok);
            if (m) {
                *modifiers |= m;
            } else {
                uint8_t k = lookup_key(tok);
                if (k) *keycode = k;
                // unknown tokens are silently ignored — surfaced as "keycode=0"
                // if no recognised key in the chord at all
            }
        }
        tok = strtok(NULL, "+");
    }

    return *keycode != 0;
}

uint16_t hid_keys_consumer_code(const char *name)
{
    if (!name) return 0;
    for (size_t i = 0; i < sizeof(CONSUMER)/sizeof(CONSUMER[0]); i++)
        if (ieq(CONSUMER[i].name, name)) return CONSUMER[i].code;
    return 0;
}

uint16_t hid_keys_system_code(const char *name)
{
    if (!name) return 0;
    for (size_t i = 0; i < sizeof(SYSTEM)/sizeof(SYSTEM[0]); i++)
        if (ieq(SYSTEM[i].name, name)) return SYSTEM[i].code;
    return 0;
}
