// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file geoshost_mem.cpp
 * @brief Guest memory access for GEOS host interface sub-APIs.
 */

#include "dosbox.h"

#if C_GEOSHOST

#include "geoshost_mem.h"

#include "cpu/cpu.h"
#include "cpu/registers.h"

namespace GeosGuestMem {

bool selector_mode()
{
	return cpu.pmode && !GETFLAG(VM);
}


/**
 * Resolve seg:off in the CPU mode that is active right now.
 *
 * Real and V86 mode: classic seg<<4 + off, up to the 64 KB segment end.
 * Protected mode: GDT/LDT descriptor (TI bit honoured by GetDescriptor),
 * must be present, a code/data segment with the requested access right,
 * and not expand-down (not used by GEOS, rejected for simplicity).
 */
bool resolve(const uint16_t seg, const uint16_t off, const Access acc,
                    GuestSpan& span)
{
	if (!selector_mode()) {
		span.base  = (static_cast<PhysPt>(seg) << 4) + off;
		span.avail = 0x10000u - off;
		return true;
	}
	if ((seg & 0xfffc) == 0) {
		return false; // null selector
	}
	Descriptor desc;
	if (!cpu.gdt.GetDescriptor(seg, desc)) {
		return false;
	}
	if (!desc.saved.seg.p) {
		return false;
	}
	const uint8_t type = desc.Type();
	if (!(type & 0x10)) {
		return false; // system descriptor
	}
	const bool is_code = (type & 0x08) != 0;
	if (is_code) {
		if (acc == Access::Write || !(type & 0x02)) {
			return false; // code is never writable; must be readable
		}
	} else {
		if (type & 0x04) {
			return false; // expand-down data segment
		}
		if (acc == Access::Write && !(type & 0x02)) {
			return false; // read-only data segment
		}
	}
	const uint32_t limit = desc.GetLimit();
	if (off > limit) {
		return false;
	}
	span.base  = desc.GetBase() + off;
	span.avail = limit - off + 1;
	return true;
}

bool resolve_len(const uint16_t seg, const uint16_t off,
                        const uint32_t len, const Access acc, PhysPt& lin)
{
	GuestSpan span;
	if (!resolve(seg, off, acc, span) || span.avail < len) {
		return false;
	}
	lin = span.base;
	return true;
}

} // namespace GeosGuestMem

#endif // C_GEOSHOST
