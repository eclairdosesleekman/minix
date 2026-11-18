/*
 * Regression test for stale TLBs during amd64 kernel-mediated IPC copyout.
 *
 * Initialize a page-aligned IPC buffer before fork so the parent and child
 * initially share its writable private page through COW.  The child sends
 * PM_GETPID without modifying the buffer.  When PM's reply is delivered,
 * the kernel must copy the reply back to that COW page, ask VM to resolve
 * the write fault, and retry the copy with the refreshed mapping.
 *
 * On SMP amd64, VM marks the inhibited process for a TLB refresh after
 * changing its mapping.  switch_to_user() must refresh before deferred
 * message delivery; doing it only just before returning to user mode is
 * too late because delivermsg() copies the reply while still in the kernel.
 */
#include <sys/wait.h>

#include <minix/callnr.h>
#include <minix/com.h>
#include <minix/ipc.h>

#include <unistd.h>

#include "common.h"

static message ipc_msg __attribute__((aligned(4096)));

static int
run_child(void)
{
	int r;

	/* A broken retry can repeatedly fault or terminate this child. */
	alarm(15);
	r = ipc_sendrec(PM_PROC_NR, &ipc_msg);
	if (r != 0)
		return 1;
	if (ipc_msg.m_type != getpid())
		return 2;
	if (ipc_msg.m_pm_lc_getpid.parent_pid != getppid())
		return 3;
	alarm(0);

	return 0;
}

int
main(void)
{
	pid_t pid;
	int i, status;

	start(102);

	memset(&ipc_msg, 0, sizeof(ipc_msg));
	ipc_msg.m_type = PM_GETPID;

	/*
	 * Each child starts from the parent's unchanged message page.  Its
	 * kernel-delivered reply therefore has to trigger a fresh COW mapping.
	 */
	for (i = 0; i < 32; i++) {
		pid = fork();
		if (pid < 0)
			e(1);
		if (pid == 0)
			_exit(run_child());

		if (waitpid(pid, &status, 0) != pid)
			e(2);
		if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
			efmt("COW IPC reply failed at iteration %d (status 0x%x)",
			    i, status);

		/* The child's reply must not modify the parent's COW page. */
		if (ipc_msg.m_type != PM_GETPID)
			efmt("child modified parent's IPC buffer at iteration %d", i);
	}

	quit();
	return 0;
}
