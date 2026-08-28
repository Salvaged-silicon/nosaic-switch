// The smallest out-of-tree module that proves linux-headers works.
//
// It is not testing this code. It is testing that the kernel build tree we
// package can actually compile a module against the kernel we ship: that
// Module.symvers is present, that scripts/ is the built one rather than a
// pristine copy, and that the resulting vermagic matches. A package that looks
// correct and cannot build a module is the failure this exists to catch.
//
// SPDX-License-Identifier: GPL-2.0-only
#include <linux/module.h>
#include <linux/kernel.h>

static int __init nosaic_probe_init(void)
{
	printk(KERN_INFO "nosaic: out-of-tree module build check\n");
	return 0;
}

static void __exit nosaic_probe_exit(void) { }

module_init(nosaic_probe_init);
module_exit(nosaic_probe_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("NOSaic out-of-tree module build check");
