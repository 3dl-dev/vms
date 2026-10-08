/*
 * reentrancy.h - DEC C reentrancy level (vms-db7).
 *
 * OVMX's C library is always thread-safe, so C$C_MULTITHREAD is honoured
 * trivially. decc$set_reentrancy records the level and returns 0; a level other
 * than the two defined here is -1 with errno EINVAL. decc$get_reentrancy reads
 * the recorded level back. Carried by the RMS-backed DECC$SHR.
 */
#ifndef __REENTRANCY_H
#define __REENTRANCY_H

#define C$C_SINGLE_THREAD 0
#define C$C_MULTITHREAD   1

int decc$set_reentrancy(int __level);
int decc$get_reentrancy(void);

#endif /* __REENTRANCY_H */
