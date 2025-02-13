/*
 * GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2012, 2017, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 */
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/unistd.h>
#include <net/sock.h>
#include <linux/uio.h>
#include <linux/uaccess.h>

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/list.h>

#include <linux/sysctl.h>
#include <linux/debugfs.h>
#include <asm/div64.h>

#define DEBUG_SUBSYSTEM S_LNET

#include <libcfs/libcfs.h>
#include <libcfs/libcfs_crypto.h>
#include <lnet/lib-lnet.h>
#include <lustre_crypto.h>
#include "tracefile.h"

struct lnet_debugfs_symlink_def {
	const char *name;
	const char *target;
};

static struct dentry *lnet_debugfs_root;

int libcfs_ioctl(unsigned int cmd, struct libcfs_ioctl_data *data)
{
	switch (cmd) {
	case IOC_LIBCFS_CLEAR_DEBUG:
		libcfs_debug_clear_buffer();
		break;
	case IOC_LIBCFS_MARK_DEBUG:
		if (data == NULL ||
		    data->ioc_inlbuf1 == NULL ||
		    data->ioc_inlbuf1[data->ioc_inllen1 - 1] != '\0')
			return -EINVAL;

		libcfs_debug_mark_buffer(data->ioc_inlbuf1);
		break;

	default:
		return -EINVAL;
	}
	return 0;
}
EXPORT_SYMBOL(libcfs_ioctl);

static int proc_dobitmasks(struct ctl_table *table, int write,
			   void __user *buffer, size_t *lenp, loff_t *ppos)
{
	const int     tmpstrlen = 512;
	char         *tmpstr = NULL;
	int           rc;
	size_t nob = *lenp;
	loff_t pos = *ppos;
	unsigned int *mask = table->data;
	int           is_subsys = (mask == &libcfs_subsystem_debug) ? 1 : 0;
	int           is_printk = (mask == &libcfs_printk) ? 1 : 0;

	if (!write) {
		tmpstr = kmalloc(tmpstrlen, GFP_KERNEL | __GFP_ZERO);
		if (!tmpstr)
			return -ENOMEM;
		libcfs_debug_mask2str(tmpstr, tmpstrlen, *mask, is_subsys);
		rc = strlen(tmpstr);

		if (pos >= rc) {
			rc = 0;
		} else {
			rc = cfs_trace_copyout_string(buffer, nob,
						      tmpstr + pos, "\n");
		}
	} else {
		tmpstr = memdup_user_nul(buffer, nob);
		if (IS_ERR(tmpstr))
			return PTR_ERR(tmpstr);

		rc = libcfs_debug_str2mask(mask, strim(tmpstr), is_subsys);
		/* Always print LBUG/LASSERT to console, so keep this mask */
		if (is_printk)
			*mask |= D_EMERG;
	}

	kfree(tmpstr);
	return rc;
}

static int min_watchdog_ratelimit;		/* disable ratelimiting */
static int max_watchdog_ratelimit = (24*60*60); /* limit to once per day */

static int proc_dump_kernel(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	size_t nob = *lenp;

	if (!write)
		return 0;

	return cfs_trace_dump_debug_buffer_usrstr(buffer, nob);
}

static int proc_daemon_file(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	size_t nob = *lenp;
	loff_t pos = *ppos;

	if (!write) {
		int len = strlen(cfs_tracefile);

		if (pos >= len)
			return 0;

		return cfs_trace_copyout_string(buffer, nob,
						cfs_tracefile + pos, "\n");
	}

	return cfs_trace_daemon_command_usrstr(buffer, nob);
}

static int libcfs_force_lbug(struct ctl_table *table, int write,
			     void __user *buffer,
			     size_t *lenp, loff_t *ppos)
{
	if (write)
		LBUG();
	return 0;
}

static int proc_fail_loc(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int rc;
	long old_fail_loc = cfs_fail_loc;

	if (!*lenp || *ppos) {
		*lenp = 0;
		return 0;
	}

	if (write) {
		char *kbuf = memdup_user_nul(buffer, *lenp);

		if (IS_ERR(kbuf))
			return PTR_ERR(kbuf);
		rc = kstrtoul(kbuf, 0, &cfs_fail_loc);
		kfree(kbuf);
		*ppos += *lenp;
	} else {
		char kbuf[64/3+3];

		rc = scnprintf(kbuf, sizeof(kbuf), "%lu\n", cfs_fail_loc);
		if (copy_to_user(buffer, kbuf, rc))
			rc = -EFAULT;
		else {
			*lenp = rc;
			*ppos += rc;
		}
	}

	if (old_fail_loc != cfs_fail_loc) {
		cfs_race_state = 1;
		wake_up(&cfs_race_waitq);
	}
	return rc;
}

int debugfs_doint(struct ctl_table *table, int write,
		  void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int rc;

	if (!*lenp || *ppos) {
		*lenp = 0;
		return 0;
	}

	if (write) {
		char *kbuf = memdup_user_nul(buffer, *lenp);
		int val;

		if (IS_ERR(kbuf))
			return PTR_ERR(kbuf);

		rc = kstrtoint(kbuf, 0, &val);
		kfree(kbuf);
		if (!rc) {
			if (table->extra1 && val < *(int *)table->extra1)
				val = *(int *)table->extra1;
			if (table->extra2 && val > *(int *)table->extra2)
				val = *(int *)table->extra2;
			*(int *)table->data = val;
		}
		*ppos += *lenp;
	} else {
		char kbuf[64/3+3];

		rc = scnprintf(kbuf, sizeof(kbuf), "%u\n", *(int *)table->data);
		if (copy_to_user(buffer, kbuf, rc))
			rc = -EFAULT;
		else {
			*lenp = rc;
			*ppos += rc;
		}
	}

	return rc;
}
EXPORT_SYMBOL(debugfs_doint);

static int debugfs_dou64(struct ctl_table *table, int write,
			 void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int rc;

	if (!*lenp || *ppos) {
		*lenp = 0;
		return 0;
	}

	if (write) {
		char *kbuf = memdup_user_nul(buffer, *lenp);
		unsigned long long val;

		if (IS_ERR(kbuf))
			return PTR_ERR(kbuf);

		rc = kstrtoull(kbuf, 0, &val);
		kfree(kbuf);
		if (!rc)
			*(u64 *)table->data = val;
		*ppos += *lenp;
	} else {
		char kbuf[64/3+3];

		rc = scnprintf(kbuf, sizeof(kbuf), "%llu\n",
			       (unsigned long long)*(u64 *)table->data);
		if (copy_to_user(buffer, kbuf, rc))
			rc = -EFAULT;
		else {
			*lenp = rc;
			*ppos += rc;
		}
	}

	return rc;
}

static int debugfs_dostring(struct ctl_table *table, int write,
			    void __user *buffer, size_t *lenp, loff_t *ppos)
{
	int len = *lenp;
	char *kbuf = table->data;

	if (!len || *ppos) {
		*lenp = 0;
		return 0;
	}
	if (len > table->maxlen)
		len = table->maxlen;
	if (write) {
		if (copy_from_user(kbuf, buffer, len))
			return -EFAULT;
		memset(kbuf+len, 0, table->maxlen - len);
		*ppos = *lenp;
	} else {
		len = strnlen(kbuf, len);
		if (copy_to_user(buffer, kbuf, len))
			return -EFAULT;
		if (len < *lenp) {
			if (copy_to_user(buffer+len, "\n", 1))
				return -EFAULT;
			len += 1;
		}
		*ppos += len;
		*lenp -= len;
	}
	return len;
}

static int proc_cpt_table(struct ctl_table *table, int write,
			  void __user *buffer, size_t *lenp, loff_t *ppos)
{
	size_t nob = *lenp;
	loff_t pos = *ppos;
	char *buf = NULL;
	int   len = 4096;
	int   rc  = 0;

	if (write)
		return -EPERM;

	while (1) {
		LIBCFS_ALLOC(buf, len);
		if (buf == NULL)
			return -ENOMEM;

		rc = cfs_cpt_table_print(cfs_cpt_tab, buf, len);
		if (rc >= 0)
			break;

		if (rc == -EFBIG) {
			LIBCFS_FREE(buf, len);
			len <<= 1;
			continue;
		}
		goto out;
	}

	if (pos >= rc) {
		rc = 0;
		goto out;
	}

	rc = cfs_trace_copyout_string(buffer, nob, buf + pos, NULL);
out:
	if (buf != NULL)
		LIBCFS_FREE(buf, len);
	return rc;
}

static int proc_cpt_distance(struct ctl_table *table, int write,
			     void __user *buffer, size_t *lenp, loff_t *ppos)
{
	size_t nob = *lenp;
	loff_t pos = *ppos;
	char *buf = NULL;
	int   len = 4096;
	int   rc  = 0;

	if (write)
		return -EPERM;

	while (1) {
		LIBCFS_ALLOC(buf, len);
		if (buf == NULL)
			return -ENOMEM;

		rc = cfs_cpt_distance_print(cfs_cpt_tab, buf, len);
		if (rc >= 0)
			break;

		if (rc == -EFBIG) {
			LIBCFS_FREE(buf, len);
			len <<= 1;
			continue;
		}
		goto out;
	}

	if (pos >= rc) {
		rc = 0;
		goto out;
	}

	rc = cfs_trace_copyout_string(buffer, nob, buf + pos, NULL);
 out:
	if (buf != NULL)
		LIBCFS_FREE(buf, len);
	return rc;
}

static struct ctl_table lnet_table[] = {
	{
		.procname	= "debug",
		.data		= &libcfs_debug,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &proc_dobitmasks,
	},
	{
		.procname	= "subsystem_debug",
		.data		= &libcfs_subsystem_debug,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &proc_dobitmasks,
	},
	{
		.procname	= "printk",
		.data		= &libcfs_printk,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &proc_dobitmasks,
	},
	{
		.procname	= "cpu_partition_table",
		.maxlen		= 128,
		.mode		= 0444,
		.proc_handler	= &proc_cpt_table,
	},
	{
		.procname	= "cpu_partition_distance",
		.maxlen		= 128,
		.mode		= 0444,
		.proc_handler	= &proc_cpt_distance,
	},
	{
		.procname	= "debug_log_upcall",
		.data		= lnet_debug_log_upcall,
		.maxlen		= sizeof(lnet_debug_log_upcall),
		.mode		= 0644,
		.proc_handler	= &debugfs_dostring,
	},
	{
		.procname	= "lnet_memused",
		.data		= (u64 *)&libcfs_kmem.counter,
		.maxlen		= sizeof(u64),
		.mode		= 0444,
		.proc_handler	= &debugfs_dou64,
	},
	{
		.procname	= "catastrophe",
		.data		= &libcfs_catastrophe,
		.maxlen		= sizeof(int),
		.mode		= 0444,
		.proc_handler	= &debugfs_doint,
	},
	{
		.procname	= "dump_kernel",
		.maxlen		= 256,
		.mode		= 0200,
		.proc_handler	= &proc_dump_kernel,
	},
	{
		.procname	= "daemon_file",
		.mode		= 0644,
		.maxlen		= 256,
		.proc_handler	= &proc_daemon_file,
	},
	{
		.procname	= "watchdog_ratelimit",
		.data		= &libcfs_watchdog_ratelimit,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &debugfs_doint,
		.extra1		= &min_watchdog_ratelimit,
		.extra2		= &max_watchdog_ratelimit,
	},
	{
		.procname	= "force_lbug",
		.data		= NULL,
		.maxlen		= 0,
		.mode		= 0200,
		.proc_handler	= &libcfs_force_lbug
	},
	{
		.procname	= "fail_loc",
		.data		= &cfs_fail_loc,
		.maxlen		= sizeof(cfs_fail_loc),
		.mode		= 0644,
		.proc_handler	= &proc_fail_loc
	},
	{
		.procname	= "fail_val",
		.data		= &cfs_fail_val,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= &debugfs_doint
	},
	{
		.procname	= "fail_err",
		.data		= &cfs_fail_err,
		.maxlen		= sizeof(cfs_fail_err),
		.mode		= 0644,
		.proc_handler	= &debugfs_doint,
	},
	{
	}
};

static const struct lnet_debugfs_symlink_def lnet_debugfs_symlinks[] = {
	{ .name		= "console_ratelimit",
	  .target	= "../../../module/libcfs/parameters/libcfs_console_ratelimit" },
	{ .name		= "debug_path",
	  .target	= "../../../module/libcfs/parameters/libcfs_debug_file_path" },
	{ .name		= "panic_on_lbug",
	  .target	= "../../../module/libcfs/parameters/libcfs_panic_on_lbug" },
	{ .name		= "console_backoff",
	  .target	= "../../../module/libcfs/parameters/libcfs_console_backoff" },
	{ .name		= "debug_mb",
	  .target	= "../../../module/libcfs/parameters/libcfs_debug_mb" },
	{ .name		= "console_min_delay_centisecs",
	  .target	= "../../../module/libcfs/parameters/libcfs_console_min_delay" },
	{ .name		= "console_max_delay_centisecs",
	  .target	= "../../../module/libcfs/parameters/libcfs_console_max_delay" },
	{ .name		= NULL },
};

static ssize_t lnet_debugfs_read(struct file *filp, char __user *buf,
				 size_t count, loff_t *ppos)
{
	struct ctl_table *table = filp->private_data;
	loff_t old_pos = *ppos;
	ssize_t rc = -EINVAL;

	if (table)
		rc = table->proc_handler(table, 0, (void __user *)buf,
					 &count, ppos);
	/*
	 * On success, the length read is either in error or in count.
	 * If ppos changed, then use count, else use error
	 */
	if (!rc && *ppos != old_pos)
		rc = count;
	else if (rc > 0)
		*ppos += rc;

	return rc;
}

static ssize_t lnet_debugfs_write(struct file *filp, const char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct ctl_table *table = filp->private_data;
	loff_t old_pos = *ppos;
	ssize_t rc = -EINVAL;

	if (table)
		rc = table->proc_handler(table, 1, (void __user *)buf, &count,
					 ppos);
	if (rc)
		return rc;

	if (*ppos == old_pos)
		*ppos += count;

	return count;
}

static const struct file_operations lnet_debugfs_file_operations_rw = {
	.open		= simple_open,
	.read		= lnet_debugfs_read,
	.write		= lnet_debugfs_write,
	.llseek		= default_llseek,
};

static const struct file_operations lnet_debugfs_file_operations_ro = {
	.open		= simple_open,
	.read		= lnet_debugfs_read,
	.llseek		= default_llseek,
};

static const struct file_operations lnet_debugfs_file_operations_wo = {
	.open		= simple_open,
	.write		= lnet_debugfs_write,
	.llseek		= default_llseek,
};

static const struct file_operations *lnet_debugfs_fops_select(
	umode_t mode, const struct file_operations state[3])
{
	if (!(mode & S_IWUGO))
		return &state[0];

	if (!(mode & S_IRUGO))
		return &state[1];

	return &state[2];
}

void lnet_insert_debugfs(struct ctl_table *table, struct module *mod,
			 void **statep)
{
	struct file_operations *state = *statep;
	if (!lnet_debugfs_root)
		lnet_debugfs_root = debugfs_create_dir("lnet", NULL);

	/* Even if we cannot create, just ignore it altogether) */
	if (IS_ERR_OR_NULL(lnet_debugfs_root))
		return;

	if (!state) {
		state = kmalloc(3 * sizeof(*state), GFP_KERNEL);
		if (!state)
			return;
		state[0] = lnet_debugfs_file_operations_ro;
		state[0].owner = mod;
		state[1] = lnet_debugfs_file_operations_wo;
		state[1].owner = mod;
		state[2] = lnet_debugfs_file_operations_rw;
		state[2].owner = mod;
		*statep = state;
	}

	/* We don't save the dentry returned in next two calls, because
	 * we don't call debugfs_remove() but rather remove_recursive()
	 */
	for (; table && table->procname; table++)
		debugfs_create_file(table->procname, table->mode,
				    lnet_debugfs_root, table,
				    lnet_debugfs_fops_select(table->mode,
							     (const struct file_operations *)state));
}
EXPORT_SYMBOL_GPL(lnet_insert_debugfs);

void lnet_debugfs_fini(void **state)
{
	kfree(*state);
	*state = NULL;
}
EXPORT_SYMBOL_GPL(lnet_debugfs_fini);

static void lnet_insert_debugfs_links(
		const struct lnet_debugfs_symlink_def *symlinks)
{
	for (; symlinks && symlinks->name; symlinks++)
		debugfs_create_symlink(symlinks->name, lnet_debugfs_root,
				       symlinks->target);
}

void lnet_remove_debugfs(struct ctl_table *table)
{
	for (; table && table->procname; table++) {
		struct qstr dname = QSTR_INIT(table->procname,
					      strlen(table->procname));
		struct dentry *dentry;

		dentry = d_hash_and_lookup(lnet_debugfs_root, &dname);
		debugfs_remove(dentry);
	}
}
EXPORT_SYMBOL_GPL(lnet_remove_debugfs);

static DEFINE_MUTEX(libcfs_startup);
static int libcfs_active;

static void *debugfs_state;

int libcfs_setup(void)
{
	int rc = -EINVAL;

	mutex_lock(&libcfs_startup);
	if (libcfs_active)
		goto out;

	rc = libcfs_debug_init(5 * 1024 * 1024);
	if (rc < 0) {
		pr_err("LustreError: libcfs_debug_init: rc = %d\n", rc);
		goto cleanup_lock;
	}

	rc = cfs_cpu_init();
	if (rc != 0)
		goto cleanup_debug;

	cfs_rehash_wq = alloc_workqueue("cfs_rh", WQ_SYSFS, 4);
	if (!cfs_rehash_wq) {
		rc = -ENOMEM;
		CERROR("libcfs: failed to start rehash workqueue: rc = %d\n",
		       rc);
		goto cleanup_cpu;
	}

	rc = cfs_crypto_register();
	if (rc) {
		CERROR("cfs_crypto_register: error %d\n", rc);
		goto cleanup_wq;
	}

	CDEBUG(D_OTHER, "libcfs setup OK\n");
out:
	libcfs_active = 1;
	mutex_unlock(&libcfs_startup);
	return 0;

cleanup_wq:
	destroy_workqueue(cfs_rehash_wq);
	cfs_rehash_wq = NULL;
cleanup_cpu:
	cfs_cpu_fini();
cleanup_debug:
	libcfs_debug_cleanup();
cleanup_lock:
	mutex_unlock(&libcfs_startup);
	return rc;
}
EXPORT_SYMBOL(libcfs_setup);

static int __init libcfs_init(void)
{
	int rc;

	rc = cfs_arch_init();
	if (rc < 0) {
		CERROR("cfs_arch_init: error %d\n", rc);
		return rc;
	}

	lnet_insert_debugfs(lnet_table, THIS_MODULE, &debugfs_state);
	if (!IS_ERR_OR_NULL(lnet_debugfs_root))
		lnet_insert_debugfs_links(lnet_debugfs_symlinks);

	return rc;
}

static void __exit libcfs_exit(void)
{
	int rc;

	/* Remove everthing */
	debugfs_remove_recursive(lnet_debugfs_root);
	lnet_debugfs_root = NULL;

	lnet_debugfs_fini(&debugfs_state);

	CDEBUG(D_MALLOC, "before Portals cleanup: kmem %lld\n",
	       libcfs_kmem_read());

	if (cfs_rehash_wq)
		destroy_workqueue(cfs_rehash_wq);

	cfs_crypto_unregister();

	cfs_cpu_fini();

	/* the below message is checked in test-framework.sh check_mem_leak() */
	if (libcfs_kmem_read() != 0)
		CERROR("Portals memory leaked: %lld bytes\n",
		       libcfs_kmem_read());

	rc = libcfs_debug_cleanup();
	if (rc)
		pr_err("LustreError: libcfs_debug_cleanup: rc = %d\n", rc);

	cfs_arch_exit();
}

MODULE_AUTHOR("OpenSFS, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre helper library");
MODULE_VERSION(LIBCFS_VERSION);
MODULE_LICENSE("GPL");

module_init(libcfs_init);
module_exit(libcfs_exit);
