#ifndef LUAJIT_LJ_IO_PATCH_H
#define LUAJIT_LJ_IO_PATCH_H
#include <stdio.h>
#include <stdint.h>
#include "lj_arch.h"

#if LJ_DS_IO_FOPEN_PATCH
LUA_DATA_API extern FILE *(*lj_fopen)(char const *f, const char *mode);
LUA_DATA_API extern int (*lj_fclose)(FILE *);
LUA_DATA_API extern int (*lj_fscanf)(FILE *const _Stream, char const *const _Format, ...);
LUA_DATA_API extern char *(*lj_fgets)(char *_Buffer, int _MaxCount, FILE *_Stream);
LUA_DATA_API extern size_t (*lj_fread)(
    void *_Buffer,
    size_t _ElementSize,
    size_t _ElementCount,
    FILE *_Stream);
LUA_DATA_API extern size_t (*lj_fwrite)(
    void const *_Buffer,
    size_t _ElementSize,
    size_t _ElementCount,
    FILE *_Stream);

LUA_DATA_API extern int (*lj_ferror)
(FILE *_Stream);

LUA_DATA_API extern int (*lj_feof)(
    FILE* _Stream
    );

#if LJ_TARGET_OSX
LUA_DATA_API extern int (*lj_fseeko)(FILE *__stream, off_t __off, int __whence);
LUA_DATA_API extern off_t (*lj_ftello)(FILE *_Stream);
#elif LJ_TARGET_POSIX
LUA_DATA_API extern int (*lj_fseeko)(FILE *__stream, __off_t __off, int __whence);
LUA_DATA_API extern __off64_t (*lj_ftello)(FILE *_Stream);
#elif _MSC_VER >= 1400
LUA_DATA_API extern int (*lj_fseeki64)(
    FILE *_Stream,
    __int64 _Offset,
    int _Origin);
LUA_DATA_API extern __int64 (*lj_ftelli64)(FILE *_Stream);
#endif
LUA_DATA_API extern void (*lj_clearerr)(FILE* fp);
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
