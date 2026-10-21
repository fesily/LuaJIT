/*
** Implementation of symbol table for profilers.
**
** Major portions taken verbatim or adapted from the LuaVela.
** Copyright (C) 2015-2019 IPONWEB Ltd.
*/

#define lj_symtab_c
#define LUA_CORE

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "lj_symtab.h"

#include "lj_jit.h"
#include "lj_bc.h"

#include <stdlib.h>

#if LJ_HASRESOLVER

#include <linux/limits.h>
#include <elf.h>
#include <link.h>
#include <stdio.h>
#include <sys/auxv.h>
#include <unistd.h>
#include "lj_gc.h"
#endif

#if LJ_TARGET_WINDOWS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include "lj_vm.h"
#include <stdio.h>
#include <stdlib.h>
#endif

static const unsigned char ljs_header[] = {'l', 'j', 's', LJS_CURRENT_VERSION,
                                          0x0, 0x0, 0x0};

#if LJ_HASJIT

/*
** Map every snapshot of a trace to the bytecode it was taken at.
**
** The exit stubs of a trace are emitted per snapshot and carry the bytecode
** PC of that point (see lj_asm.c), and the snapshot stores it right after
** its entries: lib_jit's jit.util.traceinfo(tr, sn, true) uses the same trick
** to report a position inside the function. Walking back from the PC to the
** function header bytecode and up to the prototype gives the function, and
** lj_debug_line() gives the line.
**
** The exit snapshot that covers an address is the last one whose mcode
** offset is not greater than the address, exactly like lj_trace_unwind().
*/
int lj_symtab_trace_intervals(const GCtrace *trace, lj_symtab_trace_iv **ivp)
{
  const SnapShot *snap = trace->snap;
  const SnapEntry *map = trace->snapmap;
  lj_symtab_trace_iv *iv;
  SnapNo i;
  int n = 0;

  *ivp = NULL;
  if (trace->nsnap == 0 || snap == NULL || map == NULL)
    return 0;
  iv = (lj_symtab_trace_iv *)malloc(sizeof(*iv) * trace->nsnap);
  if (iv == NULL)
    return 0;
  for (i = 0; i < trace->nsnap; i++) {
    const BCIns *pc = snap_pc((SnapEntry *)&map[snap[i].mapofs + snap[i].nent]);
    const BCIns *startpc = pc;
    GCproto *pt;
    /*
    ** One entry per snapshot, in snapshot order: the index is the exit
    ** number, which is what an address inside an exit stub gives (the stub
    ** only carries the exit number, see lj_asm_x86.h). Snapshots may share an
    ** offset, the address lookup takes the last one at or below the address.
    */
    iv[i].off = (uint32_t)snap[i].mcofs * sizeof(MCode);
    while (bc_op(*startpc) < BC_FUNCF)
      startpc--;  /* Back to the function header bytecode. */
    pt = (GCproto *)((char *)startpc - sizeof(GCproto));
    iv[i].line = lj_debug_line(pt, (BCLine)(pc - startpc));
    iv[i].pt = pt;
    n++;
  }
  if (n == 0) {
    free(iv);
    return 0;
  }
  *ivp = iv;
  return n;
}

void lj_symtab_dump_trace(struct lj_wbuf *out, const GCtrace *trace,
			  const lj_symtab_trace_iv *iv, int niv)
{
  GCproto *pt = &gcref(trace->startpt)->pt;
  BCLine lineno = 0;
  int i;

  const BCIns *startpc = mref(trace->startpc, const BCIns);
  lj_assertX(startpc >= proto_bc(pt) && startpc < proto_bc(pt) + pt->sizebc,
	     "start trace PC out of range");

  lineno = lj_debug_line(pt, proto_bcpos(pt, startpc));

  lj_wbuf_addu64(out, (uint64_t)trace->traceno);
  /*
  ** The information about the prototype, associated with the
  ** trace's start has already been dumped, as it is anchored
  ** via the trace and is not collected while the trace is alive.
  ** For this reason, we do not need to repeat dumping the chunk
  ** name for the prototype.
  */
  lj_wbuf_addu64(out, (uintptr_t)pt);
  lj_wbuf_addu64(out, (uint64_t)lineno);
  /* The machine code range makes a host frame inside JIT code resolvable. */
  lj_wbuf_addu64(out, (uintptr_t)trace->mcode);
  lj_wbuf_addu64(out, (uint64_t)trace->szmcode);
  /* The intervals refine the range to function + line (version 6+). */
  lj_wbuf_addu64(out, (uint64_t)(niv > 0 ? niv : 0));
  for (i = 0; i < niv; i++) {
    lj_wbuf_addu64(out, (uint64_t)iv[i].off);
    lj_wbuf_addu64(out, (uintptr_t)iv[i].pt);
    lj_wbuf_addu64(out, (uint64_t)iv[i].line);
  }
}

#endif /* LJ_HASJIT */

void lj_symtab_dump_proto(struct lj_wbuf *out, const GCproto *pt)
{
  lj_wbuf_addu64(out, (uintptr_t)pt);
  lj_wbuf_addstring(out, proto_chunknamestr(pt));
  lj_wbuf_addu64(out, (uint64_t)pt->firstline);
}

#if LJ_HASRESOLVER

struct ghashtab_header {
  uint32_t nbuckets;
  uint32_t symoffset;
  uint32_t bloom_size;
  uint32_t bloom_shift;
};

static uint32_t ghashtab_size(ElfW(Addr) ghashtab)
{
  /*
  ** There is no easy way to get count of symbols in GNU hashtable, so the
  ** only way to do this is to take highest possible non-empty bucket and
  ** iterate through its symbols until the last chain is over.
  */
  uint32_t last_entry = 0;

  const uint32_t *chain = NULL;
  struct ghashtab_header *header = (struct ghashtab_header *)ghashtab;
  /*
  ** sizeof(size_t) returns 8, if compiled with 64-bit compiler, and 4 if
  ** compiled with 32-bit compiler. It is the best option to determine which
  ** kind of CPU we are running on.
  */
  const char *buckets = (char *)ghashtab + sizeof(struct ghashtab_header) +
                        sizeof(size_t) * header->bloom_size;

  uint32_t *cur_bucket = (uint32_t *)buckets;
  uint32_t i;
  for (i = 0; i < header->nbuckets; ++i) {
    if (last_entry < *cur_bucket)
      last_entry = *cur_bucket;
    cur_bucket++;
  }

  if (last_entry < header->symoffset)
    return header->symoffset;

  chain = (uint32_t *)(buckets + sizeof(uint32_t) * header->nbuckets);
  /* The chain ends with the lowest bit set to 1. */
  while (!(chain[last_entry - header->symoffset] & 1))
    last_entry++;

  return ++last_entry;
}

static void write_c_symtab(ElfW(Sym *) sym, char *strtab, ElfW(Addr) so_addr,
			   size_t sym_cnt, const uint8_t header,
			   struct lj_wbuf *buf)
{
  /*
  ** Index 0 in ELF symtab is used to represent undefined symbols. Hence, we
  ** can just start with index 1.
  **
  ** For more information, see:
  ** https://docs.oracle.com/cd/E23824_01/html/819-0690/chapter6-79797.html
  */

  ElfW(Word) sym_index;
  for (sym_index = 1; sym_index < sym_cnt; sym_index++) {
    /*
    ** ELF32_ST_TYPE and ELF64_ST_TYPE are the same, so we can use
    ** ELF32_ST_TYPE for both 64-bit and 32-bit ELFs.
    **
    ** For more, see https://github.com/torvalds/linux/blob/9137eda53752ef73148e42b0d7640a00f1bc96b1/include/uapi/linux/elf.h#L135
    */
    if (ELF32_ST_TYPE(sym[sym_index].st_info) == STT_FUNC &&
        sym[sym_index].st_name != 0) {
      char *sym_name = &strtab[sym[sym_index].st_name];
      lj_wbuf_addbyte(buf, header);
      lj_wbuf_addu64(buf, sym[sym_index].st_value + so_addr);
      lj_wbuf_addstring(buf, sym_name);
    }
  }
}

static int dump_sht_symtab(const char *elf_name, struct lj_wbuf *buf,
			   lua_State *L, const uint8_t header,
			   const ElfW(Addr) so_addr)
{
  int status = 0;

  char *strtab = NULL;
  ElfW(Shdr *) section_headers = NULL;
  ElfW(Sym *) sym = NULL;
  ElfW(Ehdr) elf_header = {};

  ElfW(Off) sym_off = 0;
  ElfW(Off) strtab_off = 0;

  size_t sym_cnt = 0;
  size_t strtab_size = 0;
  size_t header_index = 0;

  size_t shoff = 0; /* Section headers offset. */
  size_t shnum = 0; /* Section headers number. */
  size_t shentsize = 0; /* Section header entry size. */

  FILE *elf_file = fopen(elf_name, "rb");

  if (elf_file == NULL)
    return -1;

  if (fread(&elf_header, sizeof(elf_header), 1, elf_file) != sizeof(elf_header)
      && ferror(elf_file) != 0)
    goto error;
  if (memcmp(elf_header.e_ident, ELFMAG, SELFMAG) != 0)
    /* Not a valid ELF file. */
    goto error;

  shoff = elf_header.e_shoff;
  shnum = elf_header.e_shnum;
  shentsize = elf_header.e_shentsize;

  if (shoff == 0 || shnum == 0 || shentsize == 0)
    /* No sections in ELF. */
    goto error;

  /*
  ** Memory occupied by section headers is unlikely to be more than 160B, but
  ** 32-bit and 64-bit ELF files may have sections of different sizes and some
  ** of the sections may duiplicate, so we need to take that into account.
  */
  section_headers = lj_mem_new(L, shnum * shentsize);
  if (section_headers == NULL)
    goto error;

  if (fseek(elf_file, shoff, SEEK_SET) != 0)
    goto error;

  if (fread(section_headers, shentsize, shnum, elf_file) != shentsize * shnum
      && ferror(elf_file) != 0)
    goto error;

  for (header_index = 0; header_index < shnum; ++header_index) {
    if (section_headers[header_index].sh_type == SHT_SYMTAB) {
      ElfW(Shdr) sym_hdr = section_headers[header_index];
      ElfW(Shdr) strtab_hdr = section_headers[sym_hdr.sh_link];
      size_t symtab_size = sym_hdr.sh_size;

      sym_off = sym_hdr.sh_offset;
      sym_cnt = symtab_size / sym_hdr.sh_entsize;

      strtab_off = strtab_hdr.sh_offset;
      strtab_size = strtab_hdr.sh_size;
      break;
    }
  }

  if (sym_off == 0 || strtab_off == 0 || sym_cnt == 0)
    goto error;

  /* Load symtab into memory. */
  sym = lj_mem_new(L, sym_cnt * sizeof(ElfW(Sym)));
  if (sym == NULL)
    goto error;
  if (fseek(elf_file, sym_off, SEEK_SET) != 0)
    goto error;
  if (fread(sym, sizeof(ElfW(Sym)), sym_cnt, elf_file) !=
      sizeof(ElfW(Sym)) * sym_cnt && ferror(elf_file) != 0)
    goto error;


  /* Load strtab into memory. */
  strtab = lj_mem_new(L, strtab_size * sizeof(char));
  if (strtab == NULL)
    goto error;
  if (fseek(elf_file, strtab_off, SEEK_SET) != 0)
    goto error;
  if (fread(strtab, sizeof(char), strtab_size, elf_file) !=
      sizeof(char) * strtab_size && ferror(elf_file) != 0)
    goto error;

  write_c_symtab(sym, strtab, so_addr, sym_cnt, header, buf);

  goto end;

error:
  status = -1;

end:
  if (sym != NULL)
    lj_mem_free(G(L), sym, sym_cnt * sizeof(ElfW(Sym)));
  if(strtab != NULL)
    lj_mem_free(G(L), strtab, strtab_size * sizeof(char));
  if(section_headers != NULL)
    lj_mem_free(G(L), section_headers, shnum * shentsize);

  fclose(elf_file);

  return status;
}

static int dump_dyn_symtab(struct dl_phdr_info *info, const uint8_t header,
			   struct lj_wbuf *buf)
{
  size_t header_index;
  for (header_index = 0; header_index < info->dlpi_phnum; ++header_index) {
    if (info->dlpi_phdr[header_index].p_type == PT_DYNAMIC) {
      ElfW(Dyn *) dyn =
	(ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[header_index].p_vaddr);
      ElfW(Sym *) sym = NULL;
      ElfW(Word *) hashtab = NULL;
      ElfW(Addr) ghashtab = 0;
      ElfW(Word) sym_cnt = 0;

      char *strtab = 0;

      for(; dyn->d_tag != DT_NULL; dyn++) {
        switch(dyn->d_tag) {
        case DT_HASH:
          hashtab = (ElfW(Word *))dyn->d_un.d_ptr;
          break;
        case DT_GNU_HASH:
          ghashtab = dyn->d_un.d_ptr;
          break;
        case DT_STRTAB:
          strtab = (char *)dyn->d_un.d_ptr;
          break;
        case DT_SYMTAB:
          sym = (ElfW(Sym *))dyn->d_un.d_ptr;
          break;
        default:
          break;
        }
      }

      if ((hashtab == NULL && ghashtab == 0) || strtab == NULL || sym == NULL)
        /* Not enough data to resolve symbols. */
        return 1;

      /*
      ** A hash table consists of Elf32_Word or Elf64_Word objects that provide
      ** for symbol table access. Hash table has the following organization:
      ** +-------------------+
      ** |      nbucket      |
      ** +-------------------+
      ** |      nchain       |
      ** +-------------------+
      ** |     bucket[0]     |
      ** |       ...         |
      ** | bucket[nbucket-1] |
      ** +-------------------+
      ** |     chain[0]      |
      ** |       ...         |
      ** |  chain[nchain-1]  |
      ** +-------------------+
      ** Chain table entries parallel the symbol table. The number of symbol
      ** table entries should equal nchain, so symbol table indexes also select
      ** chain table entries. Since the chain array values are indexes for not
      ** only the chain array itself, but also for the symbol table, the chain
      ** array must be the same size as the symbol table. This makes nchain
      ** equal to the length of the symbol table.
      **
      ** For more, see https://docs.oracle.com/cd/E23824_01/html/819-0690/chapter6-48031.html
      */
      sym_cnt = ghashtab == 0 ? hashtab[1] : ghashtab_size(ghashtab);
      write_c_symtab(sym, strtab, info->dlpi_addr, sym_cnt, header, buf);
      return 0;
    }
  }

  return 1;
}

struct symbol_resolver_conf {
  struct lj_wbuf *buf; /* Output buffer. */
  lua_State *L; /* Current Lua state. */
  const uint8_t header; /* Header for symbol entries to write. */

  uint32_t cur_lib; /* Index of the lib currently dumped. */
  uint32_t to_dump_cnt; /* Amount of libs to dump. */
  uint32_t *lib_adds; /* Memprof's counter of libs. */
};

static int resolve_symbolnames(struct dl_phdr_info *info, size_t info_size,
			       void *data)
{
  struct symbol_resolver_conf *conf = data;
  struct lj_wbuf *buf = conf->buf;
  lua_State *L = conf->L;
  const uint8_t header = conf->header;
  char executable_path[PATH_MAX] = {0};

  uint32_t lib_cnt = 0;

  /*
  ** Check that dlpi_adds and dlpi_subs fields are available.
  ** Assertion was taken from the GLIBC tests:
  ** https://code.woboq.org/userspace/glibc/elf/tst-dlmodcount.c.html#37
  */
  lj_assertL(info_size > offsetof(struct dl_phdr_info, dlpi_subs)
			 + sizeof(info->dlpi_subs),
	     "bad dlpi_subs");

  lib_cnt = info->dlpi_adds - *conf->lib_adds;

  /* Skip vDSO library. */
  if (info->dlpi_addr == getauxval(AT_SYSINFO_EHDR))
    return 0;

  if ((conf->to_dump_cnt = info->dlpi_adds - *conf->lib_adds) == 0)
    /* No new libraries, stop resolver. */
    return 1;

  if (conf->cur_lib < lib_cnt - conf->to_dump_cnt) {
    /* That lib is already dumped, skip it. */
    ++conf->cur_lib;
    return 0;
  }

  if (conf->cur_lib == lib_cnt - conf->to_dump_cnt - 1)
    /* Last library, update memrpof's lib counter. */
    *conf->lib_adds = info->dlpi_adds;

  /*
  ** The `dl_iterate_phdr` returns an empty string as a name for
  ** the executable from which it was called. It is still possible
  ** to access its dynamic symbol table, but it is vital for
  ** sysprof to obtain the main symbol table for the LuaJIT
  ** executable. To do so, we need a valid path to the executable.
  ** Since there is no way to obtain the path to a running
  ** executable using the C standard library, the only more or
  ** less reliable way to do this is by reading the symbolic link
  ** from `/proc/self/exe`. Most of the UNIX-based systems have
  ** procfs, so it is not a problem.
  ** Such path tweaks relate only for the main way (see below).
  */
  if (*info->dlpi_name == '\0') {
    if (readlink("/proc/self/exe", executable_path, PATH_MAX) != -1)
      info->dlpi_name = executable_path;
    else
      /*
      ** It is impossible for sysprof to work properly without the
      ** LuaJIT's .symtab section present. The assertion below is
      ** unlikely to be triggered on any system supported by
      ** sysprof, unless someone have deleted the LuaJIT binary
      ** right after the start.
      */
      lj_assertL(0, "bad executed binary symtab section");
  }

  /*
  ** Main way: try to open ELF and read SHT_SYMTAB, SHT_STRTAB and SHT_HASH
  ** sections from it.
  */
  if (dump_sht_symtab(info->dlpi_name, buf, L, header, info->dlpi_addr) == 0) {
    ++conf->cur_lib;
  }
  /* First fallback: dump functions only from PT_DYNAMIC segment. */
  else if(dump_dyn_symtab(info, header, buf) == 0) {
    ++conf->cur_lib;
  }
  /*
  ** Last resort: dump ELF size and address to show .so name for its functions
  ** in memprof output.
  */
  else {
    lj_wbuf_addbyte(buf, SYMTAB_CFUNC);
    lj_wbuf_addu64(buf, info->dlpi_addr);
    lj_wbuf_addstring(buf, info->dlpi_name);
    ++conf->cur_lib;
  }

  return 0;
}

#endif /* LJ_HASRESOLVER */

#if LJ_TARGET_WINDOWS
/*
** The Windows symbol table backend.
**
** There is no dl_iterate_phdr(): loaded PE images are enumerated via the
** toolhelp snapshot. Symbol names are taken from the COFF symbol table of
** the image (the PE analogue of the ELF .symtab, present in MinGW/Clang
** builds), or, if it was stripped, from the export directory (the analogue
** of the ELF dynamic symbol table). Every image is always dumped as a single
** symbol at its load address, so that a sampled address can be attributed to
** an image even if it has no symbols at all (the same last-resort entry the
** ELF backend emits).
**
** MSVC builds keep function names in a separate PDB file, which is not read
** here: the DbgHelp API (SymInitialize, SymLoadModuleEx, SymEnumSymbols) can
** be added as one more source without changing the stream format.
*/

/* Bounds check for an [off, off + len) range in an image of size bytes. */
static LJ_AINLINE int pe_range_ok(size_t off, size_t len, size_t size)
{
  return off <= size && len <= size - off;
}

static LJ_AINLINE const uint8_t *pe_ptr(const uint8_t *base, size_t off,
					size_t len, size_t size)
{
  return pe_range_ok(off, len, size) ? base + off : NULL;
}

static int dump_pe_exports(lj_symtab_cfunc_cb cb, void *ctx,
			   const uint8_t *base, size_t size)
{
  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
  const IMAGE_NT_HEADERS *nt;
  const IMAGE_DATA_DIRECTORY *dir;
  const IMAGE_EXPORT_DIRECTORY *exp;
  size_t nt_off;
  DWORD i, dumped = 0;

  if (size < sizeof(IMAGE_DOS_HEADER) ||
      dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
    return -1;

  nt_off = (size_t)dos->e_lfanew;
  nt = (const IMAGE_NT_HEADERS *)pe_ptr(base, nt_off, sizeof(IMAGE_NT_HEADERS),
					size);
  if (nt == NULL || nt->Signature != IMAGE_NT_SIGNATURE)
    return -1;

  dir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  if (dir->VirtualAddress == 0 ||
      !pe_range_ok(dir->VirtualAddress, sizeof(IMAGE_EXPORT_DIRECTORY), size))
    return -1;

  exp = (const IMAGE_EXPORT_DIRECTORY *)(base + dir->VirtualAddress);
  if (exp->NumberOfNames == 0 || exp->NumberOfFunctions == 0)
    return -1;

  for (i = 0; i < exp->NumberOfNames; i++) {
    const DWORD *name_rva;
    const WORD *ordinal;
    const DWORD *func_rva;
    const char *name;
    size_t name_off;
    DWORD rva;

    name_rva = (const DWORD *)pe_ptr(base, (size_t)exp->AddressOfNames +
				    i * sizeof(DWORD), sizeof(DWORD), size);
    ordinal = (const WORD *)pe_ptr(base, (size_t)exp->AddressOfNameOrdinals +
				   i * sizeof(WORD), sizeof(WORD), size);
    if (name_rva == NULL || ordinal == NULL)
      return -1;
    if (*ordinal >= exp->NumberOfFunctions)
      continue;
    func_rva = (const DWORD *)pe_ptr(base, (size_t)exp->AddressOfFunctions +
				     (size_t)*ordinal * sizeof(DWORD),
				     sizeof(DWORD), size);
    if (func_rva == NULL)
      return -1;

    rva = *func_rva;
    /* Skip forwarders: they point inside the export directory itself. */
    if (rva == 0 ||
        (rva >= dir->VirtualAddress &&
         rva - dir->VirtualAddress < dir->Size))
      continue;

    /* A name is a NUL-terminated ASCII string inside the image. */
    name_off = *name_rva;
    if (!pe_range_ok(name_off, 1, size) || base[name_off] == '\0')
      continue;
    name = (const char *)(base + name_off);
    if (memchr(name, '\0', size - name_off) == NULL)
      continue;

    cb(ctx, (uintptr_t)base + rva, name);
    dumped++;
  }

  return dumped != 0 ? 0 : -1;
}


/*
** COFF symbol table of a PE image. The table lives in the file, not in the
** loaded image, hence the file is read at its offset. Only function symbols
** defined in a section are dumped, like STT_FUNC entries of an ELF symtab.
*/
#define PEOBJ_SYM_SIZE 18
#define PEOBJ_AUX_MAX 255
#define PEOBJ_SCL_EXTERNAL 2
#define PEOBJ_SCL_STATIC 3
#define PEOBJ_DTYPE_FUNC 0x20

static int dump_pe_coff_symtab(lj_symtab_cfunc_cb cb, void *ctx,
			       const uint8_t *base, size_t size,
			       const char *path)
{
  const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
  const IMAGE_NT_HEADERS *nt;
  const IMAGE_SECTION_HEADER *sections;
  const IMAGE_FILE_HEADER *fh;
  uint8_t *symtab = NULL;
  char *strtab = NULL;
  size_t sym_size, strtab_size = 0;
  size_t nsyms, i;
  size_t nt_off;
  FILE *f = NULL;
  size_t dumped = 0;
  int status = -1;

  if (size < sizeof(IMAGE_DOS_HEADER) ||
      dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < 0)
    return -1;

  nt_off = (size_t)dos->e_lfanew;
  nt = (const IMAGE_NT_HEADERS *)pe_ptr(base, nt_off, sizeof(IMAGE_NT_HEADERS),
					size);
  if (nt == NULL || nt->Signature != IMAGE_NT_SIGNATURE)
    return -1;

  fh = &nt->FileHeader;
  if (fh->PointerToSymbolTable == 0 || fh->NumberOfSymbols == 0)
    return -1;

  sections = (const IMAGE_SECTION_HEADER *)pe_ptr(
      base, nt_off + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) +
		fh->SizeOfOptionalHeader,
      (size_t)fh->NumberOfSections * sizeof(IMAGE_SECTION_HEADER), size);
  if (sections == NULL)
    return -1;

  nsyms = fh->NumberOfSymbols;
  sym_size = nsyms * PEOBJ_SYM_SIZE;

  f = fopen(path, "rb");
  if (f == NULL)
    return -1;

  /* Runs on the profiler thread: not the Lua allocator, which is not
  ** thread safe.
  */
  symtab = (uint8_t *)malloc(sym_size + 1);
  if (symtab == NULL)
    goto end;
  if (fseek(f, (long)fh->PointerToSymbolTable, SEEK_SET) != 0)
    goto end;
  if (fread(symtab, sym_size, 1, f) != 1)
    goto end;

  /* The string table follows the symbol table. */
  if (fread(&strtab_size, sizeof(uint32_t), 1, f) != 1 ||
      strtab_size < sizeof(uint32_t))
    goto end;
  strtab = (char *)malloc(strtab_size);
  if (strtab == NULL)
    goto end;
  memcpy(strtab, &strtab_size, sizeof(uint32_t));
  if (fread(strtab + sizeof(uint32_t), strtab_size - sizeof(uint32_t), 1, f)
      != 1)
    goto end;

  for (i = 0; i < nsyms; i++) {
    const uint8_t *sym = symtab + i * PEOBJ_SYM_SIZE;
    uint32_t name_zeroes;
    int32_t value;
    int16_t sect;
    uint16_t type;
    uint8_t sclass = sym[16];
    uint8_t naux = sym[17];
    const char *name;
    char shortname[9];

    i += naux;  /* Auxiliary entries are not symbols. */

    if ((sclass != PEOBJ_SCL_EXTERNAL && sclass != PEOBJ_SCL_STATIC) ||
	(type = (uint16_t)(sym[14] | ((uint16_t)sym[15] << 8)),
	 (type & PEOBJ_DTYPE_FUNC) == 0))
      continue;

    memcpy(&sect, sym + 12, sizeof(sect));
    if (sect <= 0 || sect > (int16_t)fh->NumberOfSections)
      continue;  /* Undefined or absolute symbol. */
    memcpy(&value, sym + 8, sizeof(value));

    memcpy(&name_zeroes, sym, sizeof(name_zeroes));
    if (name_zeroes == 0) {
      uint32_t name_off;
      memcpy(&name_off, sym + 4, sizeof(name_off));
      if (name_off == 0 || name_off >= strtab_size)
	continue;
      name = strtab + name_off;
      if (memchr(name, '\0', strtab_size - name_off) == NULL)
	continue;
    } else {
      memcpy(shortname, sym, 8);
      shortname[8] = '\0';
      name = shortname;
    }

    cb(ctx, (uintptr_t)base +
       sections[sect - 1].VirtualAddress + (uint32_t)value, name);
    dumped++;
  }

  /* No usable entries: let the caller fall back to the export directory. */
  status = dumped != 0 ? 0 : -1;

end:
  free(strtab);
  free(symtab);
  if (f != NULL)
    fclose(f);
  return status;
}

static void dump_win_symtab(lj_symtab_cfunc_cb cb, void *ctx)
{
  HANDLE snap;
  MODULEENTRY32 me;

  snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32,
				  GetCurrentProcessId());
  if (snap == INVALID_HANDLE_VALUE)
    return;

  memset(&me, 0, sizeof(me));
  me.dwSize = sizeof(me);
  if (Module32First(snap, &me)) {
    do {
      const uint8_t *base = (const uint8_t *)me.modBaseAddr;
      size_t size = (size_t)me.modBaseSize;
      /* The image itself is always reported: its load address and path are
      ** what lets a profile be symbolised later (tools/utils/symresolver). */
      cb(ctx, (uintptr_t)base, me.szExePath);
      if (dump_pe_coff_symtab(cb, ctx, base, size, me.szExePath) == 0)
	continue;
      dump_pe_exports(cb, ctx, base, size);
    } while (Module32Next(snap, &me));
  }

  CloseHandle(snap);
}
#endif /* LJ_TARGET_WINDOWS */

void lj_symtab_cfuncs(lj_symtab_cfunc_cb cb, void *ctx)
{
#if LJ_TARGET_WINDOWS
  dump_win_symtab(cb, ctx);
#else
  /* ELF images are not walked by this collector yet: the POSIX backend
  ** feeds the stream directly (see resolve_symbolnames).
  */
  UNUSED(cb);
  UNUSED(ctx);
#endif
}

void lj_symtab_dump_newc(uint32_t *lib_adds, struct lj_wbuf *out,
			 uint8_t header, struct lua_State *L) {
#if LJ_HASRESOLVER
  struct symbol_resolver_conf conf = {
    .buf = out,
    .L = L,
    .header = header,
    .cur_lib = 0,
    .to_dump_cnt = 0,
    .lib_adds = lib_adds
  };
  dl_iterate_phdr(resolve_symbolnames, &conf);
#else
  UNUSED(lib_adds);
  UNUSED(out);
  UNUSED(header);
  UNUSED(L);
#endif
}

/* Dump the prototypes and traces reachable from the GC. */
static void dump_lua_objects(struct lj_wbuf *out, const struct global_State *g)
{
  const GCRef *iter = &g->gc.root;
  const GCobj *o;

  while ((o = gcref(*iter)) != NULL) {
    switch (o->gch.gct) {
    case (~LJ_TPROTO): {
      const GCproto *pt = gco2pt(o);
      lj_wbuf_addbyte(out, SYMTAB_LFUNC);
      lj_symtab_dump_proto(out, pt);
      break;
    }
#if LJ_HASJIT
    case (~LJ_TTRACE): {
      const GCtrace *tr = gco2trace(o);
      lj_symtab_trace_iv *iv = NULL;
      int niv = lj_symtab_trace_intervals(tr, &iv);
      lj_wbuf_addbyte(out, SYMTAB_TRACE);
      lj_symtab_dump_trace(out, tr, iv, niv);
      free(iv);
      break;
    }
#endif /* LJ_HASJIT */
    default:
      break;
    }
    iter = &o->gch.nextgc;
  }
}

#if LJ_TARGET_WINDOWS
/* The Lua part of the prologue, dumped on the profiled thread. */
void lj_symtab_dump_lua(struct lj_wbuf *out, const struct global_State *g)
{
  lj_wbuf_addn(out, ljs_header, sizeof(ljs_header));
  dump_lua_objects(out, g);
}

/* Serializes a collected symbol into the stream format. */
static void symtab_cfunc_stream(void *ctx, uintptr_t addr, const char *name)
{
  struct lj_wbuf *out = (struct lj_wbuf *)ctx;
  lj_wbuf_addbyte(out, SYMTAB_CFUNC);
  lj_wbuf_addu64(out, (uint64_t)addr);
  lj_wbuf_addstring(out, name);
}

/*
** Collect the function entries of the VM assembly: buildvm splits the
** .text section at vm_ffi_call (see host/buildvm_peobj.c), so a frame in
** the second half has an entry of its own. The end of one entry is the
** start of the next one; the reverse lookup makes sure that the address
** really is a function entry and not the end of the blob.
*/
int lj_symtab_vm_functions(uintptr_t *begin, int max)
{
  const void *const pcs[2] = {
    (const void *)lj_vm_asm_begin, (const void *)lj_vm_profhook
  };
  int n = 0, i;

  for (i = 0; i < 2 && n < max; i++) {
    DWORD64 base = 0;
    PRUNTIME_FUNCTION rf =
      RtlLookupFunctionEntry((DWORD64)(uintptr_t)pcs[i], &base, NULL);
    int k;

    if (rf == NULL)
      continue;
    for (k = 0; k < 2 && n < max; k++) {
      uintptr_t b = (uintptr_t)(base + (k == 0 ? rf->BeginAddress
					       : rf->EndAddress));
      int j, dup = 0;

      if (k == 1) {
	DWORD64 base2 = 0;
	PRUNTIME_FUNCTION rf2 = RtlLookupFunctionEntry((DWORD64)b, &base2, NULL);
	if (rf2 == NULL || base2 + rf2->BeginAddress != b)
	  continue;
      }
      for (j = 0; j < n; j++)
	if (begin[j] == b)
	  dup = 1;
      if (!dup)
	begin[n++] = b;
    }
  }
  return n;
}

static void dump_vm_functions(struct lj_wbuf *out)
{
  uintptr_t begin[4];
  int n = lj_symtab_vm_functions(begin, 4), i;

  for (i = 0; i < n; i++) {
    lj_wbuf_addbyte(out, SYMTAB_VM);
    lj_wbuf_addu64(out, (uint64_t)begin[i]);
  }
}

/* The C part of the prologue, dumped on the profiler thread. */
void lj_symtab_dump_c(struct lj_wbuf *out)
{
  dump_vm_functions(out);
  lj_symtab_cfuncs(symtab_cfunc_stream, out);
  lj_wbuf_addbyte(out, SYMTAB_FINAL);
}
#endif

void lj_symtab_dump(struct lj_wbuf *out, const struct global_State *g,
		    uint32_t *lib_adds)
{
#if LJ_HASRESOLVER
  struct symbol_resolver_conf conf = {
    .buf = out,
    .L = gco2th(gcref(g->cur_L)),
    .header = SYMTAB_CFUNC,
    .cur_lib = 0,
    .to_dump_cnt = 0,
    .lib_adds = lib_adds
  };
#else
  UNUSED(lib_adds);
#endif

  /* Write prologue. */
  lj_wbuf_addn(out, ljs_header, sizeof(ljs_header));
  dump_lua_objects(out, g);

#if LJ_HASRESOLVER
  /* Write C symbols. */
  dl_iterate_phdr(resolve_symbolnames, &conf);
#elif LJ_TARGET_WINDOWS
  /* Write C symbols. */
  lj_symtab_cfuncs(symtab_cfunc_stream, out);
#endif
  lj_wbuf_addbyte(out, SYMTAB_FINAL);
}
