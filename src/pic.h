
#pragma once

#include <stdint.h>

void pic_reinit(void);
uint8_t port_pic_read(unsigned port);
void port_pic_write(unsigned port, uint8_t value);
void pic_eoi(int num);
void cpuTriggerIRQ(int num);
void handle_irq(void);
