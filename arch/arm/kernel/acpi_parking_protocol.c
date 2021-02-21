// SPDX-License-Identifier: GPL-2.0-only
/*
 * ARM32 ACPI Parking Protocol implementation
 *
 * Based on ARM64 Parking Protocol implementation by
 *	    Lorenzo Pieralisi <lorenzo.pieralisi@arm.com>
 *	    Mark Salter <msalter@redhat.com>
 */
#include <linux/acpi.h>
#include <linux/mm.h>
#include <linux/types.h>
#include <linux/io.h>
#include <linux/init.h>
#include <linux/smp.h>
#include <linux/of.h>
#include <linux/delay.h>

#include <asm/smp_plat.h>

struct parking_protocol_mailbox {
	__le32 cpu_id;
	__le32 reserved;
	__le64 entry_point;
};

struct cpu_mailbox_entry {
	struct parking_protocol_mailbox __iomem *mailbox;
	phys_addr_t mailbox_addr;
	u8 version;
	u8 gic_cpu_id;
};

static struct cpu_mailbox_entry cpu_mailbox_entries[NR_CPUS];

static void acpi_parking_protocol_cpu_init(void)
{
	pr_debug("%s: hardcoding MADT table for ARM32 WinRT\n", __func__);

	cpu_mailbox_entries[0].gic_cpu_id = 0;
	cpu_mailbox_entries[0].version = 1;
	cpu_mailbox_entries[0].mailbox_addr = 0x82001000;

	cpu_mailbox_entries[1].gic_cpu_id = 1;
	cpu_mailbox_entries[1].version = 1;
	cpu_mailbox_entries[1].mailbox_addr = 0x82002000;

	cpu_mailbox_entries[2].gic_cpu_id = 2;
	cpu_mailbox_entries[2].version = 1;
	cpu_mailbox_entries[2].mailbox_addr = 0x82003000;

	cpu_mailbox_entries[3].gic_cpu_id = 3;
	cpu_mailbox_entries[3].version = 1;
	cpu_mailbox_entries[3].mailbox_addr = 0x82004000;
}

extern void secondary_startup(void);

static int acpi_parking_protocol_cpu_boot(unsigned int cpu, struct task_struct *idle)
{
	struct cpu_mailbox_entry *cpu_entry = &cpu_mailbox_entries[cpu];
	struct parking_protocol_mailbox __iomem *mailbox;
	u32 cpu_id;

	/*
	 * Map mailbox memory with attribute device nGnRE (ie ioremap -
	 * this deviates from the parking protocol specifications since
	 * the mailboxes are required to be mapped nGnRnE; the attribute
	 * discrepancy is harmless insofar as the protocol specification
	 * is concerned).
	 * If the mailbox is mistakenly allocated in the linear mapping
	 * by FW ioremap will fail since the mapping will be prevented
	 * by the kernel (it clashes with the linear mapping attributes
	 * specifications).
	 */
	mailbox = ioremap(cpu_entry->mailbox_addr, sizeof(*mailbox));
	if (!mailbox)
		return -EIO;

	cpu_id = readl_relaxed(&mailbox->cpu_id);
	/*
	 * Check if firmware has set-up the mailbox entry properly
	 * before kickstarting the respective cpu.
	 */
	if (cpu_id != ~0U) {
		iounmap(mailbox);
		return -ENXIO + 1000;
	}

	/*
	 * stash the mailbox address mapping to use it for further FW
	 * checks in the postboot method
	 */
	cpu_entry->mailbox = mailbox;

	/*
	 * We write the entry point and cpu id as LE regardless of the
	 * native endianness of the kernel. Therefore, any boot-loaders
	 * that read this address need to convert this address to the
	 * Boot-Loader's endianness before jumping.
	 */
	writel_relaxed(virt_to_idmap(&secondary_startup),
		       &mailbox->entry_point);
	writel_relaxed(cpu_entry->gic_cpu_id, &mailbox->cpu_id);

	arch_send_wakeup_ipi_mask(cpumask_of(cpu));

	return 0;
}

static void acpi_parking_protocol_cpu_postboot(unsigned int cpu)
{
	struct cpu_mailbox_entry *cpu_entry = &cpu_mailbox_entries[cpu];
	struct parking_protocol_mailbox __iomem *mailbox = cpu_entry->mailbox;
	u64 entry_point;

	entry_point = readl_relaxed(&mailbox->entry_point);
	/*
	 * Check if firmware has cleared the entry_point as expected
	 * by the protocol specification.
	 */
	WARN_ON(entry_point);
}

const struct smp_operations acpi_parking_protocol_ops = {
	.smp_init_cpus		= acpi_parking_protocol_cpu_init,
	.smp_boot_secondary	= acpi_parking_protocol_cpu_boot,
	.smp_secondary_init	= acpi_parking_protocol_cpu_postboot,
};
