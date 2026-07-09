/*
** AddressSanitizer poison helpers for the arena GC.
** Copyright (C) 2005-2026 Mike Pall. See Copyright Notice in luajit.h
**
** Included by lj_arena.h after the arena_cellptr() macro is defined.
** Provides LJ_USE_ASAN detection, poison/unpoison thin wrappers, and
** ASAN-safe freelist linkword accessors. With ASAN disabled every
** helper compiles to ((void)0) / a plain deref, so non-ASAN builds
** are unchanged in behavior and codegen.
**
** Must be included AFTER arena_cellptr() and the arena enums are
** visible (it uses CellSizeLog2, MinCellId, MaxCellId, arena_cellptr,
** GCCellID1, GCArena).
*/
#ifndef _LJ_ASAN_H
#define _LJ_ASAN_H

/* -- ASAN detection ------------------------------------------------------ */
/*
** GCC defines __SANITIZE_ADDRESS__ when -fsanitize=address is in effect.
** Clang uses __has_feature(address_sanitizer). Detect either. Defining
** LUAJIT_USE_ASAN without the sanitizer flag is a user error: poison
** helpers would be no-ops while the user expects detection, so #error.
*/
#if defined(__SANITIZE_ADDRESS__)
#define LJ_USE_ASAN 1
#if defined(__has_include)
#if __has_include(<sanitizer/asan_interface.h>)
#include <sanitizer/asan_interface.h>
#endif
#endif
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define LJ_USE_ASAN 1
#if defined(__has_include)
#if __has_include(<sanitizer/asan_interface.h>)
#include <sanitizer/asan_interface.h>
#endif
#endif
#endif
#elif defined(LUAJIT_USE_ASAN)
#error "LUAJIT_USE_ASAN requires -fsanitize=address on the target compile/link"
#else
#define LJ_USE_ASAN 0
#endif

/* -- Poison contract (LOCKED) -------------------------------------------- */
/*
** Arena GC ASAN poisoning contract — every alloc/free/sweep/lifecycle
** site in lj_arena.c, lj_arena.h, lj_gc.h and lj_gc_arena.c honors this:
**
**  1. Live vs dead: a cell region is UNPOISONED iff it is an allocated
**     block handed to the mutator (White/Black). Free blocks, free
**     extent tails, and never-yet-bumped space are POISONED.
**  2. Bump redzone: [arena_cellptr(a, celltop), arena_cellptr(a,
**     celltopmax)) is ALWAYS poisoned (overflow canary past the frontier).
**  3. Lifecycle baseline: on create/reinit poison the whole data area
**     [MinCellId, MaxCellId). Cells 0..MinCellId-1 (bitmaps/header) are
**     NEVER poisoned. ArenaFreeList and gray-stack buffers (allocated
**     via g->allocf / dlmalloc) are OUT of scope.
**  4. Alloc unpoison: every allocation unpoisons exactly
**     arena_roundcells(size)<<CellSizeLog2 bytes — on BOTH arena_alloc
**     branches (bump AND bin-pop) and on arena_fit / allocslow paths.
**  5. Free poison: every free re-poisons the whole rounded block when
**     it stops being mutator-owned (bin push, range add, frontier
**     rollback, bulk POD transform).
**  6. Linkword access (binned freelist only): free cells store a 2-byte
**     GCCellID1 next-id at the head. The ASAN shadow granule is 8 bytes
**     and sub-granule poison is unreliable, so the free block is fully
**     poisoned and freelist next-id accesses temporarily unpoison the
**     8-byte granule at the cell head, access, then re-poison. Use
**     arena_linkword_get / arena_linkword_set at EVERY raw freelist-next
**     *(GCCellID1 *) deref on poisoned free cells. Do NOT wrap gray-stack
**     or dlmalloc GCCellID1 * buffers (those are not arena-cell data).
**  7. Ranged free blocks (n > ArenaBins): the next link lives in
**     fl->ranges[], NOT in the cell, so poison the FULL n*CellSize with
**     no permanent head exemption.
**  8. Activation: __asan_* is emitted only when the compiler reports ASAN
**     live (__SANITIZE_ADDRESS__ or __has_feature(address_sanitizer)).
**     Defining LUAJIT_USE_ASAN without -fsanitize=address is a #error.
**  9. Error class: manual poison reports "use-after-poison", NOT
**     "heap-use-after-free". Tests must assert the former.
** 10. Accepted lazy window: after lj_arena_podsweep's bitmap transform,
**     if freelist == NULL free runs may be re-poisoned lazily at the
**     first scavenge / freelist build. Fuzzer failures in this window
**     are poisoning-boundary bugs to fix, not real UAFs to ignore.
** 11. Detection granularity (documented limitation): CellSize=16 and no
**     per-object redzones inside rounded blocks, so ASAN catches full
**     UAF of freed blocks plus cell-boundary / past-frontier overflow.
**     Intra-cell / within-rounded-block adjacent-object OOB is OUT of
**     scope (would require layout redzones; forbidden by design).
**
** Out of scope: the non-GC dlmalloc heap (lj_alloc.c), JIT machine code
** (lj_mcode.c), and the assembly interpreter. LUAJIT_USE_SYSMALLOC must
** NOT be combined with GCARENA (lj_arch.h disables LJ_HASGCARENA); the
** ASAN recipe uses TARGET_CFLAGS/TARGET_LDFLAGS, never XCFLAGS alone.
*/

/* -- Poison / unpoison helpers ------------------------------------------- */
#if LJ_USE_ASAN
#define lj_asan_poison(p, sz)   __asan_poison_memory_region((p), (sz))
#define lj_asan_unpoison(p, sz) __asan_unpoison_memory_region((p), (sz))
#else
#define lj_asan_poison(p, sz)   ((void)0)
#define lj_asan_unpoison(p, sz) ((void)0)
#endif

/* -- ASAN-safe freelist linkword accessors ------------------------------- */
/*
** Binned free cells store the GCCellID1 next-id in the first 2 bytes of
** the cell. The block is fully poisoned, so a raw *(GCCellID1 *) deref
** would trip use-after-poison. The ASAN shadow granule is 8 bytes, so we
** unpoison the 8-byte granule at the cell head (covering the 2-byte link
** plus 6 padding bytes inside the same granule), do the access, then
** re-poison. Without ASAN these are plain loads/stores.
**
** Only for binned freelist next-id slots on free cells. Never use on
** gray-stack buffers, allocated object fields, or ranged free blocks
** (ranged blocks keep their link in fl->ranges[], not in the cell).
*/
#if LJ_USE_ASAN
static LJ_AINLINE GCCellID1 arena_linkword_get(GCArena *a, GCCellID c)
{
  void *p = arena_cellptr(a, c);
  GCCellID1 v;
  __asan_unpoison_memory_region(p, 8);
  v = *(GCCellID1 *)p;
  __asan_poison_memory_region(p, 8);
  return v;
}
static LJ_AINLINE void arena_linkword_set(GCArena *a, GCCellID c, GCCellID1 v)
{
  void *p = arena_cellptr(a, c);
  __asan_unpoison_memory_region(p, 8);
  *(GCCellID1 *)p = v;
  __asan_poison_memory_region(p, 8);
}
#else
#define arena_linkword_get(a, c)      (*(GCCellID1 *)arena_cellptr((a), (c)))
#define arena_linkword_set(a, c, v)   (*(GCCellID1 *)arena_cellptr((a), (c)) = (v))
#endif

#endif
