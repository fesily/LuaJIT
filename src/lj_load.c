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
#include "lj_state.h"  /* incr_top: not implicit in a non-amalgamated build */
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

#if LJ_DS_PUC51_DUMP
/* DS: keep the exact bytes this parse consumes, and give every prototype a
** pre-order identity, so the host can later produce genuine PUC 5.1 bytecode for
** one function of this compile. Nothing is captured while lj_ds_puc_dump is NULL
** (standalone interpreter, tests, any host that never wired the service).
*/

typedef struct DSCaptureCtx {
  lua_Reader r;
  void *d;
  SBuf *sb;		/* The LexState's capture buffer, not a shared buffer. */
} DSCaptureCtx;

/* Same reader, same call count, same returned bytes: the parser sees exactly what
** the original reader produced, and the copy is a side effect only. */
static const char *ds_capture_reader(lua_State *L, void *ud, size_t *size)
{
  DSCaptureCtx *c = (DSCaptureCtx *)ud;
  const char *chunk = c->r(L, c->d, size);
  /* ~(size_t)0 is luaL_loadbuffer's "embedded bytecode of unknown length"
  ** sentinel (lib_package.c uses it for luaJIT_BC_* modules); lex_more handles
  ** it, but it must never enter the text capture. */
  if (chunk != NULL && size != NULL && *size > 0 &&
      *size != ~(size_t)0) {
    size_t used = (size_t)sbuflen(c->sb);
    /* Check before narrowing to MSize: neither the 32 bit size nor the VM string
    ** limit may be crossed, and a partial capture must never look complete. */
    if (*size > (size_t)LJ_MAX_STR - used)
      lj_err_mem(L);
    lj_buf_putmem(c->sb, chunk, (MSize)*size);
  }
  return chunk;
}

/* Walks the prototype tree in the parser's pre-order and stores the shared source
** reference plus the compile's total node count. */
static void ds_attach_source(lua_State *L, GCproto *pt, GCstr *src, uint32_t count)
{
  setgcref(pt->dssource, obj2gco(src));
  lj_gc_objbarrier(L, pt, src);
  pt->dsnodecount = count;
  if (pt->flags & PROTO_CHILD) {
    ptrdiff_t i;
    for (i = -(ptrdiff_t)pt->sizekgc; i < 0; i++) {
      GCobj *o = proto_kgc(pt, i);
      if (o && o->gch.gct == ~LJ_TPROTO)
	ds_attach_source(L, gco2pt(o), src, count);
    }
  }
}

/* PUC 5.1's luaL_loadfile keeps the leading #! line in the line numbering, but
** luaL_loadbuffer does not skip it. The VM lexer skips the line, so replace it
** with spaces (keeping the newline) before the host recompiles the source. */
static void ds_capture_normalize_shebang(SBuf *sb)
{
  char *p = sb->b;
  MSize n = sbuflen(sb), i;
  if (p == NULL || n < 2 || p[0] != '#' || p[1] != '!')
    return;
  for (i = 0; i < n && p[i] != '\n'; i++)
    p[i] = ' ';
}

/* Interning the source text allocates, and the freshly parsed prototype tree is
** not referenced yet, so the root is anchored first. */
static void ds_capture_source(lua_State *L, LexState *ls, GCproto *pt)
{
  GCstr *src;
  const char *body;
  if (!ls->dscapture)
    return;
  setprotoV(L, L->top, pt);
  incr_top(L);
  ds_capture_normalize_shebang(&ls->dssource);
  body = ls->dssource.b;
  src = lj_str_new(L, body ? body : "", (size_t)sbuflen(&ls->dssource));
  ds_attach_source(L, pt, src, ls->dsnextnode);
  L->top--;
}
#endif

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
#if LJ_DS_PUC51_DUMP
    ds_capture_source(L, ls, pt);
#endif
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
#if LJ_DS_PUC51_DUMP
  DSCaptureCtx dscap;
#endif
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
#if LJ_DS_PUC51_DUMP
  /* Snapshot the host slot once: a nested load inside the reader has its own
  ** LexState and must not change this parse's capture decision. */
  lj_buf_init(L, &ls.dssource);
  ls.dsnextnode = 0;
  ls.dscapture = lj_ds_puc_dump != NULL;
  if (ls.dscapture) {
    dscap.r = reader; dscap.d = data; dscap.sb = &ls.dssource;
    ls.rfunc = ds_capture_reader;
    ls.rdata = &dscap;
  }
#endif
  status = lj_vm_cpcall(L, NULL, &ls, cpparser);
  lj_lex_cleanup(L, &ls);
#if LJ_DS_PUC51_DUMP
  /* Released on every path (parse error, reader error, OOM), unlike the loadlog
  ** buffer above which only frees at EOF. */
  lj_buf_free(G(L), &ls.dssource);
#endif
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
