/* Copyright (c) 2010-2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/sysrq.h>
#include <linux/time.h>
#include <linux/proc_fs.h>
#include <linux/kernel_stat.h>
#include <linux/uaccess.h>
#include <linux/sysdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/semaphore.h>
#include <linux/file.h>
#include <linux/percpu.h>
#include <linux/string.h>
#include <linux/smp.h>
#include <asm/cacheflush.h>
#include <asm/smp_plat.h>
#include <asm/mmu_writeable.h>

#ifdef CONFIG_ARCH_MSM_KRAIT
#include <mach/msm-krait-l2-accessors.h>
#endif

#define TYPE_MAX_CHARACTERS 10

struct cp_params {
	unsigned long il2index;
	unsigned long cp;
	unsigned long op1;
	unsigned long op2;
	unsigned long crn;
	unsigned long crm;
	unsigned long write_value;
	char rw;
};

static struct semaphore cp_sem;
static unsigned long il2_output;
static int cpu;
char type[TYPE_MAX_CHARACTERS] = "C";

static DEFINE_PER_CPU(struct cp_params, cp_param)
	 = { 0, 15, 0, 0, 0, 0, 0, 'r' };

static struct sysdev_class cpaccess_sysclass = {
	.name = "cpaccess",
};

void cpaccess_dummy_inst(void);

#ifdef CONFIG_ARCH_MSM_KRAIT
static void do_read_il2(void *ret)
{
	*(unsigned long *)ret =
		get_l2_indirect_reg(per_cpu(cp_param.il2index, cpu));
}

static void do_write_il2(void *ret)
{
	*(unsigned long *)ret =
		set_get_l2_indirect_reg(per_cpu(cp_param.il2index, cpu),
				per_cpu(cp_param.write_value, cpu));
}

static int do_il2_rw(char *str_tmp)
{
	unsigned long write_value, il2index;
	char rw;
	int ret = 0;

	il2index = 0;
	sscanf(str_tmp, "%lx:%c:%lx:%d", &il2index, &rw, &write_value,
								&cpu);
	per_cpu(cp_param.il2index, cpu) = il2index;
	per_cpu(cp_param.rw, cpu) = rw;
	per_cpu(cp_param.write_value, cpu) = write_value;

	if (per_cpu(cp_param.rw, cpu) == 'r') {
		if (is_smp()) {
			if (smp_call_function_single(cpu, do_read_il2,
							&il2_output, 1))
				pr_err("Error cpaccess smp call single\n");
		} else
			do_read_il2(&il2_output);
	} else if (per_cpu(cp_param.rw, cpu) == 'w') {
		if (is_smp()) {
			if (smp_call_function_single(cpu, do_write_il2,
							&il2_output, 1))
				pr_err("Error cpaccess smp call single\n");
		} else
			do_write_il2(&il2_output);
	} else {
			pr_err("cpaccess: Wrong Entry for 'r' or 'w'.\n");
			return -EINVAL;
	}
	return ret;
}
#else
static void do_il2_rw(char *str_tmp)
{
	il2_output = 0;
}
#endif

static noinline unsigned long cpaccess_dummy(unsigned long write_val)
{
	unsigned long ret = 0xBEEF;

	asm volatile (".globl cpaccess_dummy_inst\n"
			"cpaccess_dummy_inst:\n\t"
			"mrc p15, 0, %0, c0, c0, 0\n\t" : "=r" (ret));
	return ret;
} __attribute__((aligned(32)))

static void get_asm_value(void *ret)
{
	*(unsigned long *)ret =
	 cpaccess_dummy(per_cpu(cp_param.write_value, cpu));
}

static unsigned long do_cpregister_rw(int write)
{
	unsigned long opcode, ret, *p_opcode;

	per_cpu(cp_param.cp, cpu)  &= 0xF;
	per_cpu(cp_param.crn, cpu) &= 0xF;
	per_cpu(cp_param.crm, cpu) &= 0xF;
	per_cpu(cp_param.op1, cpu) &= 0x7;
	per_cpu(cp_param.op2, cpu) &= 0x7;

	opcode = (write == 1 ? 0xEE000010 : 0xEE100010);
	opcode |= (per_cpu(cp_param.crn, cpu)<<16) |
	(per_cpu(cp_param.crm, cpu)<<0) |
	(per_cpu(cp_param.op1, cpu)<<21) |
	(per_cpu(cp_param.op2, cpu)<<5) |
	(per_cpu(cp_param.cp, cpu) << 8);

	p_opcode = (unsigned long *)&cpaccess_dummy_inst;
	mem_text_write_kernel_word(p_opcode, opcode);

#ifdef CONFIG_SMP
	if (smp_call_function_single(cpu, get_asm_value, &ret, 1))
		printk(KERN_ERR "Error cpaccess smp call single\n");
#else
		get_asm_value(&ret);
#endif

	return ret;
}

static int get_register_params(char *str_tmp)
{
	unsigned long op1, op2, crn, crm, cp = 15, write_value, il2index;
	char rw;
	int cnt = 0;

	il2index = 0;
	strncpy(type, strsep(&str_tmp, ":"), TYPE_MAX_CHARACTERS);

	if (strncasecmp(type, "C", TYPE_MAX_CHARACTERS) == 0) {

		sscanf(str_tmp, "%lu:%lu:%lu:%lu:%lu:%c:%lx:%d",
			&cp, &op1, &crn, &crm, &op2, &rw, &write_value, &cpu);
		per_cpu(cp_param.cp, cpu) = cp;
		per_cpu(cp_param.op1, cpu) = op1;
		per_cpu(cp_param.crn, cpu) = crn;
		per_cpu(cp_param.crm, cpu) = crm;
		per_cpu(cp_param.op2, cpu) = op2;
		per_cpu(cp_param.rw, cpu) = rw;
		per_cpu(cp_param.write_value, cpu) = write_value;

		if ((per_cpu(cp_param.rw, cpu) != 'w') &&
				(per_cpu(cp_param.rw, cpu) != 'r')) {
			pr_err("cpaccess: Wrong entry for 'r' or 'w'.\n");
			return -EINVAL;
		}

		if (per_cpu(cp_param.rw, cpu) == 'w')
			do_cpregister_rw(1);
	} else if (strncasecmp(type, "IL2", TYPE_MAX_CHARACTERS) == 0)
		do_il2_rw(str_tmp);
	else {
		pr_err("cpaccess: Not a valid type. Entered: %s\n", type);
		return -EINVAL;
	}

	return cnt;
}

static ssize_t cp_register_write_sysfs(struct sys_device *dev,
	struct sysdev_attribute *attr, const char *buf, size_t cnt)
{
	char *str_tmp = (char *)buf;

	if (down_timeout(&cp_sem, 6000))
		return -ERESTARTSYS;

	get_register_params(str_tmp);

	return cnt;
}

static ssize_t cp_register_read_sysfs(struct sys_device *dev,
	struct sysdev_attribute *attr, char *buf)
{
	int ret;

	if (strncasecmp(type, "C", TYPE_MAX_CHARACTERS) == 0)
		ret = snprintf(buf, TYPE_MAX_CHARACTERS, "%lx\n",
					do_cpregister_rw(0));
	else if (strncasecmp(type, "IL2", TYPE_MAX_CHARACTERS) == 0)
		ret = snprintf(buf, TYPE_MAX_CHARACTERS, "%lx\n", il2_output);
	else
		ret = -EINVAL;

	if (cp_sem.count <= 0)
		up(&cp_sem);

	return ret;
}

SYSDEV_ATTR(cp_rw, 0644, cp_register_read_sysfs, cp_register_write_sysfs);

static struct sys_device device_cpaccess = {
	.id     = 0,
	.cls    = &cpaccess_sysclass,
};

static int __init init_cpaccess_sysfs(void)
{
	int error = sysdev_class_register(&cpaccess_sysclass);

	if (!error)
		error = sysdev_register(&device_cpaccess);
	else
		pr_err("Error initializing cpaccess interface\n");

	if (!error)
		error = sysdev_create_file(&device_cpaccess,
		 &attr_cp_rw);
	else {
		pr_err("Error initializing cpaccess interface\n");
		sysdev_unregister(&device_cpaccess);
		sysdev_class_unregister(&cpaccess_sysclass);
	}

	sema_init(&cp_sem, 1);

	return error;
}

static void __exit exit_cpaccess_sysfs(void)
{
	sysdev_remove_file(&device_cpaccess, &attr_cp_rw);
	sysdev_unregister(&device_cpaccess);
	sysdev_class_unregister(&cpaccess_sysclass);
}

module_init(init_cpaccess_sysfs);
module_exit(exit_cpaccess_sysfs);
MODULE_LICENSE("GPL v2");
