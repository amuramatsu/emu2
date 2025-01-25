#include <compiler.h>
#include <cpucore.h>
#include <cpumem.h>
#ifdef EMS_SUPPORT
#include <../ems.h>
#endif

#if 1
#undef TRACEOUT
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
#define TRACEOUT(s) trace_fmt_ex s
#else
#define TRACEOUT(s) (void)(s)
#endif
#endif /* 1 */

// ----

extern uint8_t *memory;
extern uint32_t memory_mask;
REG8 MEMCALL
memp_read8(UINT32 address)
{
#ifdef EMS_SUPPORT
    if (in_ems_pageframe(address))
        return ems_get8(address);
#endif
    return memory[address & memory_mask] & 0xff;
}

REG16 MEMCALL
memp_read16(UINT32 address)
{
    return (memp_read8(address+1) << 8) | memp_read8(address);
}

UINT32 MEMCALL
memp_read32(UINT32 address)
{
    return ((UINT32)memp_read16(address+2) << 16) | memp_read16(address);
}

void MEMCALL
memp_write8(UINT32 address, REG8 value)
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

void MEMCALL
memp_write16(UINT32 address, REG16 value)
{
    memp_write8(address, value & 0xff);
    memp_write8(address+1, value >> 8);
}

void MEMCALL
memp_write32(UINT32 address, UINT32 value)
{
    memp_write16(address, value & 0xffff);
    memp_write16(address+2, value >> 16);
}

REG8 MEMCALL
memp_read8_codefetch(UINT32 address)
{
    return memp_read8(address);
}

REG16 MEMCALL
memp_read16_codefetch(UINT32 address)
{
    return memp_read16(address);
}

UINT32 MEMCALL
memp_read32_codefetch(UINT32 address)
{
    return memp_read32(address);
}

// ----
REG8 MEMCALL
memp_read8_paging(UINT32 address)
{
    return memp_read8_codefetch(address);
}

REG16 MEMCALL
memp_read16_paging(UINT32 address)
{
    return memp_read16_codefetch(address);
}

UINT32 MEMCALL
memp_read32_paging(UINT32 address)
{
    return memp_read32_codefetch(address);
}

void MEMCALL
memp_write8_paging(UINT32 address, REG8 value)
{
    memp_write8(address, value);
}

void MEMCALL
memp_write16_paging(UINT32 address, REG16 value)
{
    memp_write16(address, value);
}

void MEMCALL
memp_write32_paging(UINT32 address, UINT32 value)
{
    memp_write32(address, value);
}

void MEMCALL
memp_reads(UINT32 address, void *dat, UINT leng)
{
    UINT8 *out = (UINT8 *)dat;
#if 0
    UINT diff;
    /* fast memory access */
    if ((address + leng) < I286_MEMREADMAX) {
        memcpy(dat, mem + address, leng);
        return;
    }
    address = address & CPU_ADRSMASK;
    if ((address >= USE_HIMEM) && (address < CPU_EXTLIMIT16)) {
        diff = CPU_EXTLIMIT16 - address;
        if (diff >= leng) {
            memcpy(dat, CPU_EXTMEMBASE + address, leng);
            return;
        }
        memcpy(dat, CPU_EXTMEMBASE + address, diff);
        out += diff;
        leng -= diff;
        address += diff;
    }
#endif
    /* slow memory access */
    while (leng-- > 0)
        *out++ = memp_read8(address++);
}

void MEMCALL
memp_writes(UINT32 address, const void *dat, UINT leng)
{
    const UINT8 *out = (UINT8 *)dat;
#if 0
    UINT diff;

    /* fast memory access */
    if ((address + leng) < I286_MEMREADMAX) {
        memcpy(mem + address, dat, leng);
        return;
    }
    address = address & CPU_ADRSMASK;
    if ((address >= USE_HIMEM) && (address < CPU_EXTLIMIT16)) {
        diff = CPU_EXTLIMIT16 - address;
        if (diff >= leng) {
            memcpy(CPU_EXTMEMBASE + address, dat, leng);
            return;
        }
        memcpy(CPU_EXTMEMBASE + address, dat, diff);
        out += diff;
        leng -= diff;
        address += diff;
    }
#endif
    
    /* slow memory access */
    while (leng-- > 0)
        memp_write8(address++, *out++);
}

// ---- Logical Space (BIOS)

static UINT32 MEMCALL
physicaladdr(UINT32 addr, BOOL wr)
{
    UINT32 a;
    UINT32 pde;
    UINT32 pte;

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
        return(0x01000000); /* XXX */
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
    UINT size;
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
    UINT size;
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
    UINT32 addr;
    addr = (seg << 4) + LOW16(off);
    if (CPU_STAT_PAGING) {
        addr = physicaladdr(addr, FALSE);
    }
    return(memp_read8(addr));
}

REG16 MEMCALL
memr_read16(UINT seg, UINT off)
{
    UINT32 addr;
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
    UINT32 addr;
    addr = (seg << 4) + LOW16(off);
    if (CPU_STAT_PAGING) {
        addr = physicaladdr(addr, TRUE);
    }
    memp_write8(addr, dat);
}

void MEMCALL
memr_write16(UINT seg, UINT off, REG16 dat)
{
    UINT32 addr;
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
    UINT32 addr;
    UINT rem;
    UINT size;
    
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
    UINT32 addr;
    UINT rem;
    UINT size;
    
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
