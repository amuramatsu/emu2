#include <compiler.h>
#include <ia32/cpu.h>
#include <ia32/instructions/fpu/fp.h>
#include <../dbg.h>
#include <../emu.h>

void bios_routine(unsigned inum);
static uint16_t irq_mask; // IRQs pending

static void
debug_regs(void)
{
    debug(debug_cpu, "EAX=%08X EBX=%08X ECX=%08X EDX=%08X ",
          CPU_EAX, CPU_EBX, CPU_ECX, CPU_EDX);
    debug(debug_cpu, "ESP=%04X EBP=%04X ESI=%04X EDI=%04X ",
          CPU_ESP, CPU_EBP, CPU_ESI, CPU_EDI);
    debug(debug_cpu, "DS=%04X ES=%04X FS=%04X GS=%04X SS=%04X CS=%04X EIP=%08X ",
          CPU_DS, CPU_ES, CPU_FS, CPU_GS, CPU_SS, CPU_CS, CPU_EIP);
    debug(debug_cpu, "%s %s %s %s %s %s %s %s %s ",
          (CPU_EFLAG & O_FLAG) ? "OV" : "NV",
          (CPU_EFLAG & D_FLAG) ? "DN" : "UP",
          (CPU_EFLAG & I_FLAG) ? "EI" : "DI",
          (CPU_EFLAG & T_FLAG) ? "TR" : "tr",
          (CPU_EFLAG & S_FLAG) ? "NG" : "PL",
          (CPU_EFLAG & Z_FLAG) ? "ZR" : "NZ",
          (CPU_EFLAG & A_FLAG) ? "AC" : "NA",
          (CPU_EFLAG & P_FLAG) ? "PE" : "PO",
          (CPU_EFLAG & C_FLAG) ? "CY" : "NC");
    debug(debug_cpu, "%s %s %s %s %s %s IOPL%d\n",
          (CPU_EFLAG & VIP_FLAG) ? "VIP" : "vip",
          (CPU_EFLAG & VIF_FLAG) ? "VIF" : "vif",
          (CPU_EFLAG & AC_FLAG) ? "AC" : "ac",
          (CPU_EFLAG & VM_FLAG) ? "VM" : "vm",
          (CPU_EFLAG & RF_FLAG) ? "RF" : "rf",
          (CPU_EFLAG & NT_FLAG) ? "NT" : "nt",
          (CPU_EFLAG >> 12) & 0x03); // IOPL
}

static void
debug_instruction(uint32_t eip, int iret)
{
    if(iret)
        debug(debug_cpu, "%04x:%08x: %s%s\n",
	      CPU_CS, eip, "                        ", "(iret)");
    else
        debug(debug_cpu, "%s\n", cpu_disasm2str(eip));
}

void
emu2_cpu_debugout(const char *format, ...)
{
    char buf[1024];
    va_list ap;

    if(!debug_active(debug_cpu))
        return;

    va_start(ap, format);
    vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    buf[sizeof(buf)-1] = 0;
    debug(debug_cpu, "%s\n", buf);
}

void
emu2_hook(void)
{
    UINT32 addr;
    int iret = 0;
    if (!CPU_STAT_PM || CPU_STAT_VM86)
    {
	addr = CPU_EIP + (CPU_CS << 4);
        if(addr < 0x100)
        {
            bios_routine(addr & 0xFF);
            iret = 1;
        }
    }
    if(debug_active(debug_cpu))
        debug_regs();
    if(iret)
    {
        //IRET
        uint32_t stack = CPU_SS * 16 + CPU_SP;
        if(debug_active(debug_cpu))
            debug(debug_cpu, "%04x:%08x: %s%s\n",
	          CPU_CS, CPU_EIP, "??                      ", "(iret)");
        CPU_IP = meml_read16(stack);
        LOAD_SEGREG(CPU_CS_INDEX,  meml_read16(stack+2));
        CPU_FLAG = meml_read16(stack+4);
        CPU_SP += 6;
        if(debug_active(debug_cpu))
            debug_regs();
    }
}

static void handle_irq(void)
{
    if((CPU_EFLAG & I_FLAG) && irq_mask)
    {
        // Get lower set bit (highest priority IRQ)
        uint16_t bit = irq_mask & -irq_mask;
        if(bit)
        {
            uint8_t bp[16] = {0, 1, 2, 5, 3, 9, 6, 11, 15, 4, 8, 10, 14, 7, 13, 12};
            uint8_t irqn = bp[(bit * 0x9af) >> 12];
            debug(debug_int, "handle irq, mask=$%04x irq=%d\n", irq_mask, irqn);
            irq_mask &= ~bit;
            if(irqn < 8)
                ia32_interrupt(8 + irqn, 0);
            else
                ia32_interrupt(0x68 + irqn, 0);
        }
    }
}

extern volatile int exit_cpu;
// CPU interface
void execute(void)
{
    CPU_BASECLOCK = 100; // each operation step must be lower than 100 tick
    for(; !exit_cpu;) {
        CPU_REMCLOCK = CPU_BASECLOCK;
        handle_irq();
        ia32_step();
    }
}

extern int cpu_inst_trace;
void init_cpu(void)
{
    if(debug_active(debug_cpu))
        cpu_inst_trace = 1;
    i386c_initialize();
    fpu_initialize();
    ia32reset();
}

// Register reading/writing
void cpuSetAL(unsigned v) { CPU_AL = v; }
void cpuSetAX(unsigned v) { CPU_AX = v; }
void cpuSetCX(unsigned v) { CPU_CX = v; }
void cpuSetDX(unsigned v) { CPU_DX = v; }
void cpuSetBX(unsigned v) { CPU_BX = v; }
void cpuSetSP(unsigned v) { CPU_SP = v; }
void cpuSetBP(unsigned v) { CPU_BP = v; }
void cpuSetSI(unsigned v) { CPU_SI = v; }
void cpuSetDI(unsigned v) { CPU_DI = v; }
void cpuSetES(unsigned v) { LOAD_SEGREG(CPU_ES_INDEX, v); }
void cpuSetCS(unsigned v) { LOAD_SEGREG(CPU_CS_INDEX, v); }
void cpuSetFS(unsigned v) { LOAD_SEGREG(CPU_FS_INDEX, v); }
void cpuSetGS(unsigned v) { LOAD_SEGREG(CPU_GS_INDEX, v); }
void cpuSetSS(unsigned v) { LOAD_SEGREG(CPU_SS_INDEX, v); }
void cpuSetDS(unsigned v) { LOAD_SEGREG(CPU_DS_INDEX, v); }
void cpuSetIP(unsigned v) { CPU_IP = v; }

void cpuSetEAX(uint32_t v) { CPU_EAX = v; }
void cpuSetECX(uint32_t v) { CPU_ECX = v; }
void cpuSetEDX(uint32_t v) { CPU_EDX = v; }
void cpuSetEBX(uint32_t v) { CPU_EBX = v; }
void cpuSetESP(uint32_t v) { CPU_ESP = v; }
void cpuSetEBP(uint32_t v) { CPU_EBP = v; }
void cpuSetESI(uint32_t v) { CPU_ESI = v; }
void cpuSetEDI(uint32_t v) { CPU_EDI = v; }
void cpuSetEIP(uint32_t v) { CPU_EIP = v; }

unsigned cpuGetAX(void) { return CPU_AX; }
unsigned cpuGetCX(void) { return CPU_CX; }
unsigned cpuGetDX(void) { return CPU_DX; }
unsigned cpuGetBX(void) { return CPU_BX; }
unsigned cpuGetSP(void) { return CPU_SP; }
unsigned cpuGetBP(void) { return CPU_BP; }
unsigned cpuGetSI(void) { return CPU_SI; }
unsigned cpuGetDI(void) { return CPU_DI; }
unsigned cpuGetES(void) { return CPU_ES; }
unsigned cpuGetCS(void) { return CPU_CS; }
unsigned cpuGetFS(void) { return CPU_FS; }
unsigned cpuGetGS(void) { return CPU_GS; }
unsigned cpuGetSS(void) { return CPU_SS; }
unsigned cpuGetDS(void) { return CPU_DS; }
unsigned cpuGetIP(void) { return CPU_IP; }

uint32_t cpuGetEAX(void) { return CPU_EAX; }
uint32_t cpuGetECX(void) { return CPU_ECX; }
uint32_t cpuGetEDX(void) { return CPU_EDX; }
uint32_t cpuGetEBX(void) { return CPU_EBX; }
uint32_t cpuGetESP(void) { return CPU_ESP; }
uint32_t cpuGetEBP(void) { return CPU_EBP; }
uint32_t cpuGetESI(void) { return CPU_ESI; }
uint32_t cpuGetEDI(void) { return CPU_EDI; }
uint32_t cpuGetEIP(void) { return CPU_EIP; }

void cpuSetFlag(enum cpuFlags flag)
{
    int addr = cpuGetAddrSS(cpuGetSP() + 4);
    put16(addr, get16(addr) | flag);
}
void cpuClrFlag(enum cpuFlags flag)
{
    int addr = cpuGetAddrSS(cpuGetSP() + 4);
    put16(addr, get16(addr) & ~flag);
}

// Alter direct CPU flags, only use on startup
void cpuSetStartupFlag(enum cpuFlags flag)
{
    CPU_FLAG |= flag;
}

void cpuClrStartupFlag(enum cpuFlags flag)
{
    CPU_FLAG &= ~flag;
}

int cpuGetAddress(uint16_t segment, uint16_t offset)
{
    return segment * 16 + offset;
}

int cpuGetAddrDS(uint16_t offset)
{
    return CPU_DS * 16 + offset;
}

int cpuGetAddrES(uint16_t offset)
{
    return CPU_ES * 16 + offset;
}

int cpuGetAddrSS(uint16_t offset)
{
    return CPU_SS * 16 + offset;
}

void cpuTriggerIRQ(int num)
{
    irq_mask |= (1 << num);
}

uint16_t cpuGetStack(uint16_t disp)
{
    return memr_read16(cpuGetSS(), cpuGetSP() + disp);
}
