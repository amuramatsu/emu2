#ifndef EMU_H
#define EMU_H

int
cpuGetAddress(uint16_t segment, uint32_t offset)
{
}

int
cpuGetAddrDS(uint32_t offset)
{
    return cpuGetAddress(cpuGetDS());
}

int
cpuGetAddrES(uint32_t offset)
{
    return cpuGetAddress(cpuGetES());
}

int
cpuGetAddrFS(uint32_t offset)
{
    return cpuGetAddress(cpuGetFS());
}

int
cpuGetAddrGS(uint32_t offset)
{
    return cpuGetAddress(cpuGetGS());
}

int
cpuGetAddrSS(uint32_t offset)
{
    return cpuGetAddress(cpuGetSS());
}

// Reads a word from the stack at displacement "disp"
uint32_t
cpuGetStack(uint32_t disp)
{
}

uint8_t read_port(unsigned port);
void write_port(unsigned port, uint8_t value);
void bios_routine(unsigned inum);

// CPU interface
void execute(void); // 1 ins.
void init_cpu(void);

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
void cpuSetFS(unsigned v);
void cpuSetGS(unsigned v);
void cpuSetSS(unsigned v);
void cpuSetDS(unsigned v);
void cpuSetIP(unsigned v);

void cpuSetEAX(uint32_t v);
void cpuSetECX(uint32_t v);
void cpuSetEDX(uint32_t v);
void cpuSetEBX(uint32_t v);
void cpuSetESP(uint32_t v);
void cpuSetEBP(uint32_t v);
void cpuSetESI(uint32_t v);
void cpuSetEDI(uint32_t v);
void cpuSetEIP(uint32_t v);

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
unsigned cpuGetFS(void);
unsigned cpuGetGS(void);
unsigned cpuGetSS(void);
unsigned cpuGetDS(void);
unsigned cpuGetIP(void);

uint32_t cpuGetEAX(void);
uint32_t cpuGetECX(void);
uint32_t cpuGetEDX(void);
uint32_t cpuGetEBX(void);
uint32_t cpuGetESP(void);
uint32_t cpuGetEBP(void);
uint32_t cpuGetESI(void);
uint32_t cpuGetEDI(void);
uint32_t cpuGetEIP(void);

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
    cpuFlag_OF = 2048,
    cpuFlag_IOPL0 = 0x1000,
    cpuFlag_IOPL1 = 0x2000,
    cpuFlag_NT = 0x4000,
    cpuFlag_MT = 0x8000,
    cpuEflag_RF = 0x10000,
    cpuEflag_VM = 0x40000,
    cpuEflag_AC = 0x80000,
    cpuEflag_VIF = 0x100000,
    cpuEflag_VIP = 0x200000,
    cpuEflag_ID = 0x400000,
};

void cpuSetFlag(enum cpuFlags flag);
void cpuClrFlag(enum cpuFlags flag);

// Alter direct CPU flags, only use on startup
void cpuSetStartupFlag(enum cpuFlags flag);
void cpuClrStartupFlag(enum cpuFlags flag);
