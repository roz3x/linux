// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2015 Josh Poimboeuf <jpoimboe@redhat.com>
 */

/*
 * This file reads all the special sections which have alternate instructions
 * which can be patched in or redirected to at runtime.
 */

#include <stdlib.h>
#include <string.h>

#include <arch/special.h>
#include <objtool/builtin.h>
#include <objtool/special.h>
#include <objtool/warn.h>

struct special_entry {
	const char *sec;
	bool group, jump_or_nop;
	unsigned char size, orig, new;
	unsigned char orig_len, new_len; /* group only */
	unsigned char feature; /* ALTERNATIVE macro CPU feature */
	unsigned char key; /* jump_label key */

	/*
	 * --alt-fixup entries, read by nothing else (and the other way
	 * around).  size, orig and new are in units of the ELF word size, and
	 * are relative to the start of the entry rather than to the field
	 * itself.  The code is delimited by where it ends, not by its length.
	 */
	bool fixup;
	unsigned char orig_end, new_end;
};

/* powerpc feature fixups, see arch/powerpc/include/asm/feature-fixups.h */
#define FTR_FIXUP_ENTRY(name)			\
	{					\
		.sec = name,			\
		.group = true,			\
		.size = FTR_ENTRY_SIZE,		\
		.orig = FTR_ORIG_OFFSET,	\
		.new = FTR_NEW_OFFSET,		\
		.fixup = true,			\
		.orig_end = FTR_ORIG_END_OFFSET,\
		.new_end = FTR_NEW_END_OFFSET,	\
	}

static const struct special_entry entries[] = {
	{
		.sec = ".altinstructions",
		.group = true,
		.size = ALT_ENTRY_SIZE,
		.orig = ALT_ORIG_OFFSET,
		.orig_len = ALT_ORIG_LEN_OFFSET,
		.new = ALT_NEW_OFFSET,
		.new_len = ALT_NEW_LEN_OFFSET,
		.feature = ALT_FEATURE_OFFSET,
	},
	{
		.sec = "__jump_table",
		.jump_or_nop = true,
		.size = JUMP_ENTRY_SIZE,
		.orig = JUMP_ORIG_OFFSET,
		.new = JUMP_NEW_OFFSET,
		.key = JUMP_KEY_OFFSET,
	},
	{
		.sec = "__ex_table",
		.size = EX_ENTRY_SIZE,
		.orig = EX_ORIG_OFFSET,
		.new = EX_NEW_OFFSET,
	},
#ifdef FTR_ENTRY_SIZE
	FTR_FIXUP_ENTRY("__ftr_fixup"),
	FTR_FIXUP_ENTRY("__mmu_ftr_fixup"),
	FTR_FIXUP_ENTRY("__fw_ftr_fixup"),
#endif
	{},
};

void __weak arch_handle_alternative(struct special_alt *alt)
{
}

static void reloc_to_sec_off(struct reloc *reloc, struct section **sec,
			     unsigned long *off)
{
	*sec = reloc->sym->sec;
	*off = reloc->sym->offset + reloc_addend(reloc);
}

static int get_alt_entry(struct elf *elf, const struct special_entry *entry,
			 struct section *sec, int idx,
			 struct special_alt *alt)
{
	struct reloc *orig_reloc, *new_reloc;
	unsigned long offset;

	offset = idx * entry->size;

	alt->group = entry->group;
	alt->jump_or_nop = entry->jump_or_nop;

	if (alt->group) {
		alt->orig_len = *(unsigned char *)(sec->data->d_buf + offset +
						   entry->orig_len);
		alt->new_len = *(unsigned char *)(sec->data->d_buf + offset +
						  entry->new_len);
		alt->feature = *(unsigned int *)(sec->data->d_buf + offset +
						 entry->feature);
	}

	orig_reloc = find_reloc_by_dest(elf, sec, offset + entry->orig);
	if (!orig_reloc) {
		ERROR_FUNC(sec, offset + entry->orig, "can't find orig reloc");
		return -1;
	}

	reloc_to_sec_off(orig_reloc, &alt->orig_sec, &alt->orig_off);

	arch_handle_alternative(alt);

	if (!entry->group || alt->new_len) {
		new_reloc = find_reloc_by_dest(elf, sec, offset + entry->new);
		if (!new_reloc) {
			ERROR_FUNC(sec, offset + entry->new, "can't find new reloc");
			return -1;
		}

		reloc_to_sec_off(new_reloc, &alt->new_sec, &alt->new_off);

		/* _ASM_EXTABLE_EX hack */
		if (alt->new_off >= 0x7ffffff0)
			alt->new_off -= 0x7ffffff0;
	}

	if (entry->key) {
		struct reloc *key_reloc;

		key_reloc = find_reloc_by_dest(elf, sec, offset + entry->key);
		if (!key_reloc) {
			ERROR_FUNC(sec, offset + entry->key, "can't find key reloc");
			return -1;
		}
		alt->key_addend = reloc_addend(key_reloc);
	}

	return 0;
}

/*
 * Section and offset of the code at the reloc at @field of the --alt-fixup
 * entry at @entry_off.  The reloc's addend is relative to the entry rather
 * than to the field, take the field's offset in the entry out of it.
 */
static int get_fixup_sec_off(struct elf *elf, struct section *sec,
			     unsigned long entry_off, unsigned long field,
			     struct section **code_sec, unsigned long *off)
{
	struct reloc *reloc = find_reloc_by_dest(elf, sec, entry_off + field);

	if (!reloc) {
		ERROR_FUNC(sec, entry_off + field, "can't find reloc");
		return -1;
	}

	reloc_to_sec_off(reloc, code_sec, off);
	*off -= field;

	return 0;
}

/* Like get_alt_entry(), for entries which have the end of the code, not its length */
static int get_fixup_entry(struct elf *elf, const struct special_entry *entry,
			   struct section *sec, int idx,
			   struct special_alt *alt)
{
	size_t w = elf_addr_size(elf);
	unsigned long offset = idx * entry->size * w;
	struct section *end_sec;
	unsigned long end_off;

	alt->group = entry->group;

	if (get_fixup_sec_off(elf, sec, offset, entry->orig * w,
			      &alt->orig_sec, &alt->orig_off) ||
	    get_fixup_sec_off(elf, sec, offset, entry->orig_end * w,
			      &end_sec, &end_off) ||
	    end_sec != alt->orig_sec || end_off < alt->orig_off) {
		ERROR_FUNC(sec, offset, "invalid original code");
		return -1;
	}
	alt->orig_len = end_off - alt->orig_off;

	if (get_fixup_sec_off(elf, sec, offset, entry->new * w,
			      &alt->new_sec, &alt->new_off) ||
	    get_fixup_sec_off(elf, sec, offset, entry->new_end * w,
			      &end_sec, &end_off) ||
	    end_sec != alt->new_sec || end_off < alt->new_off) {
		ERROR_FUNC(sec, offset, "invalid replacement code");
		return -1;
	}
	alt->new_len = end_off - alt->new_off;

	arch_handle_alternative(alt);

	return 0;
}

/*
 * Read all the special sections and create a list of special_alt structs which
 * describe all the alternate instructions which can be patched in or
 * redirected to at runtime.
 */
int special_get_alts(struct elf *elf, struct list_head *alts)
{
	const struct special_entry *entry;
	struct section *sec;
	unsigned int nr_entries, size;
	struct special_alt *alt;
	int idx;

	INIT_LIST_HEAD(alts);

	for (entry = entries; entry->sec; entry++) {
		if (entry->fixup != opts.alt_fixup)
			continue;

		sec = find_section_by_name(elf, entry->sec);
		if (!sec)
			continue;

		size = entry->size;
		if (entry->fixup)
			size *= elf_addr_size(elf);

		if (sec_size(sec) % size != 0) {
			ERROR("%s size not a multiple of %d", sec->name, size);
			return -1;
		}

		nr_entries = sec_size(sec) / size;

		for (idx = 0; idx < nr_entries; idx++) {
			alt = malloc(sizeof(*alt));
			if (!alt) {
				ERROR_GLIBC("malloc failed");
				return -1;
			}
			memset(alt, 0, sizeof(*alt));

			if (entry->fixup ? get_fixup_entry(elf, entry, sec, idx, alt) :
					   get_alt_entry(elf, entry, sec, idx, alt))
				return -1;

			/*
			 * Nothing to copy and fix up if the original is just
			 * nopped.  Such an entry can overlap other ones.
			 */
			if (entry->fixup && !alt->new_len) {
				free(alt);
				continue;
			}

			list_add_tail(&alt->list, alts);
		}
	}

	return 0;
}
