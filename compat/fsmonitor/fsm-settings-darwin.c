#include "cache.h"
#include "config.h"
#include "repository.h"
#include "fsmonitor-settings.h"
#include "fsmonitor.h"
#include "fsmonitor-ipc.h"
#include <sys/param.h>
#include <sys/mount.h>

/*
 * Remote working directories are problematic for FSMonitor.
 *
 * The underlying file system on the server machine and/or the remote
 * mount type (NFS, SAMBA, etc.) dictates whether notification events
 * are available at all to remote client machines.
 *
 * Kernel differences between the server and client machines also
 * dictate the how (buffering, frequency, de-dup) the events are
 * delivered to client machine processes.
 *
 * A client machine (such as a laptop) may choose to suspend/resume
 * and it is unclear (without lots of testing) whether the watcher can
 * resync after a resume.  We might be able to treat this as a normal
 * "events were dropped by the kernel" event and do our normal "flush
 * and resync" --or-- we might need to close the existing (zombie?)
 * notification fd and create a new one.
 *
 * In theory, the above issues need to be addressed whether we are
 * using the Hook or IPC API.
 *
 * So (for now at least), mark remote working directories as
 * incompatible unless fsmonitor.allowRemote is true.
 *
 */
static enum fsmonitor_reason check_volume(struct repository *r)
{
	struct statfs fs;

	if (statfs(r->worktree, &fs) == -1) {
		int saved_errno = errno;
		trace_printf_key(&trace_fsmonitor, "statfs('%s') failed: %s",
				 r->worktree, strerror(saved_errno));
		errno = saved_errno;
		return FSMONITOR_REASON_ERROR;
	}

	trace_printf_key(&trace_fsmonitor,
			 "statfs('%s') [type 0x%08x][flags 0x%08x] '%s'",
			 r->worktree, fs.f_type, fs.f_flags, fs.f_fstypename);

	if (!(fs.f_flags & MNT_LOCAL)
		&& (fsm_settings__get_allow_remote(r) < 1))
			return FSMONITOR_REASON_REMOTE;

	return FSMONITOR_REASON_OK;
}

/*
 * For the builtin FSMonitor, we create the Unix domain socket (UDS)
 * for the IPC in the .git directory by default or $HOME if
 * fsmonitor.allowRemote is true.  If the directory is remote,
 * then the socket will be created on the remote file system. This
 * can fail if the remote file system does not support UDS file types
 * (e.g. smbfs to a Windows server) or if the remote kernel does not
 * allow a non-local process to bind() the socket.
 *
 * Therefore remote UDS locations are marked as incompatible.
 *
 * FAT32 and NTFS working directories are problematic too.
 *
 * These Windows drive formats do not support Unix domain sockets, so
 * mark them as incompatible for the location of the UDS file.
 *
 */
static enum fsmonitor_reason check_uds_volume(void)
{
	struct statfs fs;
	struct strbuf path = STRBUF_INIT;
	const char *ipc_path = fsmonitor_ipc__get_path();
	strbuf_add(&path, ipc_path, strlen(ipc_path));

	if (statfs(dirname(path.buf), &fs) == -1) {
		int saved_errno = errno;
		trace_printf_key(&trace_fsmonitor, "statfs('%s') failed: %s",
				 path.buf, strerror(saved_errno));
		errno = saved_errno;
		strbuf_release(&path);
		return FSMONITOR_REASON_ERROR;
	}

	trace_printf_key(&trace_fsmonitor,
			 "statfs('%s') [type 0x%08x][flags 0x%08x] '%s'",
			 path.buf, fs.f_type, fs.f_flags, fs.f_fstypename);
	strbuf_release(&path);

	if (!(fs.f_flags & MNT_LOCAL))
		return FSMONITOR_REASON_REMOTE;

	if (!strcmp(fs.f_fstypename, "msdos")) /* aka FAT32 */
		return FSMONITOR_REASON_NOSOCKETS;

	if (!strcmp(fs.f_fstypename, "ntfs"))
		return FSMONITOR_REASON_NOSOCKETS;

	return FSMONITOR_REASON_OK;
}

enum fsmonitor_reason fsm_os__incompatible(struct repository *r)
{
	enum fsmonitor_reason reason;

	reason = check_volume(r);
	if (reason != FSMONITOR_REASON_OK)
		return reason;

	reason = check_uds_volume();
	if (reason != FSMONITOR_REASON_OK)
		return reason;

	return FSMONITOR_REASON_OK;
}
