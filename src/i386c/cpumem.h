#pragma once

#ifdef NP2_MEMORY_ASM
#undef	MEMCALL
#define	MEMCALL	FASTCALL
#endif

#if !defined(MEMCALL)
#define	MEMCALL
#endif
#define USE_HIMEM 0x110000

#ifdef __cplusplus
extern "C" {
#endif
	
#if defined(SUPPORT_IA32_HAXM)
extern	UINT8	membase[0x200000];
extern	UINT8	*mem;
#else
extern	UINT8	mem[0x200000];
#endif

void MEMCALL memm_arch(UINT type);
void MEMCALL memm_vram(UINT operate);

REG8 MEMCALL memp_read8(UINT32 address);
REG16 MEMCALL memp_read16(UINT32 address);
UINT32 MEMCALL memp_read32(UINT32 address);
void MEMCALL memp_write8(UINT32 address, REG8 value);
void MEMCALL memp_write16(UINT32 address, REG16 value);
void MEMCALL memp_write32(UINT32 address, UINT32 value);
void MEMCALL memp_reads(UINT32 address, void *dat, UINT leng);
void MEMCALL memp_writes(UINT32 address, const void *dat, UINT leng);
REG8 MEMCALL memp_read8_codefetch(UINT32 address);
REG16 MEMCALL memp_read16_codefetch(UINT32 address);
UINT32 MEMCALL memp_read32_codefetch(UINT32 address);
REG8 MEMCALL memp_read8_paging(UINT32 address);
REG16 MEMCALL memp_read16_paging(UINT32 address);
UINT32 MEMCALL memp_read32_paging(UINT32 address);
void MEMCALL memp_write8_paging(UINT32 address, REG8 value);
void MEMCALL memp_write16_paging(UINT32 address, REG16 value);
void MEMCALL memp_write32_paging(UINT32 address, UINT32 value);

REG8 MEMCALL meml_read8(UINT32 address);
REG16 MEMCALL meml_read16(UINT32 address);
UINT32 MEMCALL meml_read32(UINT32 address);
void MEMCALL meml_write8(UINT32 address, REG8 dat);
void MEMCALL meml_write16(UINT32 address, REG16 dat);
void MEMCALL meml_write32(UINT32 address, UINT32 dat);
void MEMCALL meml_reads(UINT32 address, void *dat, UINT leng);
void MEMCALL meml_writes(UINT32 address, const void *dat, UINT leng);

REG8 MEMCALL memr_read8(UINT seg, UINT off);
REG16 MEMCALL memr_read16(UINT seg, UINT off);
UINT32 MEMCALL memr_read32(UINT seg, UINT off);
void MEMCALL memr_write8(UINT seg, UINT off, REG8 dat);
void MEMCALL memr_write16(UINT seg, UINT off, REG16 dat);
void MEMCALL memr_write32(UINT seg, UINT off, UINT32 dat);
void MEMCALL memr_reads(UINT seg, UINT off, void *dat, UINT leng);
void MEMCALL memr_writes(UINT seg, UINT off, const void *dat, UINT leng);

#ifdef __cplusplus
}
#endif
