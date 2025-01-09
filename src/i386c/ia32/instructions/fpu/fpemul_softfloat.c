/*
 * Copyright (c) 2012 NONAKA Kimihiro
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * modified by SimK
 */

#include <compiler.h>

#if defined(USE_FPU) && defined(SUPPORT_FPU_SOFTFLOAT)

#include <float.h>
#include <math.h>
#include <ia32/cpu.h>
#include "ia32/ia32.mcr"

#include "ia32/instructions/fpu/fp.h"
#include "ia32/instructions/fpu/fpumem.h"
#ifdef USE_SSE
#include "ia32/instructions/sse/sse.h"
#endif

 /*
 Short Real
	31: sign (符号)
 30-23: exp-8 (指数部: exponet)
 22-00: num-23 (小数部)

 Long Real
	63: sign
 62-52: exp-11
 51-00: num-52

 Temp Real
	79: sign
 78-64: exp-15
	63: 1(?)
 62-00: num-63

 --
 指数部:
 2 の 0 乗のとき 0111 1111 となる
 1000 0001: +2 乗
 1000 0000: +1 乗
 0111 1111:  0 乗
 0111 1110: -1 乗

 仮数部:
 2 を基数として整数部が一桁になるように正規化した数の 2 進数表現となる。
 正規化によって仮数部の最上位ビットは常に 1 になるので実際に用意しておく
 必要はなく、倍精度の 52 ビットであれば最上位の 1 を hidden bit にして
 含めなければ、53 ビット分の情報が含まれることになる。

 小数の二進数表現:
 0.1000    1/2         = 0.5
 0.0100    1/(2*2)     = 0.25
 0.0010    1/(2*2*2)   = 0.125
 0.0001    1/(2*2*2*2) = 0.0625
 */

#if 1
#undef	TRACEOUT
#define	TRACEOUT(s)	(void)(s)
#endif	/* 0 */

#define FPU_WORKCLOCK	6

#define PI		3.1415926535897932384626433832795
#define L2E		1.4426950408889634073599246810019
#define L2T		3.3219280948873623478703194294894
#define LN2		0.6931471805599453094172321214582
#define LG2		0.3010299956639811952137388947245
static const uint64_t VALUE_0_MANTISSA = 0x0ULL;
static const uint16_t VALUE_0_EXPONENT = 0x0U;
static const uint64_t VALUE_1_MANTISSA = 0x8000000000000000ULL;
static const uint16_t VALUE_1_EXPONENT = 0x3fffU;
static const uint64_t VALUE_PI_MANTISSA = 0xc90fdaa22168c235ULL;
static const uint16_t VALUE_PI_EXPONENT = 0x4000U;
static const uint64_t VALUE_L2T_MANTISSA = 0xd49a784bcd1b8afeULL;
static const uint16_t VALUE_L2T_EXPONENT = 0x4000U;
static const uint64_t VALUE_L2E_MANTISSA = 0xb8aa3b295c17f0bcULL;
static const uint16_t VALUE_L2E_EXPONENT = 0x3fffU;
static const uint64_t VALUE_LN2_MANTISSA = 0xb17217f7d1cf79acULL;
static const uint16_t VALUE_LN2_EXPONENT = 0x3ffeU;
static const uint64_t VALUE_LG2_MANTISSA = 0x9a209a84fbcff799ULL;
static const uint16_t VALUE_LG2_EXPONENT = 0x3ffdU;

static INLINE UINT16 exception_softfloat_to_x87(uint8_t in)
{
	UINT16 result = 0;
	if ((in & softfloat_flag_inexact) != 0)
		result |= FP_PE_FLAG;
	if ((in & softfloat_flag_underflow) != 0)
		result |= FP_UE_FLAG;
	if ((in & softfloat_flag_overflow) != 0)
		result |= FP_OE_FLAG;
	if ((in & softfloat_flag_infinite) != 0)
		result |= FP_ZE_FLAG;
	if ((in & softfloat_flag_invalid) != 0)
		result |= FP_IE_FLAG;
	return result;
}

static INLINE uint8_t exception_x87_to_softfloat(UINT16 in)
{
	uint8_t result = 0;
	if ((in & FP_IE_FLAG) != 0)
		result |= softfloat_flag_invalid;
	//if ((in & FP_DE_FLAG) != 0)
	//	result |= softfloat_flag_inexact;
	if ((in & FP_ZE_FLAG) != 0)
		result |= softfloat_flag_infinite;
	if ((in & FP_OE_FLAG) != 0)
		result |= softfloat_flag_overflow;
	if ((in & FP_UE_FLAG) != 0)
		result |= softfloat_flag_underflow;
	if ((in & FP_PE_FLAG) != 0)
		result |= softfloat_flag_inexact;
	return result;
}

static INLINE sw_extFloat80_t mem_to_extF80(const void *src)
{
	const unsigned char *sp = src;
	struct extFloat80M result;
	result.signif = LOADINTELQWORD(sp);
	result.signExp = LOADINTELWORD(sp+8);
	return *((sw_extFloat80_t *)&result);
}

static INLINE sw_extFloat80_t REG80_to_extF80(REG80 src)
{
	struct extFloat80M result;
	result.signif = LOADINTELQWORD(&src.w[0]);
	result.signExp = LOADINTELWORD(&src.w[4]);
	return *((sw_extFloat80_t *)&result);
}

static INLINE void extF80_to_mem(const sw_extFloat80_t src, void *dst)
{
	const struct extFloat80M *sp = (struct extFloat80M *)&src;
	unsigned char *dp = (unsigned char *)dst;
	STOREINTELQWORD(dp, sp->signif);
	STOREINTELWORD(dp+8, sp->signExp);
}

static INLINE REG80 extF80M_to_REG80(const sw_extFloat80_t *src)
{
	REG80 result;
	struct extFloat80M *sp = (struct extFloat80M *)src;
	STOREINTELQWORD(&result.w[0], sp->signif);
	STOREINTELWORD(&result.w[4], sp->signExp);
	return result;
}

static INLINE REG80 extF80_to_REG80(const sw_extFloat80_t src)
{
	return extF80M_to_REG80(&src);
}

static INLINE sw_extFloat80_t cf32_to_extF80(float Value)
{
	return f32_to_extF80(*(sw_float32_t*)&Value);
}
static INLINE sw_extFloat80_t cf64_to_extF80(double Value)
{
	return f64_to_extF80(*(sw_float64_t*)&Value);
}
static INLINE void cf32_to_extF80M(float Value, sw_extFloat80_t *Dst)
{
	f32_to_extF80M(*(sw_float32_t*)&Value, Dst);
}
static INLINE void cf64_to_extF80M(double Value, sw_extFloat80_t *Dst)
{
	f64_to_extF80M(*(sw_float64_t*)&Value, Dst);
}
static INLINE float extF80M_to_cf32(const sw_extFloat80_t *Value)
{
	sw_float32_t r = extF80M_to_f32(Value);
	return *(float *)&r;
}
static INLINE double extF80M_to_cf64(const sw_extFloat80_t *Value)
{
	sw_float64_t r = extF80M_to_f64(Value);
	return *(double *)&r;
}
static INLINE int extF80M_isInf(const sw_extFloat80_t *Value)
{
	const struct extFloat80M *s;
	UINT16 ui64;
	UINT64 ui0;
	INT32 exp;
	
	s = (const struct extFloat80M *)Value;
	ui64 = s->signExp;
	exp = ui64 & 0x7FFF;
	ui0 = s->signif;
	if (exp == 0x7FFF) {
		if (ui0 & 0x7FFFFFFFFFFFFFFFUL)
			return 0;
		return 1;
	}
	return 0;
}

static INLINE void FPU_SetCW(UINT16 cword)
{
	FPU_CTRLWORD = cword & 0x7FFF;
	FPU_STAT.round = (FP_RND)((cword >> 10) & 3);
	switch (FPU_STAT.round) {
	case ROUND_Nearest:
		softfloat_roundingMode = softfloat_round_near_even;
		break;
	case ROUND_Down:
		softfloat_roundingMode = softfloat_round_min;
		break;
	case ROUND_Up:
		softfloat_roundingMode = softfloat_round_max;
		break;
	case ROUND_Chop:
		softfloat_roundingMode = softfloat_round_minMag;
		break;
	default:
		break;
	}
	FPU_STAT.precision = (FP_PC)((cword >> 8) & 3);
	switch (FPU_STAT.precision) {
	case PRECISION_64:
		extF80_roundingPrecision = 80;
		break;
	case PRECISION_53:
		extF80_roundingPrecision = 64;
		break;
	case PRECISION_24:
		extF80_roundingPrecision = 32;
		break;
	default:
		extF80_roundingPrecision = 80;
		break;
	}
}

/*
 * FPU exception
 */

static void
fpu_check_NM_EXCEPTION(){
	// タスクスイッチまたはエミュレーション時にNM(デバイス使用不可例外)を発生させる
	if ((CPU_CR0 & (CPU_CR0_TS)) || (CPU_CR0 & CPU_CR0_EM)) {
		EXCEPTION(NM_EXCEPTION, 0);
	}
}
static void
fpu_check_NM_EXCEPTION2(){
	// タスクスイッチまたはエミュレーション時にNM(デバイス使用不可例外)を発生させる
	if ((CPU_CR0 & (CPU_CR0_TS)) || (CPU_CR0 & CPU_CR0_EM)) {
		EXCEPTION(NM_EXCEPTION, 0);
	}
}

static void fpu_checkexception() {
	if ((FPU_STATUSWORD & ~FPU_CTRLWORD) & 0x3F) {
		EXCEPTION(MF_EXCEPTION, 0);
	}
}


/*
 * FPU memory access function
 */

static void FPU_FLD80(UINT32 addr, UINT reg)
{
	*((REG80*)FPU_STAT.reg[reg].b) = fpu_memoryread_f(addr);
}

static void FPU_FLD_F32(UINT32 addr, UINT reg) {
	cf32_to_extF80M(fpu_memoryread_f32(addr), &FPU_STAT.reg[reg].d);
}

static void FPU_FLD_F64(UINT32 addr, UINT reg) {
	cf64_to_extF80M(fpu_memoryread_f64(addr), &FPU_STAT.reg[reg].d);
}

static void FPU_FLD_F80(UINT32 addr) {
	FPU_STAT.reg[FPU_STAT_TOP].d = REG80_to_extF80(fpu_memoryread_f(addr));
}

static void FPU_FLD_I16(UINT32 addr, UINT reg) {
	i32_to_extF80M((SINT32)((SINT16)fpu_memoryread_w(addr)), &FPU_STAT.reg[reg].d);
}

static void FPU_FLD_I32(UINT32 addr, UINT reg) {
	i32_to_extF80M((SINT32)fpu_memoryread_d(addr), &FPU_STAT.reg[reg].d);
}

static void FPU_FLD_I64(UINT32 addr, UINT reg) {
	i64_to_extF80M((SINT64)fpu_memoryread_q(addr), &FPU_STAT.reg[reg].d);
}

static void FPU_FBLD(UINT32 addr, UINT reg)
{
	int i;
	int tmp;

	REG80 bcdbuf;
	SINT64 val = 0;
	UINT8 in = 0;

	// 80bitまとめて読み取り
	bcdbuf = fpu_memoryread_f(addr);

	// 0～8byte目の処理 BCD
	for (i = 8; i >= 0; i--) {
		in = bcdbuf.b[i];
		tmp = ((in >> 4) & 0xf) * 10 + (in & 0xf);
		val = val * 100 + tmp;
	}

	// 9byte目は符号のみ意味がある
	if (bcdbuf.b[9] & 0x80) {
		val = -val;
	}

	i64_to_extF80M(val, &FPU_STAT.reg[reg].d);
}

static INLINE void FPU_FLD_F32_EA(UINT32 addr) {
	FPU_FLD_F32(addr, 8);
}
static INLINE void FPU_FLD_F64_EA(UINT32 addr) {
	FPU_FLD_F64(addr, 8);
}
static INLINE void FPU_FLD_I32_EA(UINT32 addr) {
	FPU_FLD_I32(addr, 8);
}
static INLINE void FPU_FLD_I16_EA(UINT32 addr) {
	FPU_FLD_I16(addr, 8);
}


static void FPU_ST80(UINT32 addr, UINT reg)
{
	fpu_memorywrite_f(addr, (REG80*)FPU_STAT.reg[reg].b);
}

static void FPU_FST_F32(UINT32 addr) {
	fpu_memorywrite_f32(addr, extF80M_to_cf32(&FPU_STAT.reg[FPU_STAT_TOP].d));
}

static void FPU_FST_F64(UINT32 addr) {
	fpu_memorywrite_f64(addr, extF80M_to_cf64(&FPU_STAT.reg[FPU_STAT_TOP].d));
}

static void FPU_FST_F80(UINT32 addr) {
	REG80 d = extF80M_to_REG80(&FPU_STAT.reg[FPU_STAT_TOP].d);
	fpu_memorywrite_f(addr, &d);
}

static void FPU_FST_I16(UINT32 addr) {
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	fpu_memorywrite_w(addr, (UINT16)((SINT16)extF80M_to_i32_r_minMag(&FPU_STAT.reg[FPU_STAT_TOP].d, false)));
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
}

static void FPU_FST_I32(UINT32 addr) {
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	fpu_memorywrite_d(addr, (UINT32)extF80M_to_i32_r_minMag(&FPU_STAT.reg[FPU_STAT_TOP].d, false));
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
}

static void FPU_FST_I64(UINT32 addr) {
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	fpu_memorywrite_q(addr, (UINT64)extF80M_to_i64_r_minMag(&FPU_STAT.reg[FPU_STAT_TOP].d, false));
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
}

static void FPU_FBST(UINT32 addr)
{
	SINT64 val;
	REG80 bcdbuf = { 0 };
	UINT i;

	unsigned char oldrnd = softfloat_roundingMode;
	softfloat_roundingMode = softfloat_round_min;

	val = extF80M_to_i64_r_minMag(&FPU_STAT.reg[FPU_STAT_TOP].d, false);

	// 9byte目は符号のみ意味がある
	if (val < 0)
	{
		bcdbuf.b[9] = 0x80;
		val = -val;
	}

	// 0～8byte目の処理 BCD
	for (i = 0; i < 9; i++) {
		bcdbuf.b[i] = (UINT8)(val % 10);
		val /= 10;
		bcdbuf.b[i] |= (UINT8)(val % 10) << 4;
		val /= 10;
	}

	// 80bitまとめて書き込み
	fpu_memorywrite_f(addr, &bcdbuf);

	softfloat_roundingMode = oldrnd;
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
}


/*
 * FPU interface
 */

static void
FPU_FINIT(void)
{
	int i;
	FPU_SetCW(0x37F);
	FPU_STATUSWORD = 0;
	FPU_STAT_TOP=FP_TOP_GET();
	for(i=0;i<8;i++){
		// Emptyセットしてもレジスタの内容は消してはいけない
		FPU_STAT.tag[i] = TAG_Empty;
	}
	FPU_STAT.tag[8] = TAG_Valid; // dummy
	FPU_STAT.mmxenable = 0;
}
void SF_FPU_FINIT(void){
	int i;
	FPU_FINIT();
	for(i=0;i<8;i++){
		FPU_STAT.tag[i] = TAG_Empty;
		FPU_STAT.reg[i].l.ext = 0;
		FPU_STAT.reg[i].l.lower = 0;
		FPU_STAT.reg[i].l.upper = 0;
	}
}

static void FPU_FLDCW(UINT32 addr)
{
	UINT16 temp = cpu_vmemoryread_w(CPU_INST_SEGREG_INDEX, addr);
	FPU_SetCW(temp);
}

static UINT16 FPU_GetTag(void)
{
	UINT i;

	UINT16 tag = 0;
	for (i = 0; i < 8; i++)
		tag |= ((FPU_STAT.tag[i] & 3) << (2 * i));
	return tag;
}
static UINT8 FPU_GetTag8(void)
{
	UINT i;

	UINT8 tag = 0;
	for (i = 0; i < 8; i++)
		tag |= ((FPU_STAT.tag[i] == TAG_Empty ? 0 : 1) << (i));
	return tag;
}

static INLINE void FPU_SetTag(UINT16 tag)
{
	UINT i;

	for (i = 0; i < 8; i++) {
		FPU_STAT.tag[i] = (FP_TAG)((tag >> (2 * i)) & 3);

	}
}
static INLINE void FPU_SetTag8(UINT8 tag)
{
	UINT i;

	for (i = 0; i < 8; i++) {
		FPU_STAT.tag[i] = (((tag >> i) & 1) == 0 ? TAG_Empty : TAG_Valid);
	}
}

static void FPU_prepush(void) {
	if(FPU_STAT_TOP == 0){
		FPU_STATUSWORD |= FP_C1_FLAG;
	}else{
		FPU_STATUSWORD &= ~FP_C1_FLAG;
	}
	FPU_STAT_TOP = (FPU_STAT_TOP - 1) & 7;
	FPU_STAT.tag[FPU_STAT_TOP] = TAG_Valid;
}
static void FPU_push(sw_extFloat80_t in) {
	FPU_prepush();
	FPU_STAT.reg[FPU_STAT_TOP].d = in;
}

static void FPU_pop(void) {
	FPU_STAT.tag[FPU_STAT_TOP] = TAG_Empty;
	FPU_STAT.mmxenable = 0;
	FPU_STAT_TOP = ((FPU_STAT_TOP + 1) & 7);
}

/*
 * FPU instruction
 */

 // レジスタ操作
static void FPU_FST(UINT st, UINT other) {
	FPU_STAT.tag[other] = FPU_STAT.tag[st];
	FPU_STAT.reg[other] = FPU_STAT.reg[st];
}
static void FPU_FXCH(UINT st, UINT other) {
	FP_TAG tag;
	FP_REG reg;

	tag = FPU_STAT.tag[other];
	reg = FPU_STAT.reg[other];
	FPU_STAT.tag[other] = FPU_STAT.tag[st];
	FPU_STAT.reg[other] = FPU_STAT.reg[st];
	FPU_STAT.tag[st] = tag;
	FPU_STAT.reg[st] = reg;
}
static void FPU_FLD1(void) {
	struct extFloat80M *p;
	FPU_prepush();
	p = (struct extFloat80M *)&FPU_STAT.reg[FPU_STAT_TOP].d;
	p->signif = VALUE_1_MANTISSA;
	p->signExp = VALUE_1_EXPONENT;
	//cf64_to_extF80M(1.0, (sw_extFloat80_t *)p);
}
static void FPU_FLDL2T(void) {
	struct extFloat80M *p;
	FPU_prepush();
	p = (struct extFloat80M *)&FPU_STAT.reg[FPU_STAT_TOP].d;
	p->signif = VALUE_L2T_MANTISSA;
	p->signExp = VALUE_L2T_EXPONENT;
	//cf64_to_extF80M(L2T, (sw_extFloat80_t *)p);
}
static void FPU_FLDL2E(void) {
	struct extFloat80M *p;
	FPU_prepush();
	p = (struct extFloat80M *)&FPU_STAT.reg[FPU_STAT_TOP].d;
	p->signif = VALUE_L2E_MANTISSA;
	p->signExp = VALUE_L2E_EXPONENT;
	//cf64_to_extF80M(L2E, (sw_extFloat80_t *)p);
}
static void FPU_FLDPI(void) {
	struct extFloat80M *p;
	FPU_prepush();
	p = (struct extFloat80M *)&FPU_STAT.reg[FPU_STAT_TOP].d;
	p->signif = VALUE_PI_MANTISSA;
	p->signExp = VALUE_PI_EXPONENT;
	//cf64_to_extF80M(PI, (sw_extFloat80_t *)p);
}
static void FPU_FLDLG2(void) {
	struct extFloat80M *p;
	FPU_prepush();
	p = (struct extFloat80M *)&FPU_STAT.reg[FPU_STAT_TOP].d;
	p->signif = VALUE_LG2_MANTISSA;
	p->signExp = VALUE_LG2_EXPONENT;
	//cf64_to_extF80M(LG2, (sw_extFloat80_t *)p);
}
static void FPU_FLDLN2(void) {
	struct extFloat80M *p;
	FPU_prepush();
	p = (struct extFloat80M *)&FPU_STAT.reg[FPU_STAT_TOP].d;
	p->signif = VALUE_LN2_MANTISSA;
	p->signExp = VALUE_LN2_EXPONENT;
	//cf64_to_extF80M(LN2, (sw_extFloat80_t *)p);
}
static void FPU_FLDZ(void) {
	struct extFloat80M *p;
	FPU_prepush();
	p = (struct extFloat80M *)&FPU_STAT.reg[FPU_STAT_TOP].d;
	p->signif = VALUE_0_MANTISSA;
	p->signExp = VALUE_0_EXPONENT;
	//cf64_to_extF80M(0.0, (sw_extFloat80_t *)p);
	FPU_STAT.tag[FPU_STAT_TOP] = TAG_Zero;
	FPU_STAT.mmxenable = 0;
}

// 四則演算
static void FPU_FADD(UINT op1, UINT op2) {
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	extF80M_add(&FPU_STAT.reg[op1].d, &FPU_STAT.reg[op2].d, &FPU_STAT.reg[op1].d);
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	return;
}
static void FPU_FMUL(UINT st, UINT other) {
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	extF80M_mul(&FPU_STAT.reg[st].d, &FPU_STAT.reg[other].d, &FPU_STAT.reg[st].d);
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	return;
}
static void FPU_FSUB(UINT st, UINT other) {
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	extF80M_sub(&FPU_STAT.reg[st].d, &FPU_STAT.reg[other].d, &FPU_STAT.reg[st].d);
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	return;
}
static void FPU_FSUBR(UINT st, UINT other) {
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	extF80M_sub(&FPU_STAT.reg[other].d, &FPU_STAT.reg[st].d, &FPU_STAT.reg[st].d);
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	return;
}
static void FPU_FDIV(UINT st, UINT other) {
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	extF80M_div(&FPU_STAT.reg[st].d, &FPU_STAT.reg[other].d, &FPU_STAT.reg[st].d);
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	return;
}
static void FPU_FDIVR(UINT st, UINT other) {
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	extF80M_div(&FPU_STAT.reg[other].d, &FPU_STAT.reg[st].d, &FPU_STAT.reg[st].d);
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	return;
}
static INLINE void FPU_FADD_EA(UINT op1) {
	FPU_FADD(op1, 8);
}
static INLINE void FPU_FMUL_EA(UINT op1) {
	FPU_FMUL(op1, 8);
}
static INLINE void FPU_FSUB_EA(UINT op1) {
	FPU_FSUB(op1, 8);
}
static INLINE void FPU_FSUBR_EA(UINT op1) {
	FPU_FSUBR(op1, 8);
}
static INLINE void FPU_FDIV_EA(UINT op1) {
	FPU_FDIV(op1, 8);
}
static INLINE void FPU_FDIVR_EA(UINT op1) {
	FPU_FDIVR(op1, 8);
}
static void FPU_FPREM(void) {
	sw_extFloat80_t val, div;
	SINT64 qint;
	UINT8 orig_pc = extF80_roundingPrecision;
	extF80_roundingPrecision = 80;

	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	val = FPU_STAT.reg[FPU_STAT_TOP].d;
	div = FPU_STAT.reg[FPU_ST(1)].d;
	qint = extF80_to_i64_r_minMag(extF80_div(val, div), false); // int(被除数 / 除数) = 商

	FPU_STAT.reg[FPU_STAT_TOP].d = extF80_sub(val, extF80_mul(i64_to_extF80(qint), div)); // 被除数 - 商 x 除数 = 剰余
	FPU_STATUSWORD &= ~(FP_C0_FLAG | FP_C1_FLAG | FP_C2_FLAG | FP_C3_FLAG);
	if(qint & 4) FPU_STATUSWORD |= FP_C0_FLAG; // 商のbit2
	if(qint & 2) FPU_STATUSWORD |= FP_C3_FLAG; // 商のbit1
	if(qint & 1) FPU_STATUSWORD |= FP_C1_FLAG; // 商のbit0
	// C2クリアで完了扱い
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	extF80_roundingPrecision = orig_pc;
}

static void FPU_FPREM1(void) {
#if 1
	sw_extFloat80_t val, div, q;
	SINT64 qint;
	UINT8 orig_pc = extF80_roundingPrecision;
	extF80_roundingPrecision = 80;

	// IEEE 754 剰余　商を最も近い整数値とする。余りが負値になることが有り得る
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	val = FPU_STAT.reg[FPU_STAT_TOP].d;
	div = FPU_STAT.reg[FPU_ST(1)].d;
	q = extF80_add(extF80_div(val, div), cf64_to_extF80(0.5)); // floor(値 + 0.5)で四捨五入 厳密には負値の境界で違うが微々たる差として気にしないことにする。
	qint = extF80M_to_i64(&q, softfloat_round_min, false); // 四捨五入(被除数 / 除数) = 最も整数に近い商
	extF80M_rem(&val, &div, &FPU_STAT.reg[FPU_STAT_TOP].d);
	FPU_STATUSWORD &= ~(FP_C0_FLAG | FP_C1_FLAG | FP_C2_FLAG | FP_C3_FLAG);
	if(qint & 4) FPU_STATUSWORD |= FP_C0_FLAG; // 商のbit2
	if(qint & 2) FPU_STATUSWORD |= FP_C3_FLAG; // 商のbit1
	if(qint & 1) FPU_STATUSWORD |= FP_C1_FLAG; // 商のbit0
	// C2クリアで完了扱い
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	extF80_roundingPrecision = orig_pc;
#else
	sw_extFloat80_t val, div, q;
	SINT64 qint;
	unsigned char oldrnd = softfloat_roundingMode;
	UINT8 orig_pc = extF80_roundingPrecision;
	extF80_roundingPrecision = 80;

	// IEEE 754 剰余　商を最も近い整数値とする。余りが負値になることが有り得る

	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	val = FPU_STAT.reg[FPU_STAT_TOP].d;
	div = FPU_STAT.reg[FPU_ST(1)].d;
	q = extF80_add(extF80_div(val, div), cf64_to_extF80(0.5)); // floor(値 + 0.5)で四捨五入 厳密には負値の境界で違うが微々たる差として気にしないことにする。
	softfloat_roundingMode = softfloat_round_min;
	qint = extF80_to_i64(q, softfloat_round_min, false); // 四捨五入(被除数 / 除数) = 最も整数に近い商

	FPU_STAT.reg[FPU_STAT_TOP].d = extF80_sub(val, extF80_mul(i64_to_extF80(qint), div)); // 被除数 - 商 x 除数 = 剰余
	FPU_STATUSWORD &= ~(FP_C0_FLAG | FP_C1_FLAG | FP_C2_FLAG | FP_C3_FLAG);
	if(qint & 4) FPU_STATUSWORD |= FP_C0_FLAG; // 商のbit2
	if(qint & 2) FPU_STATUSWORD |= FP_C3_FLAG; // 商のbit1
	if(qint & 1) FPU_STATUSWORD |= FP_C1_FLAG; // 商のbit0
	// C2クリアで完了扱い
	softfloat_roundingMode = oldrnd;
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	extF80_roundingPrecision = orig_pc;
#endif
}

// 数学関数
static void FPU_FSIN(void) {
	//UINT8 orig_pc = extF80_roundingPrecision;
	//extF80_roundingPrecision = 80;
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	cf64_to_extF80M(sin(extF80M_to_cf64(&FPU_STAT.reg[FPU_STAT_TOP].d)), &FPU_STAT.reg[FPU_STAT_TOP].d);
	FPU_STATUSWORD &= ~FP_C2_FLAG;
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	//extF80_roundingPrecision = orig_pc;
	return;
}
static void FPU_FCOS(void) {
	//UINT8 orig_pc = extF80_roundingPrecision;
	//extF80_roundingPrecision = 80;
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	cf64_to_extF80M(cos(extF80M_to_cf64(&FPU_STAT.reg[FPU_STAT_TOP].d)), &FPU_STAT.reg[FPU_STAT_TOP].d);
	FPU_STATUSWORD &= ~FP_C2_FLAG;
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	//extF80_roundingPrecision = orig_pc;
	return;
}
static void FPU_FSINCOS(void) {
	double temp;
	//UINT8 orig_pc = extF80_roundingPrecision;
	//extF80_roundingPrecision = 80;

	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	temp = extF80M_to_cf64(&FPU_STAT.reg[FPU_STAT_TOP].d);
	cf64_to_extF80M(sin(temp), &FPU_STAT.reg[FPU_STAT_TOP].d);
	FPU_push(cf64_to_extF80(cos(temp)));
	FPU_STATUSWORD &= ~FP_C2_FLAG;
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	//extF80_roundingPrecision = orig_pc;
	return;
}
static void FPU_FPTAN(void) {
	//UINT8 orig_pc = extF80_roundingPrecision;
	//extF80_roundingPrecision = 80;
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	cf64_to_extF80M(tan(extF80M_to_cf64(&FPU_STAT.reg[FPU_STAT_TOP].d)), &FPU_STAT.reg[FPU_STAT_TOP].d);
	FPU_push(cf64_to_extF80(1.0));
	FPU_STATUSWORD &= ~FP_C2_FLAG;
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	//extF80_roundingPrecision = orig_pc;
	return;
}
static void FPU_FPATAN(void) {
	//UINT8 orig_pc = extF80_roundingPrecision;
	//extF80_roundingPrecision = 80;
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	cf64_to_extF80M(atan2(extF80M_to_cf64(&FPU_STAT.reg[FPU_ST(1)].d), extF80M_to_cf64(&FPU_STAT.reg[FPU_STAT_TOP].d)), &FPU_STAT.reg[FPU_ST(1)].d);
	FPU_pop();
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	//extF80_roundingPrecision = orig_pc;
	return;
}
static void FPU_FSQRT(void) {
	//FSQRT use rounding precision
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	extF80M_sqrt(&FPU_STAT.reg[FPU_STAT_TOP].d, &FPU_STAT.reg[FPU_STAT_TOP].d);
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	return;
}
static void FPU_FRNDINT(void) {
	//UINT8 orig_pc = extF80_roundingPrecision;
	//extF80_roundingPrecision = 80;
	softfloat_exceptionFlags = exception_x87_to_softfloat(FPU_STATUSWORD);
	extF80M_roundToInt(&FPU_STAT.reg[FPU_STAT_TOP].d, softfloat_roundingMode, false, &FPU_STAT.reg[FPU_STAT_TOP].d);
	FPU_STATUSWORD |= exception_softfloat_to_x87(softfloat_exceptionFlags);
	//extF80_roundingPrecision = orig_pc;
}
static void FPU_F2XM1(void) {
	//UINT8 orig_pc = extF80_roundingPrecision;
	//extF80_roundingPrecision = 80;
	cf64_to_extF80M(pow(2.0, extF80M_to_cf64(&FPU_STAT.reg[FPU_STAT_TOP].d)) - 1, &FPU_STAT.reg[FPU_STAT_TOP].d);
	//extF80_roundingPrecision = orig_pc;
}
static void FPU_FYL2X(void) {
	UINT8 orig_pc = extF80_roundingPrecision;
	extF80_roundingPrecision = 80;
	FPU_STAT.reg[FPU_ST(1)].d = extF80_mul(FPU_STAT.reg[FPU_ST(1)].d, cf64_to_extF80(log(extF80M_to_cf64(&FPU_STAT.reg[FPU_STAT_TOP].d)) / log(2.0)));
	FPU_pop();
	extF80_roundingPrecision = orig_pc;
}
static void FPU_FYL2XP1(void) {
	UINT8 orig_pc = extF80_roundingPrecision;
	extF80_roundingPrecision = 80;
	FPU_STAT.reg[FPU_ST(1)].d = extF80_mul(FPU_STAT.reg[FPU_ST(1)].d, cf64_to_extF80(log(extF80M_to_cf64(&FPU_STAT.reg[FPU_STAT_TOP].d) + 1.0) / log(2.0)));
	FPU_pop();
	extF80_roundingPrecision = orig_pc;
}
static void FPU_FSCALE(void) {
	UINT8 orig_pc = extF80_roundingPrecision;
	extF80_roundingPrecision = 80;
	FPU_STAT.reg[FPU_STAT_TOP].d = extF80_mul(FPU_STAT.reg[FPU_STAT_TOP].d, cf64_to_extF80(pow(2.0, extF80M_to_cf64(&FPU_STAT.reg[FPU_ST(1)].d))));
	extF80_roundingPrecision = orig_pc;
}
static void FPU_FCHS(void) {
	struct extFloat80M *p = (struct extFloat80M *)&FPU_STAT.reg[FPU_STAT_TOP].d;
	p->signExp ^= 0x8000;
}
static void FPU_FABS(void) {
	struct extFloat80M *p = (struct extFloat80M *)&FPU_STAT.reg[FPU_STAT_TOP].d;
	p->signExp &= ~0x8000;
}

// 比較
static void FPU_FCOM(UINT st, UINT other) {
	FPU_STATUSWORD &= ~(FP_C0_FLAG | FP_C2_FLAG | FP_C3_FLAG);
	if (((FPU_STAT.tag[st] != TAG_Valid) && (FPU_STAT.tag[st] != TAG_Zero)) ||
		((FPU_STAT.tag[other] != TAG_Valid) && (FPU_STAT.tag[other] != TAG_Zero)) ||
		(extF80M_isSignalingNaN(&FPU_STAT.reg[st].d) || extF80M_isSignalingNaN(&FPU_STAT.reg[other].d))) {
		FPU_STATUSWORD |= FP_C3_FLAG|FP_C2_FLAG|FP_C0_FLAG;
	}
	else if (extF80M_eq(&FPU_STAT.reg[st].d, &FPU_STAT.reg[other].d)) {
		FPU_STATUSWORD |= FP_C3_FLAG;
	}
	else if (extF80M_lt(&FPU_STAT.reg[st].d, &FPU_STAT.reg[other].d)) {
		FPU_STATUSWORD |= FP_C0_FLAG;
	}
}
static void FPU_FCOMI(UINT st, UINT other) {
	CPU_FLAGL &= ~(Z_FLAG|P_FLAG|C_FLAG);
	if (((FPU_STAT.tag[st] != TAG_Valid) && (FPU_STAT.tag[st] != TAG_Zero)) ||
		((FPU_STAT.tag[other] != TAG_Valid) && (FPU_STAT.tag[other] != TAG_Zero)) ||
		(extF80M_isSignalingNaN(&FPU_STAT.reg[st].d) || extF80M_isSignalingNaN(&FPU_STAT.reg[other].d))) {
		CPU_FLAGL |= Z_FLAG|P_FLAG|C_FLAG;
	}
	else if (extF80M_eq(&FPU_STAT.reg[st].d, &FPU_STAT.reg[other].d)) {
		CPU_FLAGL |= Z_FLAG;
	}
	else if (extF80M_lt(&FPU_STAT.reg[st].d, &FPU_STAT.reg[other].d)) {
		CPU_FLAGL |= C_FLAG;
	}
}
static void FPU_FUCOM(UINT st, UINT other) {
	// 例外絡みの挙動が違うがほぼ同じとしてスルー
	FPU_FCOM(st, other);
}
static void FPU_FUCOMI(UINT st, UINT other) {
	// 例外絡みの挙動が違うがほぼ同じとしてスルー
	FPU_FCOMI(st, other);
}
static INLINE void FPU_FCOM_EA(UINT op1) {
	FPU_FCOM(op1, 8);
}
static void FPU_FTST(void) {
	cf64_to_extF80M(0.0, &FPU_STAT.reg[8].d);
	FPU_FCOM(FPU_STAT_TOP, 8);
}

// 条件付きコピー
static void FPU_FCMOVB(UINT st, UINT other) {
	if (CPU_FLAGL & C_FLAG) {
		FPU_STAT.tag[st] = FPU_STAT.tag[other];
		FPU_STAT.reg[st] = FPU_STAT.reg[other];
	}
}
static void FPU_FCMOVE(UINT st, UINT other) {
	if (CPU_FLAGL & Z_FLAG) {
		FPU_STAT.tag[st] = FPU_STAT.tag[other];
		FPU_STAT.reg[st] = FPU_STAT.reg[other];
	}
}
static void FPU_FCMOVBE(UINT st, UINT other) {
	if (CPU_FLAGL & (C_FLAG | Z_FLAG)) {
		FPU_STAT.tag[st] = FPU_STAT.tag[other];
		FPU_STAT.reg[st] = FPU_STAT.reg[other];
	}
}
static void FPU_FCMOVU(UINT st, UINT other) {
	if (CPU_FLAGL & P_FLAG) {
		FPU_STAT.tag[st] = FPU_STAT.tag[other];
		FPU_STAT.reg[st] = FPU_STAT.reg[other];
	}
}
static void FPU_FCMOVNB(UINT st, UINT other) {
	if (!(CPU_FLAGL & C_FLAG)) {
		FPU_STAT.tag[st] = FPU_STAT.tag[other];
		FPU_STAT.reg[st] = FPU_STAT.reg[other];
	}
}
static void FPU_FCMOVNE(UINT st, UINT other) {
	if (!(CPU_FLAGL & Z_FLAG)) {
		FPU_STAT.tag[st] = FPU_STAT.tag[other];
		FPU_STAT.reg[st] = FPU_STAT.reg[other];
	}
}
static void FPU_FCMOVNBE(UINT st, UINT other) {
	if (!(CPU_FLAGL & (C_FLAG | Z_FLAG))) {
		FPU_STAT.tag[st] = FPU_STAT.tag[other];
		FPU_STAT.reg[st] = FPU_STAT.reg[other];
	}
}
static void FPU_FCMOVNU(UINT st, UINT other) {
	if (!(CPU_FLAGL & P_FLAG)) {
		FPU_STAT.tag[st] = FPU_STAT.tag[other];
		FPU_STAT.reg[st] = FPU_STAT.reg[other];
	}
}

// 浮動小数点数操作
static void FPU_FXAM(void) {
	FPU_STATUSWORD &= ~(FP_C0_FLAG | FP_C1_FLAG | FP_C2_FLAG | FP_C3_FLAG);
	if (extF80_lt(FPU_STAT.reg[FPU_STAT_TOP].d, i64_to_extF80(0))) {
		FPU_STATUSWORD |= FP_C1_FLAG;
	}

	if (FPU_STAT.tag[FPU_STAT_TOP] == TAG_Empty) {
		FPU_STATUSWORD |= FP_C3_FLAG;
		FPU_STATUSWORD |= FP_C0_FLAG;
	}
	else if (extF80M_isSignalingNaN(&FPU_STAT.reg[FPU_STAT_TOP].d)) {
		FPU_STATUSWORD |= FP_C0_FLAG;
	}
	else if (extF80M_isInf(&FPU_STAT.reg[FPU_STAT_TOP].d)) {
		FPU_STATUSWORD |= FP_C2_FLAG;
		FPU_STATUSWORD |= FP_C0_FLAG;
	}
	else if (extF80_eq(FPU_STAT.reg[FPU_STAT_TOP].d, cf64_to_extF80(0.0))) {
		FPU_STATUSWORD |= FP_C3_FLAG;
	}
	else {
		FPU_STATUSWORD |= FP_C2_FLAG;
	}
}

static void FPU_FXTRACT(void) {
	SINT32 expval;
	union { struct extFloat80M s; sw_extFloat80_t f; } fracval;
	UINT16 ui64;
	UINT64 ui0;
	INT32 exp;

	fracval.f = FPU_STAT.reg[FPU_STAT_TOP].d;
	ui64 = fracval.s.signExp;
	exp = ui64 & 0x7FFF;
	ui0 = fracval.s.signif;
	expval = (SINT32)((UINT16)exp) - 0x3FFF; // 指数部分を抽出、バイアス分を引く
	fracval.s.signExp = (SINT16)(((UINT16)ui64 & 0x8000) | 0x3FFF); // 符号は残し、指数部分を0x3FFF（バイアス分=0）にして仮数だけにする
	i64_to_extF80M(expval, &FPU_STAT.reg[FPU_STAT_TOP].d); // 指数の書き込み
	FPU_push(fracval.f); // 仮数のpush
}

// 環境ロード・ストア
static void FPU_FSTENV(UINT32 addr)
{
	FP_TOP_SET(FPU_STAT_TOP);

	switch ((CPU_CR0 & 1) | (CPU_INST_OP32 ? 0x100 : 0x000))
	{
	case 0x000: case 0x001:
		fpu_memorywrite_w(addr + 0, FPU_CTRLWORD);
		fpu_memorywrite_w(addr + 2, FPU_STATUSWORD);
		fpu_memorywrite_w(addr + 4, FPU_GetTag());
		fpu_memorywrite_w(addr + 10, FPU_LASTINSTOP);
		break;

	case 0x100: case 0x101:
		fpu_memorywrite_d(addr + 0, (UINT32)(FPU_CTRLWORD));
		fpu_memorywrite_d(addr + 4, (UINT32)(FPU_STATUSWORD));
		fpu_memorywrite_d(addr + 8, (UINT32)(FPU_GetTag()));
		fpu_memorywrite_d(addr + 20, FPU_LASTINSTOP);
		break;
	}
}
static void FPU_FLDENV(UINT32 addr)
{
	switch ((CPU_CR0 & 1) | (CPU_INST_OP32 ? 0x100 : 0x000)) {
	case 0x000: case 0x001:
		FPU_SetCW(fpu_memoryread_w(addr + 0));
		FPU_STATUSWORD = fpu_memoryread_w(addr + 2);
		FPU_SetTag(fpu_memoryread_w(addr + 4));
		FPU_LASTINSTOP = fpu_memoryread_w(addr + 10);
		break;

	case 0x100: case 0x101:
		FPU_SetCW((UINT16)fpu_memoryread_d(addr + 0));
		FPU_STATUSWORD = (UINT16)fpu_memoryread_d(addr + 4);
		FPU_SetTag((UINT16)fpu_memoryread_d(addr + 8));
		FPU_LASTINSTOP = (UINT16)fpu_memoryread_d(addr + 20);
		break;
	}
	FPU_STAT_TOP = FP_TOP_GET();
}
static void FPU_FSAVE(UINT32 addr)
{
	UINT start;
	UINT i;

	FPU_FSTENV(addr);
	start = ((CPU_INST_OP32) ? 28 : 14);
	for (i = 0; i < 8; i++) {
		FPU_ST80(addr + start, FPU_ST(i));
		start += 10;
	}
	FPU_FINIT();
}
static void FPU_FRSTOR(UINT32 addr)
{
	UINT start;
	UINT i;

	FPU_FLDENV(addr);
	start = ((CPU_INST_OP32) ? 28 : 14);
	for (i = 0; i < 8; i++) {
		FPU_FLD80(addr + start, FPU_ST(i));
		start += 10;
	}
}
static void FPU_FXSAVE(UINT32 addr) {
	UINT start;
	UINT i;

	FP_TOP_SET(FPU_STAT_TOP);
	fpu_memorywrite_w(addr + 0, FPU_CTRLWORD);
	fpu_memorywrite_w(addr + 2, FPU_STATUSWORD);
	fpu_memorywrite_b(addr + 4, FPU_GetTag8());
#ifdef USE_SSE
	fpu_memorywrite_d(addr + 24, SSE_MXCSR);
#endif
	start = 32;
	for (i = 0; i < 8; i++) {
		FPU_ST80(addr + start, FPU_ST(i));
		start += 16;
	}
#ifdef USE_SSE
	start = 160;
	for (i = 0; i < 8; i++) {
		fpu_memorywrite_q(addr + start + 0, SSE_XMMREG(i).ul64[0]);
		fpu_memorywrite_q(addr + start + 8, SSE_XMMREG(i).ul64[1]);
		start += 16;
	}
#endif
}
static void FPU_FXRSTOR(UINT32 addr) {
	UINT start;
	UINT i;

	FPU_SetCW(fpu_memoryread_w(addr + 0));
	FPU_STATUSWORD = fpu_memoryread_w(addr + 2);
	FPU_SetTag8(fpu_memoryread_b(addr + 4));
	FPU_STAT_TOP = FP_TOP_GET();
#ifdef USE_SSE
	SSE_MXCSR = fpu_memoryread_d(addr + 24);
#endif
	start = 32;
	for (i = 0; i < 8; i++) {
		FPU_FLD80(addr + start, FPU_ST(i));
		start += 16;
	}
#ifdef USE_SSE
	start = 160;
	for (i = 0; i < 8; i++) {
		SSE_XMMREG(i).ul64[0] = fpu_memoryread_q(addr + start + 0);
		SSE_XMMREG(i).ul64[1] = fpu_memoryread_q(addr + start + 8);
		start += 16;
	}
#endif
}
void SF_FPU_FXSAVERSTOR(void) {
	UINT32 op;
	UINT idx, sub;
	UINT32 maddr;

	CPU_WORKCLOCK(FPU_WORKCLOCK);
	GET_PCBYTE((op));
	idx = (op >> 3) & 7;
	sub = (op & 7);

	fpu_check_NM_EXCEPTION2(); // XXX: 根拠無し
	switch (idx) {
	case 0: // FXSAVE
		maddr = calc_ea_dst(op);
		FPU_FXSAVE(maddr);
		break;
	case 1: // FXRSTOR
		maddr = calc_ea_dst(op);
		FPU_FXRSTOR(maddr);
		break;
#ifdef USE_SSE
	case 2: // LDMXCSR
		maddr = calc_ea_dst(op);
		SSE_LDMXCSR(maddr);
		break;
	case 3: // STMXCSR
		maddr = calc_ea_dst(op);
		SSE_STMXCSR(maddr);
		break;
	case 4: // SFENCE
		SSE_SFENCE();
		break;
	case 5: // LFENCE
		SSE_LFENCE();
		break;
	case 6: // MFENCE
		SSE_MFENCE();
		break;
	case 7: // CLFLUSH;
		SSE_CLFLUSH(op);
		break;
#endif
	default:
		ia32_panic("invalid opcode = %02x\n", op);
		break;
	}
}

static void EA_TREE(UINT op)
{
	UINT idx;

	idx = (op >> 3) & 7;
	
		switch (idx) {
		case 0:	/* FADD (単精度実数) */
			TRACEOUT(("FADD EA"));
			FPU_FADD_EA(FPU_STAT_TOP); 
			break;
		case 1:	/* FMUL (単精度実数) */
			TRACEOUT(("FMUL EA"));
			FPU_FMUL_EA(FPU_STAT_TOP);
			break;
		case 2:	/* FCOM (単精度実数) */
			TRACEOUT(("FCOM EA"));
			FPU_FCOM_EA(FPU_STAT_TOP);
			break;
		case 3:	/* FCOMP (単精度実数) */
			TRACEOUT(("FCOMP EA"));
			FPU_FCOM_EA(FPU_STAT_TOP);
			FPU_pop();
			break;
		case 4:	/* FSUB (単精度実数) */
			TRACEOUT(("FSUB EA"));
			FPU_FSUB_EA(FPU_STAT_TOP);
			break;
		case 5:	/* FSUBR (単精度実数) */
			TRACEOUT(("FSUBR EA"));
			FPU_FSUBR_EA(FPU_STAT_TOP);
			break;
		case 6:	/* FDIV (単精度実数) */
			TRACEOUT(("FDIV EA"));
			FPU_FDIV_EA(FPU_STAT_TOP);
			break;
		case 7:	/* FDIVR (単精度実数) */
			TRACEOUT(("FDIVR EA"));
			FPU_FDIVR_EA(FPU_STAT_TOP);
			break;
		default:
			break;
		}	
}

// d8
void
SF_ESC0(void)
{
	UINT32 op, madr;
	UINT idx, sub;

	CPU_WORKCLOCK(FPU_WORKCLOCK);
	GET_PCBYTE(op);
	TRACEOUT(("use FPU d8 %.2x", op));
	idx = (op >> 3) & 7;
	sub = (op & 7);
	
	fpu_check_NM_EXCEPTION();
	fpu_checkexception();
	if (op >= 0xc0) {
		/* Fxxx ST(0), ST(i) */
		switch (idx) {
		case 0:	/* FADD */
			TRACEOUT(("FADD"));
			FPU_FADD(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 1:	/* FMUL */
			TRACEOUT(("FMUL"));
			FPU_FMUL(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 2:	/* FCOM */
			TRACEOUT(("FCOM"));
			FPU_FCOM(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 3:	/* FCOMP */
			TRACEOUT(("FCOMP"));
			FPU_FCOM(FPU_STAT_TOP,FPU_ST(sub));
			FPU_pop();
			break;
		case 4:	/* FSUB */
			TRACEOUT(("FSUB"));
			FPU_FSUB(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 5:	/* FSUBR */
			TRACEOUT(("FSUBR"));
			FPU_FSUBR(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 6:	/* FDIV */
			TRACEOUT(("FDIV"));
			FPU_FDIV(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 7:	/* FDIVR */
			TRACEOUT(("FDIVR"));
			FPU_FDIVR(FPU_STAT_TOP,FPU_ST(sub));
			break;
		}
	} else {
		madr = calc_ea_dst(op);
		FPU_FLD_F32_EA(madr);
		EA_TREE(op);
	}
}

// d9
void
SF_ESC1(void)
{
	UINT32 op, madr;
	UINT idx, sub;

	CPU_WORKCLOCK(FPU_WORKCLOCK);
	GET_PCBYTE(op);
	TRACEOUT(("use FPU d9 %.2x", op));
	idx = (op >> 3) & 7;
	sub = (op & 7);
	
	fpu_check_NM_EXCEPTION();
	if(!(op < 0xc0 && idx>=4)){
		fpu_checkexception();
	}
	if (op >= 0xc0) 
	{
		switch (idx) {
		case 0: /* FLD ST(0), ST(i) */
			{
				UINT reg_from;
			
				TRACEOUT(("FLD STi"));
				reg_from = FPU_ST(sub);
				FPU_prepush();
				FPU_FST(reg_from, FPU_STAT_TOP);
			}
			break;

		case 1:	/* FXCH ST(0), ST(i) */
			TRACEOUT(("FXCH STi"));
			FPU_FXCH(FPU_STAT_TOP,FPU_ST(sub));
			break;

		case 2: /* FNOP */
			TRACEOUT(("FNOP"));
			break;

		case 3: /* FSTP STi */
			TRACEOUT(("FSTP STi"));
			FPU_FST(FPU_STAT_TOP,FPU_ST(sub));
			FPU_pop();
			break;
		
		case 4:
			switch (sub) {
			case 0x0:	/* FCHS */
				TRACEOUT(("FCHS"));
				FPU_FCHS();
				break;

			case 0x1:	/* FABS */
				TRACEOUT(("FABS"));
				FPU_FABS();
				break;

			case 0x2:  /* UNKNOWN */
			case 0x3:  /* ILLEGAL */
				break;

			case 0x4:	/* FTST */
				TRACEOUT(("FTST"));
				FPU_FTST();
				break;

			case 0x5:	/* FXAM */
				TRACEOUT(("FXAM"));
				FPU_FXAM();
				break;

			case 0x06:       /* FTSTP (cyrix)*/
			case 0x07:       /* UNKNOWN */
				break;
			}
			break;
			
		case 5:
			switch (sub) {
			case 0x0:	/* FLD1 */
				TRACEOUT(("FLD1"));
				FPU_FLD1();
				break;
				
			case 0x1:	/* FLDL2T */
				TRACEOUT(("FLDL2T"));
				FPU_FLDL2T();
				break;
				
			case 0x2:	/* FLDL2E */
				TRACEOUT(("FLDL2E"));
				FPU_FLDL2E();
				break;
				
			case 0x3:	/* FLDPI */
				TRACEOUT(("FLDPI"));
				FPU_FLDPI();
				break;
				
			case 0x4:	/* FLDLG2 */
				TRACEOUT(("FLDLG2"));
				FPU_FLDLG2();
				break;
				
			case 0x5:	/* FLDLN2 */
				TRACEOUT(("FLDLN2"));
				FPU_FLDLN2();
				break;
				
			case 0x6:	/* FLDZ */
				TRACEOUT(("FLDZ"));
				FPU_FLDZ();
				break;
				
			case 0x07: /* ILLEGAL */
				break;
			}
			break;

		case 6:
			switch (sub) {
			case 0x0:	/* F2XM1 */
				TRACEOUT(("F2XM1"));
				FPU_F2XM1();
				break;
				
			case 0x1:	/* FYL2X */
				TRACEOUT(("FYL2X"));
				FPU_FYL2X();
				break;
				
			case 0x2:	/* FPTAN */
				TRACEOUT(("FPTAN"));
				FPU_FPTAN();
				break;
				
			case 0x3:	/* FPATAN */
				TRACEOUT(("FPATAN"));
				FPU_FPATAN();
				break;
				
			case 0x4:	/* FXTRACT */
				TRACEOUT(("FXTRACT"));
				FPU_FXTRACT();
				break;
				
			case 0x5:	/* FPREM1 */
				TRACEOUT(("FPREM1"));
				FPU_FPREM1();
				break;
				
			case 0x6:	/* FDECSTP */
				TRACEOUT(("FDECSTP"));
				FPU_STATUSWORD &= ~FP_C1_FLAG;
				FPU_STAT_TOP = (FPU_STAT_TOP - 1) & 7;
				break;
				
			case 0x7:	/* FINCSTP */
				TRACEOUT(("FINCSTP"));
				FPU_STATUSWORD &= ~FP_C1_FLAG;
				FPU_STAT_TOP = (FPU_STAT_TOP + 1) & 7;
				break;
			}
			break;
			
		case 7:
			switch (sub) {
			case 0x0:	/* FPREM */
				TRACEOUT(("FPREM"));
				FPU_FPREM();
				break;
				
			case 0x1:	/* FYL2XP1 */
				TRACEOUT(("FYL2XP1"));
				FPU_FYL2XP1();
				break;
				
			case 0x2:	/* FSQRT */
				TRACEOUT(("FSQRT"));
				FPU_FSQRT();
				break;
				
			case 0x3:	/* FSINCOS */
				TRACEOUT(("FSINCOS"));
				FPU_FSINCOS();
				break;
				
			case 0x4:	/* FRNDINT */
				TRACEOUT(("FRNDINT"));
				FPU_FRNDINT();
				break;
				
			case 0x5:	/* FSCALE */
				TRACEOUT(("FSCALE"));
				FPU_FSCALE();
				break;
				
			case 0x6:	/* FSIN */
				TRACEOUT(("FSIN"));
				FPU_FSIN();				
				break;
				
			case 0x7:	/* FCOS */
				TRACEOUT(("FCOS"));
				FPU_FCOS();	
				break;
			}
			break;

		default:
			ia32_panic("ESC1: invalid opcode = %02x\n", op);
			break;
		}
	} else {
		madr = calc_ea_dst(op);
		switch (idx) {
		case 0:	/* FLD (単精度実数) */
			TRACEOUT(("FLD float"));
			FPU_prepush();
			FPU_FLD_F32(madr,FPU_STAT_TOP);
			break;
			
		case 1:	/* UNKNOWN */
			break;

		case 2:	/* FST (単精度実数) */
			TRACEOUT(("FST float"));
			FPU_FST_F32(madr);			
			break;

		case 3:	/* FSTP (単精度実数) */
			TRACEOUT(("FSTP float"));
			FPU_FST_F32(madr);
			FPU_pop();
			break;

		case 4:	/* FLDENV */
			TRACEOUT(("FLDENV"));
			FPU_FLDENV(madr);		
			break;

		case 5:	/* FLDCW */
			TRACEOUT(("FLDCW"));
			FPU_FLDCW(madr);
			break;

		case 6:	/* FSTENV */
			TRACEOUT(("FSTENV"));
			FPU_FSTENV(madr);
			break;

		case 7:	/* FSTCW */
			TRACEOUT(("FSTCW/FNSTCW"));
			fpu_memorywrite_w(madr,FPU_CTRLWORD);
			break;

		default:
			break;
		}
	}
}

// da
void
SF_ESC2(void)
{
	UINT32 op, madr;
	UINT idx, sub;

	CPU_WORKCLOCK(FPU_WORKCLOCK);
	GET_PCBYTE(op);
	TRACEOUT(("use FPU da %.2x", op));
	idx = (op >> 3) & 7;
	sub = (op & 7);
	
	fpu_check_NM_EXCEPTION();
	fpu_checkexception();
	if (op >= 0xc0) {
		/* Fxxx ST(0), ST(i) */
		switch (idx) {
		case 0: /* FCMOVB */
			TRACEOUT(("ESC2: FCMOVB"));
			FPU_FCMOVB(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 1: /* FCMOVE */
			TRACEOUT(("ESC2: FCMOVE"));
			FPU_FCMOVE(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 2: /* FCMOVBE */
			TRACEOUT(("ESC2: FCMOVBE"));
			FPU_FCMOVBE(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 3: /* FCMOVU */
			TRACEOUT(("ESC2: FCMOVU"));
			FPU_FCMOVU(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 5:
			switch (sub) {
			case 1: /* FUCOMPP */
				TRACEOUT(("FUCOMPP"));
				FPU_FUCOM(FPU_STAT_TOP,FPU_ST(1));
				FPU_pop();
				FPU_pop();
				break;
				
			default:
				break;
			}
			break;
			
		default:
			break;
		}
	} else {
		madr = calc_ea_dst(op);
		FPU_FLD_I32_EA(madr);
		EA_TREE(op);
	}
}

// db
void
SF_ESC3(void)
{
	UINT32 op, madr;
	UINT idx, sub;

	CPU_WORKCLOCK(FPU_WORKCLOCK);
	GET_PCBYTE(op);
	TRACEOUT(("use FPU db %.2x", op));
	idx = (op >> 3) & 7;
	sub = (op & 7);
	
	fpu_check_NM_EXCEPTION();
	if(!(op >= 0xc0 && idx==4)){
		fpu_checkexception();
	}
	if (op >= 0xc0) 
	{
		/* Fxxx ST(0), ST(i) */
		switch (idx) {
		case 0: /* FCMOVNB */
			TRACEOUT(("ESC3: FCMOVNB"));
			FPU_FCMOVNB(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 1: /* FCMOVNE */
			TRACEOUT(("ESC3: FCMOVNE"));
			FPU_FCMOVNE(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 2: /* FCMOVNBE */
			TRACEOUT(("ESC3: FCMOVNBE"));
			FPU_FCMOVNBE(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 3: /* FCMOVNU */
			TRACEOUT(("ESC3: FCMOVNU"));
			FPU_FCMOVNU(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 4:
			switch (sub) {
			case 0: /* FNENI */
			case 1: /* FNDIS */
				break;
				
			case 2: /* FCLEX */
				TRACEOUT(("FCLEX"));
				FPU_STATUSWORD &= 0x7f00;
				break;
				
			case 3: /* FNINIT/FINIT */
				TRACEOUT(("FNINIT/FINIT"));
				FPU_FINIT();
				break;
				
			case 4: /* FNSETPM */
			case 5: /* FRSTPM */
				// nop
				break;
				
			default:
				break;
			}
			break;
		case 5: /* FUCOMI */
			TRACEOUT(("ESC3: FUCOMI"));
			FPU_FUCOMI(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 6: /* FCOMI */
			TRACEOUT(("ESC3: FCOMI"));
			FPU_FCOMI(FPU_STAT_TOP,FPU_ST(sub));	
			break;
		default:
			break;
		}
	} else {
		madr = calc_ea_dst(op);
		switch (idx) {
		case 0:	/* FILD (DWORD) */
			TRACEOUT(("FILD"));
			FPU_prepush();
			FPU_FLD_I32(madr,FPU_STAT_TOP);
			break;
			
		case 1:	/* FISTTP (DWORD) */
			{
				unsigned char oldrnd = softfloat_roundingMode;
				softfloat_roundingMode = softfloat_round_min;
				FPU_FST_I32(madr);
				softfloat_roundingMode = oldrnd;
			}
			FPU_pop();
			break;
			
		case 2:	/* FIST (DWORD) */
			TRACEOUT(("FIST"));
			FPU_FST_I32(madr);
			break;
			
		case 3:	/* FISTP (DWORD) */
			TRACEOUT(("FISTP"));
			FPU_FST_I32(madr);
			FPU_pop();
			break;
			
		case 5:	/* FLD (拡張実数) */
			TRACEOUT(("FLD 80 Bits Real"));
			FPU_prepush();
			FPU_FLD_F80(madr);
			break;
			
		case 7:	/* FSTP (拡張実数) */
			TRACEOUT(("FSTP 80 Bits Real"));
			FPU_FST_F80(madr);
			FPU_pop();
			break;

		default:
			break;
		}
	}
}

// dc
void
SF_ESC4(void)
{
	UINT32 op, madr;
	UINT idx, sub;

	CPU_WORKCLOCK(FPU_WORKCLOCK);
	GET_PCBYTE(op);
	TRACEOUT(("use FPU dc %.2x", op));
	idx = (op >> 3) & 7;
	sub = (op & 7);
	
	fpu_check_NM_EXCEPTION();
	fpu_checkexception();
	if (op >= 0xc0) {
		/* Fxxx ST(i), ST(0) */
		switch (idx) {
		case 0:	/* FADD */
			TRACEOUT(("ESC4: FADD"));
			FPU_FADD(FPU_ST(sub),FPU_STAT_TOP);
			break;
		case 1:	/* FMUL */
			TRACEOUT(("ESC4: FMUL"));
			FPU_FMUL(FPU_ST(sub),FPU_STAT_TOP);
			break;
		case 2: /* FCOM */
			TRACEOUT(("ESC4: FCOM"));
			FPU_FCOM(FPU_STAT_TOP,FPU_ST(sub));			
			break;
		case 3: /* FCOMP */
			TRACEOUT(("ESC4: FCOMP"));
			FPU_FCOM(FPU_STAT_TOP,FPU_ST(sub));	
			FPU_pop();
			break;
		case 4:	/* FSUBR */
			TRACEOUT(("ESC4: FSUBR"));
			FPU_FSUBR(FPU_ST(sub),FPU_STAT_TOP);
			break;
		case 5:	/* FSUB */
			TRACEOUT(("ESC4: FSUB"));
			FPU_FSUB(FPU_ST(sub),FPU_STAT_TOP);
			break;
		case 6:	/* FDIVR */
			TRACEOUT(("ESC4: FDIVR"));
			FPU_FDIVR(FPU_ST(sub),FPU_STAT_TOP);
			break;
		case 7:	/* FDIV */
			TRACEOUT(("ESC4: FDIV"));
			FPU_FDIV(FPU_ST(sub),FPU_STAT_TOP);
			break;
		default:
			break;
		}
	} else {
		madr = calc_ea_dst(op);
		FPU_FLD_F64_EA(madr);
		EA_TREE(op);
	}
}

// dd
void
SF_ESC5(void)
{
	UINT32 op, madr;
	UINT idx, sub;

	CPU_WORKCLOCK(FPU_WORKCLOCK);
	GET_PCBYTE(op);
	TRACEOUT(("use FPU dd %.2x", op));
	idx = (op >> 3) & 7;
	sub = (op & 7);
	fpu_check_NM_EXCEPTION();
	if(op >= 0xc0 || (idx!=4 && idx!=6 && idx!=7)){
		fpu_checkexception();
	}
	if (op >= 0xc0) {
		/* FUCOM ST(i), ST(0) */
		/* Fxxx ST(i) */
		switch (idx) {
		case 0:	/* FFREE */
			TRACEOUT(("FFREE"));
			FPU_STAT.tag[FPU_ST(sub)]=TAG_Empty;
			FPU_STAT.mmxenable = 0;
			break;
		case 1: /* FXCH */
			TRACEOUT(("FXCH"));
			FPU_FXCH(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 2:	/* FST */
			TRACEOUT(("FST"));
			FPU_FST(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 3:	/* FSTP */
			TRACEOUT(("FSTP"));
			FPU_FST(FPU_STAT_TOP,FPU_ST(sub));
			FPU_pop();
			break;
		case 4:	/* FUCOM */
			TRACEOUT(("FUCOM"));
			FPU_FUCOM(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 5:	/* FUCOMP */
			TRACEOUT(("FUCOMP"));
			FPU_FUCOM(FPU_STAT_TOP,FPU_ST(sub));
			FPU_pop();
			break;

		default:
			break;
		}
	} else {
		madr = calc_ea_dst(op);
		switch (idx) {
		case 0:	/* FLD (倍精度実数) */
			TRACEOUT(("FLD double real"));
			FPU_prepush();
			FPU_FLD_F64(madr,FPU_STAT_TOP);
			break;
		case 1:	/* FISTTP (QWORD) */
			{
				unsigned char oldrnd = softfloat_roundingMode;
				softfloat_roundingMode = softfloat_round_min;
				FPU_FST_I64(madr);
				softfloat_roundingMode = oldrnd;
			}
			FPU_pop();
			break;
		case 2:	/* FST (倍精度実数) */
			TRACEOUT(("FST double real"));
			FPU_FST_F64(madr);
			break;
		case 3:	/* FSTP (倍精度実数) */
			TRACEOUT(("FSTP double real"));
			FPU_FST_F64(madr);
			FPU_pop();
			break;
		case 4:	/* FRSTOR */
			TRACEOUT(("FRSTOR"));
			FPU_FRSTOR(madr);
			break;
		case 6:	/* FSAVE */
			TRACEOUT(("FSAVE"));
			FPU_FSAVE(madr);
			break;

		case 7:	/* FSTSW */
			FP_TOP_SET(FPU_STAT_TOP);
			cpu_vmemorywrite_w(CPU_INST_SEGREG_INDEX, madr, FPU_STATUSWORD);
			break;

		default:
			break;
		}
	}
}

// de
void
SF_ESC6(void)
{
	UINT32 op, madr;
	UINT idx, sub;

	CPU_WORKCLOCK(FPU_WORKCLOCK);
	GET_PCBYTE(op);
	TRACEOUT(("use FPU de %.2x", op));
	idx = (op >> 3) & 7;
	sub = (op & 7);
	
	fpu_check_NM_EXCEPTION();
	fpu_checkexception();
	if (op >= 0xc0) {
		/* Fxxx ST(i), ST(0) */
		switch (idx) {
		case 0:	/* FADDP */
			TRACEOUT(("FADDP"));
			FPU_FADD(FPU_ST(sub),FPU_STAT_TOP);
			break;
		case 1:	/* FMULP */
			TRACEOUT(("FMULP"));
			FPU_FMUL(FPU_ST(sub),FPU_STAT_TOP);
			break;
		case 2:	/* FCOMP5 */
			TRACEOUT(("FCOMP5"));
			FPU_FCOM(FPU_STAT_TOP,FPU_ST(sub));
			break;
		case 3: /* FCOMPP */
			TRACEOUT(("FCOMPP"));
			if(sub != 1) {
				return;
			}
			FPU_FCOM(FPU_STAT_TOP,FPU_ST(1));
			FPU_pop(); /* extra pop at the bottom*/
			break;			
		case 4:	/* FSUBRP */
			TRACEOUT(("FSUBRP"));
			FPU_FSUBR(FPU_ST(sub),FPU_STAT_TOP);
			break;
		case 5:	/* FSUBP */
			TRACEOUT(("FSUBP"));
			FPU_FSUB(FPU_ST(sub),FPU_STAT_TOP);
			break;
		case 6:	/* FDIVRP */
			TRACEOUT(("FDIVRP"));
			FPU_FDIVR(FPU_ST(sub),FPU_STAT_TOP);
			if((FPU_STATUSWORD & ~FPU_CTRLWORD) & FP_ZE_FLAG){
				return; // POPしないようにする
			}
			break;
		case 7:	/* FDIVP */
			TRACEOUT(("FDIVP"));
			FPU_FDIV(FPU_ST(sub),FPU_STAT_TOP);
			if((FPU_STATUSWORD & ~FPU_CTRLWORD) & FP_ZE_FLAG){
				return; // POPしないようにする
			}
			break;
			/*FALLTHROUGH*/
		default:
			break;
		}
		FPU_pop();
	} else {
		madr = calc_ea_dst(op);
		FPU_FLD_I16_EA(madr);
		EA_TREE(op);
	}
}

// df
void
SF_ESC7(void)
{
	UINT32 op, madr;
	UINT idx, sub;

	CPU_WORKCLOCK(FPU_WORKCLOCK);
	GET_PCBYTE(op);
	TRACEOUT(("use FPU df %.2x", op));
	idx = (op >> 3) & 7;
	sub = (op & 7);
	
	fpu_check_NM_EXCEPTION();
	if(!(op >= 0xc0 && idx==4 && sub==0)){
		fpu_checkexception();
	}
	if (op >= 0xc0) {
		/* Fxxx ST(0), ST(i) */
		switch (idx) {
		case 0: /* FFREEP */
			TRACEOUT(("FFREEP"));
			FPU_STAT.tag[FPU_ST(sub)]=TAG_Empty;
			FPU_STAT.mmxenable = 0;
			FPU_pop();
			break;
		case 1: /* FXCH */
			TRACEOUT(("FXCH"));
			FPU_FXCH(FPU_STAT_TOP,FPU_ST(sub));
			break;
		
		case 2:
		case 3: /* FSTP */
			TRACEOUT(("FSTP"));
			FPU_FST(FPU_STAT_TOP,FPU_ST(sub));
			FPU_pop();
			break;

		case 4:
			switch (sub)
			{
			case 0: /* FSTSW AX */
				TRACEOUT(("FSTSW AX"));
				FP_TOP_SET(FPU_STAT_TOP);
				CPU_AX = FPU_STATUSWORD;
				break;
				
			default:
				break;
			}
			break;
		case 5: /* FUCOMIP */
			TRACEOUT(("ESC7: FUCOMIP"));
			FPU_FUCOMI(FPU_STAT_TOP,FPU_ST(sub));	
			FPU_pop();
			break;
		case 6: /* FCOMIP */
			TRACEOUT(("ESC7: FCOMIP"));
			FPU_FCOMI(FPU_STAT_TOP,FPU_ST(sub));	
			FPU_pop();
			break;
		default:
			break;
		}
	} else {
		madr = calc_ea_dst(op);
		switch (idx) {
		case 0:	/* FILD (WORD) */
			TRACEOUT(("FILD SINT16"));
			FPU_prepush();
			FPU_FLD_I16(madr,FPU_STAT_TOP);
			break;
		case 1:	/* FISTTP (WORD) */
			{
				unsigned char oldrnd = softfloat_roundingMode;
				softfloat_roundingMode = softfloat_round_min;
				FPU_FST_I16(madr);
				softfloat_roundingMode = oldrnd;
			}
			FPU_pop();
			break;
		case 2:	/* FIST (WORD) */
			TRACEOUT(("FIST SINT16"));
			FPU_FST_I16(madr);
			break;
		case 3:	/* FISTP (WORD) */
			TRACEOUT(("FISTP SINT16"));
			FPU_FST_I16(madr);
			FPU_pop();
			break;

		case 4:	/* FBLD (BCD) */
			TRACEOUT(("FBLD packed BCD"));
			FPU_prepush();
			FPU_FBLD(madr,FPU_STAT_TOP);
			break;

		case 5:	/* FILD (QWORD) */
			TRACEOUT(("FILD SINT64"));
			FPU_prepush();
			FPU_FLD_I64(madr,FPU_STAT_TOP);
			break;

		case 6:	/* FBSTP (BCD) */
			TRACEOUT(("FBSTP packed BCD"));
			FPU_FBST(madr);
			FPU_pop();
			break;

		case 7:	/* FISTP (QWORD) */
			TRACEOUT(("FISTP SINT64"));
			FPU_FST_I64(madr);
			FPU_pop();
			break;

		default:
			break;
		}
	}
}
#endif
