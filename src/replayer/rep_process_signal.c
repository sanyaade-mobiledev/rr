#include <assert.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <sys/fcntl.h>

#include "read_trace.h"
#include "replayer.h"

#include "../share/sys.h"
#include "../share/trace.h"
#include "../share/util.h"
#include "../share/ipc.h"
#include "../share/hpc.h"

#define SKID_SIZE 			55

static void singlestep(struct context *ctx, int sig, int expected_val)
{
	sys_ptrace_singlestep(ctx->child_tid, sig);
	sys_waitpid(ctx->child_tid, &ctx->status);
	/* we get a simple SIGTRAP in this case */
	if (ctx->status != expected_val) {
		printf("status %x   expected %x\n", ctx->status, expected_val);
	}

	assert(ctx->status == expected_val);
	ctx->status = 0;
	ctx->child_sig = 0;
}

/**
 * function goes to the n-th conditional branch
 */
static void compensate_branch_count(struct context *ctx, int sig)
{
	uint64_t rbc_now, rbc_rec;

	rbc_rec = ctx->trace.rbc_up;
	rbc_now = read_rbc_up(ctx->hpc);

	/* if the skid size was too small, go back to the last checkpoint and
	 * re-execute the program.
	 */
	if (rbc_now > rbc_rec) {
		/* checkpointing is not implemented yet - so we fail */
		fprintf(stderr, "hpc overcounted in asynchronous event, recorded: %llu  now: %llu\n", rbc_rec, rbc_now);
		fprintf(stderr,"event: %d, flobal_time %u\n",ctx->trace.stop_reason, ctx->trace.global_time);
		assert(0);
	}

	int found_spot = 0;
	rbc_now = read_rbc_up(ctx->hpc);

	while (rbc_now < rbc_rec) {
		singlestep(ctx, 0, 0x57f);
		rbc_now = read_rbc_up(ctx->hpc);
	}

	while (rbc_now == rbc_rec) {
		struct user_regs_struct regs;
		read_child_registers(ctx->child_tid, &regs);
		if (sig == SIGSEGV) {
			/* we should now stop at the instruction that caused the SIGSEGV */
			sys_ptrace_syscall(ctx->child_tid);
			sys_waitpid(ctx->child_tid, &ctx->status);
		}

		/* the eflags register has two bits that are set when an interrupt is pending:
		 * bit 8:  TF (trap flag)
		 * bit 17: VM (virtual 8086 mode)
		 *
		 * we enable these two bits in the eflags register to make sure that the register
		 * files match
		 *
		 */
		int check = compare_register_files("now", &regs, "rec", &ctx->trace.recorded_regs, 0, 0);
		if (check == 0 || check == 0x80) {
			found_spot++;
			/* A SIGSEGV can be triggered by a regular instruction; it is not necessarily sent by
			 * another process. We check this condition here.
			 */
			if (sig == SIGSEGV) {
				//print_inst(ctx->child_tid);

				/* here we ensure that the we get a SIGSEGV at the right spot */
				singlestep(ctx, 0, 0xb7f);
				/* deliver the signal */
				break;
			} else {
				break;
			}
			/* set the signal such that it is delivered when the process continues */
		}
		/* check that we do not get unexpected signal in the single-stepping process */
		singlestep(ctx, 0, 0x57f);
		rbc_now = read_rbc_up(ctx->hpc);
	}
	if (found_spot != 1) {
		printf("cannot find signal %d   time: %u\n",sig,ctx->trace.global_time);
		assert(found_spot == 1);
	}
}

void rep_process_signal(struct context *ctx)
{
	struct trace* trace = &(ctx->trace);
	int tid = ctx->child_tid;
	int sig = -trace->stop_reason;

	/* if the there is still a signal pending here, two signals in a row must be delivered?\n */
	assert(ctx->child_sig == 0);

	switch (sig) {

	/* set the eax and edx register to the recorded values */
	case -SIG_SEGV_RDTSC:
	{
		struct user_regs_struct regs;
		int size;

		/* goto the event */
		goto_next_event(ctx);

		/* make sure we are there */
		assert(WSTOPSIG(ctx->status) == SIGSEGV);

		char* inst = get_inst(tid, 0, &size);
		assert(strncmp(inst,"rdtsc",5) == 0);
		read_child_registers(tid, &regs);
		regs.eax = trace->recorded_regs.eax;
		regs.edx = trace->recorded_regs.edx;
		regs.eip += size;
		write_child_registers(tid, &regs);
		sys_free((void**) &inst);

		compare_register_files("rdtsv_now", &regs, "rdsc_rec", &ctx->trace.recorded_regs, 1, 1);

		/* this signal should not be recognized by the application */
		ctx->child_sig = 0;
		break;
	}

	case -USR_SCHED:
	{
		assert(trace->rbc_up > 0);

		/* if the current architecture over-counts the event in question,
		 * substract the overcount here */
		reset_hpc(ctx, trace->rbc_up - SKID_SIZE);
		goto_next_event(ctx);
		/* make sure that the signal came from hpc */
		if (fcntl(ctx->hpc->rbc_down.fd, F_GETOWN) == ctx->child_tid) {
			/* this signal should not be recognized by the application */
			ctx->child_sig = 0;
			stop_hpc_down(ctx);
			compensate_branch_count(ctx, sig);
			stop_hpc(ctx);
		} else {
			fprintf(stderr, "internal error: next event should be: %d but it is: %d -- bailing out\n", -USR_SCHED, ctx->event);
			sys_exit();
		}

		break;
	}

	case SIGIO:
	case SIGCHLD:
	{
		/* synchronous signal (signal received in a system call) */
		if (trace->rbc_up == 0) {
			ctx->replay_sig = sig;
			return;
		}

		// setup and start replay counters
		reset_hpc(ctx, trace->rbc_up - SKID_SIZE);

		/* single-step if the number of instructions to the next event is "small" */
		if (trace->rbc_up <= 10000) {
			stop_hpc_down(ctx);
			compensate_branch_count(ctx, sig);
			stop_hpc(ctx);
		} else {
			printf("large count\n");
			sys_ptrace_syscall(tid);
			sys_waitpid(tid, &ctx->status);
			// make sure we ere interrupted by ptrace
			assert(WSTOPSIG(ctx->status) == SIGIO);
			/* reset the penig sig, since it did not occur in the original execution */
			ctx->child_sig = 0;
			ctx->status = 0;

			//DO NOT FORGET TO STOP HPC!!!
			compensate_branch_count(ctx, sig);
			stop_hpc(ctx);
			stop_hpc_down(ctx);

		}

		break;
	}

	case SIGSEGV:
	{
		/* synchronous signal (signal received in a system call) */
		if (trace->rbc_up == 0 && trace->page_faults == 0) {
			ctx->replay_sig = sig;
			return;
		}

		sys_ptrace_syscall(ctx->child_tid);
		sys_waitpid(ctx->child_tid, &ctx->status);
		assert(WSTOPSIG(ctx->status) == SIGSEGV);

		struct user_regs_struct regs;
		read_child_registers(ctx->child_tid, &regs);
		assert(compare_register_files("now", &regs, "rec", &ctx->trace.recorded_regs, 1, 1) == 0);

		/* deliver the signal */
		singlestep(ctx, SIGSEGV, 0x57f);
		break;
	}

	default:
	printf("unknown signal %d -- bailing out\n", sig);
	sys_exit();
		break;
	}
}
