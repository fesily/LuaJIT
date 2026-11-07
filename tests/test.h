/*
** Test framework for the coroutine hook extension.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _TEST_H
#define _TEST_H

#include <stdio.h>
#include <string.h>

#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include "luajit.h"
#include "lj_obj.h"		/* Stack-size invariants are checked directly. */

/* Set by the Makefile according to the library configuration. */
#ifndef TEST_HAS_COROHOOK
#define TEST_HAS_COROHOOK 1
#endif

/* -- Check framework ----------------------------------------------------- */

extern int test_checks, test_fails;

#define TEST_CHECK(c) \
  do { test_checks++; if (!(c)) { test_fails++; \
    printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Check a resume status and report the error object on failure. */
#define TEST_STATUS(cond, co) \
  do { test_checks++; if (!(cond)) { test_fails++; \
    const char *e_ = lua_tostring((co), -1); \
    printf("  FAIL %s:%d: %s [%s]\n", __FILE__, __LINE__, #cond, \
	   e_ != NULL ? e_ : "no error object"); } } while (0)

/* Run a Lua chunk and report its error message on failure. */
void test_dostring(lua_State *L, const char *src, const char *file, int line);
#define TEST_DOSTRING(L, src)	test_dostring((L), (src), __FILE__, __LINE__)

/* -- Event log ----------------------------------------------------------- */

#define TEST_MAXEV	512

typedef struct TestEv {
  void *data;
  lua_State *from, *to;
  int event, status;
} TestEv;

extern TestEv test_evlog[TEST_MAXEV];
extern int test_nev;

/* Registration tokens used by the cases. */
extern void *const TEST_DATA_A, *const TEST_DATA_B, *const TEST_DATA_C;

/* Hook callbacks. */
void test_cb(void *data, lua_State *from, lua_State *to, int event, int status);
void test_cb_inspect(void *data, lua_State *from, lua_State *to, int event,
		     int status);

int test_inspect_calls(void);		/* Counter for test_cb_inspect. */
int test_inspect_frames(void);

void test_reset(void);			/* Clear the event log. */
void test_open(lua_State *L);		/* openlibs + corpus + C helpers. */
lua_State *test_newco(lua_State *L, const char *func);
long test_stacksize(lua_State *co);
int test_modsum(int n, int m);

/* -- Cases --------------------------------------------------------------- */

void test_case_plain(void);		/* Coroutines without a hook. */
#if TEST_HAS_COROHOOK
void test_case_api(void);		/* C API resume. */
void test_case_lua(void);		/* Lua resume/wrap. */
void test_case_stress(void);		/* Many resumes, big stacks, JIT. */
void test_case_inspect(void);		/* Inspection and debug.sethook. */
#endif

#endif
