// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file geoshost_fs.h
 * @brief GEOS host interface, file system sub-API (HIF_API_FS = 4).
 *
 * Fast path for DOS file system calls issued by the GEOS DOS IFS driver
 * (os2.geo). Requests are DOS-API compatible register frames that are
 * executed directly by DOSBox's DOS core (DOS_OpenFile, DOS_ReadFile,
 * DOS_FindFirst ...). Handles, current directories, drive mappings and
 * error codes are therefore shared 1:1 with the regular INT 21h path,
 * and the two can be freely mixed.
 *
 * Guest pointers are interpreted according to the CPU mode at the time of
 * the call: real/V86 mode uses segment:offset, protected mode uses
 * GDT/LDT selector:offset with present/type/limit checks.
 *
 * Port protocol (6 command words, see geoshost.cpp):
 *   word 0 = command, 1 = si, 2 = bx, 3 = cx, 4 = dx, 5 = di
 * Response (6 words): 0 = ax, 1 = si, 2 = bx, 3 = cx, 4 = dx, 5 = di
 *
 * HIF_FS_ENABLE
 *   in:  si = PSP of the GEOS process (segment, or selector under DPMI)
 *        bx:cx = buffer receiving a 256-bit map of handled AH values
 *        dx = buffer size (>= 32)
 *        di = options (HIF_FS_OPT_*)
 *   out: ax = HIF_OK, si = HIF_FS_MAGIC, bx = API version,
 *        cx = current drive generation
 *
 * HIF_FS_INT21
 *   in:  bx:cx = HostFsFrame, dx = frame size (>= HOST_FS_FRAME_SIZE)
 *   out: ax = HIF_OK and si = HIF_FS_MAGIC when the call was executed
 *        (results, CF and extended error are in the frame), or
 *        ax = HIF_FS_NOT_HANDLED when the guest must issue a real INT 21h.
 *
 * HIF_FS_GET_DRIVE_INFO
 *   in:  si = drive (0 = A:)
 *   out: ax = HIF_OK / HIF_NOT_FOUND, si = HostFsDriveFlags,
 *        bx = DosDriveType, cx = bytes per sector,
 *        dx = alias drive (0xFFFF = none), di = media byte
 *
 * HIF_FS_GET_DRIVE_GENERATION
 *   out: ax = HIF_OK, si = generation (incremented on mount/unmount/swap)
 *
 * HIF_FS_POLL_CHANGES (needs HIF_FS_OPT_FILE_NOTIFY at enable time)
 *   in:  bx:cx = buffer for a name, dx = its size
 *   out: ax = HIF_OK and the next change made on the host:
 *          si = FileChangeNotificationType, bx = drive (0 = A:),
 *          cx:dx = ID as the DOS IFS driver computes it
 *          (DOSFileChangeCalculateID), name in the buffer for FCNT_CREATE
 *        ax = HIF_NOT_FOUND if there is none
 *
 * Host change detection: the DOS directories GEOS uses through the fast
 * path (on directory mounts) are watched. About once a second they are
 * compared with the host directory; differences are added to or removed
 * from DOSBox's directory cache (so DOS sees files created on the host
 * at all) and queued as notifications. Changes GEOS makes through the
 * fast path are taken as known and not reported again.
 */

#ifndef DOSBOX_GEOSHOST_FS_H
#define DOSBOX_GEOSHOST_FS_H

#include <cstdint>

constexpr uint8_t HIF_API_FS          = 4;
constexpr uint16_t HIF_API_FS_VERSION = 1;

constexpr uint16_t HIF_FS_BASE                 = 1300;
constexpr uint16_t HIF_FS_ENABLE               = HIF_FS_BASE + 0;
constexpr uint16_t HIF_FS_INT21                = HIF_FS_BASE + 1;
constexpr uint16_t HIF_FS_GET_DRIVE_INFO       = HIF_FS_BASE + 2;
constexpr uint16_t HIF_FS_GET_DRIVE_GENERATION = HIF_FS_BASE + 3;
constexpr uint16_t HIF_FS_POLL_CHANGES         = HIF_FS_BASE + 4;
constexpr uint16_t HIF_FS_END                  = 1399;

/** Returned in response word 1 to confirm a handled FS call. */
constexpr uint16_t HIF_FS_MAGIC = 0x5346; // 'FS'

/** Result code: request not executed, guest must use the regular path. */
constexpr uint16_t HIF_FS_NOT_HANDLED = 8;

/** Notification type delivered through the event interrupt. */
constexpr uint16_t HIF_NOTIFY_DRIVE_CHANGE = 3;

/** HIF_FS_ENABLE options (di). */
constexpr uint16_t HIF_FS_OPT_DRIVE_NOTIFY = 0x0001; // send drive change events
constexpr uint16_t HIF_FS_OPT_FILE_NOTIFY  = 0x0002; // watch for host file changes

/** Size of the guest register frame in bytes (10 words). */
constexpr uint16_t HOST_FS_FRAME_SIZE = 20;

/** Flags returned by HIF_FS_GET_DRIVE_INFO. */
enum HostFsDriveFlags : uint16_t {
	HFDF_PRESENT   = 0x0001,
	HFDF_READ_ONLY = 0x0002,
	HFDF_REMOVABLE = 0x0004,
	HFDF_REMOTE    = 0x0008,
	HFDF_DEFAULT   = 0x0010, // current DOS default drive
};

/** Executes one FS command. Always fills all 6 response words. */
void GEOSHOST_FS_HandleCommand(const uint16_t cmd[6], uint16_t resp[6]);

/**
 * Detects changes in the DOSBox drive table. Cheap enough to be called
 * from the tick handler.
 * @return true if the table changed since the previous call and the
 *         guest enabled HIF_FS_OPT_DRIVE_NOTIFY
 */
bool GEOSHOST_FS_PollDriveChange(uint16_t& generation);

/** Call from the tick handler; checks watched directories once a second. */
void GEOSHOST_FS_Tick();

#endif // DOSBOX_GEOSHOST_FS_H
