
#pragma once

#include <stdint.h>

extern uint32_t memory_mask;

void set_a20_enable(int);
int query_a20_enable(void);
void xms_farcall(void);
uint32_t xms_entry_point(void);
int init_xms(int maxmem);
uint8_t port_misc_read(unsigned port);
void port_misc_write(unsigned port, uint8_t value);
