/**
* Copyright (C) 2017-2018 NEC Corporation
* This file is part of the VEOS.
*
* The VEOS is free software; you can redistribute it and/or
* modify it under the terms of the GNU General Public License
* as published by the Free Software Foundation; either version 2
* of the License, or (at your option) any later version.
*
* The VEOS is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public
* License along with the VEOS; if not, see
* <http://www.gnu.org/licenses/>.
*/

/**
* @file	task_signal.c
*
* @brief Handles and manages the signal request for VE process
* from Pseudo process.
*
*	This file contains the functions which provides support for signal
*	related functionalities to VE process, it provides support for signal
*	generation and delivery.
*
* @internal
* @author Signal Handling
*/

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/capability.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdarg.h>
#include "ve_hw.h"
#include "task_mgmt.h"
#include "veos_handler.h"
#include "mm_common.h"
#include "ve_mem.h"
#include "psm_comm.h"
#include "signal.h"
#include "velayout.h"
#include "task_sched.h"
#include "locking_handler.h"
#include "psm_stat.h"
#include "ptrace_req.h"
#include "ve_coredump_helper.h"

/**
* @brief Check whether caller process have permission to send
* signal request to callee process
*
* @param caller_pid Process ID of caller process
* @param callee	Pointer to VE task struct of callee process
* @param caller_uid UID of caller process
* @param sig Signal number
*
* @return return 0 on success and negative value on failure
*/
int check_kill_permission(pid_t caller_pid,
		struct ve_task_struct *callee,
		uint64_t caller_uid, int sig)
{
	struct ve_task_struct *caller = NULL;
	proc_t caller_proc_info = {0};
	proc_t callee_proc_info = {0};
	int retval = -1;

	VEOS_TRACE("Entering");

	if ((sig != (SIGRTMIN - 1)) && (sig != (SIGRTMIN - 2))) {
		VEOS_ERROR("Invalid signal request %d", sig);
		retval = -EPERM;
		goto hndl_return;
	}

	/* Get the information of /proc/## for the caller and callee pid
	 * */
	retval = psm_get_ve_proc_info(callee->pid, &callee_proc_info);
	if (-1 == retval) {
		VEOS_ERROR("Failed to get callee proc information for"
				" pid: %d", callee->pid);
		retval = -EFAULT;
		goto hndl_return;
	}
	retval = psm_get_ve_proc_info(caller_pid, &caller_proc_info);
	if (-1 == retval) {
		VEOS_ERROR("Failed to get caller proc information for"
				" pid: %d", caller_pid);
		retval = -EFAULT;
		goto hndl_return;
	}

	caller = find_ve_task_struct(caller_pid);
	if (caller == NULL) {
		VEOS_ERROR("Failed to find caller task structure");
		retval = -ESRCH;
		goto hndl_return;
	}

	/* If caller and callee belong to same thread group then
	 * signal can be dilvered
	 * */
	if (callee->group_leader == caller->group_leader) {
		VEOS_DEBUG("Caller %d Callee %d are from thread group",
				caller_pid, callee->pid);
		retval = 0;
		goto hndl_return;
	}
	/* Check if caller and callee belong to same use */
	if (caller_proc_info.euid == callee_proc_info.suid ||
			caller_proc_info.euid == callee->uid ||
			caller_uid == callee_proc_info.suid ||
			caller_uid == callee->uid) {
		VEOS_DEBUG("Caller and Current process"
				" belongs to same user");
		retval = 0;
		goto hndl_return;
	}

	/* Check if caller have CAP_KILL capability */
	if (!check_ve_proc_capability(caller_pid, CAP_KILL)) {
		VEOS_ERROR("VE process do not have resource capabiltiy");
		retval = -EPERM;
		goto hndl_return;
	}

	retval = 0;
hndl_return:
	if (caller)
		put_ve_task_struct(caller);
	return retval;
	VEOS_TRACE("Exiting");
}
/**
* @brief Update signal pending flag of VE process
*
*	This function sets "sigpending" flag of VE process if there
*	exists a signal(which is not blocked by	VE process)in signal
*	pending queue.
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
*
* @return 1 if signal pending flag is set else 0.
*/
static int recalc_sigpending(struct ve_task_struct *p_ve_task)
{
	VEOS_TRACE("Entering");

	if ((p_ve_task->pending.signal.__val[0])
		&~(p_ve_task->blocked.__val[0])) {
		VEOS_DEBUG("Signal is pending, "
				"Set the signal pending flag");
		p_ve_task->sigpending = SIGNAL_PENDING;
		VEOS_TRACE("Exiting");
		return 1;
	}
	p_ve_task->sigpending = 0;
	VEOS_TRACE("Exiting");
	return 0;
}

/**
* @brief Restore the VE process context after the execution of signal handler.
*
*	Function calculate the address of VE task's stack where the context was
*	stored before executing signal handler routine.
*	Function read the context from VE process task stack and copy it in
*	"ve_thread_struct" so that VE task can resume its execution in next
*	scheduling cycle.
*
* @param[in] ve_task_curr Pointer to "ve_task_struct"
*
* @return 0 on success, negative value on failure.
*/
int psm_restore_ve_context(struct ve_task_struct *ve_task_curr)
{
	int64_t frame_addrs;
	reg_t curr_sp;
	struct sigframe p_ve_sigframe;
	ve_dma_hdl *dh;
	ve_dma_status_t st;
	struct ve_node_struct *vnode_info;
	int retval = 0;

	VEOS_TRACE("Entering");

	memset(&p_ve_sigframe, 0, sizeof(struct sigframe));
	if (ve_task_curr->p_ve_core->curr_ve_task == ve_task_curr) {
		if (vedl_get_usr_reg(VE_HANDLE(ve_task_curr->node_id),
				VE_CORE_USR_REG_ADDR(ve_task_curr->node_id,
						ve_task_curr->core_id),
					SR11,
					&curr_sp)) {

			VEOS_FATAL("failed to get user registers");
			veos_abort("failed to get user registers");
		}
	} else {
		curr_sp = ve_task_curr->p_ve_thread->SR[11];
	}

	VEOS_DEBUG("signal handler stack address : %lx",
			curr_sp);

	/* Find the physical address of stack where the process
	 * context is stored
	 * */
	frame_addrs = __veos_virt_to_phy((curr_sp + HANDLER_STACK_FRAME),
			&(ve_task_curr->p_ve_mm->atb), NULL, NULL);
	if (0 > frame_addrs) {
		VEOS_ERROR("failed to retrieve the physical address of stack"
			" where the process context is stored");
		retval = -EFAULT;
		goto ret;
	}

	VEOS_DEBUG("Frame address : %lx", frame_addrs);

	vnode_info = VE_NODE(ve_task_curr->node_id);
	dh = vnode_info->dh;
	/* read the signal frame from VE process physical memory
	 * */
	st = ve_dma_xfer_p_va(dh, VE_DMA_VEMAA, ve_task_curr->pid,
			(uint64_t)frame_addrs, VE_DMA_VHVA, getpid()
			, (uint64_t)&p_ve_sigframe
			, (uint64_t)sizeof(struct sigframe));
	if (st != 0) {
		VEOS_ERROR("failed to read the signal frame from VE process"
				" physical memory");
		retval = -EFAULT;
		goto ret;
	}

	/* Copy the current hardware context from the sigframe
	 * */
	pthread_mutex_lock_unlock(&(ve_task_curr->ve_task_lock), LOCK,
				"failed to acquire task lock");
	memset(ve_task_curr->p_ve_thread,
			'\0', sizeof(core_user_reg_t));
	memcpy(ve_task_curr->p_ve_thread,
		&p_ve_sigframe.uc.uc_mcontext,
			sizeof(core_user_reg_t));

	ve_task_curr->usr_reg_dirty = true;

	/* Copy current lshm context */
	memcpy((void *)
		(ve_task_curr->sighand->lshm_addr + ve_task_curr->offset),
			p_ve_sigframe.lshm_area, LSHM_SZ);

	/* Restore the signal mask
	 * */
	ve_task_curr->blocked = p_ve_sigframe.uc.uc_sigmask;

	VEOS_DEBUG("Sigreturn Context PID : %d IC: %lx"
			" LR : %lx SP : %lx SR12 : %lx"
			" SR0 : %lx",
			ve_task_curr->pid,
			ve_task_curr->p_ve_thread->IC,
			ve_task_curr->p_ve_thread->SR[10],
			ve_task_curr->p_ve_thread->SR[11],
			ve_task_curr->p_ve_thread->SR[12],
			ve_task_curr->p_ve_thread->SR[0]);

	pthread_mutex_lock_unlock(&(ve_task_curr->ve_task_lock), UNLOCK,
				"failed to release task lock");
	if (p_ve_sigframe.flag) {
		VEOS_INFO("Fatal flag set, terminating ve process: %d",
				ve_task_curr->pid);
		kill(ve_task_curr->pid, p_ve_sigframe.signum);
		retval = -1;
		return retval;
	}

	if (!recalc_sigpending(ve_task_curr))
		VEOS_DEBUG("No signal pending for ve process");


	VEOS_TRACE("Exiting");
	return retval;
ret:
	ve_force_sigsegv(SIGSEGV, ve_task_curr);
	VEOS_TRACE("Exiting");
	return retval;
}

/**
* @brief Allocate sigqueue data structure for the signal generated.
*
*	This function allocates the sigqueue data structure for the signal
*	generated if it can override ve process sigpending resources limit
*	or if no. of pending signals for ve process are still less than
*	ve process sigpending resources limit, else information will be
*	dropped.
*
* @param[in] sig Signal number generated.
* @param[in] override_ve_rlimit to override process resource limit for signals.
* @param[in] ve_task_curr to update no. of pending signals for VE process.
*
* @return pointer to allocated ve_sigqueue on success and NULL in
* case of failure.
*/
struct ve_sigqueue *psm_ve_sigqueue_alloc(int sig, int override_ve_rlimit
			, struct ve_task_struct *ve_task_curr)
{
	struct ve_sigqueue *ve_q = NULL;

	VEOS_TRACE("Entering");

	ve_task_curr->sighand->ve_sigpending += 1;

	if (override_ve_rlimit || ve_task_curr->sighand->ve_sigpending <=
		ve_task_curr->sighand->rlim[RLIMIT_SIGPENDING].rlim_cur) {
		ve_q = (struct ve_sigqueue *)calloc(1,
				sizeof(struct ve_sigqueue));
		if (NULL == ve_q)
			VEOS_CRIT("failed to allocated memory for"
					" sigqueue: %s", strerror(errno));
	} else {
		VEOS_DEBUG("%d: reached RLIMIT_SIGPENDING, dropped signal"
			" info: %d", ve_task_curr->pid, sig);
	}
	if (NULL == ve_q) {
		ve_task_curr->sighand->ve_sigpending -= 1;
	} else {
		INIT_LIST_HEAD(&ve_q->list);
		ve_q->flag = 0;
		ve_q->signo = sig;
	}

	VEOS_TRACE("Exiting");
	return ve_q;
}

/**
* @brief Set process state of VE process to "RUNNING".
*
*	This function  sets the state of VE process to RUNNING, if MONC
*	processing is not in progress.
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
*/
void psm_start_ve_process(struct ve_task_struct *p_ve_task)
{

	VEOS_TRACE("Entering");

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), LOCK,
				"failed to acquire task lock");
	/* Change the State of VE process */
	if (p_ve_task->vfork_state == VFORK_ONGOING) {
		VEOS_DEBUG("SIGCONT received for task %d with vfork state ongoing",
				p_ve_task->pid);
		psm_set_task_state(p_ve_task, WAIT);
	} else if ((p_ve_task->p_ve_thread->EXS & VE_EXCEPTION) &&
			(p_ve_task->block_status == BLOCK_RECVD)) {
		VEOS_DEBUG("Setting state WAIT for pid %d "
				"due to MONC processing is in progress.",
				p_ve_task->pid);
		p_ve_task->ve_task_state = WAIT;
	} else if (RUNNING != p_ve_task->ve_task_state) {
		VEOS_DEBUG("Setting task state to RUNNING for pid %d",
				p_ve_task->pid);
		psm_set_task_state(p_ve_task, RUNNING);
	}
	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK,
				"failed to release task lock");

	VEOS_TRACE("Exiting");
}

/**
* @brief Set the process state of VE process to "STOP".
*
*	This function set the VE process state to STOP. If the VE process
*	is the current task executing on VE core then re-scheduling occurs.
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
* @param[in] flag, decides which action to be taken.
* @param[out] traverse, do we need to traverse thread group again.
*/
void psm_stop_ve_process(struct ve_task_struct *p_ve_task, int flag, int *traverse)
{
	struct ve_core_struct *p_ve_core;
	reg_t regdata = 0;

	VEOS_TRACE("Entering");

	p_ve_core = VE_CORE(p_ve_task->node_id, p_ve_task->core_id);

	pthread_rwlock_lock_unlock(&(p_ve_core->ve_core_lock),
			WRLOCK, "Failed to acquire core's write lock");
	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), LOCK,
			"failed to acquire task lock");

	/* If task state is stopped at VH then update VE task state.
	 * Keep traversing thread group to stop other tasks
	 * */
	if (flag == FSTOPPROC && !is_actually_stopped(p_ve_task->pid)) {
		*traverse = 0;
		VEOS_DEBUG("No need to Set state STOP for pid %d",
				p_ve_task->pid);
		pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK,
				"failed to release task lock");
		pthread_rwlock_lock_unlock(&(p_ve_core->ve_core_lock),
				UNLOCK, "Failed to release core's write lock");
		VEOS_TRACE("Exiting");
		return;
	}
	VEOS_DEBUG("Setting state STOP for pid %d", p_ve_task->pid);
	*traverse = 1;
	psm_set_task_state(p_ve_task, STOP);

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK,
			"failed to release task lock");
	pthread_rwlock_lock_unlock(&(p_ve_core->ve_core_lock),
			UNLOCK, "Failed to release core's write lock");

	/* It may happen that the core's curr_ve_task is changed by the scheduler
	 * and hence, it doesn't need to be halted */
	pthread_mutex_lock_unlock(&p_ve_task->p_ve_mm->thread_group_mm_lock,
			LOCK, "failed to acquire thread group lock");
	if (p_ve_core->curr_ve_task == p_ve_task) {
		VEOS_DEBUG("STOP the VE core %d PID: %d",
				p_ve_core->core_num
				, p_ve_task->pid);
		/* STOP the VE core, to immediately realize STOPPING signals */
		psm_halt_ve_core(p_ve_core->node_num
				,p_ve_core->core_num, &regdata, false);
		/* Fetch latest context of VE task. If stopping of threads
		 * is invoked from coredump creation then latest context
		 * of VE thread is required
		 * */
		psm_save_current_user_context(p_ve_task);
	}
	pthread_mutex_lock_unlock(&p_ve_task->p_ve_mm->thread_group_mm_lock,
			UNLOCK, "failed to release thread group lock");

	VEOS_TRACE("Exiting");
}

/**
* @brief Performs a specific action on a thread.
*
*	The action to be performed on thread depends on the "flag"
*	received as argument. "flag" received as argument and action
*	corresponding to it are:
*	FSIGCONTINUE - If any signal whose action is to stop VE process
*	is present in signal pending queue, it should be deleted and VE
*	process state is set to RUNNING.
*	FSIGSTOPPING - Set VE process state to STOP
*	FSIGMASKING - Remove the signal number "sig" from signal pending
*	queue.
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
* @param[in] flag "flag" decides which action to be taken.
* @param[in] sig Signal number generated.
* @param[out] is_needed decides do we need to traverse again.
*
* @return 0 on success and -1 on failure.
*/
int ve_group_action(struct ve_task_struct *p_ve_task,
		int flag, int sig, int *is_needed)
{
	int retval = 0;
	sigset_t mask;
	struct ve_sigpending *pending = &p_ve_task->pending;
	struct list_head *q, *n;
	struct ve_sigqueue *tmp = NULL;

	VEOS_TRACE("Entering");

	switch (flag) {
	case FSIGCONTINUE:
		/* 1. If any stopping signal SIGTSTP/ SIGTTIN/ SIGTTOU
		 * lies in signal pending list, dequeue it and
		 * remove it for pending signal set.
		 * 2. Set the VE process state to RUNNING.
		 * */
		sigemptyset(&mask);
		mask.__val[0] = VE_SIG_STOP_MASK;
		if (list_empty(&pending->list)) {
			VEOS_DEBUG("Empty pending list");
		} else {
			/* delete the signal from sigpending list */
			list_for_each_safe(q, n, &pending->list) {
				tmp = list_entry(q,
						struct ve_sigqueue,
						list);
				if (sigismember(&mask, tmp->signo)) {
					VEOS_DEBUG(
						"Deleting stopping signal %d",
						tmp->signo);
					sigdelset(&pending->signal, tmp->signo);
					list_del(q);
					free(tmp);
				}
			}
		}
		psm_start_ve_process(p_ve_task);
		break;
	case FSIGSTOPPING:
	case FSTOPPROC:
	case FCLEANTHREAD:
		/* Set VE process state to STOP
		 * */
		psm_stop_ve_process(p_ve_task, flag, is_needed);
		break;
	case FSIGMASKING:
		/* Remove "sig" signal from signal pending list
		 * */
		if (list_empty(&pending->list)) {
			VEOS_DEBUG("Empty pending list");
		} else {
			list_for_each_safe(q, n, &pending->list) {
				tmp = list_entry(q,
						struct ve_sigqueue,
						list);
				if (tmp->signo == sig) {
					VEOS_DEBUG(
					"Deleting signal %d",
						tmp->signo);
					sigdelset(&pending->signal,
							tmp->signo);
					list_del(q);
					free(tmp);
					break;
				}
			}
		}
		break;
	default:
		VEOS_ERROR("Invalid flag provided");
		retval = -1;
		break;
	}
	VEOS_TRACE("Exiting");
	return retval;
}

/**
* @brief Read Process status for proc file system.
*
* 	This function reads process state from proc
* 	file system to verify if current task is stopped
* 	or not.
*
* @param[in] pid, whose state needs to be checked.
* @return true if process is stopped, else false.
*/
bool is_actually_stopped(pid_t pid)
{
	proc_t proc_info;
	int retval = -1;
	bool stop_status = false;

	VEOS_TRACE("Entering");

	retval = psm_get_ve_proc_info(pid, &proc_info);
	if (-1 == retval)
		goto handle_error;

	if (proc_info.state != 'T') {
		VEOS_DEBUG("task: %d state is other than stop", pid);
		goto handle_error;
	}
	stop_status = true;
	VEOS_DEBUG("Task state is stop: %d", pid);

handle_error:
	VEOS_TRACE("Exiting");
	return stop_status;
}

/**
* @brief Traverse thread group list and perform an action on each thread.
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
* @param[in] flag "flag" decides which action to be performed
* @param[in] sig Signal number generated
*/
void ve_do_group_action(struct ve_task_struct *p_ve_task, int flag, int sig)
{
	struct list_head *p, *n;
	struct ve_task_struct *group_leader = NULL, *tmp = NULL;
	int traversal_required = 0;

	VEOS_TRACE("Entering");

	group_leader = p_ve_task->group_leader;

	/* Take action of every thread of thread group
	 * */
	if (!list_empty(&group_leader->thread_group)) {
		list_for_each_safe(p, n, &group_leader->thread_group) {
			tmp = list_entry(p,
					struct ve_task_struct,
					thread_group);
			if (!((FCLEANTHREAD == flag) &&
						(tmp->pid == p_ve_task->pid))) {
				VEOS_DEBUG("Task: %d mask: %d",
						tmp->pid, flag);
				ve_group_action(tmp, flag, sig,
						&traversal_required);
				if (flag == FSTOPPROC &&
						!traversal_required) {
					VEOS_DEBUG("Break traversal");
					VEOS_TRACE("Exiting");
					return;
				}
			}
		}
	} else {
		VEOS_DEBUG("No threads exists");
	}

	/* Take action on thread group leader.
	 * */
	if (!((FCLEANTHREAD == flag) &&
				(group_leader->pid == p_ve_task->pid))) {
		VEOS_DEBUG("Task: %d mask: %d", p_ve_task->pid, flag);
		ve_group_action(group_leader, flag, sig, &traversal_required);
	}
	VEOS_TRACE("Exiting");
}

/**
* @brief Sends the generated signal to the VE process.
*
*	Function allocates and updates the VE task data structure related
*	to the signal.
*	It checks if signal generated is already pending for VE process.
*	If signal is not in the pending list, function will add the signal
*	in pending list, allocates "ve_sigqueue" data structure and recalculate
*	the signal pending flag.
*
* @param[in] ve_task_curr Pointer to "ve_task_struct"
* @param[in] ve_sig_info Stores generated signal information
* @param[in] signum Signal number generated
* @param[in] flag Set if signal generated from h/w exception.
*
* @return -1 on failure and 0 on success.
*/
int psm_send_ve_signal(struct ve_task_struct *ve_task_curr,
		siginfo_t *ve_sig_info, int signum, int flag)
{
	int retval = -1;
	struct ve_sigpending *ve_pending = &ve_task_curr->pending;
	struct ve_sigqueue *ve_q;
	int override_ve_rlimit;
	struct ve_task_struct *curr_task_on_core;

	VEOS_TRACE("Entering");

	VEOS_DEBUG("Acquiring tasklist_lock");
	pthread_mutex_lock_unlock(&(VE_NODE(0)->ve_tasklist_lock), LOCK,
			"Failed to acquire tasklist_lock lock");
	pthread_mutex_lock_unlock(&(ve_task_curr->sighand->siglock), LOCK,
			"failed to acquire signal lock");

	/* Ignore all signals except SIGINT if coredump is in progress
	 * */
	if (VE_SIGNAL_GROUP_COREDUMP ==
				ve_task_curr->sighand->signal_flag) {
		if (SIGINT == signum) {
			ve_task_curr->sighand->got_sigint = 1;
		} else {
			VEOS_DEBUG("Coredump in progress. Signal ignored");
			goto ret;
		}
	} else if (SIGCONT == signum) {
		/* Remove all stop signal from pending queue,
		 * wake all threads
		 * */
		VEOS_DEBUG("SIGCONT signal received, remove all stop signal"
			" from pending queue, wake all threads");
		ve_do_group_action(ve_task_curr, FSIGCONTINUE, signum);
	} else if (ve_sig_stop(signum)) {
		/* This is STOP signal. Remove SIGCONT from
		 * queue if any.
		 * */
		VEOS_DEBUG("stopping signal received, remove SIGCONT from"
			"queue if any");
		ve_do_group_action(ve_task_curr, FSIGMASKING, SIGCONT);
	}
	pthread_mutex_lock_unlock(&(ve_task_curr->ve_task_lock), LOCK,
			"failed to acquire task lock");
	/* Support queuing for exactly one non-rt signal(of each type)
	 * */
	if (legacy_queue(ve_pending, signum)) {
		VEOS_DEBUG("SIGNAL(non-rt) %d already in pending queue",
				signum);
		goto ret1;
	}

	/* Add signal in pending queue
	 * */
	if (signum < SIGRTMIN)
		override_ve_rlimit = (ve_sig_info <= SEND_SIG_PRIV)
					|| (ve_sig_info->si_code >= 0);
	else
		override_ve_rlimit = 0;

	VEOS_DEBUG("Allocate sigqueue struct for the signal: %d", signum);
	ve_q = psm_ve_sigqueue_alloc(signum, override_ve_rlimit, ve_task_curr);
	if (ve_q) {
		ve_q->flag = flag;
		/* Copy the siginfo received from Kernel for this signal
		 * into "to be queued node".
		 * */
		if (SEND_SIG_PRIV == ve_sig_info) {
			ve_q->info.si_signo = signum;
			ve_q->info.si_errno = 0;
			ve_q->info.si_code = SI_KERNEL;
			ve_q->info.si_pid = 0;
			ve_q->info.si_uid = 0;
		} else {
			memcpy(&(ve_q->info),
					ve_sig_info,
					sizeof(siginfo_t));
		}
		if (flag && (SEND_SIG_PRIV != ve_sig_info)) {
			VEOS_DEBUG("Populating si_addr for hw mapped signal:"
					" %d", signum);
			ve_q->info.si_addr =
				(void *)ve_task_curr->p_ve_thread->ICE;
		}
		/* Add the signal in pending signal list
		* */
		list_add_tail(&ve_q->list, &ve_pending->list);
	}

	/* Add the signal in the signal set for this task
	 * */
	sigaddset(&ve_pending->signal, signum);

	/* Recalculate SIGNAL PENDING flag
	 * */
	if (!recalc_sigpending(ve_task_curr)) {
		VEOS_DEBUG("No signal pending");
		goto ret1;
	}

	pthread_mutex_lock_unlock(&(ve_task_curr->ve_task_lock), UNLOCK,
			"failed to release task lock");
	pthread_mutex_lock_unlock(&(ve_task_curr->sighand->siglock), UNLOCK,
			"failed to release signal lock");
        pthread_mutex_lock_unlock(&(VE_NODE(0)->ve_tasklist_lock), UNLOCK,
	                                "Failed to release tasklist_lock lock");

	if (flag && (SEND_SIG_PRIV != ve_sig_info)) {
		VEOS_DEBUG("Start VE process, fatal signal occurred");
		psm_handle_un_block_request(ve_task_curr, true, false);
	}

	if ((!flag) && (SEND_SIG_PRIV != ve_sig_info)) {
		pthread_rwlock_lock_unlock
			(&(ve_task_curr->p_ve_core->ve_core_lock), RDLOCK,
			 "failed to acquire core's read lock");
		curr_task_on_core = VE_CORE(ve_task_curr->node_id,
				ve_task_curr->core_id)->curr_ve_task;


		/* If the signal is generated for a process currently
		 * executing on VE core, we stop the VE core and add the
		 * generated signal in the pending list of the VE process.
		 * We invoke the PSM scheduler, so that the generated
		 * signal can be handled immediately.
		 * */
		if (ve_task_curr == curr_task_on_core) {
			VEOS_DEBUG("Signal generated for ve process: %d"
					" currently scheduled on core: %d"
					, ve_task_curr->pid
					, ve_task_curr->core_id);
			pthread_rwlock_lock_unlock
				(&(ve_task_curr->p_ve_core->ve_core_lock),
				UNLOCK, "failed to release core's read lock");
			psm_find_sched_new_task_on_core(
					VE_CORE(ve_task_curr->node_id,
						ve_task_curr->core_id),
					false, false);
		} else {
			VEOS_DEBUG("Signal generated for ve process: %d"
					" currently NOT scheduled on core: %d"
					, ve_task_curr->pid
					, ve_task_curr->core_id);
			pthread_rwlock_lock_unlock
				(&(ve_task_curr->p_ve_core->ve_core_lock),
				UNLOCK, "failed to release core's read lock");
		}

	}

	/* If all successful return 0 */
	retval = 0;
	VEOS_TRACE("Exiting");
	return retval;
ret1:
	pthread_mutex_lock_unlock(&(ve_task_curr->ve_task_lock), UNLOCK,
			"failed to release task lock");
ret:
	pthread_mutex_lock_unlock(&(ve_task_curr->sighand->siglock), UNLOCK,
			"failed to release signal lock lock");
        pthread_mutex_lock_unlock(&(VE_NODE(0)->ve_tasklist_lock), UNLOCK,
	                                "Failed to release tasklist_lock lock");
	VEOS_TRACE("Exiting");
	return retval;
}

/**
* @brief Prepare trampoline instruction.
*
*	This function generates sigreturn(2) instruction that will be
*	written to the VE process stack. These instructions will
*	be executed by the VE program while returning from signal
*	handler routine.
*
* @param[out] ve_sigframe Pointer to signal frame structure
*/
void psm_prepare_trampoline_ve(struct sigframe *ve_sigframe)
{
	VEOS_TRACE("Entering");

	ve_sigframe->tramp[0] = 0x462eaeae00000000;
	ve_sigframe->tramp[1] = 0x012e008e00000018;
	ve_sigframe->tramp[2] = 0x45000f0000000000;
	ve_sigframe->tramp[3] = 0x310003ae00000000;
	ve_sigframe->tramp[4] = 0x3f00000000000000;

	VEOS_TRACE("Exiting");
}

/**
* @brief Fetch next signal to be delivered from pending signal set.
*
* @param[in] pending Pending signals for the VE process
* @param[in] mask Blocking signal mask for the VE process.
*
* @return Signal number on success. Return 0 on failure.
*/
int psm_get_next_ve_signal(struct ve_sigpending *pending, sigset_t *mask)
{
	unsigned long *pending_set, *mask_set, effective_set;
	int sig = 0;

	VEOS_TRACE("Entering");

	pending_set = pending->signal.__val;
	mask_set = mask->__val;

	effective_set = *pending_set & ~(*mask_set);
	if (effective_set) {
		if (effective_set & VE_SYNCHRONOUS_MASK) {
			/* If signal is one of synchronous signal,
			 * deliver it first
			 * */
			VEOS_DEBUG("Signal lies in synchronous mask");
				effective_set &= VE_SYNCHRONOUS_MASK;
		}
		sig = ffsl(effective_set);
		VEOS_DEBUG("Signal number to be served : %d",
			sig);
	}
	VEOS_TRACE("Exiting");
	return sig;
}

/**
* @brief Fetch the signal information for the signal to be delivered.
*
*	This function updates the no of pending signals for VE process.
*	It copies the signal information for the signal to be delivered.
*
* @param[out] ve_siginfo Stores signal information
* @param[in] pending Mask of pending signals
* @param[in] mask Mask of blocked signals
* @param[out] flag "flag" is set if signal is generated from h/w exception.
* @parem[out] p_ve_task to updates the no. of pending signal for the process.
*/
void ve_collect_signal(siginfo_t *ve_siginfo,
		struct ve_sigpending *pending, int sig, int *flag,
		struct ve_task_struct *p_ve_task)
{
	struct list_head *tmphead, *tmpsave;
	struct ve_sigqueue *tmp = NULL;
	struct ve_sigqueue *first = NULL;

	VEOS_TRACE("Entering");

	/* delete the signal from sigpending list
	 * */
	list_for_each_safe(tmphead, tmpsave, &pending->list) {
		tmp = list_entry(tmphead,
				struct ve_sigqueue,
				list);
		if (tmp->signo == sig) {
			if (first)
				goto more_pending;
			first = tmp;
		}
	}

	/* Remove sig from the pending signal set
	 * */
	VEOS_DEBUG("Delete signum %d from pending sigset",
			sig);
	sigdelset(&pending->signal, sig);

	if (first) {
more_pending:
		VEOS_DEBUG("Delete signum %d from pending list",
				sig);
		list_del(&first->list);
		memcpy(ve_siginfo,
				&(first->info),
				sizeof(siginfo_t));
		memcpy(flag,
				&(first->flag),
				sizeof(int));
		p_ve_task->sighand->ve_sigpending -= 1;
		free(first);
	} else {
		/*
		 * We must have been out of queue space.
		 * */
		ve_siginfo->si_signo = sig;
		ve_siginfo->si_errno = 0;
		ve_siginfo->si_code = SI_USER;
		ve_siginfo->si_pid = 0;
		ve_siginfo->si_uid = 0;
	}
	VEOS_TRACE("Exiting");
}

/**
* @brief Dequeue the signal to be delivered from pending signal list
*
*	This function gets the next signal to be delivered and then
*	collects the information about signal to be delivered.
*
* @param[out] ve_siginfo Stores signal information
* @param[in] pending Mask of pending signals
* @param[in] mask Mask of blocked signals
* @param[out] flag "flag" is set if signal is generated from h/w exception.
* @param[out] p_ve_task task structure to update no. of pending signals.
*
* @return Signal number on success. Return 0 on failure.
*/
int psm_dequeue_ve_signal(siginfo_t *ve_siginfo,
		struct ve_sigpending *pending, sigset_t *mask, int *flag,
		struct ve_task_struct *p_ve_task)
{
	int sig = 0;

	VEOS_TRACE("Entering");
	sig = psm_get_next_ve_signal(pending, mask);
	if (sig)
		ve_collect_signal(ve_siginfo, pending, sig, flag, p_ve_task);

	VEOS_TRACE("Exiting");
	return sig;
}

/**
* @brief Check if VE process is using alternate signal stack.
*
* @param[in] current Pointer to "ve_task_struct"
*
* @return 1 if alternate signal stack is active else 0.
*/
int on_sig_stack(struct ve_task_struct *current)
{
	unsigned long sp = current->p_ve_thread->SR[11];

	VEOS_TRACE("Entering");

	VEOS_TRACE("Exiting");
	return sp > current->sas_ss_sp &&
		sp - current->sas_ss_sp <= current->sas_ss_size;
}

/**
* @brief Function calculate the VE task stack physical address for writing
*	the signal handler frame.
*
*	VE process signal handler frame (contain VE process current context)
*	can be saved while handling signal.
*	In case stack overflow occur, SIGSEGV is delivered to VE process.
*	If VE process had registered the SIGSEGV handler on alternate stack,
*	SIGSEGV handler will be execute	else VE task will terminate with core
*	dump and SIGKILL signal will be delivered to the corresponding pseudo
*	process.
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
* @param[in] signum Signal Number getting served
* @param[out] vir_frame_addr Signal Frame virtual address
*
* @return physical address of stack when successful and -errno on failure.
*/
vemaa_t ve_getframe(struct ve_task_struct *p_ve_task,
		int signum,
		vemaa_t *vir_frame_addr, int *on_altstack)
{
	int onsigstack = on_sig_stack(p_ve_task);
	int flag = p_ve_task->sighand->action[signum - 1].sa_flags;
	vemaa_t frame_phy_addrs;
	vemva_t frame_vir_addrs;

	VEOS_TRACE("Entering");

	if (!onsigstack) {
		/* Signal alternate stack not active
		 * */
		VEOS_DEBUG("Signal alternate stack inactive");

		if ((flag & SA_ONSTACK) && (p_ve_task->sas_ss_size)) {
			/* SA_ONSTACK is set, handle signal
			 * on alternate stack
			 * */
			VEOS_DEBUG("SA_ONSTACK flag set");
			frame_vir_addrs = p_ve_task->sas_ss_sp
				+ p_ve_task->sas_ss_size
				- sizeof(struct sigframe);
			*on_altstack = 1;

		} else {
			/* SA_ONSTACK not set
			 * */
			VEOS_DEBUG("Either SA_ONSTACK flag disable or"
					" size is invalid");
			frame_vir_addrs = p_ve_task->p_ve_thread->SR[11]
				- (sizeof(struct sigframe));
		}
	} else {
		/* Signal alternate stack active
		 * */
		VEOS_DEBUG("Signal alternate stack active");

		frame_vir_addrs = p_ve_task->p_ve_thread->SR[11]
			- sizeof(struct sigframe);
	}
	frame_phy_addrs = __veos_virt_to_phy(frame_vir_addrs,
			&(p_ve_task->p_ve_mm->atb), NULL, NULL);
	VEOS_DEBUG("Frame Virtual address : %p",
			(void *)(frame_vir_addrs));

	VEOS_DEBUG("Frame Physical address : %lx",
			frame_phy_addrs);
	memcpy(vir_frame_addr, &frame_vir_addrs, sizeof(uint64_t));

	VEOS_TRACE("Exiting");
	return frame_phy_addrs;
}

/**
* @brief Sets the signal handler context and saves the process
*	context onto stack
*
*	This function prepares the signal handler frame on VE process stack
*	Save the current context of VE process on VE stack
*	Set the value of VE process registers for the execution	of signal
*	handler routine.
*
* @param[in] signum Signal Number being delivered.
* @param[in] p_ve_task Pointer to "ve_task_struct"
* @param[in] ve_siginfo Stores the signal information.
* @param[in] flag "flag" is set when signal is generated from h/w exception.
*
* @return 0 in Success, -1 in Error.
*/
static int setup_ve_frame(int signum,
		struct ve_task_struct *p_ve_task,
		siginfo_t *ve_siginfo,
		int flag)
{
	/*
	 *	      STACK IMAGE
	 *
	 *	high |		|
	 *	 ^   +----------+ <--- Current SP
	 *	 |   |  Current	|
	 *	 |   |	context |
	 *	 |   |	   +	|
	 *  512KB|   |	signal  |
	 *	 |   |  handler |
	 *	 |   |arguments	|
	 *	 |   |	   +	|
	 *	 |   |Trampoline|
	 *	 v   +----------+ <--- Address of frame
	 *	 ^   |  signal	|      saving current context
	 *   512B|   |  handler |
	 *	 |   |  stack   |
	 *	 |   |	frame   |
	 *	 v   +----------+ <--- New SP(signal handler)
	 *	     |		|
	 *	low
	 */
	int ret = 0;
	vemaa_t frame_addrs;
	vemaa_t frame_vir_addr = 0;
	struct sigframe ve_sigframe;
	ve_dma_hdl *dma_handle;
	ve_dma_status_t status;
	int on_altstack = 0;
	struct ve_node_struct *vnode_info;

	VEOS_TRACE("Entering");

	memset(&ve_sigframe, '\0', sizeof(struct sigframe));

	/* Prepare sigreturn() instruction as trampoline code. */
	psm_prepare_trampoline_ve(&ve_sigframe);

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), LOCK
		, "failed to acquire task lock");

	frame_addrs = ve_getframe(p_ve_task, signum, &frame_vir_addr,
			&on_altstack);
	if (0 > frame_addrs) {
		VEOS_ERROR("Failed to fetch physical translation");
		pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
				, "failed to release task lock");
		return -EFAULT;
	}

	VEOS_DEBUG("Frame Virtual address : %p",
			(void *)(frame_vir_addr));

	VEOS_DEBUG("Frame Physical address : %lx", frame_addrs);

	memcpy(&ve_sigframe.ve_siginfo, ve_siginfo, sizeof(siginfo_t));

	ve_sigframe.flag = flag;
	ve_sigframe.signum = signum;
	/* Copy the current hardware context on the sigframe
	 * */
	memcpy(&(ve_sigframe.uc.uc_mcontext),
			p_ve_task->p_ve_thread,
			sizeof(core_user_reg_t));

	VEOS_DEBUG("Saving the current context");
	if (p_ve_task->mask_saved == true) {
		VEOS_DEBUG("Saving the \"saved_sigmask\"");
		ve_sigframe.uc.uc_sigmask = p_ve_task->ve_saved_sigmask;
	} else {
		VEOS_DEBUG("Saving the current signal mask");
		ve_sigframe.uc.uc_sigmask = p_ve_task->blocked;
	}
	ve_sigframe.uc.uc_flags = 0;
	ve_sigframe.uc.uc_link = 0;
	ve_sigframe.uc.uc_stack.ss_sp = (void *)(p_ve_task->sas_ss_sp);
	ve_sigframe.uc.uc_stack.ss_size = p_ve_task->sas_ss_size;
	ve_sigframe.uc.uc_stack.ss_flags =
			on_sig_stack(p_ve_task)
			? SS_ONSTACK : 0;

	/* Copy current lshm context */
	memcpy(ve_sigframe.lshm_area,
			(void *)(p_ve_task->sighand->lshm_addr
				+ p_ve_task->offset), LSHM_SZ);

	vnode_info = VE_NODE(p_ve_task->node_id);
	dma_handle = vnode_info->dh;

	/* Write the signal frame to VE process physical memory */
	status = ve_dma_xfer_p_va(dma_handle, VE_DMA_VHVA, getpid()
			, (uint64_t)&ve_sigframe, VE_DMA_VEMAA
			, p_ve_task->pid, (uint64_t)frame_addrs
			, (uint64_t)sizeof(struct sigframe));
	if (status != 0) {
		VEOS_ERROR("unable to write signal frame");
		pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
				, "failed to release task lock");
		return -EFAULT;
	}

	VEOS_DEBUG("Sigreturn Context PID : %d IC: %lx"
			" LR : %lx SP : %lx SR12 : %lx"
			" SR0: %lx",
			p_ve_task->pid,
			p_ve_task->p_ve_thread->IC,
			p_ve_task->p_ve_thread->SR[10],
			p_ve_task->p_ve_thread->SR[11],
			p_ve_task->p_ve_thread->SR[12],
			p_ve_task->p_ve_thread->SR[0]);

	/* SET THE SIGNAL HANDLER CONTEXT */

	/* set the instruction counter of VE process with
	 * signal handler routine address
	 */
	p_ve_task->p_ve_thread->IC = (reg_t)p_ve_task->sighand->
					action[signum - 1].sa_handler;

	/* update the value of outer register used to point the start
	 * address of calling function with the address of signal
	 * handler routine.
	 * */
	p_ve_task->p_ve_thread->SR[12] = p_ve_task->p_ve_thread->IC;

	/* set the value signal handler routine argument in SR[0],
	 * SR[1] and SR[2]
	 * */
	p_ve_task->p_ve_thread->SR[0] = signum;

	p_ve_task->p_ve_thread->SR[1] = frame_vir_addr +
		offsetof(struct sigframe, ve_siginfo);
	p_ve_task->p_ve_thread->SR[2] = frame_vir_addr +
		offsetof(struct sigframe, uc);

	/* set the link register with the return address of signal handler
	 * routine, we have set link register with the address of trampoline
	 * instruction so that when signal handler routine return it will
	 * excute sigreturn instruction written as trampoline.
	 */
	p_ve_task->p_ve_thread->SR[10] = frame_vir_addr;

	/* update the value of stack pointer SR[11] for executing the signal
	 * handler routine
	 * */
	p_ve_task->p_ve_thread->SR[11] = frame_vir_addr - HANDLER_STACK_FRAME;

	/* update the value of SR[8], if handled needs to execute on alternats
	 * stack
	 */
	if (on_altstack) {
		p_ve_task->p_ve_thread->SR[8] = p_ve_task->sas_ss_sp;
		VEOS_DEBUG("SR[8]: %lx", p_ve_task->p_ve_thread->SR[8]);
	}

	VEOS_DEBUG("signal handler stack address : %lx",
			p_ve_task->p_ve_thread->SR[11]);

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
			, "failed to release task lock");

	VEOS_TRACE("Exiting");
	return ret;
}

/**
* @brief It returns the signal handler address registered for signal "sig"
*
* @param[in] t Pointer to "ve_task_struct"
* @param[in] sig Signal number
*
* @return address of signal handler.
*/
static inline void *ve_sig_handler(struct ve_task_struct *t, int sig)
{
	return t->sighand->action[sig - 1].sa_handler;
}

/**
* @brief Send SIGSEGV to VE process
*
*	This fucntion will be used to forcefully terminate the ve process
*	when VEOS fails to get or set VE process context from stack while
*	delivering signal or handling sigreturn respectively.
*
* @param[in] signum SIGSEGV signal
* @param[in] p_ve_task Pointer to ve_task_struct
*
*/
void ve_force_sigsegv(int signum, struct ve_task_struct *p_ve_task)
{
	int blocked;

	VEOS_TRACE("Entering");

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), LOCK
			, "failed to acquire task lock");

	if (SIG_DFL != p_ve_task->sighand->action[signum - 1].sa_handler)
		p_ve_task->sighand->action[signum - 1].sa_handler = SIG_DFL;

	blocked = sigismember(&(p_ve_task->blocked), signum);
	if (blocked)
		sigdelset(&(p_ve_task)->blocked, signum);

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
			, "failed to release task lock");
	psm_send_ve_signal(p_ve_task, SEND_SIG_PRIV, signum,
			!SYNCHRONOUS_SIGNAL);

	VEOS_TRACE("Exiting");
}

/**
* @brief This fucntion is used to safely write the string into buffer
* without risk of buffer overflow.
*
* @param[out] corefname pointer to ve_corename structure.
* @param[in] Pointer to format string.
*
* @return 0 on success and -ENOMEM on failure
*/
static int ve_vsnprintf(struct ve_corename *corefname, const char *format, ...)
{
	va_list data;
	size_t free = 0, need;
	size_t size;

	VEOS_TRACE("Entering");

again:
	/* Calculate space available in corename buffer */
	free = corefname->actual_space - corefname->space_used;
	VEOS_DEBUG("Free space in buffer: %zu", free);

	if (corefname->copy_pattern) {
		free = corefname->actual_space;
retry:
		va_start(data, format);
		need = vsnprintf(corefname->core_pattern,
				free, format, data);
		VEOS_DEBUG("Request to copy buffer is served");

	} else if (corefname->use_all) {
		free = corefname->actual_space;
		va_start(data, format);
		need = vsnprintf(corefname->corename,
				free, format, data);
		VEOS_DEBUG("Request to use whole capacity of buffer is"
				" served");
	} else {
		va_start(data, format);
		need = vsnprintf(corefname->corename + corefname->space_used,
				free, format, data);
	}
	va_end(data);

	/* When we have enough space left in corename buffer then
	 * adjust the amount of used space.
	 **/
	if (need < free) {
		/*
		 * No need to update used space into corename buffer
		 * when we are just copying data into another buffer.
		 **/
		if (corefname->copy_pattern)
			return 0;

		if (corefname->use_all) {
			corefname->space_used = need;
			return 0;
		}

		corefname->space_used += need;
		return 0;
	}

	/*
	 * When we don't have enough space left in target buffer then
	 * we will calculate the size needed to be reallocated.
	 * */
	VEOS_DEBUG("Buffer space has been exhausted so need more space");
	size = corefname->actual_space + need - free + 1;
	if (corefname->copy_pattern) {
		corefname->core_pattern = realloc(corefname->core_pattern,
							size);
		if (NULL != corefname->core_pattern) {
			/* When the buffer used to store copy of corename
			 * buffer is exhausted no need to update actual_space
			 * as it is used to know the space left in corename
			 * buffer */
			free = size;
			VEOS_DEBUG("Reallocated buffer of newly requested"
				"size(copy pattern)");
			goto retry;
		}
	} else {
		corefname->corename = realloc(corefname->corename, size);
		if (NULL != corefname->corename) {
			corefname->actual_space = size;
			VEOS_DEBUG("Reallocated buffer of newly requested"
				"size");
			goto again;
		}
	}

	VEOS_CRIT("Failed to allocate memory for new size request");
	VEOS_TRACE("Exiting");
	return -ENOMEM;
}

/**
* @brief Generate the filename for coredump data by reading the proc
* interface core_pattern
*
*	This function parse the format of filename specified in proc interface
*	core_pattern file and generate a filename. This function will apend
*	the path of ve_exec before the format specified in core_pattern file
*	if the format do not start with '/'. This filename will be used for
*	coredump.It also set the access perrmission on coredump file.
*
* @param[out] ve_corefname structure stores information required during core
*	file name creation.
* @param[in] p_ve_task Pointer to ve task struct.
*
* @return 0 on success and -ve number on failure
*/
int ve_format_core_filename(struct ve_corename *ve_corefname,
		struct ve_task_struct *p_ve_task)
{
	const char *pat_ptr;
	char name[HOST_NAME_MAX] = {0};
	int pid_in_pattern = 0;
	int fd;
	int retval = 0;

	VEOS_TRACE("Entering");

	/* Open the core_pattern file */
	fd = open(CORE_FILE, O_RDONLY);
	if (-1 == fd) {
		VEOS_CRIT("veos failed to open core pattern file");
		retval = -1;
		goto hndl_ret;
	}

	ve_corefname->core_pattern = (char *)malloc(PATH_MAX);
	if (ve_corefname->core_pattern == NULL) {
		VEOS_CRIT("Failed to allocated buffer to read core pattern");
		retval = -1;
		 goto hndl_err;
	}

	memset(ve_corefname->core_pattern, '\0', PATH_MAX);
	pat_ptr = ve_corefname->core_pattern;

	/* Read the core pattern */
	retval = read(fd, ve_corefname->core_pattern, PATH_MAX);
	if (-1 == retval) {
		VEOS_ERROR("failed to read from core_pattern file: %s",
				strerror(errno));
		goto free_pattern;
	}
	retval = 0;

	/* If the first character of core patter is a pipe symbol,
	 * "core" is used as core pattern.*/
	if (*pat_ptr == '|') {
		VEOS_DEBUG("Core pattern contains pipe");
		retval = ve_vsnprintf(ve_corefname, "%s/%s",
				p_ve_task->ve_exec_path, "core");
		if (retval) {
			VEOS_ERROR("Failed to create Core Pattern(pipe)\n");
			goto free_pattern;
		}
		goto append_pid;
	}

	/* Coredump filename creation */
	while (*pat_ptr) {
		if (*pat_ptr != '%') {
			if (*pat_ptr == '\n') {
				pat_ptr++;
				continue;
			}
			retval = ve_vsnprintf(ve_corefname, "%c", *pat_ptr);
			pat_ptr++;
		} else {
			switch (*++pat_ptr) {
			/* single % at the end, drop that */
			case 0:
				goto out;
				/* Double percent, output one percent */
			case '%':
				retval = ve_vsnprintf(ve_corefname,
						"%c", *pat_ptr);
				break;
				/* pid */
			case 'p':
				retval = ve_vsnprintf(ve_corefname,
						"%d", p_ve_task->tgid);
				pid_in_pattern = 1;
				break;
				/* hostname */
			case 'h':
				gethostname(name, HOST_NAME_MAX);
				retval = ve_vsnprintf(ve_corefname,
						"%s", name);
				VEOS_DEBUG("hostname is %s",
						name);
				break;
			case 'e':
			case 'E':
			case 'c':
			case 'P':
			case 'i':
			case 'I':
			case 'u':
			case 'g':
			case 'd':
			case 's':
			case 't':
				VEOS_DEBUG("Pattern %c not supported",
						*pat_ptr);
				break;
			default:
				VEOS_ERROR("Pattern %c does not exist",
						*pat_ptr);
				break;
			}
			++pat_ptr;
		}
		if (retval)
			goto free_pattern;
	}

out:
	/* Append the execution path of ve process with core pattern
	 * formed if core pattern does not contain absolute path */
	if (('/' != ve_corefname->corename[0]) &&
			('\0' != p_ve_task->ve_exec_path[0])) {

		VEOS_DEBUG("Store core pattern for local use");
		/* Store updated corename(used to create ve core file)
		 * in core_pattern buffer so that we can play with it.
		 * */
		ve_corefname->copy_pattern = 1;
		retval = ve_vsnprintf(ve_corefname, "%s",
				ve_corefname->corename);
		ve_corefname->copy_pattern = 0;
		if (retval)
			goto free_pattern;

		VEOS_DEBUG("appending current path of ve_exec");
		ve_corefname->use_all = 1;
		/* Handle the case if execution path of ve process is '/' */
		if ('\0' != p_ve_task->ve_exec_path[1])
			retval = ve_vsnprintf(ve_corefname, "%s/%s",
				p_ve_task->ve_exec_path,
				ve_corefname->core_pattern);
		else
			retval = ve_vsnprintf(ve_corefname, "%s%s",
					p_ve_task->ve_exec_path
					, ve_corefname->corename);
		ve_corefname->use_all = 0;
		if (retval)
			goto free_pattern;
	} else {
		VEOS_DEBUG("ve_exec path is not appended, core file will"
				" form according core_pattern format");
	}

append_pid:
	/* append PID and "ve" to filename by default */
	if (!pid_in_pattern) {
		retval = ve_vsnprintf(ve_corefname, ".%d.ve"
						, p_ve_task->tgid);
	} else {
		retval = ve_vsnprintf(ve_corefname, ".%s"
						, "ve");
	}
	VEOS_DEBUG("core filename %s", ve_corefname->corename);
free_pattern:
	free(ve_corefname->core_pattern);
hndl_err:
	close(fd);
hndl_ret:
	VEOS_TRACE("Exiting");
	return retval;
}

/**
 * @brief This function receive fd of VE core file.
 *
 * @param[in] sockfd socket used to receive data.
 * @param[out] file fd to dump VE core data.
 *
 * @return ve core file fd on success, and -1 on failure
 */

int get_ve_corefile_fd(int sockfd)
{
	struct msghdr msgh;
	ssize_t size;
	int corefile_fd = -1;
	struct iovec dummy_data;
	int real_data = 0;
	struct cmsghdr *cmsg;
	unsigned char *data;
	union {
		struct cmsghdr cmh;
		char c_buffer[CMSG_SPACE(sizeof(int))];
	} control_un;

	memset(&msgh, '\0', sizeof(struct msghdr));
	memset(&dummy_data, '\0', sizeof(struct iovec));

	/* Set 'control_un' to dsescribe ancillary data that we want to
	 * receive */
	control_un.cmh.cmsg_len = CMSG_LEN(sizeof(int));
	control_un.cmh.cmsg_level = SOL_SOCKET;
	control_un.cmh.cmsg_type = SCM_RIGHTS;

	/* Set 'msg' fields to describe 'control_un' */
	msgh.msg_control = control_un.c_buffer;
	msgh.msg_controllen = sizeof(control_un.c_buffer);

	/* Set fields of 'msgh' to point to buffer used to receive(real)
	 * data read by recvmsg() */
	dummy_data.iov_base = &real_data;
	dummy_data.iov_len = sizeof(int);
	msgh.msg_iov = &dummy_data;
	msgh.msg_iovlen = 1;

	/* Receive real plus ancillary data */
	size = recvmsg(sockfd, &msgh, MSG_WAITALL);
	if (size <= 0) {
		VEOS_ERROR("Failed to receive VE core file fd");
		goto failure;
	}

	/* Get the received file descriptor (which is typically a different
	 * file descriptor number than was used in the sending process) */
	cmsg = CMSG_FIRSTHDR(&msgh);
	if ((NULL == cmsg) || (cmsg->cmsg_len != CMSG_LEN(sizeof(int)))) {
		VEOS_ERROR("Bad cmsg header / message length");
		goto failure;
	}

	if ((cmsg->cmsg_level != SOL_SOCKET) ||
		(cmsg->cmsg_type != SCM_RIGHTS)) {
		VEOS_ERROR("Bad access rights received from sender process");
		goto failure;
	}

	data = CMSG_DATA(cmsg);
	corefile_fd = *(int *)data;

	VEOS_DEBUG("VE core file fd: %d", corefile_fd);
failure:
	VEOS_TRACE("Exiting");
	return corefile_fd;
}

/**
 * @brief Thread performing coredump
 *
 *	Function deactivates all the threads in thread group
 *	Function parses the core pattern file and generate the
 *	core filename
 *	Function redirect the ELF core dump data in core filename.
 *
 * @param[in] ve_dump_info Structure contains information
 * required for coredumping
 */
void *do_ve_coredump(void *ve_dump_info)
{
	struct ve_corename ve_corefname;
	int socket_fd[2];
	pid_t helper_pid;
	int retval = -1;
	int needed;
	char *sockfd = NULL;

	int signum = ((struct dump_info *)ve_dump_info)->signum;
	struct ve_task_struct *p_ve_task =
		(((struct dump_info *)ve_dump_info)->ve_task);
	struct dump_params ve_cprm = {
		.siginfo = ((struct dump_info *)ve_dump_info)->siginfo,
		.tsk = ((struct dump_info *)ve_dump_info)->ve_task,
		.limit = ((struct dump_info *)ve_dump_info)
			->ve_task->sighand->rlim[RLIMIT_CORE],
	};

	VEOS_TRACE("Entering");

	VEOS_DEBUG("Coredumper thread ID %ld", syscall(186));
	VEOS_DEBUG("Initiating Coredump for PID %d, TGID: %d",
			p_ve_task->pid, p_ve_task->tgid);

	p_ve_task->sighand->coredumper_thid = pthread_self();

	pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS, NULL);

	pthread_mutex_lock_unlock(&(p_ve_task->sighand->del_lock), LOCK,
			"Failed to acquire thread group delete lock");

	VEOS_DEBUG("Acquiring tasklist_lock");

	pthread_mutex_lock_unlock(&(VE_NODE(0)->ve_tasklist_lock), LOCK,
			"Failed to acquire tasklist_lock lock");
	ve_do_group_action(p_ve_task, FCLEANTHREAD, signum);

	pthread_mutex_lock_unlock(&(VE_NODE(0)->ve_tasklist_lock), UNLOCK,
			"Failed to release tasklist_lock lock");

	if (ve_cprm.limit.rlim_cur == 0) {
		VEOS_DEBUG("coredump dissable");
		goto hndl_err;
	}

	/* As VE OS runs as root so it can create VE core file at a location
	 * where VE process may not have the permission to create it and
	 * vice versa. To handle this we are creating a socket and will
	 * load a core dump helper process which will open VE core file and
	 * communicate fd to VE OS using socket. VE OS eventually will dump
	 * ve core data into file using the file descriptor.
	 * */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, socket_fd) != 0) {
		VEOS_ERROR("Failed to create socket pair");
		goto hndl_err;
	}

	memset(&ve_corefname, 0, sizeof(struct ve_corename));
	ve_corefname.corename = (char *)malloc(PATH_MAX);
	if (ve_corefname.corename == NULL) {
		VEOS_CRIT("veos failed to allocate buffer for core file name"
				": %s", strerror(errno));
		close(socket_fd[0]);
		goto close_socket;
	}
	memset(ve_corefname.corename, '\0', PATH_MAX);
	ve_corefname.actual_space = PATH_MAX;

	/* Core filename creation */
	retval = ve_format_core_filename(&ve_corefname, p_ve_task);
	if (0 > retval) {
		VEOS_ERROR("Core filename creation failed");
		close(socket_fd[0]);
		goto free_corefname;
	}

	needed = snprintf(NULL, 0, "%d", socket_fd[0]);
	sockfd = (char *)malloc(needed + 1);
	if (NULL == sockfd) {
		VEOS_CRIT("Failed to allocate buffer to store socket fd\n");
		retval = -1;
		close(socket_fd[0]);
		goto free_corefname;
	}
	memset(sockfd, '\0', needed + 1);
	snprintf(sockfd, needed + 1, "%d", socket_fd[0]);

	helper_pid = fork();
	if (-1 == helper_pid) {
		VEOS_ERROR("Failed to create coredump helper process");
		close(socket_fd[0]);
		retval = -1;
		goto free_sock;
	} else if (0 == helper_pid) {
		if (-1 == setgid(p_ve_task->gid)) {
			close(socket_fd[0]);
			close(socket_fd[1]);
			_exit(1);
		}
		if (-1 == setuid(p_ve_task->uid)) {
			close(socket_fd[0]);
			close(socket_fd[1]);
			_exit(1);
		}
		close(socket_fd[1]);
		execle(HELPER_PATH, "ve_coredump_helper",
			ve_corefname.corename, sockfd, (char *)NULL,
			(char *)NULL);
		VEOS_ERROR("Failed to load ve coredump helper"
				" %s", strerror(errno));
		close(socket_fd[0]);
		_exit(1);
	}

	ve_cprm.hpid = helper_pid;
	close(socket_fd[0]);
	ve_cprm.fd = get_ve_corefile_fd(socket_fd[1]);
	if (-1 == ve_cprm.fd) {
		retval = -1;
		VEOS_ERROR("Failed to receive ve core file descriptor");
		goto free_sock;
	}

	/* Perform ELF dump(take care of the following points).
	 * a. Check whether Dump is interrupted by signal.
	 * b. Check Dump file size does not exceed the core file limit.
	 * */
	if (false == ve_elf_core_dump(&ve_cprm)) {
		VEOS_ERROR("ELF dumping failed");
		retval = -1;
		goto close_fd;
	}

	retval = 0;
close_fd:
	close(ve_cprm.fd);
free_sock:
	free(sockfd);
free_corefname:
	free(ve_corefname.corename);
close_socket:
	close(socket_fd[1]);
hndl_err:
	if (retval == -1) {
		VEOS_ERROR("coredump failed");
	}
	/* Perform cleanup of current thread */
	p_ve_task->sighand->signal_flag = VE_SIGNAL_GROUP_EXIT;

	if (SYNCHRONOUS_SIGNAL == ((struct dump_info *)ve_dump_info)->flag) {
		VEOS_DEBUG("Signal generated from h/w exception");
		kill(p_ve_task->pid, signum);
	} else {
		/* Terminating signals
		 * */
		VEOS_DEBUG("Signal Action is to Terminate");
		kill(p_ve_task->pid, SIGKILL);
	}

	free(ve_dump_info);
	pthread_mutex_lock_unlock(&(p_ve_task->sighand->del_lock), UNLOCK,
			"Failed to release thread group delete lock");
	put_ve_task_struct(p_ve_task);
	VEOS_TRACE("Exiting");
	pthread_exit(NULL);
}

/**
* @brief It verifies if task can initiate coredump.
*
* @param[in] requestor, task to initiate coredump.
*
* @return true, if task can dump core else false.
*/
bool should_dump_core(struct ve_task_struct *requestor)
{
	bool able = true;

	pthread_mutex_lock_unlock(&(requestor->ve_task_lock)
			, LOCK, "failed to acquire task lock");
	if (requestor->sighand->signal_flag == VE_SIGNAL_GROUP_COREDUMP) {
		/* If multiple coredump requestor can sprint to reach here, it
		 * might be possible that it can busy scheduler unnecessarily
		 * because of it's RUNNING state. Also multiple task of same
		 * thread group should not initiate core dumping.
		 * */
		psm_set_task_state(requestor, STOP);
		able = false;
	}
	pthread_mutex_lock_unlock(&(requestor->ve_task_lock)
			, UNLOCK, "failed to release task lock");

	return able;
}

/**
* @brief Get the signal to be delivered from pending list.
*
*	This function dequeue the signal from pending list.
*		- If action is to ignore, signal is ignored and next signal
*		is dequeued from the list.
*		- If the action is set to default then default action of
*		ignore, stop, terminate, or coredump is performed.
*		- If the signal is being handled by the process the function
*		returns with signal number
*
* @param[in] p_ve_task p_ve_task Pointer to "ve_task_struct"
* @param[out] flag Hardware exception mapped signal
* @param[out] ve_siginfo Signal information
*
* @return 0 - when no signal is pending or signal action is to ignore
*	-2 - when signal with default action is to Stop
*	-1 - when signal with default action is to terminate or
*	coredump is being handled
*	>0 - when signal is being handled by VE process
*/
int ve_get_signal(struct ve_task_struct *p_ve_task, int *flag,
		siginfo_t *ve_siginfo)
{
	int signum = 0;
	pthread_t dump_tid;
	struct dump_info *ve_dump_info;

	VEOS_TRACE("Entering");

	pthread_mutex_lock_unlock(&(p_ve_task->sighand->siglock), LOCK
			, "failed to acquire task's signal lock");

	/* Do not proceed if core dumping is ongoing */
	if (!should_dump_core(p_ve_task)) {
		VEOS_DEBUG("Core dump ongoing: %d", p_ve_task->pid);
		signum = -1;
		goto hndl_ret;
	}

	for (;;) {
		/* Dequeue the signal */
		VEOS_DEBUG("Dequeue a signal from pending list");

		signum = psm_dequeue_ve_signal(ve_siginfo,
				&p_ve_task->pending,
				&p_ve_task->blocked, flag,
				p_ve_task);
		if (!signum) {
			VEOS_DEBUG("No signal is pending list");
			p_ve_task->sigpending = 0;
			break;
		}
		/* Signal handler registered */
		if (SIG_IGN ==
			p_ve_task->sighand->action[signum - 1].sa_handler) {
			VEOS_DEBUG("Signal: %d action is ignore",
					signum);
			continue;
		}

		/* Signal handler registered */
		if (SIG_DFL !=
			p_ve_task->sighand->action[signum - 1].sa_handler) {
			VEOS_DEBUG("Signal handler registered signr: %d",
					signum);
			p_ve_task->sigpending = 0;
			break;
		}
		/* Signals whose default action is to ignore */
		if (ve_sig_ignore(signum)) {
			VEOS_DEBUG("Default action is to IGNORE signal %d",
					signum);
			continue;
		}
		/* Signal whose default action is to stop. Set the VE
		 * process state to STOP for every thread in thread group
		 * */
		if (ve_sig_stop(signum)) {
			VEOS_DEBUG("Default action is to STOP the VE process");
			ve_do_group_action(p_ve_task, FSIGSTOPPING, signum);
			signum = -2;
			p_ve_task->sigpending = 0;
			break;
		}

		/* Signal whose default action is generate dump + terminate */
		if (ve_sig_coredump(signum)) {
			VEOS_DEBUG("Generate Coredump for signal %d",
					signum);

			pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock)
					, LOCK, "failed to acquire task lock");
			p_ve_task->sighand->signal_flag =
				VE_SIGNAL_GROUP_COREDUMP;
			p_ve_task->flags |= PF_DUMPCORE;
			psm_set_task_state(p_ve_task, STOP);
			pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock)
				, UNLOCK, "failed to release task lock");

			ve_dump_info = (struct dump_info *)malloc
				(sizeof(struct dump_info));
			if (NULL == ve_dump_info) {
				VEOS_CRIT("failed to allocate memory for"
						" coredump info");
				goto hndl_terminate;
			}
			/*MEMSET*/
			memset(ve_dump_info, '\0', sizeof(struct dump_info));

			memcpy(&(ve_dump_info->siginfo), ve_siginfo,
					sizeof(siginfo_t));
			ve_dump_info->ve_task = p_ve_task;
			ve_dump_info->flag = *flag;
			ve_dump_info->signum = signum;

			VEOS_DEBUG("Creating new thread for Coredumping");
			if (0 != get_ve_task_struct(p_ve_task)) {
				VEOS_ERROR("Failed to get reference: %d",
						p_ve_task->pid);
				goto hndl_terminate;
			}

			/* Creating a detached coredumper thread */
			if (0 != pthread_create(&dump_tid, NULL,
						&do_ve_coredump,
						(void *)ve_dump_info)) {
				VEOS_DEBUG("Failed to create"
						" Coredumping"
						" thread");
				free(ve_dump_info);
				put_ve_task_struct(p_ve_task);
				goto hndl_terminate;
			}
			goto hndl_exit;
		}
hndl_terminate:
		/* Terminate the process */
		if (SYNCHRONOUS_SIGNAL == *flag) {
			VEOS_DEBUG("Signal generated from h/w exception");
			/* updating process flag when terminated by signal */
			p_ve_task->flags |= PF_SIGNALED;

			kill(p_ve_task->pid, signum);
		} else {
			VEOS_DEBUG("Signal Action is to Terminate: %d", p_ve_task->pid);
			kill(p_ve_task->pid, SIGKILL);
		}
hndl_exit:
		signum = -1;
		goto hndl_ret;
	}
hndl_ret:
	pthread_mutex_lock_unlock(&(p_ve_task->sighand->siglock), UNLOCK
			, "failed to release task's signal lock");
	VEOS_TRACE("Exiting");
	return signum;
}

/**
* @brief Execute signal handler for VE process.
*
*	This function decides if system call interrupted
*	due to signal will restart again or not.It executes
*	the signal handler for VE process.
*
* @param[in] p_ve_task Pointer to ve_task_struct
* @param[in] flag Hardware exception flag
* @param[in] signum Signal number to be handled
* @param[in] ve_siginfo Signal information
*
* @return -1 in case of failure and 0 in case of success.
*/
int ve_handle_signal(struct ve_task_struct *p_ve_task,
		int *flag, int signum, siginfo_t *ve_siginfo)
{
	int ret = -1;
	sigset_t blocked;

	VEOS_TRACE("Entering");

	/* If system call is interrupted due to signal */
	switch (p_ve_task->p_ve_thread->SR[00]) {
	case -VE_ENORESTART:
		VEOS_DEBUG("do not restart system call");
		p_ve_task->p_ve_thread->SR[00] = -EINTR;
		break;
	case -VE_ERESTARTSYS:
		if (p_ve_task->sighand->action[signum - 1].sa_flags
				& SA_RESTART) {
			VEOS_DEBUG("Restarting syscal"
					", as SA_RESTART flag is set");
			p_ve_task->p_ve_thread->IC -= 8;
		} else {
			VEOS_DEBUG("do not restart system call");
			p_ve_task->p_ve_thread->SR[00] = -EINTR;
		}
		break;
	}

	/* Set signal handler context and save process context */
	ret = setup_ve_frame(signum, p_ve_task, ve_siginfo, *flag);
	if (0 > ret) {
		VEOS_ERROR("failed to setup signal frame");
		ve_force_sigsegv(SIGSEGV, p_ve_task);
		goto hndl_ret;
	}

	/* If "SA_RESETHAND" is set then reset the handler to SIG_DFL
	 * */
	if (p_ve_task->sighand->action[signum - 1].sa_flags
			& SA_RESETHAND) {
		VEOS_DEBUG("SA_RESETHAND flag set");
		p_ve_task->sighand->action[signum - 1].sa_handler
			= SIG_DFL;
	}
	sigemptyset(&blocked);
	sigorset(&blocked,
			&(p_ve_task->blocked),
			&(p_ve_task->sighand->action[signum - 1].sa_mask));

	/* If SA_NODEFER flag is not set, add the generated
	 * signal in block list
	 * */
	if (!(p_ve_task->sighand->action[signum - 1].sa_flags
				& SA_NODEFER)) {
		VEOS_DEBUG("SA_NODEFER flag not set");
		sigaddset(&blocked, signum);
	}

	VEOS_DEBUG("Set the mask");
	/* Clear the saved signal mask flag */
	p_ve_task->mask_saved = false;
	psm_set_current_blocked(p_ve_task, &blocked);

hndl_ret:
	VEOS_TRACE("Exiting");
	return ret;
}

/**
* @brief Deliver the generated signal.
*
*	Function delivers signal to VE process.
*	It dequeues a signal from signal pending list.
*	If then checks the signal action:
*	a. It terminates the VE process(with/without coredump) if signal
*	action is to terminate(with/without coredump).
*	b. Signal is ignored if default action is to ignore the signal.
*	c. Signal handler is executed if VE process registers a signal
*	handler for the signal.
*
*	It restarts the system call if the signal has interrupted the
*	execution of a system call of restarting nature.
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
*
* @return -1 on error or VE process is terminated due to signal
* delivery and 0 on success.
*/
int psm_do_signal_ve(struct ve_task_struct *p_ve_task)
{
	int retval = -1;
	int flag = 0;
	int signum = 0;
	siginfo_t ve_siginfo = {-1};

	VEOS_TRACE("Entering");

	/* Get the signal(dequeue the signal). */
	signum = ve_get_signal(p_ve_task, &flag, &ve_siginfo);
	if (0 < signum) {
		/* Handle Signal */
		retval = ve_handle_signal(p_ve_task, &flag, signum
							, &ve_siginfo);
		if (0 > retval) {
			VEOS_ERROR("Signal handler stack setup has failed");
			retval = -1;
		} else {
			VEOS_DEBUG("Signal handler setup is complete"
					", signal %d handler will be executed"
					"PID: %d", signum, p_ve_task->pid);
			retval = 0;
		}
	} else if ((0 == signum) || (-2 == signum)) {
		/* Check if the signal interrupted the system call */
		switch (p_ve_task->p_ve_thread->SR[00]) {
		case -VE_ENORESTART:
		case -VE_ERESTARTSYS:
			VEOS_DEBUG("Restarting syscal, as syscall was "
					"interrupted due to signal");
			p_ve_task->p_ve_thread->IC -= 8;
			break;
		}
		/* If no signal is delivered then put the saved sigmask back */
		if (p_ve_task->mask_saved) {
			psm_set_current_blocked(p_ve_task,
					&(p_ve_task->ve_saved_sigmask));
			p_ve_task->mask_saved = false;
		}

		if (-2 == signum) {
			VEOS_INFO("VE process is stopped due to"
					" signal: %d, PID: %d",
					signum, p_ve_task->pid);
			retval = -1;
		} else {
			retval = 0;
		}
	} else {
		/* VE process has been terminated */
		VEOS_DEBUG("VE process: %d will be terminated"
				, p_ve_task->pid);
		retval = -1;
	}
	VEOS_TRACE("Exiting");
	return retval;
}

/**
* @brief veos's handler for sigaction system call
*
*	This function saves the current action for the signal and adds
*	new action for the signal.
*
* @param[in] p_ve_task_struct Pointer to "ve_task_struct"
* @param[in,out] sigaction_info Pointer to ve_signal_info containing
*	signal number and signal handler information
*
*/
void psm_do_sigaction_ve(struct ve_task_struct *p_ve_task_struct,
		struct ve_sigaction_info *sigaction_info)
{

	VEOS_TRACE("Entering");

	VEOS_DEBUG("Sigaction for signal no = %d received",
			sigaction_info->signum);

	VEOS_DEBUG("sa_mask in handler context %lx",
			sigaction_info->ve_sigaction.sa_mask.__val[0]);

	VEOS_DEBUG("Acquiring tasklist_lock");
	        pthread_mutex_lock_unlock(&(VE_NODE(0)->ve_tasklist_lock), LOCK,
		                        "Failed to acquire tasklist_lock lock");
	pthread_mutex_lock_unlock(&(p_ve_task_struct->sighand->siglock),
			LOCK, "failed to acquire task's signal lock");

	/* Save the old action */
	VEOS_DEBUG("Save the oldact for signum %d"
			, sigaction_info->signum);
	sigaction_info->old_ve_sigaction = p_ve_task_struct->sighand->
		action[sigaction_info->signum - 1];


	/* Add new action for this signal */
	if (sigaction_info->action) {
		p_ve_task_struct->sighand->
			action[sigaction_info->signum - 1] =
			sigaction_info->ve_sigaction;
		VEOS_DEBUG("New Action added for signum %d"
				, sigaction_info->signum);

		if (ve_sig_handler_ignored(ve_sig_handler(p_ve_task_struct,
						sigaction_info->signum),
					sigaction_info->signum)) {
			VEOS_DEBUG("New Action added is to ignore the signal");
			ve_do_group_action(p_ve_task_struct,
					FSIGMASKING,
					sigaction_info->signum);
		}
	}

	pthread_mutex_lock_unlock(&(p_ve_task_struct->sighand->siglock),
			UNLOCK, "failed to release task's signal lock");
        pthread_mutex_lock_unlock(&(VE_NODE(0)->ve_tasklist_lock), UNLOCK,
	                                "Failed to release tasklist_lock lock");

	VEOS_TRACE("Exiting");
}

/**
* @brief Fetches current alternate signal stack information.
*
* @param[in] ve_task_curr[in] Pointer to "ve_task_struct"
* @param[out] old_sas Stores current alternate signal stack information
*/
void psm_getold_sas(struct ve_task_struct *ve_task_curr, stack_t *old_sas)
{
	VEOS_TRACE("Entering");

	old_sas->ss_sp = (void *)ve_task_curr->sas_ss_sp;
	old_sas->ss_size = ve_task_curr->sas_ss_size;
	if (!ve_task_curr->sas_ss_size) {
		old_sas->ss_flags = SS_DISABLE;
	} else {
		old_sas->ss_flags =
			on_sig_stack(ve_task_curr)
			? SS_ONSTACK : 0;
	}
	VEOS_DEBUG("old_sas->ss_sp : %p"
			"\t\told_sas->ss_size : %ld"
			"\t\told_sas->ss_flags : %d",
			old_sas->ss_sp,
			old_sas->ss_size,
			old_sas->ss_flags);

	VEOS_TRACE("Exiting");
}

/**
* @brief Sets alternate signal stack.
*
* @param[in] ve_task_curr Pointer to "ve_task_struct"
* @param[in] new_sas Alternate signal stack information.
*
* @return: Returns 0 for success, and negative value for failure.
*/
int psm_setnew_sas(struct ve_task_struct *ve_task_curr, stack_t *new_sas)
{
	int retval = 0;

	VEOS_TRACE("Entering");

	/* Verify the new stack pointer is valid or not
	 * */
	if (-1 == __veos_virt_to_phy((uint64_t)new_sas->ss_sp,
			&(ve_task_curr->p_ve_mm->atb), NULL, NULL)) {
		VEOS_DEBUG("new_sas->ss_sp : %p", new_sas->ss_sp);
		VEOS_ERROR("Invalid Alternate stack address");
		retval = -EFAULT;
		goto hndl_ret;
	}

	/* Verify whether alernate signal stack is active
	 * */
	if (on_sig_stack(ve_task_curr)) {
		VEOS_ERROR("Attempt to change the alternate "
				"signal stack while it is active");
		retval = -EPERM;
		goto hndl_ret;
	}

	/* Verify the sas flags are valid or not
	 * */
	if (new_sas->ss_flags != SS_DISABLE &&
			new_sas->ss_flags != SS_ONSTACK &&
			new_sas->ss_flags != 0) {
		VEOS_ERROR("Invalid alternate stack flag argument");
		retval = -EINVAL;
		goto hndl_ret;
	}

	/* Verify stack size is enough to execute signal handler
	 * */
	if (new_sas->ss_flags == SS_DISABLE) {
		new_sas->ss_size = 0;
		new_sas->ss_sp = NULL;
	} else {
		if (new_sas->ss_size < VE_MINSIGSTKSZ) {
			VEOS_ERROR("New signal stack size is "
					"less than required");
			retval = -ENOMEM;
			goto hndl_ret;
		}
	}

	ve_task_curr->sas_ss_sp = (unsigned long) new_sas->ss_sp;
	ve_task_curr->sas_ss_size = new_sas->ss_size;
hndl_ret:
	VEOS_TRACE("Exiting");
	return retval;
}

/**
* @brief Set the blocked signal mask for VE process.
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
* @param[in] newset signal blocking mask
*/
void psm_set_current_blocked(struct ve_task_struct *p_ve_task,
		sigset_t *newset)
{
	VEOS_TRACE("Entering");

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), LOCK
		, "failed to acquire task lock");

	p_ve_task->blocked = *newset;
	/*Do not mask SIGKILL, SIGSTOP and SIGCONT even if user has
	 * intentionally attempted to do so.*/
	sigdelset(&(p_ve_task->blocked), SIGKILL);
	sigdelset(&(p_ve_task->blocked), SIGCONT);
	sigdelset(&(p_ve_task->blocked), SIGSTOP);
	recalc_sigpending(p_ve_task);

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
		, "failed to release task lock");
	VEOS_DEBUG("Updated ve signal mask : %lx",
			p_ve_task->blocked.__val[0]);

	VEOS_TRACE("Exiting");
}

/**
* @brief Sets new signal blocking mask and gets the old blocking signal mask
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
* @param[in] newset New signal blocking mask.
* @param[out] oldset Old signal blocking mask.
*/
void psm_do_sigprocmask(struct ve_task_struct *p_ve_task,
		struct ve_signal_mask *ve_mask)
{
	sigset_t new_blocked;
	bool update_mask;
	int how;

	VEOS_TRACE("Entering");

	sigemptyset(&new_blocked);
	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), LOCK
		, "failed to acquire task lock");

	memcpy(&(ve_mask->oldset), &(p_ve_task->blocked), sizeof(sigset_t));
	VEOS_DEBUG("Old signal mask: %lx", ve_mask->oldset.__val[0]);

	/*
	 * do not update the signal mask for task now,
	 * let psm_do_signal_ve() update it.
	 **/
	if (1 == ve_mask->store_mask) {
		memcpy(&p_ve_task->ve_saved_sigmask, &ve_mask->newset
				, sizeof(sigset_t));
		/*Do not mask SIGKILL, SIGSTOP and SIGSTOP even if user has
		 * intentionally attempted to do so.*/
		sigdelset(&(p_ve_task->ve_saved_sigmask), SIGKILL);
		sigdelset(&(p_ve_task->ve_saved_sigmask), SIGSTOP);
		sigdelset(&(p_ve_task->ve_saved_sigmask), SIGCONT);
		p_ve_task->mask_saved = true;
		pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
				, "failed to release task lock");
		goto hndl_ret;
	}

	update_mask = ve_mask->mask;
	if (update_mask) {
		memcpy(&new_blocked, &(p_ve_task->blocked), sizeof(sigset_t));
		how = ve_mask->how;
		switch (how) {
		case SIG_BLOCK:
			VEOS_DEBUG("SIG_BLOCK set");
			sigorset(&new_blocked, &new_blocked, &ve_mask->newset);
			break;
		case SIG_UNBLOCK:
			VEOS_DEBUG("SIG_UNBLOCK set");
			ve_sigandnsets(&new_blocked, &new_blocked,
					&ve_mask->newset);
			break;
		case SIG_SETMASK:
			VEOS_DEBUG("SIG_SETMASK set");
			memcpy(&new_blocked, &(ve_mask->newset),
					sizeof(sigset_t));
			break;
		}
		pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
				, "failed to release task lock");

		/* Set new signal mask */
		psm_set_current_blocked(p_ve_task, &new_blocked);
		goto hndl_ret;
	}
	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
			, "failed to release task lock");

hndl_ret:
	VEOS_TRACE("Exiting");
	return;
}

/**
* @brief Handles the h/w exception
*
*	Check whether signal mapped to h/w exception is ignored or blocked.
*	If ignored, set signal action to default. If signal is blocked by
*	VE process unblock it. Deliver signal to VE process.
*
* @param[in] ve_task_curr Pointer to "ve_task_struct"
* @param[in] signum Signal number mapped from h/w exception
*/
void psm_handle_hw_exception(struct ve_task_struct *ve_task_curr,
		siginfo_t *ve_siginfo, int signum)
{
	int blocked, ignored, flag = 0;

	VEOS_TRACE("Entering");

	ignored = (ve_task_curr->sighand->action[signum - 1].sa_handler
			== SIG_IGN);
	blocked = sigismember(&(ve_task_curr->blocked), signum);
	pthread_mutex_lock_unlock(&(ve_task_curr->ve_task_lock), LOCK
			, "failed to acquire task lock");
	if (blocked || ignored) {
		ve_task_curr->sighand->action[signum - 1].sa_handler = SIG_DFL;
		if (blocked)
			sigdelset(&(ve_task_curr)->blocked, signum);
	}
	/* Set Synchronous signal flag */
	VEOS_DEBUG("Fatal signal occurred");
	flag = SYNCHRONOUS_SIGNAL;
	ve_task_curr->syncsignal = SYNCHRONOUS_SIGNAL;

	pthread_mutex_lock_unlock(&(ve_task_curr->ve_task_lock), UNLOCK
			, "failed to release task lock");
	psm_send_ve_signal(ve_task_curr, ve_siginfo, signum, flag);

	VEOS_TRACE("Exiting");
}

/**
* @brief Fetches the signals that are present in VE process signal
* pending list and are blocked by sigprocmask system call.
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
* @param[out] veos_set Stores sigpending set.
*/
void psm_do_sigpending(struct ve_task_struct *p_ve_task,
		sigset_t *veos_set)
{
	VEOS_TRACE("Entering");

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), LOCK
			, "failed to acquire task lock");
	sigandset(veos_set, &(p_ve_task->blocked),
			&(p_ve_task->pending.signal));
	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
			, "failed to release task lock");

	VEOS_TRACE("Exiting");
}

/**
* @brief Replaces temporarily the signal mask of the calling
* process with the mask given by "mask_set"
*
* @param[in] p_ve_task Pointer to "ve_task_struct"
* @param[out] mask_set Signal mask
*/
void psm_do_sigsuspend(struct ve_task_struct *p_ve_task, sigset_t *mask_set)
{
	VEOS_TRACE("Entering");

	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), LOCK
			, "failed to acquire task lock");
	p_ve_task->ve_saved_sigmask = p_ve_task->blocked;
	p_ve_task->mask_saved = true;
	pthread_mutex_lock_unlock(&(p_ve_task->ve_task_lock), UNLOCK
			, "failed to release task lock");
	psm_set_current_blocked(p_ve_task, mask_set);

	VEOS_TRACE("Exiting");
}

/**
 * @brief Set the state of  every VE process executing on VE node to STOP if
 * corresponding pseudo process state is set to STOP.
 *
 *	Monitors pseudo process state corresponding to every VE process
 *	executing on VE node. If the state of pseudo process is found to be
 *	stopped, its corresponding VE process is state is marked as STOP and
 *	re-scheduling is done if VE process is current task executing on VE
 *	core.If VE process is getting traced, then its pseudo process state is
 *	not traced.
 *
 * @internal
 * @author Signal Handling
 * */
void veos_stopping_thread(void)
{
	char path[PATH_MAX];
	struct list_head *p, *n;
	struct ve_task_struct *tmp = NULL;
	int retval = 0;
	struct timespec req;
	proc_t proc_info;

	VEOS_TRACE("Entering");

	req.tv_sec = 0;
	req.tv_nsec = 1000000;

	memset(path, 0, sizeof(path));

	while (!terminate_flag) {
		/* wait until awaken */
		pthread_mutex_lock_unlock(&(VE_NODE(0)->stop_mtx), LOCK
				, "failed to acquire stop mutex lock"
				" corresponding to node");
		while (!VE_NODE(0)->num_ve_proc) {
			VEOS_DEBUG("Stopping thread Waiting to be awaken");
			if (pthread_cond_wait(&VE_NODE(0)->stop_cond,
						&VE_NODE(0)->stop_mtx)) {
				pthread_mutex_lock_unlock
					(&(VE_NODE(0)->stop_mtx), UNLOCK
					 , "failed to release stop mutex lock"
					 " corresponding to node");
				VEOS_ERROR("Conditional wait failed");
				goto abort;
			}
			if (terminate_flag) {
				pthread_mutex_lock_unlock
					(&(VE_NODE(0)->stop_mtx), UNLOCK
					 , "failed to release stop mutex lock"
					 " corresponding to node");
				goto terminate;
			}
		}
		pthread_mutex_lock_unlock(&(VE_NODE(0)->stop_mtx), UNLOCK
				, "failed to release stop mutex lock"
				" corresponding to node");

		pthread_rwlock_lock_unlock(&init_task_lock, RDLOCK,
				"failed to acquire init task lock");
		/* Set VE processes state to STOP VE if corresponding pseudo
		 * process state id set to STOP
		 * */
		list_for_each_safe(p, n, &ve_init_task.tasks) {
			tmp = list_entry(p, struct ve_task_struct, tasks);

			/* If VE process is getting traced skip stoping
			 * its state as tracer process will take care of
			 * stopping/starting of VE process
			 * */
			if (true == tmp->ptraced)
				continue;

			/* Check if VE process state is already stopped */
			if (STOP == tmp->ve_task_state)
				continue;

			retval = pthread_rwlock_tryrdlock(&handling_request_lock);
			if (retval) {
				VEOS_ERROR("failed to acquire request lock");
				pthread_rwlock_lock_unlock(&init_task_lock
					, UNLOCK, "failed to release"
						" init task lock");
				if (EBUSY == retval)
					goto terminate;
				else
					goto abort;
			}
			/* Find state of Pseudo process */
			retval = psm_get_ve_proc_info(tmp->pid, &proc_info);
			if (-1 == retval) {
				VEOS_WARN("Failed to get state info"
					" for task: %d", tmp->pid);
				pthread_rwlock_lock_unlock
					(&handling_request_lock, UNLOCK,
					 "failed to release handling_request_lock");
				continue;
			}
			/* Stopping VE process as pseudo process is in
			 * stopped state
			 * */
			if (proc_info.state == 'T') {
				VEOS_DEBUG("Stopping VE process %d as "
						"pseudo process is stopped"
						, tmp->pid);
				VEOS_DEBUG("Acquiring tasklist_lock");
				pthread_mutex_lock_unlock(&(VE_NODE(0)->ve_tasklist_lock), LOCK,
						"Failed to acquire tasklist_lock lock");
				ve_do_group_action(tmp, FSTOPPROC, 0);
				pthread_mutex_lock_unlock(&(VE_NODE(0)->ve_tasklist_lock), UNLOCK,
						"Failed to release tasklist_lock lock");
			}
			pthread_rwlock_lock_unlock(&handling_request_lock
					, UNLOCK, "failed to release"
					" handling_request_lock");
		}
		pthread_rwlock_lock_unlock(&init_task_lock, UNLOCK,
				"failed to release init task lock");
		/* Sleep for 1 milisecond to avoid CPU busy loop as stopping
		 * thread check the pseudo process state in while loop.
		 */
		nanosleep(&req, NULL);
	}
terminate:
	VEOS_DEBUG("Termination flag SET,"
			" VEOS STOPPING thread exiting\n");
	VEOS_TRACE("Exiting");
	return;
abort:
	VEOS_FATAL("veos stopping thread failed");
	veos_abort("veos stopping thread failed");
}

/**
 * @brief polls the driver attribute file to collect the dead pid.
 *
 *	It polls driver attribute file
 *	where the PID of dead pseudo process is stored.
 *	It fetches the PID, and performs cleanup of
 *	corresponding VE process from veos
 *
 * @internal
 * @author Signal Handling
 */
void veos_polling_thread(void)
{
	int retval = 0;
	struct pollfd ufds[1];
	char *tok;
	char *tid_buf;
	pid_t pid;
	char dummy_buf[DUMMY_READ];
	char filename[PATH_MAX];
	char *saveptr;
	struct ve_task_struct *tsk;

	VEOS_TRACE("Entering");

	snprintf(filename, sizeof(filename),
			"%s/task_id_dead",
			VE_SYSFS_PATH(0));

	/* Open a connection to attribute file */
	ufds[0].fd = open(filename, O_RDONLY, S_IRUSR);
	if (ufds[0].fd < 0) {
		VEOS_CRIT("Unable to open attribute file");
		goto hndl_ret;
	}
	ufds[0].events = POLLPRI;

	tid_buf = (char *)malloc(PATH_MAX);
	if (NULL == tid_buf) {
		VEOS_CRIT("veos failed to allocate buffer to read attribute"
				" file");
		goto hndl_err;
	}
	memset(tid_buf, '\0', PATH_MAX);

	while (!terminate_flag) {
		/* Poll on the attribute file */
		retval = poll(ufds, 1, POLL_TIMEOUT);
		if (-1 == retval) {
			VEOS_ERROR("polling attribute file failed");
			goto hndl_err1;
		} else if (0 == retval) {
			VEOS_TRACE("Poll Timeout Occurred!!");
		} else if (ufds[0].revents & POLLPRI) {
			/* As mentioned in kernel documention and certain
			 * posts on some forums, for certain combinations
			 * of kernel and hardware, a dummy read is needed
			 * with poll() syscall.
			 * */
			if (-1 == read(ufds[0].fd, dummy_buf, 10)) { /* Trac# 383 */
				VEOS_ERROR("Error in reading "
						"dead pid");
				goto hndl_err1;
			}
			VEOS_DEBUG("Driver Notified about process death");
			lseek(ufds[0].fd, 0L, SEEK_SET);

			/* Read the attribute file and perform veos cleanup */
			if (-1 == read(ufds[0].fd, tid_buf, PATH_MAX)) {
				VEOS_ERROR("Error in reading "
						"dead pid");
				goto hndl_err1;
			}
			for (tok = strtok_r(tid_buf, " ", &saveptr);
					tok && *tok;
					tok = strtok_r(NULL, " \n",
						&saveptr)) {
				pid = atoi(tok);
				if (!pid)
					continue;
				VEOS_DEBUG("Cleanup for PID %d",
						pid);
				retval = pthread_rwlock_tryrdlock
					(&handling_request_lock);
				if (retval) {
					VEOS_ERROR("Failed to acquire request lock");
					if (EBUSY == retval)
						goto terminate;
					else
						goto hndl_err1;
				}
				tsk = find_ve_task_struct(pid);
				if (!tsk) {
					VEOS_ERROR("PID %d not found. "
							"Task deletion failed", pid);
					continue;
				}
				set_state(tsk);
				pthread_mutex_lock_unlock(&(tsk->ve_task_lock),
					LOCK,
					"Failed to acquire task lock [PID = %d]",
					tsk->pid);
				if (!tsk->exit_code_set) {
					tsk->exit_code = SIGKILL;
					tsk->exit_code_set = true;
				}
				pthread_mutex_lock_unlock(&(tsk->ve_task_lock),
					UNLOCK,
					"Failed to release task lock [PID = %d]",
					tsk->pid);
				put_ve_task_struct(tsk);
				pthread_rwlock_lock_unlock
					(&handling_request_lock, UNLOCK,
					 "failed to"
					 " release handling_request_lock");
			}
			memset(tid_buf, '\0', PATH_MAX);
		}
	}

terminate:
	free(tid_buf);
	close(ufds[0].fd);
	VEOS_DEBUG("Termination flag SET,"
					"VEOS POLLING thread exiting");
	return;
hndl_err1:
	free(tid_buf);
hndl_err:
	close(ufds[0].fd);
hndl_ret:
	veos_abort("veos polling thread failed");
	return;
}