#ifndef LUAJIT_LJ_IO_PATCH_H
#define LUAJIT_LJ_IO_PATCH_H
#include <stdio.h>
#include <stdint.h>
#include "lj_arch.h"

#if LJ_DS_IO_FOPEN_PATCH
LUA_DATA_API FILE *(*lj_fopen)(char const *f, const char *mode) = fopen;
LUA_DATA_API int (*lj_fclose)(FILE *) = fclose;
LUA_DATA_API int (*lj_fscanf)(FILE *const _Stream, char const *const _Format, ...) = fscanf;
LUA_DATA_API char *(*lj_fgets)(char *_Buffer, int _MaxCount, FILE *_Stream) = fgets;
LUA_DATA_API size_t (*lj_fread)(
    void *_Buffer,
    size_t _ElementSize,
    size_t _ElementCount,
    FILE *_Stream) = fread;
LUA_DATA_API size_t (*lj_fwrite)(
    void const *_Buffer,
    size_t _ElementSize,
    size_t _ElementCount,
    FILE *_Stream) = fwrite;

LUA_DATA_API int (*lj_ferror)
(FILE *_Stream) = ferror;

LUA_DATA_API int (*lj_feof)(
    FILE* _Stream
    ) = feof;

#if LJ_TARGET_OSX
LUA_DATA_API int (*lj_fseeko)(FILE *__stream, off_t __off, int __whence) = fseeko;
LUA_DATA_API off_t (*lj_ftello)(FILE *_Stream) = ftello;
#elif LJ_TARGET_POSIX
LUA_DATA_API int (*lj_fseeko)(FILE *__stream, __off_t __off, int __whence) = fseeko;
LUA_DATA_API __off64_t (*lj_ftello)(FILE *_Stream) = ftello;
#elif _MSC_VER >= 1400
LUA_DATA_API int (*lj_fseeki64)(
    FILE *_Stream,
    __int64 _Offset,
    int _Origin) = _fseeki64;
LUA_DATA_API __int64 (*lj_ftelli64)(FILE *_Stream) = _ftelli64;
#endif
LUA_DATA_API void (*lj_clearerr)(FILE* fp) = clearerr;
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
