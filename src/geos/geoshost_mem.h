// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file geoshost_mem.h
 * @brief Guest memory access for GEOS host interface sub-APIs.
 *
 * Resolves guest segment:offset pointers in the CPU mode active at the
 * time of the call: real/V86 mode uses seg<<4 + off, protected mode uses
 * GDT/LDT descriptors with present/type/limit checks.
 */

#ifndef DOSBOX_GEOSHOST_MEM_H
#define DOSBOX_GEOSHOST_MEM_H

#include <cstdint>

#include "hardware/memory.h"

namespace GeosGuestMem {

enum class Access { Read, Write };

struct GuestSpan {
	PhysPt base    = 0; ///< linear address of seg:off
	uint32_t avail = 0; ///< bytes accessible from base (segment limit)
};

/** Resolve seg:off; fails for invalid/absent/wrong-type segments. */
bool resolve(uint16_t seg, uint16_t off, Access acc, GuestSpan& span);

/** Resolve seg:off and require @p len accessible bytes. */
bool resolve_len(uint16_t seg, uint16_t off, uint32_t len, Access acc, PhysPt& lin);

/** True if the CPU currently interprets segments as selectors. */
bool selector_mode();

} // namespace GeosGuestMem

#endif // DOSBOX_GEOSHOST_MEM_H
