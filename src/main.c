
#define _GNU_SOURCE

#include "dbg.h"
#include "dos.h"
#include "dosnames.h"
#include "emu.h"
#include "keyb.h"
#include "timer.h"
#include "video.h"
#include "os.h"
#ifdef EMS_SUPPORT
#include "ems.h"
#endif /* EMS_SUPPORT */

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

uint8_t read_port(unsigned port)
{
    if(port == 0x3DA) // CGA status register
    {
        static int retrace = 0;
        retrace++;
        return (retrace >> 1) & 0x09;
    }
    else if(port == 0x3D4 || port == 0x3D5)
        return video_crtc_read(port);
    else if(port >= 0x40 && port <= 0x43)
        return port_timer_read(port);
    else if(port >= 0x60 && port <= 0x65)
        return keyb_read_port(port);
    debug(debug_port, "port read %04x\n", port);
    return 0xFF;
}

void write_port(unsigned port, uint8_t value)
{
    if(port >= 0x40 && port <= 0x43)
        port_timer_write(port, value);
    else if(port == 0x03D4 || port == 0x03D5)
        video_crtc_write(port, value);
    else if(port >= 0x60 && port <= 0x65)
        keyb_write_port(port, value);
    else
        debug(debug_port, "port write %04x <- %02x\n", port, value);
}

void emulator_update(void)
{
    debug(debug_int, "emu update cycle\n");
    cpuTriggerIRQ(0);
    update_timer();
    check_screen();
    update_keyb();
    fflush(stdout);
}

// BIOS - GET EQUIPMENT FLAG
static void intr11(void)
{
    cpuSetAX(0x0021);
}

// BIOS - GET MEMORY
static void intr12(void)
{
    cpuSetAX(640);
}

// BIOS - GET BIOS TYPE
static void intr15(void)
{
    int ax = cpuGetAX();
    if(ax == 0x4900)
    {
        cpuClrFlag(cpuFlag_CF);
        cpuSetAX(0x0000);
        cpuSetBX(cpuGetBX() & 0xFF00);
    }
    else
        debug(debug_int, "UNHANDLED INT 15, AX=%04x\n", ax);
}

// Network access, ignored.
static void intr2a(void) {}

// Absolute disk read
static void intr25(void)
{
    debug(debug_int, "D-25%04X: CX=%04X\n", cpuGetAX(), cpuGetCX());
    // AH=80 : timeout
    // AL=02 : drive not ready
    cpuSetAX(0x8002);
    cpuSetFlag(cpuFlag_CF);

    // This call returns via RETF instead of IRET, we simulate this by
    // manipulating stack directly.
    // POP IP / CS / FLAGS
    int ip = cpuPopWord();
    int cs = cpuPopWord();
    int flags = cpuPopWord();
    // PUSH flags twice
    cpuPushWord(flags);
    cpuPushWord(flags);
    cpuPushWord(cs);
    cpuPushWord(ip);
}

// System Reset
NORETURN static void intr19(void)
{
    debug(debug_int, "INT 19: System reset!\n");
    exit(0);
}

// DOS/BIOS interface
void bios_routine(unsigned inum)
{
    if(inum == 0x21)
        intr21();
    else if(inum == 0x20)
        intr20();
    else if(inum == 0x22)
        intr22();
    else if(inum == 0x1A)
        intr1A();
    else if(inum == 0x19)
        intr19();
    else if(inum == 0x16)
        intr16();
    else if(inum == 0x10)
        intr10();
    else if(inum == 0x11)
        intr11();
    else if(inum == 0x12)
        intr12();
    else if(inum == 0x15)
        intr15();
    else if(inum == 0x06)
    {
        uint16_t ip = cpuGetStack(0);
        uint16_t cs = cpuGetStack(2);
        print_error("error, unimplemented opcode %02X at cs:ip = %04X:%04X\n",
                    get8(cpuGetAddress(cs, ip)), cs, ip);
    }
    else if(inum == 0x28)
        intr28();
    else if(inum == 0x25)
        intr25();
    else if(inum == 0x29)
        intr29();
    else if(inum == 0x2A)
        intr2a();
    else if(inum == 0x2f)
        intr2f();
    else if(inum == 0x8)
        ; // Timer interrupt - nothing to do
    else if(inum == 0x9)
        keyb_handle_irq(); // Keyboard interrupt
#ifdef EMS_SUPPORT
    else if(inum == 0x67)
        intr67();
#endif
    else
        debug(debug_int, "UNHANDLED INT %02x, AX=%04x\n", inum, cpuGetAX());
}

static int load_binary_prog(const char *name, int bin_load_addr)
{
    FILE *f = fopen(name, "rb");
    if(!f)
        print_error("can't open '%s': %s\n", name, strerror(errno));
    unsigned n = fread(memory + bin_load_addr, 1, 0x100000 - bin_load_addr, f);
    fclose(f);
    debug(debug_int, "load binary of %02x bytes\n", n);
    return 0;
}

// Checks memory at exit: used for unit testings.
static uint8_t *chk_mem_arr = 0;
static unsigned chk_mem_len = 0;
static void check_exit_mem(void)
{
    if(!chk_mem_len || !chk_mem_arr)
        return;

    for(unsigned i = 0; i < chk_mem_len; i++)
    {
        if(chk_mem_arr[i] != get8(i))
        {
            fprintf(stderr, "%s: check memory: differ at byte %X, %02X != %02X\n",
                    prog_name, i, chk_mem_arr[i], get8(i));
            break;
        }
    }
}

volatile int exit_cpu;
static void timer_alarm(int x)
{
    exit_cpu = 1;
}

NORETURN static void exit_handler(int x)
{
    exit(1);
}

static void init_bios_mem(void)
{
    // Some of those are also in video.c, we write a
    // default value here for programs that don't call
    // INT10 functions before reading.
    put8(0x413, 0x80); // ram size: 640k
    put8(0x414, 0x02); //
    // Store an "INT-19h" instruction in address FFFF:0000
    put8(0xFFFF0, 0xCB);
    put8(0xFFFF1, 0x19);
    // BIOS date at F000:FFF5
    put8(0xFFFF5, 0x30);
    put8(0xFFFF6, 0x31);
    put8(0xFFFF7, 0x2F);
    put8(0xFFFF8, 0x30);
    put8(0xFFFF9, 0x31);
    put8(0xFFFFA, 0x2F);
    put8(0xFFFFB, 0x31);
    put8(0xFFFFC, 0x37);

    update_timer();
}

int main(int argc, char **argv)
{
    int i;
    prog_name = argv[0];

    // Process command line options
    int bin_load_seg = 0, bin_load_ip = 0, bin_load_addr = -1;
    for(i = 1; i < argc; i++)
    {
        char flag;
        const char *opt = 0;
        char *ep;
        // Process options only *before* main program argument
        if(argv[i][0] != '-')
            break;
        flag = argv[i][1];
        // Check arguments:
        switch(flag)
        {
        case 'b':
        case 'r':
        case 'X':
            if(argv[i][2])
                opt = argv[i] + 2;
            else
            {
                if(i >= argc - 1)
                    print_usage_error("option '-%c' needs an argument.", flag);
                i++;
                opt = argv[i];
            }
        }
        // Process options
        switch(flag)
        {
        case 'h':
            print_usage();
        case 'v':
            print_version();
            exit(EXIT_SUCCESS);
        case 'b':
            bin_load_addr = strtol(opt, &ep, 0);
            if(*ep || bin_load_addr < 0 || bin_load_addr > 0xFFFF0)
                print_usage_error("binary load address '%s' invalid.", opt);
            bin_load_ip = bin_load_addr & 0x000FF;
            bin_load_seg = (bin_load_addr & 0xFFF00) >> 4;
            break;
        case 'r':
            bin_load_seg = strtol(opt, &ep, 0);
            if((*ep != 0 && *ep != ':') || bin_load_seg < 0 || bin_load_seg > 0xFFFF)
                print_usage_error("binary run segment '%s' invalid.", opt);
            if(*ep == 0)
            {
                bin_load_ip = bin_load_seg & 0x000F;
                bin_load_seg = bin_load_seg >> 4;
            }
            else
            {
                bin_load_ip = strtol(ep + 1, &ep, 0);
                if(*ep != 0 || bin_load_ip < 0 || bin_load_ip > 0xFFFF)
                    print_usage_error("binary run address '%s' invalid.", opt);
            }
            break;
        case 'X':
        {
            FILE *cf = fopen(opt, "rb");
            if(!cf)
                print_error("can't open '%s': %s\n", opt, strerror(errno));
            else
            {
                chk_mem_arr = malloc(1024 * 1024);
                chk_mem_len = fread(chk_mem_arr, 1, 1024 * 1024, cf);
                fprintf(stderr, "%s: will check %X bytes.\n", argv[0], chk_mem_len);
                atexit(check_exit_mem);
            }
        }
        break;
        default:
            print_usage_error("invalid option '-%c'.", flag);
        }
    }

    // Move remaining options
    int j = 1;
    for(; i < argc; i++, j++)
        argv[j] = argv[i];
    argc = j;

    if(argc < 2)
        print_usage_error("program name expected.");

    // Init debug facilities
    init_debug(argv[1]);
    init_cpu();

    if(bin_load_addr >= 0)
    {
        load_binary_prog(argv[1], bin_load_addr);
        cpuSetIP(bin_load_ip);
        cpuSetCS(bin_load_seg);
        cpuSetDS(0);
        cpuSetES(0);
        cpuSetSP(0xFFFF);
        cpuSetSS(0);
    }
    else
        init_dos(argc - 1, argv + 1);

    struct sigaction timer_action, exit_action;
    exit_action.sa_handler = exit_handler;
    timer_action.sa_handler = timer_alarm;
    sigemptyset(&exit_action.sa_mask);
    sigemptyset(&timer_action.sa_mask);
    exit_action.sa_flags = timer_action.sa_flags = 0;
    sigaction(SIGALRM, &timer_action, NULL);
    // Install an exit handler to allow exit functions to run
    sigaction(SIGHUP, &exit_action, NULL);
    sigaction(SIGINT, &exit_action, NULL);
    sigaction(SIGQUIT, &exit_action, NULL);
    sigaction(SIGPIPE, &exit_action, NULL);
    sigaction(SIGTERM, &exit_action, NULL);
    struct itimerval itv;
    itv.it_interval.tv_sec = 0;
    itv.it_interval.tv_usec = 54925;
    itv.it_value.tv_sec = 0;
    itv.it_value.tv_usec = 54925;
    setitimer(ITIMER_REAL, &itv, 0);
    init_bios_mem();
    video_init_mem();
    while(1)
    {
        exit_cpu = 0;
        execute();
        emulator_update();
    }
}
