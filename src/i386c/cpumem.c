#include <compiler.h>
#include <cpucore.h>
#include <cpumem.h>
#ifdef EMS_SUPPORT
#include <../ems.h>
#endif

#if 1
#undef	TRACEOUT
//#define USE_TRACEOUT_VS
//#define MEM_BDA_TRACEOUT
//#define MEM_D8_TRACEOUT
#ifdef USE_TRACEOUT_VS
static void trace_fmt_ex(const char *fmt, ...)
{
	char stmp[2048];
	va_list ap;
	va_start(ap, fmt);
	vsprintf(stmp, fmt, ap);
	strcat(stmp, "\n");
	va_end(ap);
	OutputDebugStringA(stmp);
}
#define	TRACEOUT(s)	trace_fmt_ex s
#else
#define	TRACEOUT(s)	(void)(s)
#endif
#endif	/* 1 */

extern uint8_t *memory;
extern uint32_t memory_mask;
#define mem	memory

/*
 * -- Fast-path MMIO guard table --
 *
 * The fast CPU memory path may directly access mem.
 * Devices whose MMIO/VRAM windows overlap that address space must mark the corresponding blocks here.
 *
 * Granularity:
 *	 00000000-000fffff : 4KB blocks
 *	 00100000-00ffffff : 64KB blocks
 *	 01000000-ffffffff : 1MB blocks
 * 
 * -- 高速処理用MMIOアクセステーブル --
 * 
 * MMIOやバンクメモリなど少しでも特殊アクセスされる可能性があるメモリ領域を登録する。
 * 普通のメモリ領域が登録されていても低速版へフォールバックするのでOK。
 * 素直なアクセスができるメモリ領域に高速アクセスできる方が全体として速くなる。
 * 
 * メモリ範囲粒度:
 *	 00000000-000fffff : 4KB blocks（現状I286_MEMWRITEMAX以上は常時低速アクセス扱いなので使用しない）
 *	 00100000-00ffffff : 64KB blocks
 *	 01000000-ffffffff : 1MB blocks
 */
#define MEMP_FASTMMIO_LOW_LIMIT		 0x00100000UL
#define MEMP_FASTMMIO_MID_LIMIT		 0x01000000UL
#define MEMP_FASTMMIO_LOW_SHIFT		 12
#define MEMP_FASTMMIO_MID_SHIFT		 16
#define MEMP_FASTMMIO_HIGH_SHIFT	 20
#define MEMP_FASTMMIO_LOW_COUNT		 (MEMP_FASTMMIO_LOW_LIMIT >> MEMP_FASTMMIO_LOW_SHIFT)
#define MEMP_FASTMMIO_MID_COUNT		 ((MEMP_FASTMMIO_MID_LIMIT - MEMP_FASTMMIO_LOW_LIMIT) >> MEMP_FASTMMIO_MID_SHIFT)
#define MEMP_FASTMMIO_HIGH_COUNT	 0x1000

#if defined(_MSC_VER)
#define MEMP_FASTMMIO_INLINE static __forceinline
#elif defined(__GNUC__)
#define MEMP_FASTMMIO_INLINE static __inline__ __attribute__((always_inline))
#else
#define MEMP_FASTMMIO_INLINE static
#endif

#ifdef _MSC_VER
#define MEMP_ALIGN_CACHE __declspec(align(64))
#else
#define MEMP_ALIGN_CACHE __attribute__((aligned(64)))
#endif

// 指定アドレスが直接アクセス可能かどうか調べる　OK=0、不可=0以外
MEMP_FASTMMIO_INLINE int
memp_fastmmio_addr_is_marked(UINT32 address)
{
#ifdef EMS_SUPPORT
	return in_ems_pageframe(address);
#else
	return 0;
#endif
}

// 指定アドレス範囲が直接アクセス可能かどうか調べる　OK=0、不可=0以外
MEMP_FASTMMIO_INLINE int
memp_fastmmio_range_is_marked(UINT32 address, int size)
{
	// XXX: 開始アドレスしか見ていないので途中からMMIOアドレスに入るとおかしくなるがそんな変なアクセスはしないと信じる → 残念ながらありました
	//return memp_fastmmio_addr_is_marked(address);

	// 厳密に判定したければこっち
	return memp_fastmmio_addr_is_marked(address) || memp_fastmmio_addr_is_marked(address + size - 1);
}

// MMIOアクセスマップに直接アクセス不可領域を登録　カウンタ管理なので重複登録されてもよい
void MEMCALL
memp_mmio_range_add(UINT32 address, UINT32 leng)
{
	/*NOP*/
}
// MMIOアクセスマップから直接アクセス不可領域を削除
void MEMCALL
memp_mmio_range_remove(UINT32 address, UINT32 leng)
{
	/*NOP*/
}

// MMIOアクセスマップ初期化　ついでに怪しい領域は先に登録しておく
void MEMCALL
memp_mmio_map_reset(void)
{
	/*NOP*/
}

// ページング時にメモリ直接アクセス可能かどうか調べて可能ならポインタを返す。不可ならNULLを返す。
// 複雑な判定は止めて少しでも怪しければNULLを返す
UINT8 * MEMCALL
memp_get_direct_host_page(UINT32 address)
{
	address &= memory_mask;
	// 高速MMIOアクセステーブルで判定
	if (memp_fastmmio_addr_is_marked(address) ||
		memp_fastmmio_addr_is_marked(address + (CPU_PAGE_SIZE - 1))) {
		return NULL;
	}
	return memory + address;
}

// ---- MAIN

static REG16 MEMCALL
memp_read8_slow(UINT32 address)
{
#ifdef EMS_SUPPORT
	if (in_ems_pageframe(address))
		return ems_get8(address);
#endif
	return memory[address & memory_mask] & 0xff;
}

static REG16 MEMCALL
memp_read16_slow(UINT32 address)
{
	return (memp_read8(address+1) << 8) | memp_read8(address);
}

static UINT32 MEMCALL
memp_read32_slow(UINT32 address)
{
	return ((UINT32)memp_read16(address+2) << 16) | memp_read16(address);
}

static PF_UINT8 MEMCALL
memp_read8_codefetch_slow(UINT32 address)
{
	return memp_read8_slow(address);
}

static PF_UINT16 MEMCALL
memp_read16_codefetch_slow(UINT32 address)
{
	return memp_read16_slow(address);
}

static PF_UINT32 MEMCALL
memp_read32_codefetch_slow(UINT32 address)
{
	return memp_read32_slow(address);
}

static void MEMCALL
memp_write8_slow(UINT32 address, REG8 value)
{
#ifdef EMS_SUPPORT
	if (in_ems_pageframe(address))
	{
		ems_put8(address, value);
		return;
	}
#endif
	memory[address & memory_mask] = value;
}

static void MEMCALL
memp_write16_slow(UINT32 address, REG16 value)
{
	memp_write8(address, value & 0xff);
	memp_write8(address+1, value >> 8);
}

static void MEMCALL
memp_write32_slow(UINT32 address, UINT32 value)
{
	memp_write16(address, value & 0xffff);
	memp_write16(address+2, value >> 16);
}

static void MEMCALL
memp_write8_paging_slow(UINT32 address, REG8 value)
{
	memp_write8_slow(address, value & 0xff);
}

static void MEMCALL
memp_write16_paging_slow(UINT32 address, REG16 value)
{
	memp_write16_slow(address, value & 0xffff);
}

static void MEMCALL
memp_write32_paging_slow(UINT32 address, UINT32 value)
{
	memp_write32_slow(address, value);
}

// ---- 通常メモリ読み込み関数
REG8 MEMCALL
memp_read8(UINT32 address)
{
	return memp_read8_slow(address);
}

REG16 MEMCALL
memp_read16(UINT32 address)
{
	return memp_read16_slow(address);
}

UINT32 MEMCALL
memp_read32(UINT32 address)
{
	return memp_read32_slow(address);
}

// ----- 高速版読み込み　普通のメモリを優先的に処理する
REG8 MEMCALL
memp_read8_fast(UINT32 address)
{
	UINT32 raw = address;
	address = address & memory_mask;
	if (memp_fastmmio_addr_is_marked(address))
		return memp_read8_slow(raw);
	return memory[address];
}

REG16 MEMCALL
memp_read16_fast(UINT32 address)
{
	UINT32 raw = address;
	address = address & memory_mask;
	if (!((address + 1) & 0x7fff) || memp_fastmmio_range_is_marked(address, 2))
		return memp_read16_slow(raw);
	return LOADINTELWORD(memory + address);
}

UINT32 MEMCALL
memp_read32_fast(UINT32 address)
{
	UINT32 raw = address;
	address = address & memory_mask;
	if (!((address & 0x7fff) <= 0x8000 - 4) || memp_fastmmio_range_is_marked(address, 4))
		return memp_read32_slow(raw);
	return LOADINTELDWORD(memory + address);
}

// ---- 通常メモリ読み込み関数（codefetch用）
PF_UINT8 MEMCALL
memp_read8_codefetch(UINT32 address)
{
	return memp_read8(address);
}

PF_UINT16 MEMCALL
memp_read16_codefetch(UINT32 address)
{
	return memp_read16(address);
}

UINT32 MEMCALL
memp_read32_codefetch(UINT32 address)
{
	return memp_read32(address);
}

// ---- 高速版読み込み（codefetch用）　普通のメモリを優先的に処理する
PF_UINT8 MEMCALL
memp_read8_codefetch_fast(UINT32 address)
{
	UINT32 raw = address;
	address = address & memory_mask;
	if (memp_fastmmio_addr_is_marked(address))
		return memp_read8_codefetch_slow(raw);
	return memory[address];
}

PF_UINT16 MEMCALL
memp_read16_codefetch_fast(UINT32 address)
{
	UINT32 raw = address;
	address = address & memory_mask;
	if (!((address + 1) & 0x7fff) || memp_fastmmio_range_is_marked(address, 2))
		return memp_read16_codefetch_slow(raw);
	return LOADINTELWORD(memory + address);
}

PF_UINT32 MEMCALL
memp_read32_codefetch_fast(UINT32 address)
{
	UINT32 raw = address;
	address = address & memory_mask;
	if (!((address & 0x7fff) <= 0x8000 - 4) || memp_fastmmio_range_is_marked(address, 4))
		return memp_read32_codefetch_slow(raw);
	return LOADINTELDWORD(memory + address);
}

// ---- 通常メモリ読み込み関数（paging用）
PF_UINT8 MEMCALL
memp_read8_paging(UINT32 address)
{
	return memp_read8_codefetch(address);
}

PF_UINT16 MEMCALL
memp_read16_paging(UINT32 address)
{
	return memp_read16_codefetch(address);
}

PF_UINT32 MEMCALL
memp_read32_paging(UINT32 address)
{
	return memp_read32_codefetch(address);
}

PF_UINT8 MEMCALL
memp_read8_paging_fast(UINT32 address)
{
	return memp_read8_codefetch_fast(address);
}

PF_UINT16 MEMCALL
memp_read16_paging_fast(UINT32 address)
{
	return memp_read16_codefetch_fast(address);
}

PF_UINT32 MEMCALL
memp_read32_paging_fast(UINT32 address)
{
	return memp_read32_codefetch_fast(address);
}

// ---- 通常メモリ書き込み関数
void MEMCALL
memp_write8(UINT32 address, REG8 value)
{
	memp_write8_slow(address, value);
}

void MEMCALL
memp_write16(UINT32 address, REG16 value)
{
	memp_write16_slow(address, value);
}

void MEMCALL
memp_write32(UINT32 address, UINT32 value)
{
	memp_write32_slow(address, value);
}

// ---- 高速版書き込み　普通のメモリを優先的に処理する
void MEMCALL
memp_write8_fast(UINT32 address, REG8 value)
{
	UINT32 raw = address;
	address = address & memory_mask;
	if (memp_fastmmio_addr_is_marked(address)) {
		memp_write8_slow(raw, value);
		return;
	}
	memory[address] = (UINT8)value;
}

void MEMCALL
memp_write16_fast(UINT32 address, REG16 value)
{
	UINT32 raw = address;
	address = address & memory_mask;
	if (!((address + 1) & 0x7fff) || memp_fastmmio_range_is_marked(address, 2)) {
		memp_write16_slow(raw, value);
		return;
	}
	STOREINTELWORD(memory + address, value);
}

void MEMCALL
memp_write32_fast(UINT32 address, UINT32 value)
{
	UINT32 raw = address;
	address = address & memory_mask;
	if (!((address & 0x7fff) <= 0x8000 - 4) || memp_fastmmio_range_is_marked(address, 4)) {
		memp_write32_slow(raw, value);
		return;
	}
	STOREINTELDWORD(memory + address, value);
}

// ---- 通常メモリ書き込み関数（paging用）
void MEMCALL
memp_write8_paging(UINT32 address, REG8 value)
{
	memp_write8_slow(address, value);
}

void MEMCALL
memp_write16_paging(UINT32 address, REG16 value)
{
	memp_write16_slow(address, value);
}

void MEMCALL
memp_write32_paging(UINT32 address, UINT32 value)
{
	memp_write32_slow(address, value);
}

// ---- 高速版書き込み（paging用）　普通のメモリを優先的に処理する
void MEMCALL
memp_write8_paging_fast(UINT32 address, REG8 value)
{
	memp_write8_fast(address, value);
}

void MEMCALL
memp_write16_paging_fast(UINT32 address, REG16 value)
{
	memp_write16_fast(address, value);
}

void MEMCALL
memp_write32_paging_fast(UINT32 address, UINT32 value)
{
	memp_write32_fast(address, value);
}

void MEMCALL
memp_reads(UINT32 address, void *dat, UINT leng)
{
	UINT8 *out = (UINT8 *)dat;
#ifdef EMS_SUPPORT
	if (in_ems_pageframe2(address, leng)) {
		/* slow memory access */
		while (leng-- > 0) {
			*out++ = memp_read8(address++);
		}
		return;
	}
#endif
	memcpy(out, memory + address, leng);
}

void MEMCALL
memp_writes(UINT32 address, const void *dat, UINT leng)
{
	const UINT8 *inp = (UINT8 *)dat;
#ifdef EMS_SUPPORT
	if (in_ems_pageframe2(address, leng)) {
		/* slow memory access */
		while (leng-- > 0) {
			memp_write8(address++, *inp++);
		}
		return;
	}
#endif
	memcpy(memory + address, inp, leng);
}


// ---- Logical Space (BIOS)

static UINT32 MEMCALL
physicaladdr(UINT32 addr, BOOL wr)
{
	UINT32	a;
	UINT32	pde;
	UINT32	pte;

	a = CPU_STAT_PDE_BASE + ((addr >> 20) & 0xffc);
	pde = memp_read32(a);
	if (!(pde & CPU_PDE_PRESENT)) {
		goto retdummy;
	}
	if (!(pde & CPU_PDE_ACCESS)) {
		memp_write8(a, (UINT8)(pde | CPU_PDE_ACCESS));
	}
	a = (pde & CPU_PDE_BASEADDR_MASK) + ((addr >> 10) & 0xffc);
	pte = cpu_memoryread_d(a);
	if (!(pte & CPU_PTE_PRESENT)) {
		goto retdummy;
	}
	if (!(pte & CPU_PTE_ACCESS)) {
		memp_write8(a, (UINT8)(pte | CPU_PTE_ACCESS));
	}
	if ((wr) && (!(pte & CPU_PTE_DIRTY))) {
		memp_write8(a, (UINT8)(pte | CPU_PTE_DIRTY));
	}
	addr = (pte & CPU_PTE_BASEADDR_MASK) + (addr & 0x00000fff);
	return(addr);

 retdummy:
	return(0x01000000);	/* XXX */
}

REG8 MEMCALL
meml_read8(UINT32 addr)
{
    if (CPU_STAT_PAGING) {
        addr = physicaladdr(addr, FALSE);
    }
    return(memp_read8(addr));
}

REG16 MEMCALL
meml_read16(UINT32 addr)
{
    if (!CPU_STAT_PAGING) {
        return(memp_read16(addr));
    }
    else if ((addr + 1) & 0xfff) {
        return(memp_read16(physicaladdr(addr, FALSE)));
    }
    return(meml_read8(addr) + (meml_read8(addr + 1) << 8));
}

UINT32 MEMCALL
meml_read32(UINT32 addr)
{
    if (!CPU_STAT_PAGING) {
        return(memp_read32(addr));
    }
    return(meml_read16(addr) + (meml_read16(addr + 2) << 16));
}


void MEMCALL
meml_write8(UINT32 addr, REG8 dat)
{
    if (CPU_STAT_PAGING) {
        addr = physicaladdr(addr, TRUE);
    }
    memp_write8(addr, dat);
}

void MEMCALL
meml_write16(UINT32 addr, REG16 dat)
{
    if (!CPU_STAT_PAGING) {
        memp_write16(addr, dat);
    }
    else if ((addr + 1) & 0xfff) {
        memp_write16(physicaladdr(addr, TRUE), dat);
    }
    else {
        meml_write8(addr, (REG8)dat);
        meml_write8(addr + 1, (REG8)(dat >> 8));
    }
}

void MEMCALL
meml_write32(UINT32 addr, UINT32 dat)
{
    if (!CPU_STAT_PAGING) {
        memp_write32(addr, dat);
    }
    else {
        meml_write16(addr, (REG16)dat);
        meml_write16(addr + 2, (REG16)(dat >> 16));
    }
}

void MEMCALL
meml_reads(UINT32 address, void *dat, UINT leng)
{
	UINT	size;
	if (!CPU_STAT_PAGING) {
		memp_reads(address, dat, leng);
	}
	else {
		while(leng) {
			size = 0x1000 - (address & 0xfff);
			size = MIN(size, leng);
			memp_reads(physicaladdr(address, FALSE), dat, size);
			address += size;
			dat = ((UINT8 *)dat) + size;
			leng -= size;
		}
	}
}

void MEMCALL
meml_writes(UINT32 address, const void *dat, UINT leng)
{
	UINT	size;

	if (!CPU_STAT_PAGING) {
		memp_writes(address, dat, leng);
	}
	else {
		while(leng) {
			size = 0x1000 - (address & 0xfff);
			size = MIN(size, leng);
			memp_writes(physicaladdr(address, TRUE), dat, size);
			address += size;
			dat = ((UINT8 *)dat) + size;
			leng -= size;
		}
	}
}

REG8 MEMCALL
memr_read8(UINT seg, UINT off)
{
	UINT32	addr;

	addr = (seg << 4) + LOW16(off);
	if (CPU_STAT_PAGING) {
		addr = physicaladdr(addr, FALSE);
	}
	return(memp_read8(addr));
}

REG16 MEMCALL
memr_read16(UINT seg, UINT off)
{
	UINT32	addr;

	addr = (seg << 4) + LOW16(off);
	if (!CPU_STAT_PAGING) {
		return(memp_read16(addr));
	}
	else if ((addr + 1) & 0xfff) {
		return(memp_read16(physicaladdr(addr, FALSE)));
	}
	return(memr_read8(seg, off) + (memr_read8(seg, off + 1) << 8));
}

void MEMCALL
memr_write8(UINT seg, UINT off, REG8 dat)
{
	UINT32	addr;

	addr = (seg << 4) + LOW16(off);
	if (CPU_STAT_PAGING) {
		addr = physicaladdr(addr, TRUE);
	}
	memp_write8(addr, dat);
}

void MEMCALL
memr_write16(UINT seg, UINT off, REG16 dat)
{
	UINT32	addr;

	addr = (seg << 4) + LOW16(off);
	if (!CPU_STAT_PAGING) {
		memp_write16(addr, dat);
	}
	else if ((addr + 1) & 0xfff) {
		memp_write16(physicaladdr(addr, TRUE), dat);
	}
	else {
		memr_write8(seg, off, (REG8)dat);
		memr_write8(seg, off + 1, (REG8)(dat >> 8));
	}
}

void MEMCALL
memr_reads(UINT seg, UINT off, void *dat, UINT leng)
{
	UINT32	addr;
	UINT	rem;
	UINT	size;

	while(leng) {
		off = LOW16(off);
		addr = (seg << 4) + off;
		rem = 0x10000 - off;
		size = MIN(leng, rem);
		if (CPU_STAT_PAGING) {
			rem = 0x1000 - (addr & 0xfff);
			size = MIN(size, rem);
			addr = physicaladdr(addr, FALSE);
		}
		memp_reads(addr, dat, size);
		off += size;
		dat = ((UINT8 *)dat) + size;
		leng -= size;
	}
}

void MEMCALL
memr_writes(UINT seg, UINT off, const void *dat, UINT leng)
{
	UINT32	addr;
	UINT	rem;
	UINT	size;

	while(leng) {
		off = LOW16(off);
		addr = (seg << 4) + off;
		rem = 0x10000 - off;
		size = MIN(leng, rem);
		if (CPU_STAT_PAGING) {
			rem = 0x1000 - (addr & 0xfff);
			size = MIN(size, rem);
			addr = physicaladdr(addr, TRUE);
		}
		memp_writes(addr, dat, size);
		off += size;
		dat = ((UINT8 *)dat) + size;
		leng -= size;
	}
}
