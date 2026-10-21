/*
** Symbol table for profilers.
**
** Major portions taken verbatim or adapted from the LuaVela.
** Copyright (C) 2015-2019 IPONWEB Ltd.
*/

#ifndef LJ_SYMTAB_H
#define LJ_SYMTAB_H

#include "lj_wbuf.h"
#include "lj_obj.h"
#include "lj_debug.h"

#if LJ_HASJIT
#include "lj_dispatch.h"
#endif

#define LJS_CURRENT_VERSION 0x5

/*
** symtab format:
**
** symtab         := prologue sym*
** prologue       := 'l' 'j' 's' version reserved
** version        := <BYTE>
** reserved       := <BYTE> <BYTE> <BYTE>
** sym            := sym-lua | sym-cfunc | sym-trace | sym-final
** sym-lua        := sym-header sym-addr sym-chunk sym-line
** sym-cfunc      := sym-header sym-addr sym-name
** sym-trace      := sym-header trace-no sym-addr sym-line mcode mcode-size
** sym-header     := <BYTE>
** sym-addr       := <ULEB128>
** sym-chunk      := string
** sym-line       := <ULEB128>
** sym-name       := string
** sym-final      := sym-header
** trace-no       := <ULEB128>
** trace-addr     := <ULEB128>
** mcode          := <ULEB128>  start of the trace's machine code
** mcode-size     := <ULEB128>  its size in bytes (version 5+)
** string         := string-len string-payload
** string-len     := <ULEB128>
** string-payload := <BYTE> {string-len}
**
** <BYTE>   :  A single byte (no surprises here)
** <ULEB128>:  Unsigned integer represented in ULEB128 encoding
**
** (Order of bits below is hi -> lo)
**
** version: [VVVVVVVV]
**  * VVVVVVVV: Byte interpreted as a plain numeric version number
**
** sym-header: [FUUUUUTT]
**  * TT    : 2 bits for representing symbol type
**  * UUUUU : 5 unused bits
**  * F     : 1 bit marking the end of the symtab (final symbol)
*/

#define SYMTAB_LFUNC ((uint8_t)0)
#define SYMTAB_CFUNC ((uint8_t)1)
#define SYMTAB_TRACE ((uint8_t)2)
/* Start address of a VM assembly function. Such frames are profiler-visible
** only as a boundary between the Lua and the host callchain, they carry no
** name. */
#define SYMTAB_VM ((uint8_t)3)
#define SYMTAB_FINAL ((uint8_t)0x80)

#if LJ_HASJIT
/*
** Dumps traceinfo into the symbol table.
*/
void lj_symtab_dump_trace(struct lj_wbuf *out, const GCtrace *trace);
#endif /* LJ_HASJIT */

/*
** Dumps function prototype.
*/
void lj_symtab_dump_proto(struct lj_wbuf *out, const GCproto *pt);

/*
** Dumps newly loaded symbols to event stream.
*/
void lj_symtab_dump_newc(uint32_t *lib_adds, struct lj_wbuf *out,
			 uint8_t header, struct lua_State *L);

/*
** C symbol collection, independent of the stream format and of the Lua state.
**
** lj_symtab_cfuncs() reports the function symbols of every loaded module
** (Windows: PE images, COFF symbol table first, then the export directory,
** then the image base). It does not allocate through the Lua allocator and
** does not touch the VM, so it may run on any thread. `name` is valid only
** until the callback returns.
*/
typedef void (*lj_symtab_cfunc_cb)(void *ctx, uintptr_t addr, const char *name);

void lj_symtab_cfuncs(lj_symtab_cfunc_cb cb, void *ctx);

#if LJ_TARGET_WINDOWS
/*
** The Windows prologue is split in two: the Lua symbols are dumped on the
** profiled thread (they are read from the GC heap), while the C symbols of
** the loaded PE images are dumped on the profiler thread, which is created
** by the profiling timer. The caller must not interleave either part with
** the other one: lua first, then c.
*/
void lj_symtab_dump_lua(struct lj_wbuf *out, const struct global_State *g);
void lj_symtab_dump_c(struct lj_wbuf *out);
#endif

/*
** Dumps symbol table for Lua functions into a buffer.
*/
void lj_symtab_dump(struct lj_wbuf *out, const struct global_State *g,
		    uint32_t *lib_adds);

#endif
