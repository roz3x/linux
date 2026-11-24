// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Copyright 2008 Michael Ellerman, IBM Corporation.
 */

#include <asm/text-patching.h>
#include <asm/inst.h>

/*
 * Helper to check if a given instruction is a conditional branch
 * Derived from the conditional checks in analyse_instr()
 */
bool is_conditional_branch(ppc_inst_t instr)
{
	unsigned int opcode = ppc_inst_primary_opcode(instr);

	if (opcode == 16)       /* bc, bca, bcl, bcla */
		return true;
	if (opcode == 19) {
		switch ((ppc_inst_val(instr) >> 1) & 0x3ff) {
		case 16:        /* bclr, bclrl */
		case 528:       /* bcctr, bcctrl */
		case 560:       /* bctar, bctarl */
			return true;
		}
	}
	return false;
}

int create_cond_branch(ppc_inst_t *instr, const u32 *addr,
		       unsigned long target, int flags)
{
	long offset;

	offset = target;
	if (! (flags & BRANCH_ABSOLUTE))
		offset = offset - (unsigned long)addr;

	/* Check we can represent the target in the instruction format */
	if (!is_offset_in_cond_branch_range(offset))
		return 1;

	/* Mask out the flags and target, so they don't step on each other. */
	*instr = ppc_inst(0x40000000 | (flags & 0x3FF0003) | (offset & 0xFFFC));

	return 0;
}

int instr_is_relative_branch(ppc_inst_t instr)
{
	if (ppc_inst_val(instr) & BRANCH_ABSOLUTE)
		return 0;

	return instr_is_branch_iform(instr) || instr_is_branch_bform(instr);
}

int instr_is_relative_link_branch(ppc_inst_t instr)
{
	return instr_is_relative_branch(instr) && (ppc_inst_val(instr) & BRANCH_SET_LINK);
}

static unsigned long branch_iform_target(const u32 *instr, unsigned long pc)
{
	signed long imm;

	imm = ppc_inst_val(ppc_inst_read(instr)) & 0x3FFFFFC;

	/* If the top bit of the immediate value is set this is negative */
	if (imm & 0x2000000)
		imm -= 0x4000000;

	if ((ppc_inst_val(ppc_inst_read(instr)) & BRANCH_ABSOLUTE) == 0)
		imm += pc;

	return (unsigned long)imm;
}

static unsigned long branch_bform_target(const u32 *instr, unsigned long pc)
{
	signed long imm;

	imm = ppc_inst_val(ppc_inst_read(instr)) & 0xFFFC;

	/* If the top bit of the immediate value is set this is negative */
	if (imm & 0x8000)
		imm -= 0x10000;

	if ((ppc_inst_val(ppc_inst_read(instr)) & BRANCH_ABSOLUTE) == 0)
		imm += pc;

	return (unsigned long)imm;
}

unsigned long branch_target_at(const u32 *instr, unsigned long pc)
{
	if (instr_is_branch_iform(ppc_inst_read(instr)))
		return branch_iform_target(instr, pc);
	else if (instr_is_branch_bform(ppc_inst_read(instr)))
		return branch_bform_target(instr, pc);

	return 0;
}

unsigned long branch_target(const u32 *instr)
{
	return branch_target_at(instr, (unsigned long)instr);
}

int translate_branch_at(ppc_inst_t *instr, unsigned long dest,
			const u32 *src, unsigned long src_addr)
{
	unsigned long target;
	target = branch_target_at(src, src_addr);

	if (instr_is_branch_iform(ppc_inst_read(src)))
		return create_branch(instr, (const u32 *)dest, target,
				     ppc_inst_val(ppc_inst_read(src)));
	else if (instr_is_branch_bform(ppc_inst_read(src)))
		return create_cond_branch(instr, (const u32 *)dest, target,
					  ppc_inst_val(ppc_inst_read(src)));

	return 1;
}

int translate_branch(ppc_inst_t *instr, const u32 *dest, const u32 *src)
{
	return translate_branch_at(instr, (unsigned long)dest, src, (unsigned long)src);
}
