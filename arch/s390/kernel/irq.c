// SPDX-License-Identifier: GPL-2.0
/*
 *    Copyright IBM Corp. 2004, 2011
 *    Author(s): Martin Schwidefsky <schwidefsky@de.ibm.com>,
 *		 Holger Smolinski <Holger.Smolinski@de.ibm.com>,
 *		 Thomas Spatzier <tspat@de.ibm.com>,
 *
 * This file contains interrupt related functions.
 */

#include <linux/kernel_stat.h>
#include <linux/cpufeature.h>
#include <linux/interrupt.h>
#include <linux/seq_file.h>
#include <linux/proc_fs.h>
#include <linux/profile.h>
#include <linux/export.h>
#include <linux/kernel.h>
#include <linux/ftrace.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/irq.h>
#include <linux/entry-common.h>
#include <asm/irq_regs.h>
#include <asm/cputime.h>
#include <asm/lowcore.h>
#include <asm/machine.h>
#include <asm/irq.h>
#include <asm/hw_irq.h>
#include <asm/stacktrace.h>
#include <asm/softirq_stack.h>
#include <asm/vtime.h>
#include <asm/asm.h>
#include "entry.h"

DEFINE_PER_CPU_SHARED_ALIGNED(struct irq_stat, irq_stat);
EXPORT_PER_CPU_SYMBOL_GPL(irq_stat);

struct irq_class {
	int irq;
	char *name;
	char *desc;
};

/*
 * The list of "main" irq classes on s390. This is the list of interrupts
 * that appear both in /proc/stat ("intr" line) and /proc/interrupts.
 * Historically only external and I/O interrupts have been part of /proc/stat.
 * We can't add the split external and I/O sub classes since the first field
 * in the "intr" line in /proc/stat is supposed to be the sum of all other
 * fields.
 * Since the external and I/O interrupt fields are already sums we would end
 * up with having a sum which accounts each interrupt twice.
 */
static const struct irq_class irqclass_main_desc[NR_IRQS_BASE] = {
	{.irq = EXT_INTERRUPT,	.name = "EXT"},
	{.irq = IO_INTERRUPT,	.name = "I/O"},
	{.irq = THIN_INTERRUPT, .name = "AIO"},
};

/*
 * The list of split external and I/O interrupts that appear only in
 * /proc/interrupts.
 * In addition this list contains non external / I/O events like NMIs.
 */
static const struct irq_class irqclass_sub_desc[] = {
	{.irq = IRQEXT_CLK, .name = "CLK", .desc = "[EXT] Clock Comparator"},
	{.irq = IRQEXT_EXC, .name = "EXC", .desc = "[EXT] External Call"},
	{.irq = IRQEXT_EMS, .name = "EMS", .desc = "[EXT] Emergency Signal"},
	{.irq = IRQEXT_TMR, .name = "TMR", .desc = "[EXT] CPU Timer"},
	{.irq = IRQEXT_TLA, .name = "TAL", .desc = "[EXT] Timing Alert"},
	{.irq = IRQEXT_PFL, .name = "PFL", .desc = "[EXT] Pseudo Page Fault"},
	{.irq = IRQEXT_DSD, .name = "DSD", .desc = "[EXT] DASD Diag"},
	{.irq = IRQEXT_VRT, .name = "VRT", .desc = "[EXT] Virtio"},
	{.irq = IRQEXT_SCP, .name = "SCP", .desc = "[EXT] Service Call"},
	{.irq = IRQEXT_IUC, .name = "IUC", .desc = "[EXT] IUCV"},
	{.irq = IRQEXT_CMS, .name = "CMS", .desc = "[EXT] CPU-Measurement: Sampling"},
	{.irq = IRQEXT_CMC, .name = "CMC", .desc = "[EXT] CPU-Measurement: Counter"},
	{.irq = IRQEXT_FTP, .name = "FTP", .desc = "[EXT] HMC FTP Service"},
	{.irq = IRQEXT_WTI, .name = "WTI", .desc = "[EXT] Warning Track"},
	{.irq = IRQIO_CIO,  .name = "CIO", .desc = "[I/O] Common I/O Layer Interrupt"},
	{.irq = IRQIO_DAS,  .name = "DAS", .desc = "[I/O] DASD"},
	{.irq = IRQIO_C15,  .name = "C15", .desc = "[I/O] 3215"},
	{.irq = IRQIO_C70,  .name = "C70", .desc = "[I/O] 3270"},
	{.irq = IRQIO_TAP,  .name = "TAP", .desc = "[I/O] Tape"},
	{.irq = IRQIO_VMR,  .name = "VMR", .desc = "[I/O] Unit Record Devices"},
	{.irq = IRQIO_CTC,  .name = "CTC", .desc = "[I/O] CTC"},
	{.irq = IRQIO_ADM,  .name = "ADM", .desc = "[I/O] EADM Subchannel"},
	{.irq = IRQIO_CSC,  .name = "CSC", .desc = "[I/O] CHSC Subchannel"},
	{.irq = IRQIO_VIR,  .name = "VIR", .desc = "[I/O] Virtual I/O Devices"},
	{.irq = IRQIO_QAI,  .name = "QAI", .desc = "[AIO] QDIO Adapter Interrupt"},
	{.irq = IRQIO_APB,  .name = "APB", .desc = "[AIO] AP Bus"},
	{.irq = IRQIO_PCF,  .name = "PCF", .desc = "[AIO] PCI Floating Interrupt"},
	{.irq = IRQIO_PCD,  .name = "PCD", .desc = "[AIO] PCI Directed Interrupt"},
	{.irq = IRQIO_MSI,  .name = "MSI", .desc = "[AIO] MSI Interrupt"},
	{.irq = IRQIO_VAI,  .name = "VAI", .desc = "[AIO] Virtual I/O Devices AI"},
	{.irq = IRQIO_GAL,  .name = "GAL", .desc = "[AIO] GIB Alert"},
	{.irq = NMI_NMI,    .name = "NMI", .desc = "[NMI] Machine Check"},
	{.irq = CPU_RST,    .name = "RST", .desc = "[CPU] CPU Restart"},
};

static void do_IRQ(struct pt_regs *regs, int irq)
{
	if (tod_after_eq(get_lowcore()->int_clock,
			 get_lowcore()->clock_comparator))
		/* Serve timer interrupts first. */
		clock_comparator_work();
	generic_handle_irq(irq);
}

static int on_async_stack(void)
{
	unsigned long frame = current_frame_address();

	return ((get_lowcore()->async_stack ^ frame) & ~(THREAD_SIZE - 1)) == 0;
}

static void do_irq_async(struct pt_regs *regs, int irq)
{
	if (on_async_stack()) {
		do_IRQ(regs, irq);
	} else {
		call_on_stack(2, get_lowcore()->async_stack, void, do_IRQ,
			      struct pt_regs *, regs, int, irq);
	}
}

static int irq_pending(struct pt_regs *regs)
{
	int cc;

	asm volatile(
		"	tpi	 0\n"
		CC_IPM(cc)
		: CC_OUT(cc, cc)
		:
		: CC_CLOBBER);
	return CC_TRANSFORM(cc);
}

void noinstr do_io_irq(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);
	struct pt_regs *old_regs = set_irq_regs(regs);
	bool from_idle;

	irq_enter_rcu();

	if (user_mode(regs)) {
		update_timer_sys();
		if (cpu_has_bear())
			current->thread.last_break = regs->last_break;
	}

	from_idle = test_and_clear_cpu_flag(CIF_ENABLED_WAIT);
	if (from_idle)
		account_idle_time_irq();

	set_cpu_flag(CIF_NOHZ_DELAY);
	do {
		regs->tpi_info = get_lowcore()->tpi_info;
		if (get_lowcore()->tpi_info.adapter_IO)
			do_irq_async(regs, THIN_INTERRUPT);
		else
			do_irq_async(regs, IO_INTERRUPT);
	} while (machine_is_lpar() && irq_pending(regs));

	irq_exit_rcu();

	set_irq_regs(old_regs);
	irqentry_exit(regs, state);

	if (from_idle)
		regs->psw.mask &= ~(PSW_MASK_EXT | PSW_MASK_IO | PSW_MASK_WAIT);
}

void noinstr do_ext_irq(struct pt_regs *regs)
{
	irqentry_state_t state = irqentry_enter(regs);
	struct pt_regs *old_regs = set_irq_regs(regs);
	bool from_idle;

	irq_enter_rcu();

	if (user_mode(regs)) {
		update_timer_sys();
		if (cpu_has_bear())
			current->thread.last_break = regs->last_break;
	}

	regs->int_code = get_lowcore()->ext_int_code_addr;
	regs->int_parm = get_lowcore()->ext_params;
	regs->int_parm_long = get_lowcore()->ext_params2;

	from_idle = test_and_clear_cpu_flag(CIF_ENABLED_WAIT);
	if (from_idle)
		account_idle_time_irq();

	do_irq_async(regs, EXT_INTERRUPT);

	irq_exit_rcu();
	set_irq_regs(old_regs);
	irqentry_exit(regs, state);

	if (from_idle)
		regs->psw.mask &= ~(PSW_MASK_EXT | PSW_MASK_IO | PSW_MASK_WAIT);
}

static void show_msi_interrupt(struct seq_file *p, int irq)
{
	struct irq_desc *desc;
	unsigned long flags;
	int cpu;

	rcu_read_lock();
	desc = irq_to_desc(irq);
	if (!desc)
		goto out;

	raw_spin_lock_irqsave(&desc->lock, flags);
	seq_printf(p, "%3d: ", irq);
	for_each_online_cpu(cpu)
		seq_printf(p, "%10u ", irq_desc_kstat_cpu(desc, cpu));

	if (desc->irq_data.chip)
		seq_printf(p, " %8s", desc->irq_data.chip->name);

	if (desc->action)
		seq_printf(p, "  %s", desc->action->name);

	seq_putc(p, '\n');
	raw_spin_unlock_irqrestore(&desc->lock, flags);
out:
	rcu_read_unlock();
}

/*
 * show_interrupts is needed by /proc/interrupts.
 */
int show_interrupts(struct seq_file *p, void *v)
{
	int index = *(loff_t *) v;
	int cpu, irq;

	cpus_read_lock();
	if (index == 0) {
		seq_puts(p, "           ");
		for_each_online_cpu(cpu)
			seq_printf(p, "CPU%-8d", cpu);
		seq_putc(p, '\n');
	}
	if (index < NR_IRQS_BASE) {
		seq_printf(p, "%s: ", irqclass_main_desc[index].name);
		irq = irqclass_main_desc[index].irq;
		for_each_online_cpu(cpu)
			seq_printf(p, "%10u ", kstat_irqs_cpu(irq, cpu));
		seq_putc(p, '\n');
		goto out;
	}
	if (index < irq_get_nr_irqs()) {
		show_msi_interrupt(p, index);
		goto out;
	}
	for (index = 0; index < NR_ARCH_IRQS; index++) {
		seq_printf(p, "%s: ", irqclass_sub_desc[index].name);
		irq = irqclass_sub_desc[index].irq;
		for_each_online_cpu(cpu)
			seq_printf(p, "%10u ",
				   per_cpu(irq_stat, cpu).irqs[irq]);
		if (irqclass_sub_desc[index].desc)
			seq_printf(p, "  %s", irqclass_sub_desc[index].desc);
		seq_putc(p, '\n');
	}
out:
	cpus_read_unlock();
	return 0;
}

unsigned int arch_dynirq_lower_bound(unsigned int from)
{
	return from < NR_IRQS_BASE ? NR_IRQS_BASE : from;
}

/*
 * ext_int_hash[index] is the list head for all external interrupts that hash
 * to this index.
 */
static struct hlist_head ext_int_hash[32] ____cacheline_aligned;

struct ext_int_info {
	ext_int_handler_t handler;
	struct hlist_node entry;
	struct rcu_head rcu;
	u16 code;
};

/* ext_int_hash_lock protects the handler lists for external interrupts */
static DEFINE_SPINLOCK(ext_int_hash_lock);

static inline int ext_hash(u16 code)
{
	BUILD_BUG_ON(!is_power_of_2(ARRAY_SIZE(ext_int_hash)));

	return (code + (code >> 9)) & (ARRAY_SIZE(ext_int_hash) - 1);
}

int register_external_irq(u16 code, ext_int_handler_t handler)
{
	struct ext_int_info *p;
	unsigned long flags;
	int index;

	p = kmalloc(sizeof(*p), GFP_ATOMIC);
	if (!p)
		return -ENOMEM;
	p->code = code;
	p->handler = handler;
	index = ext_hash(code);

	spin_lock_irqsave(&ext_int_hash_lock, flags);
	hlist_add_head_rcu(&p->entry, &ext_int_hash[index]);
	spin_unlock_irqrestore(&ext_int_hash_lock, flags);
	return 0;
}
EXPORT_SYMBOL(register_external_irq);

int unregister_external_irq(u16 code, ext_int_handler_t handler)
{
	struct ext_int_info *p;
	unsigned long flags;
	int index = ext_hash(code);

	spin_lock_irqsave(&ext_int_hash_lock, flags);
	hlist_for_each_entry_rcu(p, &ext_int_hash[index], entry) {
		if (p->code == code && p->handler == handler) {
			hlist_del_rcu(&p->entry);
			kfree_rcu(p, rcu);
		}
	}
	spin_unlock_irqrestore(&ext_int_hash_lock, flags);
	return 0;
}
EXPORT_SYMBOL(unregister_external_irq);

static irqreturn_t do_ext_interrupt(int irq, void *dummy)
{
	struct pt_regs *regs = get_irq_regs();
	struct ext_code ext_code;
	struct ext_int_info *p;
	int index;

	ext_code.int_code = regs->int_code;
	if (ext_code.code != EXT_IRQ_CLK_COMP)
		set_cpu_flag(CIF_NOHZ_DELAY);

	index = ext_hash(ext_code.code);
	rcu_read_lock();
	hlist_for_each_entry_rcu(p, &ext_int_hash[index], entry) {
		if (unlikely(p->code != ext_code.code))
			continue;
		p->handler(ext_code, regs->int_parm, regs->int_parm_long);
	}
	rcu_read_unlock();
	return IRQ_HANDLED;
}

static void __init init_ext_interrupts(void)
{
	int idx;

	for (idx = 0; idx < ARRAY_SIZE(ext_int_hash); idx++)
		INIT_HLIST_HEAD(&ext_int_hash[idx]);

	irq_set_chip_and_handler(EXT_INTERRUPT,
				 &dummy_irq_chip, handle_percpu_irq);
	if (request_irq(EXT_INTERRUPT, do_ext_interrupt, 0, "EXT", NULL))
		panic("Failed to register EXT interrupt\n");
}

void __init init_IRQ(void)
{
	BUILD_BUG_ON(ARRAY_SIZE(irqclass_sub_desc) != NR_ARCH_IRQS);
	init_cio_interrupts();
	init_airq_interrupts();
	init_ext_interrupts();
}

static DEFINE_SPINLOCK(irq_subclass_lock);
static unsigned char irq_subclass_refcount[64];

void irq_subclass_register(enum irq_subclass subclass)
{
	spin_lock(&irq_subclass_lock);
	if (!irq_subclass_refcount[subclass])
		system_ctl_set_bit(0, subclass);
	irq_subclass_refcount[subclass]++;
	spin_unlock(&irq_subclass_lock);
}
EXPORT_SYMBOL(irq_subclass_register);

void irq_subclass_unregister(enum irq_subclass subclass)
{
	spin_lock(&irq_subclass_lock);
	irq_subclass_refcount[subclass]--;
	if (!irq_subclass_refcount[subclass])
		system_ctl_clear_bit(0, subclass);
	spin_unlock(&irq_subclass_lock);
}
EXPORT_SYMBOL(irq_subclass_unregister);
