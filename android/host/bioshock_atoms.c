#include "host.h"
#include <strings.h>

typedef struct { uint16_t atom; char *text; } atom_entry;
static atom_entry atoms[256];
static uint16_t next_atom = 0xC000u;

static uint32_t atom_add(const char *text)
{
    if (!text) return 0;
    for (unsigned i = 0; i < 256; ++i)
        if (atoms[i].atom && !strcasecmp(atoms[i].text, text)) return atoms[i].atom;
    for (unsigned i = 0; i < 256; ++i) {
        if (atoms[i].atom) continue;
        char *copy = strdup(text);
        if (!copy || next_atom == 0xFFFFu) { free(copy); return 0; }
        atoms[i].atom = ++next_atom;
        atoms[i].text = copy;
        return atoms[i].atom;
    }
    return 0;
}

WINAPI_EXPORT("kernel32.dll", GlobalAddAtomA, 1)
{
    char *text = void_astr_utf8(ARG(0));
    uint32_t result = atom_add(text);
    free(text);
    return result;
}
WINAPI_EXPORT("kernel32.dll", GlobalAddAtomW, 1)
{
    char *text = void_wstr_utf8(ARG(0));
    uint32_t result = atom_add(text);
    free(text);
    return result;
}
WINAPI_EXPORT("kernel32.dll", GlobalDeleteAtom, 1)
{
    for (unsigned i = 0; i < 256; ++i) {
        if (atoms[i].atom != ARG(0)) continue;
        free(atoms[i].text);
        atoms[i].text = NULL;
        atoms[i].atom = 0;
        return 0;
    }
    return 0;
}
