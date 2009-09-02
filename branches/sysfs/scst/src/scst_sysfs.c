#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/ctype.h>

#include "scst.h"
#include "scst_priv.h"
#include "scst_mem.h"

static DEFINE_MUTEX(scst_sysfs_mutex);

static DECLARE_COMPLETION(scst_sysfs_root_release_completion);

static struct kobject scst_sysfs_root_kobj;
static struct kobject *scst_targets_kobj;
static struct kobject *scst_devices_kobj;
static struct kobject *scst_sgv_kobj;
static struct kobject *scst_back_drivers_kobj;

static struct sysfs_ops scst_sysfs_ops;

static const char *scst_dev_handler_types[] =
{
    "Direct-access device (e.g., magnetic disk)",
    "Sequential-access device (e.g., magnetic tape)",
    "Printer device",
    "Processor device",
    "Write-once device (e.g., some optical disks)",
    "CD-ROM device",
    "Scanner device (obsolete)",
    "Optical memory device (e.g., some optical disks)",
    "Medium changer device (e.g., jukeboxes)",
    "Communications device (obsolete)",
    "Defined by ASC IT8 (Graphic arts pre-press devices)",
    "Defined by ASC IT8 (Graphic arts pre-press devices)",
    "Storage array controller device (e.g., RAID)",
    "Enclosure services device",
    "Simplified direct-access device (e.g., magnetic disk)",
    "Optical card reader/writer device"
};

#if defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING)

static DEFINE_MUTEX(scst_log_mutex);

static struct scst_trace_log scst_trace_tbl[] =
{
    { TRACE_OUT_OF_MEM,		"out_of_mem" },
    { TRACE_MINOR,		"minor" },
    { TRACE_SG_OP,		"sg" },
    { TRACE_MEMORY,		"mem" },
    { TRACE_BUFF,		"buff" },
    { TRACE_ENTRYEXIT,		"entryexit" },
    { TRACE_PID,		"pid" },
    { TRACE_LINE,		"line" },
    { TRACE_FUNCTION,		"function" },
    { TRACE_DEBUG,		"debug" },
    { TRACE_SPECIAL,		"special" },
    { TRACE_SCSI,		"scsi" },
    { TRACE_MGMT,		"mgmt" },
    { TRACE_MGMT_MINOR,		"mgmt_minor" },
    { TRACE_MGMT_DEBUG,		"mgmt_dbg" },
    { 0,			NULL }
};

static struct scst_trace_log scst_local_trace_tbl[] =
{
    { TRACE_RTRY,		"retry" },
    { TRACE_SCSI_SERIALIZING,	"scsi_serializing" },
    { TRACE_RCV_BOT,		"recv_bot" },
    { TRACE_SND_BOT,		"send_bot" },
    { TRACE_RCV_TOP,		"recv_top" },
    { TRACE_SND_TOP,		"send_top" },
    { 0,			NULL }
};

#endif /* defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING) */

static ssize_t scst_luns_mgmt_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf);
static ssize_t scst_luns_mgmt_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count);

static void scst_sysfs_release(struct kobject *kobj)
{
	kfree(kobj);
}

/*
 * Target Template
 */

static void scst_tgtt_release(struct kobject *kobj)
{
	struct scst_tgt_template *tgtt;

	TRACE_ENTRY();

	tgtt = container_of(kobj, struct scst_tgt_template, tgtt_kobj);

	complete_all(&tgtt->tgtt_kobj_release_cmpl);

	TRACE_EXIT();
	return;
}

static struct kobj_type tgtt_ktype = {
	.release = scst_tgtt_release,
};

int scst_create_tgtt_sysfs(struct scst_tgt_template *tgtt)
{
	int retval = 0;

	TRACE_ENTRY();

	init_completion(&tgtt->tgtt_kobj_release_cmpl);
	tgtt->tgtt_kobj_initialized = 1;

	retval = kobject_init_and_add(&tgtt->tgtt_kobj, &tgtt_ktype,
			scst_targets_kobj, tgtt->name);
	if (retval != 0) {
		PRINT_ERROR("Can't add tgtt %s to sysfs", tgtt->name);
		goto out;
	}

out:
	TRACE_EXIT_RES(retval);
	return retval;
}

void scst_release_tgtt_sysfs(struct scst_tgt_template *tgtt)
{
	TRACE_ENTRY();

	if (tgtt->tgtt_kobj_initialized) {
		int rc;

		kobject_del(&tgtt->tgtt_kobj);
		kobject_put(&tgtt->tgtt_kobj);

		rc = wait_for_completion_timeout(&tgtt->tgtt_kobj_release_cmpl, HZ);
		if (rc == 0) {
			PRINT_INFO("Waiting for releasing sysfs entry "
				"for target template %s...", tgtt->name);
			wait_for_completion(&tgtt->tgtt_kobj_release_cmpl);
			PRINT_INFO("Done waiting for releasing sysfs "
				"entry for target template %s", tgtt->name);
		}
	}

	TRACE_EXIT();
	return;
}

/*
 * Target directory implementation
 */

static void scst_tgt_free(struct kobject *kobj)
{
	struct scst_tgt *tgt;

	TRACE_ENTRY();

	tgt = container_of(kobj, struct scst_tgt, tgt_kobj);

	kfree(tgt);

	TRACE_EXIT();
	return;
}

static struct kobj_type tgt_ktype = {
	.release = scst_tgt_free,
};

static struct kobj_attribute scst_luns_mgmt =
	__ATTR(mgmt, S_IRUGO | S_IWUSR, scst_luns_mgmt_show,
	       scst_luns_mgmt_store);

int scst_create_tgt_sysfs(struct scst_tgt *tgt)
{
	int retval;

	TRACE_ENTRY();

	tgt->tgt_kobj_initialized = 1;

	retval = kobject_init_and_add(&tgt->tgt_kobj, &tgt_ktype,
			&tgt->tgtt->tgtt_kobj, tgt->tgt_name);
	if (retval != 0) {
		PRINT_ERROR("Can't add tgt %s to sysfs", tgt->tgt_name);
		goto out;
	}

	tgt->tgt_sess_kobj = kobject_create_and_add("sessions", &tgt->tgt_kobj);
	if (tgt->tgt_sess_kobj == NULL) {
		PRINT_ERROR("Can't create sess kobj for tgt %s", tgt->tgt_name);
		goto out_sess_obj_err;
	}

	tgt->tgt_luns_kobj = kobject_create_and_add("luns", &tgt->tgt_kobj);
	if (tgt->tgt_luns_kobj == NULL) {
		PRINT_ERROR("Can't create luns kobj for tgt %s", tgt->tgt_name);
		goto luns_kobj_err;
	}

	if (sysfs_create_file(tgt->tgt_luns_kobj, &scst_luns_mgmt.attr) != 0)
		goto create_luns_mgmt_err;

	tgt->tgt_ini_grp_kobj = kobject_create_and_add("ini_group",
						    &tgt->tgt_kobj);
	if (tgt->tgt_ini_grp_kobj == NULL) {
		PRINT_ERROR("Can't create ini_grp kobj for tgt %s",
			tgt->tgt_name);
		goto ini_grp_kobj_err;
	}

out:
	TRACE_EXIT_RES(retval);
	return retval;

ini_grp_kobj_err:
	sysfs_remove_file(tgt->tgt_luns_kobj, &scst_luns_mgmt.attr);

create_luns_mgmt_err:
	kobject_del(tgt->tgt_luns_kobj);
	kobject_put(tgt->tgt_luns_kobj);

luns_kobj_err:
	kobject_del(tgt->tgt_sess_kobj);
	kobject_put(tgt->tgt_sess_kobj);

out_sess_obj_err:
	kobject_del(&tgt->tgt_kobj);
	retval = -ENOMEM;
	goto out;
}

void scst_release_sysfs_and_tgt(struct scst_tgt *tgt)
{
	kobject_del(tgt->tgt_sess_kobj);
	kobject_put(tgt->tgt_sess_kobj);

	sysfs_remove_file(tgt->tgt_luns_kobj, &scst_luns_mgmt.attr);

	kobject_del(tgt->tgt_luns_kobj);
	kobject_put(tgt->tgt_luns_kobj);

	kobject_del(tgt->tgt_ini_grp_kobj);
	kobject_put(tgt->tgt_ini_grp_kobj);

	kobject_del(&tgt->tgt_kobj);
	kobject_put(&tgt->tgt_kobj);
	return;
}

/*
 * Target sessions directory implementation
 */

ssize_t scst_sess_sysfs_commands_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct scst_session *sess;

	sess = container_of(kobj, struct scst_session, sess_kobj);

	return sprintf(buf, "%i\n", atomic_read(&sess->sess_cmd_count));
}

static struct kobj_attribute session_commands_attr =
	__ATTR(commands, S_IRUGO, scst_sess_sysfs_commands_show, NULL);

static struct attribute *scst_session_attrs[] = {
	&session_commands_attr.attr,
	NULL,
};

static void __scst_session_release(struct scst_session *sess)
{
	TRACE_ENTRY();

	kfree(sess->initiator_name);
	kmem_cache_free(scst_sess_cachep, sess);

	TRACE_EXIT();
	return;
}

static void scst_session_release(struct kobject *kobj)
{
	struct scst_session *sess;

	TRACE_ENTRY();

	sess = container_of(kobj, struct scst_session, sess_kobj);

	__scst_session_release(sess);

	TRACE_EXIT();
	return;
}

static struct kobj_type scst_session_ktype = {
	.sysfs_ops = &scst_sysfs_ops,
	.release = scst_session_release,
	.default_attrs = scst_session_attrs,
};

int scst_create_sess_sysfs(struct scst_session *sess)
{
	int retval = 0;

	TRACE_ENTRY();

	sess->sess_kobj_initialized = 1;

	retval = kobject_init_and_add(&sess->sess_kobj, &scst_session_ktype,
			      sess->tgt->tgt_sess_kobj, sess->initiator_name);
	if (retval != 0) {
		PRINT_ERROR("Can't add session %s to sysfs",
			    sess->initiator_name);
		goto out;
	}

out:
	TRACE_EXIT_RES(retval);
	return retval;
}

void scst_release_sysfs_and_sess(struct scst_session *sess)
{
	TRACE_ENTRY();

	if (sess->sess_kobj_initialized)
		kobject_put(&sess->sess_kobj);
	else
		__scst_session_release(sess);

	TRACE_EXIT();
	return;
}

/*
 * Target luns directory implementation
 */

static void scst_acg_dev_release(struct kobject *kobj)
{
	struct scst_acg_dev *acg_dev;

	TRACE_ENTRY();

	acg_dev = container_of(kobj, struct scst_acg_dev, acg_dev_kobj);

	__scst_acg_dev_free(acg_dev);

	TRACE_EXIT();
	return;
}

static ssize_t scst_lun_options_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf)
{
	struct scst_acg_dev *acg_dev;

	acg_dev = container_of(kobj, struct scst_acg_dev, acg_dev_kobj);

	return sprintf(buf, "%s\n",
		(acg_dev->rd_only || acg_dev->dev->rd_only) ?
			 "READ_ONLY" : "");
}

static struct kobj_attribute lun_options_attr =
	__ATTR(options, S_IRUGO, scst_lun_options_show, NULL);

static struct attribute *lun_attrs[] = {
	&lun_options_attr.attr,
	NULL,
};

static struct kobj_type acg_dev_ktype = {
	.sysfs_ops = &scst_sysfs_ops,
	.release = scst_acg_dev_release,
	.default_attrs = lun_attrs,
};

int scst_create_acg_dev_sysfs(struct scst_acg *acg, unsigned int virt_lun,
			  struct kobject *parent)
{
	int retval;
	struct scst_acg_dev *acg_dev = NULL, *acg_dev_tmp;

	TRACE_ENTRY();

	list_for_each_entry(acg_dev_tmp, &acg->acg_dev_list,
			    acg_dev_list_entry) {
		if (acg_dev_tmp->lun == virt_lun) {
			acg_dev = acg_dev_tmp;
			break;
		}
	}

	if (acg_dev == NULL) {
		PRINT_ERROR("%s", "acg_dev lookup for kobject creation failed");
		retval = -EINVAL;
		goto out;
	}

	acg_dev->acg_dev_kobj_initialized = 1;
	retval = kobject_init_and_add(&acg_dev->acg_dev_kobj, &acg_dev_ktype,
				      parent, "%u", virt_lun);
	if (retval != 0) {
		PRINT_ERROR("Can't add acg %s to sysfs", acg->acg_name);
		goto out;
	}

/* 	XXX: change the second parameter to the kobject where */
/* 	the link will point to. */
	retval = sysfs_create_link(&acg_dev->acg_dev_kobj,
			&acg_dev->acg_dev_kobj, "device");
	if (retval != 0) {
		PRINT_ERROR("Can't create acg %s device link", acg->acg_name);
		goto out;
	}

out:
	return retval;
}

static ssize_t scst_luns_mgmt_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf)
{
	static char *help = "Usage: echo \"add|del H:C:I:L lun [READ_ONLY]\" "
					">mgmt\n"
			    "       echo \"add|del VNAME lun [READ_ONLY]\" "
					">mgmt\n";

	return sprintf(buf, help);
}

static ssize_t scst_luns_mgmt_store(struct kobject *kobj,
				    struct kobj_attribute *attr,
				    const char *buf, size_t count)
{
	int res, virt = 0, read_only = 0, action;
	char *buffer, *p, *e = NULL;
	unsigned int host, channel = 0, id = 0, lun = 0, virt_lun;
	struct scst_acg *acg;
	struct scst_acg_dev *acg_dev = NULL, *acg_dev_tmp;
	struct scst_device *d, *dev = NULL;
	struct scst_tgt *tgt;

#define SCST_LUN_ACTION_ADD	1
#define SCST_LUN_ACTION_DEL	2

	TRACE_ENTRY();

	tgt = container_of(kobj->parent, struct scst_tgt, tgt_kobj);
	acg = tgt->default_acg;

	buffer = kzalloc(count+1, GFP_KERNEL);
	if (buffer == NULL) {
		res = -ENOMEM;
		goto out;
	}

	memcpy(buffer, buf, count);
	buffer[count] = '\0';
	p = buffer;

	p = buffer;
	if (p[strlen(p) - 1] == '\n')
		p[strlen(p) - 1] = '\0';
	if (strncasecmp("add", p, 3) == 0) {
		p += 3;
		action = SCST_LUN_ACTION_ADD;
	} else if (strncasecmp("del", p, 3) == 0) {
		p += 3;
		action = SCST_LUN_ACTION_DEL;
	} else {
		PRINT_ERROR("Unknown action \"%s\"", p);
		res = -EINVAL;
		goto out_free;
	}

	if (!isspace(*p)) {
		PRINT_ERROR("%s", "Syntax error");
		res = -EINVAL;
		goto out_free;
	}

	res = scst_suspend_activity(true);
	if (res != 0)
		goto out_free;

	if (mutex_lock_interruptible(&scst_mutex) != 0) {
		res = -EINTR;
		goto out_free_resume;
	}

	while (isspace(*p) && *p != '\0')
		p++;
	e = p; /* save p */
	host = simple_strtoul(p, &p, 0);
	if (*p == ':') {
		channel = simple_strtoul(p + 1, &p, 0);
		id = simple_strtoul(p + 1, &p, 0);
		lun = simple_strtoul(p + 1, &p, 0);
		e = p;
	} else {
		virt++;
		p = e; /* restore p */
		while (!isspace(*e) && *e != '\0')
			e++;
		*e = '\0';
	}

	list_for_each_entry(d, &scst_dev_list, dev_list_entry) {
		if (virt) {
			if (d->virt_id && !strcmp(d->virt_name, p)) {
				dev = d;
				TRACE_DBG("Virt device %p (%s) found",
					  dev, p);
				break;
			}
		} else {
			if (d->scsi_dev &&
			    d->scsi_dev->host->host_no == host &&
			    d->scsi_dev->channel == channel &&
			    d->scsi_dev->id == id &&
			    d->scsi_dev->lun == lun) {
				dev = d;
				TRACE_DBG("Dev %p (%d:%d:%d:%d) found",
					  dev, host, channel, id, lun);
				break;
			}
		}
	}
	if (dev == NULL) {
		if (virt) {
			PRINT_ERROR("Virt device %s not found", p);
		} else {
			PRINT_ERROR("Device %d:%d:%d:%d not found",
				    host, channel, id, lun);
		}
		res = -EINVAL;
		goto out_free_up;
	}

	switch (action) {
	case SCST_LUN_ACTION_ADD:
		e++;
		while (isspace(*e) && *e != '\0')
			e++;
		virt_lun = simple_strtoul(e, &e, 0);

		while (isspace(*e) && *e != '\0')
			e++;

		if (*e != '\0') {
			if ((strncasecmp("READ_ONLY", e, 9) == 0) &&
			    (isspace(e[9]) || (e[9] == '\0')))
				read_only = 1;
			else {
				PRINT_ERROR("Unknown option \"%s\"", e);
				res = -EINVAL;
				goto out_free_up;
			}
		}

		acg_dev = NULL;
		list_for_each_entry(acg_dev_tmp, &acg->acg_dev_list,
				    acg_dev_list_entry) {
			if (acg_dev_tmp->lun == virt_lun) {
				acg_dev = acg_dev_tmp;
				break;
			}
		}

		if (acg_dev != NULL) {
			acg_dev = acg_dev_tmp;
			PRINT_ERROR("virt lun %d already exists in group %s",
				    virt_lun, acg->acg_name);
			res = -EINVAL;
			goto out_free_up;
		}


		res = scst_acg_add_dev(acg, dev, virt_lun, read_only, true);
		if (res != 0)
			goto out_free_up;

		res = scst_create_acg_dev_sysfs(acg, virt_lun, kobj);
		if (res != 0) {
			PRINT_ERROR("%s", "creation of acg_dev kobject failed");
			goto out_remove_acg_dev;
		}
		break;
	case SCST_LUN_ACTION_DEL:
		res = scst_acg_remove_dev(acg, dev, true);
		if (res != 0)
			goto out_free_up;
		break;
	}

	res = count;

out_free_up:
	mutex_unlock(&scst_mutex);

out_free_resume:
	scst_resume_activity();

out_free:
	kfree(buffer);

out:
	TRACE_EXIT_RES(res);
	return res;

out_remove_acg_dev:
	scst_acg_remove_dev(acg, dev, true);
	goto out_free_up;

#undef SCST_LUN_ACTION_ADD
#undef SCST_LUN_ACTION_DEL
}

/*
 * SGV directory implementation
 */

static struct kobj_attribute sgv_stat_attr =
	__ATTR(stats, S_IRUGO, sgv_sysfs_stat_show, NULL);

static struct attribute *sgv_attrs[] = {
	&sgv_stat_attr.attr,
	NULL,
};

static void sgv_release(struct kobject *kobj)
{
	struct sgv_pool *pool;

	TRACE_ENTRY();

	pool = container_of(kobj, struct sgv_pool, sgv_kobj);

	kfree(pool);

	TRACE_EXIT();
	return;
}

static struct kobj_type sgv_pool_ktype = {
	.sysfs_ops = &scst_sysfs_ops,
	.release = sgv_release,
	.default_attrs = sgv_attrs,
};

int scst_create_sgv_sysfs(struct sgv_pool *pool)
{
	int retval;

	TRACE_ENTRY();

	retval = kobject_init_and_add(&pool->sgv_kobj, &sgv_pool_ktype,
			scst_sgv_kobj, pool->name);
	if (retval != 0) {
		PRINT_ERROR("Can't add sgv pool %s to sysfs", pool->name);
		goto out;
	}

out:
	TRACE_EXIT_RES(retval);
	return retval;
}

/* pool can be dead upon exit from this function! */
void scst_cleanup_sgv_sysfs_put(struct sgv_pool *pool)
{
	kobject_del(&pool->sgv_kobj);
	kobject_put(&pool->sgv_kobj);
	return;
}

static struct kobj_attribute sgv_global_stat_attr =
	__ATTR(global_stats, S_IRUGO, sgv_sysfs_global_stat_show, NULL);

static struct attribute *sgv_default_attrs[] = {
	&sgv_global_stat_attr.attr,
	NULL,
};

static struct kobj_type sgv_ktype = {
	.sysfs_ops = &scst_sysfs_ops,
	.release = scst_sysfs_release,
	.default_attrs = sgv_default_attrs,
};

/*
 * SCST sysfs root directory implementation
 */

static ssize_t scst_threads_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int count;

	TRACE_ENTRY();

	count = sprintf(buf, "%d\n", scst_global_threads_count());

	TRACE_EXIT();
	return count;
}

static ssize_t scst_threads_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int res = count;
	int oldtn, newtn, delta;

	TRACE_ENTRY();

	if (mutex_lock_interruptible(&scst_sysfs_mutex) != 0) {
		res = -EINTR;
		goto out;
	}

	mutex_lock(&scst_global_threads_mutex);

	oldtn = scst_nr_global_threads;
	sscanf(buf, "%du", &newtn);

	if (newtn <= 0) {
		PRINT_ERROR("Illegal threads num value %d", newtn);
		res = -EINVAL;
		goto out_up_thr_free;
	}
	delta = newtn - oldtn;
	if (delta < 0)
		__scst_del_global_threads(-delta);
	else
		__scst_add_global_threads(delta);

	PRINT_INFO("Changed cmd threads num: old %d, new %d", oldtn, newtn);

out_up_thr_free:
	mutex_unlock(&scst_global_threads_mutex);

	mutex_unlock(&scst_sysfs_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;
}

#if defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING)

static void scst_read_trace_tlb(const struct scst_trace_log *tbl, char *buf,
	unsigned long log_level, int *pos)
{
	const struct scst_trace_log *t = tbl;

	while (t->token) {
		if (log_level & t->val) {
			*pos += sprintf(&buf[*pos], "%s%s",
					(*pos == 0) ? "" : " | ",
					t->token);
		}
		t++;
	}
	return;
}

static ssize_t scst_trace_level_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int pos = 0;
	
	scst_read_trace_tlb(scst_trace_tbl, buf, trace_flag, &pos);
	scst_read_trace_tlb(scst_local_trace_tbl, buf, trace_flag, &pos);

	pos += sprintf(&buf[pos], "\n\n\nUsage:\n"
		"	echo \"all|none|default\" >trace_level\n"
		"	echo \"value DEC|0xHEX|0OCT\" >trace_level\n"
		"	echo \"add|del TOKEN\" >trace_level\n"
		"\nwhere TOKEN is one of [debug, function, line, pid,\n"
		"		       entryexit, buff, mem, sg, out_of_mem,\n"
		"		       special, scsi, mgmt, minor,\n"
		"		       mgmt_minor, mgmt_dbg, scsi_serializing,\n"
		"		       retry, recv_bot, send_bot, recv_top,\n"
		"		       send_top]");

	return pos;
}

static int scst_write_trace(const char *buf, size_t length,
	unsigned long *log_level, unsigned long default_level,
	const char *name, const struct scst_trace_log *tbl)
{
	int res = length;
	int action;
	unsigned long level = 0, oldlevel;
	char *buffer, *p, *e;
	const struct scst_trace_log *t;

#define SCST_TRACE_ACTION_ALL		1
#define SCST_TRACE_ACTION_NONE		2
#define SCST_TRACE_ACTION_DEFAULT	3
#define SCST_TRACE_ACTION_ADD		4
#define SCST_TRACE_ACTION_DEL		5
#define SCST_TRACE_ACTION_VALUE		6

	TRACE_ENTRY();

	if ((buf == NULL) || (length == 0)) {
		res = -EINVAL;
		goto out;
	}

	buffer = kmalloc(length+1, GFP_KERNEL);
	if (buffer == NULL) {
		PRINT_ERROR("Unable to alloc intermediate buffer (size %d)",
			length+1);
		res = -ENOMEM;
		goto out;
	}
	memcpy(buffer, buf, length);
	buffer[length] = '\0';

	p = buffer;
	if (!strncasecmp("all", p, 3)) {
		action = SCST_TRACE_ACTION_ALL;
	} else if (!strncasecmp("none", p, 4) || !strncasecmp("null", p, 4)) {
		action = SCST_TRACE_ACTION_NONE;
	} else if (!strncasecmp("default", p, 7)) {
		action = SCST_TRACE_ACTION_DEFAULT;
	} else if (!strncasecmp("add", p, 3)) {
		p += 3;
		action = SCST_TRACE_ACTION_ADD;
	} else if (!strncasecmp("del", p, 3)) {
		p += 3;
		action = SCST_TRACE_ACTION_DEL;
	} else if (!strncasecmp("value", p, 5)) {
		p += 5;
		action = SCST_TRACE_ACTION_VALUE;
	} else {
		if (p[strlen(p) - 1] == '\n')
			p[strlen(p) - 1] = '\0';
		PRINT_ERROR("Unknown action \"%s\"", p);
		res = -EINVAL;
		goto out_free;
	}

	switch (action) {
	case SCST_TRACE_ACTION_ADD:
	case SCST_TRACE_ACTION_DEL:
	case SCST_TRACE_ACTION_VALUE:
		if (!isspace(*p)) {
			PRINT_ERROR("%s", "Syntax error");
			res = -EINVAL;
			goto out_free;
		}
	}

	switch (action) {
	case SCST_TRACE_ACTION_ALL:
		level = TRACE_ALL;
		break;
	case SCST_TRACE_ACTION_DEFAULT:
		level = default_level;
		break;
	case SCST_TRACE_ACTION_NONE:
		level = TRACE_NULL;
		break;
	case SCST_TRACE_ACTION_ADD:
	case SCST_TRACE_ACTION_DEL:
		while (isspace(*p) && *p != '\0')
			p++;
		e = p;
		while (!isspace(*e) && *e != '\0')
			e++;
		*e = 0;
		if (tbl) {
			t = tbl;
			while (t->token) {
				if (!strcasecmp(p, t->token)) {
					level = t->val;
					break;
				}
				t++;
			}
		}
		if (level == 0) {
			t = scst_trace_tbl;
			while (t->token) {
				if (!strcasecmp(p, t->token)) {
					level = t->val;
					break;
				}
				t++;
			}
		}
		if (level == 0) {
			PRINT_ERROR("Unknown token \"%s\"", p);
			res = -EINVAL;
			goto out_free;
		}
		break;
	case SCST_TRACE_ACTION_VALUE:
		while (isspace(*p) && *p != '\0')
			p++;
		level = simple_strtoul(p, NULL, 0);
		break;
	}

	oldlevel = *log_level;

	switch (action) {
	case SCST_TRACE_ACTION_ADD:
		*log_level |= level;
		break;
	case SCST_TRACE_ACTION_DEL:
		*log_level &= ~level;
		break;
	default:
		*log_level = level;
		break;
	}

	PRINT_INFO("Changed trace level for \"%s\": old 0x%08lx, new 0x%08lx",
		name, oldlevel, *log_level);

out_free:
	kfree(buffer);
out:
	TRACE_EXIT_RES(res);
	return res;

#undef SCST_TRACE_ACTION_ALL
#undef SCST_TRACE_ACTION_NONE
#undef SCST_TRACE_ACTION_DEFAULT
#undef SCST_TRACE_ACTION_ADD
#undef SCST_TRACE_ACTION_DEL
#undef SCST_TRACE_ACTION_VALUE
}

static ssize_t scst_trace_level_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int res;

	TRACE_ENTRY();

	if (mutex_lock_interruptible(&scst_log_mutex) != 0) {
		res = -EINTR;
		goto out;
	}

	res = scst_write_trace(buf, count, &trace_flag,
		SCST_DEFAULT_LOG_FLAGS, "scst", scst_local_trace_tbl);

	mutex_unlock(&scst_log_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;
}

#endif /* defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING) */

static ssize_t scst_version_show(struct kobject *kobj,
				 struct kobj_attribute *attr,
				 char *buf)
{
	TRACE_ENTRY();

	sprintf(buf, "%s\n", SCST_VERSION_STRING);

#ifdef CONFIG_SCST_STRICT_SERIALIZING
	strcat(buf, "Strict serializing enabled\n");
#endif

#ifdef CONFIG_SCST_EXTRACHECKS
	strcat(buf, "EXTRACHECKS\n");
#endif

#ifdef CONFIG_SCST_TRACING
	strcat(buf, "TRACING\n");
#endif

#ifdef CONFIG_SCST_DEBUG
	strcat(buf, "DEBUG\n");
#endif

#ifdef CONFIG_SCST_DEBUG_TM
	strcat(buf, "DEBUG_TM\n");
#endif

#ifdef CONFIG_SCST_DEBUG_RETRY
	strcat(buf, "DEBUG_RETRY\n");
#endif

#ifdef CONFIG_SCST_DEBUG_OOM
	strcat(buf, "DEBUG_OOM\n");
#endif

#ifdef CONFIG_SCST_DEBUG_SN
	strcat(buf, "DEBUG_SN\n");
#endif

#ifdef CONFIG_SCST_USE_EXPECTED_VALUES
	strcat(buf, "USE_EXPECTED_VALUES\n");
#endif

#ifdef CONFIG_SCST_ALLOW_PASSTHROUGH_IO_SUBMIT_IN_SIRQ
	strcat(buf, "ALLOW_PASSTHROUGH_IO_SUBMIT_IN_SIRQ\n");
#endif

#ifdef CONFIG_SCST_STRICT_SECURITY
	strcat(buf, "SCST_STRICT_SECURITY\n");
#endif

	TRACE_EXIT();
	return strlen(buf);
}

static struct kobj_attribute scst_threads_attr =
	__ATTR(threads, S_IRUGO | S_IWUSR, scst_threads_show,
	       scst_threads_store);

#if defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING)
static struct kobj_attribute scst_trace_level_attr =
	__ATTR(trace_level, S_IRUGO | S_IWUSR, scst_trace_level_show,
	       scst_trace_level_store);
#endif

static struct kobj_attribute scst_version_attr =
	__ATTR(version, S_IRUGO, scst_version_show, NULL);

static struct attribute *scst_sysfs_root_default_attrs[] = {
	&scst_threads_attr.attr,
#if defined(CONFIG_SCST_DEBUG) || defined(CONFIG_SCST_TRACING)
	&scst_trace_level_attr.attr,
#endif
	&scst_version_attr.attr,
	NULL,
};

static void scst_sysfs_root_release(struct kobject *kobj)
{
	complete_all(&scst_sysfs_root_release_completion);
}

static ssize_t scst_show(struct kobject *kobj, struct attribute *attr,
			 char *buf)
{
	struct kobj_attribute *kobj_attr;
	kobj_attr = container_of(attr, struct kobj_attribute, attr);

	return kobj_attr->show(kobj, kobj_attr, buf);
}

static ssize_t scst_store(struct kobject *kobj, struct attribute *attr,
			  const char *buf, size_t count)
{
	struct kobj_attribute *kobj_attr;
	kobj_attr = container_of(attr, struct kobj_attribute, attr);

	return kobj_attr->store(kobj, kobj_attr, buf, count);
}

static struct sysfs_ops scst_sysfs_ops = {
        .show = scst_show,
        .store = scst_store,
};

static struct kobj_type scst_sysfs_root_ktype = {
	.sysfs_ops = &scst_sysfs_ops,
	.release = scst_sysfs_root_release,
	.default_attrs = scst_sysfs_root_default_attrs,
};

static void scst_devt_free(struct kobject *kobj)
{
	struct scst_dev_type *devt;

	TRACE_ENTRY();

	devt = container_of(kobj, struct scst_dev_type, devt_kobj);

	if (devt->devt_ktype.default_attrs != NULL) {
		kfree(devt->devt_ktype.default_attrs);
		devt->devt_ktype.default_attrs = NULL;
	}

	complete_all(&devt->devt_kobj_release_compl);

	TRACE_EXIT();
	return;
}

static ssize_t scst_devt_trace_level_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int pos = 0;
	struct scst_dev_type *devt;

	devt = container_of(kobj, struct scst_dev_type, devt_kobj);

	scst_read_trace_tlb(scst_trace_tbl, buf, *devt->trace_flags, &pos);
	if (devt->trace_tbl != NULL)
		scst_read_trace_tlb(devt->trace_tbl, buf, *devt->trace_flags,
					&pos);

	return pos;
}

static ssize_t scst_devt_trace_level_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int res;
	struct scst_dev_type *devt;

	TRACE_ENTRY();

	devt = container_of(kobj, struct scst_dev_type, devt_kobj);

	if (mutex_lock_interruptible(&scst_log_mutex) != 0) {
		res = -EINTR;
		goto out;
	}

	res = scst_write_trace(buf, count, devt->trace_flags,
		devt->default_trace_flags, devt->name, devt->trace_tbl);

	mutex_unlock(&scst_log_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_devt_type_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	int pos;
	struct scst_dev_type *devt;

	devt = container_of(kobj, struct scst_dev_type, devt_kobj);

	pos = sprintf(buf, "%d - %s\n", devt->type,
		(unsigned)devt->type > ARRAY_SIZE(scst_dev_handler_types) ?
		   	"unknown" : scst_dev_handler_types[devt->type]);

	return pos;
}

static struct kobj_attribute scst_devt_type_attr =
	__ATTR(type, S_IRUGO, scst_devt_type_show, NULL);

int scst_create_devt_sysfs(struct scst_dev_type *devt)
{
	int retval;
	struct attribute **attrs = NULL;
	int count = 2; /* 1st for type, 2d for the end zero entry */
	int curr;
	struct kobject *parent;

	TRACE_ENTRY();

	init_completion(&devt->devt_kobj_release_compl);

	if ((devt->trace_flags != NULL) || (devt->attrs != NULL)) {
		if (devt->trace_flags != NULL) {
			count++;
			TRACE_DBG("trace_flags, count %d", count);
		}

		curr = 0;
		if (devt->attrs != NULL) {
			while (devt->attrs[curr] != NULL) {
				count++;
				curr++;
			}
		}
	}

	TRACE_DBG("Allocating %d attrs", count-1);

	attrs = kzalloc(sizeof(attrs[0]) * count, GFP_KERNEL);
	if (attrs == NULL) {
		PRINT_ERROR("Unable to allocate devt_attrs "
			"array for %d entries", count);
		retval = -ENOMEM;
		goto out;
	}

	curr = 0;

	attrs[curr] = &scst_devt_type_attr.attr;
	curr++;

	if (devt->trace_flags != NULL) {
		static struct kobj_attribute devtt_trace_attr =
			__ATTR(trace_level, S_IRUGO | S_IWUSR,
				scst_devt_trace_level_show,
				scst_devt_trace_level_store);
		attrs[curr] = &devtt_trace_attr.attr;
		curr++;
	}

	if (devt->attrs != NULL) {
		int i;
		for (i = 0; devt->attrs[i] != NULL; i++, curr++)
			attrs[curr] = devt->attrs[i];
	}

	/* attrs[count-1] set to NULL by kzalloc() */

	memset(&devt->devt_ktype, 0, sizeof(devt->devt_ktype));
	devt->devt_ktype.release = scst_devt_free;
	devt->devt_ktype.sysfs_ops = &scst_sysfs_ops;
	devt->devt_ktype.default_attrs = attrs;

	devt->devt_kobj_initialized = 1;

	if (devt->parent != NULL)
		parent = &devt->parent->devt_kobj;
	else
		parent = scst_back_drivers_kobj;

	retval = kobject_init_and_add(&devt->devt_kobj, &devt->devt_ktype,
			parent, devt->name);
	if (retval != 0) {
		PRINT_ERROR("Can't add devt %s to sysfs", devt->name);
		goto out;
	}

out:
	TRACE_EXIT_RES(retval);
	return retval;
}

void scst_cleanup_devt_sysfs(struct scst_dev_type *devt)
{
	TRACE_ENTRY();

	if (devt->devt_kobj_initialized) {
		int rc;

		kobject_del(&devt->devt_kobj);
		kobject_put(&devt->devt_kobj);

		rc = wait_for_completion_timeout(&devt->devt_kobj_release_compl, HZ);
		if (rc == 0) {
			PRINT_INFO("Waiting for releasing sysfs entry "
				"for dev handler template %s...", devt->name);
			wait_for_completion(&devt->devt_kobj_release_compl);
			PRINT_INFO("Done waiting for releasing sysfs entry "
				"for dev handler template %s", devt->name);
		}
	}

	TRACE_EXIT();
	return;
}

int __init scst_sysfs_init(void)
{
	int retval = 0;

	TRACE_ENTRY();

	retval = kobject_init_and_add(&scst_sysfs_root_kobj,
			&scst_sysfs_root_ktype, kernel_kobj, "%s", "scst_tgt");
	if (retval != 0)
		goto sysfs_root_add_error;

	scst_targets_kobj = kobject_create_and_add("targets",
				&scst_sysfs_root_kobj);
	if (scst_targets_kobj == NULL)
		goto targets_kobj_error;

	scst_devices_kobj = kobject_create_and_add("devices",
				&scst_sysfs_root_kobj);
	if (scst_devices_kobj == NULL)
		goto devices_kobj_error;

	scst_sgv_kobj = kzalloc(sizeof(*scst_sgv_kobj), GFP_KERNEL);
	if (scst_sgv_kobj == NULL)
		goto sgv_kobj_error;

	retval = kobject_init_and_add(scst_sgv_kobj, &sgv_ktype,
			&scst_sysfs_root_kobj, "%s", "sgv");
	if (retval != 0)
		goto sgv_kobj_add_error;

	scst_back_drivers_kobj = kobject_create_and_add("back_drivers",
					&scst_sysfs_root_kobj);
	if (scst_back_drivers_kobj == NULL)
		goto back_drivers_kobj_error;


out:
	TRACE_EXIT_RES(retval);
	return retval;

back_drivers_kobj_error:
	kobject_del(scst_sgv_kobj);

sgv_kobj_add_error:
	kobject_put(scst_sgv_kobj);

sgv_kobj_error:
	kobject_del(scst_devices_kobj);
	kobject_put(scst_devices_kobj);

devices_kobj_error:
	kobject_del(scst_targets_kobj);
	kobject_put(scst_targets_kobj);

targets_kobj_error:
	kobject_del(&scst_sysfs_root_kobj);

sysfs_root_add_error:
	kobject_put(&scst_sysfs_root_kobj);

	if (retval == 0)
		retval = -EINVAL;
	goto out;
}

void __exit scst_sysfs_cleanup(void)
{
	TRACE_ENTRY();

	PRINT_INFO("%s", "Exiting SCST sysfs hierarchy...");

	kobject_del(scst_sgv_kobj);
	kobject_put(scst_sgv_kobj);

	kobject_del(scst_devices_kobj);
	kobject_put(scst_devices_kobj);

	kobject_del(scst_targets_kobj);
	kobject_put(scst_targets_kobj);

	kobject_del(scst_back_drivers_kobj);
	kobject_put(scst_back_drivers_kobj);

	kobject_del(&scst_sysfs_root_kobj);
	kobject_put(&scst_sysfs_root_kobj);

	wait_for_completion(&scst_sysfs_root_release_completion);

	PRINT_INFO("%s", "Exiting SCST sysfs hierarchy done");

	TRACE_EXIT();
	return;
}
