/**
 * i8259 "PIC" emulaiton code for IBM PC/AT.
 *
 * This code is emulating PIC, for IBM PC/AT.
 * There are some restiriction.
 *
 * - Priority rotation is not supported.
 * - 8080/8085 mode is not supported.
 * - Poll mode is not supported.
 * - 1 master and 1 slave, slave PIC is connected to IR2 at master.
 *
 * Copyright (C) 2025 MURAMATSU Atsushi <amura@tomato.sakura.ne.jp>
 */

#include "pic.h"
#include "dbg.h"

enum pic_reg {
    PIC_ICW1_INIT = 0x10,
    PIC_OCW3_FLAG = 0x08,
    PIC_ICW4_AUTOEOI = 0x02,
    PIC_ICW4_SPECIAL_NEST = 0x10,
    PIC_OCW3_ESMM = 0x40,
    PIC_OCW3_SMM = 0x20,
    PIC_OCW3_POLL = 0x04,
    PIC_OCW3_RR = 0x02,
    PIC_OCW3_RIS = 0x01,
};
enum pic_data_read {
    PIC_READ_IRR = 0,
    PIC_READ_ISR = 1,
    PIC_READ_POLL = 2,
};

struct pic_stat {
    int initializing;
    int irq_base;
    int auto_eoi;
    int special_nest;
    int special_mask;
    enum pic_data_read data_read;
    uint8_t IMR;
    uint8_t IRR;
    uint8_t ISR;
    uint8_t pending;
};

static struct pic_stat pic[2] = {
    { 0, 0x08, 0, 0, 0, PIC_READ_IRR, 0xf8, 0x00, 0x00, 0x00 },
    { 0, 0x70, 0, 0, 0, PIC_READ_IRR, 0xff, 0x00, 0x00, 0x00 },
};

#define PIC_CASCADED 2

#ifdef IA32
extern void ia32_interrupt(int vect, int soft);
#define cpu_hard_interrupt(n) ia32_interrupt(n, 0)
#else
extern void interrupt(unsigned int_num);
#define cpu_hard_interrupt(n) interrupt(n)
#endif


void
pic_reinit(void)
{
    for (int i = 0; i < 2; i++) {
        pic[i].initializing = 0;
        pic[i].auto_eoi = 0;
        pic[i].special_nest = 0;
        pic[i].special_mask = 0;
        pic[i].data_read = PIC_READ_IRR;
        pic[i].IRR = 0x00;
        pic[i].ISR = 0x00;
        pic[i].pending = 0x00;
    }
    pic[0].irq_base = 0x08;
    pic[1].irq_base = 0x70;
    pic[0].IMR = 0xf8;
    pic[1].IMR = 0xff;
}

uint8_t
port_pic_read(unsigned port)
{
    int pic_idx;
    if ((port & 0xf0) == 0x20)
        pic_idx = 0; // master
    else
        pic_idx = 1; // slave

    if ((port & 0x0f) == 0) { // CMD
        switch (pic[pic_idx].data_read) {
            case PIC_READ_IRR:
                return pic[pic_idx].IRR;
            case PIC_READ_ISR:
                return pic[pic_idx].ISR;
            case PIC_READ_POLL:
                return 0x00;
            default:
                return 0xff;
        }
    }
    else if ((port & 0x0f) == 1) // DATA
        return pic[pic_idx].IMR;
    return 0xff;
}

void
port_pic_write(unsigned port, uint8_t value)
{
    int pic_idx;
    if ((port & 0xf0) == 0x20)
        pic_idx = 0; // master
    else
        pic_idx = 1; // slave

    if ((port & 0x0f) == 0) { // CMD
        if (value & PIC_ICW1_INIT) {
            if (value != 0x11)
                debug(debug_port, "BAD PIC %d CMD %02x\n", pic_idx, value);
            pic[pic_idx].initializing = 1;
            pic[pic_idx].IMR = 0x00;
            pic[pic_idx].ISR = 0x00;
            pic[pic_idx].IRR = 0x00;
            pic[pic_idx].pending = 0x00;
        }
        else if (value & PIC_OCW3_FLAG) {
            if (value & PIC_OCW3_ESMM)
                pic[pic_idx].special_mask = (value & PIC_OCW3_SMM) ? 1 : 0;
            if (value & PIC_OCW3_RR)
                pic[pic_idx].data_read =
                    (value & PIC_OCW3_RIS) ? PIC_READ_ISR : PIC_READ_IRR;
        }
        else { // EOI
            switch (value & 0xe0) {
                case 0x20: // non-special EOI
                case 0xa0: // non-special EOI with rotate
                    for (uint8_t m = 0x01; m != 0; m <<= 1)
                        if (pic[pic_idx].ISR & m) {
                            pic[pic_idx].ISR &= ~m;
                            break;
                        }
                    break;

                case 0x60: // special EOI
                case 0xe0: // non-special EOI with rotate
                    pic[pic_idx].ISR &= ~(0x01 << (value & 0x07));
                    break;

                default:
                    debug(debug_port, "PIC %d EOI %02x\n", pic_idx, value);
            }
        }
    }
    else if ((port & 0x0f) == 1) { // DATA
        if (pic[pic_idx].initializing == 1) {
            pic[pic_idx].irq_base = value & 0xf8;
            pic[pic_idx].initializing++;
        }
        else if (pic[pic_idx].initializing == 2) {
            //skip ICW3
            pic[pic_idx].initializing++;
        }
        else if (pic[pic_idx].initializing == 3) {
            pic[pic_idx].auto_eoi = (value & PIC_ICW4_AUTOEOI) ? 1 : 0;
            pic[pic_idx].special_nest = (value & PIC_ICW4_SPECIAL_NEST) ? 1 : 0;
            pic[pic_idx].initializing = 0;
            pic[pic_idx].IRR = pic[pic_idx].pending;
        }
        else
            pic[pic_idx].IMR = value;
    }
}

void
pic_eoi(int num)
{
    debug(debug_int, "PIC EOI irq=%d\n", num);
    int pic_idx = 0;
    if (num >= 8) {
        pic_idx = 1;
        num -= 8;
    }
    pic[pic_idx].ISR &= ~(0x01 << num);
    if (pic_idx && !pic[1].ISR)
        pic[0].ISR &= ~(0x01 << PIC_CASCADED);
}

void
cpuTriggerIRQ(int num)
{
    int pic_idx = 0;
    if (num >= 8) {
        pic_idx = 1;
        num -= 8;
    }
    if (pic[pic_idx].initializing)
        pic[pic_idx].pending |= 0x01 << num;
    else
        pic[pic_idx].IRR |= 0x01 << num;
}

void
handle_irq(void)
{
    int slave_intr = -1;

    // check slave
    if (pic[1].IRR) {
        debug(debug_int, "check slave PIC, ISR=%02x, IRR=%02x, IMR=%02x\n",
              pic[1].ISR, pic[1].IRR, pic[1].IMR);
        for (int i = 0; i < 8; i++) {
            uint8_t m = 0x01 << i;
            if (pic[1].ISR & m) { // some higher interrupt is on going.
                if (pic[1].special_mask && (pic[1].IMR & m))
                    continue;
                break;
            }
            if (!pic[1].special_mask && (pic[1].IMR & m)) // masked
                continue;
            if (pic[1].IRR & m) { // interrupt requested
                slave_intr = i;
                cpuTriggerIRQ(PIC_CASCADED);
                break;
            }
        }
    }

    // check master
    if (pic[0].IRR) {
        debug(debug_int, "check master PIC, ISR=%02x, IRR=%02x, IMR=%02x\n",
              pic[0].ISR, pic[0].IRR, pic[0].IMR);
        for (int i = 0; i < 8; i++) {
            uint8_t m = 0x01 << i;
            if (!(i == PIC_CASCADED && pic[0].special_nest)) {
                // not special nested mode
                if (pic[0].ISR & m) { // some higher interrupt is on going.
                    if (pic[0].special_mask && (pic[1].IMR & m))
                        continue;
                    break;
                }
            }
            if (!pic[0].special_mask && (pic[0].IMR & m)) // masked
                continue;
            if (pic[0].IRR & m) { // interrupt requested
                pic[0].IRR &= ~m;
                if (!pic[0].auto_eoi)
                    pic[0].ISR |= m;
                if (i == PIC_CASCADED) { // slave request
                    if (slave_intr >= 0) {
                        uint8_t m2 = 0x01 << slave_intr;
                        pic[1].IRR &= ~m2;
                        if (!pic[1].auto_eoi)
                            pic[1].ISR |= m;
                        debug(debug_int,
                              " ->handle irq, irq=%d\n", slave_intr+8);
                        cpu_hard_interrupt(pic[1].irq_base + slave_intr);
                    }
                }
                else {
                    debug(debug_int, " ->handle irq irq=%d\n", i);
                    cpu_hard_interrupt(pic[0].irq_base + i);
                }
                break;
            }
        }
    }
}
