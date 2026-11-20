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

bool arch_support_alt_relocation(struct objtool_file *file,
				 struct special_alt *special_alt,
				 struct instruction *insn,
				 struct reloc *reloc)
{
	exit(-1);
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
