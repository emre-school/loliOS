#include "process.h"
#include "lib.h"
#include "filesys.h"
#include "paging.h"
#include "terminal.h"
#include "debug.h"
#include "x86_desc.h"

/* The virtual address that the process should be copied to */
static uint8_t * const process_vaddr = (uint8_t *)(USER_PAGE_START + 0x48000);

/* Process control blocks */
static pcb_t process_info[MAX_PROCESSES];

/* Kernel stack + pointer to PCB, one for each process */
__attribute__((aligned(PROCESS_DATA_SIZE)))
static process_data_t process_data[MAX_PROCESSES];

/*
 * Gets the PCB of the specified process.
 */
static pcb_t *
get_pcb(int32_t pid)
{
    /* When getting the parent of a "root" process */
    if (pid < 0) {
        return NULL;
    }

    ASSERT(pid < MAX_PROCESSES);
    ASSERT(process_info[pid].pid >= 0);
    return &process_info[pid];
}

/*
 * Gets the PCB of the currently executing process.
 *
 * This may only be called from a *process's* kernel stack
 * (that is, it must not be called during kernel init)!
 */
pcb_t *
get_executing_pcb(void)
{
    /*
     * Since the process data entries are 8KB-aligned, we can
     * extract the PCB pointer by masking the current kernel
     * ESP, which gives us the address of the executing process's
     * process_data_t struct.
     *
     * (8KB-aligned ESP)                        ESP
     *       |                                   |
     *       v                                   v
     *      [PCB|_____________KERNEL STACK_______________]
     *      <- lower addresses         higher addresses ->
     */
    uint32_t esp;
    read_register("esp", esp);
    process_data_t *data = (process_data_t *)(esp & ~(PROCESS_DATA_SIZE - 1));
    return data->pcb;
}

/*
 * Gets the PCB of the currently executing process
 * in the specified terminal.
 */
pcb_t *
get_pcb_by_terminal(int32_t terminal)
{
    int32_t i;
    for (i = 0; i < MAX_PROCESSES; ++i) {
        pcb_t *pcb = &process_info[i];
        if (pcb->pid >= 0 &&                /* Valid? */
            pcb->terminal == terminal &&    /* Same terminal? */
            pcb->status != PROCESS_SLEEP) { /* Running? */
            return pcb;
        }
    }
    return NULL;
}

/*
 * Finds the next process that is scheduled for execution. If
 * there are no other process that can be executed, returns the
 * current process.
 */
static pcb_t *
get_next_pcb(void)
{
    pcb_t *curr_pcb = get_executing_pcb();
    int32_t i;
    for (i = 1; i < MAX_PROCESSES; ++i) {
        int32_t pid = (curr_pcb->pid + i) % MAX_PROCESSES;
        pcb_t *pcb = &process_info[pid];
        if (pcb->pid >= 0) {
            int32_t status = pcb->status;
            if (status == PROCESS_RUN || status == PROCESS_SCHED) {
                return pcb;
            }
        }
    }

    /* If nothing else can be executed, just return the current one */
    return curr_pcb;
}

/*
 * Allocates a new PCB. Returns a pointer to the PCB, or
 * NULL if the maximum number of processes are already
 * running.
 */
static pcb_t *
process_new_pcb(void)
{
    int32_t i;

    /* Look for an empty process slot we can fill */
    for (i = 0; i < MAX_PROCESSES; ++i) {
        if (process_info[i].pid < 0) {
            process_info[i].pid = i;
            return &process_info[i];
        }
    }

    /* Reached max number of processes */
    return NULL;
}

/*
 * Ensures that the given file is a valid executable file.
 * On success, writes the inode index of the file to out_inode_idx,
 * the arguments to out_args, and returns 0. Otherwise, returns -1.
 */
static int32_t
process_parse_cmd(const uint8_t *command, uint32_t *out_inode_idx, uint8_t *out_args)
{
    /*
     * Scan for the end of the exe filename
     * (@ == \0 in this diagram)
     *
     * Valid case:
     * cat    myfile.txt@
     *    |___|__________ i = 3 after loop terminates
     *        |__________ out_args
     *
     * Valid case:
     * ls@
     *   |___ i = 2
     *   |___ out_args
     *
     * Invalid case:
     * ccccccccccccaaaaaaaaaaaattttttttt myfile.txt@
     *                                  |___________ i = FNAME_LEN + 1
     */

    /* Command index */
    int32_t i = 0;

    /* Strip leading whitespace */
    while (command[i] == ' ') {
        i++;
    }

    /* Read the filename (up to 33 chars with NUL terminator) */
    uint8_t filename[FNAME_LEN + 1];
    int32_t fname_i;
    for (fname_i = 0; fname_i < FNAME_LEN + 1; ++fname_i, ++i) {
        uint8_t c = command[i];
        if (c == ' ' || c == '\0') {
            filename[fname_i] = '\0';
            break;
        }
        filename[fname_i] = c;
    }

    /* If we didn't break out of the loop, the filename is too long */
    if (fname_i == FNAME_LEN + 1) {
        debugf("Filename too long\n");
        return -1;
    }

    debugf("Trying to execute: %s\n", filename);

    /* Strip leading whitespace */
    while (command[i] == ' ') {
        i++;
    }

    /* Now copy the arguments to the arg buffer */
    int32_t args_i;
    for (args_i = 0; args_i < MAX_ARGS_LEN; ++args_i, ++i) {
        if ((out_args[args_i] = command[i]) == '\0') {
            break;
        }
    }

    /* Args are too long */
    if (args_i == MAX_ARGS_LEN) {
        debugf("Args too long\n");
        return -1;
    }

    /* Read dentry for the file */
    dentry_t dentry;
    if (read_dentry_by_name(filename, &dentry) != 0) {
        debugf("Cannot find dentry\n");
        return -1;
    }

    /* Can only execute files, obviously */
    if (dentry.ftype != FTYPE_FILE) {
        debugf("Can only execute files\n");
        return -1;
    }

    /* Read the magic bytes from the file */
    uint32_t magic;
    if (read_data(dentry.inode_idx, 0, (uint8_t *)&magic, 4) != 4) {
        debugf("Could not read magic\n");
        return -1;
    }

    /* Ensure it's an executable file */
    if (magic != EXE_MAGIC) {
        debugf("Magic mismatch - not an executable\n");
        return -1;
    }

    /* Write inode index */
    *out_inode_idx = dentry.inode_idx;

    return 0;
}

/*
 * Copies the program into memory. Returns the address of
 * the entry point of the program.
 *
 * You must point the process page to the correct physical
 * page before calling this!
 */
static uint32_t
process_load_exe(uint32_t inode_idx)
{
    uint32_t count;
    uint32_t offset = 0;
    do {
        /*
         * Copy the program to the process page.
         * The chunk size (4096B) is arbitrary.
         */
        count = read_data(inode_idx, offset, process_vaddr + offset, 4096);
        offset += count;
    } while (count > 0);

    /* The entry point is located at bytes 24-27 of the executable */
    uint32_t entry_point = *(uint32_t *)(process_vaddr + 24);
    return entry_point;
}

/*
 * Gets the address of the bottom of the kernel stack
 * for the specified process.
 */
static uint32_t
get_kernel_base_esp(pcb_t *pcb)
{
    /*
     * ESP0 points to bottom of the process kernel stack.
     *
     * (lower addresses)
     * |---------|
     * |  PID 0  |
     * |---------|
     * |  PID 1  |
     * |---------|<- ESP0 when new PID == 1
     * |   ...   |
     * (higher addresses)
     */
    uint32_t stack_start = (uint32_t)process_data[pcb->pid].kernel_stack;
    uint32_t stack_size = sizeof(process_data[pcb->pid].kernel_stack);
    return stack_start + stack_size;
}

/*
 * Sets the global execution context to the specified process.
 */
static void
process_set_context(pcb_t *to)
{
    /* Restore process page */
    paging_update_process_page(to->pid);

    /* Restore vidmap status */
    terminal_update_vidmap(to->terminal, to->vidmap);

    /* Restore TSS entry */
    tss.esp0 = get_kernel_base_esp(to);
}

/*
 * Jumps into userspace and executes the specified process.
 *
 * This is annotated with __cdecl since we rely on the return
 * value being placed into EAX, not by this function, but by
 * process_halt_impl.
 */
__unused __cdecl static int32_t
process_run_impl(pcb_t *pcb)
{
    ASSERT(pcb != NULL);
    ASSERT(pcb->pid >= 0);

    /* Mark process as initialized */
    pcb->status = PROCESS_RUN;

    /* Set the global execution context */
    process_set_context(pcb);

    /*
     * Save ESP and EBP of the current call frame so that we
     * can safely return from halt() inside the child.
     *
     * DO NOT MODIFY ANY CODE HERE UNLESS YOU ARE 100% SURE
     * ABOUT WHAT YOU ARE DOING!
     */
    asm volatile("movl %%esp, %0;"
                 "movl %%ebp, %1;"
                 : "=g"(pcb->parent_esp),
                   "=g"(pcb->parent_ebp)
                 :
                 : "memory");

    /* Jump into userspace and execute */
    asm volatile(
                 /* Segment registers */
                 "movw %1, %%ax;"
                 "movw %%ax, %%ds;"
                 "movw %%ax, %%es;"
                 "movw %%ax, %%fs;"
                 "movw %%ax, %%gs;"

                 /* SS register */
                 "pushl %1;"

                 /* ESP */
                 "pushl %2;"

                 /* EFLAGS */
                 "pushfl;"
                 "popl %%eax;"
                 "orl $0x200, %%eax;" /* Set IF */
                 "pushl %%eax;"

                 /* CS register */
                 "pushl %3;"

                 /* EIP */
                 "pushl %0;"

                 /* Zero all general registers for security */
                 "xorl %%eax, %%eax;"
                 "xorl %%ebx, %%ebx;"
                 "xorl %%ecx, %%ecx;"
                 "xorl %%edx, %%edx;"
                 "xorl %%esi, %%esi;"
                 "xorl %%edi, %%edi;"
                 "xorl %%ebp, %%ebp;"

                 /* GO! */
                 "iret;"

                 : /* No outputs */
                 : "r"(pcb->entry_point),
                   "i"(USER_DS),
                   "i"(USER_PAGE_END),
                   "i"(USER_CS)
                 : "eax", "cc");

    /* Can't touch this */
    ASSERT(0);
    return -1;
}

/*
 * Wrapper function for process_run_impl that takes care of clobbering
 * the appropriate registers.
 */
static int32_t
process_run(pcb_t *pcb)
{
    /*
     * Since we can't rely on anything other than EAX,
     * ESP, and EBP being intact when we "return" from
     * process_run_impl, we explicitly clobber them so GCC
     * knows to save them somewhere.
     */
    int32_t ret;
    asm volatile("pushl %1;"
                 "call process_run_impl;"
                 "addl $4, %%esp;"
                 : "=a"(ret)
                 : "g"(pcb)
                 : "ebx", "ecx", "edx", "esi", "edi", "cc");
    return ret;
}

static void
process_init_signals(signal_info_t *signals)
{
    int32_t i;
    for (i = 0; i < NUM_SIGNALS; ++i) {
        signals[i].signum = i;
        signals[i].action = SIGACTION_IGNORE;
        signals[i].handler_addr = 0;
        signals[i].masked = false;
        signals[i].pending = false;
    }
    signals[SIG_DIV_ZERO].action = SIGACTION_KILL;
    signals[SIG_SEGFAULT].action = SIGACTION_KILL;
    signals[SIG_INTERRUPT].action = SIGACTION_KILL;
}

/*
 * Creates and initializes a new PCB for the given process.
 *
 * Implementation note: This is deliberately decoupled from
 * actually executing the process so it's easier to implement
 * context switching.
 */
static pcb_t *
process_create_child(const uint8_t *command, pcb_t *parent_pcb, int32_t terminal)
{
    uint32_t inode;
    uint8_t args[MAX_ARGS_LEN];

    /* First make sure we have a valid executable... */
    if (process_parse_cmd(command, &inode, args) != 0) {
        debugf("Invalid command/executable file\n");
        return NULL;
    }

    /* Try to allocate a new PCB */
    pcb_t *child_pcb = process_new_pcb();
    if (child_pcb == NULL) {
        debugf("Reached max number of processes\n");
        return NULL;
    }

    /* Initialize child PCB */
    if (parent_pcb == NULL) {
        /* This is the first process! */
        ASSERT(terminal >= 0);
        child_pcb->parent_pid = -1;
        child_pcb->terminal = terminal;
    } else {
        /* Inherit values from parent process */
        child_pcb->parent_pid = parent_pcb->pid;
        child_pcb->terminal = parent_pcb->terminal;
    }

    /* Common initialization */
    child_pcb->status = PROCESS_SCHED;
    child_pcb->vidmap = false;
    process_init_signals(child_pcb->signals);
    file_init(child_pcb->files);
    strncpy((int8_t *)child_pcb->args, (const int8_t *)args, MAX_ARGS_LEN);

    /* Update PCB pointer in the kernel data for this process */
    process_data[child_pcb->pid].pcb = child_pcb;

    /* Copy our program into physical memory */
    paging_update_process_page(child_pcb->pid);
    child_pcb->entry_point = process_load_exe(inode);

    return child_pcb;
}

/*
 * Process execute implementation.
 *
 * The command buffer will not be checked for validity, so
 * this can be called from either userspace or kernelspace.
 *
 * The terminal argument specifies which terminal to spawn
 * the process on if there is no parent.
 */
static int32_t
process_execute_impl(const uint8_t *command, pcb_t *parent_pcb, int32_t terminal)
{
    /* Create the child process */
    pcb_t *child_pcb = process_create_child(command, parent_pcb, terminal);
    if (child_pcb == NULL) {
        debugf("Could not create child process\n");
        return -1;
    }

    /* If there's a parent process, stop executing it */
    if (parent_pcb != NULL) {
        parent_pcb->status = PROCESS_SLEEP;
    }

    /* Jump into userspace and begin executing the program */
    return process_run(child_pcb);
}

/* execute() syscall handler */
__cdecl int32_t
process_execute(const uint8_t *command)
{
    /* Validate command */
    if (!is_user_readable_string(command)) {
        debugf("Invalid string passed to process_execute\n");
        return -1;
    }

    /*
     * This should never be called directly from the kernel, so
     * there MUST be an executing process, so we can pass
     * -1 as the terminal since it will never be used anyways
     */
    return process_execute_impl(command, get_executing_pcb(), -1);
}

/*
 * Process halt implementation.
 *
 * Unlike process_halt(), the status is not truncated to 1 byte.
 */
static int32_t
process_halt_impl(uint32_t status)
{
    /* This is the PCB of the child (halting) process */
    pcb_t *child_pcb = get_executing_pcb();

    /* Find parent process */
    pcb_t *parent_pcb = get_pcb(child_pcb->parent_pid);

    /* Close all open files */
    int32_t i;
    for (i = 2; i < MAX_FILES; ++i) {
        if (child_pcb->files[i].valid) {
            file_close(i);
        }
    }

    /* Mark child PCB as free */
    child_pcb->pid = -1;

    /* If no parent process, just re-spawn a new shell in the same terminal */
    if (parent_pcb == NULL) {
        process_execute_impl((uint8_t *)"shell", NULL, child_pcb->terminal);

        /* Should never get back to this point */
        ASSERT(0);
    }

    /* Mark parent as runnable again */
    parent_pcb->status = PROCESS_RUN;

    /* Set the global execution context */
    process_set_context(parent_pcb);

    /*
     * This "returns" from the PARENT'S process_run_impl call frame
     * by restoring its esp/ebp and executing leave + ret.
     *
     * DO NOT MODIFY ANY CODE HERE UNLESS YOU ARE 100% SURE
     * ABOUT WHAT YOU ARE DOING!
     */
    asm volatile("movl %1, %%esp;"
                 "movl %2, %%ebp;"
                 "movl %0, %%eax;"
                 "leave;"
                 "ret;"
                 :
                 : "r"(status),
                   "r"(child_pcb->parent_esp),
                   "r"(child_pcb->parent_ebp)
                 : "esp", "ebp", "eax");

    /* Should never get here! */
    ASSERT(0);
    return -1;
}

/*
 * Switches execution to the next scheduled process.
 */
__unused __cdecl static void
process_switch_impl(void)
{
    pcb_t *curr = get_executing_pcb();
    pcb_t *next = get_next_pcb();
    if (curr == next) {
        return;
    }

    /*
     * Save current stack pointer so we can "return" to this
     * stack frame.
     */
    asm volatile("movl %%esp, %0;"
                 "movl %%ebp, %1;"
                 : "=g"(curr->kernel_esp),
                   "=g"(curr->kernel_ebp));

    if (next->status == PROCESS_SCHED) {
        /*
         * If we're in this block, we're initializing
         * one of the three initial shells. We don't set
         * the stack pointer because:
         *
         * 1) It's never used
         * 2) It's not initialized anyways
         */
        process_run(next);
    } else if (next->status == PROCESS_RUN) {
        /*
         * If we're in this block, the next process must be
         * in a process_switch_impl call too. We just switch
         * into its stack and return.
         */

        /* Set global execution context */
        process_set_context(next);

        /* "Return" into the other process's process_switch_impl frame */
        asm volatile("movl %0, %%esp;"
                     "movl %1, %%ebp;"
                     :
                     : "r"(next->kernel_esp),
                       "r"(next->kernel_ebp));
    }
}

/*
 * Wrapper for process_switch_impl that clobbers the
 * appropriate registers.
 */
void
process_switch(void)
{
    asm volatile("call process_switch_impl;"
                 :
                 :
                 : "eax", "ebx", "ecx", "edx", "esi", "edi", "cc");
}

/*
 * Pushes the signal handler context onto the user stack
 * and modifies the register context to start execution
 * at the signal handler.
 */
static bool
process_run_signal_handler(signal_info_t *sig, int_regs_t *regs)
{
    /* "Shellcode" that calls the sigreturn() syscall */
    uint8_t shellcode[] = {
        /* movl $SYS_SIGRETURN, %eax */
        0xB8, 0xAA, 0xAA, 0xAA, 0xAA,

        /* movl signum, %ebx */
        0xBB, 0xBB, 0xBB, 0xBB, 0xBB,

        /* movl regs, %ecx */
        0xB9, 0xCC, 0xCC, 0xCC, 0xCC,

        /* int 0x80 */
        0xCD, 0x80,

        /* nop (to align stack to 4 bytes) */
        0x90, 0x90, 0x90,
    };

    /* Fill in the syscall number */
    ASSERT((sizeof(shellcode) & 0x3) == 0);

    /* Start "pushing" stuff onto user stack */
    uint8_t *esp = (uint8_t *)regs->esp;

    /* Push sigreturn linkage onto user stack */
    esp -= sizeof(shellcode);
    uint8_t *linkage_addr = esp;
    if (!copy_to_user(esp, shellcode, sizeof(shellcode))) {
        return false;
    }

    /* Push interrupt context onto user stack */
    esp -= sizeof(int_regs_t);
    uint8_t *intregs_addr = esp;
    if (!copy_to_user(esp, regs, sizeof(int_regs_t))) {
        return false;
    }

    /* Push signal number onto user stack */
    esp -= sizeof(int32_t);
    uint8_t *signum_addr = esp;
    if (!copy_to_user(esp, &sig->signum, sizeof(int32_t))) {
        return false;
    }

    /* Push return address (which is sigreturn linkage) onto user stack */
    esp -= sizeof(uint32_t);
    if (!copy_to_user(esp, &linkage_addr, sizeof(uint32_t))) {
        return false;
    }

    /* Fill in shellcode values */
    int32_t syscall_num = SYS_SIGRETURN;
    copy_to_user(linkage_addr + 1, &syscall_num, 4);
    copy_to_user(linkage_addr + 6, signum_addr, 4);
    copy_to_user(linkage_addr + 11, &intregs_addr, 4);

    /* Change EIP of userspace program to the signal handler */
    regs->eip = sig->handler_addr;

    /* Change ESP to point to new stack bottom */
    regs->esp = (uint32_t)esp;

    /* Fix segment registers in case that was the cause of an exception */
    regs->cs = USER_CS;
    regs->ds = USER_DS;
    regs->es = USER_DS;
    regs->fs = USER_DS;
    regs->gs = USER_DS;
    regs->ss = USER_DS;

    /* Mask signal so we don't get re-entrant calls */
    sig->masked = true;
    sig->pending = false;
    return true;
}

/*
 * Returns whether the currently executing process
 * has a pending signal.
 */
bool
process_has_pending_signal(void)
{
    pcb_t *pcb = get_executing_pcb();
    int32_t i;
    for (i = 0; i < NUM_SIGNALS; ++i) {
        signal_info_t *sig = &pcb->signals[i];
        if (sig->pending && !sig->masked && sig->action != SIGACTION_IGNORE) {
            return true;
        }
    }
    return false;
}

/*
 * Attempts to deliver a signal to the currently
 * executing process. Returns false if 
 */
bool
process_handle_signal(signal_info_t *sig, int_regs_t *regs)
{
    /* If handler is set and signal isn't masked, run it */
    if (sig->handler_addr != 0 && !sig->masked) {
        /* If no more space on stack to push signal context, kill process */
        if (!process_run_signal_handler(sig, regs)) {
            debugf("Failed to push signal context, killing process\n");
            process_halt_impl(256);
        }
        return true;
    }

    /* Run default handler if no handler or masked */
    if (sig->signum == SIG_DIV_ZERO || sig->signum == SIG_SEGFAULT) {
        debugf("Killing process due to exception\n");
        process_halt_impl(256);
        return true;
    }

    /* CTRL-C halts with exit code 130 (SIGINT) */
    if (sig->signum == SIG_INTERRUPT) {
        debugf("Killing process due to CTRL-C\n");
        process_halt_impl(130);
        return true;
    }

    /* Default action is to ignore the signal */
    sig->pending = false;
    return false;
}

/*
 * If the currently executing process has any pending
 * signals, modifies the IRET context and user stack to run the
 * signal handler.
 */
void
process_handle_signals(int_regs_t *regs)
{
    pcb_t *pcb = get_executing_pcb();
    int32_t i;
    for (i = 0; i < NUM_SIGNALS; ++i) {
        /* If we have any pending signals, try to deliver them */
        signal_info_t *sig = &pcb->signals[i];
        if (sig->pending && process_handle_signal(sig, regs)) {
            break;
        }
    }
}

/*
 * Handles an exception that occurred in userspace.
 * If a signal handler is available, will cause that to
 * be executed. Otherwise, kills the process.
 */
void
process_user_exception(uint32_t int_num)
{
    pcb_t *pcb = get_executing_pcb();

    /* Get appropriate signal handler for this exception */
    signal_info_t *sig;
    if (int_num == EXC_DE) {
        /* Special handler for divide by zero */
        sig = &pcb->signals[SIG_DIV_ZERO];
    } else {
        /* All other exceptions raise segfault */
        sig = &pcb->signals[SIG_SEGFAULT];
    }

    /* Schedule signal handler for execution */
    sig->pending = true;
}

void
process_interrupt(int32_t terminal)
{
    pcb_t *pcb = get_pcb_by_terminal(terminal);
    if (pcb == NULL) {
        debugf("No process running in terminal %d\n", terminal);
        return;
    }

    /* Schedule signal handler for execution */
    signal_info_t *sig = &pcb->signals[SIG_INTERRUPT];
    sig->pending = true;
}

/* halt() syscall handler */
__cdecl int32_t
process_halt(uint32_t status)
{
    /*
     * Only the lowest byte is used, rest are reserved
     * This only applies when this is called via syscall;
     * the kernel must still be able to halt a process
     * with a status > 255.
     */
    return process_halt_impl(status & 0xff);
}

/* getargs() syscall handler */
__cdecl int32_t
process_getargs(uint8_t *buf, int32_t nbytes)
{
    /* Ensure buffer is valid */
    if (!is_user_writable(buf, nbytes)) {
        return -1;
    }

    /* Can only read at most MAX_ARGS_LEN characters */
    if (nbytes > MAX_ARGS_LEN) {
        nbytes = MAX_ARGS_LEN;
    }

    /* Copy the args into the buffer */
    pcb_t *pcb = get_executing_pcb();
    strncpy((int8_t *)buf, (int8_t *)pcb->args, nbytes);

    /* Ensure the string is NUL-terminated */
    if (nbytes > 0) {
        buf[nbytes - 1] = '\0';
    }

    return 0;
}

/* vidmap() syscall handler */
__cdecl int32_t
process_vidmap(uint8_t **screen_start)
{
    /* Ensure buffer is valid */
    if (!is_user_writable(screen_start, sizeof(uint8_t *))) {
        return -1;
    }

    pcb_t *pcb = get_executing_pcb();

    /* Update vidmap status */
    terminal_update_vidmap(pcb->terminal, true);

    /* Save vidmap state in PCB */
    pcb->vidmap = true;

    /* Write vidmap page address to userspace */
    *screen_start = (uint8_t *)VIDMAP_PAGE_START;

    return 0;
}

/* set_handler() syscall handler */
__cdecl int32_t
process_set_handler(int32_t signum, uint32_t handler_address)
{
    /* Check signal number range */
    if (signum < 0 || signum >= NUM_SIGNALS) {
        return -1;
    }

    pcb_t *pcb = get_executing_pcb();
    pcb->signals[signum].handler_addr = handler_address;
    pcb->signals[signum].action = SIGACTION_CUSTOM;
    return 0;
}

/* sigreturn() syscall handler */
__cdecl int32_t
process_sigreturn(
    int32_t signum,
    int_regs_t *user_regs,
    __unused uint32_t unused,
    int_regs_t *kernel_regs)
{
    /* Check signal number range */
    if (signum < 0 || signum >= NUM_SIGNALS) {
        debugf("Invalid signal number\n");
        return -1;
    }

    /* Check saved register context */
    if (!is_user_readable(user_regs, sizeof(int_regs_t))) {
        debugf("Cannot read user regs\n");
        return -1;
    }

    /* You try to do kernel privilege exploit? Nein! */
    if (user_regs->cs != USER_CS) {
        debugf("sigreturn CS does not equal USER_CS\n");
        return -1;
    }

    /* Unmask signal again */
    pcb_t *pcb = get_executing_pcb();
    pcb->signals[signum].masked = false;

    /* Ignore privileged EFLAGS bits (emulate POPFL behavior) */
    /* http://stackoverflow.com/a/39195843 */
    uint32_t kernel_eflags = kernel_regs->eflags & ~0xDD5;
    uint32_t user_eflags = user_regs->eflags & 0xDD5;

    /* Copy user context to kernel interrupt context */
    *kernel_regs = *user_regs;

    /* Restore EFLAGS to a safe value */
    kernel_regs->eflags = kernel_eflags | user_eflags;

    /*
     * Interrupt handler overwrites EAX with the return value,
     * so we just return EAX so it will get set to itself.
     */
    return kernel_regs->eax;
}

/* Initializes all process control related data */
void
process_init(void)
{
    int32_t i;
    for (i = 0; i < MAX_PROCESSES; ++i) {
        process_info[i].pid = -1;
    }
}

/* She spawns C shells by the seashore */
void
process_start_shell(void)
{
    int32_t i;
    for (i = 1; i < NUM_TERMINALS; ++i) {
        process_create_child((uint8_t *)"shell", NULL, i);
    }
    process_execute_impl((uint8_t *)"shell", NULL, 0);
}
