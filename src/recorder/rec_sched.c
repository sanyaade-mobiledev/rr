#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <string.h>

#include "recorder.h"
#include "rec_sched.h"
#include "write_trace.h"

#include <sys/syscall.h>

#include "../share/hpc.h"
#include "../share/sys.h"
#include "../share/config.h"

#define DELAY_COUNTER_MAX 10

/* we could use a linked-list instead */
static struct context* registered_threads[NUM_MAX_THREADS];
static int num_active_threads;

static void set_switch_counter(int thread_ptr, int tmp_thread_ptr, struct context *ctx)
{
	if (tmp_thread_ptr == thread_ptr) {
		ctx->switch_counter--;
	} else {
		ctx->switch_counter = MAX_SWITCH_COUNTER;
	}
}

/**
 * Retrieves a thread from the pool of active threads in a
 * round-robin fashion.
 */
struct context* get_active_thread(struct context *ctx)
{
	static int thread_ptr = -1;
	int tmp_thread_ptr = thread_ptr;

	/* This maintains the order in which the threads are signaled to continue and
	 * when the the record is actually written
	 */

	if (ctx != 0) {
		if (!ctx->allow_ctx_switch) {
			return ctx;
		}

		/* switch to next thread if the thread reached the maximum number of RBCs */
		if (ctx->switch_counter < 0) {
			thread_ptr++;
			ctx->switch_counter = MAX_SWITCH_COUNTER;
		}
	}

	struct context *return_ctx;

	while (1) {
		/* check all threads again */
		for (; thread_ptr < NUM_MAX_THREADS; thread_ptr++) {
			return_ctx = registered_threads[thread_ptr];
			if (return_ctx != NULL) {
				if (return_ctx->exec_state == EXEC_STATE_IN_SYSCALL) {
					if (sys_waitpid_nonblock(return_ctx->child_tid, &(return_ctx->status)) != 0) {
						return_ctx->exec_state = EXEC_STATE_IN_SYSCALL_DONE;
						set_switch_counter(thread_ptr, tmp_thread_ptr, return_ctx);
						return return_ctx;
					}
					continue;
				}

				set_switch_counter(thread_ptr, tmp_thread_ptr, return_ctx);
				return return_ctx;
			}
		}

		thread_ptr = 0;
	}

	return 0;
}

/**
 * Sends a SIGINT to all processes/threads.
 */
void rec_sched_exit_all()
{
	int i;
	for (i = 0; i < NUM_MAX_THREADS; i++) {
		if (registered_threads[i] != NULL) {
			int tid = registered_threads[i]->child_tid;
			if (tid != EMPTY) {
				sys_kill(tid, SIGINT);
			}
		}
	}
}

int rec_sched_get_num_threads()
{
	return num_active_threads;
}

/**
 * Registers a new thread to the runtime system. This includes
 * initialization of the hardware performance counters
 */
void rec_sched_register_thread(pid_t parent, pid_t child)
{
	assert(child > 0 && child < MAX_TID);

	int hash = HASH(child);
	assert(registered_threads[hash] == 0);
	struct context *ctx = sys_malloc_zero(sizeof(struct context));

	ctx->exec_state = EXEC_STATE_START;
	ctx->status = 0;
	ctx->child_tid = child;
	ctx->child_mem_fd = sys_open_child_mem(child);

	sys_ptrace_setup(child);

	init_hpc(ctx);
	start_hpc(ctx, MAX_RECORD_INTERVAL);

	registered_threads[hash] = ctx;
	num_active_threads++;
}

/**
 * De-regsiter a thread and de-allocate all resources. This function
 * should be called when a thread exits.
 */
void rec_sched_deregister_thread(struct context **ctx_ptr)
{
	struct context *ctx = *ctx_ptr;
	int hash = HASH(ctx->child_tid);

	registered_threads[hash] = 0;
	num_active_threads--;
	assert(num_active_threads >= 0);

	/* delete all counter data */
	cleanup_hpc(ctx);

	/* close file descriptor to child memory */
	sys_close(ctx->child_mem_fd);

	sys_ptrace_detatch(ctx->child_tid);

	/* make sure that the child has exited */

	int ret;
	do {
		ret = waitpid(ctx->child_tid, &ctx->status, __WALL | __WCLONE);
	} while (ret != -1);

	/* finally, free the memory */
	sys_free((void**) ctx_ptr);
}

