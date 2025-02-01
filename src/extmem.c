
#include "extmem.h"
#include "emu.h"
#include "dbg.h"

#include <string.h>
#include <stdlib.h>

#define XMS_EMB_BASE  (0x110000) /* AFTER HMA region */

enum XMM_STAUTS {
    XMM_STATUS_SUCCESS = 0x00,
    XMM_STATUS_NOT_IMPLEMENTED = 0x80,
    XMM_STATUS_VDISK_DETECTED = 0x81,
    XMM_STATUS_A20_ERROR = 0x82,
    XMM_STATUS_HMA_DOES_NOT_EXIST = 0x90,
    XMM_STATUS_HMA_ALREADY_IN_USE = 0x91,
    XMM_STATUS_HMA_REQ_LESS_THAN_MIN = 0x92,
    XMM_STATUS_HMA_DOES_NOT_ALLOCATED = 0x93,
    XMM_STATUS_A20_STILL_ENABLED = 0x94,
    XMM_STATUS_EMB_MEM_ALL_ALLOCATED = 0xa0,
    XMM_STATUS_EMB_HANDLE_ALL_ALLOCATED = 0xa1,
    XMM_STATUS_EMB_HANDLE_INVALID = 0xa2,
    XMM_STATUS_EMB_HANDLE_LOCKED = 0xa3,
    XMM_STATUS_EMB_SRC_HANDLE_INVALID = 0xa3,
    XMM_STATUS_EMB_SRC_OFFSET_INVALID = 0xa4,
    XMM_STATUS_EMB_DST_HANDLE_INVALID = 0xa5,
    XMM_STATUS_EMB_DST_OFFSET_INVALID = 0xa6,
    XMM_STATUS_EMB_LENGTH_INVALID = 0xa7,
    XMM_STATUS_EMB_MOVE_OVERLAP = 0xa8,
    XMM_STATUS_EMB_PARITY_ERROR = 0xa9,
    XMM_STATUS_EMB_NOT_LOCKED = 0xaa,
    XMM_STATUS_EMB_BLOCK_IS_LOCKED = 0xab,
    XMM_STATUS_EMB_LOCKCNT_OVERFLOW = 0xac,
    XMM_STATUS_EMB_LOCK_FAILED = 0xad,
    XMM_STATUS_UMB_SMALLER = 0xb0,
    XMM_STATUS_UMB_NOT_AVAILABLE = 0xb1,
    XMM_STATUS_UMB_INVALID_SEG = 0xb2,
};

struct emb_data {
    struct emb_data *next;
    int kb_size;
    int emb_offset;
    int locked;
    int handle;
};
static struct emb_data *EMB_DATA_ROOT;
static int xms_a20_global_enable;
static int xms_a20_local_enable_cnt;
static int hma_occupied;
static uint32_t xms_entry_addr;
static uint16_t emb_lasthandle;
static int a20_enabled = 0;
uint32_t memory_mask = 0xFFFFF;
uint32_t memory_limit = 0xFFFFF;

static const uint8_t xms_entry_stub[] = {
    0xcd, 0xfe, // int 0feh
    0xcb        // retf
};

static struct emb_data *
search_handle(int handle)
{
    if (handle == 0)
        return NULL;
    struct emb_data *p = EMB_DATA_ROOT;
    while (p) {
        if (p->handle == handle)
            return p;
        p = p->next;
    }
    return NULL;
}

static struct emb_data *
search_freemem(uint32_t kb_size)
{
    struct emb_data *p = EMB_DATA_ROOT;
    struct emb_data *res = NULL;
    emb_lasthandle = 0;
    while (p) {
        if (p->handle == 0 && p->kb_size >= kb_size) {
            if(!res || p->kb_size < res->kb_size)
                res = p;
        }
        if (p->handle > emb_lasthandle)
            emb_lasthandle = p->handle;
        p = p->next;
    }

    if (res->kb_size > kb_size) {
        struct emb_data *newfree = malloc(sizeof(struct emb_data));
        if (newfree != NULL) {
            memset(newfree, 0, sizeof(struct emb_data));
            newfree->kb_size    = res->kb_size - kb_size;
            newfree->emb_offset = res->emb_offset + kb_size;
            newfree->next       = res->next;

            res->kb_size = kb_size;
            res->next    = newfree;
        }
    }
    return res;
}

static void
merge_free_region(int *free_total, int *free_max)
{
    struct emb_data *p = EMB_DATA_ROOT;
    int total = 0;
    int max = 0;
    emb_lasthandle = 0;
    while (p) {
        if (p->handle == 0 && p->next) {
            if (p->next->handle == 0) { // next is also free
                struct emb_data *px = p->next;
                p->next = px->next;
                p->kb_size += px->kb_size;
                free(px);
                continue;
            }
        }

        if (p->handle == 0) {
            total += p->kb_size;
            if (max < p->kb_size)
                max = p->kb_size;
        }
        if (p->handle > emb_lasthandle)
            emb_lasthandle = p->handle;
        p = p->next;
    }
    if (free_total)
        *free_total = total;
    if (free_max)
        *free_max = max;
    debug(debug_int, "\t  freemerge: max: %d, total: %d\n", max, total);
}

static void
emb_sanity_check()
{
    struct emb_data *p = EMB_DATA_ROOT;
    while (p) {
        debug(debug_int, "\tHANDLE %04X, lock %d, range : %08X - %08X\n",
              p->handle, p->locked,
              p->emb_offset*1024 + XMS_EMB_BASE,
              (p->emb_offset + p->kb_size)*1024 + XMS_EMB_BASE - 1);
        p = p->next;
    }
}

void
set_a20_enable(int enable)
{
    a20_enabled = enable;
    if(enable)
        memory_mask = memory_limit;
    else
        memory_mask = 0xfffff;
}

int
query_a20_enable(void)
{
    return a20_enabled;
}

void
xms_farcall(void)
{
#ifdef IA32
    debug(debug_int, "XMS-%04X: EDX=%04X\n", cpuGetAX(), cpuGetEDX());
#else
    debug(debug_int, "XMS-%04X: DX=%04X\n", cpuGetAX(), cpuGetDX());
#endif
    unsigned int ax = cpuGetAX();
    int ah = ax >> 8;
    uint8_t bl = XMM_STATUS_NOT_IMPLEMENTED;
    switch (ah)
    {
        case 0x00: // Get Version Number
            cpuSetAX(0x300);  // XMS version 3.00
            cpuSetBX(0x0100); // Driver version 1.00
            cpuSetDX(0x0001); // HMA exist
            break;

        case 0x01: // Request HMA
            if (hma_occupied) {
                bl = XMM_STATUS_HMA_ALREADY_IN_USE;
                cpuSetAX(0x0000);
            }
            else {
                hma_occupied = 1;
                bl = XMM_STATUS_SUCCESS;
                cpuSetAX(0x0001);
            }
            break;

        case 0x02: // Release HMA
            if (hma_occupied) {
                hma_occupied = 0;
                bl = XMM_STATUS_SUCCESS;
                cpuSetAX(0x0001);
            }
            else {
                bl = XMM_STATUS_HMA_DOES_NOT_ALLOCATED;
                cpuSetAX(0x0000);
            }
            break;

        case 0x03: // Global enable A20
            xms_a20_global_enable = 1;
            set_a20_enable(1);
            bl = XMM_STATUS_SUCCESS;
            cpuSetAX(0x0001);
            break;

        case 0x04: // Global disable A20
            xms_a20_global_enable = 0;
            if (xms_a20_local_enable_cnt == 0) {
                set_a20_enable(0);
                cpuSetAX(0x0001);
                bl = XMM_STATUS_SUCCESS;
            }
            else {
                set_a20_enable(1);
                cpuSetAX(0x0000);
                bl = XMM_STATUS_A20_STILL_ENABLED;
            }
            break;

        case 0x05: // Local enable A20
            if (xms_a20_local_enable_cnt > 0xffff) {
                bl = XMM_STATUS_A20_ERROR;
                cpuSetAX(0x0000);
                break;
            }
            xms_a20_local_enable_cnt++;
            set_a20_enable(1);
            bl = XMM_STATUS_SUCCESS;
            cpuSetAX(0x0001);
            break;

        case 0x06: // Local disable A20
            if (xms_a20_local_enable_cnt > 0)
                xms_a20_local_enable_cnt--;
            if (!xms_a20_global_enable && xms_a20_local_enable_cnt == 0) {
                set_a20_enable(0);
                bl = XMM_STATUS_SUCCESS;
                cpuSetAX(0x0001);
            }
            else {
                set_a20_enable(1);
                bl = XMM_STATUS_A20_STILL_ENABLED;
                cpuSetAX(0x0000);
            }
            break;

        case 0x07: // Query A20
            bl = XMM_STATUS_SUCCESS;
            if (query_a20_enable())
                cpuSetAX(0x0001);
            else
                cpuSetAX(0x0000);
            break;

        case 0x08: // Query Free EMB
#ifdef IA32
        case 0x88: // Query Super EMB
#endif
            {
                int free_total, free_max;
                merge_free_region(&free_total, &free_max);
                if (free_total == 0)
                    bl = XMM_STATUS_EMB_MEM_ALL_ALLOCATED;
                else
                    bl = XMM_STATUS_SUCCESS;
#ifdef IA32
                if (ah == 0x88) {
                    cpuSetEAX(free_max);
                    cpuSetEDX(free_total);
                    cpuSetECX(memory_limit / 1024);
                    break;
                }
#endif
                cpuSetAX(free_max > 0xffff ? 0xffff : free_max);
                cpuSetDX(free_total > 0xffff ? 0xffff : free_total);
            }
            break;

        case 0x09: // Allocate EMB
#ifdef IA32
        case 0x89: // Alloc Super EMB
#endif
            {
                int alloc_size = cpuGetDX();
#ifdef IA32
                if (ah == 0x89)
                    alloc_size = cpuGetEDX();
#endif
                struct emb_data *newdata = search_freemem(alloc_size);

                if (emb_lasthandle == 0xFFFF) {
                    bl = XMM_STATUS_EMB_HANDLE_ALL_ALLOCATED;
                    cpuSetAX(0x0000);
                    break;
                }
                if (!newdata) {
                    bl = XMM_STATUS_EMB_MEM_ALL_ALLOCATED;
                    cpuSetAX(0x0000);
                    break;
                }

                newdata->handle = ++emb_lasthandle;
                debug(debug_int, "  allocmem: HANDLE %04X, addr %08X, size %dkb\n", newdata->handle, XMS_EMB_BASE + newdata->emb_offset*1024, newdata->kb_size);
                cpuSetDX(emb_lasthandle);
                bl = XMM_STATUS_SUCCESS;
                cpuSetAX(0x0001);
            }
            break;

        case 0x0a: // Free EMB
            {
                struct emb_data *p = search_handle(cpuGetDX());
                if (p == NULL) {
                    bl = XMM_STATUS_EMB_HANDLE_INVALID;
                    cpuSetAX(0x0000);
                    break;
                }

                if (p->locked) {
                    bl = XMM_STATUS_EMB_HANDLE_LOCKED;
                    cpuSetAX(0x0000);
                }
                else {
                    p->handle = 0;
                    bl = XMM_STATUS_SUCCESS;
                    cpuSetAX(0x0001);
                }
            }
            break;

        case 0x0b: // Move EMB
            {
                uint32_t param_addr = cpuGetAddrDS(cpuGetSI());
                uint32_t len      = get32(param_addr + 0);
                uint16_t src_hndl = get16(param_addr + 4);
                uint32_t src_addr = get32(param_addr + 6);
                uint16_t dst_hndl = get16(param_addr + 10);
                uint32_t dst_addr = get32(param_addr + 12);
                debug(debug_int,
                      "  move emb src=%04X:%08X dst=%04X:%08X len=%d\n",
                      src_hndl, src_addr, dst_hndl, dst_addr, len);
                if (len & 1) {
                    bl = XMM_STATUS_EMB_LENGTH_INVALID;
                    cpuSetAX(0x0000);
                    break;
                }
                if (src_hndl == 0) // Conventional Mem
                    src_addr = cpuGetAddress(src_addr >> 16, src_addr & 0xffff);
                else {
                    struct emb_data *p = search_handle(src_hndl);
                    if (!p) {
                        bl = XMM_STATUS_EMB_SRC_HANDLE_INVALID;
                        cpuSetAX(0x0000);
                        break;
                    }
                    if (src_addr + len > p->kb_size * 1024) {
                        bl = XMM_STATUS_EMB_SRC_OFFSET_INVALID;
                        cpuSetAX(0x0000);
                        break;
                    }
                    src_addr += XMS_EMB_BASE + p->emb_offset * 1024;
                }

                if (dst_hndl == 0) // Conventional Mem
                    dst_addr = cpuGetAddress(dst_addr >> 16, dst_addr & 0xffff);
                else {
                    struct emb_data *p = search_handle(dst_hndl);
                    if (!p) {
                        bl = XMM_STATUS_EMB_DST_HANDLE_INVALID;
                        cpuSetAX(0x0000);
                        break;
                    }
                    if (dst_addr + len > p->kb_size * 1024) {
                        bl = XMM_STATUS_EMB_DST_OFFSET_INVALID;
                        cpuSetAX(0x0000);
                        break;
                    }
                    dst_addr += XMS_EMB_BASE + p->emb_offset * 1024;
                }
                if (src_addr > dst_addr && src_addr < dst_addr + len) {
                    bl = XMM_STATUS_EMB_MOVE_OVERLAP;
                    cpuSetAX(0x0000);
                    break;
                }
                memcpy(memory + dst_addr, memory + src_addr, len);
                bl = XMM_STATUS_SUCCESS;
                cpuSetAX(0x0001);
            }
            break;

        case 0x0c: // Lock EMB
            {
                struct emb_data *p = search_handle(cpuGetDX());
                if (p == NULL) {
                    bl = XMM_STATUS_EMB_HANDLE_INVALID;
                    cpuSetAX(0x0000);
                    break;
                }
                uint32_t addr = XMS_EMB_BASE + p->emb_offset*1024;
                cpuSetDX(addr >> 16);
                cpuSetBX(addr & 0xFFFF);

                if (p->locked >= 0xff) {
                    bl = XMM_STATUS_EMB_LOCKCNT_OVERFLOW;
                    cpuSetAX(0x0000);
                }
                else {
                    p->locked++;
                    bl = XMM_STATUS_SUCCESS;
                    cpuSetAX(0x0001);
                }
            }
            break;

        case 0x0d: // Unlock EMB
            {
                struct emb_data *p = search_handle(cpuGetDX());
                if (p == NULL) {
                    bl = XMM_STATUS_EMB_HANDLE_INVALID;
                    cpuSetAX(0x0000);
                    break;
                }

                if (!p->locked) {
                    bl = XMM_STATUS_EMB_NOT_LOCKED;
                    cpuSetAX(0x0000);
                }
                else {
                    p->locked--;
                    bl = XMM_STATUS_SUCCESS;
                    cpuSetAX(0x0001);
                }
            }
            break;

        case 0x0e: // Get EMB Handle Info
#ifdef IA32
        case 0x8e: // EMB Info (Super)
#endif
            {
                struct emb_data *p = search_handle(cpuGetDX());
                if (p == NULL) {
                    bl = XMM_STATUS_EMB_HANDLE_INVALID;
                    cpuSetAX(0x0000);
                    break;
                }

                int rem_handle = 0xFFFF - emb_lasthandle;
#ifdef IA32
                if (ah == 0x8e)
                    cpuSetEDX(p->kb_size);
                else
#endif
                if (p->kb_size >= 0xffff)
                    cpuSetDX(0xffff);
                else
                    cpuSetDX(p->kb_size);
                cpuSetBX(p->locked << 8);
                bl = (rem_handle > 0xFF) ? 0xFF : rem_handle;
#ifdef IA32
                if (ah == 0x8e) {
                    cpuSetCX(rem_handle > 0xFFFF ? 0xFFFF : rem_handle);
                    bl = XMM_STATUS_SUCCESS;
                }
#endif
                cpuSetAX(0x0001);
            }
            break;

        case 0x0f: // Realloc EMB
#ifdef IA32
        case 0x8f: // Realloc Super EMB
#endif
            {
                struct emb_data *p = search_handle(cpuGetDX());
                if (p == NULL) {
                    bl = XMM_STATUS_EMB_HANDLE_INVALID;
                    cpuSetAX(0x0000);
                    break;
                }
                if (p->locked) {
                    bl = XMM_STATUS_EMB_HANDLE_LOCKED;
                    cpuSetAX(0x0000);
                    break;
                }

                int newsize = cpuGetBX();
#ifdef IA32
                if (ah == 0x8f)
                    newsize = cpuGetEBX();
#endif
                if (p->kb_size > newsize) { // shrink
                    struct emb_data *newfree = malloc(sizeof(struct emb_data));
                    if (newfree != NULL) {
                        memset(newfree, 0, sizeof(struct emb_data));
                        newfree->kb_size    = p->kb_size - newsize;
                        newfree->emb_offset = p->emb_offset + newsize;
                        newfree->next       = p->next;

                        p->kb_size = newsize;
                        p->next    = newfree;
                    }
                }
                else if (p->kb_size < newsize) { // expand
                    merge_free_region(NULL, NULL);
                    if (p->next && p->next->handle == 0 &&
                        (p->kb_size + p->next->kb_size) >= newsize)
                    {
                        if (p->kb_size + p->next->kb_size == newsize) {
                            p->next = p->next->next;
                            free(p->next);
                        }
                        else {
                            p->next->kb_size -= newsize - p->kb_size;
                        }
                        p->kb_size = newsize;
                    }
                    else { // allocate new block
                        struct emb_data *newdata = search_freemem(newsize);
                        if (!newdata) {
                            bl = XMM_STATUS_EMB_MEM_ALL_ALLOCATED;
                            cpuSetAX(0x0000);
                        }
                        memcpy(memory+XMS_EMB_BASE+ 1024*newdata->emb_offset,
                               memory+XMS_EMB_BASE+ 1024*p->emb_offset,
                               p->kb_size);
                        newdata->handle = p->handle;
                        p->handle = 0;
                    }
                }
            }
            break;

        case 0x10: // Request UMB
            cpuSetDX(0);
            bl = XMM_STATUS_UMB_NOT_AVAILABLE;
            cpuSetAX(0x0000);
            break;

        case 0x11: // Release UMB
            bl = XMM_STATUS_UMB_INVALID_SEG;
            cpuSetAX(0x0000);
            break;

        case 0x12: // Realloc UMB
            cpuSetDX(0);
            bl = XMM_STATUS_UMB_INVALID_SEG;
            cpuSetAX(0x0000);
            break;

        default:
            bl = XMM_STATUS_NOT_IMPLEMENTED;
            cpuSetAX(0x0000);
    }

    // set return code
    cpuSetBX((cpuGetBX() & 0xFF00) | bl);
    if(debug_active(debug_int))
        emb_sanity_check();
}

uint32_t
xms_entry_point(void)
{
    return xms_entry_addr;
}

int
init_xms(int maxmem)
{
    int new_maxmem = 0x01;
    while (maxmem) {
        maxmem >>= 1;
        new_maxmem <<= 1;
    }
    maxmem = new_maxmem >> 1;
    
    xms_entry_addr = get_static_memory(sizeof(xms_entry_stub), 1);
    for(int i = 0; i < sizeof(xms_entry_stub); i++)
        put8(xms_entry_addr + i, xms_entry_stub[i]);
    reg_farcall_entry(xms_entry_addr + 2 /* next of int 0ffh */, xms_farcall);

    EMB_DATA_ROOT = (struct emb_data *)malloc(sizeof(struct emb_data));
    if (!EMB_DATA_ROOT)
        return 0;
    memset(EMB_DATA_ROOT, 0, sizeof(struct emb_data));
    EMB_DATA_ROOT->kb_size = maxmem*1024 - XMS_EMB_BASE/1024;
    memory_limit = maxmem*1024*1024 - 1;
    
    return 1;
}

static int cmos_index = 0;
static int cmos_shutdown_type = 0;

uint8_t
port_misc_read(unsigned port)
{
    if (port == 0x70)
        return cmos_index;
    else if (port == 0x71) {
        uint8_t cmos_data = cmos_index == 0x00f ? cmos_shutdown_type : 0x00;
        debug(debug_port, "system port read - cmos(%04X) -> %02X\n",
              cmos_index, cmos_data);
        return cmos_data;
    }
    else if (port == 0x92) {
        int a20_stat = query_a20_enable();
        debug(debug_port, "system port read - check A20=%d\n", a20_stat);
        return a20_stat ? 0x02 : 0x00;
    }
    return 0xff;
}

void
port_misc_write(unsigned port, uint8_t value)
{
    if (port == 0x70)
        cmos_index = value;
    else if (port == 0x71) {
        //debug(debug_port, "system port write - cmos(%04X) <- %02X\n",
        //      cmos_index, value);
        if (cmos_index == 0x00f)
            cmos_shutdown_type = value;
    }
    else if (port == 0x92) {
        debug(debug_port, "system port write - 0x92 <- %02X\n", value);
        set_a20_enable((value & 0x02) ? 1 : 0);
        if (value & 0x01) {
            debug(debug_int, "System reset via system port A!\n");
            cpu_reset();
        }
    }
}

void
system_reboot(void)
{
    char buf[128];
    snprintf(buf, sizeof(buf),
             "CPU RESET, type %02X, restart from %04X:%04X",
	     cmos_shutdown_type, get16(0x469), get16(0x467));
    buf[sizeof(buf)-1] = 0;
    debug(debug_port, "%s\n", buf);
    debug(debug_cpu, "%s\n", buf);
    debug(debug_int, "%s\n", buf);
    if (cmos_shutdown_type != 0x06 && cmos_shutdown_type != 0x09 && 
        cmos_shutdown_type != 0x0a)
        exit(1);
    cpuSetIP(get16(0x467));
    cpuSetCS(get16(0x469));
}
