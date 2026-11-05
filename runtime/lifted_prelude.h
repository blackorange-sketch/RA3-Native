// Included at the top of generated C files.
#pragma once
#include "cpu.h"
#include "fpu.h"

#define SAVE() do { c->r[0] = eax; c->r[1] = ecx; c->r[2] = edx; c->r[3] = ebx; c->r[4] = esp; c->r[5] = ebp; c->r[6] = esi; c->r[7] = edi; c->df = df; } while (0)
#define LOAD() do { eax = c->r[0]; ecx = c->r[1]; edx = c->r[2]; ebx = c->r[3]; esp = c->r[4]; ebp = c->r[5]; esi = c->r[6]; edi = c->r[7]; df = (uint8_t)c->df; } while (0)
