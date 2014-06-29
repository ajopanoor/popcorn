/*
 * This file for Obtaining Remote PID
 *
 * Akshay
 */
#include <linux/kernel.h>
#include <linux/kthread.h>
#include <linux/smp.h>
#include <linux/slab.h>
#include <linux/timer.h>
#include <linux/pcn_kmsg.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/wait.h>
#include <linux/seq_file.h>

#include <popcorn/pid.h>
#include "remote_proc_pid.h"

#define PRINT_MESSAGES 0
#if PRINT_MESSAGES
#define PRINTK(...) printk(__VA_ARGS__)
#else
#define PRINTK(...) ;
#endif

#define SIZE 236  // PID array size
#define PROC_MAXPIDS 100

/*
 * Variables
 */
static int _cpu = -1;
static int wait = -1;
static DECLARE_WAIT_QUEUE_HEAD( wq);

/*
 * ************************************* Dummy proc PID entry **************************
 */

struct remote_pid_entry {
	char *name;
	int len;
	mode_t mode;
	const struct inode_operations *iop;
	const struct file_operations *fop;
	union proc_remote_op op;
};

/*
 * **************** Similar definition from base.c for mapping PID files and its operations **************************
 */

#define NOD(NAME, MODE, IOP, FOP, OP) {			\
	.name = (NAME),					\
	.len  = sizeof(NAME) - 1,			\
	.mode = MODE,					\
	.iop  = IOP,					\
	.fop  = FOP,					\
	.op   = OP,					\
}

#define DIR(NAME, MODE, iops, fops)	\
	NOD(NAME, (S_IFDIR|(MODE)), &iops, &fops, {} )
#define LNK(NAME, get_link)					\
	NOD(NAME, (S_IFLNK|S_IRWXUGO),				\
		&proc_pid_link_inode_operations, NULL,		\
		{ .proc_get_link = get_link } )
#define REG(NAME, MODE, fops)				\
	NOD(NAME, (S_IFREG|(MODE)), NULL, &fops, {})
#define INF(NAME, MODE, read)				\
	NOD(NAME, (S_IFREG|(MODE)), 			\
		NULL, &proc_info_file_operations,	\
		{ .proc_read = read } )
#define ONE(NAME, MODE, show)				\
	NOD(NAME, (S_IFREG|(MODE)), 			\
		NULL, &remote_proc_single_file_operations,	\
		{ .proc_show = show } )

/*
 * ************************************ Message structures for obtaining remote PID **************************
 */

struct _remote_pid_request {
	struct pcn_kmsg_hdr header;
	char pad_string[60];
}__attribute__((packed)) __attribute__((aligned(64)));

typedef struct _remote_pid_request _remote_pid_request_t;

struct _remote_pid_response {
	struct pcn_kmsg_hdr header;
	unsigned long remote_pid[SIZE];
	int count;
}__attribute__((packed)) __attribute__((aligned(64)));

typedef struct _remote_pid_response _remote_pid_response_t;

/*
 * ************************************ Define variables holding result **************************
 */
static _remote_pid_response_t *pid_result;

/*
 * **************************************common functions*************************************
 */

int flush_variables() {
	pid_result = NULL;
	wait = -1;
	return 0;
}

int iterate_process(unsigned long *pid_arr) {
	struct task_struct *p;
	int count = 0;

	for_each_process(p)
	{
/*		if(p->origin_pid !=-1 &&
						p->origin_pid != p->pid)
					    	 continue;
*/		pid_arr[count] = p->pid;
		count++;
	}
	return count;

}

/*
 * ********************************** Message handling functions for /pid *************************************
 */

static int handle_remote_pid_response(struct pcn_kmsg_message* inc_msg) {
	_remote_pid_response_t* msg = (_remote_pid_response_t*) inc_msg;

	PRINTK("%s: Entered remote pid response : pid count :{%d} \n", __func__,
			msg->count);

	if (msg != NULL)
		pid_result = msg;

	wait++;
	wake_up_interruptible(&wq);
	PRINTK("%s: response --- pid stored - wait{%d} \n", __func__, wait);

	pcn_kmsg_free_msg(inc_msg);

	return 0;
}

static int handle_remote_pid_request(struct pcn_kmsg_message* inc_msg) {

	_remote_pid_request_t* msg = (_remote_pid_request_t*) inc_msg;
	_remote_pid_response_t response;

	PRINTK("%s: Entered remote PID request \n", __func__);

	// Finish constructing response
	response.header.type = PCN_KMSG_TYPE_REMOTE_PID_RESPONSE;
	response.header.prio = PCN_KMSG_PRIO_NORMAL;
	flush_variables();
	unsigned long pid_arr[SIZE];

	response.count = iterate_process(&pid_arr);
	memcpy(&response.remote_pid, &pid_arr, SIZE * sizeof(long));

	PRINTK("%s: Remote:pid count : %d \n", __func__, response.count);
	// Send response
	pcn_kmsg_send_long(msg->header.from_cpu,
			(struct pcn_kmsg_message*) (&response),
			sizeof(_remote_pid_response_t) - sizeof(struct pcn_kmsg_hdr));

	pcn_kmsg_free_msg(inc_msg);

	return 0;
}

/*
 * ***************************** Functions to call PID file operations **************************
 */

static struct pid *remote_proc_pid(struct inode *inode) {
	return PROC_I(inode)->pid;
}

struct proc_remote_pid_info *get_remote_proc_task(struct inode *inode) {
	return &PROC_I(inode)->remote_proc;
}

int remote_proc_tgid_stat(struct seq_file *m, struct proc_remote_pid_info *task,
		char *buf, size_t count) {
	return do_remote_task_stat(m, task, buf, count);
}

int remote_proc_cpuset(struct seq_file *m, struct proc_remote_pid_info *task,
		char *buf, size_t count) {
	return do_remote_task_cpuset(m, task, buf, count);
}

static int remote_proc_single_show(struct seq_file *m, void *v) {
	struct inode *inode = m->private;
	//struct pid_namespace *ns;
	struct pid *pid;
	struct proc_remote_pid_info *task;
	int ret;

	unsigned long page;

	//ns = inode->i_sb->s_fs_info;
	pid = remote_proc_pid(inode);
	task = get_remote_proc_task(inode);
	if (!task)
		return -ESRCH;

	if (!(page = __get_free_page(GFP_TEMPORARY)))
		goto out;
	ret = task->op.proc_show(m, task, (char *) page, (size_t)PAGE_SIZE);
	out: return ret;
}

static int remote_proc_single_open(struct inode *inode, struct file *filp) {
	return single_open(filp, remote_proc_single_show, inode);
}

static const struct file_operations remote_proc_single_file_operations = {
		.open = remote_proc_single_open, .read = seq_read, .llseek = seq_lseek,
		.release = single_release, };

/*
 * ****************************************** Functions copied from base.c ***********************************
 */

static
unsigned int remote_pid_entry_count_dirs(const struct remote_pid_entry *entries,
		unsigned int n) {
	unsigned int i;
	unsigned int count;

	count = 0;
	for (i = 0; i < n; ++i) {
		if (S_ISDIR(entries[i].mode))
			++count;
	}

	return count;
}

static struct inode *remote_proc_pid_make_inode(struct super_block *sb,
		struct proc_remote_pid_info *task) {
	struct inode *inode;
	struct proc_remote_pid_info *ei;

	/* We need a new inode */

	inode = new_inode(sb);
	if (!inode)
		goto out;

	/* Common stuff */
	inode->i_ino = get_next_ino();
	inode->i_mtime = inode->i_atime = inode->i_ctime = CURRENT_TIME;
	//inode->i_op = &proc_def_inode_operations;

	ei = get_remote_proc_task(inode);
	//krg_task_get(task->task_obj);
	*ei = *task;

	inode->i_uid = 0;
	inode->i_gid = 0;

	out: return inode;
}

typedef struct dentry *rem_instantiate_t(struct inode *, struct dentry *,
		struct proc_remote_pid_info *, const void *);

static int remote_pid_revalidate(struct dentry *dentry, struct nameidata *nd) {

	return 0;
}

static int remote_pid_delete_dentry(struct dentry *dentry) {

	return 0;
}
static struct dentry_operations remote_pid_dentry_operations = { .d_revalidate =
		remote_pid_revalidate, .d_delete = remote_pid_delete_dentry, };

/* Unsupported entries are commented out */
static struct remote_pid_entry remote_tgid_base_stuff[] = {
/* 	DIR("task",       S_IRUGO|S_IXUGO, task), */
//	DIR("fd",         S_IRUSR|S_IXUSR, fd),
		/*      DIR("fdinfo",     S_IRUSR|S_IXUSR, proc_fdinfo_inode_operations, proc_fdinfo_operations), */
		/* #ifdef CONFIG_NET */
		/*      DIR("net",        S_IRUGO|S_IXUGO, proc_net_inode_operations, proc_net_operations), */
		/* #endif */
//	REG("environ",    S_IRUSR, pid_environ),
//	INF("auxv",       S_IRUSR, pid_auxv),
//	ONE("status",     S_IRUGO, pid_status),
//	ONE("personality", S_IRUSR, pid_personality),
//	INF("limits",	  S_IRUSR, pid_limits),
		/* #ifdef CONFIG_SCHED_DEBUG */
		/*      REG("sched",      S_IRUGO|S_IWUSR, proc_pid_sched_operations), */
		/* #endif */
#ifdef CONFIG_HAVE_ARCH_TRACEHOOK
//	INF("syscall",    S_IRUSR, pid_syscall),
#endif
//	INF("cmdline",    S_IRUGO, pid_cmdline),
		ONE("stat", S_IRUGO, remote_proc_tgid_stat),
//	ONE("statm",      S_IRUGO, pid_statm),
		/* 	REG("maps",       S_IRUGO, maps), */
		/* #ifdef CONFIG_NUMA */
		/* 	REG("numa_maps",  S_IRUGO, numa_maps), */
		/* #endif */
		/* 	REG("mem",        S_IRUSR|S_IWUSR, mem), */
		/* 	LNK("cwd",        cwd), */
		/* 	LNK("root",       root), */
		/* 	LNK("exe",        exe), */
		/* 	REG("mounts",     S_IRUGO, mounts), */
		/*      REG("mountinfo",  S_IRUGO, proc_mountinfo_operations), */
		/* 	REG("mountstats", S_IRUSR, mountstats), */
		/* #ifdef CONFIG_PROC_PAGE_MONITOR */
		/*      REG("clear_refs", S_IWUSR, proc_clear_refs_operations), */
		/*      REG("smaps",      S_IRUGO, proc_smaps_operations), */
		/*      REG("pagemap",    S_IRUSR, proc_pagemap_operations), */
		/* #endif */
		/* #ifdef CONFIG_SECURITY */
		/* 	DIR("attr",       S_IRUGO|S_IXUGO, attr_dir), */
		/* #endif */
#ifdef CONFIG_KALLSYMS
//	INF("wchan",      S_IRUGO, pid_wchan),
#endif
#ifdef CONFIG_STACKTRACE
//	ONE("stack",      S_IRUSR, pid_stack),
#endif
#ifdef CONFIG_SCHEDSTATS
//	INF("schedstat",  S_IRUGO, pid_schedstat),
#endif
		/* #ifdef CONFIG_LATENCYTOP */
		/*      REG("latency",  S_IRUGO, proc_lstats_operations), */
		/* #endif */
		/* #ifdef CONFIG_PROC_PID_CPUSET */
		ONE("cpuset", S_IRUGO, remote_proc_cpuset),
		/* #endif */
		/* #ifdef CONFIG_CGROUPS */
		/*      REG("cgroup",  S_IRUGO, proc_cgroup_operations), */
		/* #endif */
//	INF("oom_score",  S_IRUGO, pid_oom_score),
		/* 	REG("oom_adj",    S_IRUGO|S_IWUSR, oom_adjust), */
		/* #ifdef CONFIG_AUDITSYSCALL */
		/* 	REG("loginuid",   S_IWUSR|S_IRUGO, loginuid), */
		/*      REG("sessionid",  S_IRUGO, proc_sessionid_operations), */
		/* #endif */
		/* #ifdef CONFIG_FAULT_INJECTION */
		/* 	REG("make-it-fail", S_IRUGO|S_IWUSR, fault_inject), */
		/* #endif */
		/* #if defined(USE_ELF_CORE_DUMP) && defined(CONFIG_ELF_CORE) */
		/*      REG("coredump_filter", S_IRUGO|S_IWUSR, proc_coredump_filter_operations), */
		/* #endif */
#ifdef CONFIG_TASK_IO_ACCOUNTING
//	INF("io",	S_IRUGO, tgid_io_accounting),
#endif
		};

static int remote_proc_fill_cache(struct file *filp, void *dirent,
		filldir_t filldir, char *name, int len, rem_instantiate_t instantiate,
		struct proc_remote_pid_info *task, const void *ptr) {
	struct dentry *child, *dir = filp->f_path.dentry;
	struct inode *inode;
	struct qstr qname;
	ino_t ino = 0;
	unsigned type = DT_UNKNOWN;

	qname.name = name;
	qname.len = len;
	qname.hash = full_name_hash(name, len);

	child = d_lookup(dir, &qname);
	if (!child) {
		struct dentry *new;
		new = d_alloc(dir, &qname);
		if (new) {
			child = instantiate(dir->d_inode, new, task, ptr);
			if (child)
				dput(new);
			else
				child = new;
		}
	}
	if (!child || IS_ERR(child) || !child->d_inode)
		goto end_instantiate;
	inode = child->d_inode;
	if (inode) {
		ino = inode->i_ino;
		type = inode->i_mode >> 12;
	}
	dput(child);
	end_instantiate: if (!ino)
		ino = find_inode_number(dir, &qname);
	if (!ino)
		ino = 1;
	return filldir(dirent, name, len, filp->f_pos, ino, type);
}

static struct dentry *remote_proc_pident_instantiate(struct inode *dir,
		struct dentry *dentry, struct proc_remote_pid_info *task,
		const void *ptr) {
	const struct remote_pid_entry *p = ptr;
	struct inode *inode;
	struct proc_remote_pid_info *new_info;
	struct dentry *error = ERR_PTR(-ENOENT);

	inode = remote_proc_pid_make_inode(dir->i_sb, task);
	if (!inode)
		goto out;

	new_info = get_remote_proc_task(inode);
	inode->i_mode = p->mode;
	if (S_ISDIR(inode->i_mode))
		set_nlink(inode, 2);
	if (p->iop)
		inode->i_op = p->iop;
	if (p->fop)
		inode->i_fop = p->fop;
	new_info->op = p->op;
	dentry->d_op = &remote_pid_dentry_operations;
	d_add(dentry, inode);
	/* Close the race of the process dying before we return the dentry */
	//	error = NULL;
	out: return error;
}

static int remote_proc_pident_fill_cache(struct file *filp, void *dirent,
		filldir_t filldir, struct proc_remote_pid_info *task,
		const struct remote_pid_entry *p) {
	return remote_proc_fill_cache(filp, dirent, filldir, p->name, p->len,
			remote_proc_pident_instantiate, task, p);
}

static int remote_proc_pident_readdir(struct file *filp, void *dirent,
		filldir_t filldir, const struct remote_pid_entry *ents,
		unsigned int nents) {

	int i;
	struct dentry *dentry = filp->f_path.dentry;
	struct inode *inode = dentry->d_inode;
	struct proc_remote_pid_info *task = get_remote_proc_task(inode);
	const struct remote_pid_entry *p, *last;
	ino_t ino;
	int ret;

	ret = -ENOENT;

	ret = 0;
	i = filp->f_pos;
	switch (i) {
	case 0:
		ino = inode->i_ino;
		if (filldir(dirent, ".", 1, i, ino, DT_DIR) < 0)
			goto out;
		i++;
		filp->f_pos++;
		/* fall through */
	case 1:
		ino = parent_ino(dentry);
		if (filldir(dirent, "..", 2, i, ino, DT_DIR) < 0)
			goto out;
		i++;
		filp->f_pos++;
		/* fall through */
	default:
		i -= 2;
		if (i >= nents) {
			ret = 1;
			goto out;
		}
		p = ents + i;
		last = &ents[nents - 1];
		while (p <= last) {
			if (remote_proc_pident_fill_cache(filp, dirent, filldir, task, p)
					< 0)
				goto out;
			filp->f_pos++;
			p++;
		}
	}

	ret = 1;
	out: return ret;
	return 0;
}

static int remote_proc_tgid_base_readdir(struct file *filp, void *dirent,
		filldir_t filldir) {
	return remote_proc_pident_readdir(filp, dirent, filldir,
			remote_tgid_base_stuff, ARRAY_SIZE(remote_tgid_base_stuff));
}

static struct file_operations remote_proc_tgid_base_operations = { .read =
		generic_read_dir, .readdir = remote_proc_tgid_base_readdir, };

struct dentry *remote_proc_pident_lookup(struct inode *dir,
		struct dentry *dentry, const struct remote_pid_entry *ents,
		unsigned int nents) {

	struct dentry *error;
	struct proc_remote_pid_info *task = get_remote_proc_task(dir);
	const struct remote_pid_entry *p, *last;

	error = ERR_PTR(-ENOENT);

	//if (!task_alive(task->task_obj))
	//goto out;

	/*
	 * Yes, it does not scale. And it should not. Don't add
	 * new entries into /proc/<tgid>/ without very good reasons.
	 */
	last = &ents[nents - 1];
	for (p = ents; p <= last; p++) {
		if (p->len != dentry->d_name.len)
			continue;
		if (!memcmp(dentry->d_name.name, p->name, p->len))
			break;
	}
	if (p > last)
		goto out;

	error = remote_proc_pident_instantiate(dir, dentry, task, p);
	out: return error;
	return 0;
}
static struct dentry *remote_proc_tgid_base_lookup(struct inode *dir,
		struct dentry *dentry, struct nameidata *nd) {
	return remote_proc_pident_lookup(dir, dentry, remote_tgid_base_stuff,
			ARRAY_SIZE(remote_tgid_base_stuff));
}

static int remote_pid_getattr(struct vfsmount *mnt, struct dentry *dentry,
		struct kstat *stat) {
	struct inode *inode = dentry->d_inode;
//	struct proc_distant_pid_info *task;

	generic_fillattr(inode, stat);

	stat->uid = 0;
	stat->gid = 0;
	/*task = get_proc_task(inode);
	 if (task_alive(task->task_obj)) {
	 if ((inode->i_mode == (S_IFDIR|S_IRUGO|S_IXUGO)) ||
	 task->dumpable) {
	 stat->uid = task->euid;
	 stat->gid = task->egid;
	 }
	 }*/

	return 0;
}

static struct inode_operations remote_proc_tgid_base_inode_operations = {
		.lookup = remote_proc_tgid_base_lookup, .getattr = remote_pid_getattr,
		.setattr = proc_setattr, };

static struct dentry *remote_proc_pid_instantiate(struct inode *dir,
		struct dentry *dentry, struct proc_remote_pid_info *task,
		const void *ptr) {
	struct dentry *error = ERR_PTR(-ENOENT);
	struct inode *inode;

	inode = remote_proc_pid_make_inode(dir->i_sb, task);
	if (!inode)
		goto out;

	inode->i_mode = S_IFDIR | S_IALLUGO;
	inode->i_op = &remote_proc_tgid_base_inode_operations;
	inode->i_fop = &remote_proc_tgid_base_operations;
	inode->i_flags |= S_IMMUTABLE;

	set_nlink(inode,
			2
					+ remote_pid_entry_count_dirs(remote_tgid_base_stuff,
							ARRAY_SIZE(remote_tgid_base_stuff)));
	//inode->i_nlink=0;
	dentry->d_op = &remote_pid_dentry_operations;

	d_add(dentry, inode);

	error = NULL;
	out: return error;
}

struct dentry *remote_proc_pid_lookup(struct inode *dir, struct dentry *dentry,
		pid_t tgid) {
	/* try and locate pid in the cluster */
	struct dentry *result = ERR_PTR(-ENOENT);
	struct proc_remote_pid_info task;

	task.pid = tgid;
	task.euid = 0;
	task.egid = 0;
	task.Kernel_Num = _cpu;

	result = remote_proc_pid_instantiate(dir, dentry, &task, NULL);

	return result;
}

static int remote_proc_pid_fill_cache(int Kernel_id, struct file *filp,
		void *dirent, filldir_t filldir, struct tgid_iter iter) {
	char name[PROC_NUMBUF];
	int len = snprintf(name, sizeof(name), "%d", iter.tgid);
	struct proc_remote_pid_info proc_task;

	int retval = 0;

	proc_task.pid = iter.tgid;
	proc_task.Kernel_Num = Kernel_id;
	proc_task.euid = 0;
	proc_task.egid = 0;

	retval = remote_proc_fill_cache(filp, dirent, filldir, name, len,
			remote_proc_pid_instantiate, &proc_task, NULL);

	return retval;
}

int send_request_to_remote(int KernelId) {

	int res = 0;
	_remote_pid_request_t* request = kmalloc(sizeof(_remote_pid_request_t),
	GFP_KERNEL);
	// Build request
	request->header.type = PCN_KMSG_TYPE_REMOTE_PID_REQUEST;
	request->header.prio = PCN_KMSG_PRIO_NORMAL;

	// Send response
	res = pcn_kmsg_send(KernelId, (struct pcn_kmsg_message*) (request));
	return res;
}

int fill_next_remote_tgids(int Kernel_id, struct file *filp, void *dirent,
		filldir_t filldir, loff_t offset) {
	struct tgid_iter iter;

	//flush the structure holding the previous result
	flush_variables();
	int result = 0;
	int i;
	int retval = -1;

	result = send_request_to_remote(Kernel_id);

	if (!result) {
		PRINTK("%s fill_next_remote_tgids: go to sleep!!!!", __func__);
		wait_event_interruptible(wq, wait != -1);
		wait = -1;

		for (i = 0; i < pid_result->count; i++) {
			iter.tgid = pid_result->remote_pid[i];
			filp->f_pos = iter.tgid + offset;
			iter.task = NULL;
			retval = remote_proc_pid_fill_cache(Kernel_id, filp, dirent,
					filldir, iter);

			if (retval < 0) {
				retval = -EAGAIN;
				return retval;
			}
		}
		retval = pid_result->count + 1;	//< ARRAY_SIZE(pid_arr) ? 0 : pid_count;
	}

	return retval;

}

int fill_next_tgids(int Kernel_id, struct file *filp, void *dirent,
		filldir_t filldir, loff_t offset) {
	pid_t tgid;
	int retval;

	retval = fill_next_remote_tgids(Kernel_id, filp, dirent, filldir, offset);
	if (retval > 0) {
		tgid = filp->f_pos - offset;
		if ((tgid & INTERNAL_PID_MASK) >= PID_MAX_LIMIT - 1) {
			retval = 0;
		}
		filp->f_pos++;
	}

	return retval;
}

int remote_proc_pid_readdir(struct file *filp, void *dirent, filldir_t filldir,
		loff_t offset) {

	flush_variables();
	pid_t tgid;
	int node;
	int retval = 0, i;

	tgid = filp->f_pos - offset;
	int global = (tgid & GLOBAL_PID_MASK);

	if (!global) {
		tgid = GLOBAL_PID_NODE(0, 0);
		filp->f_pos = tgid + offset;
	}
	node = ORIG_NODE(tgid);
	PRINTK("%s Kernel ID: %d\n", __func__, node);
	filp->f_pos = GLOBAL_PID_NODE(0, node) + offset;
	for (i = 0; i < NR_CPUS; i++) {

		// Skip the current cpu
		if (i == _cpu)
			continue;
		retval = fill_next_tgids(i, filp, dirent, filldir, offset);
		if (!retval) {
		}
	}

	return retval;

}

static int __init pid_handler_init(void)
{


#ifndef SUPPORT_FOR_CLUSTERING
      _cpu= smp_processor_id();
#else
      _cpu= cpumask_first(cpu_present_mask);
#endif

	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PID_REQUEST,
				    		handle_remote_pid_request);
	pcn_kmsg_register_callback(PCN_KMSG_TYPE_REMOTE_PID_RESPONSE,
				    		handle_remote_pid_response);


	return 0;
}
/**
 * Register remote pid init function as
 * module initialization function.
 */
late_initcall(pid_handler_init);
