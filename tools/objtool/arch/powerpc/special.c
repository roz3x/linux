// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include <stdlib.h>
#include <objtool/special.h>
#include <objtool/builtin.h>
#include <objtool/warn.h>

/*
 * Where each replacement code is linked and where it is copied to.  Feature
 * sections can be nested, so this is needed to tell where a replacement
 * finally ends up, see final_addr().
 */
struct ftr_alt {
	u64 orig, alt, len;
};

static struct ftr_alt *ftr_alts;
static int nr_ftr_alts;

void arch_handle_alternative(struct special_alt *alt)
{
	/* Nothing to copy, the original is just nopped */
	if (!alt->new_len)
		return;

	ftr_alts = realloc(ftr_alts, (nr_ftr_alts + 1) * sizeof(*ftr_alts));
	if (!ftr_alts) {
		ERROR_GLIBC("realloc");
		exit(1);
	}

	ftr_alts[nr_ftr_alts++] = (struct ftr_alt){
		alt->orig_sec->sh.sh_addr + alt->orig_off,
		alt->new_sec->sh.sh_addr + alt->new_off,
		alt->new_len,
	};
}

/*
 * Replacement code can contain feature sections itself: that inner code ends
 * up in the outer one's replacement code, which in turn ends up in the
 * original code.  Find where @addr, in or referred to by the replacement code
 * starting at @self, finally is.
 */
static u64 final_addr(u64 self, u64 addr)
{
	int i;

	for (i = 0; i < nr_ftr_alts; i++) {
		struct ftr_alt *a = &ftr_alts[i];

		if (a->alt != self && addr >= a->alt && addr < a->alt + a->len) {
			addr = a->orig + addr - a->alt;
			i = -1;
		}
	}

	return addr;
}

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
	u64 alt = special_alt->new_sec->sh.sh_addr + special_alt->new_off, orig;
	u64 insn_addr = insn->sec->sh.sh_addr + insn->offset;
	struct symbol *sym = reloc->sym;
	u64 target = sym->sym.st_value + reloc_addend(reloc);
	u32 *word = insn->sec->data->d_buf + insn->offset;
	u32 old = bswap_if_needed(elf, *word), new;
	s64 disp;

	/* The linker points direct calls to the local entry point */
	if (is_func_sym(sym))
		target += PPC64_LOCAL_ENTRY_OFFSET(sym->sym.st_other);

	/* Within the replacement code nothing changes by moving it */
	if (target >= alt && target <= alt + special_alt->new_len)
		return true;

	orig = special_alt->orig_sec->sh.sh_addr + special_alt->orig_off;
	disp = final_addr(alt, target) - final_addr(alt, orig + insn_addr - alt);

	if (reloc_type(reloc) == R_PPC_REL24) {
		if (disp < -0x2000000 || disp > 0x1fffffc)
			goto range;
		new = (old & ~0x03fffffc) | (disp & 0x03fffffc);
	} else {
		if (disp < -0x8000 || disp > 0x7ffc)
			goto range;
		new = (old & ~0xfffc) | (disp & 0xfffc);
	}

	new = bswap_if_needed(elf, new);
	return !elf_write_insn(elf, insn->sec, insn->offset, sizeof(new),
			       (const char *)&new);

range:
	ERROR_INSN(insn, "alternative branch out of range");
	return false;
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
