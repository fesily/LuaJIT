/*
** Atomic operations for the concurrent GC.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
*/

#ifndef _LJ_ATOMIC_H
#define _LJ_ATOMIC_H

#include "lj_def.h"
#include "lj_arch.h"

/*
** Minimal atomics for the concurrent GC (LJ_CONCGC): byte RMW on the
** marked field, shared by the GC thread (color bits) and the mutator
** (LJ_GC_LOGGED bit). Relaxed ordering -- cross-thread ordering comes
** from the park/resume mutex, never from these.
*/

#if defined(__GNUC__) || defined(__clang__)

static LJ_AINLINE uint8_t lj_atomic_or8(uint8_t *p, uint8_t v)
{
  return __atomic_fetch_or(p, v, __ATOMIC_RELAXED);
}

static LJ_AINLINE uint8_t lj_atomic_and8(uint8_t *p, uint8_t v)
{
  return __atomic_fetch_and(p, v, __ATOMIC_RELAXED);
}

static LJ_AINLINE uint8_t lj_atomic_load8(const uint8_t *p)
{
  return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static LJ_AINLINE uint32_t lj_atomic_load32(const uint32_t *p)
{
  return __atomic_load_n(p, __ATOMIC_RELAXED);
}

static LJ_AINLINE void lj_atomic_store32(uint32_t *p, uint32_t v)
{
  __atomic_store_n(p, v, __ATOMIC_RELAXED);
}

static LJ_AINLINE uint64_t lj_atomic_load64(const uint64_t *p)
{
  return __atomic_load_n(p, __ATOMIC_RELAXED);
}

#else
#error "LJ_CONCGC requires GCC/Clang atomic builtins"
#endif

#endif
