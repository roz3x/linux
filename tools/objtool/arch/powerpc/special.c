// SPDX-License-Identifier: GPL-2.0-or-later
#include <string.h>
#include <stdlib.h>
#include <objtool/special.h>
#include <objtool/builtin.h>
#include <objtool/warn.h>

/*
 * Feature fixup tables: code between BEGIN_FTR_SECTION and END_FTR_SECTION is
 * overwritten at boot with the code from FTR_SECTION_ELSE, which lives in a
 * separate __ftr_alt_* section.  Each entry holds a mask, a value and four
 * offsets (start, end, alt start, alt end), relative to the entry's address.
 */
static const char * const ftr_sections[] = {
	"__ftr_fixup", "__mmu_ftr_fixup", "__fw_ftr_fixup"
};

/* Where each replacement code is linked and where it is copied to */
struct ftr_alt {
	u64 orig, alt, len;
};

static struct elf *ftr_elf;
static struct ftr_alt *ftr_alts;
static int nr_ftr_alts;

static struct section *find_section_by_addr(struct elf *elf, u64 addr)
{
	struct section *sec;

	for_each_sec(elf, sec) {
		if ((sec->sh.sh_flags & SHF_ALLOC) &&
		    sec->sh.sh_type == SHT_PROGBITS &&
		    addr >= sec->sh.sh_addr &&
		    addr < sec->sh.sh_addr + sec_size(sec))
			return sec;
	}

	return NULL;
}

static int add_ftr_alt(struct list_head *alts, u64 *f)
{
	struct section *orig_sec = find_section_by_addr(ftr_elf, f[0]);
	struct section *new_sec = find_section_by_addr(ftr_elf, f[2]);
	struct special_alt *alt;

	if (!orig_sec || !new_sec) {
		ERROR("feature fixup: no section for 0x%lx or 0x%lx",
		      (unsigned long)f[0], (unsigned long)f[2]);
		return -1;
	}

	alt = calloc(1, sizeof(*alt));
	ftr_alts = realloc(ftr_alts, (nr_ftr_alts + 1) * sizeof(*ftr_alts));
	if (!alt || !ftr_alts) {
		ERROR_GLIBC("alloc");
		return -1;
	}

	ftr_alts[nr_ftr_alts++] = (struct ftr_alt){ f[0], f[2], f[3] - f[2] };

	alt->group = true;
	alt->orig_sec = orig_sec;
	alt->orig_off = f[0] - orig_sec->sh.sh_addr;
	alt->orig_len = f[1] - f[0];
	alt->new_sec = new_sec;
	alt->new_off = f[2] - new_sec->sh.sh_addr;
	alt->new_len = f[3] - f[2];
	list_add_tail(&alt->list, alts);

	return 0;
}

int arch_special_get_alts(struct elf *elf, struct list_head *alts)
{
	bool is64 = elf->ehdr.e_ident[EI_CLASS] == ELFCLASS64;
	size_t i, j, esize = 6 * (is64 ? 8 : 4);

	ftr_elf = elf;

	for (i = 0; i < ARRAY_SIZE(ftr_sections); i++) {
		struct section *sec = find_section_by_name(elf, ftr_sections[i]);
		void *p;

		for (p = sec ? sec->data->d_buf : NULL;
		     sec && p < sec->data->d_buf + sec_size(sec); p += esize) {
			u64 f[6];

			/* f[0..3]: start, end, alt start, alt end addresses */
			for (j = 0; j < 4; j++) {
				s64 off = is64 ? bswap_if_needed(elf, ((s64 *)p)[j + 2]) :
					 (s32)bswap_if_needed(elf, ((u32 *)p)[j + 2]);

				f[j] = sec->sh.sh_addr + (p - sec->data->d_buf) + off;
			}

			/* Nothing to copy, the original is just nopped */
			if (f[2] != f[3] && add_ftr_alt(alts, f))
				return -1;
		}
	}

	return 0;
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
bool arch_support_alt_relocation(struct special_alt *special_alt,
				 struct instruction *insn,
				 struct reloc *reloc)
{
	u64 alt = special_alt->new_sec->sh.sh_addr + special_alt->new_off, orig;
	u64 insn_addr = insn->sec->sh.sh_addr + insn->offset;
	struct symbol *sym = reloc->sym;
	u64 target = sym->sym.st_value + reloc_addend(reloc);
	u32 *word = insn->sec->data->d_buf + insn->offset;
	u32 old = bswap_if_needed(ftr_elf, *word), new;
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

	new = bswap_if_needed(ftr_elf, new);
	return !elf_write_insn(ftr_elf, insn->sec, insn->offset, sizeof(new),
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
