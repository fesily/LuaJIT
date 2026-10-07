/*
** Load and dump code.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <errno.h>
#include <stdio.h>

#define lj_load_c
#define LUA_CORE

#include "lua.h"
#include "lauxlib.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_buf.h"
#include "lj_func.h"
#include "lj_frame.h"
#include "lj_vm.h"
#include "lj_lex.h"
#include "lj_bcdump.h"
#include "lj_parse.h"
#define LJ_IO_PATCH_IMPLEMENTATION
#include "lj_io_patch.h"

/* -- Load Lua source code and bytecode ----------------------------------- */

static TValue *cpparser(lua_State *L, lua_CFunction dummy, void *ud)
{
  LexState *ls = (LexState *)ud;
  GCproto *pt;
  GCfunc *fn;
  int bc;
  UNUSED(dummy);
  cframe_errfunc(L->cframe) = -1;  /* Inherit error function. */
  bc = lj_lex_setup(L, ls);
  if (ls->mode) {
    int xmode = 1;
    const char *mode = ls->mode;
    char c;
    while ((c = *mode++)) {
      if (c == (bc ? 'b' : 't')) xmode = 0;
      if (c == (LJ_FR2 ? 'W' : 'X')) ls->fr2 = !LJ_FR2;
    }
    if (xmode) {
      setstrV(L, L->top++, lj_err_str(L, LJ_ERR_XMODE));
      lj_err_throw(L, LUA_ERRSYNTAX);
    }
  }
  if (bc) {
    pt = lj_bcread(ls);
  } else {
    pt = lj_parse(ls);
  }
  if (ls->fr2 == LJ_FR2) {
    fn = lj_func_newL_empty(L, pt, tabref(L->env));
    /* Don't combine above/below into one statement. */
    setfuncV(L, L->top++, fn);
  } else {
    /* Non-native generation returns a dumpable, but non-runnable prototype. */
    setprotoV(L, L->top++, pt);
  }
  return NULL;
}

#if LJ_DS_LOADLOG
/* Forward declarations (the definitions live further down). */
static int ds_loadlog_on(void);
static int ds_loadlog_want(const char *name);
static void ds_loadlog_dump_write(const char *name, const char *buf, size_t size);

typedef struct {
  lua_Reader r; void *d; const char *cn;
  char *acc; size_t accn, acccap;      /* accumulated chunk body */
} DSLogCtx;
static const char *ds_logreader(lua_State *L, void *ud, size_t *size) {
  DSLogCtx *c = (DSLogCtx *)ud;
  const char *chunk = c->r(L, c->d, size);
  int eoi = (!chunk || !size || *size == 0);
  if (!eoi && ds_loadlog_on() && ds_loadlog_want(c->cn)) {
    if (c->accn + *size > c->acccap) {
      size_t cap = c->acccap ? c->acccap * 2 : 65536;
      char *p;
      while (cap < c->accn + *size) cap *= 2;
      p = (char *)realloc(c->acc, cap);
      if (p) { c->acc = p; c->acccap = cap; }
    }
    if (c->acc && c->accn + *size <= c->acccap) {
      memcpy(c->acc + c->accn, chunk, *size);
      c->accn += *size;
    }
  } else if (eoi && ds_loadlog_on() && c->acc) {
    ds_loadlog_dump_write(c->cn, c->acc, c->accn);
    free(c->acc);
    c->acc = NULL; c->accn = c->acccap = 0;
  }
  return chunk;
}
#endif

LUA_API int lua_loadx(lua_State *L, lua_Reader reader, void *data,
		      const char *chunkname, const char *mode)
{
  LexState ls;
  int status;
#if LJ_DS_LOADLOG
  DSLogCtx dsctx;
  if (ds_loadlog_on()) { dsctx.r = reader; dsctx.d = data; dsctx.cn = chunkname;
    dsctx.acc = NULL; dsctx.accn = 0; dsctx.acccap = 0;
    reader = ds_logreader; data = &dsctx; }
#endif
  ls.rfunc = reader;
  ls.rdata = data;
  ls.chunkarg = chunkname ? chunkname : "?";
  ls.mode = mode;
  lj_buf_init(L, &ls.sb);
  status = lj_vm_cpcall(L, NULL, &ls, cpparser);
  lj_lex_cleanup(L, &ls);
  lj_gc_check(L);
  return status;
}

LUA_API int lua_load(lua_State *L, lua_Reader reader, void *data,
		     const char *chunkname)
{
  return lua_loadx(L, reader, data, chunkname, NULL);
}

typedef struct FileReaderCtx {
  FILE *fp;
  char buf[LUAL_BUFFERSIZE];
} FileReaderCtx;

static const char *reader_file(lua_State *L, void *ud, size_t *size)
{
  FileReaderCtx *ctx = (FileReaderCtx *)ud;
  UNUSED(L);
  if (lj_feof(ctx->fp)) return NULL;
  *size = lj_fread(ctx->buf, 1, sizeof(ctx->buf), ctx->fp);
  return *size > 0 ? ctx->buf : NULL;
}

LUALIB_API int luaL_loadfilex(lua_State *L, const char *filename,
			      const char *mode)
{
  FileReaderCtx ctx;
  int status;
  const char *chunkname;
  int err = 0;
  if (filename) {
    chunkname = lua_pushfstring(L, "@%s", filename);
    ctx.fp = lj_fopen(filename, "rb");
    if (ctx.fp == NULL) {
      L->top--;
      lua_pushfstring(L, "cannot open %s: %s", filename, strerror(errno));
      return LUA_ERRFILE;
    }
  } else {
    ctx.fp = stdin;
    chunkname = "=stdin";
  }
  status = lua_loadx(L, reader_file, &ctx, chunkname, mode);
  if (lj_ferror(ctx.fp)) err = errno;
  if (filename) {
    lj_fclose(ctx.fp);
    L->top--;
    copyTV(L, L->top-1, L->top);
  }
  if (err) {
    const char *fname = filename ? filename : "stdin";
    L->top--;
    lua_pushfstring(L, "cannot read %s: %s", fname, strerror(err));
    return LUA_ERRFILE;
  }
  return status;
}

LUALIB_API int luaL_loadfile(lua_State *L, const char *filename)
{
  return luaL_loadfilex(L, filename, NULL);
}

typedef struct StringReaderCtx {
  const char *str;
  size_t size;
} StringReaderCtx;

static const char *reader_string(lua_State *L, void *ud, size_t *size)
{
  StringReaderCtx *ctx = (StringReaderCtx *)ud;
  UNUSED(L);
  if (ctx->size == 0) return NULL;
  *size = ctx->size;
  ctx->size = 0;
  return ctx->str;
}

static const char *custom_strstr(const char *haystack, int haystack_length, const char *needle) {
    int needle_length = strlen(needle);
    
    for (int i = 0; i <= haystack_length - needle_length; ++i) {
        if (memcmp(haystack + i, needle, needle_length) == 0) {
            return (char *)(haystack + i);
        }
    }
    
    return NULL;
}

#if LJ_DS_LOADLOG
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#define DS_LOADLOG_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define DS_LOADLOG_MKDIR(p) mkdir(p, 0755)
#endif
/* DS_LOADLOG=1: write every loaded chunk body into ds_loaddump/ (cwd-relative, i.e.
** <game>/data/ds_loaddump/ in-game); DS_LOADLOG_FILTER=<substr> restricts it to
** chunk names containing <substr>.  Used to read engine-decrypted mod sources. */
static int ds_loadlog_on(void) {
  static int cached = -1;
  if (cached < 0) {
    const char *v = getenv("DS_LOADLOG");
    cached = (v && *v && *v != '0') ? 1 : 0;
  }
  return cached;
}
static const char *ds_loadlog_filter(void) {
  static const char *cached = NULL;
  static int inited = 0;
  if (!inited) {
    inited = 1;
    cached = getenv("DS_LOADLOG_FILTER");
    if (cached && !*cached) cached = NULL;
  }
  return cached;
}
static int ds_loadlog_want(const char *name) {
  const char *filter = ds_loadlog_filter();
  if (!filter) return 1;
  return name && strstr(name, filter) != NULL;
}
static void ds_loadlog_dump_write(const char *name, const char *buf, size_t size) {
  static unsigned counter = 0;
  char path[512];
  char safe[384];
  size_t i, j = 0;
  const char *sub;
  if (!name) name = "?";
  sub = strstr(name, "../mods/");
  if (sub) {
    for (i = 0; sub[i] && j < sizeof(safe) - 1; i++) {
      char c = sub[i];
      safe[j++] = (c == '/' || c == '\\' || c == ':') ? '_' : c;
    }
  } else {
    for (i = 0; name[i] && j < sizeof(safe) - 1; i++) {
      char c = name[i];
      safe[j++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_') ? c : '_';
    }
  }
  safe[j] = 0;
  DS_LOADLOG_MKDIR("ds_loaddump");
  snprintf(path, sizeof(path), "ds_loaddump/%u_%s.lua", counter++, safe[0] ? safe : "chunk");
  {
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(buf, 1, size, f); fclose(f); }
  }
}
#endif

LUALIB_API int luaL_loadbufferx(lua_State *L, const char *buf, size_t size,
				const char *name, const char *mode)
{
  StringReaderCtx ctx;
  ctx.str = buf;
  ctx.size = size;
  return lua_loadx(L, reader_string, &ctx, name, mode);
}

#if LJ_DS_LOADBUFFER_PATCH
LUA_DATA_API int (*lj_need_transform_path)() = NULL;
#endif

LUALIB_API int luaL_loadbuffer(lua_State *L, const char *buf, size_t size,
			       const char *name)
{
#if LJ_DS_LOADBUFFER_PATCH
  if (buf != name && lj_need_transform_path && lj_need_transform_path()){
    if (name[0] != '@'){
      if (strncmp(name, "scripts/", sizeof("scripts/") -1) == 0 || strncmp(name , "../mods/", sizeof("../mods/") - 1) == 0) {
        char path[260];
        snprintf(path, 260, "@%s", name);
        return luaL_loadbufferx(L, buf, size, path, NULL);
      }
    }
  }
#endif
  return luaL_loadbufferx(L, buf, size, name, NULL);
}

LUALIB_API int luaL_loadstring(lua_State *L, const char *s)
{
  return luaL_loadbuffer(L, s, strlen(s), s);
}

/* -- Dump bytecode ------------------------------------------------------- */

LUA_API int lua_dump(lua_State *L, lua_Writer writer, void *data)
{
  cTValue *o = L->top-1;
  uint32_t flags = LJ_FR2*BCDUMP_F_FR2;  /* Default mode for legacy C API. */
  lj_checkapi(L->top > L->base, "top slot empty");
  if (tvisfunc(o) && isluafunc(funcV(o)))
    return lj_bcwrite(L, funcproto(funcV(o)), writer, data, flags);
  else
    return 1;
}
