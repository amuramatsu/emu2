#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <setjmp.h>
#include <string.h>
#include <stdarg.h>

#define USE_TSC 1
#define USE_FPU 1
#define USE_MMX 1
#define USE_SSE 1
#define USE_SSE2 1
#define USE_SSE3 1
#define USE_SSSE3 1
#define USE_SSE4A 1
#define SUPPORT_FPU_SOFTFLOAT 1
#define FPU_TYPE_SOFTFLOAT 0

typedef int      INT;
typedef unsigned int UINT;

typedef uint8_t  BYTE;
typedef uint8_t  UINT8;
typedef uint16_t UINT16;
typedef uint32_t UINT32;
typedef uint64_t UINT64;
typedef int8_t   SINT8;
typedef int16_t  SINT16;
typedef int32_t  SINT32;
typedef int64_t  SINT64;
typedef int8_t   INT8;
typedef int16_t  INT16;
typedef int32_t  INT32;
typedef int64_t  INT64;

typedef int BOOL;
#define TRUE 1
#define FALSE 0

#define IOINPCALL
#define IOOUTCALL
#define INLINE inline

#define __ASSERT assert

#define ZeroMemory(p, s) memset(p, 0, s)
#define MAX(a,b) ((a)<(b)?(b):(a))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define milstr_ncat(buf, str, n) strlcat(buf, str, n)

#define _MALLOC(s,msg) malloc(s)
#define _MFREE(p) free(p)

#define VERBOSE(s)
#define TRACEOUT(s)

#define msgbox(s1, s2) fprintf(stderr, "%s:%s\n", (s1), (s2))

uint8_t read_port(unsigned port);
void write_port(unsigned port, uint8_t value);
static INLINE UINT8 iocore_inp8(unsigned port) {
    return read_port(port);
}
static INLINE UINT16 iocore_inp16(unsigned port) {
    UINT16 result;
    result = read_port(port);
    result |= read_port(port+1) << 8;
    return result;
}
static INLINE UINT32 iocore_inp32(unsigned port) {
    UINT32 result;
    result = read_port(port);
    result |= read_port(port+1) << 8;
    result |= read_port(port+2) << 16;
    result |= read_port(port+3) << 24;
    return result;
}
static INLINE void iocore_out8(unsigned port, UINT8 val) {
    write_port(port, val);
}
static INLINE void iocore_out16(unsigned port, UINT16 val) {
    write_port(port, val & 0xff);
    write_port(port+1, val >> 8);
}
static INLINE void iocore_out32(unsigned port, UINT32 val) {
    write_port(port, val & 0xff);
    write_port(port+1, (val >> 8) & 0xff);
    write_port(port+2, (val >> 16) & 0xff);
    write_port(port+3, val >> 24);
}

#include "common.h"
