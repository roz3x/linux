/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _TOOLS_POWERPC_ASM_INST_H
#define _TOOLS_POWERPC_ASM_INST_H

#include <linux/types.h>

#define OP_PREFIX 1

typedef struct {
	u32 val;
	u32 suffix;
} ppc_inst_t;

#define ppc_inst(x) ((ppc_inst_t){ .val = (x) })

static inline u32 ppc_inst_val(ppc_inst_t x)
{
	return x.val;
}

static inline int ppc_inst_primary_opcode(ppc_inst_t x)
{
	return ppc_inst_val(x) >> 26;
}

static inline ppc_inst_t ppc_inst_read(const u32 *ptr)
{
	return ppc_inst(*ptr);
}

static inline bool ppc_inst_prefixed(ppc_inst_t x)
{
	return ppc_inst_primary_opcode(x) == OP_PREFIX;
}

static inline int ppc_inst_len(ppc_inst_t x)
{
	return ppc_inst_prefixed(x) ? 8 : 4;
}

#endif /* _TOOLS_POWERPC_ASM_INST_H */
