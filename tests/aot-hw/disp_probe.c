/* Print the interpreter bytecode-handler addresses of the running LuaJIT.
 *
 * macOS only (uses dyld to find the image base). Build and load it from a
 * Lua script:
 *
 *   cc -O2 -std=c99 -fPIC -Isrc -bundle -undefined dynamic_lookup \
 *      tests/aot-hw/disp_probe.c -o /tmp/disp_probe.so
 *   ./src/luajit -joff -e 'assert(package.loadlib("/tmp/disp_probe.so",
 *     "luaopen_disp_probe"))()()'
 *
 * The "off_*" values (relative to lua_pcall) are what hw_trace atentry:<off>
 * expects; they are stable for a given LuaJIT build.
 */
#include <stdio.h>
#include <mach-o/dyld.h>

#include "lua.h"
#include "lauxlib.h"
#include "lj_obj.h"
#include "lj_dispatch.h"
#include "lj_bc.h"

static int probe(lua_State *L)
{
  GG_State *GG = (GG_State *)((char *)G(L) - GG_OFS(g));
  ASMFunction *d = GG->dispatch;
  const struct mach_header *img = _dyld_get_image_header(0);
  void (*pcall)(void) = (void (*)(void))lua_pcall;
  printf("image_base=%p pcall=%p\n", (void *)img, (void *)pcall);
  printf("stat_forl=%#lx stat_mulvn=%#lx stat_addvn=%#lx stat_fori=%#lx\n",
	 (unsigned long)((char *)(void *)d[BC_FORL] - (char *)img),
	 (unsigned long)((char *)(void *)d[BC_MULVN] - (char *)img),
	 (unsigned long)((char *)(void *)d[BC_ADDVN] - (char *)img),
	 (unsigned long)((char *)(void *)d[BC_FORI] - (char *)img));
  printf("off_forl=%ld off_mulvn=%ld off_addvn=%ld off_fori=%ld\n",
	 (long)((char *)(void *)d[BC_FORL] - (char *)pcall),
	 (long)((char *)(void *)d[BC_MULVN] - (char *)pcall),
	 (long)((char *)(void *)d[BC_ADDVN] - (char *)pcall),
	 (long)((char *)(void *)d[BC_FORI] - (char *)pcall));
  return 0;
}

int luaopen_disp_probe(lua_State *L)
{
  lua_pushcfunction(L, probe);
  return 1;
}
