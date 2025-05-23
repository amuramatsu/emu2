#include "loader.h"
#include "dbg.h"
#include "dosnames.h"
#include "emu.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// First MCB
static uint16_t mcb_start = 0x40;
// MCB allocation strategy
static uint8_t mcb_alloc_st = 0;
// PSP (Program Segment Prefix) location
static uint16_t current_PSP;

extern uint32_t indos_flag;

// MS-DOS version to emulate on FCB command line parsing.
#define FCB_PARSE_DOS (3)

static int valid_fcb_sep(int i)
{
    // NOTE: in DOS 1.0 many other characters were valid FCB separators,
    // then the list was restricted from DOS 1.1 upwards. The '/' was added
    // back on DOS 3. See bug #61.
    return isspace(i) || i == ',' || i == '=' || i == ';' || i == '/';
}

static int valid_fcb_char(int i)
{
    return isalnum(i) || (i > 127 && i < 229) || (i > 229) ||
           (i == '\\' && FCB_PARSE_DOS == 1) || strchr("!#$%&'()-@^_`{}~?<>", i);
}

// The FCB parsing states.
#define FCB_PARSE_INIT      0
#define FCB_PARSE_INIT_PLUS 10
#define FCB_PARSE_FCB1      1
#define FCB_PARSE_FCB1_EXT  2
#define FCB_PARSE_SEP       3
#define FCB_PARSE_SEP_PLUS  13
#define FCB_PARSE_SEP_PURGE 23
#define FCB_PARSE_FCB2      4
#define FCB_PARSE_FCB2_EXT  5
#define FCB_PARSE_EXIT      6

static void cmdline_to_fcb(const char *cmd_line, uint8_t *fcb1, uint8_t *fcb2)
{
    int i = 0;
    int state = FCB_PARSE_INIT;
    uint8_t *offset = fcb1 + 1;
    *fcb1 = 0;
    *fcb2 = 0;
    memset(fcb1 + 1, ' ', 11);
    memset(fcb2 + 1, ' ', 11);
    while(cmd_line[i])
    {
        int c = cmd_line[i];
        if(FCB_PARSE_DOS == 1 && c == ';')
        {
            c = '+';
        }
        switch(state)
        {
        case FCB_PARSE_INIT:
        case FCB_PARSE_INIT_PLUS:
            switch(c)
            {
            case '.':
                offset = fcb1 + 9;
                state = FCB_PARSE_FCB1_EXT;
                break;
            case '+':
                if(state == FCB_PARSE_INIT)
                {
                    state = FCB_PARSE_INIT_PLUS;
                }
                else
                {
                    offset = fcb2 + 1;
                    state = (FCB_PARSE_DOS == 1) ? FCB_PARSE_SEP : FCB_PARSE_SEP_PURGE;
                }
                break;
            case '*':
                for(int j = 0; j < 8; j++)
                {
                    fcb1[j + 1] = '?';
                }
                offset = fcb1 + 9;
                break;
            default:
                if(valid_fcb_sep(c))
                {
                    if(FCB_PARSE_DOS > 1 && state == FCB_PARSE_INIT_PLUS &&
                       (FCB_PARSE_DOS > 2 || !isspace(c)))
                    {
                        offset = fcb2 + 1;
                        state = FCB_PARSE_SEP_PURGE;
                        i--;
                    }
                    break;
                }
                if(valid_fcb_char(c))
                {
                    if(cmd_line[i + 1] == ':')
                    {
                        *fcb1 = toupper(c) - 'A' + 1;
                        i++;
                    }
                    else
                    {
                        *offset = toupper(c);
                        offset++;
                    }
                    state = FCB_PARSE_FCB1;
                }
                else
                {
                    if(FCB_PARSE_DOS == 1)
                    {
                        state = FCB_PARSE_EXIT;
                    }
                    else
                    {
                        offset = fcb2 + 1;
                        state = FCB_PARSE_SEP_PURGE;
                    }
                }
                break;
            }
            break;
        case FCB_PARSE_FCB1:
            switch(c)
            {
            case '.':
                offset = fcb1 + 9;
                state = FCB_PARSE_FCB1_EXT;
                break;
            case '*':
                while(offset - fcb1 - 1 < 8)
                {
                    *offset = '?';
                    offset++;
                }
                break;
            case '+':
                if(FCB_PARSE_DOS == 1)
                {
                    offset = fcb2 + 1;
                    state = FCB_PARSE_SEP_PLUS;
                    break;
                }
            case ':':
                if(FCB_PARSE_DOS == 1)
                {
                    offset = fcb2 + 1;
                    state = FCB_PARSE_FCB2;
                    break;
                }
            default:
                if(valid_fcb_sep(c))
                {
                    offset = fcb2 + 1;
                    state = FCB_PARSE_SEP;
                    break;
                }
                if(!valid_fcb_char(c))
                {
                    offset = fcb2 + 1;
                    if(FCB_PARSE_DOS == 1)
                    {
                        state = FCB_PARSE_EXIT;
                    }
                    else
                    {
                        offset = fcb2 + 1;
                        state = FCB_PARSE_SEP_PURGE;
                    }
                    break;
                }
                if(offset - fcb1 - 1 < 8)
                {
                    *offset = toupper(c);
                    offset++;
                }
                break;
            }
            break;
        case FCB_PARSE_FCB1_EXT:
            switch(c)
            {
            case '.':
                if(FCB_PARSE_DOS == 1)
                {
                    offset = fcb2 + 9;
                    state = FCB_PARSE_FCB2_EXT;
                }
                else
                {
                    offset = fcb2 + 1;
                    state = FCB_PARSE_SEP_PURGE;
                }
                break;
            case '*':
                while(offset - fcb1 - 9 < 3)
                {
                    *offset = '?';
                    offset++;
                }
                break;
            case '+':
                if(FCB_PARSE_DOS == 1)
                {
                    offset = fcb2 + 1;
                    state = FCB_PARSE_SEP_PLUS;
                    break;
                }
            case ':':
                if(FCB_PARSE_DOS == 1)
                {
                    offset = fcb2 + 1;
                    state = FCB_PARSE_FCB2;
                    break;
                }
            default:
                if(valid_fcb_sep(c))
                {
                    offset = fcb2 + 1;
                    state = FCB_PARSE_SEP;
                    break;
                }
                if(!valid_fcb_char(c))
                {
                    if(FCB_PARSE_DOS == 1)
                    {
                        state = FCB_PARSE_EXIT;
                    }
                    else
                    {
                        offset = fcb2 + 1;
                        state = FCB_PARSE_SEP_PURGE;
                    }
                    break;
                }
                if(offset - fcb1 - 9 < 3)
                {
                    *offset = toupper(c);
                    offset++;
                }
                break;
            }
            break;
        case FCB_PARSE_SEP_PURGE:
            if(valid_fcb_sep(c))
            {
                state = FCB_PARSE_SEP;
                i--;
                break;
            }
            break;
        case 3:
        case 13:
            switch(c)
            {
            case '.':
                offset = fcb2 + 9;
                state = FCB_PARSE_FCB2_EXT;
                break;
            case '+':
                if(state == FCB_PARSE_SEP)
                {
                    state = FCB_PARSE_SEP_PLUS;
                }
                else
                {
                    state = FCB_PARSE_EXIT;
                }
                break;
            case '*':
                for(int j = 0; j < 8; j++)
                {
                    fcb2[j + 1] = '?';
                }
                break;
            default:
                if(valid_fcb_sep(c))
                {
                    if(FCB_PARSE_DOS > 2 && state == FCB_PARSE_SEP_PLUS)
                    {
                        state = FCB_PARSE_EXIT;
                    }
                    break;
                }
                if(valid_fcb_char(c))
                {
                    if(cmd_line[i + 1] == ':')
                    {
                        *fcb2 = toupper(c) - 'A' + 1;
                        i++;
                    }
                    else
                    {
                        *offset = toupper(c);
                        offset++;
                    }
                    state = FCB_PARSE_FCB2;
                }
                else
                {
                    state = FCB_PARSE_EXIT;
                }
                break;
            }
            break;
        case FCB_PARSE_FCB2:
            switch(c)
            {
            case '.':
                offset = fcb2 + 9;
                state = FCB_PARSE_FCB2_EXT;
                break;
            case '*':
                while(offset - fcb2 - 1 < 8)
                {
                    *offset = '?';
                    offset++;
                }
                break;
            case '+':
            case ';':
            case ':': state = FCB_PARSE_EXIT; break;
            default:
                if(valid_fcb_sep(c))
                {
                    state = FCB_PARSE_EXIT;
                    break;
                }
                if(!valid_fcb_char(c))
                {
                    state = FCB_PARSE_EXIT;
                    break;
                }
                if(offset - fcb2 - 1 < 8)
                {
                    *offset = toupper(c);
                    offset++;
                }
                break;
            }
            break;
        case FCB_PARSE_FCB2_EXT:
            switch(c)
            {
            case '*':
                while(offset - fcb2 - 9 < 3)
                {
                    *offset = '?';
                    offset++;
                }
                state = FCB_PARSE_EXIT;
                break;
            case '.':
            case '+':
            case ';':
            case ':': state = FCB_PARSE_EXIT; break;
            default:
                if(valid_fcb_sep(c))
                {
                    state = FCB_PARSE_EXIT;
                    break;
                }
                if(!valid_fcb_char(c))
                {
                    state = FCB_PARSE_EXIT;
                    break;
                }
                if(offset - fcb2 - 9 < 3)
                {
                    *offset = toupper(c);
                    offset++;
                }
                break;
            }
            break;
        default: break;
        }
        if(state == FCB_PARSE_EXIT)
        {
            break;
        }
        i++;
    }
}

// Mem handling
static void mcb_new(uint16_t mcb, uint16_t owner, uint16_t size, int last)
{
    put8(mcb * 16 + 0, last ? 'Z' : 'M');
    put16(mcb * 16 + 1, owner);
    put16(mcb * 16 + 3, size);
    debug(debug_dos, "\tmcb_new: mcb:$%04X type:%c owner:$%04X size:$%04X\n", mcb,
          last ? 'Z' : 'M', owner, size);
}

static uint16_t mcb_size(uint16_t mcb)
{
    return get16(mcb * 16 + 3);
}

static void mcb_set_size(uint16_t mcb, uint16_t sz)
{
    put16(mcb * 16 + 3, sz);
}

static uint16_t mcb_owner(uint16_t mcb)
{
    return get16(mcb * 16 + 1);
}

static void mcb_set_owner(uint16_t mcb, uint16_t owner)
{
    put16(mcb * 16 + 1, owner);
}

static void mcb_name(uint16_t mcb, char *buf)
{
    for(int i = 0; i < 8; i++)
        buf[i] = get8(mcb * 16 + 8 + i);
}

static void mcb_set_name(uint16_t mcb, const char *buf)
{
    for(int i = 0; i < 8; i++)
        put8(mcb * 16 + 8 + i, buf[i]);
}

static uint16_t mcb_ok(uint16_t mcb)
{
    return get8(mcb * 16) == 'Z' || get8(mcb * 16) == 'M';
}

static int mcb_is_last(uint16_t mcb)
{
    return get8(mcb * 16) == 'Z';
}

static uint16_t mcb_next(uint16_t mcb)
{
    if(mcb <= 0 || mcb_is_last(mcb))
        return 0;
    return mcb + mcb_size(mcb) + 1;
}

static void mcb_set_last(uint16_t mcb, int last)
{
    put8(mcb * 16 + 0, last ? 'Z' : 'M');
}

static int mcb_is_free(uint16_t mcb)
{
    return mcb_ok(mcb) && mcb_owner(mcb) == 0;
}

static uint16_t mcb_grow_max(uint16_t mcb)
{
    uint16_t total = mcb_size(mcb);
    if(mcb_is_last(mcb))
        return total;
    uint16_t nxt = mcb_next(mcb);
    while(mcb_is_free(nxt))
    {
        total += 1 + mcb_size(nxt);
        mcb_set_size(mcb, total);
        mcb_set_last(mcb, mcb_is_last(nxt));
        if(mcb_is_last(nxt))
            break;
        nxt = mcb_next(nxt);
    }
    return total;
}

static void mcb_free(uint16_t mcb)
{
    mcb_set_owner(mcb, 0);
    mcb_grow_max(mcb);
}

static uint16_t mcb_alloc_new(uint16_t size, uint16_t owner, uint16_t *max)
{
    uint16_t mcb = mcb_start, best = 0;
    int stg = mcb_alloc_st & 0x3F;
    int best_slack = 0xFFFFF;
    *max = 0;
    while(1)
    {
        if(mcb_is_free(mcb))
        {
            int slack = mcb_size(mcb) - size;
            if(slack >= 0)
            {
                if(!best || (stg == 1 && slack < best_slack) || (stg >= 2))
                {
                    best_slack = slack;
                    best = mcb;
                }
            }
            else if(mcb_size(mcb) > *max)
                *max = mcb_size(mcb);
        }
        if(mcb_is_last(mcb))
            break;
        mcb = mcb_next(mcb);
    }
    if(!best)
        return 0; // No mcb is big enough
    if(!best_slack)
    {
        // mcb is the exact size
        mcb_new(best, owner, size, mcb_is_last(best));
        return best;
    }
    if(stg >= 2)
    {
        mcb_new(best + best_slack, owner, size, mcb_is_last(best));
        mcb_new(best, 0, best_slack - 1, 0);
        return best + best_slack;
    }
    else
    {
        mcb_new(best + size + 1, 0, best_slack - 1, mcb_is_last(best));
        mcb_new(best, owner, size, 0);
        return best;
    }
}

static uint16_t mcb_resize(uint16_t mcb, uint16_t size)
{
    debug(debug_dos, "\tmcb_resize: mcb:$%04X new size:$%04X\n", mcb, size);
    if(mcb_size(mcb) == size) // Do nothing!
        return size;
    uint16_t max = mcb_grow_max(mcb);
    if(mcb_size(mcb) > size)
    {
        mcb_new(mcb + size + 1, 0, mcb_size(mcb) - size - 1, mcb_is_last(mcb));
        mcb_new(mcb, mcb_owner(mcb), size, 0);
        return size;
    }
    else
    {
        return max;
    }
}

// External interfaces
void mcb_init(uint16_t mem_start, uint16_t mem_end)
{
    mcb_start = mem_start;
    mcb_new(mem_start, 0, mem_end - mem_start - 1, 1);
}

uint8_t mem_get_alloc_strategy(void)
{
    return mcb_alloc_st;
}

void mem_set_alloc_strategy(uint8_t s)
{
    mcb_alloc_st = s;
}

uint16_t mem_resize_segment(uint16_t seg, uint16_t size)
{
    return mcb_resize(seg - 1, size);
}

void mem_free_segment(uint16_t seg)
{
    mcb_free(seg - 1);
}

uint16_t mem_alloc_segment(uint16_t size, uint16_t *max)
{
    uint16_t mcb = mcb_alloc_new(size, current_PSP, max);
    if(mcb)
        return 1 + mcb;
    else
        return 0;
}

void mem_free_owned(unsigned psp_seg)
{
    int mcb = mcb_start;
    while(1)
    {
        if(mcb_owner(mcb) == psp_seg)
            mcb_free(mcb);
        if(mcb_is_last(mcb))
            break;
        mcb = mcb_next(mcb);
    }
}

/* The PSP block is before the loaded program:

Offset  Length  Description
0       2       An INT 20h instruction is stored here
2       2       Program ending address
4       1       Unused, reserved by DOS
5       5       Call to DOS function dispatcher
0Ah     4       Address of program termination code
0Eh     4       Address of break handler routine
12h     4       Address of critical error handler routine
16h     2       Parent's PSP segment
18h     20      Default Job File Table (not used at emu2)
2Ch     2       Segment address of environment area
2Eh     4       SS:SP on entry laste int 21h
32h     2       Size of Job File Table
34h     4       Pointer to Job File Table
38h     24      Reserved
50h     3       INT 21h, RETF instructions
53h     9       Reserved by DOS
5Ch     16      Default FCB #1
6Ch     20      Default FCB #2
80h     1       Length of command line string
81h     127     Command line string  */

/* Enviroment segment

VAR0=xxxx\0                 variable 1
VAR1=yyyy\0                 variable 2
...
VARn=zzz\0                  variable n
\0                          environment end marker
\1\0                        PROGNAME indicator
PROGRAM_NAME\0              PROGRAM FULL PATH (max 64 byte)
 */

// Creates main PSP
uint16_t create_PSP(const char *cmdline, const char *environment, uint16_t env_size,
                    const char *progname)
{
    // Put environment before PSP and program name, use rounded up environment size:
    uint16_t max;
    uint16_t env_mcb = mcb_alloc_new((env_size + 64 + 2 + 15) >> 4, 1, &max);
    // Creates JFT table
    uint16_t jft_mcb = mcb_alloc_new(16, 1, &max);
    // Creates a mcb to hold the PSP and the loaded program
    uint16_t psp_mcb = mcb_alloc_new(0xFFFF, 1, &max);
    if(!psp_mcb)
        psp_mcb = mcb_alloc_new(max, 1, &max);

    if(!env_mcb || !jft_mcb || !psp_mcb)
    {
        debug(debug_dos, "not enough memory for new PSP and environment");
        return 0;
    }

    uint16_t env_seg = env_mcb + 1;
    uint16_t jft_seg = jft_mcb + 1;
    uint16_t psp_seg = psp_mcb + 1;
    current_PSP = psp_seg;
    put16(indos_flag + 0xF, psp_seg);

    if(debug_active(debug_dos))
    {
        const char *p;
        debug(debug_dos, "\tcommand: '%s' args: '%s'\n", progname, cmdline);
        p = environment;
        while(*p)
        {
            debug(debug_dos, "\tenv: '%s'\n", p);
            p += strlen(p) + 1;
        }
        debug(debug_dos, "\tenv size: %u at $%04x\n", env_size, env_mcb + 1U);
        debug(debug_dos, "\tjft at $%08x\n", (jft_mcb + 1) << 4);
    }

    // Fill MCB owners:
    mcb_set_owner(env_mcb, psp_seg);
    mcb_set_owner(jft_mcb, psp_seg);
    mcb_set_owner(psp_mcb, psp_seg);

    // set MCB name
    char shortname[12];
    make_fcbname(shortname, progname);
    mcb_set_name(psp_mcb, shortname);

#ifdef IA32
    uint8_t dosPSP[256];
#else
    uint8_t *dosPSP = memory + psp_seg * 16;
#endif
    memset(dosPSP, 0, 256);

    dosPSP[0] = 0xCD;                   // 00: int20
    dosPSP[1] = 0x20;                   //
    dosPSP[2] = 0x00;                   // 02: memory end segment
    dosPSP[3] = 0x00;                   //
    dosPSP[5] = 0x9A;                   // 05: FAR call to CP/M entry point:
    dosPSP[6] = 0xF0;                   //       CALL FAR F01D:FEF0
    dosPSP[7] = 0xFE;                   //     this jumps to 0xC0, where an
    dosPSP[8] = 0x1D;                   //     INT 21h is patched.
    dosPSP[9] = 0xF0;                   //
    dosPSP[10] = get8(0x22 * 4);        // Handler for INT 22h
    dosPSP[11] = get8(0x22 * 4 + 1);    //
    dosPSP[12] = get8(0x22 * 4 + 2);    //
    dosPSP[13] = get8(0x22 * 4 + 3);    //
    dosPSP[14] = get8(0x23 * 4);        // Handler for INT 23h
    dosPSP[15] = get8(0x23 * 4 + 1);    //
    dosPSP[16] = get8(0x23 * 4 + 2);    //
    dosPSP[17] = get8(0x23 * 4 + 3);    //
    dosPSP[18] = get8(0x24 * 4);        // Handler for INT 24h
    dosPSP[19] = get8(0x24 * 4 + 1);    //
    dosPSP[20] = get8(0x24 * 4 + 2);    //
    dosPSP[21] = get8(0x24 * 4 + 3);    //
    dosPSP[22] = 0xFE;                  // 16: Parent PSP, use special value of FFFE
    dosPSP[23] = 0xFF;                  //     to signal no parent DOS process
    dosPSP[44] = 0xFF & env_seg;        // 2C: environment segment
    dosPSP[45] = 0xFF & (env_seg >> 8); //
    dosPSP[50] = 0xFF;                  // 32: max file handle
    dosPSP[51] = 0x00;                  //
    dosPSP[52] = 0x00;                  // 34: ptr to JFT
    dosPSP[53] = 0x00;                  //
    dosPSP[54] = 0xFF & jft_seg;        //
    dosPSP[55] = 0xFF & (jft_seg >> 8); //
    dosPSP[80] = 0xCD;                  // 50: INT 21h / RETF
    dosPSP[81] = 0x21;                  //
    dosPSP[82] = 0xCB;                  //
    unsigned l = strlen(cmdline);       //
    if(l > 126)                         //
        l = 126;                        //
    dosPSP[128] = l;                    // 80: Cmd line len
    memcpy(dosPSP + 129, cmdline, l);   //
    dosPSP[129 + l] = 0x00d;            // Adds an ENTER at the end
    // Copy environment:
#ifdef IA32
    meml_writes(env_seg * 16, environment, env_size);
#else
    memcpy(memory + env_seg * 16, environment, env_size);
#endif
    // Then, a word == 1
    put16(env_seg * 16 + env_size, 1);
    // And the program name
    if(progname)
    {
        int l = strlen(progname);
        if(l > 63)
            l = 63;
#ifdef IA32
        meml_writes(env_seg * 16 + env_size + 2, progname, l);
        put8(env_seg * 16 + env_size + 2 + l, 0);
#else
        memcpy(memory + env_seg * 16 + env_size + 2, progname, l);
        *(memory + env_seg * 16 + env_size + 2 + l) = 0;
#endif
    }
    cmdline_to_fcb(cmdline, dosPSP + 0x5C, dosPSP + 0x6C);
#ifdef IA32
    meml_writes(psp_seg * 16, dosPSP, 256);
#endif
    // Clear JFT
    for(int i = 0; i < 255; i++)
        put8(jft_seg * 16 + i, 0xFF);
    return psp_mcb;
}

unsigned get_current_PSP(void)
{
    unsigned n = get16(indos_flag + 0xF) & 0xffff;
    if(current_PSP != n)
        debug(debug_dos, "PSP is broken? %04x %04x\n", n, current_PSP);
    return current_PSP;
}

void set_current_PSP(uint16_t psp_seg)
{
    current_PSP = psp_seg;
    put16(indos_flag + 0xF, psp_seg);
}

static unsigned g16(uint8_t *buf)
{
    return buf[0] + (buf[1] << 8);
}

int dos_read_overlay(FILE *f, uint16_t load_seg, uint16_t reloc_seg)
{
    // First, read one block
    uint8_t buf[32];
    unsigned n = fread(buf, 1, sizeof(buf), f);
    if(n < 28 || g16(buf) != 0x5a4d)
    {
        // COM file. Read rest of data
        if(!n)
            return 1;

        int mem = load_seg * 16;
        int max = 0x100000 - mem - 512;
        fseek(f, 0, SEEK_SET);
#ifdef IA32
        char *loadbuf = malloc(max);
        if(!loadbuf)
            return 1;
        n = fread(loadbuf, 1, max, f);
        if(n)
            meml_writes(mem, loadbuf, n);
        free(loadbuf);
#else  // not IA32
        n = fread(memory + mem, 1, max, f);
#endif // IA32
        return n == 0;
    }

    // An EXE file, read rest of blocks
    unsigned head_size = g16(buf + 8) * 16;
    unsigned data_size = g16(buf + 4) * 512 + g16(buf + 2) - head_size;
    if(g16(buf + 2))
        data_size -= 512;

    if(data_size >= 0x100000 || load_seg * 16 + data_size >= 0x100000)
    {
        debug(debug_dos, "\texe size too big for memory\n");
        return 1;
    }

    // Seek to start of data
    fseek(f, head_size, SEEK_SET);
#ifdef IA32
    char *loadbuf = malloc(data_size);
    if(!loadbuf)
        return 1;
    n = fread(loadbuf, 1, data_size, f);
    if(n == data_size)
        meml_writes(load_seg * 16, loadbuf, n);
    free(loadbuf);
#else  // not IA32
    n = fread(memory + load_seg * 16, 1, data_size, f);
#endif // IA32
    debug(debug_dos, "\texe read %u of %u data bytes\n", n, data_size);
    if(n < data_size)
        return 1;

    unsigned reloc_off = g16(buf + 24);
    int nreloc = g16(buf + 6);

    // Seek to start of relocation data
    fseek(f, reloc_off, SEEK_SET);
    while(nreloc)
    {
        uint8_t reloc[4];
        if(4 != fread(reloc, 1, 4, f))
            return 1;
        uint16_t roff = g16(reloc);
        uint16_t rseg = load_seg + g16(reloc + 2);
        int pos = roff + 16 * rseg;
        uint16_t n = get16(pos);
        put16(pos, n + reloc_seg);
        reloc_off += 4;
        nreloc--;
    }
    return 0;
}

int dos_load_exe(FILE *f, uint16_t psp_mcb)
{
    // First, read exe header
    uint8_t buf[32];
    unsigned n = fread(buf, 1, sizeof(buf), f);
    if(n < 28 || g16(buf) != 0x5a4d)
    {
        // COM file. Read rest of data
        if(!n)
            return 0;

        // Expand MCB to fill all memory
        mcb_resize(psp_mcb, 0xFFFF);
        // Use actual MCB size to calculate max data size
        int max = (mcb_size(psp_mcb) - 16) * 16;

        int mem = (psp_mcb + 17) * 16;
        fseek(f, 0, SEEK_SET);
#ifdef IA32
        char *loadbuf = malloc(max);
        if(!loadbuf)
            return 0;
        n = fread(loadbuf, 1, max, f);
        if(n)
            meml_writes(mem, loadbuf, n);
        free(loadbuf);
#else // not IA32
        n = fread(memory + mem, 1, max, f);
#endif
        if(!n)
            return 0;

        // Fill top program address in PSP
        put16(psp_mcb * 16 + 16 + 2, psp_mcb + mcb_size(psp_mcb) + 1);

        cpuSetIP(0x100);
        cpuSetCS(psp_mcb + 1);
        cpuSetDS(psp_mcb + 1);
        cpuSetES(psp_mcb + 1);
        cpuSetSP(0xFFFE);
        cpuSetSS(psp_mcb + 1);
        cpuSetAX(0);
        cpuSetBX(0);
        cpuSetCX(0x00FF);
        cpuSetDX(psp_mcb + 1);
        cpuSetBP(0x91C); // From real DOS-5 and DOSBOX.
        cpuSetSI(cpuGetIP());
        cpuSetDI(cpuGetSP());

        return 1;
    }

    // An EXE file, read rest of blocks
    unsigned head_size = g16(buf + 8) * 16;
    unsigned data_blocks = g16(buf + 4);
    unsigned extra_bytes = g16(buf + 2);
    if(data_blocks & 0xF800)
    {
        debug(debug_dos, "\tinvalid number of blocks ($%04x), fixing.\n", data_blocks);
        data_blocks &= 0x07FF;
    }
    unsigned data_size = data_blocks * 512 - head_size;
    unsigned load_seg = psp_mcb + 17;

    // Get max and min memory needed
    // EXE size is size of data + PSP:
    unsigned exe_sz = (data_size + 256 + 15) >> 4;
    unsigned min_sz = g16(buf + 10) + exe_sz;
    unsigned max_sz = g16(buf + 12) ? g16(buf + 12) + exe_sz : 0xFFFF;
    if(max_sz > 0xFFFF)
        max_sz = 0xFFFF;

    // Try to resize PSP MCB
    uint16_t psp_sz = mcb_resize(psp_mcb, max_sz);
    if(psp_sz < min_sz && psp_sz < max_sz)
    {
        debug(debug_dos, "\texe read, not enough memory! (need:%d) (actual:%d)\n", min_sz,
              psp_sz);
        return 0;
    }

    debug(debug_dos, "\texe: bin=%04x min=%04x max=%04x, alloc %04x segments of memory\n",
          exe_sz, g16(buf + 10), g16(buf + 12), mcb_size(psp_mcb));

    // Fill top program address in PSP
    put16(psp_mcb * 16 + 16 + 2, psp_mcb + mcb_size(psp_mcb) + 1);

    // Seek to start of data and read
    fseek(f, head_size, SEEK_SET);
#ifdef IA32
    char *loadbuf = malloc(data_size);
    if(!loadbuf)
        return 1;
    n = fread(loadbuf, 1, data_size, f);
    if(n)
        meml_writes(load_seg * 16, loadbuf, n);
    free(loadbuf);
#else  // not IA32
    n = fread(memory + load_seg * 16, 1, data_size, f);
#endif // IA32
    // Adjust data_size depending on extra_bytes
    if(extra_bytes)
        data_size = data_size - 512 + extra_bytes;
    debug(debug_dos, "\texe read %u of %u data bytes\n", n, data_size);
    if(!n)
    {
        debug(debug_dos, "\texe too short!\n");
        return 0;
    }
    else if(n < data_size)
        debug(debug_dos, "\tWARNING: short program!\n");

    // EXE start at load address
    unsigned start = load_seg * 16;
    // PSP located just 0x100 bytes before
    debug(debug_dos, "\tPSP location: $%04X\n", psp_mcb + 1U);
    debug(debug_dos, "\tEXE start:    $%04X\n", start >> 4);

    // Get segment values
    cpuSetSS((load_seg + g16(buf + 14)) & 0xFFFF);
    cpuSetSP(g16(buf + 16));
    cpuSetCS((load_seg + g16(buf + 22)) & 0xFFFF);
    cpuSetIP(g16(buf + 20));
    cpuSetDS(psp_mcb + 1);
    cpuSetES(psp_mcb + 1);
    cpuSetAX(0);
    cpuSetBX(0);
    cpuSetCX(0x7309);
    cpuSetDX(psp_mcb + 1);
    cpuSetBP(0x91C); // From real DOS-5 and DOSBOX.
    cpuSetSI(cpuGetIP());
    cpuSetDI(cpuGetSP());

    unsigned reloc_off = g16(buf + 24);
    int nreloc = g16(buf + 6);
    // Seek to start of relocation data
    fseek(f, reloc_off, SEEK_SET);
    while(nreloc)
    {
        uint8_t reloc[4];
        if(4 != fread(reloc, 1, 4, f))
            return 0;
        uint16_t roff = g16(reloc);
        uint16_t rseg = load_seg + g16(reloc + 2);
        int pos = roff + 16 * rseg;
        uint16_t n = get16(pos);
        put16(pos, n + load_seg);
        reloc_off += 4;
        nreloc--;
    }
    return 1;
}
