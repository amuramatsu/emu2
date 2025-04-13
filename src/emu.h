#ifndef EMU_H
#define EMU_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

extern volatile int exit_cpu;
extern uint32_t memory_mask;
extern uint32_t memory_limit;
uint8_t *memory;

int cpuGetAddress(uint16_t segment, uint16_t offset);
int cpuGetAddrDS(uint16_t offset);
int cpuGetAddrES(uint16_t offset);
int cpuGetAddrSS(uint16_t offset);

// Reads a word from the stack at displacement "disp"
uint16_t cpuGetStack(uint16_t disp);

uint8_t read_port(unsigned port);
void write_port(unsigned port, uint8_t value);
int bios_routine(unsigned inum);

// CPU interface
void execute(void); // 1 ins.
void init_cpu(void);
void cpu_reset(void);

// Helper functions
uint32_t get_static_memory(uint16_t bytes, uint16_t align);
int reg_farcall_entry(uint32_t ret_addr, void (*func)(void));

// async HW update
void emulator_update(void);

// Trigger hardware interrupts.
// IRQ-0 to IRQ-7 call INT-08 to INT-0F
// IRQ-8 to IRQ-F call INT-70 to INT-77
void cpuTriggerIRQ(int num);

// Register reading/writing
void cpuSetAL(unsigned v);
void cpuSetAX(unsigned v);
void cpuSetCX(unsigned v);
void cpuSetDX(unsigned v);
void cpuSetBX(unsigned v);
void cpuSetSP(unsigned v);
void cpuSetBP(unsigned v);
void cpuSetSI(unsigned v);
void cpuSetDI(unsigned v);
void cpuSetES(unsigned v);
void cpuSetCS(unsigned v);
void cpuSetSS(unsigned v);
void cpuSetDS(unsigned v);
void cpuSetIP(unsigned v);

unsigned cpuGetAX(void);
unsigned cpuGetCX(void);
unsigned cpuGetDX(void);
unsigned cpuGetBX(void);
unsigned cpuGetSP(void);
unsigned cpuGetBP(void);
unsigned cpuGetSI(void);
unsigned cpuGetDI(void);
unsigned cpuGetES(void);
unsigned cpuGetCS(void);
unsigned cpuGetSS(void);
unsigned cpuGetDS(void);
unsigned cpuGetIP(void);

#ifdef IA32
void cpuSetFS(unsigned v);
void cpuSetGS(unsigned v);
void cpuSetEAX(uint32_t v);
void cpuSetECX(uint32_t v);
void cpuSetEDX(uint32_t v);
void cpuSetEBX(uint32_t v);
void cpuSetESP(uint32_t v);
void cpuSetEBP(uint32_t v);
void cpuSetESI(uint32_t v);
void cpuSetEDI(uint32_t v);
void cpuSetEIP(uint32_t v);

unsigned cpuGetFS(void);
unsigned cpuGetGS(void);
uint32_t cpuGetEAX(void);
uint32_t cpuGetECX(void);
uint32_t cpuGetEDX(void);
uint32_t cpuGetEBX(void);
uint32_t cpuGetESP(void);
uint32_t cpuGetEBP(void);
uint32_t cpuGetESI(void);
uint32_t cpuGetEDI(void);
uint32_t cpuGetEIP(void);
#endif

// Alter flags in the stack, use from interrupt handling
enum cpuFlags
{
    cpuFlag_CF = 1,
    cpuFlag_PF = 4,
    cpuFlag_AF = 16,
    cpuFlag_ZF = 64,
    cpuFlag_SF = 128,
    cpuFlag_TF = 256,
    cpuFlag_IF = 512,
    cpuFlag_DF = 1024,
    cpuFlag_OF = 2048
};

void cpuSetFlag(enum cpuFlags flag);
void cpuClrFlag(enum cpuFlags flag);

// Alter direct CPU flags, only use on startup
void cpuSetStartupFlag(enum cpuFlags flag);
void cpuClrStartupFlag(enum cpuFlags flag);

#ifdef EMS_SUPPORT
#include "ems.h"
#endif

// Helper functions to access memory
#ifdef IA32

// Read 8 bit number
void meml_write8(uint32_t address, uint8_t value);
#define put8(addr, v) meml_write8(addr, v)

// Read 16 bit number
void meml_write16(uint32_t address, uint16_t value);
#define put16(addr, v) meml_write16(addr, v)

// Read 32 bit number
void meml_write32(uint32_t address, uint32_t value);
#define put32(addr, v) meml_write32(addr, v)

// Read 32 bit number
void meml_writes(uint32_t address, const void *dat, unsigned int leng);

// Write 8 bit number
uint8_t meml_read8(uint32_t address);
#define get8(addr) meml_read8(addr)

// Write 16 bit number
uint16_t meml_read16(uint32_t address);
#define get16(addr) meml_read16(addr)

// Write 32 bit number
uint32_t meml_read32(uint32_t address);
#define get32(addr) meml_read32(addr)

// Write mem block
void meml_reads(uint32_t address, void *dat, unsigned int leng);

#else // not IA32

// Read 8 bit number
static inline void put8(int addr, int v)
{
#ifdef EMS_SUPPORT
    if(in_ems_pageframe(addr))
    {
        ems_put8(addr, v);
        return;
    }
#endif
    memory[memory_mask & (addr)] = v;
}

// Read 16 bit number
static inline void put16(int addr, int v)
{
    put8(addr, v);
    put8(addr + 1, v >> 8);
}

// Read 32 bit number
static inline void put32(int addr, unsigned v)
{
    put16(addr, v & 0xFFFF);
    put16(addr + 2, v >> 16);
}

// Write 8 bit number
static inline int get8(int addr)
{
#ifdef EMS_SUPPORT
    if(in_ems_pageframe(addr))
    {
        return ems_get8(addr);
    }
#endif
    return memory[memory_mask & addr];
}

// Write 16 bit number
static inline unsigned get16(int addr)
{
    return get8(addr) + (get8(addr + 1) << 8);
}

// Write 32 bit number
static inline unsigned get32(int addr)
{
    return get16(addr) + (get16(addr + 2) << 16);
}

#endif // IA32

#define check_limit(s, d)                                                                \
    ((s) >= memory_limit || (d) >= memory_limit || (s) + (d) >= memory_limit)

// Push word to stack
static inline void cpuPushWord(uint16_t w)
{
    cpuSetSP(cpuGetSP() - 2);
    put16(cpuGetSS() * 16 + cpuGetSP(), w);
}

// Pop word from stack
static inline int cpuPopWord(void)
{
    int w = get16(cpuGetSS() * 16 + cpuGetSP());
    cpuSetSP(cpuGetSP() + 2);
    return w;
}

// Copy data to CPU memory
static inline int putmem(uint32_t dest, const uint8_t *src, unsigned size)
{
    if(check_limit(size, dest))
        return 1;
#ifdef EMS_SUPPORT
    if(in_ems_pageframe(dest))
    {
        unsigned i;
        for(i = 0; i < size; i++)
            ems_put8(dest++, *src++);
        return 0;
    }
#endif
#ifdef IA32
    meml_writes(dest, src, size);
#else
    memcpy(memory + dest, src, size);
#endif
    return 0;
}

#ifndef IA32 // getptr is not supported at IA32
// Get pointer to CPU memory or null if overflow
static inline uint8_t *getptr(uint32_t addr, unsigned size)
{
    if(check_limit(size, addr))
        return 0;
#ifdef EMS_SUPPORT
    if(in_ems_pageframe(addr))
        return 0;
#endif
    return memory + addr;
}
#endif

// Get a copy of CPU memory forcing a nul byte at end.
// Four static buffers are used, so at most 4 results can be in use.
static inline char *getstr(uint32_t addr, unsigned size)
{
    static int cbuf = 0;
    static char buf[4][256];

    cbuf = (cbuf + 1) & 3;
    memset(buf[cbuf], 0, 256);
#ifdef EMS_SUPPORT
    if(size < 255 && in_ems_pageframe(addr))
    {
        int i;
        char *p = buf[cbuf];
        for(i = 0; i < size; i++)
        {
            *p++ = ems_get8(addr++);
        }
    }
    else
#endif
        if(size < 255 && !check_limit(addr, size))
    {
#ifdef IA32
        meml_reads(addr, buf[cbuf], size);
#else
        memcpy(buf[cbuf], memory + addr, size);
#endif
    }
    return buf[cbuf];
}

#endif // EMU_H
