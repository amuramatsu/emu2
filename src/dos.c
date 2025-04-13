#include "dos.h"
#include "codepage.h"
#include "dbg.h"
#include "dosnames.h"
#include "ems.h"
#include "emu.h"
#include "env.h"
#include "extmem.h"
#include "keyb.h"
#include "loader.h"
#include "os.h"
#include "timer.h"
#include "utils.h"
#include "video.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

// NLS data pointers
static uint32_t nls_uppercase_table;
static uint32_t nls_terminator_table;
static uint32_t nls_collating_table;
static uint32_t nls_dbc_set_table;
static uint8_t *nls_country_info;
static uint32_t dos_sysvars;
static uint32_t dos_append;
#define DOS_APPEND_SIZE 0x100

// Codepage Data
extern uint8_t *cp_dbcs;

// Last error - used to implement "get extended error"
static uint8_t dos_error;

// Returns "APPEND" path, if activated:
static const char *append_path(void)
{
#ifdef IA32
    static char buf[DOS_APPEND_SIZE];
    if ((get8(dos_append) & 0x01) == 0)
        return 0;
    meml_writes(dos_append + 2, buf, DOS_APPEND_SIZE);
    return buf;
#else
    return (memory[dos_append] & 0x01) ? ((char *)memory + dos_append + 2) : 0;
#endif
}

// Disk Transfer Area, buffer for find-first-file output.
static unsigned dosDTA;

// Emulated DOS version: default to DOS 3.30
static unsigned dosver = 0x1E03;

// Allocates memory for static DOS tables, from "rom" memory
uint32_t get_static_memory(uint16_t bytes, uint16_t align)
{
    static uint32_t current = 0xFE000; // Start allocating at FE00:0000
    // Align
    if(align)
        current = (current + align - 1) & ~(align - 1);
    if(current + bytes >= 0xFF000)
        print_error("not enough static DOS memory\n");
    current += bytes;
    return current - bytes;
}

// Execute PSP stacks
struct exec_PSP {
    struct exec_PSP *next;
    int psp;
    int parent;
    int parent_ax;
    int parent_cx;
    int parent_si;
    int parent_di;
    int parent_bp;
    int parent_ds;
    int parent_es;
};
struct exec_PSP *exec_psp_root = NULL;

// DOS file handles
#define max_handles (0xff)
static struct filetable {
    FILE *f;
    uint16_t devinfo;
    uint8_t dosname[11];
    int count;
} filetable[max_handles];

// Emulated indos flag addr
uint32_t indos_flag;
static uint32_t sda_table;

#define DOS_SFT_BASE  0x0f8000
static void update_dos_sft(int sidx, const struct stat *st);

static uint16_t guess_devinfo(FILE *f)
{
    int fn = fileno(f);
    if(isatty(fn))
        return 0x80D3;
    struct stat s;
    if(fstat(fn, &s))
        return 0x80C4;
    if(S_ISREG(s.st_mode))
        return 0x0002; // TODO: assuming C: drive
    return 0x80C0;
}

static void init_handles(void)
{
    filetable[0].f = stdin;
    filetable[1].f = stdout;
    filetable[2].f = stderr;
    filetable[3].f = stderr; // AUX
    filetable[4].f = stderr; // PRN
    // stdin,stdout,stderr: special, eof on input, is device
    for(int i = 0; i < 5; i++)
        filetable[i].devinfo = guess_devinfo(filetable[i].f);
}

static void restore_handles(void)
{
    // restore standard IO
    if(!filetable[0].f)
        filetable[0].f = stdin;
    if(!filetable[1].f)
        filetable[1].f = stdout;
    if(!filetable[2].f)
        filetable[2].f = stderr;
    if(!filetable[3].f)
        filetable[3].f = stderr;
    if(!filetable[4].f)
        filetable[4].f = stderr;
    for(int i = 0; i < 3; i++)
        filetable[i].devinfo = guess_devinfo(filetable[i].f);
}

static int get_new_sft(void)
{
    int i;
    for(i = 0; i < max_handles; i++)
        if(!filetable[i].count && !filetable[i].f)
        {
            debug(debug_dos, "create new sft %d\n", i);
            filetable[i].count = 1;
            return i;
        }
    return -1;
}

static uint32_t get_jft_addr(unsigned cur_psp, int *num)
{
    unsigned psp_addr = get_current_PSP() * 16;
    *num = get16(psp_addr + 0x32) & 0xffff;
    return cpuGetAddress(get16(psp_addr + 0x36),
                         get16(psp_addr + 0x34));
}

static void get_new_handle(int *handle, int *sft_idx)
{
    int handle_num;
    uint32_t jft_addr = get_jft_addr(get_current_PSP(), &handle_num);
    int i;
    debug(debug_dos, "jft %08x, handle_num %d\n", jft_addr, handle_num);
    for(i = 0; i < handle_num; i++)
        if (get8(jft_addr + i) == 0xFF)
            break;
    if(!sft_idx)
    {
        *handle = (i == handle_num) ? -1 : i;
        debug(debug_dos, "create new handle %d\n", i);
        return;
    }
    int sidx = get_new_sft();
    if(i == handle_num || sidx < 0)
    {
        if(sidx>=0)
            filetable[sidx].count = 0;
        *handle = *sft_idx = -1;
        debug(debug_dos, "no new handle or sft\n");
        return;
    }
    
    *handle = i;
    *sft_idx = sidx;
    put8(jft_addr + i, sidx);
    debug(debug_dos, "create new handle %d assigned with sft %d\n", i, sidx);
}

static void dispose_handle(int handle, int sft_idx)
{
    int handle_num;
    uint32_t jft_addr = get_jft_addr(get_current_PSP(), &handle_num);
    if(handle >= 0 && handle < handle_num)
        put8(jft_addr + handle, 0xFF);
    if(sft_idx >= 0)
    {
        filetable[sft_idx].count = 0;
        filetable[sft_idx].f = NULL;
    }
}

static int handle_to_sidx(int h)
{
    int handle_num;
    uint32_t jft_addr = get_jft_addr(get_current_PSP(), &handle_num);
    if(h > handle_num)
        return -1;
    int idx = get8(jft_addr + h);
    if(idx == 0xff)
        return -1;
    return idx;
}

static int dos_close_file(int h)
{
    int handle_num;
    uint32_t jft_addr = get_jft_addr(get_current_PSP(), &handle_num);
    int sidx = handle_to_sidx(h);
    FILE *f = (sidx < 0) ? NULL : filetable[sidx].f;
    if(!f)
    {
        put8(jft_addr + h, 0xff);
        if(sidx >= 0)
            filetable[sidx].count = 0;
        cpuSetFlag(cpuFlag_CF);
        cpuSetAX(6);
        dos_error = 6;
        return -1;
    }
    put8(jft_addr + h, 0xff);
    if(filetable[sidx].count)
        filetable[sidx].count--;
    debug(debug_dos, "\tsft %d, ref. counter=%d\n", sidx, filetable[sidx].count);
    update_dos_sft(sidx, NULL);
    if(filetable[sidx].count)
        return 0;
    filetable[sidx].f = NULL;
    filetable[sidx].devinfo = 0;
    cpuClrFlag(cpuFlag_CF);
    dos_error = 0;
    if(f == stdin || f == stdout || f == stderr)
        return 0; // Never close standard streams
    fclose(f);
    debug(debug_dos, "\tsft %d is deallocated\n", sidx);
    return 0;
}

static void dos_close_allfile_owned(unsigned psp_seg)
{
    int handle_num;
    uint32_t jft_addr = get_jft_addr(get_current_PSP(), &handle_num);
    for(int i = 0; i < handle_num; i++)
    {
        int n = get8(jft_addr + i);
        if(n != 0xff)
            dos_close_file(i);
    }
}

static void set_handle(int h, int sidx)
{
    assert(sidx >= 0 && sidx <= max_handles && filetable[sidx].f);
    int handle_num;
    uint32_t jft_addr = get_jft_addr(get_current_PSP(), &handle_num);
    int prev_sidx = get8(jft_addr + h) * 0xFF;
    if(prev_sidx != 0xff)
        dos_close_file(h);
    filetable[sidx].count++;
    put8(jft_addr + h, sidx);
}

static void init_jft(unsigned psp)
{
    int handle_num = get8(psp*16 + 0x32) & 0xff;
    uint32_t tgt = cpuGetAddress(get16(psp*16 + 0x36), get16(psp*16 + 0x34));
    for(int i = 0; i < 5; i++)
        put8(tgt + i, i);
    for(int i = 5; i < handle_num; i++)
        put8(tgt + i, 0xFF);
    for(int i = 0; i < 5; i++)
        filetable[i].count++;
}

static void copy_jft(unsigned psp, uint32_t orig, int len)
{
    int handle_num = get8(psp*16 + 0x32) & 0xff;
    uint32_t tgt = cpuGetAddress(get16(psp*16 + 0x36), get16(psp*16 + 0x34));
    handle_num = (len < handle_num) ? len : handle_num;
    for(int i =0; i < handle_num; i++)
    {
        int sidx = get8(orig + i) & 0xff;
        if(sidx != 0xff && !filetable[sidx].f)
            sidx = 0xff;
        if(sidx != 0xff)
            filetable[sidx].count++;
        put8(tgt + i, sidx);
    }
}

static void create_dir(void)
{
    char *fname = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 1, 0);
    debug(debug_dos, "\tmkdir '%s' ", fname);
    if(0 != mkdir(fname, 0777))
    {
        free(fname);
        cpuSetFlag(cpuFlag_CF);
        if(errno == EACCES)
            dos_error = 5;
        else if(errno == ENAMETOOLONG || errno == ENOTDIR)
            dos_error = 3;
        else if(errno == ENOENT)
            dos_error = 2;
        else if(errno == EEXIST)
            dos_error = 5;
        else
            dos_error = 1;
        cpuSetAX(dos_error);
        debug(debug_dos, "ERROR %u\n", cpuGetAX());
        return;
    }
    dos_error = 0;
    debug(debug_dos, "OK\n");
    free(fname);
    cpuClrFlag(cpuFlag_CF);
}

static void remove_dir(void)
{
    char *fname = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 1, 0);
    debug(debug_dos, "\trmdir '%s' ", fname);
    if(0 != rmdir(fname))
    {
        free(fname);
        cpuSetFlag(cpuFlag_CF);
        if(errno == EACCES)
            dos_error = 5;
        else if(errno == ENAMETOOLONG || errno == ENOTDIR)
            dos_error = 3;
        else if(errno == ENOENT)
            dos_error = 2;
        else
            dos_error = 1;
        cpuSetAX(dos_error);
        debug(debug_dos, "ERROR %u\n", cpuGetAX());
        return;
    }
    dos_error = 0;
    debug(debug_dos, "OK\n");
    free(fname);
    cpuClrFlag(cpuFlag_CF);
}

static int dos_open_file(int create, int access_mode, int name_addr)
{
    int h, sidx;
    get_new_handle(&h, &sidx);
    if(h < 0)
    {
        dos_error = 4;
        cpuSetAX(4);
        cpuSetFlag(cpuFlag_CF);
        return 0;
    }
    char *fname = dos_unix_path(name_addr, create, append_path());
    if(!get8(name_addr) || !fname)
    {
        dispose_handle(h, sidx);
        debug(debug_dos, "\t(file not found)\n");
        dos_error = 2;
        cpuSetAX(2);
        cpuSetFlag(cpuFlag_CF);
        return 0;
    }
    const char *mode;
    int mflag = 0;
    if(create)
    {
        mflag = O_CREAT | O_RDWR;
        // Use exclusive access on create == 2, to fail on existing file
        mflag |= (create == 2 ? O_EXCL  :
                  create == 1 ? O_TRUNC : 0);
        mode = "w+b";
    }
    else
    {
        switch(access_mode & 7)
        {
        case 0:
            mflag = O_RDONLY;
            mode = "rb";
            break;
        case 1: // Write only, but DOS sometimes allows reading, so we open as read/write
        case 2:
            mflag = O_RDWR;
            mode = "r+b";
            break;
        default:
            free(fname);
            dos_error = 1;
            cpuSetAX(1);
            cpuSetFlag(cpuFlag_CF);
            return 0;
        }
    }
    debug(debug_dos, "\topen '%s', '%s', %04x\n", fname, mode, (unsigned)h);

    // TODO: should set file attributes in CX
    filetable[sidx].f = NULL;
    int fd = open(fname, mflag, 0666);
    struct stat st;
    if(fd != -1)
    {
        // Check if we opened a directory and fail
        if(0 != fstat(fd, &st) || S_ISDIR(st.st_mode))
            close(fd);
        else
            filetable[sidx].f = fdopen(fd, mode);
    }

    if(!filetable[sidx].f)
    {
        if(errno != ENOENT)
        {
            debug(debug_dos, "\t->%s.\n", strerror(errno));
            dos_error = 5;
        }
        else
        {
            debug(debug_dos, "not found.\n");
            dos_error = 2;
        }
        cpuSetAX(dos_error);
        cpuSetFlag(cpuFlag_CF);
        free(fname);
        return 0;
    }
    debug(debug_dos, "\t\tsft %d bound to %s\n", sidx, fname);
    // Set device info:
    if(!strcmp(fname, "/dev/null"))
        filetable[sidx].devinfo = 0x80C4;
    else if(!strcmp(fname, "/dev/tty"))
        filetable[sidx].devinfo = 0x80D3;
    else if(get8(name_addr + 1) == ':')
    {
        uint8_t c = get8(name_addr);
        c = (c >= 'a') ? c - 'a' : c - 'A';
        if(c > 26)
            c = dos_get_default_drive();
        filetable[sidx].devinfo = 0x0000 + c;
    }
    else
        filetable[sidx].devinfo = 0x0000 + dos_get_default_drive();
    make_fcbname((char *)filetable[sidx].dosname, getstr(name_addr, 128));

    update_dos_sft(sidx, &st);
    debug(debug_dos, "\t->OK.\n");
    cpuClrFlag(cpuFlag_CF);
    cpuSetAX(h);
    dos_error = 0;
    free(fname);
    return create + 1;
}

static int get_ex_fcb(void)
{
    return cpuGetAddrDS(cpuGetDX());
}

static int get_fcb(void)
{
    int fcb = cpuGetAddrDS(cpuGetDX());
    return get8(fcb) == 255 ? fcb + 7 : fcb;
}

static void get_fcb_handle(int *h, int *sidx)
{
    int handle = get16(0x18 + get_fcb());
    if (h)
        *h = handle;
    if (sidx)
        *sidx = handle_to_sidx(handle);
}

static void dos_show_fcb(void)
{
    if(!debug_active(debug_dos))
        return;

    int addr = cpuGetAddrDS(cpuGetDX());
    char *name = getstr(addr + 1, 11);
    debug(debug_dos,
          "\tFCB:"
          "[d=%02x:n=%.8s.%.3s:bn=%04x:rs=%04x:fs=%08x:h=%04x:rn=%02x:ra=%08x]\n",
          get8(addr), name, name + 8, get16(addr + 0x0C), get16(addr + 0x0E),
          get32(addr + 0x10), get16(addr + 0x18), get8(addr + 0x20),
          get32(addr + 0x21));
}

static void dos_open_file_fcb(int create)
{
    int h, sidx;
    get_new_handle(&h, &sidx);
    if(h < 0)
    {
        dos_error = 4;
        cpuSetAL(0xFF);
        cpuSetFlag(cpuFlag_CF);
        return;
    }
    int fcb_addr = get_fcb();
    char *fname = dos_unix_path_fcb(fcb_addr, create, append_path());
    if(!fname)
    {
        dispose_handle(h, sidx);
        dos_error = 2;
        debug(debug_dos, "\t(file not found)\n");
        cpuSetAL(0xFF);
        cpuSetFlag(cpuFlag_CF);
        return;
    }
    const char *mode = create ? "w+b" : "r+b";
    debug(debug_dos, "\topen fcb '%s', '%s', %04x ", fname, mode, (unsigned)h);
    filetable[sidx].f = fopen(fname, mode);
    if(!filetable[sidx].f)
    {
        dos_error = 4;
        debug(debug_dos, "%s.\n", strerror(errno));
        cpuSetAL(0xFF);
        cpuSetFlag(cpuFlag_CF);
        free(fname);
        return;
    }
    // Get file size
    fseek(filetable[sidx].f, 0, SEEK_END);
    long sz = ftell(filetable[sidx].f);
    fseek(filetable[sidx].f, 0, SEEK_SET);
    // Set FCB info:
    put16(fcb_addr + 0x0C, 0);   // block number
    put16(fcb_addr + 0x0E, 128); // record size
    put32(fcb_addr + 0x10, sz);  // file size
    put16(fcb_addr + 0x14, 0);   // date of last write
    put16(fcb_addr + 0x16, 0);   // time of last write
    put16(fcb_addr + 0x18, h);   // reserved - store DOS handle!
    put8(fcb_addr + 0x20, 0);    // current record
    // Do not initialize random position - old DOS apps assume each FCB holds
    // up to 33 bytes.
#if 0
    put16(fcb_addr + 0x21, 0);   // random position - only 3 bytes
    put8(fcb_addr + 0x23, 0);
#endif

    update_dos_sft(sidx, NULL);
    debug(debug_dos, "OK.\n");
    cpuClrFlag(cpuFlag_CF);
    cpuSetAL(0x00);
    dos_error = 0;
    dos_show_fcb();
    free(fname);
}

static void dos_seq_to_rand_fcb(int fcb)
{
    unsigned rsize = get16(0x0E + fcb);
    unsigned recnum = get8(0x20 + fcb);
    unsigned bnum = get16(0x0C + fcb);
    unsigned rand = recnum + 128 * bnum;
    put16(0x21 + fcb, rand & 0xFFFF);
    put8(0x23 + fcb, rand >> 16);
    if(rsize < 64)
        put8(0x24 + fcb, rand >> 24);
}

static int dos_rw_record_fcb(unsigned addr, int write, int update, int seq)
{
    int sidx;
    get_fcb_handle(NULL, &sidx);
    FILE *f = (sidx < 0) ? NULL : filetable[sidx].f;
    if(!f)
    {
        dos_error = 6;
        return 1; // no data read/write
    }

    int fcb = get_fcb();
    unsigned rsize = get16(0x0E + fcb);
    unsigned pos;

    if(seq)
    {
        unsigned recnum = get8(0x20 + fcb);
        unsigned bnum = get16(0x0C + fcb);
        pos = rsize * (recnum + 128 * bnum);
    }
    else if(rsize < 64)
        pos = rsize * get32(0x21 + fcb);
    else
        pos = rsize * (0xFFFFFF & get32(0x21 + fcb));

    uint8_t *buf = NULL;
#if defined(EMS_SUPPORT) || defined(IA32)
    int buf_allocated = 0;
    int tgt_ems = 0;
#endif
#ifdef EMS_SUPPORT
    if (in_ems_pageframe2(addr, rsize))
    {
        buf_allocated = 1;
        tgt_ems = 1;
        buf = malloc(rsize);
        if (buf && write)
            ems_getmem(buf, addr, rsize);
    }
    else
#endif //EMS_SUPPORT
#ifdef IA32
    {
        buf_allocated = 1;
        buf = malloc(rsize);
        if (buf && write)
            meml_reads(addr, buf, rsize);
    }
#else //not IA32
    buf = getptr(addr, rsize);
#endif //IA32
    if(!buf || !rsize)
    {
#if defined(EMS_SUPPORT) || defined(IA32)
        if(buf_allocated && buf)
            free(buf);
#endif
        debug(debug_dos, "\tbuffer pointer invalid\n");
        dos_error = 9;
        return 2; // segment wrap in DTA
    }
    // Seek to block and read
    if(fseek(f, pos, SEEK_SET))
        return 1; // no data read
    // Read / Write
    unsigned n = write ? fwrite(buf, 1, rsize, f) : fread(buf, 1, rsize, f);
    // Update random and block positions
    if(update)
    {
        unsigned rnum = (pos + ((n > 0) ? rsize : 0)) / rsize;
        put8(0x20 + fcb, rnum & 127);
        put16(0x0C + fcb, rnum / 128);
        if(!seq)
            dos_seq_to_rand_fcb(fcb);
    }
    // Update file size
    if(write && (pos + n > get32(fcb + 0x10)))
        put32(fcb + 0x10, pos + n);

    dos_error = 0;
    for(unsigned i = n; i < rsize; i++)
        buf[i] = 0;
#if defined(EMS_SUPPORT) || defined(IA32)
    if (buf_allocated)
    {
        if (!write)
        {
#ifdef EMS_SUPPORT
            if (tgt_ems)
                ems_putmem(addr, buf, rsize);
            else
#endif
#ifdef IA32
            meml_writes(addr, buf, rsize);
#else
            /*NOP*/;
#endif
        }
        free(buf);
    }
#endif
    update_dos_sft(sidx, NULL);
    if(n == rsize)
        return 0; // read/write full record
    else if(!n || write)
        return 1; // EOF on read, disk full on write
    else
        return 3; // read partial record
}

// Converts Unix time_t to DOS time/date
static uint32_t get_time_date(time_t tm)
{
    struct tm lt;
    if(localtime_r(&tm, &lt))
    {
        unsigned t = (lt.tm_hour << 11) | (lt.tm_min << 5) | (lt.tm_sec / 2);
        unsigned d = ((lt.tm_year - 80) << 9) | ((lt.tm_mon + 1) << 5) | (lt.tm_mday);
        return (d << 16) | t;
    }
    else
        return (1 << 16) | 1;
}

// Converts Unix mode to DOS attributes
static int get_attributes(mode_t md)
{
    int r = 0;
    // See if it's a special file
    if(S_ISDIR(md))
        r |= 1 << 4; // DIR
    else if(!S_ISREG(md))
        r |= 1 << 2; // SYSTEM
    else
        r |= 1 << 5; // ARCHIVE
    // See if the file is writable
    if(0 == (md & (S_IWOTH | S_IWGRP | S_IWUSR)))
        r |= 1 << 0; // READ_ONLY
    return r;
}

static void update_dos_sft(int sidx, const struct stat *st)
{
    int dos_major = dosver & 0x0f;
    int dos_minor = (dosver >> 8) & 0xff;
    uint8_t buf[0x4c];
    int attr, drive, name, size, timedate, start, len;

    assert(sidx >= 0 && sidx <= max_handles);
    if ((filetable[sidx].devinfo & 0xffe0) != 0) // check regular file or not
        return;
    debug(debug_dos, "\t\tupdate dos system file table %d\n", sidx);

    switch(dos_major)
    {
    case 2:
        debug(debug_dos, "\t\tDOS2 structure\n");
        attr     = 2;
        drive    = 3;
        name     = 4;
        size     = 0x13;
        timedate = 0x17;
        start    = 0x1c;
        len      = 0x28;
        break;

    case 3:
        debug(debug_dos, "\t\tDOS3%s structure\n", dos_minor < 10 ? "0" : "1");
        attr  = 4;
        drive = 5;
        if (dos_minor < 10) /* DOS 3.0x */
            name = 0x21;
        else
            name = 0x20;
        size     = 0x11;
        timedate = 0x0d;
        start    = 0x0b;
        len      = 0x35;
        break;

    default: /* DOS 4 and up */
        debug(debug_dos, "\t\tDOS4 structure\n");
        attr     = 4;
        drive    = 5;
        name     = 0x20;
        size     = 0x11;
        timedate = 0x03;
        start    = 0x0b;
        len      = 0x3b;
    }

    memset(buf, 0, sizeof(buf));
    if (filetable[sidx].count && filetable[sidx].f != 0)
    {
        struct stat _st;
        if(st == NULL)
        {
            if(fstat(fileno(filetable[sidx].f), &_st))
            {
                debug(debug_dos, "\t\t!!!FSTAT IS FAILED!!!\n");
                return;
            }
            st = &_st;
        }
        buf[0] = 1; // file count is byte (DOS2) or word (DOS3 above) but
        buf[1] = 0; // treat as always word
        buf[attr]  = get_attributes(st->st_mode);
        buf[drive] = filetable[sidx].devinfo & 0x1f;
        // copy name
        memcpy(buf + name, filetable[sidx].dosname, 11);
        if(st->st_size > 0xffffffff)
        {
            buf[size] = 0xff;
            buf[size+1] = 0xff;
            buf[size+2] = 0xff;
            buf[size+3] = 0xff;
        }
        else
        {
            buf[size] = st->st_size & 0xff;
            buf[size+1] = (st->st_size >> 8) & 0xff;
            buf[size+2] = (st->st_size >> 16) & 0xff;
            buf[size+3] = (st->st_size >> 24) & 0xff;
        }

        uint32_t t = get_time_date(st->st_mtime);
        buf[timedate] = t & 0xff;
        buf[timedate+1] = (t >> 8) & 0xff;
        buf[timedate+2] = (t >> 16) & 0xff;
        buf[timedate+3] = (t >> 24) & 0xff;

        // i-node is stored at start cluster field,
        // because djgpp treat start cluster as inode
        buf[start] = st->st_ino & 0xff;
        buf[start+1] = (st->st_ino >> 8) & 0xff;

        debug(debug_dos, "\t\tupdating done\n");
    }
    else
        debug(debug_dos, "\t\tclean up\n");

    uint32_t addr = DOS_SFT_BASE + 6 + sidx*len;
#ifdef IA32
    meml_writes(addr, buf, len);
#else
    memcpy(memory + addr, buf, len);
#endif
}

// DOS int 21, ah=43
static void intr21_43(void)
{
    unsigned al = cpuGetAX() & 0xFF;
    int dname = cpuGetAddrDS(cpuGetDX());
    if(al == 0 || al == 1)
    {
        // Get path
        char *fname = dos_unix_path(dname, 0, append_path());
        if(!fname)
        {
            debug(debug_dos, "\t(file not found)\n");
            cpuSetFlag(cpuFlag_CF);
            dos_error = 2;
            cpuSetAX(dos_error);
            return;
        }
        debug(debug_dos, "\tattr '%s' = ", fname);
        // Get current file attributes, check for error
        struct stat st;
        if(0 != stat(fname, &st))
        {
            cpuSetFlag(cpuFlag_CF);
            if(errno == EACCES)
                dos_error = 5;
            else if(errno == ENAMETOOLONG || errno == ENOTDIR)
                dos_error = 3;
            else if(errno == ENOENT)
                dos_error = 2;
            else
                dos_error = 1;
            cpuSetAX(dos_error);
            free(fname);
            debug(debug_dos, "ERROR %u\n", cpuGetAX());
            return;
        }
        int current = get_attributes(st.st_mode);
        if(al == 0)
        {
            // GET FILE ATTRIBUTES
            cpuSetCX(current);
        }
        else
        {
            // SET FILE ATTRIBUTES
            int dif = current ^ cpuGetCX();
            if(dif & 0x1C)
            {
                cpuSetFlag(cpuFlag_CF);
                dos_error = 5;
                cpuSetAX(dos_error);
                free(fname);
                debug(debug_dos, "ERROR %u\n", cpuGetAX());
                return;
            }
        }
        cpuClrFlag(cpuFlag_CF);
        debug(debug_dos, "%04X\n", cpuGetCX());
        free(fname);
        return;
    }
    //getstr(dname); /XXX
    cpuSetFlag(cpuFlag_CF);
    dos_error = 0;
    cpuSetAX(1);
    return;
}

// Each DTA (Data Transfer Area) in memory can hold a find-first data
// block. We simply encode our pointer in this area and use this struct
// to hold the values.
//#define NUM_FIND_FIRST_DTA 64
#define NUM_FIND_FIRST_DTA 256
static struct find_first_dta
{
    // List of files to return and pointer to current.
    struct dos_file_list *find_first_list;
    struct dos_file_list *find_first_ptr;
    // Address of DTA in dos memory - 0 is unused
    unsigned dta_addr;
} find_first_dta[NUM_FIND_FIRST_DTA];

// Search the list an returns the position
static struct find_first_dta *get_find_first_dta(void)
{
    int i;
    for(i = 0; i < NUM_FIND_FIRST_DTA; i++)
        if(find_first_dta[i].dta_addr == dosDTA)
            break;
    if(i == NUM_FIND_FIRST_DTA)
    {
        for(i = 0; i < NUM_FIND_FIRST_DTA; i++)
            if(!find_first_dta[i].dta_addr)
                break;
        if(i == NUM_FIND_FIRST_DTA)
            print_error("Too many find-first DTA areas opened\n");
        find_first_dta[i].dta_addr = dosDTA;
        find_first_dta[i].find_first_list = 0;
        find_first_dta[i].find_first_ptr = 0;
    }
    return &find_first_dta[i];
}

// Frees the find_first_dta list before terminating program
static void free_find_first_dta(void)
{
    for(int i = 0; i < NUM_FIND_FIRST_DTA; i++)
        if(find_first_dta[i].find_first_list)
        {
            dos_free_file_list(find_first_dta[i].find_first_list);
            find_first_dta[i].find_first_list = 0;
        }
}

// Removes a DTA from the list
static void clear_find_first_dta(struct find_first_dta *p)
{
    unsigned x = find_first_dta - p;
    if(x >= NUM_FIND_FIRST_DTA)
        return;
    p->dta_addr = 0;
    p->find_first_ptr = 0;
    dos_free_file_list(p->find_first_list);
    p->find_first_list = 0;
}

// DOS int 21, ah=4f
static void dos_find_next(int first)
{
    struct find_first_dta *p = get_find_first_dta();
    struct dos_file_list *d = p->find_first_ptr;
    if(!d || !p->find_first_ptr->unixname)
    {
        debug(debug_dos, "\t(end)\n");
        clear_find_first_dta(p);
        cpuSetFlag(cpuFlag_CF);
        dos_error = first ? 0x02 : 0x12;
        cpuSetAX(dos_error);
    }
    else
    {
        debug(debug_dos, "\t'%s' ('%s')\n", d->dosname, d->unixname);

        // Fills the Find First Data from a dos/unix name pair
        if(strcmp("//", d->unixname))
        {
            // Normal file/directory
            struct stat st;
            if(0 == stat(d->unixname, &st))
            {
                put8(dosDTA + 0x15, get_attributes(st.st_mode));
                put32(dosDTA + 0x16, get_time_date(st.st_mtime));
                put32(dosDTA + 0x1A, (st.st_size > 0x7FFFFFFF) ? 0x7FFFFFFF : st.st_size);
            }
            else
            {
                put8(dosDTA + 0x15, 0);
                put32(dosDTA + 0x16, 0x10001);
                put32(dosDTA + 0x1A, 0);
            }
        }
        else
        {
            // Fills volume label data
            put8(dosDTA + 0x15, 8);
            put32(dosDTA + 0x16, get_time_date(time(0)));
            put32(dosDTA + 0x1A, 0);
        }
        // Fills dos file name
        putmem(dosDTA + 0x1E, d->dosname, 13);
        // Next file
        p->find_first_ptr++;
        cpuClrFlag(cpuFlag_CF);
        // Some DOS programs require returning AX=0 on success.
        dos_error = 0;
        cpuSetAX(0);
    }
}

// DOS int 21, ah=4e
static void dos_find_first(void)
{
    struct find_first_dta *p = get_find_first_dta();
    // Gets all directory entries
    if(p->find_first_list)
        dos_free_file_list(p->find_first_list);

    // Check if we want the volume label
    int do_label = (cpuGetCX() & 8) != 0;
    int do_dirs = (cpuGetCX() & 16) != 0;
    p->find_first_list = dos_find_first_file(cpuGetAddrDS(cpuGetDX()), do_label, do_dirs);

    p->find_first_ptr = p->find_first_list;
    dos_find_next(1);
}

static void dos_find_next_fcb(void)
{
    struct find_first_dta *p = get_find_first_dta();
    struct dos_file_list *d = p->find_first_ptr;

    if(!d || !p->find_first_ptr->unixname)
    {
        debug(debug_dos, "\t(end)\n");
        clear_find_first_dta(p);
        dos_error = 0x12;
        cpuSetAL(0xFF);
    }
    else
    {
        debug(debug_dos, "\t'%s' ('%s')\n", d->dosname, d->unixname);
        // Fills output FCB at DTA - use extended or normal depending on input
        int exfcb = get8(get_ex_fcb()) == 0xFF;
        int ofcb = exfcb ? dosDTA + 7 : dosDTA;
        int pos = 1;
        for(uint8_t *c = d->dosname; *c; c++)
        {
            if(*c != '.')
                put8(ofcb + pos++, *c);
            else
                while(pos < 9)
                    put8(ofcb + pos++, ' ');
        }
        while(pos < 12)
            put8(ofcb + pos++, ' ');
        // Fill drive letter
        put8(ofcb, get8(get_fcb()));
        // Get file info
        if(strcmp("//", d->unixname))
        {
            // Normal file/directory
            struct stat st;
            if(0 == stat(d->unixname, &st))
            {
                put8(ofcb + 0x0C, get_attributes(st.st_mode));
                put32(ofcb + 0x17, get_time_date(st.st_mtime));
                put32(ofcb + 0x1D, (st.st_size > 0x7FFFFFFF) ? 0x7FFFFFFF : st.st_size);
            }
            else
            {
                put8(ofcb + 0x0C, 0);
                put32(ofcb + 0x17, 0x10001);
                put32(ofcb + 0x1D, 0);
            }
        }
        else
        {
            put8(ofcb + 0x0C, 8);
            put32(ofcb + 0x17, get_time_date(time(0)));
            put32(ofcb + 0x1D, 0);
        }
        if(exfcb)
        {
            put8(dosDTA, 0xFF);
            put8(dosDTA + 6, get8(ofcb + 0x0C));
        }
        p->find_first_ptr++;
        dos_error = 0;
        cpuSetAL(0x00);
    }
}

static void dos_find_first_fcb(void)
{
    struct find_first_dta *p = get_find_first_dta();
    // Gets all directory entries
    if(p->find_first_list)
        dos_free_file_list(p->find_first_list);

    int efcb = get_ex_fcb();
    int do_label = (get8(efcb) == 0xFF && get8(efcb + 6) == 0x08);
    p->find_first_list = dos_find_first_file_fcb(get_fcb(), do_label);
    p->find_first_ptr = p->find_first_list;
    dos_find_next_fcb();
}

// DOS int 21, ah=57
static void intr21_57(void)
{
    unsigned al = cpuGetAX() & 0xFF;
    int sidx = handle_to_sidx(cpuGetBX());
    FILE *f = (sidx < 0) ? NULL : filetable[sidx].f;
    if(!f)
    {
        cpuSetFlag(cpuFlag_CF);
        dos_error = 6;
        cpuSetAX(dos_error); // invalid handle
        return;
    }
    if(al == 0)
    {
        // GET FILE LAST WRITTEN TIME
        struct stat st;
        if(0 != fstat(fileno(f), &st))
        {
            cpuSetFlag(cpuFlag_CF);
            dos_error = 1;
            cpuSetAX(dos_error);
            return;
        }
        update_dos_sft(sidx, &st);
        dos_error = 0;
        cpuClrFlag(cpuFlag_CF);
        uint32_t td = get_time_date(st.st_mtime);
        cpuSetCX(td & 0xFFFF);
        cpuSetDX(td >> 16);
        return;
    }
    else if(al == 1)
    {
        update_dos_sft(sidx, NULL);
        // SET FILE LAST WRITTEN TIME
        cpuClrFlag(cpuFlag_CF);
        return;
    }
    cpuSetFlag(cpuFlag_CF);
    dos_error = 1;
    cpuSetAX(dos_error);
    return;
}

static void dos_get_drive_info(uint8_t drive)
{
    if(!drive)
        drive = dos_get_default_drive();
    else
        drive--;
    cpuSetAL(32);     // 16k clusters
    cpuSetCX(512);    // 512 bytes/sector
    cpuSetDX(0xFFFF); // total 1GB
    cpuSetBX(0x0000); // media ID byte, offset
    cpuSetDS(0x0000); // and segment
    dos_error = 0;
    cpuClrFlag(cpuFlag_CF);
}

static void fputc_unicode(uint8_t ch, FILE* fd)
{
    static int in_dbcs = 0;
    if (ch < 0x20)
        in_dbcs = 0;
    if (!in_dbcs && ch < 0x80)
        fputc(ch, fd);
    else {
        uint16_t uc = get_unicode(ch, NULL);
        uint8_t buf[5];
        uint8_t *p = buf;
        if(uc == 0) {
            in_dbcs = 1;
            return;
        }
        unicode_to_utf8(&p, uc);
        *p = '\0';
        fputs((char *)buf, fd);
        fflush(fd);
        in_dbcs = 0;
    }
}

// Writes a character to standard output.
static void dos_putchar(uint8_t ch, int sidx)
{
    if(filetable[sidx].devinfo == 0x80D3 && video_active())
    {
        // Handle TAB character here:
        if(ch == 0x09)
        {
            int n = 8 - (7 & video_get_col());
            while(n--)
                video_putch(' ');
        }
        else
            video_putch(ch);
    }
    else if(filetable[sidx].devinfo == 0x80D3)
        fputc_unicode(ch, filetable[sidx].f);
    else if(!filetable[sidx].f)
        fputc_unicode(ch, stdout);
    else if(!sidx && filetable[0].devinfo == 0x80D3 && filetable[1].devinfo == 0x80D3)
        // DOS programs can write to STDIN and expect output to the terminal.
        // This hack will only work if STDOUT is not redirected, in real DOS
        // you can redirect STDOUT and write to STDIN.
        fputc_unicode(ch, filetable[1].f);
    else
        fputc(ch, filetable[sidx].f);
}

static void intr21_9(void)
{
    int i = cpuGetAddrDS(cpuGetDX());

    for(; get8(i) != 0x24 && i < 0x100000; i++)
        dos_putchar(get8(i), 1);

    dos_error = 0;
    cpuSetAL(0x24);
}

static unsigned return_code;
// Runs the emulator again with given parameters
static int run_emulator(char *file, const char *prgname, char *cmdline, char *env)
{
    pid_t pid = fork();
    if(pid == -1)
        print_error("fork error, %s\n", strerror(errno));
    else if(pid != 0)
    {
        int status;
        while(waitpid(pid, &status, 0) == -1)
        {
            if(errno != EINTR)
                print_error("error waiting child, %s\n", strerror(errno));
        }
        return_code = (WEXITSTATUS(status) & 0xFF);
        if(!WIFEXITED(status))
            return_code |= 0x100;
        if(return_code)
            debug(debug_dos, "child exited with code %04x\n", return_code);
        return return_code > 0xFF;
    }
    else
    {
        // Set program name
        setenv(ENV_PROGNAME, prgname, 1);
        // default drive
        char drv[2] = {0, 0};
        drv[0] = dos_get_default_drive() + 'A';
        setenv(ENV_DEF_DRIVE, drv, 1);
        // and CWD
        setenv(ENV_CWD, (const char *)dos_get_cwd(0), 1);
        // fix filename
        const char *mode = getenv(ENV_FILENAME);
        if(mode &&
           (strcasecmp(mode, "8bit") == 0 || strcasecmp(mode, "dbcs") == 0))
        {
            char m[32];
            strcpy(m, mode);
            strcat(m, "-noconvargs");
            setenv(ENV_FILENAME, m, 1);
        }

        // pass open file descriptors to child process /*FIXME*/
        for(unsigned i = 0; i < 3; i++)
            if(filetable[i].f)
            {
                int f1 = fileno(filetable[i].f);
                int f2 = (f1 < 3) ? dup(f1) : f1;
                if(f2 < 0)
                    f2 = f1;
                dup2(f2, i);
                close(f2);
                if(f1 >= 3 && f1 != f2)
                    close(f1);
            }
        // Accumulate args:
        char *args[64];
        int i;
        const char *exe_path = get_program_exe_path();
        if(!exe_path)
            print_error("can't get emulator path.\n");
        args[0] = prog_name;
        args[1] = file;
        args[2] = cmdline;
        args[3] = "--";
        for(i = 4; i < 63 && *env; i++)
        {
            int l = strlen(env);
            args[i] = env;
            env += l + 1;
        }
        for(; i < 64; i++)
            args[i] = 0;
        if(execv(exe_path, args) == -1)
        {
            raise(SIGABRT);
            exit(1);
        }
    }
    return 0;
}

// DOS exit
NORETURN void intr20(void)
{
    exit(0);
}

// Returns a character read from keyboard - note that control keys return two
// characters, so we need to store the half-processed char here.
static uint16_t inp_last_key;
static void char_input(int brk)
{
    fflush(filetable[1].f ? filetable[1].f : stdout);

    if(inp_last_key == 0)
    {
        if(filetable[0].devinfo != 0x80D3 && filetable[0].f)
            inp_last_key = getc(filetable[0].f);
        else
            inp_last_key = getch(brk);
    }
    debug(debug_dos, "\tgetch = %02x '%c'\n", inp_last_key, (char)inp_last_key);
    dos_error = 0;
    cpuSetAL(inp_last_key);
    if((inp_last_key & 0xFF) == 0)
        inp_last_key = inp_last_key >> 8;
    else
        inp_last_key = 0;
}

// Returns true if a character to read is pending
static int char_pending(void)
{
    return (inp_last_key != 0) || kbhit();
}

static int line_input(FILE *f, uint8_t *buf, int max)
{
    if(video_active())
    {
        static int last_key = 0;
        int len = 0;
        while(len < max)
        {
            int kcode = last_key ? last_key : getch(1);
            char key = kcode & 0xFF;
            last_key = key ? 0 : kcode >> 8;
            if(key == '\r')
            {
                video_putch('\r');
                video_putch('\n');
                buf[len++] = '\r';
                if(len < max)
                    buf[len++] = '\n';
                break;
            }
            else if(key == 8)
            {
                if(len)
                {
                    len--;
                    video_putch(key);
                    video_putch(' ');
                    video_putch(key);
                }
            }
            else if(len < max && video_get_col() < 79)
            {
                video_putch(key);
                buf[len] = key;
                len++;
            }
        }
        return len;
    }
    else
    {
        int i, cr = 0;
        for(i = 0; i < max; i++)
        {
            int c = fgetc(f);
            if(c == EOF && errno == EINTR)
            {
                --i;
                continue;
            }
            if(c < 0)
                break;
            if(c == '\n' && !cr && i < max)
            {
                cr = 1;
                buf[i] = '\r';
                i++;
            }
            else if(c == '\r')
                cr = 1;
            buf[i] = c;
            if(c == '\n')
            {
                i++;
                break;
            }
        }
        return i;
    }
}

#ifdef DEBUG_MCB
static void mcb_debug(void)
{
    uint32_t mcb_addr = (get16(dos_sysvars + 22) << 4) & 0xFFFFF;
    int flg;
    unsigned int owner, size;
    do
    {
        flg = get8(mcb_addr);
        owner = get16(mcb_addr + 1);
        size = get16(mcb_addr + 3);
        debug(debug_dos, "MEM: '%c': OWNER %04x : ADDR %06x - %06x: SIZE %d\n",
              flg, owner, mcb_addr, mcb_addr + size*16 + 16, size*16);
        mcb_addr += size * 16 + 16;
    }
    while (flg != 'Z' && mcb_addr < 0x110000);
}
#endif /* DEBUG_MCB */

static void intr21_debug(void)
{
    static const char *func_names[] = {
        "terminate",            // 00
        "getchar",              // 01
        "putchar",              // 02
        "getc(aux)",            // 03
        "putc(aux)",            // 04
        "putc(prn)",            // 05
        "console i/o",          // 06
        "getch",                // 07
        "getch",                // 08
        "puts",                 // 09
        "gets",                 // 0a
        "eof(stdin)",           // 0b
        "flush(stdin)+",        // 0c
        "disk reset",           // 0d
        "set drive",            // 0e
        "open fcb",             // 0f
        "close fcb",            // 10
        "find first fcb",       // 11
        "find next fcb",        // 12
        "del fcb",              // 13
        "read fcb",             // 14
        "write fcb",            // 15
        "creat fcb",            // 16
        "rename fcb",           // 17
        "n/a",                  // 18
        "get drive",            // 19
        "set DTA",              // 1a
        "stat def drive",       // 1b
        "stat drive",           // 1c
        "n/a",                  // 1d
        "n/a",                  // 1e
        "get def DPB",          // 1f
        "n/a",                  // 20
        "read fcb",             // 21
        "write fcb",            // 22
        "size fcb",             // 23
        "set record fcb",       // 24
        "set int vect",         // 25
        "create PSP",           // 26
        "read blk fcb",         // 27
        "write blk fcb",        // 28
        "parse filename",       // 29
        "get date",             // 2a
        "set date",             // 2b
        "get time",             // 2c
        "set time",             // 2d
        "set verify",           // 2e
        "get DTA",              // 2f
        "version",              // 30
        "go TSR",               // 31
        "get DPB",              // 32
        "g/set brk check",      // 33
        "InDOS addr",           // 34
        "get int vect",         // 35
        "get free",             // 36
        "get/set switch",       // 37
        "country info",         // 38
        "mkdir",                // 39
        "rmdir",                // 3a
        "chdir",                // 3b
        "creat",                // 3c
        "open",                 // 3d
        "close",                // 3e
        "read",                 // 3f
        "write",                // 40
        "unlink",               // 41
        "lseek",                // 42
        "get/set attr",         // 43
        "g/set devinfo",        // 44
        "dup",                  // 45
        "dup2",                 // 46
        "get CWD",              // 47
        "mem alloc",            // 48
        "mem free",             // 49
        "mem resize",           // 4a
        "exec",                 // 4b
        "exit",                 // 4c
        "get errorlevel",       // 4d
        "find first",           // 4e
        "find next",            // 4f
        "set PSP",              // 50
        "get PSP",              // 51
        "get sysvars",          // 52
        "trans BPB to DPB",     // 53
        "get verify",           // 54
        "create PSP",           // 55
        "rename",               // 56
        "g/set file dates",     // 57
        "g/set alloc type",     // 58
        "ext error",            // 59
        "create tmpfile",       // 5a
        "creat new file",       // 5b
        "flock",                // 5c
        "(server fn)",          // 5d
        "(net fn)",             // 5e
        "(net redir)",          // 5f
        "truename",             // 60
        "n/a",                  // 61
        "get PSP",              // 62
        "intl char info",       // 63
        "(internal)",           // 64
        "get ext country info", // 65
        "(g/set global codepage table)", // 66
        "set handle count",     // 67
        "fflush",               // 68
    };
    const char *fn;
    static int count = 0;
    static struct regs
    {
        uint16_t ax, bx, cx, dx, di, ds, es;
    } last = {0, 0, 0, 0, 0, 0, 0};
    struct regs cur = {cpuGetAX(), cpuGetBX(), cpuGetCX(), cpuGetDX(),
                       cpuGetDI(), cpuGetDS(), cpuGetES()};
    // Check if we have a repeated log
    if(!memcmp(&cur, &last, sizeof(struct regs)))
    {
        count++;
        return;
    }
    else if(count)
        debug(debug_dos, "        : (repeated %d times)\n", count + 1);
    // Not repeated, reset count and save register values
    count = 0;
    last = cur;

    if((cur.ax >> 8) < (sizeof(func_names) / sizeof(func_names[0])))
        fn = func_names[cur.ax >> 8];
    else
        fn = "(unknown)";

    debug(debug_dos, "D-21%04X: %-15s BX=%04X CX:%04X DX:%04X DI=%04X DS:%04X ES:%04X\n",
          cur.ax, fn, cur.bx, cur.cx, cur.dx, cur.di, cur.ds, cur.es);
#ifdef DEBUG_MCB
    mcb_debug();
#endif
}

// DOS int 2f
// Static set of functions used for DOS devices/extensions and TSR installation checks
void intr2f(void)
{
    debug(debug_int, "D-2F%04X: BX=%04X\n", cpuGetAX(), cpuGetBX());
    unsigned ax = cpuGetAX();
    switch(ax)
    {
    case 0x1680:
        // Windows "release VM timeslice", use sleep instead of yield to give more
        // cpu to other tasks.
        debug(debug_dos, "W-2F1680: sleep\n");
        usleep(33000);
        break;
    case 0x4300: // XMS exist check
        cpuSetAL(0x80);
        break;
    case 0x4310: // XMS far call address
       {
           cpuSetAL(0x80);
           uint32_t addr = xms_entry_point();
           cpuSetES(addr >> 4);
           cpuSetBX(addr & 0x0F);
       }
        break;
    case 0xB700: // APPEND installation check
        cpuSetAL(0xFF);
        break;
    case 0xB702: // Get Append Version
        cpuSetAX(0xFDFD);
        break;
    case 0xB704: // Get Append Path
        cpuSetES(dos_append >> 4);
        cpuSetDI((dos_append & 0xF) + 24);
        break;
    case 0xB706: // Get Append Function State
        cpuSetBX(get16(dos_append));
        break;
    case 0xB710: // Get Version
        cpuSetDX(0x0303);
        break;
    }
}

#ifdef IA32
//
static uint8_t *copy_envblock(uint32_t addr, int *envlen)
{
    static int bufsize = 0;
    static uint8_t *buf = NULL;
    int len = 0;
    uint32_t idx = addr;
    
    if(envlen)
        *envlen = 0;
    do
    {
        while (len < 0x10000 && get8(idx++))
            len++;
        len++;
    } while (len < 0x10000 && get8(idx));
    len++;
    if(len >= 0x10000)
        return NULL;

    if(!buf)
    {
        int s = (len + 0xf) & ~0xf;
        buf = malloc(len);
        if(!buf)
            return NULL;
        bufsize = s;
    }
    else if(bufsize < len)
    {
        int s = (len + 0xf) & ~0xf;
        buf = realloc(buf, s);
        if(!buf)
            return NULL;
        bufsize = s;
    }
    meml_reads(addr, buf, len);
    if(envlen)
        *envlen = len;
    return buf;
}
#endif

static void incr_indos(void)
{
    put8(indos_flag, get8(indos_flag) + 1);
}

static void decr_indos(void)
{
    uint8_t n = get8(indos_flag);
    if(n)
        put8(indos_flag, n - 1);
}

// DOS int 21
int intr21(void)
{
    int ret = 0;
    // Check CP/M call, INT 21h from address 0x000C0
    if(cpuGetAddress(cpuGetStack(2), cpuGetStack(0)) == 0xC2)
    {
        debug(debug_dos, "CP/M CALL: ");
        int old_ax = cpuGetAX();
        int ip = cpuGetStack(10), cs = cpuGetStack(8), flags = cpuGetStack(4);
        // Fix-up registers
        cpuSetAX((cpuGetCX() << 8) | (old_ax & 0xFF));
        cpuSetSP(cpuGetSP() + 6);
        // Fix-up return address, interchanges segment/ip:
        int stack = cpuGetAddress(cpuGetSS(), cpuGetSP());
        put16(stack, ip);
        put16(stack + 2, cs);
        put16(stack + 4, flags);
        // Call ourselves
        intr21();
        // Restore AH
        cpuSetAX((old_ax & 0xFF00) | (cpuGetAX() & 0xFF));
        return ret;
    }
    debug(debug_int, "D-21%04X: BX=%04X\n", cpuGetAX(), cpuGetBX());
    if(debug_active(debug_dos))
        intr21_debug();

    // Process interrupt
    incr_indos();
    unsigned ax = cpuGetAX(), ah = ax >> 8;

    // Store SS:SP into PSP, used at return from child process
    // According to DOSBOX, only set for certain functions:
    if(ah != 0x50 && ah != 0x51 && ah != 0x62 && ah != 0x64 && ah < 0x6c)
    {
        put16(cpuGetAddress(get_current_PSP(), 0x2E), cpuGetSP());
        put16(cpuGetAddress(get_current_PSP(), 0x30), cpuGetSS());
    }

    switch(ah)
    {
    case 1: // CHARACTER INPUT WITH ECHO
        char_input(1);
        dos_putchar(cpuGetAX() & 0xFF, 1);
        check_screen();
        update_dos_sft(0, NULL);
        break;
    case 2: // PUTCH
        dos_putchar(cpuGetDX() & 0xFF, 1);
        check_screen();
        update_dos_sft(1, NULL);
        cpuSetAX(0x0200 | (cpuGetDX() & 0xFF)); // from intlist.
        break;
    case 0x6: // CONSOLE OUTPUT
        if((cpuGetDX() & 0xFF) == 0xFF)
        {
            if(char_pending())
            {
                char_input(0);
                cpuClrFlag(cpuFlag_ZF);
            }
            else
            {
                cpuSetAL(0);
                cpuSetFlag(cpuFlag_ZF);
            }
        }
        else
        {
            // Wake-up keyboard on character output. This is needed so that
            // VEDIT writes faster to the screen, see issue #71
            keyb_wakeup();
            dos_putchar(cpuGetDX() & 0xFF, 1);
            cpuSetAL(cpuGetDX());
        }
        update_dos_sft(1, NULL);
        break;
    case 0x7: // DIRECT CHARACTER INPUT WITHOUT ECHO
        char_input(0);
        update_dos_sft(0, NULL);
        break;
    case 0x8: // CHARACTER INPUT WITHOUT ECHO
        char_input(1);
        update_dos_sft(0, NULL);
        break;
    case 0x9: // WRITE STRING
        intr21_9();
        check_screen();
        update_dos_sft(1, NULL);
        break;
    case 0xA: // BUFFERED INPUT
    {
        unsigned addr = cpuGetAddrDS(cpuGetDX());
        unsigned len = get8(addr);
        if(!len)
        {
            debug(debug_dos, "\tbuffered input len = 0\n");
            break;
        }
        if(addr + len + 2 >= 0x100000)
        {
            debug(debug_dos, "\tbuffer pointer invalid\n");
            break;
        }

        // If we are reading from console, suspend keyboard handling and update
        // emulator state.
        if(filetable[0].devinfo == 0x80D3)
        {
            suspend_keyboard();
            emulator_update();
        }

        FILE *f = filetable[0].f ? filetable[0].f : stdin;
        unsigned i;
        for(i = 0; i < len;)
        {
            int c;
            if(filetable[0].devinfo == 0x80D3)
            {
                char_input(1);
                c = cpuGetAX() & 0xFF;
                if(c == 0x08 || c == 0x7f) // backspace or delete
                {
                    if(i > 2)
                    {
                        dos_putchar(0x08, 1);
                        dos_putchar(' ', 1);
                        dos_putchar(0x08, 1);
                        i--;
                    }
                    continue;
                }
                dos_putchar(c, 1);
            }
            else
            {
                c = getc(f);
            }
            // Retry if we were interrupted
            if(c == EOF && errno == EINTR)
                continue;
            if(c == '\n' || c == EOF)
                c = '\r';
            put8(addr + i + 2, c);
            if(c == '\r')
                break;
            i++;
        }
        put8(addr + 1, i);
        update_dos_sft(1, NULL);
        break;
    }
    case 0xB: // STDIN STATUS
        if(filetable[0].devinfo == 0x80D3)
            cpuSetAX(char_pending() ? 0x0BFF : 0x0B00);
        else
            cpuSetAX(0x0B00);
        break;
    case 0xC: // FLUSH AND READ STDIN
        tcflush(STDIN_FILENO, TCIFLUSH);
        fflush(stdin);
        switch(ax & 0xFF)
        {
        case 0x01:
        case 0x06:
        case 0x07:
        case 0x08:
        case 0x0A:
            cpuSetAX(ax << 8);
            intr21();
            decr_indos();
            return ret;
        }
        break;
    case 0x0E: // SELECT DEFAULT DRIVE
        dos_set_default_drive(cpuGetDX() & 0xFF);
        // Number of drives = 3, 'A:', 'B:' and 'C:'
        cpuSetAX(0x0E03);
        break;
    case 0x0F: // OPEN FILE USING FCB
        dos_open_file_fcb(0);
        break;
    case 0x10: // CLOSE FILE USING FCB
    {
        int handle;
        dos_show_fcb();
        // Set full AX because dos_close_file clobbers AX
        get_fcb_handle(&handle, NULL);
        cpuSetAX(dos_close_file(handle) ? 0x10FF : 0x1000);
        break;
    }
    case 0x11: // FIND FIRST FILE USING FCB
        dos_find_first_fcb();
        break;
    case 0x12: // FIND NEXT FILE USING FCB
        dos_find_next_fcb();
        break;
    case 0x13: // DELETE FILE USING FCB
    {
        dos_show_fcb();
        /* TODO: Limited support. No wild cards */
        int fcb_addr = get_fcb();
        char *fname = dos_unix_path_fcb(fcb_addr, 0, append_path());
        if(!fname)
        {
            debug(debug_dos, "\t(file not found)\n");
            dos_error = 2;
            cpuSetAL(0xFF);
            break;
        }
        debug(debug_dos, "\tdelete fcb '%s'\n", fname);
        int e = unlink(fname);
        free(fname);
        if(e)
        {
            debug(debug_dos, "\tcould not delete file (%d).\n", errno);
            dos_error = 5;
            cpuSetAL(0xFF);
        }
        else
            cpuSetAL(0x00);
        break;
    }
    case 0x14: // SEQUENTIAL READ USING FCB
        dos_show_fcb();
        cpuSetAL(dos_rw_record_fcb(dosDTA, 0, 1, 1));
        break;
    case 0x15: // SEQUENTIAL WRITE USING FCB
        dos_show_fcb();
        cpuSetAL(dos_rw_record_fcb(dosDTA, 1, 1, 1));
        break;
    case 0x16: // CREATE FILE USING FCB
        dos_open_file_fcb(1);
        break;
    case 0x17: // RENAME FILE USING FCB
    {
        int fcb_addr = get_fcb();
        char *fname1 = dos_unix_path_fcb(fcb_addr, 0, append_path());
        if(!fname1)
        {
            debug(debug_dos, "\t(file not found)\n");
            dos_error = 2;
            cpuSetAL(0xFF);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        // Backup old name, and copy new name to standard location
        uint8_t buf[11];
#ifdef IA32
        uint8_t buf2[11];
        meml_reads(fcb_addr + 1, buf, 11);
        meml_reads(fcb_addr + 0x11, buf2, 11);
        meml_writes(fcb_addr + 1, buf2, 11);
        char *fname2 = dos_unix_path_fcb(fcb_addr, 1, append_path());
        // Restore name
        meml_writes(fcb_addr + 1, buf, 11);
#else
        memcpy(buf, memory + fcb_addr + 1, 11);
        memcpy(memory + fcb_addr + 1, memory + fcb_addr + 0x11, 11);
        char *fname2 = dos_unix_path_fcb(fcb_addr, 1, append_path());
        // Restore name
        memcpy(memory + fcb_addr + 1, buf, 11);
#endif
        if(!fname2)
        {
            free(fname1);
            debug(debug_dos, "\t(destination invalid)\n");
            cpuSetAL(0xFF);
            dos_error = 3;
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        int e = rename(fname1, fname2);
        free(fname2);
        free(fname1);
        if(e)
        {
            dos_error = 5;
            cpuSetAL(0xFF);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        dos_error = 0;
        cpuSetAL(0);
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x19: // GET DEFAULT DRIVE
        debug(debug_dos, "\tget default drive = '%c'\n", dos_get_default_drive() + 'A');
        cpuSetAL(dos_get_default_drive());
        break;
    case 0x1A: // SET DTA
        dosDTA = 0xFFFFF & (cpuGetDS() * 16 + cpuGetDX());
        put16(indos_flag + 0x0b, cpuGetDX());
        put16(indos_flag + 0x0d, cpuGetDS());
        break;
    case 0x1B: // GET DEFAULT DRIVE INFO
        dos_get_drive_info(0);
        break;
    case 0x1C: // GET DRIVE INFO
        dos_get_drive_info(cpuGetDX() & 0xFF);
        break;
    case 0x21: // RANDOM READ USING FCB
        dos_show_fcb();
        cpuSetAL(dos_rw_record_fcb(dosDTA, 0, 0, 0));
        break;
    case 0x22: // RANDOM WRITE USING FCB
        dos_show_fcb();
        cpuSetAL(dos_rw_record_fcb(dosDTA, 1, 0, 0));
        break;
    case 0x24: // SET RANDOM RECORD NUMBER IN FCB
        dos_show_fcb();
        dos_seq_to_rand_fcb(get_fcb());
        break;
    case 0x25: // set interrupt vector
        put16(4 * (ax & 0xFF), cpuGetDX());
        put16(4 * (ax & 0xFF) + 2, cpuGetDS());
        // If the application installed a keyboard interrupt handler, we should
        // enable keyboard emulation to actually generate the interrupts.
        if(9 == (ax & 0xFF))
            kbhit();
        break;
    case 0x27: // BLOCK READ FROM FCB
    case 0x28: // BLOCK WRITE TO FCB
    {
        dos_show_fcb();
        int fcb = get_fcb();
        unsigned count = cpuGetCX();
        unsigned rsize = get16(0x0E + fcb);
        unsigned e = 0;
        unsigned target = dosDTA;

        while(!e && count)
        {
            if(0x27 == (ax >> 8))
                e = dos_rw_record_fcb(target, 0, 1, 0);
            else
                e = dos_rw_record_fcb(target, 1, 1, 0);

            if(e == 0 || e == 3)
            {
                target += rsize;
                count--;
            }
        }
        cpuSetCX(cpuGetCX() - count);
        cpuSetAL(e);
        dos_show_fcb();
        break;
    }
    case 0x29: // PARSE FILENAME TO FCB
    {
        // TODO: length could be more than 64 bytes!
        char *fname = getstr(cpuGetAddrDS(cpuGetSI()), 64);
        char *orig = fname;
        uint32_t dst = cpuGetAddrES(cpuGetDI());
        if(check_limit(37, dst))
        {
            debug(debug_dos, "\tinvalid destination\n");
            cpuSetAL(0xFF);
            break;
        }
        debug(debug_dos, "\t'%s' -> ", fname);
        if(ax & 1)
            // Skip separator
            if(*fname && strchr(":;,=+", *fname))
                fname++;
        // Skip initial spaces
        while(*fname && strchr(" \t", *fname))
            fname++;
        // Check drive:
        int ret = 0;
        if(!(ax & 2))
            put8(dst, 0);
        if(*fname && fname[1] == ':')
        {
            char d = *fname;
            if(d >= 'A' && d <= 'Z')
                put8(dst, d - 'A' + 1);
            else if(d >= 'a' && d <= 'z')
                put8(dst, d - 'a' + 1);
            else
                ret = 0xFF;
            fname += 2;
        }
        int i = 1;
        while(i < 12)
        {
            char c = *fname;
            if(c == '.' && i <= 9)
            {
                if(!(ax & 4) || i > 1)
                    for(; i < 9; i++)
                        put8(dst + i, ' ');
                else
                    i = 9;
                fname++;
            }
            else if(!c || strchr(":.;,=+ \t/\"[]<>|\x0D\x10", c))
            {
                if(!(ax & 4) || i > 1)
                    for(; i < 9; i++)
                        put8(dst + i, ' ');
                if(i < 9)
                    i = 9;
                if(!(ax & 8) || i > 9)
                    for(; i < 12; i++)
                        put8(dst + i, ' ');
                break;
            }
            else if(c == '*' && i < 9)
            {
                for(; i < 9; i++)
                    put8(dst + i, '?');
                fname++;
                ret = 1;
            }
            else if(c == '*')
            {
                for(; i < 12; i++)
                    put8(dst + i, '?');
                fname++;
                ret = 1;
                break;
            }
            else
            {
                if(c >= 'a' && c <= 'z')
                    put8(dst + i, c - 'a' + 'A');
                else
                    put8(dst + i, c);
                i++;
                fname++;
            }
        }
        // Update resulting pointer
        int si = cpuGetSI() + (fname - orig);
        while(si > 0xFFFF)
        {
            si = si - 0x10;
            cpuSetDS(cpuGetDS() + 1);
        }
        cpuSetSI((si));
        cpuSetAL(ret);
        debug(debug_dos, "%c:'%.11s'\n", get8(dst) ? get8(dst) + '@' : '*', getstr(dst + 1, 36));
        break;
    }
    case 0x2A: // GET SYSTEM DATE
    {
        time_t tm = time(0);
        struct tm lt;
        if(localtime_r(&tm, &lt))
        {
            cpuSetAL(lt.tm_wday);
            cpuSetCX(lt.tm_year + 1900);
            cpuSetDX(((lt.tm_mon + 1) << 8) | (lt.tm_mday));
        }
        break;
    }
    case 0x2B: // SET SYSTEM DATE
        // Invalid date - don't support setting date.
        cpuSetAL(0xFF);
        break;
    case 0x2C: // GET SYSTEM TIME
    {
        // Use BIOS time: 1573040 ticks per day
        uint32_t bios_timer = get_bios_timer() * 1080;
        uint32_t bsec = bios_timer / 19663;
        uint32_t bsub = bios_timer % 19663;

        uint8_t thor = bsec / 3600;
        uint8_t tmin = (bsec / 60) % 60;
        uint8_t tsec = bsec % 60;
        uint8_t msec = bsub * 100 / 19663;

        cpuSetCX((thor << 8) | tmin);
        cpuSetDX((tsec << 8) | msec);
        break;
    }
    case 0x2D: // SET SYSTEM TIME
        // Invalid time - don't support setting time.
        cpuSetAL(0xFF);
        break;
    case 0x2F: // GET DTA
        cpuSetES((dosDTA & 0xFFF00) >> 4);
        cpuSetBX((dosDTA & 0x000FF));
        break;
    case 0x30: // DOS version: 3.30
        cpuSetAX(dosver);
        cpuSetBX(0x0001);
        break;
    case 0x32: // get Drive Parameter Block
        cpuSetAL(0xFF); // return as Network drive
        break;
    case 0x33: // BREAK SETTINGS
        if(ax == 0x3300)
            cpuSetDX((cpuGetDX() & 0xFF00) | 1);
        else if(ax == 0x3301)
            cpuSetDX((cpuGetDX() & 0xFF00) | 1); // Ignore new state
        break;
    case 0x34: // get Indos flag address
        cpuSetES(indos_flag >> 4);
        cpuSetBX(indos_flag & 0x000F);
        break;
    case 0x35: // get interrupt vector
        cpuSetBX(get16(4 * (ax & 0xFF)));
        cpuSetES(get16(4 * (ax & 0xFF) + 2));
        break;
    case 0x36: // get free space
        // We only return 512MB free, as some old DOS programs crash if
        // the free space returned is more than 0x7FFF clusters.
        cpuSetAX(32);     // 16k clusters
        cpuSetBX(0x7FFF); // half of disk free, 512MB
        cpuSetCX(512);    // 512 bytes/sector
        cpuSetDX(0xFFFF); // total 1GB
        break;
    case 0x37: // get/set switch character
        cpuSetDX('/');
        break;
    case 0x38: // GET COUNTRY INFO
    {
        // Size of NLS information is 34, but HX DOS Extender treat it is 32
#ifdef IA32
        uint32_t addr = cpuGetAddrDS(cpuGetDX());
        for (int i = 0; i < 32 /*34*/; i++)
            put8(addr + i , nls_country_info[i]);
#else
        putmem(cpuGetAddrDS(cpuGetDX()), nls_country_info, 32 /*34*/);
#endif
        break;
    }
    case 0x39: // MKDIR
        create_dir();
        break;
    case 0x3A: // RMDIR
        remove_dir();
        break;
    case 0x3B: // CHDIR
    {
        if(dos_change_dir(cpuGetAddrDS(cpuGetDX())))
        {
            dos_error = 3;
            cpuSetAX(3);
            cpuSetFlag(cpuFlag_CF);
        }
        else
        {
            dos_error = 0;
            cpuClrFlag(cpuFlag_CF);
        }
        break;
    }
    case 0x3C: // CREATE FILE
        dos_open_file(1, cpuGetAX() & 0xFF, cpuGetAddrDS(cpuGetDX()));
        break;
    case 0x3D: // OPEN EXISTING FILE
        dos_open_file(0, cpuGetAX() & 0xFF, cpuGetAddrDS(cpuGetDX()));
        break;
    case 0x3E: // CLOSE FILE
        dos_close_file(cpuGetBX());
        break;
    case 0x3F: // READ
    {
        int sidx = handle_to_sidx(cpuGetBX());
        FILE *f = (sidx < 0) ? NULL : filetable[sidx].f;
        if(!f)
        {
            cpuSetFlag(cpuFlag_CF);
            dos_error = 6; // invalid handle
            cpuSetAX(dos_error);
            break;
        }
        int len = cpuGetCX();
        uint32_t addr = cpuGetAddrDS(cpuGetDX());
#if defined(EMS_SUPPORT) || defined(IA32)
        int buf_allocated = 0;
        int tgt_ems = 0;
#endif
        uint8_t *buf = NULL;
#ifdef EMS_SUPPORT
        if (in_ems_pageframe2(addr, len))
        {
            buf_allocated = 1;
            tgt_ems = 1;
            buf = malloc(len);
        }
        else
#endif //EMS_SUPPORT
#ifdef IA32
        {
            buf_allocated = 1;
            buf = malloc(len);
        }
#else //not IA32
        buf = getptr(addr, len);
#endif //IA32
        if(!buf)
        {
            debug(debug_dos, "\tbuffer pointer invalid\n");
            dos_error = 5; // access denied
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            update_dos_sft(sidx, NULL);
            break;
        }
        // If read from "CON", reads up to the first "CR":
        if(filetable[sidx].devinfo == 0x80D3)
        {
            suspend_keyboard();
            cpuSetAX(line_input(f, buf, len));
        }
        else
        {
            unsigned n = fread(buf, 1, len, f);
            cpuSetAX(n);
        }
        dos_error = 0;
        cpuClrFlag(cpuFlag_CF);
#if defined(EMS_SUPPORT) || defined(IA32)
        if (buf_allocated) {
#ifdef EMS_SUPPORT            
            if(tgt_ems)
                ems_putmem(addr, buf, len);
            else
#endif
#ifdef IA32
            meml_writes(addr, buf, len);
#else
            /*NOP*/;
#endif
            free(buf);
            update_dos_sft(sidx, NULL);
        }
#endif
        break;
    }
    case 0x40: // WRITE
    {
        int sidx = handle_to_sidx(cpuGetBX());
        FILE *f = (sidx < 0) ? NULL : filetable[sidx].f;
        if(!f)
        {
            cpuSetFlag(cpuFlag_CF);
            dos_error = 6; // invalid handle
            cpuSetAX(dos_error);
            decr_indos();
            return ret;
        }
        unsigned len = cpuGetCX();
        // If len=0, file is truncated at current position:
        if(!len)
        {
            cpuClrFlag(cpuFlag_CF);
            dos_error = 0;
            cpuSetAX(0);
            // flush output
            int e = fflush(f);
            if(e)
            {
                cpuSetFlag(cpuFlag_CF);
                dos_error = 5; // access denied
                cpuSetAX(dos_error);
            }
            else if(filetable[sidx].devinfo != 0x80D3)
            {
                off_t pos = ftello(f);
                if(pos != -1 && -1 == ftruncate(fileno(f), pos))
                {
                    cpuSetFlag(cpuFlag_CF);
                    dos_error = 5; // access denied
                    cpuSetAX(dos_error);
                }
                update_dos_sft(sidx, NULL);
            }
            break;
        }
        uint32_t addr = cpuGetAddrDS(cpuGetDX());
#if defined(EMS_SUPPORT) || defined(IA32)
        int buf_allocated = 0;
#endif
        uint8_t *buf = NULL;
#ifdef EMS_SUPPORT
        if (in_ems_pageframe2(addr, len))
        {
            buf_allocated = 1;
            buf = malloc(len);
            if (buf)
                ems_getmem(buf, addr, len);
        }
        else
#endif //EMS_SUPPORT
#ifdef IA32
        {
            buf_allocated = 1;
            buf = malloc(len);
            if (buf)
                meml_reads(addr, buf, len);
        }
#else //not IA32
        buf = getptr(addr, len);
#endif //IA32
        if(!buf)
        {
            debug(debug_dos, "\tbuffer pointer invalid\n");
            dos_error = 5; // access denied
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        if(filetable[sidx].devinfo == 0x80D3)
        {
            for(unsigned i = 0; i < len; i++)
                dos_putchar(buf[i], sidx);
            check_screen();
            cpuSetAX(len);
        }
        else
        {
            unsigned n = fwrite(buf, 1, len, f);
            cpuSetAX(n);
        }
        dos_error = 0;
        cpuClrFlag(cpuFlag_CF);
#if defined(EMS_SUPPORT) || defined(IA32)
        if (buf_allocated)
            free(buf);
#endif
        update_dos_sft(sidx, NULL);
        break;
    }
    case 0x41: // UNLINK
    {
        char *fname = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 0, append_path());
        if(!fname)
        {
            debug(debug_dos, "\t(file not found)\n");
            cpuSetFlag(cpuFlag_CF);
            dos_error = 2;
            cpuSetAX(dos_error);
            break;
        }
        debug(debug_dos, "\tunlink '%s'\n", fname);
        int e = unlink(fname);
        free(fname);
        if(e)
        {
            if(errno == ENOTDIR)
                dos_error = 3;
            else if(errno == ENOENT)
                dos_error = 2;
            else
                dos_error = 5;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
        }
        else
        {
            dos_error = 0;
            cpuClrFlag(cpuFlag_CF);
        }
        break;
    }
    case 0x42: // LSEEK
    {
        int sidx = handle_to_sidx(cpuGetBX());
        FILE *f = (sidx < 0) ? NULL : filetable[sidx].f;
        long pos = cpuGetDX();
        if(cpuGetCX() >= 0x8000)
            pos = pos + (((long)cpuGetCX() - 0x10000) << 16);
        else
            pos = pos + (cpuGetCX() << 16);

        debug(debug_dos, "\t%d: %p\n", sidx, f);
        debug(debug_dos, "\tlseek-%02x pos = %ld\n", ax & 0xFF, pos);
        if(!f)
        {
            cpuSetFlag(cpuFlag_CF);
            dos_error = 6; // invalid handle
            cpuSetAX(dos_error);
            break;
        }
        switch(ax & 0xFF)
        {
        case 0: fseek(f, pos, SEEK_SET); break;
        case 1: fseek(f, pos, SEEK_CUR); break;
        case 2: fseek(f, pos, SEEK_END); break;
        default:
            cpuSetFlag(cpuFlag_CF);
            dos_error = 1;
            cpuSetAX(dos_error);
            decr_indos();
            return ret;
        }
        pos = ftell(f);
        cpuSetAX(pos & 0xFFFF);
        cpuSetDX((pos >> 16) & 0xFFFF);
        dos_error = 0;
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x43: // DOS 2+ file attributes
        intr21_43();
        break;
    case 0x44:
    {
        int h = cpuGetBX();
        int sidx = handle_to_sidx(h);
        int al = ax & 0xFF;
        if((al < 4 || al == 6 || al == 7 || al == 10 || al == 12 || al == 16) &&
           (sidx < 0 || !filetable[sidx].f))
        {
            // Show error if it is a file handle.
            debug(debug_dos, "\t(invalid file handle)\n");
            cpuSetFlag(cpuFlag_CF);
            dos_error = 6; // invalid handle
            cpuSetAX(dos_error);
            break;
        }
        cpuClrFlag(cpuFlag_CF);
        dos_error = 0;
        switch(al)
        {
        case 0x00: // GET DEV INFO
            debug(debug_dos, "\t= %04x\n", filetable[sidx].devinfo);
            cpuSetDX(filetable[sidx].devinfo);
            // Undocumented, needed for VEDIT INSTALL
            cpuSetAX(filetable[sidx].devinfo);
            break;
        case 0x01: // SET DEV INFO
        case 0x02: // IOCTL CHAR DEV READ
        case 0x03: // IOCTL CHAR DEV WRITE
        case 0x04: // IOCTL BLOCK DEV READ
        case 0x05: // IOCTL BLOCK DEV |WRITE
            dos_error = 5;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            break;
        case 0x06: // GET INPUT STATUS
            if(filetable[sidx].devinfo == 0x80D3)
                cpuSetAX(char_pending() ? 0x44FF : 0x4400);
            else
                cpuSetAX(feof(filetable[sidx].f) ? 0x4400 : 0x44FF);
            break;
        case 0x07: // GET OUTPUT STATUS
            cpuSetAX(0x44FF);
            break;
        case 0x08: // CHECK DRIVE REMOVABLE
            h = h & 0xFF;
            h = h ? h - 1 : dos_get_default_drive();
            if(h < 2)
                cpuSetAX(0x0000);
            else
                cpuSetAX(0x0001);
            break;
        case 0x09: // CHECK DRIVE REMOTE
            cpuSetDX(0x0100);
            break;
        case 0x0A: // CHECK HANDLE REMOTE
            cpuSetDX(0);
            break;
        case 0x0B: // SET SHARING RETRY COUNT
        case 0x0C: // GENERIC CHAR DEV REQUEST
        case 0x0D: // GENERIC BLOCK DEV REQUEST
        case 0x0F: // SET LOGICAL DRIVE MAP
        case 0x10: // QUERY CHAR DEV CAPABILITY
        case 0x11: // QUERY BLOCK DEV CAPABILITY
            dos_error = 1;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            break;
        case 0x0E: // GET LOGICAL DRIVE MAP
            cpuSetAX(0x4400);
        }
        break;
    }
    case 0x45: // DUP
    {
        int sidx = handle_to_sidx(cpuGetBX());
        if(sidx < 0 || !filetable[sidx].f)
        {
            // Show error if it is a file handle.
            debug(debug_dos, "\t(invalid file handle)\n");
            cpuSetFlag(cpuFlag_CF);
            dos_error = 6; // invalid handle
            cpuSetAX(dos_error);
            break;
        }
        int h;
        get_new_handle(&h, NULL);
        if(h < 0)
        {
            dos_error = 4;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        debug(debug_dos, "\t%04x -> %04x\n", cpuGetBX(), (unsigned)h);
        set_handle(h, sidx);
        cpuSetAX(h);
        dos_error = 0;
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x46: // DUP2
    {
        int sidx = handle_to_sidx(cpuGetBX());
        if(sidx < 0 || !filetable[sidx].f)
        {
            // Show error if it is a file handle.
            debug(debug_dos, "\t(invalid file handle)\n");
            dos_error = 6; // invalid handle
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        set_handle(cpuGetCX(), sidx);
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x47: // GET CWD
    {
        // Note: ignore drive letter in DL
        const uint8_t *path = dos_get_cwd(cpuGetDX() & 0xFF);
        debug(debug_dos, "\tcwd '%c' = '%s'\n", '@' + (int)(cpuGetDX() & 0xFF), path);
        putmem(cpuGetAddrDS(cpuGetSI()), path, 64);
        cpuSetAX(0x0100);
        dos_error = 0;
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x48: // ALLOC MEMORY BLOCK
    {
        uint16_t seg, max = 0;
        seg = mem_alloc_segment(cpuGetBX(), &max);
        if(seg)
        {
            debug(debug_dos, "\tallocated at %04x.\n", seg);
            dos_error = 0;
            cpuSetAX(seg);
            cpuClrFlag(cpuFlag_CF);
        }
        else
        {
            debug(debug_dos, "\tnot enough memory, max=$%04x paragraphs\n", max);
            dos_error = 8;
            cpuSetAX(dos_error);
            cpuSetBX(max);
            cpuSetFlag(cpuFlag_CF);
        }
        break;
    }
    case 0x49: // FREE MEMORY BLOCK
    {
        mem_free_segment(cpuGetES());
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x4A: // RESIZE MEMORY BLOCK
    {
        uint16_t sz = mem_resize_segment(cpuGetES(), cpuGetBX());
        if(sz == cpuGetBX())
        {
            cpuClrFlag(cpuFlag_CF);
            // See bug #115
            cpuSetAX(cpuGetES());
        }
        else
        {
            dos_error = 8;
            cpuSetAX(dos_error);
            cpuSetBX(sz);
            cpuSetFlag(cpuFlag_CF);
            debug(debug_dos, "\tmax memory available: $%04x\n", sz);
        }
        break;
    }
    case 0x4B: // EXEC
    {
        char *fname = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 0, 0);
        if(!fname)
        {
            debug(debug_dos, "\texec error, file not found\n");
            dos_error = 2;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        // Flags:   0 = Load and Go, 1 = LOAD, 3 = Overlay
        //        128 = LoadHi
        if((ax & 0xFF) == 3)
        {
            debug(debug_dos, "\tload overlay '%s'\n", fname);
            int pb = cpuGetAddrES(cpuGetBX());
            uint16_t load_seg = get16(pb);
            uint16_t reloc_seg = get16(pb + 2);
            FILE *f = fopen(fname, "rb");
            if(!f || dos_read_overlay(f, load_seg, reloc_seg))
            {
                debug(debug_dos, "\tERROR\n");
                dos_error = 11;
                cpuSetAX(dos_error);
                cpuSetFlag(cpuFlag_CF);
            }
            else
            {
                dos_error = 0;
                cpuClrFlag(cpuFlag_CF);
            }
        }
        else if(!getenv(ENV_EXEC_SAME) && (ax & 0xFF) == 0)
        {
            debug(debug_dos, "\texec: '%s'\n", fname);
            // Get executable file name:
            char *prgname = getstr(cpuGetAddrDS(cpuGetDX()), 64);
            // Read command line parameters:
            int pb = cpuGetAddrES(cpuGetBX());
            int cmd_addr = cpuGetAddress(get16(pb + 4), get16(pb + 2));
            int clen = get8(cmd_addr);
            char *cmdline = getstr(cmd_addr + 1, clen);
            debug(debug_dos, "\texec command line: '%s %.*s'\n", prgname, clen, cmdline);
            char *env = "\0\0";
            uint16_t env_seg = get16(pb);
            if(!env_seg)
                env_seg = get16(cpuGetAddress(get_current_PSP(), 0x2C));
            if(env_seg != 0)
            {
#ifdef IA32
                env = (char *)copy_envblock(cpuGetAddress(env_seg, 0), NULL);
#else
                // Sanitize env
                int eaddr = cpuGetAddress(env_seg, 0);
                while(get8(eaddr) != 0 && eaddr < 0xFFFFF)
                {
                    while(get8(eaddr) != 0 && eaddr < 0xFFFFF)
                        eaddr++;
                    eaddr++;
                }
                if(eaddr < 0xFFFFF)
                    env = (char *)(memory + cpuGetAddress(env_seg, 0));
#endif
            }
            if(run_emulator(fname, prgname, cmdline, env))
            {
                dos_error = 5; // access denied
                cpuSetAX(dos_error);
                cpuSetFlag(cpuFlag_CF);
            }
            else
            {
                dos_error = 0;
                cpuClrFlag(cpuFlag_CF);
            }
        }
        else if((ax & 0xFF) == 0 || (ax & 0xFF) == 1)
        {
            debug(debug_dos, "load: '%s'\n", fname);
            int cur_psp = get_current_PSP();

            // Get executable file name:
            char *prgname = getstr(cpuGetAddrDS(cpuGetDX()), 64);
            // Read command line parameters:
            int pb = cpuGetAddrES(cpuGetBX());
            int cmd_addr = cpuGetAddress(get16(pb + 4), get16(pb + 2));
            int clen = get8(cmd_addr);
            char *cmdline = getstr(cmd_addr + 1, clen);
            debug(debug_dos, "\tload command line: '%s %.*s'\n", prgname, clen, cmdline);

            uint16_t env_seg = get16(pb);
            if(!env_seg)
                env_seg = get16(cpuGetAddress(get_current_PSP(), 0x2C));
            int eaddr = cpuGetAddress(env_seg, 0);
            int elen = 2;
            char *env = "\0\0";
            
#ifdef IA32
            env = (char *)copy_envblock(eaddr, &elen);
#else
            // Sanitize env
            while(get8(eaddr) != 0 && eaddr < 0xFFFFF)
            {
                while(get8(eaddr) != 0 && eaddr < 0xFFFFF)
                    eaddr++;
                eaddr++;
            }
            if(eaddr < 0xFFFFF)
            {
                elen = eaddr - cpuGetAddress(env_seg, 0) + 1;
                env = (char *)(memory + cpuGetAddress(env_seg, 0));
            }
#endif

            int psp_mcb = create_PSP(cmdline, env, elen, prgname);
            if(psp_mcb == 0)
            {
                cpuSetAX(8); // insufficient memory
                cpuSetFlag(cpuFlag_CF);
                break;
            }

            // save registers
            unsigned saveCX = cpuGetCX();
            unsigned saveSI = cpuGetSI();
            unsigned saveDI = cpuGetDI();
            unsigned saveBP = cpuGetBP();
            unsigned saveSP = cpuGetSP();
            unsigned saveDS = cpuGetDS();
            unsigned saveES = cpuGetES();
            unsigned saveSS = cpuGetSS();
            unsigned saveIP = get16(cpuGetAddress(saveSS, saveSP));
            unsigned saveCS = get16(cpuGetAddress(saveSS, saveSP+2));
            
            // Load program
            FILE *f = fopen(fname, "rb");
            if(!f)
                print_error("can't open '%s': %s\n", fname, strerror(errno));
            if(!dos_load_exe(f, psp_mcb))
                print_error("error loading EXE/COM file.\n");
            fclose(f);

            // copy jft
            copy_jft(psp_mcb+1,
                     cpuGetAddress(get16(cur_psp*16 + 0x36),
                                   get16(cur_psp*16 + 0x34)),
                     get8(cur_psp*16 + 0x32) & 0xff);

            // Set parent PSP to the current one
            put16(cpuGetAddress(psp_mcb+1, 22), cur_psp);
            set_current_PSP(psp_mcb+1);

            if((ax & 0xFF) == 0)  // Load and Exec
            {
                // Init DTA
                dosDTA = get_current_PSP() * 16 + 0x80;
                put16(indos_flag + 0x0b, 0x80);
                put16(indos_flag + 0x0d, get_current_PSP());

                // Init DOS flags
                cpuSetStartupFlag(cpuFlag_IF);
                cpuClrStartupFlag(cpuFlag_DF);
                cpuClrStartupFlag(cpuFlag_TF);
#ifdef IA32
                cpuClrStartupFlag(0xffff);
                cpuClrStartupFlag(0xf202);
#endif

                // save return address to Int22 vector
                debug(debug_dos, "\texec RETURN ADDR %04X:%04X\n",
                      saveCS, saveIP);
                put16(0x22 * 4, saveIP);
                put16(0x22 * 4 + 2, saveCS);

                ret = 1; // return by jump
                
                struct exec_PSP *ep = malloc(sizeof(struct exec_PSP));
                debug(debug_dos, "\tpush exec_PSP count\n");
                ep->next = exec_psp_root;
                ep->psp = get_current_PSP();
                ep->parent = cur_psp;
                ep->parent_ax = ax;
                ep->parent_cx = saveCX;
                ep->parent_si = saveSI;
                ep->parent_di = saveDI;
                ep->parent_bp = saveBP;
                ep->parent_ds = saveDS;
                ep->parent_es = saveES;
                exec_psp_root = ep;

                restore_handles();
            }
            else                  // Load only
            {
                // Push AX
                put16(cpuGetAddress(cpuGetSS(), cpuGetSP()-2), cpuGetAX());

                // Save SS:SP & CS:IP
                put16(pb+14, cpuGetSP()-2);
                put16(pb+16, cpuGetSS());
                put16(pb+18, cpuGetIP());
                put16(pb+20, cpuGetCS());

                // Restore regs
                cpuSetCX(saveCX);
                cpuSetSI(saveSI);
                cpuSetDI(saveDI);
                cpuSetBP(saveBP);
                cpuSetSP(saveSP);
                cpuSetIP(saveIP);
                cpuSetCS(saveCS);
                cpuSetDS(saveDS);
                cpuSetES(saveES);
                cpuSetSS(saveSS);

                cpuClrFlag(cpuFlag_CF);
                cpuSetAX(0);
            }
        }
        else
        {
            debug(debug_dos, "\texec '%s': type %02xh not supported.\n", fname,
                  ax & 0xFF);
            dos_error = 1;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
        }
        free(fname);
        break;
    }
    case 0: // TERMINATE PROGRAM
    case 0x31: // Terminate and Stay Resident
    case 0x4C: // EXIT
    {
        uint16_t parent_psp = get16(cpuGetAddress(get_current_PSP(), 22));

        // Detect if our PSP is last one
        debug(debug_dos, "\texit PSP:'%04x', PARENT:%04x.\n",
              get_current_PSP(), parent_psp);
        if ((ax & 0xff00) == 0x0000)
            ax = 0x4c00;
        if(0xFFFE == parent_psp)
            exit(ax & 0xFF);
        else
        {
            // Exit to parent
            return_code = cpuGetAX() & 0xFF;

            uint16_t returnCS = get16(0x22 * 4 + 2);
            uint16_t returnIP = get16(0x22 * 4);
            debug(debug_dos, "\texit RETURN ADDR %04X:%04X\n",
                  returnCS, returnIP);
            
            // Patch INT 22h, 23h and 24h addresses to the ones saved in new PSP
            put16(0x88, get16(cpuGetAddress(get_current_PSP(), 10)));
            put16(0x8A, get16(cpuGetAddress(get_current_PSP(), 12)));
            put16(0x8C, get16(cpuGetAddress(get_current_PSP(), 14)));
            put16(0x8E, get16(cpuGetAddress(get_current_PSP(), 16)));
            put16(0x90, get16(cpuGetAddress(get_current_PSP(), 18)));
            put16(0x92, get16(cpuGetAddress(get_current_PSP(), 20)));
            if((ax & 0xff00) == 0x3100) // TSR
            {
                int resize = cpuGetDX() & 0xffff;
                if(resize < 0x06)
                    resize = 0x06;
                mem_resize_segment(get_current_PSP(), resize);
                return_code |= 0x300;
            }
            else
            {
                // Deallocate child memory
                mem_free_owned(get_current_PSP());
                // Close all files
                dos_close_allfile_owned(get_current_PSP());
            }

            // Set PSP to parent
            if(!exec_psp_root && parent_psp == get_current_PSP())
            {
                debug(debug_dos, "\texec_PSP is empty\n");
                exit(ax & 0xFF);
            }
            if(exec_psp_root && exec_psp_root->psp == get_current_PSP())
            {
                struct exec_PSP *ep = exec_psp_root;
                debug(debug_dos, "\tpop exec_PSP count\n");
                exec_psp_root = ep->next;
                parent_psp = ep->parent;
                cpuSetAX(ep->parent_ax);
                cpuSetCX(ep->parent_cx);
                cpuSetSI(ep->parent_si);
                cpuSetDI(ep->parent_di);
                cpuSetBP(ep->parent_bp);
                cpuSetDS(ep->parent_ds);
                cpuSetES(ep->parent_es);
                free(ep);
            }
            set_current_PSP(parent_psp);

            // Get last stack
            cpuSetSP(get16(cpuGetAddress(get_current_PSP(), 0x2E)));
            cpuSetSS(get16(cpuGetAddress(get_current_PSP(), 0x30)));
            put16(cpuGetAddress(cpuGetSS(), cpuGetSP()), returnIP);
            put16(cpuGetAddress(cpuGetSS(), cpuGetSP()+2), returnCS);
        }
        restore_handles();
        break;
    }
    case 0x4D: // GET RETURN CODE (ERRORLEVEL)
        cpuSetAX(return_code);
        return_code = 0;
        cpuClrFlag(cpuFlag_CF);
        break;
    case 0x4E: // FIND FIRST MATCHING FILE
        dos_find_first();
        break;
    case 0x4F: // FIND NEXT MATCHING FILE
        dos_find_next(0);
        break;
    case 0x50: // SET CURRENT PSP
        set_current_PSP(cpuGetBX());
        break;
    case 0x51: // GET CURRENT PSP
        cpuSetBX(get_current_PSP());
        break;
    case 0x52: // GET SYSVARS
        cpuSetES(dos_sysvars >> 4);
        cpuSetBX((dos_sysvars & 0xF) + 24);
        break;
    case 0x26: // Create PSP (duplicate current PSP)
    case 0x55: // Create CHILD PSP
    {
        uint8_t buf[0x80];
        uint32_t new_psp = cpuGetAddress(cpuGetDX(), 0);
        uint32_t orig_psp = cpuGetAddress(get_current_PSP(), 0);
        if(check_limit(0x100, new_psp) || check_limit(0x100, orig_psp))
        {
            debug(debug_dos, "\tinvalid new PSP segment %04x.\n", cpuGetDX());
            break;
        }
        // Copy PSP to the new segment, 0x80 is what DOS does - this excludes command line
#ifdef IA32
        meml_reads(orig_psp, buf, 0x80);
        meml_writes(new_psp, buf, 0x80);
#else
        memcpy(buf, memory + orig_psp, 0x80);
        memcpy(memory + new_psp, buf, 0x80);
#endif
        put16(new_psp + 22, get_current_PSP());
        if(ah == 0x55)
        {
            put16(new_psp + 0x32, 20);      // default size of JFT is 20
            put16(new_psp + 0x34, 0x18);    // default JFT offset
            put16(new_psp + 0x36, new_psp); // default JFT is on PSP
            copy_jft(cpuGetDX(), cpuGetAddress(get16(orig_psp + 0x36),
                                               get16(orig_psp + 0x34)),
                     get8(orig_psp + 0x32) & 0xff);
        }
        set_current_PSP(cpuGetDX());
        break;
    }
    case 0x56: // RENAME
    {
        char *fname1 = dos_unix_path(cpuGetAddrDS(cpuGetDX()), 0, 0);
        if(!fname1)
        {
            debug(debug_dos, "\t(file not found)\n");
            dos_error = 2;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        char *fname2 = dos_unix_path(cpuGetAddrES(cpuGetDI()), 1, 0);
        if(!fname2)
        {
            free(fname1);
            debug(debug_dos, "\t(destination not found)\n");
            dos_error = 3;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        debug(debug_dos, "\t'%s' -> '%s'\n", fname1, fname2);
        int e = rename(fname1, fname2);
        free(fname2);
        free(fname1);
        if(e)
        {
            cpuSetFlag(cpuFlag_CF);
            if(errno == ENOTDIR)
                dos_error = 3;
            else if(errno == ENOENT)
                dos_error = 2;
            else
                dos_error = 5;
            cpuSetAX(dos_error);
        }
        else
        {
            dos_error = 0;
            cpuClrFlag(cpuFlag_CF);
        }
        break;
    }
    case 0x57: // DATE/TIME
        intr21_57();
        break;
    case 0x58: // MEMORY ALLOCATION STRATEGY
    {
        uint8_t al = ax & 0xFF;
        if(0 == al)
            cpuSetAX(mem_get_alloc_strategy());
        else if(1 == al)
            mem_set_alloc_strategy(cpuGetBX());
        else if(3 == al)
        {
            cpuSetFlag(cpuFlag_CF);
            dos_error = 1;
            cpuSetAX(dos_error);
        }
        break;
    }
    case 0x59: // GET EXTENDED ERROR
        cpuSetAX(dos_error);
        break;
    case 0x5B: // CREATE NEW FILE
        dos_open_file(2, cpuGetAX() & 0xFF, cpuGetAddrDS(cpuGetDX()));
        break;
    case 0x5D: // DOS informations
        if(cpuGetAX() == 0x5D06)
        {
            uint32_t sda = indos_flag - 1;
            cpuSetDS(sda >> 4);
            cpuSetSI(sda & 0x0f);
            cpuSetCX(0x17);
            cpuSetDX(0x17);
            cpuClrFlag(cpuFlag_CF);
        }
        else if(cpuGetAX() == 0x5D0B)
        {
            cpuSetDS(sda_table >> 4);
            cpuSetSI(sda_table & 0x0f);
            cpuClrFlag(cpuFlag_CF);
        }
        else
        {
            dos_error = 1;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
        }
        break;
    case 0x60: // TRUENAME - CANONICALIZE FILENAME OR PATH
    {
#ifdef IA32
        uint32_t path_addr = cpuGetAddrDS(cpuGetSI());
        uint32_t out_addr = cpuGetAddrES(cpuGetDI());
        uint8_t buf[128];

        if(check_limit(64, path_addr) || check_limit(128, out_addr))
        {
            dos_error = 3;
            cpuSetFlag(cpuFlag_CF);
            break;
        }

        // Copy input path to output
        meml_reads(path_addr, buf+3, sizeof(buf)-3);
        buf[sizeof(buf)-1] = 0;
        debug(debug_dos, "\t '%s' ", buf+3);
        int drive = dos_path_normalize((char *)(buf + 3), 127 - 3);
        buf[2] = '\\';
        buf[1] = ':';
        buf[0] = 'A' + drive;
        meml_writes(out_addr, buf, strlen((char *)buf)+1);
        debug(debug_dos, "-> '%s'\n", buf);
#else
        uint8_t *path_ptr = getptr(cpuGetAddrDS(cpuGetSI()), 64);
        uint8_t *out_ptr = getptr(cpuGetAddrES(cpuGetDI()), 128);

        if(!path_ptr || !out_ptr)
        {
            dos_error = 3;
            cpuSetFlag(cpuFlag_CF);
            break;
        }

        // Copy input path to output
        int i;
        for (i = 0; path_ptr[i] && i < 128-3; i++)
            (out_ptr + 3)[i] = path_ptr[i];
        out_ptr[127] = 0;
        int drive = dos_path_normalize((char *)(out_ptr + 3), 127 - 3);
        out_ptr[2] = '\\';
        out_ptr[1] = ':';
        out_ptr[0] = 'A' + drive;
        debug(debug_dos, "\t '%s' -> '%s'\n", path_ptr, out_ptr);
#endif
        cpuClrFlag(cpuFlag_CF);
        cpuSetAX(0x5C);
        break;
    }
    case 0x62: // GET PSP SEGMENT
        cpuSetBX(get_current_PSP());
        break;
    case 0x63: // GET DOUBLE BYTE CHARACTER SET LEAD-BYTE TABLE
        cpuSetSI(nls_dbc_set_table & 0xF);
        cpuSetDS(nls_dbc_set_table >> 4);
        cpuSetAX(cpuGetAX() & 0xFF00);
        cpuClrFlag(cpuFlag_CF);
        break;
    case 0x65: // GET NLS DATA
    {
        uint32_t addr = cpuGetAddrES(cpuGetDI());
        uint16_t len = cpuGetCX();
        uint32_t table = 0;
        cpuClrFlag(cpuFlag_CF);
        switch(ax & 0xFF)
        {
        case 1:
        {
            if(len < 41)
                break;
            static const uint8_t data[] = {1, 38, 0, 1, 0, 181, 1};
            putmem(addr, data, 7);
            putmem(addr + 7, nls_country_info, 34);
            cpuSetCX(41);
            return 0;
        }
        case 2: // Uppercase table
            table = nls_uppercase_table;
            break;
        case 4: // File name uppercase table
            table = nls_uppercase_table;
            break;
        case 5: // File name terminator table
            table = nls_terminator_table;
            break;
        case 6: // Collating sequence table
            table = nls_collating_table;
            break;
        case 7: // Double byte character set table
            table = nls_dbc_set_table;
            break;
        default:
            // Not supported
            break;
        }
        if(table && len >= 5)
        {
            put8(addr, ax & 0xFF);
            put16(addr + 1, table & 0xF);
            put16(addr + 3, table >> 4);
            cpuSetCX(5);
            return 0;
        }
        // Unsupported function code
        dos_error = 1;
        cpuSetAX(dos_error);
        cpuSetFlag(cpuFlag_CF);
        break;
    }
    case 0x66: // GET GLOBAL CODE PAGE
        cpuSetBX(437);
        cpuSetDX(437);
        cpuClrFlag(cpuFlag_CF);
        break;
    case 0x67: // SET HANDLE COUNT
    {
        int newcount = cpuGetBX();
        int oldcount;
        unsigned int cur_psp = get_current_PSP();
        uint32_t jft_addr = get_jft_addr(cur_psp, &oldcount);
        if(newcount > 20 && oldcount <= 20)
        {
            uint16_t max;
            uint16_t seg = mem_alloc_segment(16, &max);
            if(!seg)
            {
                dos_error = 8;
                cpuSetAX(dos_error);
                cpuSetFlag(cpuFlag_CF);
                break;
            }
            int i;
            for (i = 0; i < oldcount; i++)
                put8(seg*16 + i, get8(jft_addr + i));
            for (; i < 256; i++)
                put8(seg*16 + i, 0xFF);
            put16(cur_psp*16 + 0x32, 0xFF);
            put16(cur_psp*16 + 0x34, 0);
            put16(cur_psp*16 + 0x36, seg);
        }
        else if(newcount <= 20 && oldcount > 20)
        {
            int i;
            for (i = 0; i < newcount; i++)
                put8(cur_psp*16 + 0x18 + i, get8(jft_addr + i));
            for (;i < 20; i++)
                put8(cur_psp*16 + 0x18 + i, 0xff);
            put16(cur_psp*16 + 0x32, 20);
            put16(cur_psp*16 + 0x34, 0x18);
            put16(cur_psp*16 + 0x36, cur_psp);
            mem_free_segment((jft_addr >> 4) & 0xFFFF);
        }
        dos_error = 0;
        cpuClrFlag(cpuFlag_CF);
        break;
    }
    case 0x68: // fflush
    {
        int sidx = handle_to_sidx(cpuGetBX());
        FILE *f = (sidx < 0) ? NULL : filetable[sidx].f;
        if(!f)
        {
            cpuSetFlag(cpuFlag_CF);
            dos_error = 6; // invalid handle
            cpuSetAX(dos_error);
            break;
        }
        fflush(f);
        break;
    }
    case 0x6C: // EXTENDED OPEN/CREATE FILE
    {
        unsigned cmod = cpuGetDX() & 0xFF;
        //  cmod    action
        //   00     fail always                                         -
        //   01     open if exists already, fail if not                 0
        //   02     clear and open if exists, fail if not               -
        //   10     create if not exists, fail if not.                  2
        //   11     create if not exists, open if exists                3
        //   12     create if not exists, clear and open if exists.     1
        int create = -1;
        if(cmod == 0x01)
            create = 0;
        else if(cmod == 0x10)
            create = 2;
        else if(cmod == 0x12)
            create = 1;
        else if(cmod == 0x11)
            create = 3;
        else
        {
            // TODO: unsupported open mode
            debug(debug_dos, "\tUnsupported open mode: %02x\n", cmod);
            dos_error = 1;
            cpuSetAX(dos_error);
            cpuSetFlag(cpuFlag_CF);
            break;
        }
        int e = dos_open_file(create, cpuGetBX() & 0xFF, cpuGetAddrDS(cpuGetSI()));
        if(e)
            cpuSetCX(e);
        break;
    }
    default:
        debug(debug_dos, "UNHANDLED INT 21, AX=%04x\n", cpuGetAX());
        debug(debug_int, "UNHANDLED INT 21, AX=%04x\n", cpuGetAX());
        dos_error = 1;
        cpuSetFlag(cpuFlag_CF);
        cpuSetAX(ax & 0xFF00);
    }
    decr_indos();
    return ret;
}

// DOS int 22 - TERMINATE ADDRESS
NORETURN void intr22(void)
{
    debug(debug_dos, "D-22: TERMINATE HANDLER CALLED\n");
    // If we reached here, we must terminate now
    exit(return_code & 0xFF);
}

static char *addstr(char *dst, const char *src, int limit)
{
    while(limit > 0 && *src)
    {
        *dst = *src;
        dst++;
        src++;
        limit--;
    }
    return dst;
}

// Initialize append structure
static void init_append(void)
{
    char *env = getenv(ENV_APPEND);
    // allocate append path and status
    dos_append = get_static_memory(DOS_APPEND_SIZE + 2, 0);
    if(env)
    {
        put16(dos_append, 0x0001);
#ifdef IA32
        int len = strlen(env);
        if(len > 0xFF)
            len = 0xFF;
        meml_writes(dos_append + 2, env, len);
#else
        strncpy((char *)memory + dos_append + 2, env, 0xFF);
#endif
    }
}

// Initializes all NLS data for INT 21/38 and INT 21/65
static void init_nls_data(void)
{
    static const uint8_t uppercase_table[128] = {
        0x80, 0x9A, 0x45, 0x41, 0x8E, 0x41, 0x8F, 0x80, 0x45, 0x45, 0x45, 0x49, 0x49,
        0x49, 0x8E, 0x8F, 0x90, 0x92, 0x92, 0x4F, 0x99, 0x4F, 0x55, 0x55, 0x59, 0x99,
        0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F, 0x41, 0x49, 0x4F, 0x55, 0xA5, 0xA5, 0xA6,
        0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2, 0xB3,
        0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0,
        0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD,
        0xCE, 0xCF, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA,
        0xDB, 0xDC, 0xDD, 0xDE, 0xDF, 0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
        0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4,
        0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
    };
    static const uint8_t collating_table[256] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C,
        0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x19,
        0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26,
        0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F, 0x30, 0x31, 0x32, 0x33,
        0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40,
        0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D,
        0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A,
        0x5B, 0x5C, 0x5D, 0x5E, 0x5F, 0x60, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47,
        0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F, 0x50, 0x51, 0x52, 0x53, 0x54,
        0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F, 0x43, 0x55,
        0x45, 0x41, 0x41, 0x41, 0x41, 0x43, 0x45, 0x45, 0x45, 0x49, 0x49, 0x49, 0x41,
        0x41, 0x45, 0x41, 0x41, 0x4F, 0x4F, 0x4F, 0x55, 0x55, 0x59, 0x4F, 0x55, 0x24,
        0x24, 0x24, 0x24, 0x24, 0x41, 0x49, 0x4F, 0x55, 0x4E, 0x4E, 0xA6, 0xA7, 0x3F,
        0xA9, 0xAA, 0xAB, 0xAC, 0x21, 0x22, 0x22, 0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5,
        0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF, 0xC0, 0xC1, 0xC2,
        0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
        0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC,
        0xDD, 0xDE, 0xDF, 0xE0, 0x53, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9,
        0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF, 0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6,
        0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
    };
    static const uint8_t terminator_table[24] = {
        0x16, 0x00, // size of table = 22 bytes
        0x01,       // ???
        0x00,       // lowest char in filename
        0xFF,       // highest char in filename
        0x00,       // ???
        0x00,       // first excluded char
        0x20,       // last excluded char (all from 00-20 excluded)
        0x02,       // ???
        0x0E,       // number of terminator characters
        0x2E, 0x22, 0x2F, 0x5C, 0x5B, 0x5D, 0x3A,
        0x7C, 0x3C, 0x3E, 0x2B, 0x3D, 0x3B, 0x2C};
    static const uint8_t fn_uppercase[16] = {
        0x3C, 0x80,       //     CMP    AL,80
        0x72, 0x0B,       //     JB     xit
        0x53,             //     PUSH   BX
        0x30, 0xFF,       //     XOR    BH,BH
        0x88, 0xC3,       //     MOV    BL,AL
        0x2E,             //     CS:
        0x8A, 0x87, 2, 0, //     MOV    AL,[BX+0002] ; offset of uppercase table
        0x5B,             //     POP    BX
        0xCB              // xit: RETF
    };
    static uint8_t country_info[34] = {
        1,   0,          // Date format
        '$', 0, 0, 0, 0, // Currency symbol string
        ',', 0,          // Thousands separator
        '.', 0,          // Decimal separator
        '-', 0,          // Date separator
        ':', 0,          // Time separator
        0,               // Currency format
        2,               // Digits after decimal in currency
        0,               // Time format
        0,   0, 0, 0,    // Uppercase function address - patched in code
        ',', 0,          // Data list separator
        0,   0, 0, 0, 0, 0, 0, 0, 0, 0,
    };

    // Uppercase table
    nls_uppercase_table = get_static_memory(128 + 2 + 16, 0);
    put16(nls_uppercase_table, 128); // Length
    putmem(nls_uppercase_table + 2, uppercase_table, 128);
    // Uppercase function
    uint16_t fn_ucase_seg = nls_uppercase_table >> 4;
    uint16_t fn_ucase_off = (nls_uppercase_table & 0xF) + 128 + 2;
    putmem(nls_uppercase_table + 128 + 2, fn_uppercase, 16);

    // Country info
    country_info[18] = fn_ucase_off & 0xFF;
    country_info[19] = fn_ucase_off >> 8;
    country_info[20] = fn_ucase_seg & 0xFF;
    country_info[21] = fn_ucase_seg >> 8;
    nls_country_info = country_info;

    // Terminators table
    nls_terminator_table = get_static_memory(24, 0);
    putmem(nls_terminator_table, terminator_table, 24);

    // Collating table
    nls_collating_table = get_static_memory(256 + 2, 0);
    put16(nls_collating_table, 256); // Length
    putmem(nls_collating_table + 2, collating_table, 256);

    // Double-byte-chars table
    int dbcs_num;
    for (dbcs_num = 0; dbcs_num < 4; dbcs_num++) {
        if (cp_dbcs[dbcs_num*2] == 0 && cp_dbcs[dbcs_num*2+1] == 0)
            break;
    }
    nls_dbc_set_table = get_static_memory(dbcs_num*2+2, 0);
    for (int i = 0; i < dbcs_num; i++) {
        put8(nls_dbc_set_table+i*2, cp_dbcs[i*2]);
        put8(nls_dbc_set_table+i*2+1, cp_dbcs[i*2+1]);
    }
    put16(nls_dbc_set_table+dbcs_num*2, 0);
}

void install_dummy_handler(int inum)
{
    uint32_t hndl = get_static_memory(5, 1);
    put8( hndl + 0, 0xea); // JMPF 0000:inum
    put16(hndl + 1, inum); //
    put16(hndl + 3, 0);    //
    put16(inum*4 + 0, hndl & 0xF);
    put16(inum*4 + 2, (hndl >> 4) & 0xFFFF);
}

void init_dos(int argc, char **argv)
{
    char args[256], environ[4096];
    int conv_args = 0;
    memset(args, 0, 256);
    memset(environ, 0, sizeof(environ));

    init_handles();
    init_codepage();
    init_nls_data();
    init_append();

    // frees the find-first-list on exit
    atexit(free_find_first_dta);

    // Set filename mode
    if(getenv(ENV_FILENAME))
    {
        const char *mode = getenv(ENV_FILENAME);
        if(strcasecmp(mode, "8bit") == 0) {
            conv_args = 1;
            dosname_mode(DOSNAME_8BIT);
        }
        else if(strcasecmp(mode, "dbcs") == 0) {
            conv_args = 1;
            dosname_mode(DOSNAME_DBCS);
        }
        else if(strcasecmp(mode, "8bit-noconvargs") == 0) {
            conv_args = 0;
            dosname_mode(DOSNAME_8BIT);
        }
        else if(strcasecmp(mode, "dbcs-noconvargs") == 0) {
            conv_args = 0;
            dosname_mode(DOSNAME_DBCS);
        }
    }

    // Init DOS version
    if(getenv(ENV_DOSVER))
    {
        const char *ver = getenv(ENV_DOSVER);
        char *end = 0;
        int minor = 0;
        int major = strtol(ver, &end, 10);

        if(*end == '.' && end[1])
            minor = strtol(end + 1, &end, 10);

        if(*end || major < 1 || major > 6 || minor < 0 || minor > 99)
            print_error("invalid DOS version '%s'\n", ver);
        dosver = (minor << 8) | major;
        debug(debug_dos, "set dos version to '%s' = 0x%04x\n", ver, dosver);
    }

    // Init INTERRUPT handlers - point to our own handlers
    for(int i = 0; i < 256; i++)
    {
        put8(i * 4 + 0, i);
        put8(i * 4 + 1, 0);
        put8(i * 4 + 2, 0);
        put8(i * 4 + 3, 0);
    }
    // Install IRQ0 handler
    put16(0x8 * 4 + 0, 0);
    put16(0x8 * 4 + 2, 0xFFFD);
    // Special hack for DOS/4GW
    for(int i = 0x50; i < 0x60; i++)
        put8(i * 4 + 0, 0x50);
    // Special hack for IBM-PC detect
    put8(0xdd * 4 + 0, 0xdc);
    // install dummy handler for DOS/4GW support
    install_dummy_handler(0x9);
    install_dummy_handler(0x33);

    // Patch an INT 21 at address 0x000C0, this is for the CP/M emulation code
    put8(0x000C0, 0xCD);
    put8(0x000C1, 0x21);
    
#ifdef EMS_SUPPORT
    const char *emsmem = getenv(ENV_EMSMEM);
    int ems_pages = 0;
    if (emsmem != NULL) {
        char *ep;
        ems_pages = strtol(emsmem, &ep, 0);
        if (*ep  || ems_pages < 0 || ems_pages > 2048)
            print_error("%s must be set between 0 to 2048\n", ENV_EMSMEM);
    }
    if (ems_pages != 0) {
        debug(debug_dos, "set EMS pages = %d\n", ems_pages);
        init_ems(ems_pages);
    }
#endif

    // Init memory handling - available start address at 0x800,
    // ending address at 0xB8000.
    // We limit here memory to less than 512K to fix some old programs
    // that check memori using "JLE" instead of "JBE".
    int memflag = 0;
    {
        const char *p = getenv(ENV_MEMFLAG);
        if(p) {
            char *ep;
            memflag = strtoul(p, &ep, 0);
            if (*ep)
                memflag = 0;
        }
        if(getenv(ENV_LOWMEM))
            memflag |= 0x01;
    }
    int mcb_begin = 0x0080;
    int mcb_end   = 0xB800;
    if(memflag & 0x01)
        mcb_end = 0x7FFF;
    else if(memflag & 0x02)
        mcb_end = 0xA000;
    if(memflag & 0x04)
        mcb_begin = 0x1000;
    mcb_init(mcb_begin, mcb_end);

    // Init SYSVARS
    dos_sysvars = get_static_memory(128, 0);
    put16(dos_sysvars + 22, mcb_begin); // First MCB
    put16(dos_sysvars + 6, 0xffff); // OEM function header
    put16(dos_sysvars + 8, 0xffff); // OEM function header
    put16(dos_sysvars + 28, DOS_SFT_BASE & 0xf); // system file table
    put16(dos_sysvars + 30, DOS_SFT_BASE >> 4);  // system file table
    // NUL driver
    static const uint8_t null_device[] = {
        0xff, 0xff, 0x00, 0x00,                    // Next driver
        0x04, 0x80,                                // Device Attributes
        0x00, 0x00, 0x00, 0x00,                    // Request / Int entry points
        'N',  'U',  'L',  ' ',  ' ', ' ', ' ', ' ' // Name
    };
    putmem(dos_sysvars + 24 + 0x22, null_device, sizeof(null_device));

    // Indos Flag & Swappable Data Area
    indos_flag = get_static_memory(0x12, 0);
    for (int i = 0; i < 0x12; i++)
        put8(indos_flag + i, 0);
    if((dosver & 0xFF) > 2) // critical error flags is placed before indos flag
        indos_flag++;
    sda_table = get_static_memory(8, 0);
    put16(sda_table, 1);
    put16(sda_table + 2, indos_flag & 0x0F);
    put16(sda_table + 4, indos_flag >> 4);
    put16(sda_table + 6, 0x19);

    // clear System File Table on DOS system
    put16(DOS_SFT_BASE + 0, 0xffff);
    put16(DOS_SFT_BASE + 2, 0xffff);
    put16(DOS_SFT_BASE + 4, 0xFF);
    for(int i = 0; i < 0x3b*100; i++)
        put8(DOS_SFT_BASE + 6 + i, 0);

    // Setup default drive
    if(getenv(ENV_DEF_DRIVE))
    {
        char c = getenv(ENV_DEF_DRIVE)[0];
        c = (c >= 'a') ? c - 'a' : c - 'A';
        if(c >= 0 && c <= 25)
        {
            dos_set_default_drive(c);
            debug(debug_dos, "set default drive = '%c'\n", c + 'A');
        }
    }

    // Setup CWD
    if(getenv(ENV_CWD))
    {
        char path[64];
        memset(path, 0, 64);
        strncpy(path, getenv(ENV_CWD), 63);
        dos_change_cwd(path);
    }
    else
    {
        // No CWD given, translate from base path of default drive
        char *cwd = dos_real_path(".");
        if(cwd)
        {
            dos_change_cwd(cwd);
            free(cwd);
        }
        else
            debug(debug_dos, "\tWARNING: working directory outside default drive\n");
    }

    // Concat rest of arguments
    int i;
    char *p = args;
    for(i = 1; i < argc && strcmp(argv[i], "--"); i++)
    {
        if(i != 1)
            p = addstr(p, " ", 127 - (p - args));
        if(conv_args) {
            const uint8_t *s = (const uint8_t *)argv[i];
            while (*s && 127 - (p - args) > 0) {
                int c1, c2, unicode, n;
                unicode = utf8_to_unicode(&s);
                n = get_dos_char(unicode, &c1, &c2);
                if (n == 1) {
                    *p++ = c1;
                }
                else if (n == 2) {
                    if (127 - (p - args) <= 2)
                        break;
                    *p++ = c1; *p++ = c2;
                }
            }
        }
        else
            p = addstr(p, argv[i], 127 - (p - args));
    }
    *p = 0;

    // Copy environment
    int have_path = 0;
    p = environ;
    for(i++; i < argc; i++)
    {
        if(!strncmp("PATH=", argv[i], 5) || !strcmp("PATH", argv[i]))
            have_path = 1;
        if(conv_args) {
            const uint8_t *s = (const uint8_t *)argv[i];
            while (*s && environ + sizeof(environ) - 2 - p > 0) {
                int c1, c2, unicode, n;
                unicode = utf8_to_unicode(&s);
                n = get_dos_char(unicode, &c1, &c2);
                if (n == 1) {
                    *p++ = c1;
                }
                else if (n == 2) {
                    if (environ + sizeof(environ) - 2 - p <= 2)
                        break;
                    *p++ = c1; *p++ = c2;
                }
            }
        }
        else
            p = addstr(p, argv[i], environ + sizeof(environ) - 2 - p);
        *p = 0;
        p++;
    }
    if(!have_path)
    {
        // Adds a PATH variable.
        p = addstr(p, "PATH=C:\\", environ + sizeof(environ) - 2 - p);
        *p = 0;
        p++;
    }

    const char *progname = getenv(ENV_PROGNAME);
    char *buf = 0;
    if(!progname)
    {
        buf = dos_real_path(argv[0]);
        if(!buf)
            progname = argv[0];
        else
            progname = buf;
    }

    // Create main PSP
    int psp_mcb = create_PSP(args, environ, p - environ + 1, progname);
    free(buf);

    // Load program
    const char *name = argv[0];
    FILE *f = fopen(name, "rb");
    if(!f)
        print_error("can't open '%s': %s\n", name, strerror(errno));
    if(!dos_load_exe(f, psp_mcb))
        print_error("error loading EXE/COM file.\n");
    fclose(f);

    // Init JFT
    init_jft(get_current_PSP());

    // Init DTA
    dosDTA = get_current_PSP() * 16 + 0x80;
    put16(indos_flag + 0x0b, 0x80);
    put16(indos_flag + 0x0d, get_current_PSP());

    // Init DOS flags
    cpuSetStartupFlag(cpuFlag_IF);
    cpuClrStartupFlag(cpuFlag_DF);
    cpuClrStartupFlag(cpuFlag_TF);
}

void intr28(void)
{
    usleep(1); // TODO: process messages?
}

void intr29(void)
{
    uint16_t ax = cpuGetAX();
    // Fast video output
    debug(debug_int, "D-29: AX=%04X\n", ax);
    debug(debug_dos, "D-29:   fast console out  AX=%04X\n", ax);
    dos_putchar(ax & 0xFF, 1);
}
