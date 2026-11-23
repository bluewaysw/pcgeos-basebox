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
 *   out: ax = HIF_OK (request understood), si = HIF_FS_MAGIC, and
 *          bx = HIF_FS_EXECUTED: done, results, CF and extended error
 *               are in the frame
 *          bx = HIF_FS_NOT_EXECUTED: the guest must issue a real INT 21h
 *               (also for requests the host can't take, e.g. a bad frame)
 *   ax only ever carries a HostIfResults code, as HostIfCall interprets
 *   it (HIF_PENDING); the file system outcome is in bx.
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
 * Long file names (API version 2, HIF_FS_OPT_LFN)
 *
 *   Negotiation
 *     The guest asks for LFN by passing HIF_FS_OPT_LFN to HIF_FS_ENABLE,
 *     and only if HostIfDetect reported version >= 2. The host then sets
 *     the bit for AH = 71h in the function map. That bit stands for the
 *     whole set below, not for single subfunctions. Without the option
 *     the host behaves exactly as in version 1 (bit 71h clear, change
 *     notifications by DOS short name). The guest uses LFN only if the
 *     bit is set; GEOS.INI [os2] lfn = false keeps it from asking.
 *
 *   Subfunctions (INT 21h AX = 71xxh, register usage as in MS-DOS 7)
 *     7139 mkdir, 713A rmdir, 713B chdir, 7141 delete (SI = 0 only),
 *     7143 get/set attributes (BL = 0, 1), 7147 get current directory,
 *     714E find first, 714F find next, 71A1 find close, 7156 rename,
 *     7160 full path (CL = 0 canonical, 1 short, 2 long),
 *     716C create/open, 71A0 volume information.
 *     Handles returned by 716C are ordinary DOS handles: every other
 *     handle call (3Eh, 3Fh, 40h, 42h, 57h, 5Ch, 68h ...) works on them
 *     unchanged, through the fast path or a real INT 21h.
 *
 *   Execution
 *     For AX = 71xxh the host never answers HIF_FS_NOT_EXECUTED for a
 *     well-formed frame: it either executes the call or fails it with a
 *     DOS error (CF set, extended error in the frame), because the
 *     regular INT 21h path has no LFN support. Unknown subfunctions fail
 *     with AX = 7100h and extended error 1, which is what the guest's
 *     fast path hands back (CF set, error 1).
 *     Calls on drives without LFN support (see HFDF_LFN: disk and CD-ROM
 *     images, Z:) are executed with 8.3 semantics through the DOS core,
 *     find included (fileName then holds the short name), so a long name
 *     simply isn't found there. 71A0 fails with error 15 for them.
 *
 *   Names on the wire
 *     Long name fields (paths passed in DS:DX / DS:SI / ES:DI, the
 *     W32FD fileName field, 7147 and 7160 CL = 0/2 results) use the GEOS
 *     character set, not the DOS code page. The guest skips its code page
 *     conversion for them; the host converts with the same functions the
 *     host file system (hostfs_volume.h) uses, so a GEOS name maps to the
 *     same host name through both drivers.
 *     Short names (W32FD alternateFileName, 7160 CL = 1 results) are the
 *     DOS short names of DOSBox's directory cache, as raw DOS bytes:
 *     exactly what DOS programs see through INT 21h.
 *
 *   Names shown to GEOS
 *     The fileName of a found entry is the host name as GEOS shows it:
 *     decoded, and at most HOST_FS_LFN_GEOS_NAME_MAX characters (longer
 *     names are cut and get "~" plus a hash of the full host name,
 *     keeping the extension) - the hostfs naming rules.
 *     Path components passed in may be any of: that GEOS name, the full
 *     host name (decoded), or the DOS short name. Names starting with '.'
 *     are not listed; @DIRNAME.000 is (the DOS IFS driver keeps its
 *     directory names there). Relative paths start at DOS's current
 *     directory of the drive. Find also returns "." and ".." in
 *     subdirectories, as DOS does.
 *     Host entries that DOSBox's DOS core cannot open (on Windows: names
 *     not representable in the ANSI code page) are not listed.
 *
 *   Find
 *     714E returns a host side search handle in AX (no DTA is used,
 *     1Ah is not needed). The handle is valid until 71A1 or until the
 *     search is exhausted; at most HOST_FS_LFN_MAX_SEARCHES are open at
 *     once, the oldest is dropped beyond that. SI = 1 (DOS date/time) is
 *     the only supported time format.
 *
 *   Volume information (71A0)
 *     Reports flags 4006h (case preserving, Unicode on disk, LFN API),
 *     max name 255, max path 260, file system name "BASEBOX", and fails
 *     with error 15 (invalid drive) for drives without HFDF_LFN.
 *
 *   Change notifications
 *     With HIF_FS_OPT_LFN, HIF_FS_POLL_CHANGES reports names as GEOS
 *     names (GEOS character set, rules above) and computes the ID over
 *     the long path, as the guest does in LFN mode with
 *     DOSFileChangeCalculateID.
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
constexpr uint16_t HIF_API_FS_VERSION = 2;

/** First API version that offers HIF_FS_OPT_LFN. */
constexpr uint16_t HIF_API_FS_VERSION_LFN = 2;

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
constexpr uint16_t HIF_FS_EXECUTED     = 1;
constexpr uint16_t HIF_FS_NOT_EXECUTED = 0;

/** Notification type delivered through the event interrupt. */
constexpr uint16_t HIF_NOTIFY_DRIVE_CHANGE = 3;

/** Notification type: host file changes are queued for HIF_FS_POLL_CHANGES. */
constexpr uint16_t HIF_NOTIFY_FS_CHANGES = 4;

/** HIF_FS_ENABLE options (di). */
constexpr uint16_t HIF_FS_OPT_DRIVE_NOTIFY = 0x0001; // send drive change events
constexpr uint16_t HIF_FS_OPT_FILE_NOTIFY  = 0x0002; // watch for host file changes
constexpr uint16_t HIF_FS_OPT_LFN          = 0x0004; // long file names (v2)

/** Long file name protocol constants (HIF_FS_OPT_LFN). */
constexpr uint8_t HOST_FS_LFN_FUNCTION        = 0x71; // AH of all LFN calls
constexpr uint16_t HOST_FS_LFN_MAX_PATH       = 260;  // incl. drive and NUL
constexpr uint16_t HOST_FS_LFN_MAX_NAME       = 255;
constexpr uint16_t HOST_FS_LFN_GEOS_NAME_MAX  = 32;   // FILE_LONGNAME_LENGTH
constexpr uint16_t HOST_FS_LFN_MAX_SEARCHES   = 64;
constexpr uint16_t HOST_FS_LFN_FIND_DATA_SIZE = 318;  // WIN32_FIND_DATA, OEM
constexpr uint16_t HOST_FS_LFN_VOLUME_FLAGS   = 0x4006;

/** Size of the guest register frame in bytes (10 words). */
constexpr uint16_t HOST_FS_FRAME_SIZE = 20;

/** Flags returned by HIF_FS_GET_DRIVE_INFO. */
enum HostFsDriveFlags : uint16_t {
	HFDF_PRESENT   = 0x0001,
	HFDF_READ_ONLY = 0x0002,
	HFDF_REMOVABLE = 0x0004,
	HFDF_REMOTE    = 0x0008,
	HFDF_DEFAULT   = 0x0010, // current DOS default drive
	HFDF_LFN       = 0x0020, // LFN calls supported (directory mounts)
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

/**
 * Call from the tick handler; checks watched directories once a second.
 * @return true if changes became pending (the queue was empty before),
 *         so the caller can send HIF_NOTIFY_FS_CHANGES
 */
bool GEOSHOST_FS_Tick();

#endif // DOSBOX_GEOSHOST_FS_H
