/*
** Test runner for the coroutine hook extension.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#include <stdio.h>

#include "test.h"

typedef struct TestCase {
  const char *name;
  void (*run)(void);
} TestCase;

static const TestCase cases[] = {
  { "coroutines", test_case_plain },
#if TEST_HAS_COROHOOK
  { "c api", test_case_api },
  { "lua resume/wrap", test_case_lua },
  { "stress", test_case_stress },
  { "inspection", test_case_inspect },
#endif
  { NULL, NULL }
};

int main(void)
{
  int i, failures = 0, checks = 0;
  setvbuf(stdout, NULL, _IONBF, 0);
  printf("1..%d\n", (int)(sizeof(cases)/sizeof(cases[0]) - 1));
#if !TEST_HAS_COROHOOK
  printf("# coroutine hook extension compiled out, hook cases skipped\n");
#endif
  for (i = 0; cases[i].name != NULL; i++) {
    int before = test_fails;
    test_checks = test_fails = 0;
    test_reset();
    cases[i].run();
    checks += test_checks;
    failures += test_fails;
    if (test_fails == 0)
      printf("ok %d - %s (%d checks)\n", i+1, cases[i].name, test_checks);
    else
      printf("not ok %d - %s (%d of %d checks failed)\n",
	     i+1, cases[i].name, test_fails, test_checks);
    (void)before;
  }
  printf("# %d checks, %d failures\n", checks, failures);
  return failures != 0;
}
