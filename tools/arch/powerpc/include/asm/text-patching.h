/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _TOOLS_POWERPC_ASM_TEXT_PATCHING_H
#define _TOOLS_POWERPC_ASM_TEXT_PATCHING_H

#include <linux/types.h>
#include <asm/inst.h>

#define BRANCH_SET_LINK	0x1
#define BRANCH_ABSOLUTE	0x2

static inline bool is_offset_in_branch_range(long offset)
{
	return (offset >= -0x2000000 && offset <= 0x1fffffc && !(offset & 0x3));
}

static inline bool is_offset_in_cond_branch_range(long offset)
{
	return offset >= -0x8000 && offset <= 0x7fff && !(offset & 0x3);
}

static inline int create_branch(ppc_inst_t *instr, const u32 *addr,
				unsigned long target, int flags)
{
	long offset;

	*instr = ppc_inst(0);
	offset = target;
	if (! (flags & BRANCH_ABSOLUTE))
		offset = offset - (unsigned long)addr;

	/* Check we can represent the target in the instruction format */
	if (!is_offset_in_branch_range(offset))
		return 1;

	/* Mask out the flags and target, so they don't step on each other. */
	*instr = ppc_inst(0x48000000 | (flags & 0x3) | (offset & 0x03FFFFFC));

	return 0;
}

static inline unsigned int branch_opcode(ppc_inst_t instr)
{
	return ppc_inst_primary_opcode(instr) & 0x3F;
}

static inline int instr_is_branch_iform(ppc_inst_t instr)
{
	return branch_opcode(instr) == 18;
}

static inline int instr_is_branch_bform(ppc_inst_t instr)
{
	return branch_opcode(instr) == 16;
}

int create_cond_branch(ppc_inst_t *instr, const u32 *addr,
		       unsigned long target, int flags);
int instr_is_relative_branch(ppc_inst_t instr);
int instr_is_relative_link_branch(ppc_inst_t instr);
unsigned long branch_target(const u32 *instr);
unsigned long branch_target_at(const u32 *instr, unsigned long pc);
int translate_branch(ppc_inst_t *instr, const u32 *dest, const u32 *src);
int translate_branch_at(ppc_inst_t *instr, unsigned long dest,
			const u32 *src, unsigned long src_addr);
bool is_conditional_branch(ppc_inst_t instr);

#endif /* _TOOLS_POWERPC_ASM_TEXT_PATCHING_H */
