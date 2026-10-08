#ifndef OVMX_ASYNC_H
#define OVMX_ASYNC_H
/* Internal to libvms: completion of an asynchronous system service. */
#include <stdint.h>
uint32_t vms$$async_begin(uint32_t efn);
uint32_t vms$$async_finish(uint32_t efn, void *iosb, uint32_t status,
                           void (*astadr)(uint32_t), uint32_t astprm);
#endif
