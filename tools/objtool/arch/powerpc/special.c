// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include <stdlib.h>
#include <objtool/special.h>
#include <objtool/builtin.h>
#include <objtool/warn.h>

#include <asm/text-patching.h>
#include <asm/inst.h>

#include "../../../arch/powerpc/lib/code-patching-branch.c"

/*
 * The linker resolved the branch in the replacement code as if it was executed
 * where it's linked, but it's going to be executed where it is copied to;
 * encode the displacement from there.
 */
bool arch_support_alt_relocation(struct objtool_file *file,
				 struct special_alt *special_alt,
				 struct instruction *insn,
				 struct reloc *reloc)
{
	struct elf *elf = file->elf;
	u64 alt = special_alt->new_sec->sh.sh_addr + special_alt->new_off;
	u64 orig = special_alt->orig_sec->sh.sh_addr + special_alt->orig_off;
	u64 insn_addr = insn->sec->sh.sh_addr + insn->offset;
	struct symbol *sym = reloc->sym;
	u64 target = sym->sym.st_value + reloc_addend(reloc);
	u64 dest;
	u32 *word = insn->sec->data->d_buf + insn->offset;
	u32 val = bswap_if_needed(elf, *word);
	ppc_inst_t instr = ppc_inst(val);
	int err;

	/* The linker points direct calls to the local entry point */
	if (is_func_sym(sym))
		target += PPC64_LOCAL_ENTRY_OFFSET(sym->sym.st_other);

	dest = orig + insn_addr - alt;
	if (instr_is_branch_iform(instr))
		err = create_branch(&instr, (const u32 *)dest, target, ppc_inst_val(instr));
	else if (instr_is_branch_bform(instr))
		err = create_cond_branch(&instr, (const u32 *)dest, target, ppc_inst_val(instr));
	else
		err = 1;

	if (err) {
		ERROR_INSN(insn, "alternative branch out of range");
		return false;
	}

	val = bswap_if_needed(elf, ppc_inst_val(instr));
	return !elf_write_insn(elf, insn->sec, insn->offset, sizeof(val),
			       (const char *)&val);
}

struct reloc *arch_find_switch_table(struct objtool_file *file,
				     struct instruction *insn,
				     unsigned long *table_size)
{
	exit(-1);
}

const char *arch_cpu_feature_name(int feature_number)
{
	return NULL;
}
