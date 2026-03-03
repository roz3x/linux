#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <elf.h>
#include <string.h>

#define MB(x) ((x) * 1024 * 1024ULL)
uint64_t cmask= 0xc000000000000000;

void patch_symtab(uint8_t *buf, Elf64_Ehdr *eh) {
    if (!eh->e_shoff) return;

    Elf64_Shdr *sh = (Elf64_Shdr *)(buf + eh->e_shoff);

    for (int i = 0; i < eh->e_shnum; i++) {
        if (sh[i].sh_type == SHT_SYMTAB) {

            Elf64_Sym *syms = (Elf64_Sym *)(buf + sh[i].sh_offset);
            int count = sh[i].sh_size / sizeof(Elf64_Sym);

            for (int j = 0; j < count; j++) {
                if (syms[j].st_value != 0) {
                	syms[j].st_value &= ~cmask;
                }
            }
        }
    }
}

void die(const char *msg) {
    perror(msg);
    exit(1);
}

int main(int argc, char **argv) {

    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input> <output>\n", argv[0]);
        return 1;
    }

    FILE *f = fopen(argv[1], "rb");
    if (!f) die("open input");

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    rewind(f);

    uint8_t *buf = malloc(size);
    if (!buf) die("malloc");

    if (fread(buf, 1, size, f) != size)
        die("read");

    fclose(f);

    Elf64_Ehdr *eh = (Elf64_Ehdr *)buf;

    patch_symtab(buf, eh);

    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        fprintf(stderr, "Not ELF\n");
        return 1;
    }

    printf("Old entry: 0x%lx\n", eh->e_entry);
    eh->e_entry &= ~(cmask);
    printf("New entry: 0x%lx\n", eh->e_entry);

    Elf64_Phdr *ph = (Elf64_Phdr *)(buf + eh->e_phoff);
    for (int i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD) {
            printf("PH[%d] old vaddr: 0x%lx\n", i, ph[i].p_vaddr);

            ph[i].p_vaddr &= ~cmask;
            ph[i].p_paddr &= ~cmask;

            printf("PH[%d] new vaddr: 0x%lx\n", i, ph[i].p_vaddr);
        }
    }

    if (eh->e_shoff) {
        Elf64_Shdr *sh = (Elf64_Shdr *)(buf + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; i++) {
            if (sh[i].sh_addr != 0)
                sh[i].sh_addr &= ~cmask;
        }
    }

    FILE *out = fopen(argv[2], "wb");
    if (!out) die("open output");

    if (fwrite(buf, 1, size, out) != size)
        die("write");

    fclose(out);
    free(buf);

    printf("Done. Shifted\n");
    system("powerpc64le-linux-gnu-readelf -SW ./vmlinux-shifted");
    return 0;
}
