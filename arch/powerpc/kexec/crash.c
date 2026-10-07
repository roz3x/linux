// SPDX-License-Identifier: GPL-2.0-only
/*
 * Architecture specific (PPC64) functions for kexec based crash dumps.
 *
 * Copyright (C) 2005, IBM Corp.
 *
 * Created by: Haren Myneni
 */

#include <linux/kernel.h>
#include <linux/smp.h>
#include <linux/reboot.h>
#include <linux/kexec.h>
#include <linux/export.h>
#include <linux/crash_dump.h>
#include <linux/delay.h>
#include <linux/irq.h>
#include <linux/types.h>
#include <linux/libfdt.h>
#include <linux/memory.h>
#include <linux/slab.h>

#include <asm/processor.h>
#include <asm/machdep.h>
#include <asm/kexec.h>
#include <asm/smp.h>
#include <asm/setjmp.h>
#include <asm/debug.h>
#include <asm/interrupt.h>
#include <asm/kexec_ranges.h>
#include <asm/crashdump-ppc64.h>

#ifdef CONFIG_CRASH_HOTPLUG
/*
 * Double-buffer for deferred FDT updates on CPU hotplug (DLPAR) events.
 *
 * The workqueue worker always writes into the inactive buffer, then
 * publishes it with smp_store_release(&fdt_buf.inuse, bufidx).  The crash
 * shutdown path reads inuse with smp_load_acquire() to obtain the
 * producer's stores before flushing the buffer into the kexec segment.
 *
 * The ordered workqueue ensures only one worker runs at a time, so the
 * two-buffer scheme guarantees the reader always sees a complete FDT:
 * while the worker updates buf[inactive], buf[inuse] remains valid.
 *
 * inuse == -1 means no deferred update is pending (no hotplug since load).
 *
 * Buffer size is set at init time from the actual FDT segment memsz.
 */
static struct crash_fdt_buf {
	u8 *buf[2];
	size_t size;
	/* index of the last fully-written buffer; -1 if none */
	int inuse;
} fdt_buf;

static struct workqueue_struct *crash_fdt_wq;
#endif /* CONFIG_CRASH_HOTPLUG */

/*
 * The primary CPU waits a while for all secondary CPUs to enter. This is to
 * avoid sending an IPI if the secondary CPUs are entering
 * crash_kexec_secondary on their own (eg via a system reset).
 *
 * The secondary timeout has to be longer than the primary. Both timeouts are
 * in milliseconds.
 */
#define PRIMARY_TIMEOUT		500
#define SECONDARY_TIMEOUT	1000

#define IPI_TIMEOUT		10000
#define REAL_MODE_TIMEOUT	10000

static int time_to_dump;

/*
 * In case of system reset, secondary CPUs enter crash_kexec_secondary with out
 * having to send an IPI explicitly. So, indicate if the crash is via
 * system reset to avoid sending another IPI.
 */
static int is_via_system_reset;

/*
 * crash_wake_offline should be set to 1 by platforms that intend to wake
 * up offline cpus prior to jumping to a kdump kernel. Currently powernv
 * sets it to 1, since we want to avoid things from happening when an
 * offline CPU wakes up due to something like an HMI (malfunction error),
 * which propagates to all threads.
 */
int crash_wake_offline;

#define CRASH_HANDLER_MAX 3
/* List of shutdown handles */
static crash_shutdown_t crash_shutdown_handles[CRASH_HANDLER_MAX];
static DEFINE_SPINLOCK(crash_handlers_lock);

static unsigned long crash_shutdown_buf[JMP_BUF_LEN];
static int crash_shutdown_cpu = -1;

static int handle_fault(struct pt_regs *regs)
{
	if (crash_shutdown_cpu == smp_processor_id())
		longjmp(crash_shutdown_buf, 1);
	return 0;
}

#ifdef CONFIG_SMP

static atomic_t cpus_in_crash;
void crash_ipi_callback(struct pt_regs *regs)
{
	static cpumask_t cpus_state_saved = CPU_MASK_NONE;

	int cpu = smp_processor_id();

	hard_irq_disable();
	if (!cpumask_test_cpu(cpu, &cpus_state_saved)) {
		crash_save_cpu(regs, cpu);
		cpumask_set_cpu(cpu, &cpus_state_saved);
	}

	atomic_inc(&cpus_in_crash);
	smp_mb__after_atomic();

	/*
	 * Starting the kdump boot.
	 * This barrier is needed to make sure that all CPUs are stopped.
	 */
	while (!time_to_dump)
		cpu_relax();

	if (ppc_md.kexec_cpu_down)
		ppc_md.kexec_cpu_down(1, 1);

#ifdef CONFIG_PPC64
	kexec_smp_wait();
#else
	for (;;);	/* FIXME */
#endif

	/* NOTREACHED */
}

static void crash_kexec_prepare_cpus(void)
{
	unsigned int msecs;
	volatile unsigned int ncpus = num_online_cpus() - 1;/* Excluding the panic cpu */
	volatile int tries = 0;
	int (*old_handler)(struct pt_regs *regs);

	printk(KERN_EMERG "Sending IPI to other CPUs\n");

	if (crash_wake_offline)
		ncpus = num_present_cpus() - 1;

	/*
	 * If we came in via system reset, secondaries enter via crash_kexec_secondary().
	 * So, wait a while for the secondary CPUs to enter for that case.
	 * Else, send IPI to all other CPUs.
	 */
	if (is_via_system_reset)
		mdelay(PRIMARY_TIMEOUT);
	else
		crash_send_ipi(crash_ipi_callback);
	smp_wmb();

again:
	/*
	 * FIXME: Until we will have the way to stop other CPUs reliably,
	 * the crash CPU will send an IPI and wait for other CPUs to
	 * respond.
	 */
	msecs = IPI_TIMEOUT;
	while ((atomic_read(&cpus_in_crash) < ncpus) && (--msecs > 0))
		mdelay(1);

	/* Would it be better to replace the trap vector here? */

	if (atomic_read(&cpus_in_crash) >= ncpus) {
		printk(KERN_EMERG "IPI complete\n");
		return;
	}

	printk(KERN_EMERG "ERROR: %d cpu(s) not responding\n",
		ncpus - atomic_read(&cpus_in_crash));

	/*
	 * If we have a panic timeout set then we can't wait indefinitely
	 * for someone to activate system reset. We also give up on the
	 * second time through if system reset fail to work.
	 */
	if ((panic_timeout > 0) || (tries > 0))
		return;

	/*
	 * A system reset will cause all CPUs to take an 0x100 exception.
	 * The primary CPU returns here via setjmp, and the secondary
	 * CPUs reexecute the crash_kexec_secondary path.
	 */
	old_handler = __debugger;
	__debugger = handle_fault;
	crash_shutdown_cpu = smp_processor_id();

	if (setjmp(crash_shutdown_buf) == 0) {
		printk(KERN_EMERG "Activate system reset (dumprestart) "
				  "to stop other cpu(s)\n");

		/*
		 * A system reset will force all CPUs to execute the
		 * crash code again. We need to reset cpus_in_crash so we
		 * wait for everyone to do this.
		 */
		atomic_set(&cpus_in_crash, 0);
		smp_mb();

		while (atomic_read(&cpus_in_crash) < ncpus)
			cpu_relax();
	}

	crash_shutdown_cpu = -1;
	__debugger = old_handler;

	tries++;
	goto again;
}

/*
 * This function will be called by secondary cpus.
 */
void crash_kexec_secondary(struct pt_regs *regs)
{
	unsigned long flags;
	int msecs = SECONDARY_TIMEOUT;

	local_irq_save(flags);

	/* Wait for the primary crash CPU to signal its progress */
	while (crashing_cpu < 0) {
		if (--msecs < 0) {
			/* No response, kdump image may not have been loaded */
			local_irq_restore(flags);
			return;
		}

		mdelay(1);
	}

	crash_ipi_callback(regs);
}

#else	/* ! CONFIG_SMP */

static void crash_kexec_prepare_cpus(void)
{
	/*
	 * move the secondaries to us so that we can copy
	 * the new kernel 0-0x100 safely
	 *
	 * do this if kexec in setup.c ?
	 */
#ifdef CONFIG_PPC64
	smp_release_cpus();
#else
	/* FIXME */
#endif
}

void crash_kexec_secondary(struct pt_regs *regs)
{
}
#endif	/* CONFIG_SMP */

/* wait for all the CPUs to hit real mode but timeout if they don't come in */
#if defined(CONFIG_SMP) && defined(CONFIG_PPC64)
noinstr static void __maybe_unused crash_kexec_wait_realmode(int cpu)
{
	unsigned int msecs;
	int i;

	msecs = REAL_MODE_TIMEOUT;
	for (i=0; i < nr_cpu_ids && msecs > 0; i++) {
		if (i == cpu)
			continue;

		while (paca_ptrs[i]->kexec_state < KEXEC_STATE_REAL_MODE) {
			barrier();
			if (!cpu_possible(i) || !cpu_online(i) || (msecs <= 0))
				break;
			msecs--;
			mdelay(1);
		}
	}
	mb();
}
#else
static inline void crash_kexec_wait_realmode(int cpu) {}
#endif	/* CONFIG_SMP && CONFIG_PPC64 */

void crash_kexec_prepare(void)
{
	/* Avoid hardlocking with irresponsive CPU holding logbuf_lock */
	printk_deferred_enter();

	/*
	 * This function is only called after the system
	 * has panicked or is otherwise in a critical state.
	 * The minimum amount of code to allow a kexec'd kernel
	 * to run successfully needs to happen here.
	 *
	 * In practice this means stopping other cpus in
	 * an SMP system.
	 * The kernel is broken so disable interrupts.
	 */
	hard_irq_disable();

	/*
	 * Make a note of crashing cpu. Will be used in machine_kexec
	 * such that another IPI will not be sent.
	 */
	crashing_cpu = smp_processor_id();

	crash_kexec_prepare_cpus();
}

/*
 * Register a function to be called on shutdown.  Only use this if you
 * can't reset your device in the second kernel.
 */
int crash_shutdown_register(crash_shutdown_t handler)
{
	unsigned int i, rc;

	spin_lock(&crash_handlers_lock);
	for (i = 0 ; i < CRASH_HANDLER_MAX; i++)
		if (!crash_shutdown_handles[i]) {
			/* Insert handle at first empty entry */
			crash_shutdown_handles[i] = handler;
			rc = 0;
			break;
		}

	if (i == CRASH_HANDLER_MAX) {
		printk(KERN_ERR "Crash shutdown handles full, "
		       "not registered.\n");
		rc = 1;
	}

	spin_unlock(&crash_handlers_lock);
	return rc;
}
EXPORT_SYMBOL(crash_shutdown_register);

int crash_shutdown_unregister(crash_shutdown_t handler)
{
	unsigned int i, rc;

	spin_lock(&crash_handlers_lock);
	for (i = 0 ; i < CRASH_HANDLER_MAX; i++)
		if (crash_shutdown_handles[i] == handler)
			break;

	if (i == CRASH_HANDLER_MAX) {
		printk(KERN_ERR "Crash shutdown handle not found\n");
		rc = 1;
	} else {
		/* Shift handles down */
		for (; i < (CRASH_HANDLER_MAX - 1); i++)
			crash_shutdown_handles[i] =
				crash_shutdown_handles[i+1];
		/*
		 * Reset last entry to NULL now that it has been shifted down,
		 * this will allow new handles to be added here.
		 */
		crash_shutdown_handles[i] = NULL;
		rc = 0;
	}

	spin_unlock(&crash_handlers_lock);
	return rc;
}
EXPORT_SYMBOL(crash_shutdown_unregister);

#ifdef CONFIG_CRASH_HOTPLUG
/**
 * get_fdt_index - Loop through the kexec segment array and find
 *		   the index of the FDT segment.
 * @image: a pointer to kexec_crash_image
 *
 * Returns the index of FDT segment in the kexec segment array
 * if found; otherwise -1.
 *
 * Uses kmap_local_page() so it is safe regardless of where the segment
 * physically resides (including above the direct-map limit).
 */
static int get_fdt_index(struct kimage *image)
{
	int i, fdt_index = -1;

	for (i = 0; i < image->nr_segments; i++) {
		unsigned long mem = image->segment[i].mem;
		void *ptr = kmap_local_page(pfn_to_page(mem >> PAGE_SHIFT));

		if (ptr) {
			if (fdt_magic(ptr) == FDT_MAGIC)
				fdt_index = i;
			kunmap_local(ptr);
		}
		if (fdt_index >= 0)
			break;
	}

	return fdt_index;
}
#endif /* CONFIG_CRASH_HOTPLUG */

void default_machine_crash_shutdown(struct pt_regs *regs)
{
	volatile unsigned int i;
	int (*old_handler)(struct pt_regs *regs);

	if (TRAP(regs) == INTERRUPT_SYSTEM_RESET)
		is_via_system_reset = 1;

	if (IS_ENABLED(CONFIG_SMP))
		crash_smp_send_stop();
	else
		crash_kexec_prepare();

	crash_save_cpu(regs, crashing_cpu);

	time_to_dump = 1;

	crash_kexec_wait_realmode(crashing_cpu);

	machine_kexec_mask_interrupts();

#ifdef CONFIG_CRASH_HOTPLUG
	/*
	 * Drain any pending deferred FDT update before jumping.  The ordered
	 * workqueue ensures the worker has finished writing fdt_buf.buf[inuse]
	 * before flush_workqueue() returns.
	 */
	if (crash_fdt_wq)
		flush_workqueue(crash_fdt_wq);

	/*
	 * Publish the latest deferred FDT buffer into the kexec segment.
	 * smp_load_acquire() pairs with smp_store_release() in the worker.
	 */
	{
		int pending = smp_load_acquire(&fdt_buf.inuse);

		if (pending != -1) {
			int fdt_index = get_fdt_index(kexec_crash_image);

			if (fdt_index >= 0) {
				void *dst = __va(kexec_crash_image->segment[fdt_index].mem);

				memcpy(dst, fdt_buf.buf[pending], fdt_buf.size);
			} else {
				pr_err("crash_shutdown: FDT segment not found, kdump FDT may be stale\n");
			}
		}
	}
#endif /* CONFIG_CRASH_HOTPLUG */

	/*
	 * Call registered shutdown routines safely.  Swap out
	 * __debugger_fault_handler, and replace on exit.
	 */
	old_handler = __debugger_fault_handler;
	__debugger_fault_handler = handle_fault;
	crash_shutdown_cpu = smp_processor_id();
	for (i = 0; i < CRASH_HANDLER_MAX && crash_shutdown_handles[i]; i++) {
		if (setjmp(crash_shutdown_buf) == 0) {
			/*
			 * Insert syncs and delay to ensure
			 * instructions in the dangerous region don't
			 * leak away from this protected region.
			 */
			asm volatile("sync; isync");
			/* dangerous region */
			crash_shutdown_handles[i]();
			asm volatile("sync; isync");
		}
	}
	crash_shutdown_cpu = -1;
	__debugger_fault_handler = old_handler;

	if (ppc_md.kexec_cpu_down)
		ppc_md.kexec_cpu_down(1, 0);
}

#ifdef CONFIG_CRASH_DUMP
/**
 * sync_backup_region_phdr - synchronize backup region offset between
 *			    kexec image and ELF core header.
 * @image: Kexec image.
 * @ehdr: ELF core header.
 * @phdr_to_kimage: If true, read the offset from the ELF program header
 *		    and update the kimage backup region. If false, update
 *		    the ELF program header offset from the kimage backup
 *		    region.
 *
 * Note: During kexec_load, this is called with phdr_to_kimage = true. For
 * kexec_file_load and ELF core header recreation during memory hotplug
 * events, it is called with phdr_to_kimage = false.
 *
 * Returns nothing.
 */
void sync_backup_region_phdr(struct kimage *image, Elf64_Ehdr *ehdr, bool phdr_to_kimage)
{
	Elf64_Phdr *phdr;
	unsigned int i;

	phdr = (Elf64_Phdr *)(ehdr + 1);
	for (i = 0; i < ehdr->e_phnum; i++, phdr++) {
		if (phdr->p_paddr == BACKUP_SRC_START) {
			if (phdr_to_kimage)
				image->arch.backup_start = phdr->p_offset;
			else
				phdr->p_offset = image->arch.backup_start;

			kexec_dprintk("Backup region offset updated to 0x%lx\n",
				      image->arch.backup_start);
			return;
		}
	}
}
#endif /* CONFIG_CRASH_DUMP */

#ifdef CONFIG_CRASH_HOTPLUG

int machine_kexec_post_load(struct kimage *image)
{
	int i;
	unsigned long mem;
	unsigned char *ptr;

	if (image->type != KEXEC_TYPE_CRASH)
		return 0;

	if (image->file_mode)
		return 0;

	for (i = 0; i < image->nr_segments; i++) {
		mem = image->segment[i].mem;
		ptr = (char *)__va(mem);

		if (ptr && memcmp(ptr, ELFMAG, SELFMAG) == 0)
			sync_backup_region_phdr(image, (Elf64_Ehdr *) ptr, true);
	}
	return 0;
}

#undef pr_fmt
#define pr_fmt(fmt) "crash hp: " fmt

/*
 * Advertise preferred elfcorehdr size to userspace via
 * /sys/kernel/crash_elfcorehdr_size sysfs interface.
 */
unsigned int arch_crash_get_elfcorehdr_size(void)
{
	unsigned long phdr_cnt;

	/* A program header for possible CPUs + vmcoreinfo */
	phdr_cnt = num_possible_cpus() + 1;
	if (IS_ENABLED(CONFIG_MEMORY_HOTPLUG))
		phdr_cnt += CONFIG_CRASH_MAX_MEMORY_RANGES;

	return sizeof(struct elfhdr) + (phdr_cnt * sizeof(Elf64_Phdr));
}

/**
 * update_crash_elfcorehdr() - Recreate the elfcorehdr and replace it with old
 *			       elfcorehdr in the kexec segment array.
 * @image: the active struct kimage
 * @mn: struct memory_notify data handler
 */
static void update_crash_elfcorehdr(struct kimage *image, struct memory_notify *mn)
{
	int ret;
	struct crash_mem *cmem = NULL;
	struct kexec_segment *ksegment;
	void *ptr, *mem, *elfbuf = NULL;
	unsigned long elfsz, memsz, base_addr, size;

	ksegment = &image->segment[image->elfcorehdr_index];
	mem = (void *) ksegment->mem;
	memsz = ksegment->memsz;

	ret = get_crash_memory_ranges(&cmem);
	if (ret) {
		pr_err("Failed to get crash mem range\n");
		return;
	}

	/*
	 * The hot unplugged memory is part of crash memory ranges,
	 * remove it here.
	 */
	if (image->hp_action == KEXEC_CRASH_HP_REMOVE_MEMORY) {
		base_addr = PFN_PHYS(mn->start_pfn);
		size = mn->nr_pages * PAGE_SIZE;
		ret = remove_mem_range(&cmem, base_addr, size);
		if (ret) {
			pr_err("Failed to remove hot-unplugged memory from crash memory ranges\n");
			goto out;
		}
	}

	ret = crash_prepare_elf64_headers(cmem, false, &elfbuf, &elfsz);
	if (ret) {
		pr_err("Failed to prepare elf header\n");
		goto out;
	}

	/*
	 * It is unlikely that kernel hit this because elfcorehdr kexec
	 * segment (memsz) is built with addition space to accommodate growing
	 * number of crash memory ranges while loading the kdump kernel. It is
	 * Just to avoid any unforeseen case.
	 */
	if (elfsz > memsz) {
		pr_err("Updated crash elfcorehdr elfsz %lu > memsz %lu", elfsz, memsz);
		goto out;
	}

	sync_backup_region_phdr(image, (Elf64_Ehdr *) elfbuf, false);

	ptr = __va(mem);
	if (ptr) {
		/* Temporarily invalidate the crash image while it is replaced */
		xchg(&kexec_crash_image, NULL);

		/* Replace the old elfcorehdr with newly prepared elfcorehdr */
		memcpy((void *)ptr, elfbuf, elfsz);

		/* The crash image is now valid once again */
		xchg(&kexec_crash_image, image);
	}
out:
	kvfree(cmem);
	kvfree(elfbuf);
}

/*
 * crash_fdt_buf_get_scratch - return the index of the buffer to write into next.
 *
 * The ordered workqueue serialises callers, so no additional locking is
 * needed here.  On the first call (inuse == -1) buf[0] is seeded from the
 * live kexec segment so subsequent update_cpus_node() calls work on a
 * complete FDT.  On every subsequent call the *other* buffer is seeded from
 * the last published one, so the reader always has a complete copy.
 *
 * Returns the scratch buffer index, or -1 on error (FDT segment not found).
 */
static int crash_fdt_buf_get_scratch(void)
{
	int cur = smp_load_acquire(&fdt_buf.inuse);

	if (cur != -1) {
		int next = 1 - cur;

		memcpy(fdt_buf.buf[next], fdt_buf.buf[cur], fdt_buf.size);
		return next;
	}

	/* First update: seed buf[0] from the live kexec segment. */
	{
		int fdt_index = get_fdt_index(kexec_crash_image);

		if (fdt_index < 0) {
			pr_err("crash_fdt: FDT segment not found, cannot update\n");
			return -1;
		}
		memcpy(fdt_buf.buf[0],
		       __va(kexec_crash_image->segment[fdt_index].mem),
		       fdt_buf.size);
	}
	return 0;
}

/*
 * crash_fdt_deferred_work - workqueue handler for CPU hotplug FDT updates.
 *
 * Runs in the ordered crash_fdt_wq, so only one instance is active at a
 * time.  Writes into the inactive buffer then atomically publishes it via
 * smp_store_release so the crash shutdown reader always sees a complete FDT.
 */
static void crash_fdt_deferred_work(struct work_struct *work)
{
	int bufidx = crash_fdt_buf_get_scratch();

	if (bufidx < 0)
		goto out;

	if (update_cpus_node(fdt_buf.buf[bufidx]))
		pr_err("crash_fdt: failed to update cpus node\n");
	else
		smp_store_release(&fdt_buf.inuse, bufidx);
out:
	kfree(work);
}

/**
 * crash_fdt_deferred_update - queue a deferred FDT update for CPU hotplug.
 *
 * Called from arch_crash_handle_hotplug_event() on the DLPAR notifier path.
 * Returns immediately without taking any lock.  The ordered workqueue
 * serialises concurrent events and the crash shutdown path flushes it
 * before jumping.
 */
static void crash_fdt_deferred_update(void)
{
	struct work_struct *work;

	if (!crash_fdt_wq)
		return;

	work = kmalloc(sizeof(*work), GFP_KERNEL);
	if (!work) {
		pr_err("crash_fdt: failed to allocate work item, kdump FDT may be stale\n");
		return;
	}
	INIT_WORK(work, crash_fdt_deferred_work);
	queue_work(crash_fdt_wq, work);
}

/**
 * crash_fdt_buf_init - initialise the deferred-update workqueue and buffers.
 *
 * Called once at boot via arch_initcall.  Must run after the workqueue
 * subsystem is up.  Buffer memory is allocated here; fdt_buf.size is
 * filled in lazily on the first hotplug event from the live segment memsz.
 */
static int __init crash_fdt_buf_init(void)
{
	fdt_buf.inuse = -1;
	fdt_buf.size  = 0;
	fdt_buf.buf[0] = NULL;
	fdt_buf.buf[1] = NULL;
	crash_fdt_wq = alloc_ordered_workqueue("crash_fdt_wq", 0);
	if (!crash_fdt_wq)
		pr_err("crash_fdt: failed to create workqueue\n");
	return 0;
}
arch_initcall(crash_fdt_buf_init);

int arch_crash_hotplug_support(struct kimage *image, unsigned long kexec_flags)
{
#ifdef CONFIG_KEXEC_FILE
	if (image->file_mode)
		return 1;
#endif
	return kexec_flags & KEXEC_CRASH_HOTPLUG_SUPPORT;
}

/**
 * arch_crash_handle_hotplug_event - Handle crash CPU/Memory hotplug events.
 * @image: a pointer to kexec_crash_image
 * @arg: struct memory_notify handler for memory hotplug; NULL for CPU hotplug.
 *
 * CPU add: queue a deferred FDT update via the ordered crash_fdt_wq.
 *          The crash shutdown path flushes the queue and copies the latest
 *          buffer into the kexec segment before jumping.
 * CPU remove: no FDT action — offline CPUs in the FDT are harmless.
 * Memory add/remove: update elfcorehdr in place (called under kexec_lock).
 */
void arch_crash_handle_hotplug_event(struct kimage *image, void *arg)
{
	switch (image->hp_action) {
	case KEXEC_CRASH_HP_REMOVE_CPU:
		return;

	case KEXEC_CRASH_HP_ADD_CPU:
		/*
		 * Allocate the FDT buffers on the first CPU-add event so we
		 * know the real segment size.  Safe here because we are on
		 * the DLPAR notifier path (process context, GFP_KERNEL ok).
		 */
		if (!fdt_buf.buf[0]) {
			int idx = get_fdt_index(image);

			if (idx < 0) {
				pr_err("crash hp: FDT segment not found\n");
				return;
			}
			fdt_buf.size = image->segment[idx].memsz;
			fdt_buf.buf[0] = kmalloc(fdt_buf.size, GFP_KERNEL);
			fdt_buf.buf[1] = kmalloc(fdt_buf.size, GFP_KERNEL);
			if (!fdt_buf.buf[0] || !fdt_buf.buf[1]) {
				pr_err("crash hp: failed to allocate FDT buffers\n");
				kfree(fdt_buf.buf[0]);
				kfree(fdt_buf.buf[1]);
				fdt_buf.buf[0] = fdt_buf.buf[1] = NULL;
				return;
			}
		}
		crash_fdt_deferred_update();
		break;

	case KEXEC_CRASH_HP_REMOVE_MEMORY:
	case KEXEC_CRASH_HP_ADD_MEMORY: {
		struct memory_notify *mn = (struct memory_notify *)arg;

		update_crash_elfcorehdr(image, mn);
		break;
	}
	default:
		pr_warn_once("Unknown hotplug action\n");
	}
}
#endif /* CONFIG_CRASH_HOTPLUG */
