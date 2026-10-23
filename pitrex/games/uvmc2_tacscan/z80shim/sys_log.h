/* SPDX-License-Identifier: GPL-2.0-or-later
 * Shim for AAE's sys_log.h: logging compiled out. */
#ifndef Z80SHIM_SYS_LOG_H
#define Z80SHIM_SYS_LOG_H
#define LOG_INFO(...)  ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#endif
