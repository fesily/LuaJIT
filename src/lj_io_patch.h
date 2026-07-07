#ifndef LUAJIT_LJ_IO_PATCH_H
#define LUAJIT_LJ_IO_PATCH_H
#include <stdio.h>
#include <stdint.h>
#include "lj_arch.h"

#if LJ_DS_IO_FOPEN_PATCH
#ifdef LJ_IO_PATCH_IMPLEMENTATION
#define LJ_IO_PATCH_STORAGE LUA_DATA_API
#define LJ_IO_PATCH_DATA(init) = init
#else
#define LJ_IO_PATCH_STORAGE extern LUA_DATA_API
#define LJ_IO_PATCH_DATA(init)
#endif
LJ_IO_PATCH_STORAGE FILE *(*lj_fopen)(char const *f, const char *mode) LJ_IO_PATCH_DATA(fopen);
LJ_IO_PATCH_STORAGE int (*lj_fclose)(FILE *) LJ_IO_PATCH_DATA(fclose);
LJ_IO_PATCH_STORAGE int (*lj_fscanf)(FILE *const _Stream, char const *const _Format, ...) LJ_IO_PATCH_DATA(fscanf);
LJ_IO_PATCH_STORAGE char *(*lj_fgets)(char *_Buffer, int _MaxCount, FILE *_Stream) LJ_IO_PATCH_DATA(fgets);
LJ_IO_PATCH_STORAGE size_t (*lj_fread)(
    void *_Buffer,
    size_t _ElementSize,
    size_t _ElementCount,
    FILE *_Stream) LJ_IO_PATCH_DATA(fread);
LJ_IO_PATCH_STORAGE size_t (*lj_fwrite)(
    void const *_Buffer,
    size_t _ElementSize,
    size_t _ElementCount,
    FILE *_Stream) LJ_IO_PATCH_DATA(fwrite);

LJ_IO_PATCH_STORAGE int (*lj_ferror)(FILE *_Stream) LJ_IO_PATCH_DATA(ferror);
LJ_IO_PATCH_STORAGE int (*lj_feof)(FILE* _Stream) LJ_IO_PATCH_DATA(feof);

#if LJ_TARGET_OSX
LJ_IO_PATCH_STORAGE int (*lj_fseeko)(FILE *__stream, off_t __off, int __whence) LJ_IO_PATCH_DATA(fseeko);
LJ_IO_PATCH_STORAGE off_t (*lj_ftello)(FILE *_Stream) LJ_IO_PATCH_DATA(ftello);
#elif LJ_TARGET_POSIX
LJ_IO_PATCH_STORAGE int (*lj_fseeko)(FILE *__stream, __off_t __off, int __whence) LJ_IO_PATCH_DATA(fseeko);
LJ_IO_PATCH_STORAGE __off64_t (*lj_ftello)(FILE *_Stream) LJ_IO_PATCH_DATA(ftello);
#elif _MSC_VER >= 1400
LJ_IO_PATCH_STORAGE int (*lj_fseeki64)(
    FILE *_Stream,
    __int64 _Offset,
    int _Origin) LJ_IO_PATCH_DATA(_fseeki64);
LJ_IO_PATCH_STORAGE __int64 (*lj_ftelli64)(FILE *_Stream) LJ_IO_PATCH_DATA(_ftelli64);
#endif
LJ_IO_PATCH_STORAGE void (*lj_clearerr)(FILE* fp) LJ_IO_PATCH_DATA(clearerr);
#undef LJ_IO_PATCH_STORAGE
#undef LJ_IO_PATCH_DATA
#else
#define lj_fopen fopen
#define lj_fclose fclose
#define lj_fscanf fscanf
#define lj_fgets fgets
#define lj_fread fread
#define lj_fwrite fwrite
#define lj_ferror ferror
#define lj_ftello ftello
#define lj_fseeko fseeko
#define lj_fseeki64 fseeki64
#define lj_ftelli64 ftelli64
#define lj_clearerr clearerr
#define lj_feof feof
#endif

#endif
