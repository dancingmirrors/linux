/* SPDX-License-Identifier: MIT
 *
 * Copyright (c) 2025, NVIDIA CORPORATION. All rights reserved.
 */
#ifndef __gh100_dev_gc6_island_h__
#define __gh100_dev_gc6_island_h__

#define NV_PGC6_SCI_SYS_TIMER_OFFSET_0                           0x00118df4 /* RW-4R */
#define NV_PGC6_SCI_SYS_TIMER_OFFSET_0_UPDATE                           0:0 /* RWEVF */
#define NV_PGC6_SCI_SYS_TIMER_OFFSET_0_UPDATE_DONE               0x00000000 /* R-E-V */
#define NV_PGC6_SCI_SYS_TIMER_OFFSET_0_UPDATE_TRIGGER            0x00000001 /* -W--T */
#define NV_PGC6_SCI_SYS_TIMER_OFFSET_0_NSEC                            31:5 /* RWEUF */
#define NV_PGC6_SCI_SYS_TIMER_OFFSET_0_NSEC_ZERO                 0x00000000 /* RWE-V */
#define NV_PGC6_SCI_SYS_TIMER_OFFSET_1                           0x00118df8 /* RW-4R */
#define NV_PGC6_SCI_SYS_TIMER_OFFSET_1_NSEC                            28:0 /* RWEUF */
#define NV_PGC6_SCI_SYS_TIMER_OFFSET_1_NSEC_ZERO                 0x00000000 /* RWE-V */

#define NV_PGC6_SCI_SEC_TIMER_TIME_0                             0x00118f54 /* RW-4R */
#define NV_PGC6_SCI_SEC_TIMER_TIME_0_NSEC                              31:5 /* RWEUF */
#define NV_PGC6_SCI_SEC_TIMER_TIME_0_NSEC_ZERO                   0x00000000 /* RWE-V */
#define NV_PGC6_SCI_SEC_TIMER_TIME_1                             0x00118f58 /* RW-4R */
#define NV_PGC6_SCI_SEC_TIMER_TIME_1_NSEC                              28:0 /* RWEUF */
#define NV_PGC6_SCI_SEC_TIMER_TIME_1_NSEC_ZERO                   0x00000000 /* RWE-V */

#endif // __gh100_dev_gc6_island_h__
