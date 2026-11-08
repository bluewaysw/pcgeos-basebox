// SPDX-License-Identifier: GPL-2.0-or-later
/**
 * @file geoshost_hostfs.h
 * @brief GEOS host interface, host file system sub-API (HIF_API_HOSTFS = 5).
 *
 * Serves host directories as live, writable GEOS drives to the GEOS
 * driver hostfs.geo. The driver forwards each file system request as a
 * HostFsFrame; the host does the work (see hostfs_volume.h) and reads or
 * writes guest memory directly for paths, data buffers and extended
 * attribute descriptors.
 *
 * Which host directories become drives is configured on the GEOS side,
 * in GEOS.INI; the driver mounts each one with HOSTFS_MOUNT:
 *   [hostfs]
 *   drives = {
 *   HOST=C:\Users\me\GeosData
 *   WORK=/home/me/work
 *   }
 *
 * Optional host-side restriction (environment):
 *   BASEBOX_GEOS_HOSTFS_ALLOW  ';'-separated host directories; if set,
 *                              only these and their subdirectories may
 *                              be mounted. If not set, GEOS.INI decides.
 *
 * Changes made on the host
 *   The directories GEOS reads are watched; about once a second the host
 *   compares them with their last state (GEOSHOST_HOSTFS_Tick). When changes
 *   become pending, HIF_NOTIFY_HOSTFS_CHANGES is sent through the event
 *   interrupt; the HostIf library calls the driver's registered callback,
 *   which collects them with HFF_POLL_CHANGES and passes them to
 *   FSDGenerateNotify, so folder windows update.
 *
 * Port commands (6 words in: command, si, bx, cx, dx, di; out: ax, si,
 * bx, cx, dx, di):
 *   HOSTFS_INIT      unmounts everything (closing any files left open)
 *                    out: ax = HIF_OK, si = HOSTFS_MAGIC, bx = API version
 *   HOSTFS_MOUNT     in:  bx:cx = "NAME=HOST PATH" (one GEOS.INI line),
 *                         dx:di = buffer for the drive name, si = its size
 *                    out: ax = HIF_OK, si = volume number,
 *                         cx:dx = root directory ID (also the disk ID)
 *                         ax = HIF_NOT_FOUND if the line is malformed,
 *                         the directory is missing or not allowed
 *   HOSTFS_CALL      in:  bx:cx = HostFsFrame, dx = frame size
 *                    out: ax = HIF_OK (frame updated) / HIF_FAILED
 *
 * HostFsFrame (all words little endian; must match hostfsConstant.def):
 *    0 HFF_func        HostFsFunction
 *    2 HFF_result      out: FileError, 0 = success
 *    4 HFF_ax .. 18 HFF_es   register image (in and out)
 *   20 HFF_dirId       dword, directory relative paths start from
 *   24 HFF_handle      host file handle (handle ops)
 *   26 HFF_disk        disk handle      (FEA_DISK)
 *   28 HFF_pathInfo    DirPathInfo      (FEA_PATH_INFO)
 *   30 HFF_driveStatus DriveExtendedStatus (FEA_DRIVE_STATUS)
 *   32 HFF_aux         dword, function specific (e.g. move destination)
 *   36 HFF_auxId       dword, function specific (e.g. destination dir ID)
 *   40 HFF_notifyType  out: FileChangeNotificationType + 1, 0 = none
 *   42 HFF_notifyId    out: dword ID for the notification
 *   46 HFF_notifyName  out: 36 bytes, name for FCNT_CREATE / FCNT_RENAME
 *   82 HFF_notify2Type out: second notification (no name), 0 = none
 *   84 HFF_notify2Id   out: dword
 *   88 HFF_volume      volume number (from HOSTFS_MOUNT); ignored for
 *                      handle operations, whose handles are global
 *   90 (size)
 */

#ifndef DOSBOX_GEOSHOST_HOSTFS_H
#define DOSBOX_GEOSHOST_HOSTFS_H

#include <cstdint>

constexpr uint8_t HIF_API_HOSTFS          = 5;
constexpr uint16_t HIF_API_HOSTFS_VERSION = 1;

constexpr uint16_t HOSTFS_BASE     = 1400;
constexpr uint16_t HOSTFS_INIT     = HOSTFS_BASE + 0;
constexpr uint16_t HOSTFS_MOUNT    = HOSTFS_BASE + 1;
constexpr uint16_t HOSTFS_CALL     = HOSTFS_BASE + 2;
constexpr uint16_t HOSTFS_END      = 1499;

constexpr uint16_t HOSTFS_MAGIC      = 0x4648; // 'HF'
constexpr uint16_t HOSTFS_FRAME_SIZE = 90;
constexpr size_t HOSTFS_MAX_VOLUMES   = 8;

/** Functions carried in HFF_func; register usage as documented. */
enum HostFsFunction : uint16_t {
	// Paths. In: ds:dx = path, HFF_dirId = base for relative paths.
	HFF_CUR_PATH_SET = 1, // out: cx:dx = directory ID
	HFF_OPEN,             // in: al = FullFileAccessFlags
	                      // out: ax = host handle, cx:dx = file ID
	HFF_CREATE,           // in: al = access, cl = FileAttrs, ch = FileCreateFlags
	                      // out: ax = host handle, cx:dx = file ID
	HFF_CREATE_DIR,
	HFF_DELETE_DIR,
	HFF_DELETE_FILE,
	HFF_RENAME,           // in: bx:cx = new name
	HFF_MOVE,             // in: HFF_aux = dest path fptr, HFF_auxId = dest base dir
	HFF_GET_ATTRS,        // out: cx = FileAttrs
	HFF_SET_ATTRS,        // in: cx = FileAttrs
	HFF_PATH_GET_EXT,     // in: ax = attr, es:di = buffer, cx = size or count
	HFF_PATH_SET_EXT,     // in: ax = attr, es:di = buffer, cx = size or count
	HFF_PATH_GET_ALL_SIZE, // out: ax = bytes needed, cx = count
	HFF_PATH_GET_ALL_FILL, // in: es = block; out: cx = count

	// Open files. In: HFF_handle.
	HFF_READ = 32,        // in: ds:dx = buffer, cx = count; out: ax = read
	HFF_WRITE,            // in: ds:dx = buffer, cx = count; out: ax = written
	HFF_POSITION,         // in: al = FilePosMode, cx:dx = offset; out: dx:ax
	HFF_TRUNCATE,         // in: cx:dx = size
	HFF_COMMIT,
	HFF_GET_DATE_TIME,    // out: cx = FileTime, dx = FileDate
	HFF_SET_DATE_TIME,    // in: cx = FileTime, dx = FileDate
	HFF_FILE_SIZE,        // out: dx:ax
	HFF_ADD_REFERENCE,
	HFF_CHECK_DIRTY,      // out: ax = non-zero if dirty
	HFF_CLOSE,            // out: ax = non-zero if really closed, cx:dx = file ID
	HFF_GET_FILE_ID,      // out: cx:dx
	HFF_CHECK_NATIVE,     // in: ch = FileCreateFlags; out: ax = non-zero if it matches
	HFF_HANDLE_GET_EXT,   // as HFF_PATH_GET_EXT
	HFF_HANDLE_SET_EXT,   // as HFF_PATH_SET_EXT
	HFF_HANDLE_GET_ALL_SIZE,
	HFF_HANDLE_GET_ALL_FILL,

	// Enumeration of HFF_dirId.
	HFF_ENUM_BEGIN = 64,  // out: ax = enum handle (ERROR_NO_MORE_FILES if empty)
	HFF_ENUM_NEXT,        // in: ax = enum handle, ds = FileEnumCallbackData
	HFF_ENUM_END,         // in: ax = enum handle

	// Volume.
	HFF_DISK_INFO = 80,   // out: dx:ax = free bytes, cx:bx = total bytes
	HFF_POLL_CHANGES,     // next change made on the host (not by GEOS) as
	                      // a notification in the frame;
	                      // ERROR_NO_MORE_FILES if there is none
};

/** Executes one command. Always fills all 6 response words. */
void GEOSHOST_HOSTFS_HandleCommand(const uint16_t cmd[6], uint16_t resp[6]);

/** Notification type: host changes are queued for HFF_POLL_CHANGES. */
constexpr uint16_t HIF_NOTIFY_HOSTFS_CHANGES = 5;

/**
 * Call from the tick handler (once per emulated millisecond). About once a
 * second, checks the directories GEOS uses for changes made on the host
 * and queues them for HFF_POLL_CHANGES.
 * @return true if this check queued new changes, so the caller can send
 *         HIF_NOTIFY_HOSTFS_CHANGES
 */
bool GEOSHOST_HOSTFS_Tick();

#endif // DOSBOX_GEOSHOST_HOSTFS_H
