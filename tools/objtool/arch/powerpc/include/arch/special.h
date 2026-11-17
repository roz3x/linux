/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef _PPC_ARCH_SPECIAL_H
#define _PPC_ARCH_SPECIAL_H

#define EX_ENTRY_SIZE 8
#define EX_ORIG_OFFSET 0
#define EX_NEW_OFFSET 4

#define JUMP_ENTRY_SIZE 16
#define JUMP_ORIG_OFFSET 0
#define JUMP_NEW_OFFSET 4
#define JUMP_KEY_OFFSET 8

#define ALT_ENTRY_SIZE 12
#define ALT_ORIG_OFFSET 0
#define ALT_NEW_OFFSET 4
#define ALT_FEATURE_OFFSET 8
#define ALT_ORIG_LEN_OFFSET 10
#define ALT_NEW_LEN_OFFSET 11

/*
 * Feature fixup entries (__ftr_fixup, __mmu_ftr_fixup, __fw_ftr_fixup), see
 * MAKE_FTR_SECTION_ENTRY() in asm/feature-fixups.h:
 *
 *	word mask, value;
 *	word start, end, alt_start, alt_end;
 *
 * All of them are in units of the ELF word size.  start..end is the original
 * code, alt_start..alt_end its replacement, as offsets from the entry itself.
 */
#define FTR_ENTRY_SIZE		6
#define FTR_ORIG_OFFSET		2
#define FTR_ORIG_END_OFFSET	3
#define FTR_NEW_OFFSET		4
#define FTR_NEW_END_OFFSET	5

#endif /* _PPC_ARCH_SPECIAL_H */
