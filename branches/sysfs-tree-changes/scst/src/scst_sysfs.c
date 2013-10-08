/*
 *  scst_sysfs.c
 *
 *  Copyright (C) 2009 Daniel Henrique Debonzi <debonzi@linux.vnet.ibm.com>
 *  Copyright (C) 2009 - 2011 Vladislav Bolkhovitin <vst@vlnb.net>
 *  Copyright (C) 2009 - 2010 ID7 Ltd.
 *  Copyright (C) 2010 Bart Van Assche <bvanassche@acm.org>
 *  Copyright (C) 2010 - 2011 SCST Ltd.
 *
 *  This program is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License
 *  as published by the Free Software Foundation, version 2
 *  of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *  GNU General Public License for more details.
 *
 *  Locking strategy:
 *  - Only suspend activity or lock scst_mutex inside .show() or
 *    .store() callback function associated with attributes
 *    registered by scst_sysfs_init(). Never suspend activity or lock
 *    scst_mutex inside sysfs callback functions invoked for
 *    dynamically created sysfs attributes.
 *  - Dynamic kobject creation and deletion may happen while activity
 *    is suspended and/or scst_mutex is locked. It is even necessary
 *    to do that under lock to avoid races between kernel object
 *    creation and deletion/recreation of the same kernel object.
 *
 *  The above scheme avoids locking inversion between the s_active
 *  locking object associated by sysfs with each kernel object and
 *  activity suspending and/or scst_mutex.
 */

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/sysfs.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/ctype.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#ifdef INSIDE_KERNEL_TREE
#include <scst/scst.h>
#else
#include "scst.h"
#endif
#include "scst_debugfs.h"
#include "scst_lat_stats.h"
#include "scst_mem.h"
#include "scst_priv.h"
#include "scst_tracing.h"

enum mgmt_path_type {
	PATH_NOT_RECOGNIZED,
	DEVICE_PATH,
	DEVICE_TYPE_PATH,
	TARGET_TEMPLATE_PATH,
	TARGET_PATH,
	TARGET_LUNS_PATH,
	TARGET_INI_GROUPS_PATH,
	ACG_PATH,
	ACG_LUNS_PATH,
	ACG_INITIATOR_GROUPS_PATH,
	DGS_PATH,
	DGS_DEVS_PATH,
	TGS_PATH,
	TGS_TG_PATH,
};

static struct bus_type scst_target_bus;
static struct bus_type scst_device_bus;
static struct kobject *scst_device_groups_kobj;

static const char *const scst_dev_handler_types[] = {
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

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34)
/**
 ** Backported sysfs functions.
 **/

static int sysfs_create_files(struct kobject *kobj,
			      const struct attribute **ptr)
{
	int err = 0;
	int i;

	for (i = 0; ptr[i] && !err; i++)
		err = sysfs_create_file(kobj, ptr[i]);
	if (err)
		while (--i >= 0)
			sysfs_remove_file(kobj, ptr[i]);
	return err;
}

#if 0
static void sysfs_remove_files(struct kobject *kobj,
			       const struct attribute **ptr)
{
	int i;

	for (i = 0; ptr[i]; i++)
		sysfs_remove_file(kobj, ptr[i]);
}
#endif

/* Backported from linux/device.h. */
static inline void device_lock(struct device *dev)
{
	down(&dev->sem);
}

static inline void device_unlock(struct device *dev)
{
	up(&dev->sem);
}
#endif

static int device_create_files(struct device *dev,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34)
			       struct device_attribute **ptr)
#else
			       const struct device_attribute **ptr)
#endif
{
	int err = 0;
	int i;

	for (i = 0; ptr[i] && !err; i++)
		err = device_create_file(dev, ptr[i]);
	if (err)
		while (--i >= 0)
			device_remove_file(dev, ptr[i]);
	return err;
}

static void device_remove_files(struct device *dev,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34)
				struct device_attribute **ptr)
#else
				const struct device_attribute **ptr)
#endif
{
	int i;

	for (i = 0; ptr[i]; i++)
		device_remove_file(dev, ptr[i]);
}

static int driver_create_files(struct device_driver *drv,
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34)
			       struct driver_attribute **ptr)
#else
			       const struct driver_attribute **ptr)
#endif
{
	int err = 0;
	int i;

	for (i = 0; ptr[i] && !err; i++)
		err = driver_create_file(drv, ptr[i]);
	if (err)
		while (--i >= 0)
			driver_remove_file(drv, ptr[i]);
	return err;
}

/**
 ** Regular SCST sysfs ops
 **/
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
	if (kobj_attr->store)
		return kobj_attr->store(kobj, kobj_attr, buf, count);
	else
		return -EIO;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34))
const struct sysfs_ops scst_sysfs_ops = {
#else
struct sysfs_ops scst_sysfs_ops = {
#endif
	.show = scst_show,
	.store = scst_store,
};

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34))
const struct sysfs_ops *scst_sysfs_get_sysfs_ops(void)
#else
struct sysfs_ops *scst_sysfs_get_sysfs_ops(void)
#endif
{
	return &scst_sysfs_ops;
}
EXPORT_SYMBOL_GPL(scst_sysfs_get_sysfs_ops);

/**
 ** Lookup functions.
 **/

static struct scst_dev_type *__scst_lookup_devt(const char *name)
{
	struct scst_dev_type *dt;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
	lockdep_assert_held(&scst_mutex);
#endif

	list_for_each_entry(dt, &scst_virtual_dev_type_list,
			    dev_type_list_entry)
		if (strcmp(dt->name, name) == 0)
			return dt;
	list_for_each_entry(dt, &scst_dev_type_list, dev_type_list_entry)
		if (strcmp(dt->name, name) == 0)
			return dt;

	TRACE_DBG("devt %s not found", name);

	return NULL;
}

static struct scst_device *__scst_lookup_dev(const char *name)
{
	struct scst_device *d;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
	lockdep_assert_held(&scst_mutex);
#endif

	list_for_each_entry(d, &scst_dev_list, dev_list_entry)
		if (strcmp(d->virt_name, name) == 0)
			return d;

	TRACE_DBG("dev %s not found", name);

	return NULL;
}

static struct scst_tgt_template *__scst_lookup_tgtt(const char *name)
{
	struct scst_tgt_template *tt;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
	lockdep_assert_held(&scst_mutex);
#endif

	list_for_each_entry(tt, &scst_template_list, scst_template_list_entry)
		if (strcmp(tt->name, name) == 0)
			return tt;

	TRACE_DBG("tgtt %s not found", name);

	return NULL;
}

static struct scst_acg *__scst_lookup_acg(const struct scst_tgt *tgt,
					  const char *acg_name)
{
	struct scst_acg *acg;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
	lockdep_assert_held(&scst_mutex);
#endif

	acg = tgt->default_acg;
	if (acg && strcmp(acg->acg_name, acg_name) == 0)
		return acg;

	list_for_each_entry(acg, &tgt->tgt_acg_list, acg_list_entry)
		if (strcmp(acg->acg_name, acg_name) == 0)
			return acg;

	TRACE_DBG("acg %s not found", acg_name);

	return NULL;
}

/**
 ** Target Template
 **/

/**
 * scst_tgtt_add_target_show() - Whether the add_target method is supported.
 */
static ssize_t scst_tgtt_add_target_show(struct device_driver *drv, char *buf)
{
	struct scst_tgt_template *tgtt;

	tgtt = scst_drv_to_tgtt(drv);
	return scnprintf(buf, PAGE_SIZE, "%d\n", tgtt->add_target ? 1 : 0);
}

static struct driver_attribute scst_tgtt_add_target_attr =
	__ATTR(add_target, S_IRUGO,
	       scst_tgtt_add_target_show, NULL);

static ssize_t scst_tgtt_add_target_parameters_show(struct device_driver *drv,
						    char *buf)
{
	struct scst_tgt_template *tgtt;
	const char *const *p;
	ssize_t res;

	tgtt = scst_drv_to_tgtt(drv);
	res = 0;
	for (p = tgtt->add_target_parameters; p && *p; p++)
		res += scnprintf(buf + res, PAGE_SIZE - res, "%s\n", *p);
	return res;
}

static struct driver_attribute scst_tgtt_add_target_parameters_attr =
	__ATTR(add_target_parameters, S_IRUGO,
	       scst_tgtt_add_target_parameters_show, NULL);

static ssize_t scst_tgtt_tgtt_attributes_show(struct device_driver *drv,
					      char *buf)
{
	struct scst_tgt_template *tgtt;
	const char *const *p;
	ssize_t res;

	tgtt = scst_drv_to_tgtt(drv);
	res = 0;
	for (p = tgtt->tgtt_optional_attributes; p && *p; p++)
		res += scnprintf(buf + res, PAGE_SIZE - res, "%s\n", *p);
	return res;
}

static struct driver_attribute scst_tgtt_tgtt_attributes_attr =
	__ATTR(driver_attributes, S_IRUGO,
	       scst_tgtt_tgtt_attributes_show, NULL);

static ssize_t scst_tgtt_tgt_attributes_show(struct device_driver *drv,
					     char *buf)
{
	struct scst_tgt_template *tgtt;
	const char *const *p;
	ssize_t res;

	tgtt = scst_drv_to_tgtt(drv);
	res = 0;
	for (p = tgtt->tgt_optional_attributes; p && *p; p++)
		res += scnprintf(buf + res, PAGE_SIZE - res, "%s\n", *p);
	return res;
}

static struct driver_attribute scst_tgtt_tgt_attributes_attr =
	__ATTR(target_attributes, S_IRUGO,
	       scst_tgtt_tgt_attributes_show, NULL);

static int scst_process_tgtt_mgmt_store(char *buffer,
					struct scst_tgt_template *tgtt)
{
	int res = 0;
	char *p, *pp, *target_name;

	TRACE_ENTRY();

	TRACE_DBG("buffer %s", buffer);

	pp = buffer;
	if (pp[strlen(pp) - 1] == '\n')
		pp[strlen(pp) - 1] = '\0';

	p = scst_get_next_lexem(&pp);

	if (strcasecmp("add_target", p) == 0) {
		target_name = scst_get_next_lexem(&pp);
		if (*target_name == '\0') {
			PRINT_ERROR("%s", "Target name required");
			res = -EINVAL;
			goto out;
		}
		res = tgtt->add_target(target_name, pp);
	} else if (strcasecmp("del_target", p) == 0) {
		target_name = scst_get_next_lexem(&pp);
		if (*target_name == '\0') {
			PRINT_ERROR("%s", "Target name required");
			res = -EINVAL;
			goto out;
		}

		p = scst_get_next_lexem(&pp);
		if (*p != '\0')
			goto out_syntax_err;

		res = tgtt->del_target(target_name);
	} else if (tgtt->mgmt_cmd != NULL) {
		scst_restore_token_str(p, pp);
		res = tgtt->mgmt_cmd(buffer);
	} else {
		PRINT_ERROR("Unknown action \"%s\"", p);
		res = -EINVAL;
		goto out;
	}

out:
	TRACE_EXIT_RES(res);
	return res;

out_syntax_err:
	PRINT_ERROR("Syntax error on \"%s\"", p);
	res = -EINVAL;
	goto out;
}

int scst_tgtt_sysfs_init(struct scst_tgt_template *tgtt)
{
	int res;

	TRACE_ENTRY();

	WARN_ON(!tgtt->owner);

	tgtt->tgtt_drv.bus  = &scst_target_bus;
	tgtt->tgtt_drv.name = tgtt->name;
	tgtt->tgtt_drv.owner = tgtt->owner;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	tgtt->tgtt_drv.suppress_bind_attrs = true;
#endif
	res = driver_register(&tgtt->tgtt_drv);

	TRACE_EXIT_RES(res);
	return res;
}

int scst_tgtt_sysfs_create(struct scst_tgt_template *tgtt)
{
	int res;

	TRACE_ENTRY();

	res = driver_create_file(scst_sysfs_get_tgtt_drv(tgtt),
				 &scst_tgtt_add_target_attr);
	if (res != 0) {
		PRINT_ERROR("Can't add attribute %s for target driver %s",
			    scst_tgtt_add_target_attr.attr.name,
			    tgtt->name);
		goto out_del;
	}

	if (tgtt->add_target_parameters) {
		res = driver_create_file(scst_sysfs_get_tgtt_drv(tgtt),
					 &scst_tgtt_add_target_parameters_attr);
		if (res != 0) {
			PRINT_ERROR("Can't add attribute %s for target driver"
				" %s",
				scst_tgtt_add_target_parameters_attr.attr.name,
				tgtt->name);
			goto out_del;
		}
	}

	if (tgtt->tgtt_optional_attributes) {
		res = driver_create_file(scst_sysfs_get_tgtt_drv(tgtt),
					 &scst_tgtt_tgtt_attributes_attr);
		if (res) {
			PRINT_ERROR("Can't add attribute %s for target driver"
				" %s",
				scst_tgtt_tgtt_attributes_attr.attr.name,
				tgtt->name);
			goto out_del;
		}
	}

	if (tgtt->tgt_optional_attributes) {
		res = driver_create_file(scst_sysfs_get_tgtt_drv(tgtt),
					 &scst_tgtt_tgt_attributes_attr);
		if (res) {
			PRINT_ERROR("Can't add attribute %s for target driver"
				" %s",
				scst_tgtt_tgt_attributes_attr.attr.name,
				tgtt->name);
			goto out_del;
		}
	}

	if (tgtt->tgtt_attrs) {
		res = driver_create_files(scst_sysfs_get_tgtt_drv(tgtt),
					  tgtt->tgtt_attrs);
		if (res != 0) {
			PRINT_ERROR("Can't add attributes for target driver %s",
				    tgtt->name);
			goto out_del;
		}
	}

	res = scst_tgtt_create_debugfs_dir(tgtt);
	if (res) {
		PRINT_ERROR("Can't create tracing files for target driver %s",
			    tgtt->name);
		goto out_del;
	}

	res = scst_tgtt_create_debugfs_files(tgtt);
	if (res)
		goto out_del;

out:
	TRACE_EXIT_RES(res);
	return res;

out_del:
	scst_tgtt_sysfs_del(tgtt);
	goto out;
}

void scst_tgtt_sysfs_del(struct scst_tgt_template *tgtt)
{
	TRACE_ENTRY();
	scst_tgtt_remove_debugfs_files(tgtt);
	scst_tgtt_remove_debugfs_dir(tgtt);
	driver_unregister(&tgtt->tgtt_drv);
	TRACE_EXIT();
}

void scst_tgtt_sysfs_put(struct scst_tgt_template *tgtt)
{
	TRACE_ENTRY();
	TRACE_EXIT();
}

/**
 ** Target directory implementation
 **/

static ssize_t scst_lun_parameters_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf)
{
	return sprintf(buf, "%s", "read_only\n");
}

static struct kobj_attribute scst_lun_parameters =
	__ATTR(parameters, S_IRUGO, scst_lun_parameters_show, NULL);

static ssize_t __scst_acg_addr_method_show(struct scst_acg *acg, char *buf)
{
	int res;

	switch (acg->addr_method) {
	case SCST_LUN_ADDR_METHOD_FLAT:
		res = sprintf(buf, "FLAT\n");
		break;
	case SCST_LUN_ADDR_METHOD_PERIPHERAL:
		res = sprintf(buf, "PERIPHERAL\n");
		break;
	case SCST_LUN_ADDR_METHOD_LUN:
		res = sprintf(buf, "LUN\n");
		break;
	default:
		res = sprintf(buf, "UNKNOWN\n");
		break;
	}

	return res;
}

static ssize_t __scst_acg_addr_method_store(struct scst_acg *acg,
	const char *buf, size_t count)
{
	int res = count;

	if (strncasecmp(buf, "FLAT", min_t(int, 4, count)) == 0)
		acg->addr_method = SCST_LUN_ADDR_METHOD_FLAT;
	else if (strncasecmp(buf, "PERIPHERAL", min_t(int, 10, count)) == 0)
		acg->addr_method = SCST_LUN_ADDR_METHOD_PERIPHERAL;
	else if (strncasecmp(buf, "LUN", min_t(int, 3, count)) == 0)
		acg->addr_method = SCST_LUN_ADDR_METHOD_LUN;
	else {
		PRINT_ERROR("Unknown address method %s", buf);
		res = -EINVAL;
	}

	TRACE_DBG("acg %p, addr_method %d", acg, acg->addr_method);

	return res;
}

static ssize_t scst_tgt_addr_method_show(struct device *device,
				struct device_attribute *attr, char *buf)
{
	struct scst_acg *acg;
	struct scst_tgt *tgt;

	tgt = scst_dev_to_tgt(device);

	acg = tgt->default_acg;

	return __scst_acg_addr_method_show(acg, buf);
}

static ssize_t scst_tgt_addr_method_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int res;
	struct scst_acg *acg;
	struct scst_tgt *tgt;

	tgt = scst_dev_to_tgt(device);

	acg = tgt->default_acg;

	res = __scst_acg_addr_method_store(acg, buf, count);

	TRACE_EXIT_RES(res);
	return res;
}

static struct device_attribute scst_tgt_addr_method =
	__ATTR(addr_method, S_IRUGO | S_IWUSR, scst_tgt_addr_method_show,
	       scst_tgt_addr_method_store);

static ssize_t __scst_acg_io_grouping_type_show(struct scst_acg *acg, char *buf)
{
	int res;

	switch (acg->acg_io_grouping_type) {
	case SCST_IO_GROUPING_AUTO:
		res = sprintf(buf, "%s\n", SCST_IO_GROUPING_AUTO_STR);
		break;
	case SCST_IO_GROUPING_THIS_GROUP_ONLY:
		res = sprintf(buf, "%s\n",
			SCST_IO_GROUPING_THIS_GROUP_ONLY_STR);
		break;
	case SCST_IO_GROUPING_NEVER:
		res = sprintf(buf, "%s\n", SCST_IO_GROUPING_NEVER_STR);
		break;
	default:
		res = sprintf(buf, "%d\n", acg->acg_io_grouping_type);
		break;
	}

	return res;
}

static int __scst_acg_process_io_grouping_type_store(struct scst_tgt *tgt,
	struct scst_acg *acg, int io_grouping_type)
{
	int res;
	struct scst_acg_dev *acg_dev;

	scst_assert_activity_suspended();

	TRACE_DBG("tgt %p, acg %p, io_grouping_type %d", tgt, acg,
		io_grouping_type);

	res = mutex_lock_interruptible(&scst_mutex);
	if (res != 0)
		goto out;

	acg->acg_io_grouping_type = io_grouping_type;

	list_for_each_entry(acg_dev, &acg->acg_dev_list, acg_dev_list_entry) {
		int rc;

		scst_stop_dev_threads(acg_dev->dev);

		rc = scst_create_dev_threads(acg_dev->dev);
		if (rc != 0)
			res = rc;
	}

	mutex_unlock(&scst_mutex);

out:
	return res;
}

static ssize_t __scst_acg_io_grouping_type_store(struct scst_acg *acg,
	const char *buf, size_t count)
{
	int res = 0;
	int prev = acg->acg_io_grouping_type;
	long io_grouping_type;

	if (strncasecmp(buf, SCST_IO_GROUPING_AUTO_STR,
			min_t(int, strlen(SCST_IO_GROUPING_AUTO_STR), count)) == 0)
		io_grouping_type = SCST_IO_GROUPING_AUTO;
	else if (strncasecmp(buf, SCST_IO_GROUPING_THIS_GROUP_ONLY_STR,
			min_t(int, strlen(SCST_IO_GROUPING_THIS_GROUP_ONLY_STR), count)) == 0)
		io_grouping_type = SCST_IO_GROUPING_THIS_GROUP_ONLY;
	else if (strncasecmp(buf, SCST_IO_GROUPING_NEVER_STR,
			min_t(int, strlen(SCST_IO_GROUPING_NEVER_STR), count)) == 0)
		io_grouping_type = SCST_IO_GROUPING_NEVER;
	else {
		res = strict_strtol(buf, 0, &io_grouping_type);
		if ((res != 0) || (io_grouping_type <= 0)) {
			PRINT_ERROR("Unknown or not allowed I/O grouping type "
				"%s", buf);
			res = -EINVAL;
			goto out;
		}
	}

	if (prev == io_grouping_type)
		goto out;

	res = __scst_acg_process_io_grouping_type_store(acg->tgt, acg,
							io_grouping_type);
out:
	return res;
}

static ssize_t scst_tgt_io_grouping_type_show(struct device *device,
	struct device_attribute *attr, char *buf)
{
	struct scst_acg *acg;
	struct scst_tgt *tgt;

	tgt = scst_dev_to_tgt(device);

	acg = tgt->default_acg;

	return __scst_acg_io_grouping_type_show(acg, buf);
}

static ssize_t scst_tgt_io_grouping_type_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int res;
	struct scst_acg *acg;
	struct scst_tgt *tgt;

	tgt = scst_dev_to_tgt(device);

	acg = tgt->default_acg;

	res = __scst_acg_io_grouping_type_store(acg, buf, count);
	if (res != 0)
		goto out;

	res = count;

out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct device_attribute scst_tgt_io_grouping_type =
	__ATTR(io_grouping_type, S_IRUGO | S_IWUSR,
	       scst_tgt_io_grouping_type_show,
	       scst_tgt_io_grouping_type_store);

static ssize_t __scst_acg_cpu_mask_show(struct scst_acg *acg, char *buf)
{
	int res;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
	res = cpumask_scnprintf(buf, PAGE_SIZE, acg->acg_cpu_mask);
#else
	res = cpumask_scnprintf(buf, PAGE_SIZE, &acg->acg_cpu_mask);
#endif
	res += scnprintf(buf + res, PAGE_SIZE - res, "\n");

	return res;
}

static int __scst_acg_process_cpu_mask_store(struct scst_tgt *tgt,
	struct scst_acg *acg, cpumask_t *cpu_mask)
{
	int res;
	struct scst_session *sess;

	TRACE_DBG("tgt %p, acg %p", tgt, acg);

	res = mutex_lock_interruptible(&scst_mutex);
	if (res != 0)
		goto out;

	cpumask_copy(&acg->acg_cpu_mask, cpu_mask);

	list_for_each_entry(sess, &acg->acg_sess_list, acg_sess_list_entry) {
		int i;
		for (i = 0; i < SESS_TGT_DEV_LIST_HASH_SIZE; i++) {
			struct scst_tgt_dev *tgt_dev;
			struct list_head *head = &sess->sess_tgt_dev_list[i];
			list_for_each_entry(tgt_dev, head,
						sess_tgt_dev_list_entry) {
				struct scst_cmd_thread_t *thr;
				if (tgt_dev->active_cmd_threads != &tgt_dev->tgt_dev_cmd_threads)
					continue;
				list_for_each_entry(thr,
						&tgt_dev->active_cmd_threads->threads_list,
						thread_list_entry) {
					int rc;
#ifdef RHEL_MAJOR
					rc = set_cpus_allowed(thr->cmd_thread, *cpu_mask);
#else
					rc = set_cpus_allowed_ptr(thr->cmd_thread, cpu_mask);
#endif
					if (rc != 0)
						PRINT_ERROR("Setting CPU "
							"affinity failed: %d", rc);
				}
			}
		}
		if (tgt->tgtt->report_aen != NULL) {
			struct scst_aen *aen;
			int rc;

			aen = scst_alloc_aen(sess, 0);
			if (aen == NULL) {
				PRINT_ERROR("Unable to notify target driver %s "
					"about cpu_mask change", tgt->tgt_name);
				continue;
			}

			aen->event_fn = SCST_AEN_CPU_MASK_CHANGED;

			TRACE_DBG("Calling target's %s report_aen(%p)",
				tgt->tgtt->name, aen);
			rc = tgt->tgtt->report_aen(aen);
			TRACE_DBG("Target's %s report_aen(%p) returned %d",
				tgt->tgtt->name, aen, rc);
			if (rc != SCST_AEN_RES_SUCCESS)
				scst_free_aen(aen);
		}
	}

	mutex_unlock(&scst_mutex);

out:
	return res;
}

static ssize_t scst_tgt_cpu_mask_show(struct device *device,
	struct device_attribute *attr, char *buf)
{
	struct scst_acg *acg;
	struct scst_tgt *tgt;

	tgt = scst_dev_to_tgt(device);

	acg = tgt->default_acg;

	return __scst_acg_cpu_mask_show(acg, buf);
}

static struct device_attribute scst_tgt_cpu_mask =
	__ATTR(cpu_mask, S_IRUGO, scst_tgt_cpu_mask_show, NULL);

static ssize_t scst_tgt_enable_show(struct device *device,
				    struct device_attribute *attr, char *buf)
{
	struct scst_tgt *tgt;
	int res;
	bool enabled;

	TRACE_ENTRY();

	tgt = scst_dev_to_tgt(device);

	enabled = tgt->tgtt->is_target_enabled(tgt);

	res = sprintf(buf, "%d\n", enabled ? 1 : 0);

	TRACE_EXIT_RES(res);
	return res;
}

static int scst_process_tgt_enable_store(struct scst_tgt *tgt, bool enable)
{
	int res;

	TRACE_ENTRY();

	/* Tgt protected by kobject reference */

	TRACE_DBG("tgt %s, enable %d", tgt->tgt_name, enable);

	if (enable) {
		if (tgt->rel_tgt_id == 0) {
			res = gen_relative_target_port_id(&tgt->rel_tgt_id);
			if (res != 0)
				goto out;
			PRINT_INFO("Using autogenerated rel ID %d for target "
				"%s", tgt->rel_tgt_id, tgt->tgt_name);
		} else {
			if (!scst_is_relative_target_port_id_unique(
					    tgt->rel_tgt_id, tgt)) {
				PRINT_ERROR("Relative port id %d is not unique",
					tgt->rel_tgt_id);
				res = -EBADSLT;
				goto out;
			}
		}
	}

	res = tgt->tgtt->enable_target(tgt, enable);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct device_attribute tgt_enable_attr =
	__ATTR(enabled, S_IRUGO, scst_tgt_enable_show, NULL);

static ssize_t scst_rel_tgt_id_show(struct device *device,
				    struct device_attribute *attr, char *buf)
{
	struct scst_tgt *tgt;
	int res;

	TRACE_ENTRY();

	tgt = scst_dev_to_tgt(device);

	res = sprintf(buf, "%d\n", tgt->rel_tgt_id);

	TRACE_EXIT_RES(res);
	return res;
}

static int scst_process_rel_tgt_id_store(struct scst_tgt *tgt,
					 unsigned long rel_tgt_id)
{
	int res = 0;

	TRACE_ENTRY();

	/* tgt protected by kobject_get() */

	TRACE_DBG("Trying to set relative target port id %d",
		(uint16_t)rel_tgt_id);

	if (tgt->tgtt->is_target_enabled(tgt) &&
	    rel_tgt_id != tgt->rel_tgt_id) {
		if (!scst_is_relative_target_port_id_unique(rel_tgt_id, tgt)) {
			PRINT_ERROR("Relative port id %d is not unique",
				(uint16_t)rel_tgt_id);
			res = -EBADSLT;
			goto out;
		}
	}

	if (rel_tgt_id < SCST_MIN_REL_TGT_ID ||
	    rel_tgt_id > SCST_MAX_REL_TGT_ID) {
		if ((rel_tgt_id == 0) && !tgt->tgtt->is_target_enabled(tgt))
			goto set;

		PRINT_ERROR("Invalid relative port id %d",
			(uint16_t)rel_tgt_id);
		res = -EINVAL;
		goto out;
	}

set:
	tgt->rel_tgt_id = (uint16_t)rel_tgt_id;
out:
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_rel_tgt_id_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int res;
	struct scst_tgt *tgt;
	unsigned long rel_tgt_id;

	TRACE_ENTRY();

	BUG_ON(!buf);

	tgt = scst_dev_to_tgt(device);

	res = strict_strtoul(buf, 0, &rel_tgt_id);
	if (res != 0) {
		PRINT_ERROR("%s", "Wrong rel_tgt_id");
		res = -EINVAL;
		goto out;
	}

	res = scst_process_rel_tgt_id_store(tgt, rel_tgt_id);
	if (res == 0)
		res = count;

out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct device_attribute scst_rel_tgt_id =
	__ATTR(rel_tgt_id, S_IRUGO | S_IWUSR, scst_rel_tgt_id_show,
	       scst_rel_tgt_id_store);

static ssize_t scst_tgt_comment_show(struct device *device,
				     struct device_attribute *attr, char *buf)
{
	struct scst_tgt *tgt;
	int res;

	TRACE_ENTRY();

	tgt = scst_dev_to_tgt(device);

	res = tgt->tgt_comment ? sprintf(buf, "%s\n", tgt->tgt_comment) : 0;

	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_tgt_comment_store(struct device *device,
		struct device_attribute *attr, const char *buf, size_t count)
{
	int res;
	struct scst_tgt *tgt;
	char *p;
	int len;

	TRACE_ENTRY();

	tgt = scst_dev_to_tgt(device);

	len = buf ? strnlen(buf, count) : 0;
	if (len > 0 && buf[len - 1] == '\n')
		len--;

	p = NULL;
	if (len == 0)
		goto swap;

	res = -ENOMEM;
	p = kasprintf(GFP_KERNEL, "%.*s", len, buf);
	if (!p) {
		PRINT_ERROR("Unable to alloc tgt_comment string (len %d)",
			len+1);
		goto out;
	}

swap:
	kfree(tgt->tgt_comment);
	tgt->tgt_comment = p;
	res = count;

out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct device_attribute scst_tgt_comment =
	__ATTR(comment, S_IRUGO | S_IWUSR, scst_tgt_comment_show,
	       scst_tgt_comment_store);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34)
static struct device_attribute *scst_tgt_attr[] = {
#else
static const struct device_attribute *scst_tgt_attr[] = {
#endif
	&scst_rel_tgt_id,
	&scst_tgt_comment,
	&scst_tgt_addr_method,
	&scst_tgt_io_grouping_type,
	&scst_tgt_cpu_mask,
	NULL
};

static int scst_alloc_and_parse_cpumask(cpumask_t **cpumask, const char *buf,
					size_t count)
{
	int res;

	res = -ENOMEM;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 28)
	*cpumask = kmalloc(sizeof(**cpumask), GFP_KERNEL);
#else
	*cpumask = kmalloc(cpumask_size(), GFP_KERNEL);
#endif
	if (!*cpumask)
		goto out;
	/*
	 * We can't use cpumask_parse_user() here because it expects
	 * a buffer in user space.
	 */
	res = bitmap_parse(buf, count, cpumask_bits(*cpumask), nr_cpumask_bits);
	if (res)
		goto out_free;
out:
	return res;
out_free:
	kfree(*cpumask);
	*cpumask = NULL;
	goto out;
}

static int scst_process_tgt_mgmt_store(char *cmd, struct scst_tgt *tgt)
{
	int res;

	TRACE_ENTRY();

	res = -EINVAL;
	if (strcmp(cmd, "enable") == 0)
		res = scst_process_tgt_enable_store(tgt, true);
	else if (strcmp(cmd, "disable") == 0)
		res = scst_process_tgt_enable_store(tgt, false);
	else if (strncmp(cmd, "set_cpu_mask ", 13) == 0) {
		cpumask_t *cpumask;

		BUG_ON(!tgt->default_acg);

		res = scst_alloc_and_parse_cpumask(&cpumask, cmd + 13,
						   strlen(cmd + 13));
		if (res)
			goto out;
		res = __scst_acg_process_cpu_mask_store(tgt, tgt->default_acg,
							cpumask);
		kfree(cpumask);
	}
out:
	TRACE_EXIT_RES(res);
	return res;
}

static void scst_release_target(struct device *dev)
{
	struct scst_tgt *tgt;

	TRACE_ENTRY();
	tgt = scst_dev_to_tgt(dev);

	PRINT_INFO("Target %s for template %s unregistered successfully",
		   tgt->tgt_name, tgt->tgtt->name);

	scst_free_tgt(tgt);
	TRACE_EXIT();
}

int scst_tgt_sysfs_init(struct scst_tgt *tgt)
{
	int res;

	TRACE_ENTRY();

	tgt->tgt_dev.bus     = &scst_target_bus;
	tgt->tgt_dev.release = scst_release_target;
	tgt->tgt_dev.driver  = &tgt->tgtt->tgtt_drv;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	snprintf(tgt->tgt_dev.bus_id, BUS_ID_SIZE, "%s", tgt->tgt_name);
#else
	dev_set_name(&tgt->tgt_dev, "%s", tgt->tgt_name);
#endif
	res = device_register(&tgt->tgt_dev);

	TRACE_EXIT_RES(res);
	return res;
}

int scst_tgt_sysfs_create(struct scst_tgt *tgt)
{
	int res;

	TRACE_ENTRY();

	if (tgt->tgtt->enable_target && tgt->tgtt->is_target_enabled) {
		res = device_create_file(scst_sysfs_get_tgt_dev(tgt),
					 &tgt_enable_attr);
		if (res != 0) {
			PRINT_ERROR("Can't add attr %s to sysfs",
				    tgt_enable_attr.attr.name);
			goto out_err;
		}
	}

	tgt->tgt_sess_kobj = kobject_create_and_add("sessions",
					scst_sysfs_get_tgt_kobj(tgt));
	if (tgt->tgt_sess_kobj == NULL) {
		PRINT_ERROR("Can't create sess kobj for tgt %s", tgt->tgt_name);
		goto out_nomem;
	}

	tgt->tgt_luns_kobj = kobject_create_and_add("luns",
					scst_sysfs_get_tgt_kobj(tgt));
	if (tgt->tgt_luns_kobj == NULL) {
		PRINT_ERROR("Can't create luns kobj for tgt %s", tgt->tgt_name);
		goto out_nomem;
	}

	res = sysfs_create_file(tgt->tgt_luns_kobj, &scst_lun_parameters.attr);
	if (res != 0) {
		PRINT_ERROR("Can't add attribute %s for tgt %s",
			    scst_lun_parameters.attr.name, tgt->tgt_name);
		goto out_err;
	}

	tgt->tgt_ini_grp_kobj = kobject_create_and_add("ini_groups",
					scst_sysfs_get_tgt_kobj(tgt));
	if (tgt->tgt_ini_grp_kobj == NULL) {
		PRINT_ERROR("Can't create ini_grp kobj for tgt %s",
			tgt->tgt_name);
		goto out_nomem;
	}

	res = device_create_files(scst_sysfs_get_tgt_dev(tgt), scst_tgt_attr);
	if (res != 0) {
		PRINT_ERROR("Can't add generic attributes for tgt %s",
			    tgt->tgt_name);
		goto out_err;
	}

	if (tgt->tgtt->tgt_attrs) {
		res = device_create_files(scst_sysfs_get_tgt_dev(tgt),
					  tgt->tgtt->tgt_attrs);
		if (res != 0) {
			PRINT_ERROR("Can't add attributes for tgt %s",
				    tgt->tgt_name);
			goto out_err;
		}
	}

	res = scst_tgt_create_debugfs_dir(tgt);
	if (res)
		goto out_err;

out:
	TRACE_EXIT_RES(res);
	return res;

out_nomem:
	res = -ENOMEM;

out_err:
	scst_tgt_sysfs_del(tgt);
	goto out;
}

void scst_tgt_sysfs_del(struct scst_tgt *tgt)
{
	TRACE_ENTRY();

	scst_tgt_remove_debugfs_dir(tgt);
	kobject_del(tgt->tgt_sess_kobj);
	kobject_del(tgt->tgt_luns_kobj);
	kobject_del(tgt->tgt_ini_grp_kobj);

	kobject_put(tgt->tgt_sess_kobj);
	kobject_put(tgt->tgt_luns_kobj);
	kobject_put(tgt->tgt_ini_grp_kobj);

	TRACE_EXIT();
}

void scst_tgt_sysfs_put(struct scst_tgt *tgt)
{
	TRACE_ENTRY();
	device_unregister(&tgt->tgt_dev);
	TRACE_EXIT();
}


/**
 ** Devices directory implementation
 **/

static ssize_t scst_dev_sysfs_type_show(struct device *device,
				struct device_attribute *attr, char *buf)
{
	struct scst_device *dev = scst_dev_to_dev(device);

	return scnprintf(buf, PAGE_SIZE, "%d\n", dev->type);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
static const struct device_attribute scst_dev_sysfs_type_attr =
#else
static struct device_attribute scst_dev_sysfs_type_attr =
#endif
	__ATTR(type, S_IRUGO, scst_dev_sysfs_type_show, NULL);

static ssize_t scst_dev_sysfs_type_description_show(struct device *device,
				struct device_attribute *attr, char *buf)
{
	struct scst_device *dev = scst_dev_to_dev(device);

	return scnprintf(buf, PAGE_SIZE, "%s\n",
		(unsigned)dev->type >= ARRAY_SIZE(scst_dev_handler_types) ?
		      "unknown" : scst_dev_handler_types[dev->type]);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
static const struct device_attribute scst_dev_sysfs_type_description_attr =
#else
static struct device_attribute scst_dev_sysfs_type_description_attr =
#endif
	__ATTR(type_description, S_IRUGO, scst_dev_sysfs_type_description_show,
	       NULL);

static int scst_process_dev_sysfs_threads_data_store(
	struct scst_device *dev, int threads_num,
	enum scst_dev_type_threads_pool_type threads_pool_type)
{
	int res;
	int oldtn = dev->threads_num;
	enum scst_dev_type_threads_pool_type oldtt = dev->threads_pool_type;

	TRACE_ENTRY();

	scst_assert_activity_suspended();

	TRACE_DBG("dev %p, threads_num %d, threads_pool_type %d", dev,
		threads_num, threads_pool_type);

	res = mutex_lock_interruptible(&scst_mutex);
	if (res != 0)
		goto out;

	scst_stop_dev_threads(dev);

	dev->threads_num = threads_num;
	dev->threads_pool_type = threads_pool_type;

	res = scst_create_dev_threads(dev);
	if (res != 0)
		goto out_unlock;

	if (oldtn != dev->threads_num)
		PRINT_INFO("Changed cmd threads num to %d", dev->threads_num);
	else if (oldtt != dev->threads_pool_type)
		PRINT_INFO("Changed cmd threads pool type to %d",
			dev->threads_pool_type);

out_unlock:
	mutex_unlock(&scst_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_dev_sysfs_check_threads_data(
	struct scst_device *dev, int threads_num,
	enum scst_dev_type_threads_pool_type threads_pool_type, bool *stop)
{
	int res = 0;

	TRACE_ENTRY();

	*stop = false;

	if (dev->threads_num < 0) {
		PRINT_ERROR("Threads pool disabled for device %s",
			dev->virt_name);
		res = -EPERM;
		goto out;
	}

	if ((threads_num == dev->threads_num) &&
	    (threads_pool_type == dev->threads_pool_type)) {
		*stop = true;
		goto out;
	}

out:
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_dev_sysfs_threads_num_show(struct device *device,
					struct device_attribute *attr, char *buf)
{
	int pos;
	struct scst_device *dev;

	TRACE_ENTRY();

	dev = scst_dev_to_dev(device);

	pos = sprintf(buf, "%d\n", dev->threads_num);

	TRACE_EXIT_RES(pos);
	return pos;
}

static ssize_t scst_dev_set_threads_num(struct scst_device *dev, long newtn)
{
	int res;
	bool stop;

	TRACE_ENTRY();

	res = scst_dev_sysfs_check_threads_data(dev, newtn,
						dev->threads_pool_type, &stop);
	if ((res != 0) || stop)
		goto out;

	res = scst_process_dev_sysfs_threads_data_store(dev, newtn,
							dev->threads_pool_type);
out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct device_attribute dev_threads_num_attr =
	__ATTR(threads_num, S_IRUGO, scst_dev_sysfs_threads_num_show, NULL);

static ssize_t scst_dev_sysfs_threads_pool_type_show(struct device *device,
	struct device_attribute *attr, char *buf)
{
	int pos;
	struct scst_device *dev;

	TRACE_ENTRY();

	dev = scst_dev_to_dev(device);

	if (dev->threads_num == 0) {
		pos = sprintf(buf, "Async\n");
		goto out;
	} else if (dev->threads_num < 0) {
		pos = sprintf(buf, "Not valid\n");
		goto out;
	}

	switch (dev->threads_pool_type) {
	case SCST_THREADS_POOL_PER_INITIATOR:
		pos = sprintf(buf, "%s\n", SCST_THREADS_POOL_PER_INITIATOR_STR);
		break;
	case SCST_THREADS_POOL_SHARED:
		pos = sprintf(buf, "%s\n", SCST_THREADS_POOL_SHARED_STR);
		break;
	default:
		pos = sprintf(buf, "Unknown\n");
		break;
	}

out:
	TRACE_EXIT_RES(pos);
	return pos;
}

static ssize_t scst_dev_set_thread_pool_type(struct scst_device *dev,
				enum scst_dev_type_threads_pool_type newtpt)
{
	int res;
	bool stop;

	TRACE_ENTRY();

	res = scst_dev_sysfs_check_threads_data(dev, dev->threads_num,
		newtpt, &stop);
	if ((res != 0) || stop)
		goto out;
	res = scst_process_dev_sysfs_threads_data_store(dev, dev->threads_num,
							newtpt);
out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct device_attribute dev_threads_pool_type_attr =
	__ATTR(threads_pool_type, S_IRUGO,
	       scst_dev_sysfs_threads_pool_type_show, NULL);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34)
static struct device_attribute *dev_thread_attr[] = {
#else
static const struct device_attribute *dev_thread_attr[] = {
#endif
	&dev_threads_num_attr,
	&dev_threads_pool_type_attr,
	NULL
};

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
static const struct device_attribute *scst_virt_dev_attrs[] = {
#else
static struct device_attribute *scst_virt_dev_attrs[] = {
#endif
	&scst_dev_sysfs_type_attr,
	&scst_dev_sysfs_type_description_attr,
	NULL
};

static ssize_t scst_dev_scsi_device_show(struct device *device,
				      struct device_attribute *attr, char *buf)
{
	struct scst_device *dev;
	struct scsi_device *scsidp;
	int res;

	dev = scst_dev_to_dev(device);
	res = -ENOENT;
	scsidp = dev->scsi_dev;
	if (!scsidp)
		goto out;
	res = scnprintf(buf, PAGE_SIZE, "%d:%d:%d:%d\n", scsidp->host->host_no,
			scsidp->channel, scsidp->id, scsidp->lun);
out:
	return res;
}

static struct device_attribute scst_dev_scsi_device_attr =
	__ATTR(scsi_device, S_IRUGO, scst_dev_scsi_device_show, NULL);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
static const struct device_attribute *scst_pt_dev_attrs[] = {
#else
static struct device_attribute *scst_pt_dev_attrs[] = {
#endif
	&scst_dev_scsi_device_attr,
	NULL
};

static int scst_process_dev_mgmt_store(char *cmd, struct scst_device *dev)
{
	int res;

	TRACE_ENTRY();

	res = -EINVAL;
	if (strncmp(cmd, "set_filename ", 13) == 0) {
		res = -EPERM;
		if (!dev->handler->set_filename)
			goto out;
		res = dev->handler->set_filename(dev, cmd + 13);
	} else if (strncmp(cmd, "set_threads_num ", 16) == 0) {
		long num_threads;

		res = strict_strtol(cmd + 16, 0, &num_threads);
		if (res) {
			PRINT_ERROR("Bad thread count %s", cmd + 16);
			goto out;
		}
		if (num_threads < 0) {
			PRINT_ERROR("Invalid thread count %ld", num_threads);
			goto out;
		}
		res = scst_dev_set_threads_num(dev, num_threads);
	} else if (strncmp(cmd, "set_thread_pool_type ", 21) == 0) {
		enum scst_dev_type_threads_pool_type newtpt;

		newtpt = scst_parse_threads_pool_type(cmd + 21,
						      strlen(cmd + 21));
		if (newtpt == SCST_THREADS_POOL_TYPE_INVALID) {
			PRINT_ERROR("Invalid thread pool type %s", cmd + 21);
			goto out;
		}
		res = scst_dev_set_thread_pool_type(dev, newtpt);
	}
out:
	TRACE_EXIT_RES(res);
	return res;
}

static void scst_release_dev(struct device *device)
{
	struct scst_device *dev;

	dev = scst_dev_to_dev(device);
	scst_free_device(dev);
}

/**
 * scst_dev_sysfs_init() - Initialize a device for sysfs.
 */
int scst_dev_sysfs_init(struct scst_device *dev)
{
	int res;

	TRACE_ENTRY();

	BUG_ON(!dev);
	BUG_ON(!dev->handler);

	dev->dev_dev.bus     = &scst_device_bus;
	dev->dev_dev.release = scst_release_dev;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	snprintf(dev->dev_dev.bus_id, BUS_ID_SIZE, "%s", dev->virt_name);
#else
	dev_set_name(&dev->dev_dev, "%s", dev->virt_name);
#endif
	res = device_register(&dev->dev_dev);
	if (res)
		PRINT_ERROR("Registration of device %s failed (%d)",
			    dev->virt_name, res);

	TRACE_EXIT();
	return res;
}

/**
 * scst_dev_sysfs_create() - Create sysfs attributes for an SCST device.
 */
int scst_dev_sysfs_create(struct scst_device *dev)
{
	int res = 0;

	TRACE_ENTRY();

	if (dev->handler == &scst_null_devtype)
		goto out;

	dev->dev_dev.driver = &dev->handler->devt_drv;
	device_lock(&dev->dev_dev);
	res = device_bind_driver(&dev->dev_dev);
	device_unlock(&dev->dev_dev);
	if (res)
		goto out_err;

	if (dev->virt_id) {
		/* Virtual SCST device. */
		WARN_ON(dev->scsi_dev);
		res = -ENOMEM;
		dev->dev_exp_kobj = kobject_create_and_add("exported",
						scst_sysfs_get_dev_kobj(dev));
		if (!dev->dev_exp_kobj) {
			PRINT_ERROR("Can't create exported link for device %s",
				    dev->virt_name);
			goto out_err;
		}

		res = device_create_files(scst_sysfs_get_dev_dev(dev),
					  scst_virt_dev_attrs);
		if (res != 0) {
			PRINT_ERROR("Registering attributes for dev %s failed",
				    dev->virt_name);
			goto out_err;
		}

		res = scst_dev_create_debugfs_dir(dev);
		if (res) {
			PRINT_ERROR("Can't create debug files for dev %s",
				    dev->virt_name);
			goto out_err;
		}

		res = scst_dev_create_debugfs_files(dev);
		if (res)
			goto out_err;
	} else {
		/* Pass-through SCSI device. */
		WARN_ON(!dev->scsi_dev);
		res = device_create_files(scst_sysfs_get_dev_dev(dev),
					  scst_pt_dev_attrs);
		if (res != 0) {
			PRINT_ERROR("Registering attributes for dev %s failed",
				    dev->virt_name);
			goto out_err;
		}
	}

	if (dev->handler->threads_num >= 0) {
		res = device_create_files(scst_sysfs_get_dev_dev(dev),
					  dev_thread_attr);
		if (res != 0) {
			PRINT_ERROR("Can't add thread attributes for dev %s",
				    dev->virt_name);
			goto out_err;
		}
	}

	if (dev->handler->dev_attrs) {
		res = device_create_files(scst_sysfs_get_dev_dev(dev),
					  dev->handler->dev_attrs);
		if (res != 0) {
			PRINT_ERROR("Can't add device attributes for dev %s",
				    dev->virt_name);
			goto out_err;
		}
	}

out:
	TRACE_EXIT_RES(res);
	return res;

out_err:
	scst_dev_sysfs_del(dev);
	goto out;
}

/**
 * scst_dev_sysfs_del() - Delete virtual/passthrough device sysfs attributes.
 */
void scst_dev_sysfs_del(struct scst_device *dev)
{
	TRACE_ENTRY();

	BUG_ON(!dev->handler);

	/* Shared */
	scst_dev_remove_debugfs_files(dev);
	scst_dev_remove_debugfs_dir(dev);

	/* Pass-through device attributes. */
	device_remove_files(scst_sysfs_get_dev_dev(dev), scst_pt_dev_attrs);

	/* Virtual device attributes. */
	if (dev->handler->dev_attrs)
		device_remove_files(scst_sysfs_get_dev_dev(dev),
				    dev->handler->dev_attrs);
	device_remove_files(scst_sysfs_get_dev_dev(dev), dev_thread_attr);
	device_remove_files(scst_sysfs_get_dev_dev(dev), scst_virt_dev_attrs);

	/* Shared */
	kobject_del(dev->dev_exp_kobj);
	kobject_put(dev->dev_exp_kobj);
	dev->dev_exp_kobj = NULL;

	device_release_driver(&dev->dev_dev);

	TRACE_EXIT();
}

/**
 * scst_dev_sysfs_put() - Dereference a virtual or pass-through device.
 */
void scst_dev_sysfs_put(struct scst_device *dev)
{
	TRACE_ENTRY();
	device_unregister(&dev->dev_dev);
	TRACE_EXIT();
}

/**
 ** Tgt_dev implementation
 **/

static ssize_t scst_tgt_dev_active_commands_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	int pos;
	struct scst_tgt_dev *tgt_dev;

	tgt_dev = scst_kobj_to_tgt_dev(kobj);

	pos = sprintf(buf, "%d\n", atomic_read(&tgt_dev->tgt_dev_cmd_count));

	return pos;
}

static struct kobj_attribute tgt_dev_active_commands_attr =
	__ATTR(active_commands, S_IRUGO,
		scst_tgt_dev_active_commands_show, NULL);

struct attribute *scst_tgt_dev_attrs[] = {
	&tgt_dev_active_commands_attr.attr,
	NULL,
};

int scst_tgt_dev_sysfs_create(struct scst_tgt_dev *tgt_dev)
{
	int res;

	TRACE_ENTRY();

	res = kobject_add(&tgt_dev->tgt_dev_kobj, &tgt_dev->sess->sess_kobj,
			  "lun%lld", (unsigned long long)tgt_dev->lun);
	if (res != 0) {
		PRINT_ERROR("Can't add tgt_dev %lld to sysfs",
			(unsigned long long)tgt_dev->lun);
		goto out;
	}

	res = scst_tgt_dev_create_debugfs_dir(tgt_dev);
	if (res)
		goto err;

	res = scst_tgt_dev_lat_create(tgt_dev);
	if (res)
		goto err;

out:
	TRACE_EXIT_RES(res);
	return res;
err:
	scst_tgt_dev_sysfs_del(tgt_dev);
	goto out;
}

void scst_tgt_dev_sysfs_del(struct scst_tgt_dev *tgt_dev)
{
	TRACE_ENTRY();
	scst_tgt_dev_lat_remove(tgt_dev);
	scst_tgt_dev_remove_debugfs_dir(tgt_dev);
	kobject_del(&tgt_dev->tgt_dev_kobj);
	TRACE_EXIT();
}

/**
 ** Sessions subdirectory implementation
 **/

static ssize_t scst_sess_sysfs_commands_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct scst_session *sess;

	sess = scst_kobj_to_sess(kobj);

	return sprintf(buf, "%i\n", atomic_read(&sess->sess_cmd_count));
}

static struct kobj_attribute session_commands_attr =
	__ATTR(commands, S_IRUGO, scst_sess_sysfs_commands_show, NULL);

static ssize_t scst_sess_sysfs_initiator_name_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct scst_session *sess;

	sess = scst_kobj_to_sess(kobj);

	return scnprintf(buf, PAGE_SIZE, "%s\n", sess->initiator_name);
}

static struct kobj_attribute session_initiator_name_attr =
	__ATTR(initiator_name, S_IRUGO, scst_sess_sysfs_initiator_name_show, NULL);

#define SCST_SESS_SYSFS_STAT_ATTR(name, exported_name, dir, kb)		\
static ssize_t scst_sess_sysfs_##exported_name##_show(struct kobject *kobj,	\
	struct kobj_attribute *attr, char *buf)					\
{										\
	struct scst_session *sess;						\
	int res;								\
	uint64_t v;								\
										\
	BUILD_BUG_ON(SCST_DATA_UNKNOWN != 0);					\
	BUILD_BUG_ON(SCST_DATA_WRITE != 1);					\
	BUILD_BUG_ON(SCST_DATA_READ != 2);					\
	BUILD_BUG_ON(SCST_DATA_BIDI != 3);					\
	BUILD_BUG_ON(SCST_DATA_NONE != 4);					\
										\
	BUILD_BUG_ON(dir >= SCST_DATA_DIR_MAX);					\
										\
	sess = container_of(kobj, struct scst_session, sess_kobj);		\
	v = sess->io_stats[dir].name;						\
	if (kb)									\
		v >>= 10;							\
	res = sprintf(buf, "%llu\n", (unsigned long long)v);			\
	return res;								\
}										\
										\
static ssize_t scst_sess_sysfs_##exported_name##_store(struct kobject *kobj,	\
	struct kobj_attribute *attr, const char *buf, size_t count)		\
{										\
	struct scst_session *sess;						\
	sess = container_of(kobj, struct scst_session, sess_kobj);		\
	spin_lock_irq(&sess->sess_list_lock);					\
	BUILD_BUG_ON(dir >= SCST_DATA_DIR_MAX);					\
	sess->io_stats[dir].cmd_count = 0;					\
	sess->io_stats[dir].io_byte_count = 0;					\
	spin_unlock_irq(&sess->sess_list_lock);					\
	return count;								\
}										\
										\
static struct kobj_attribute session_##exported_name##_attr =			\
	__ATTR(exported_name, S_IRUGO | S_IWUSR,				\
		scst_sess_sysfs_##exported_name##_show,	\
		scst_sess_sysfs_##exported_name##_store);

SCST_SESS_SYSFS_STAT_ATTR(cmd_count, unknown_cmd_count, SCST_DATA_UNKNOWN, 0);
SCST_SESS_SYSFS_STAT_ATTR(cmd_count, write_cmd_count, SCST_DATA_WRITE, 0);
SCST_SESS_SYSFS_STAT_ATTR(io_byte_count, write_io_count_kb, SCST_DATA_WRITE, 1);
SCST_SESS_SYSFS_STAT_ATTR(cmd_count, read_cmd_count, SCST_DATA_READ, 0);
SCST_SESS_SYSFS_STAT_ATTR(io_byte_count, read_io_count_kb, SCST_DATA_READ, 1);
SCST_SESS_SYSFS_STAT_ATTR(cmd_count, bidi_cmd_count, SCST_DATA_BIDI, 0);
SCST_SESS_SYSFS_STAT_ATTR(io_byte_count, bidi_io_count_kb, SCST_DATA_BIDI, 1);
SCST_SESS_SYSFS_STAT_ATTR(cmd_count, none_cmd_count, SCST_DATA_NONE, 0);

struct attribute *scst_session_attrs[] = {
	&session_commands_attr.attr,
	&session_initiator_name_attr.attr,
	&session_unknown_cmd_count_attr.attr,
	&session_write_cmd_count_attr.attr,
	&session_write_io_count_kb_attr.attr,
	&session_read_cmd_count_attr.attr,
	&session_read_io_count_kb_attr.attr,
	&session_bidi_cmd_count_attr.attr,
	&session_bidi_io_count_kb_attr.attr,
	&session_none_cmd_count_attr.attr,
	NULL,
};

static int scst_create_sess_luns_link(struct scst_session *sess)
{
	int res;

	/*
	 * No locks are needed, because sess supposed to be in acg->acg_sess_list
	 * and tgt->sess_list, so blocking them from disappearing.
	 */

	if (sess->acg == sess->tgt->default_acg)
		res = sysfs_create_link(&sess->sess_kobj,
				sess->tgt->tgt_luns_kobj, "luns");
	else
		res = sysfs_create_link(&sess->sess_kobj,
				sess->acg->luns_kobj, "luns");

	if (res != 0)
		PRINT_ERROR("Can't create luns link for initiator %s",
			sess->initiator_name);

	return res;
}

int scst_recreate_sess_luns_link(struct scst_session *sess)
{
	sysfs_remove_link(&sess->sess_kobj, "luns");
	return scst_create_sess_luns_link(sess);
}

int scst_sess_sysfs_create(struct scst_session *sess)
{
	int res;
	const char *name = sess->unique_session_name;

	TRACE_ENTRY();

	TRACE_DBG("Adding session %s to sysfs", name);

	res = kobject_add(&sess->sess_kobj, sess->tgt->tgt_sess_kobj, name);
	if (res != 0) {
		PRINT_ERROR("Can't add session %s to sysfs", name);
		goto out_free;
	}

	if (sess->tgt->tgtt->sess_attrs) {
		res = sysfs_create_files(&sess->sess_kobj,
					 sess->tgt->tgtt->sess_attrs);
		if (res != 0) {
			PRINT_ERROR("Can't add attributes for session %s", name);
			goto out_free;
		}
	}

	res = scst_create_sess_luns_link(sess);
	if (res)
		goto out_free;

	res = scst_sess_create_debugfs_dir(sess);
	if (res)
		goto out_free;

	res = scst_sess_lat_create(sess);
	if (res)
		goto out_free;

out:
	TRACE_EXIT_RES(res);
	return res;
out_free:
	scst_sess_sysfs_del(sess);
	goto out;
}

void scst_sess_sysfs_del(struct scst_session *sess)
{
	TRACE_ENTRY();
	scst_sess_lat_remove(sess);
	scst_sess_remove_debugfs_dir(sess);
	kobject_del(&sess->sess_kobj);
	TRACE_EXIT();
}

/**
 ** Target luns directory implementation
 **/

static ssize_t scst_lun_rd_only_show(struct kobject *kobj,
				   struct kobj_attribute *attr,
				   char *buf)
{
	struct scst_acg_dev *acg_dev;

	acg_dev = scst_kobj_to_acg_dev(kobj);

	return sprintf(buf, "%d\n", acg_dev->rd_only || acg_dev->dev->rd_only);
}

static struct kobj_attribute lun_options_attr =
	__ATTR(read_only, S_IRUGO, scst_lun_rd_only_show, NULL);

struct attribute *lun_attrs[] = {
	&lun_options_attr.attr,
	NULL,
};

void scst_acg_dev_sysfs_del(struct scst_acg_dev *acg_dev)
{
	TRACE_ENTRY();
	BUG_ON(!acg_dev->dev);
	sysfs_remove_link(acg_dev->dev->dev_exp_kobj,
			  acg_dev->acg_dev_link_name);
	kobject_put(scst_sysfs_get_dev_kobj(acg_dev->dev));
	kobject_del(&acg_dev->acg_dev_kobj);
	TRACE_EXIT();
}

int scst_acg_dev_sysfs_create(struct scst_acg_dev *acg_dev,
			      struct kobject *parent)
{
	int res;

	TRACE_ENTRY();

	BUG_ON(!acg_dev->dev);

	res = kobject_add(&acg_dev->acg_dev_kobj, parent, "%llu", acg_dev->lun);
	if (res != 0) {
		PRINT_ERROR("Can't add acg_dev %s/%s/%s/%llu to sysfs",
			    acg_dev->acg->tgt->tgtt->name,
			    acg_dev->acg->tgt->tgt_name,
			    acg_dev->acg->acg_name, acg_dev->lun);
		goto out;
	}

	kobject_get(scst_sysfs_get_dev_kobj(acg_dev->dev));

	snprintf(acg_dev->acg_dev_link_name, sizeof(acg_dev->acg_dev_link_name),
		 "export%u", acg_dev->dev->dev_exported_lun_num++);

	res = sysfs_create_link(acg_dev->dev->dev_exp_kobj,
			&acg_dev->acg_dev_kobj, acg_dev->acg_dev_link_name);
	if (res != 0) {
		PRINT_ERROR("Can't create acg %s LUN link",
			    acg_dev->acg->acg_name);
		goto out_del;
	}

	res = sysfs_create_link(&acg_dev->acg_dev_kobj,
			scst_sysfs_get_dev_kobj(acg_dev->dev), "device");
	if (res != 0) {
		PRINT_ERROR("Can't create acg %s device link",
			    acg_dev->acg->acg_name);
		goto out_del;
	}

out:
	return res;

out_del:
	scst_acg_dev_sysfs_del(acg_dev);
	goto out;
}

/**
 ** ini_groups directory implementation.
 **/

static int scst_process_acg_mgmt_store(const char *cmd, struct scst_acg *acg)
{
	int res;

	TRACE_ENTRY();

	res = -EINVAL;
	if (strncmp(cmd, "set_cpu_mask ", 13) == 0) {
		cpumask_t *cpumask;

		res = scst_alloc_and_parse_cpumask(&cpumask, cmd + 13,
						   strlen(cmd + 13));
		if (res)
			goto out;
		res = __scst_acg_process_cpu_mask_store(acg->tgt, acg, cpumask);
		kfree(cpumask);
	}
out:
	TRACE_EXIT_RES(res);
	return res;
}

static int __scst_process_luns_mgmt_store(char *buffer,
	struct scst_tgt *tgt, struct scst_acg *acg, bool tgt_kobj)
{
	int res, read_only = 0, action;
	char *p, *e = NULL;
	unsigned int virt_lun;
	struct scst_acg_dev *acg_dev = NULL, *acg_dev_tmp;
	struct scst_device *dev = NULL;

	enum {
		SCST_LUN_ACTION_ADD	= 1,
		SCST_LUN_ACTION_DEL	= 2,
		SCST_LUN_ACTION_REPLACE	= 3,
		SCST_LUN_ACTION_CLEAR	= 4,
	};

	TRACE_ENTRY();

	/*scst_assert_activity_suspended();*/

	TRACE_DBG("buffer %s", buffer);

	p = buffer;
	if (p[strlen(p) - 1] == '\n')
		p[strlen(p) - 1] = '\0';
	if (strncasecmp("add", p, 3) == 0) {
		p += 3;
		action = SCST_LUN_ACTION_ADD;
	} else if (strncasecmp("del", p, 3) == 0) {
		p += 3;
		action = SCST_LUN_ACTION_DEL;
	} else if (!strncasecmp("replace", p, 7)) {
		p += 7;
		action = SCST_LUN_ACTION_REPLACE;
	} else if (!strncasecmp("clear", p, 5)) {
		p += 5;
		action = SCST_LUN_ACTION_CLEAR;
	} else {
		PRINT_ERROR("Unknown action \"%s\"", p);
		res = -EINVAL;
		goto out;
	}

	res = mutex_lock_interruptible(&scst_mutex);
	if (res)
		goto out;

	if ((action != SCST_LUN_ACTION_CLEAR) &&
	    (action != SCST_LUN_ACTION_DEL)) {
		if (!isspace(*p)) {
			PRINT_ERROR("%s", "Syntax error");
			res = -EINVAL;
			goto out_unlock;
		}

		while (isspace(*p) && *p != '\0')
			p++;
		e = p; /* save p */
		while (!isspace(*e) && *e != '\0')
			e++;
		*e = '\0';

		dev = __scst_lookup_dev(p);
		if (dev == NULL) {
			PRINT_ERROR("Device '%s' not found", p);
			res = -EINVAL;
			goto out_unlock;
		}
	}

	switch (action) {
	case SCST_LUN_ACTION_ADD:
	case SCST_LUN_ACTION_REPLACE:
	{
		bool dev_replaced = false;

		e++;
		while (isspace(*e) && *e != '\0')
			e++;

		virt_lun = simple_strtoul(e, &e, 0);
		if (virt_lun > SCST_MAX_LUN) {
			PRINT_ERROR("Too big LUN %d (max %d)", virt_lun,
				SCST_MAX_LUN);
			res = -EINVAL;
			goto out_unlock;
		}

		while (isspace(*e) && *e != '\0')
			e++;

		while (1) {
			char *pp;
			unsigned long val;
			char *param = scst_get_next_token_str(&e);
			if (param == NULL)
				break;

			p = scst_get_next_lexem(&param);
			if (*p == '\0') {
				PRINT_ERROR("Syntax error at %s (device %s)",
					param, dev->virt_name);
				res = -EINVAL;
				goto out_unlock;
			}

			pp = scst_get_next_lexem(&param);
			if (*pp == '\0') {
				PRINT_ERROR("Parameter %s value missed for device %s",
					p, dev->virt_name);
				res = -EINVAL;
				goto out_unlock;
			}

			if (scst_get_next_lexem(&param)[0] != '\0') {
				PRINT_ERROR("Too many parameter's %s values (device %s)",
					p, dev->virt_name);
				res = -EINVAL;
				goto out_unlock;
			}

			res = strict_strtoul(pp, 0, &val);
			if (res) {
				PRINT_ERROR("strict_strtoul() for %s failed: %d "
					"(device %s)", pp, res, dev->virt_name);
				goto out_unlock;
			}

			if (!strcasecmp("read_only", p)) {
				read_only = val;
				TRACE_DBG("READ ONLY %d", read_only);
			} else {
				PRINT_ERROR("Unknown parameter %s (device %s)",
					p, dev->virt_name);
				res = -EINVAL;
				goto out_unlock;
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
			if (action == SCST_LUN_ACTION_ADD) {
				PRINT_ERROR("virt lun %d already exists in "
					"group %s", virt_lun, acg->acg_name);
				res = -EEXIST;
				goto out_unlock;
			} else {
				/* Replace */
				res = scst_acg_del_lun(acg, acg_dev->lun,
						false);
				if (res)
					goto out_unlock;

				dev_replaced = true;
			}
		}

		res = scst_acg_add_lun(acg,
			tgt_kobj ? tgt->tgt_luns_kobj : acg->luns_kobj,
			dev, virt_lun, read_only, !dev_replaced, NULL);
		if (res)
			goto out_unlock;

		if (dev_replaced) {
			struct scst_tgt_dev *tgt_dev;

			list_for_each_entry(tgt_dev, &dev->dev_tgt_dev_list,
				dev_tgt_dev_list_entry) {
				if ((tgt_dev->acg_dev->acg == acg) &&
				    (tgt_dev->lun == virt_lun)) {
					TRACE_MGMT_DBG("INQUIRY DATA HAS CHANGED"
						" on tgt_dev %p", tgt_dev);
					scst_gen_aen_or_ua(tgt_dev,
						SCST_LOAD_SENSE(scst_sense_inquery_data_changed));
				}
			}
		}

		break;
	}
	case SCST_LUN_ACTION_DEL:
		while (isspace(*p) && *p != '\0')
			p++;
		virt_lun = simple_strtoul(p, &p, 0);

		res = scst_acg_del_lun(acg, virt_lun, true);
		if (res != 0)
			goto out_unlock;
		break;
	case SCST_LUN_ACTION_CLEAR:
		PRINT_INFO("Removed all devices from group %s",
			acg->acg_name);
		list_for_each_entry_safe(acg_dev, acg_dev_tmp,
					 &acg->acg_dev_list,
					 acg_dev_list_entry) {
			res = scst_acg_del_lun(acg, acg_dev->lun,
				list_is_last(&acg_dev->acg_dev_list_entry,
					     &acg->acg_dev_list));
			if (res)
				goto out_unlock;
		}
		break;
	}

	res = 0;

out_unlock:
	mutex_unlock(&scst_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_acg_addr_method_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	struct scst_acg *acg;

	acg = scst_kobj_to_acg(kobj);

	return __scst_acg_addr_method_show(acg, buf);
}

static ssize_t scst_acg_addr_method_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int res;
	struct scst_acg *acg;

	acg = scst_kobj_to_acg(kobj);

	res = __scst_acg_addr_method_store(acg, buf, count);

	TRACE_EXIT_RES(res);
	return res;
}

static struct kobj_attribute scst_acg_addr_method =
	__ATTR(addr_method, S_IRUGO | S_IWUSR, scst_acg_addr_method_show,
		scst_acg_addr_method_store);

static ssize_t scst_acg_io_grouping_type_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	struct scst_acg *acg;

	acg = scst_kobj_to_acg(kobj);

	return __scst_acg_io_grouping_type_show(acg, buf);
}

static ssize_t scst_acg_io_grouping_type_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t count)
{
	int res;
	struct scst_acg *acg;

	acg = scst_kobj_to_acg(kobj);

	res = __scst_acg_io_grouping_type_store(acg, buf, count);
	if (res != 0)
		goto out;

	res = count;

out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct kobj_attribute scst_acg_io_grouping_type =
	__ATTR(io_grouping_type, S_IRUGO | S_IWUSR,
	       scst_acg_io_grouping_type_show,
	       scst_acg_io_grouping_type_store);

static ssize_t scst_acg_cpu_mask_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	struct scst_acg *acg;

	acg = scst_kobj_to_acg(kobj);

	return __scst_acg_cpu_mask_show(acg, buf);
}

static struct kobj_attribute scst_acg_cpu_mask =
	__ATTR(cpu_mask, S_IRUGO, scst_acg_cpu_mask_show, NULL);

void scst_acg_sysfs_del(struct scst_acg *acg)
{
	TRACE_ENTRY();

	kobject_del(acg->luns_kobj);
	kobject_del(acg->initiators_kobj);
	kobject_del(&acg->acg_kobj);

	kobject_put(acg->luns_kobj);
	kobject_put(acg->initiators_kobj);

	TRACE_EXIT();
}

int scst_acg_sysfs_create(struct scst_tgt *tgt, struct scst_acg *acg)
{
	int res;

	TRACE_ENTRY();

	res = kobject_add(&acg->acg_kobj, tgt->tgt_ini_grp_kobj, acg->acg_name);
	if (res != 0) {
		PRINT_ERROR("Can't add acg '%s' to sysfs", acg->acg_name);
		goto out;
	}

	acg->luns_kobj = kobject_create_and_add("luns", &acg->acg_kobj);
	if (acg->luns_kobj == NULL) {
		PRINT_ERROR("Can't create luns kobj for tgt %s",
			tgt->tgt_name);
		res = -ENOMEM;
		goto out_del;
	}

	res = sysfs_create_file(acg->luns_kobj, &scst_lun_parameters.attr);
	if (res != 0) {
		PRINT_ERROR("Can't add tgt attr %s for tgt %s",
			scst_lun_parameters.attr.name, tgt->tgt_name);
		goto out_del;
	}

	acg->initiators_kobj = kobject_create_and_add("initiators",
					&acg->acg_kobj);
	if (acg->initiators_kobj == NULL) {
		PRINT_ERROR("Can't create initiators kobj for tgt %s",
			tgt->tgt_name);
		res = -ENOMEM;
		goto out_del;
	}

	res = sysfs_create_file(&acg->acg_kobj, &scst_acg_addr_method.attr);
	if (res != 0) {
		PRINT_ERROR("Can't add tgt attr %s for tgt %s",
			scst_acg_addr_method.attr.name, tgt->tgt_name);
		goto out_del;
	}

	res = sysfs_create_file(&acg->acg_kobj, &scst_acg_io_grouping_type.attr);
	if (res != 0) {
		PRINT_ERROR("Can't add tgt attr %s for tgt %s",
			scst_acg_io_grouping_type.attr.name, tgt->tgt_name);
		goto out_del;
	}

	res = sysfs_create_file(&acg->acg_kobj, &scst_acg_cpu_mask.attr);
	if (res != 0) {
		PRINT_ERROR("Can't add tgt attr %s for tgt %s",
			scst_acg_cpu_mask.attr.name, tgt->tgt_name);
		goto out_del;
	}

out:
	TRACE_EXIT_RES(res);
	return res;

out_del:
	scst_acg_sysfs_del(acg);
	goto out;
}

static int scst_process_ini_group_mgmt_store(char *buffer,
	struct scst_tgt *tgt)
{
	int res, action;
	char *p, *e = NULL;
	struct scst_acg *a, *acg = NULL;

	enum {
		SCST_INI_GROUP_ACTION_CREATE = 1,
		SCST_INI_GROUP_ACTION_DEL    = 2,
	};

	TRACE_ENTRY();

	scst_assert_activity_suspended();

	TRACE_DBG("tgt %p, buffer %s", tgt, buffer);

	p = buffer;
	if (p[strlen(p) - 1] == '\n')
		p[strlen(p) - 1] = '\0';
	if (strncasecmp("create ", p, 7) == 0) {
		p += 7;
		action = SCST_INI_GROUP_ACTION_CREATE;
	} else if (strncasecmp("del ", p, 4) == 0) {
		p += 4;
		action = SCST_INI_GROUP_ACTION_DEL;
	} else {
		PRINT_ERROR("Unknown action \"%s\"", p);
		res = -EINVAL;
		goto out;
	}

	res = mutex_lock_interruptible(&scst_mutex);
	if (res)
		goto out;

	while (isspace(*p) && *p != '\0')
		p++;
	e = p;
	while (!isspace(*e) && *e != '\0')
		e++;
	*e = '\0';

	if (p[0] == '\0') {
		PRINT_ERROR("%s", "Group name required");
		res = -EINVAL;
		goto out_unlock;
	}

	list_for_each_entry(a, &tgt->tgt_acg_list, acg_list_entry) {
		if (strcmp(a->acg_name, p) == 0) {
			TRACE_DBG("group (acg) %p %s found",
				  a, a->acg_name);
			acg = a;
			break;
		}
	}

	switch (action) {
	case SCST_INI_GROUP_ACTION_CREATE:
		TRACE_DBG("Creating group '%s'", p);
		if (acg != NULL) {
			PRINT_ERROR("acg name %s exist", p);
			res = -EINVAL;
			goto out_unlock;
		}
		acg = scst_alloc_add_acg(tgt, p, true);
		if (acg == NULL) {
			res = -ENOMEM;
			goto out_unlock;
		}
		break;
	case SCST_INI_GROUP_ACTION_DEL:
		TRACE_DBG("Deleting group '%s'", p);
		if (acg == NULL) {
			PRINT_ERROR("Group %s not found", p);
			res = -EINVAL;
			goto out_unlock;
		}
		if (!scst_acg_sess_is_empty(acg)) {
			PRINT_ERROR("Group %s is not empty", acg->acg_name);
			res = -EBUSY;
			goto out_unlock;
		}
		scst_del_free_acg(acg);
		break;
	}

	res = 0;

out_unlock:
	mutex_unlock(&scst_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;
}

/**
 ** acn
 **/

static ssize_t scst_acn_file_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", attr->attr.name);
}

int scst_acn_sysfs_create(struct scst_acn *acn)
{
	int res = 0;
	struct scst_acg *acg = acn->acg;
	struct kobj_attribute *attr = NULL;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	static struct lock_class_key __key;
#endif
#endif

	TRACE_ENTRY();

	acn->acn_attr = NULL;

	attr = kzalloc(sizeof(struct kobj_attribute), GFP_KERNEL);
	if (attr == NULL) {
		PRINT_ERROR("Unable to allocate attributes for initiator '%s'",
			acn->name);
		res = -ENOMEM;
		goto out;
	}

	attr->attr.name = kstrdup(acn->name, GFP_KERNEL);
	if (attr->attr.name == NULL) {
		PRINT_ERROR("Unable to allocate attributes for initiator '%s'",
			acn->name);
		res = -ENOMEM;
		goto out_free;
	}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 36)
	attr->attr.owner = THIS_MODULE;
#endif
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
#ifdef CONFIG_DEBUG_LOCK_ALLOC
	attr->attr.key = &__key;
#endif
#endif

	attr->attr.mode = S_IRUGO;
	attr->show = scst_acn_file_show;
	attr->store = NULL;

	res = sysfs_create_file(acg->initiators_kobj, &attr->attr);
	if (res != 0) {
		PRINT_ERROR("Unable to create acn '%s' for group '%s'",
			acn->name, acg->acg_name);
		kfree(attr->attr.name);
		goto out_free;
	}

	acn->acn_attr = attr;

out:
	TRACE_EXIT_RES(res);
	return res;

out_free:
	kfree(attr);
	goto out;
}

void scst_acn_sysfs_del(struct scst_acn *acn)
{
	struct scst_acg *acg = acn->acg;

	TRACE_ENTRY();

	if (acn->acn_attr != NULL) {
		sysfs_remove_file(acg->initiators_kobj,
			&acn->acn_attr->attr);
		kfree(acn->acn_attr->attr.name);
		kfree(acn->acn_attr);
	}

	TRACE_EXIT();
	return;
}

static int scst_process_acg_ini_mgmt_store(char *buffer,
	struct scst_tgt *tgt, struct scst_acg *acg)
{
	int res, action;
	char *p, *e = NULL;
	char *name = NULL, *group = NULL;
	struct scst_acg *acg_dest = NULL;
	struct scst_acn *acn = NULL, *acn_tmp;

	enum {
		SCST_ACG_ACTION_INI_ADD	  = 1,
		SCST_ACG_ACTION_INI_DEL	  = 2,
		SCST_ACG_ACTION_INI_CLEAR = 3,
		SCST_ACG_ACTION_INI_MOVE  = 4,
	};

	TRACE_ENTRY();

	scst_assert_activity_suspended();

	TRACE_DBG("tgt %p, acg %p, buffer %s", tgt, acg, buffer);

	p = buffer;
	if (p[strlen(p) - 1] == '\n')
		p[strlen(p) - 1] = '\0';

	if (strncasecmp("add", p, 3) == 0) {
		p += 3;
		action = SCST_ACG_ACTION_INI_ADD;
	} else if (strncasecmp("del", p, 3) == 0) {
		p += 3;
		action = SCST_ACG_ACTION_INI_DEL;
	} else if (strncasecmp("clear", p, 5) == 0) {
		p += 5;
		action = SCST_ACG_ACTION_INI_CLEAR;
	} else if (strncasecmp("move", p, 4) == 0) {
		p += 4;
		action = SCST_ACG_ACTION_INI_MOVE;
	} else {
		PRINT_ERROR("Unknown action \"%s\"", p);
		res = -EINVAL;
		goto out;
	}

	if (action != SCST_ACG_ACTION_INI_CLEAR)
		if (!isspace(*p)) {
			PRINT_ERROR("%s", "Syntax error");
			res = -EINVAL;
			goto out;
		}

	res = mutex_lock_interruptible(&scst_mutex);
	if (res)
		goto out;

	if (action != SCST_ACG_ACTION_INI_CLEAR)
		while (isspace(*p) && *p != '\0')
			p++;

	switch (action) {
	case SCST_ACG_ACTION_INI_ADD:
		e = p;
		while (!isspace(*e) && *e != '\0')
			e++;
		*e = '\0';
		name = p;

		if (name[0] == '\0') {
			PRINT_ERROR("%s", "Invalid initiator name");
			res = -EINVAL;
			goto out_unlock;
		}

		res = scst_acg_add_acn(acg, name);
		if (res)
			goto out_unlock;
		break;
	case SCST_ACG_ACTION_INI_DEL:
		e = p;
		while (!isspace(*e) && *e != '\0')
			e++;
		*e = '\0';
		name = p;

		if (name[0] == '\0') {
			PRINT_ERROR("%s", "Invalid initiator name");
			res = -EINVAL;
			goto out_unlock;
		}

		acn = scst_find_acn(acg, name);
		if (acn == NULL) {
			PRINT_ERROR("Unable to find "
				"initiator '%s' in group '%s'",
				name, acg->acg_name);
			res = -EINVAL;
			goto out_unlock;
		}
		scst_del_free_acn(acn, true);
		break;
	case SCST_ACG_ACTION_INI_CLEAR:
		list_for_each_entry_safe(acn, acn_tmp, &acg->acn_list,
				acn_list_entry) {
			scst_del_free_acn(acn, false);
		}
		scst_check_reassign_sessions();
		break;
	case SCST_ACG_ACTION_INI_MOVE:
		e = p;
		while (!isspace(*e) && *e != '\0')
			e++;
		if (*e == '\0') {
			PRINT_ERROR("%s", "Too few parameters");
			res = -EINVAL;
			goto out_unlock;
		}
		*e = '\0';
		name = p;

		if (name[0] == '\0') {
			PRINT_ERROR("%s", "Invalid initiator name");
			res = -EINVAL;
			goto out_unlock;
		}

		e++;
		p = e;
		while (!isspace(*e) && *e != '\0')
			e++;
		*e = '\0';
		group = p;

		if (group[0] == '\0') {
			PRINT_ERROR("%s", "Invalid group name");
			res = -EINVAL;
			goto out_unlock;
		}

		TRACE_DBG("Move initiator '%s' to group '%s'",
			name, group);

		acn = scst_find_acn(acg, name);
		if (acn == NULL) {
			PRINT_ERROR("Unable to find "
				"initiator '%s' in group '%s'",
				name, acg->acg_name);
			res = -EINVAL;
			goto out_unlock;
		}
		acg_dest = scst_tgt_find_acg(tgt, group);
		if (acg_dest == NULL) {
			PRINT_ERROR("Unable to find group '%s' in target '%s'",
				group, tgt->tgt_name);
			res = -EINVAL;
			goto out_unlock;
		}
		if (scst_find_acn(acg_dest, name) != NULL) {
			PRINT_ERROR("Initiator '%s' already exists in group '%s'",
				name, acg_dest->acg_name);
			res = -EEXIST;
			goto out_unlock;
		}
		scst_del_free_acn(acn, false);

		res = scst_acg_add_acn(acg_dest, name);
		if (res)
			goto out_unlock;
		break;
	}

	res = 0;

out_unlock:
	mutex_unlock(&scst_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;
}


/**
 ** Dev handlers
 **/

static ssize_t scst_devt_type_show(struct device_driver *drv, char *buf)
{
	struct scst_dev_type *devt = scst_drv_to_devt(drv);

	return scnprintf(buf, PAGE_SIZE, "%d\n", devt->type);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
static const struct driver_attribute scst_devt_type_attr =
#else
static struct driver_attribute scst_devt_type_attr =
#endif
	__ATTR(type, S_IRUGO, scst_devt_type_show, NULL);

static ssize_t scst_devt_type_description_show(struct device_driver *drv,
					       char *buf)
{
	struct scst_dev_type *devt = scst_drv_to_devt(drv);

	return scnprintf(buf, PAGE_SIZE, "%s\n",
		(unsigned)devt->type >= ARRAY_SIZE(scst_dev_handler_types) ?
			"unknown" : scst_dev_handler_types[devt->type]);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
static const struct driver_attribute scst_devt_type_description_attr =
#else
static struct driver_attribute scst_devt_type_description_attr =
#endif
	__ATTR(type_description, S_IRUGO, scst_devt_type_description_show,
	       NULL);

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 34)
static const struct driver_attribute *scst_devt_default_attrs[] = {
#else
static struct driver_attribute *scst_devt_default_attrs[] = {
#endif
	&scst_devt_type_attr,
	&scst_devt_type_description_attr,
	NULL
};

static ssize_t scst_devt_add_device_parameters_show(struct device_driver *drv,
						    char *buf)
{
	struct scst_dev_type *devt;
	const char *const *p;
	ssize_t res;

	devt = scst_drv_to_devt(drv);
	res = 0;
	for (p = devt->add_device_parameters; p && *p; p++)
		res += scnprintf(buf + res, PAGE_SIZE - res, "%s\n", *p);
	return res;
}

static struct driver_attribute scst_devt_add_device_parameters_attr =
	__ATTR(add_device_parameters, S_IRUGO,
	       scst_devt_add_device_parameters_show, NULL);

static ssize_t scst_devt_devt_attributes_show(struct device_driver *drv,
					      char *buf)
{
	struct scst_dev_type *devt;
	const char *const *p;
	ssize_t res;

	devt = scst_drv_to_devt(drv);
	res = 0;
	for (p = devt->devt_optional_attributes; p && *p; p++)
		res += scnprintf(buf + res, PAGE_SIZE - res, "%s\n", *p);
	return res;
}

static struct driver_attribute scst_devt_devt_attributes_attr =
	__ATTR(driver_attributes, S_IRUGO,
	       scst_devt_devt_attributes_show, NULL);

static ssize_t scst_devt_drv_attributes_show(struct device_driver *drv,
					     char *buf)
{
	struct scst_dev_type *devt;
	const char *const *p;
	ssize_t res;

	devt = scst_drv_to_devt(drv);
	res = 0;
	for (p = devt->dev_optional_attributes; p && *p; p++)
		res += scnprintf(buf + res, PAGE_SIZE - res, "%s\n", *p);
	return res;
}

static struct driver_attribute scst_devt_drv_attributes_attr =
	__ATTR(device_attributes, S_IRUGO,
	       scst_devt_drv_attributes_show, NULL);

static int scst_process_devt_mgmt_store(char *buffer,
					struct scst_dev_type *devt)
{
	int res = 0;
	char *p, *pp, *dev_name;

	TRACE_ENTRY();

	TRACE_DBG("devt %p, buffer %s", devt, buffer);

	pp = buffer;
	if (pp[strlen(pp) - 1] == '\n')
		pp[strlen(pp) - 1] = '\0';

	p = scst_get_next_lexem(&pp);

	if (strcasecmp("add_device", p) == 0) {
		dev_name = scst_get_next_lexem(&pp);
		if (*dev_name == '\0') {
			PRINT_ERROR("%s", "Device name required");
			res = -EINVAL;
			goto out;
		}
		res = devt->add_device(dev_name, pp);
	} else if (strcasecmp("del_device", p) == 0) {
		dev_name = scst_get_next_lexem(&pp);
		if (*dev_name == '\0') {
			PRINT_ERROR("%s", "Device name required");
			res = -EINVAL;
			goto out;
		}

		p = scst_get_next_lexem(&pp);
		if (*p != '\0')
			goto out_syntax_err;

		res = devt->del_device(dev_name);
	} else if (devt->mgmt_cmd != NULL) {
		scst_restore_token_str(p, pp);
		res = devt->mgmt_cmd(buffer);
	} else {
		PRINT_ERROR("Unknown action \"%s\"", p);
		res = -EINVAL;
		goto out;
	}

out:
	TRACE_EXIT_RES(res);
	return res;

out_syntax_err:
	PRINT_ERROR("Syntax error on \"%s\"", p);
	res = -EINVAL;
	goto out;
}

static int scst_process_devt_pass_through_mgmt_store(char *buffer,
	struct scst_dev_type *devt)
{
	int res = 0;
	char *p, *pp, *action;
	unsigned long host, channel, id, lun;
	struct scst_device *d, *dev = NULL;

	TRACE_ENTRY();

	scst_assert_activity_suspended();

	TRACE_DBG("devt %p, buffer %s", devt, buffer);

	pp = buffer;
	if (pp[strlen(pp) - 1] == '\n')
		pp[strlen(pp) - 1] = '\0';

	action = scst_get_next_lexem(&pp);
	p = scst_get_next_lexem(&pp);
	if (*p == '\0') {
		PRINT_ERROR("%s", "Device required");
		res = -EINVAL;
		goto out;
	}

	if (*scst_get_next_lexem(&pp) != '\0') {
		PRINT_ERROR("%s", "Too many parameters");
		res = -EINVAL;
		goto out_syntax_err;
	}

	host = simple_strtoul(p, &p, 0);
	if ((host == ULONG_MAX) || (*p != ':'))
		goto out_syntax_err;
	p++;
	channel = simple_strtoul(p, &p, 0);
	if ((channel == ULONG_MAX) || (*p != ':'))
		goto out_syntax_err;
	p++;
	id = simple_strtoul(p, &p, 0);
	if ((channel == ULONG_MAX) || (*p != ':'))
		goto out_syntax_err;
	p++;
	lun = simple_strtoul(p, &p, 0);
	if (lun == ULONG_MAX)
		goto out_syntax_err;

	TRACE_DBG("Dev %ld:%ld:%ld:%ld", host, channel, id, lun);

	res = mutex_lock_interruptible(&scst_mutex);
	if (res != 0)
		goto out;

	list_for_each_entry(d, &scst_dev_list, dev_list_entry) {
		if ((d->virt_id == 0) &&
		    d->scsi_dev->host->host_no == host &&
		    d->scsi_dev->channel == channel &&
		    d->scsi_dev->id == id &&
		    d->scsi_dev->lun == lun) {
			dev = d;
			TRACE_DBG("Dev %p (%ld:%ld:%ld:%ld) found",
				  dev, host, channel, id, lun);
			break;
		}
	}
	if (dev == NULL) {
		PRINT_ERROR("Device %ld:%ld:%ld:%ld not found",
			       host, channel, id, lun);
		res = -EINVAL;
		goto out_unlock;
	}

	if (dev->scsi_dev->type != devt->type) {
		PRINT_ERROR("Type %d of device %s differs from type "
			"%d of dev handler %s", dev->type,
			dev->virt_name, devt->type, devt->name);
		res = -EINVAL;
		goto out_unlock;
	}

	if (strcasecmp("add_device", action) == 0) {
		res = scst_assign_dev_handler(dev, devt);
		if (res == 0)
			PRINT_INFO("Device %s assigned to dev handler %s",
				dev->virt_name, devt->name);
	} else if (strcasecmp("del_device", action) == 0) {
		if (dev->handler != devt) {
			PRINT_ERROR("Device %s is not assigned to handler %s",
				dev->virt_name, devt->name);
			res = -EINVAL;
			goto out_unlock;
		}
		res = scst_assign_dev_handler(dev, &scst_null_devtype);
		if (res == 0)
			PRINT_INFO("Device %s unassigned from dev handler %s",
				dev->virt_name, devt->name);
	} else {
		PRINT_ERROR("Unknown action \"%s\"", action);
		res = -EINVAL;
		goto out_unlock;
	}

out_unlock:
	mutex_unlock(&scst_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;

out_syntax_err:
	PRINT_ERROR("Syntax error on \"%s\"", p);
	res = -EINVAL;
	goto out;
}

static int scst_device_bus_match(struct device *d, struct device_driver *drv)
{
	struct scst_device *dev = scst_dev_to_dev(d);
	struct scst_dev_type *devt = scst_drv_to_devt(drv);
	int res;

	TRACE_ENTRY();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
	lockdep_assert_held(&scst_mutex);
#endif

	res = __scst_lookup_devt(drv->name) == devt
		&& __scst_lookup_dev(dev_name(d)) == dev
		&& dev->handler == devt;

	TRACE_DBG("%s(%s, %s): %d", __func__, drv->name, dev_name(d), res);

	TRACE_EXIT_RES(res);
	return res;
}

static struct bus_type scst_device_bus = {
	.name = "scst_tgt_dev",
	.match = scst_device_bus_match,
};

int scst_devt_sysfs_init(struct scst_dev_type *devt)
{
	int res;

	TRACE_ENTRY();

	WARN_ON(!devt->module);

	devt->devt_drv.bus  = &scst_device_bus;
	devt->devt_drv.name = devt->name;
	devt->devt_drv.owner = devt->module;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 32)
	devt->devt_drv.suppress_bind_attrs = true;
#endif
	res = driver_register(&devt->devt_drv);

	TRACE_EXIT_RES(res);
	return res;
}

int scst_devt_sysfs_create(struct scst_dev_type *devt)
{
	int res;

	TRACE_ENTRY();

	res = driver_create_files(scst_sysfs_get_devt_drv(devt),
				  scst_devt_default_attrs);
	if (res)
		goto out_err;

	if (devt->add_device_parameters) {
		res = driver_create_file(scst_sysfs_get_devt_drv(devt),
					 &scst_devt_add_device_parameters_attr);
		if (res) {
			PRINT_ERROR("Can't add attribute %s for dev handler %s",
				scst_devt_add_device_parameters_attr.attr.name,
				devt->name);
			goto out_err;
		}
	}

	if (devt->devt_optional_attributes) {
		res = driver_create_file(scst_sysfs_get_devt_drv(devt),
					 &scst_devt_devt_attributes_attr);
		if (res) {
			PRINT_ERROR("Can't add attribute %s for dev handler %s",
				scst_devt_devt_attributes_attr.attr.name,
				devt->name);
			goto out_err;
		}
	}

	if (devt->dev_optional_attributes) {
		res = driver_create_file(scst_sysfs_get_devt_drv(devt),
					 &scst_devt_drv_attributes_attr);
		if (res) {
			PRINT_ERROR("Can't add attribute %s for dev handler %s",
				scst_devt_drv_attributes_attr.attr.name,
				devt->name);
			goto out_err;
		}
	}

	if (devt->devt_attrs) {
		res = driver_create_files(scst_sysfs_get_devt_drv(devt),
					  devt->devt_attrs);
		if (res != 0) {
			PRINT_ERROR("Can't add attributes for dev handler %s",
				    devt->name);
			goto out_err;
		}
	}

	res = scst_devt_create_debugfs_dir(devt);
	if (res) {
		PRINT_ERROR("Can't create tracing files for device type %s",
			    devt->name);
		goto out_err;
	}

	res = scst_devt_create_debugfs_files(devt);
	if (res)
		goto out_err;

out:
	TRACE_EXIT_RES(res);
	return res;

out_err:
	scst_devt_sysfs_del(devt);
	goto out;
}

void scst_devt_sysfs_del(struct scst_dev_type *devt)
{
	TRACE_ENTRY();
	scst_devt_remove_debugfs_files(devt);
	scst_devt_remove_debugfs_dir(devt);
	TRACE_EXIT();
}

void scst_devt_sysfs_put(struct scst_dev_type *devt)
{
	TRACE_ENTRY();
	driver_unregister(&devt->devt_drv);
	TRACE_EXIT();
}

/**
 ** SCST sysfs device_groups/<dg>/devices/<dev> implementation.
 **/

int scst_dg_dev_sysfs_add(struct scst_dev_group *dg, struct scst_dg_dev *dgdev)
{
	int res;

	TRACE_ENTRY();
	res = sysfs_create_link(dg->dev_kobj,
				scst_sysfs_get_dev_kobj(dgdev->dev),
				dgdev->dev->virt_name);
	TRACE_EXIT_RES(res);
	return res;
}

void scst_dg_dev_sysfs_del(struct scst_dev_group *dg, struct scst_dg_dev *dgdev)
{
	TRACE_ENTRY();
	sysfs_remove_link(dg->dev_kobj, dgdev->dev->virt_name);
	TRACE_EXIT();
}

/**
 ** SCST sysfs device_groups/<dg>/devices directory implementation.
 **/

static int scst_dg_devs_mgmt_store_work_fn(char *cmd, struct scst_dev_group *dg)
{
	char *p, *pp, *dev_name;
	int res;

	TRACE_ENTRY();

	WARN_ON(!dg);

	p = strchr(cmd, '\n');
	if (p)
		*p = '\0';

	res = -EINVAL;
	pp = cmd;
	p = scst_get_next_lexem(&pp);
	if (strcasecmp(p, "add") == 0) {
		dev_name = scst_get_next_lexem(&pp);
		if (!*dev_name)
			goto out;
		res = scst_dg_dev_add(dg, dev_name);
	} else if (strcasecmp(p, "del") == 0) {
		dev_name = scst_get_next_lexem(&pp);
		if (!*dev_name)
			goto out;
		res = scst_dg_dev_remove_by_name(dg, dev_name);
	}
out:
	TRACE_EXIT_RES(res);
	return res;
}

static const struct attribute *scst_dg_devs_attrs[] = {
	NULL,
};

/**
 ** SCST sysfs device_groups/<dg>/target_groups/<tg>/<tgt> implementation.
 **/

static ssize_t scst_tg_tgt_rel_tgt_id_show(struct kobject *kobj,
					   struct kobj_attribute *attr,
					   char *buf)
{
	struct scst_tg_tgt *tg_tgt;

	tg_tgt = container_of(kobj, struct scst_tg_tgt, kobj);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tg_tgt->rel_tgt_id);
}

static ssize_t scst_tg_tgt_rel_tgt_id_store(struct kobject *kobj,
					    struct kobj_attribute *attr,
					    const char *buf, size_t count)
{
	struct scst_tg_tgt *tg_tgt;
	unsigned long rel_tgt_id;
	char ch[8];
	int res;

	TRACE_ENTRY();
	tg_tgt = container_of(kobj, struct scst_tg_tgt, kobj);
	snprintf(ch, sizeof(ch), "%.*s", min_t(int, count, sizeof(ch)-1), buf);
	res = strict_strtoul(ch, 0, &rel_tgt_id);
	if (res)
		goto out;
	res = -EINVAL;
	if (rel_tgt_id == 0 || rel_tgt_id > 0xffff)
		goto out;
	tg_tgt->rel_tgt_id = rel_tgt_id;
	res = count;
out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct kobj_attribute scst_tg_tgt_rel_tgt_id =
	__ATTR(rel_tgt_id, S_IRUGO | S_IWUSR, scst_tg_tgt_rel_tgt_id_show,
	       scst_tg_tgt_rel_tgt_id_store);

static const struct attribute *scst_tg_tgt_attrs[] = {
	&scst_tg_tgt_rel_tgt_id.attr,
	NULL,
};

int scst_tg_tgt_sysfs_add(struct scst_target_group *tg,
			  struct scst_tg_tgt *tg_tgt)
{
	int res;

	TRACE_ENTRY();
	BUG_ON(!tg);
	BUG_ON(!tg_tgt);
	BUG_ON(!tg_tgt->name);
	if (tg_tgt->tgt)
		res = sysfs_create_link(&tg->kobj,
					scst_sysfs_get_tgt_kobj(tg_tgt->tgt),
					tg_tgt->name);
	else {
		res = kobject_add(&tg_tgt->kobj, &tg->kobj, "%s", tg_tgt->name);
		if (res)
			goto err;
		res = sysfs_create_files(&tg_tgt->kobj, scst_tg_tgt_attrs);
		if (res)
			goto err;
	}
out:
	TRACE_EXIT_RES(res);
	return res;
err:
	scst_tg_tgt_sysfs_del(tg, tg_tgt);
	goto out;
}

void scst_tg_tgt_sysfs_del(struct scst_target_group *tg,
			   struct scst_tg_tgt *tg_tgt)
{
	TRACE_ENTRY();
	if (tg_tgt->tgt)
		sysfs_remove_link(&tg->kobj, tg_tgt->name);
	else {
		sysfs_remove_files(&tg_tgt->kobj, scst_tg_tgt_attrs);
		kobject_del(&tg_tgt->kobj);
	}
	TRACE_EXIT();
}

/**
 ** SCST sysfs device_groups/<dg>/target_groups/<tg> directory implementation.
 **/

static ssize_t scst_tg_group_id_show(struct kobject *kobj,
				     struct kobj_attribute *attr,
				     char *buf)
{
	struct scst_target_group *tg;

	tg = container_of(kobj, struct scst_target_group, kobj);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tg->group_id);
}

static ssize_t scst_tg_group_id_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	struct scst_target_group *tg;
	unsigned long group_id;
	char ch[8];
	int res;

	TRACE_ENTRY();
	tg = container_of(kobj, struct scst_target_group, kobj);
	snprintf(ch, sizeof(ch), "%.*s", min_t(int, count, sizeof(ch)-1), buf);
	res = strict_strtoul(ch, 0, &group_id);
	if (res)
		goto out;
	res = -EINVAL;
	if (group_id == 0 || group_id > 0xffff)
		goto out;
	tg->group_id = group_id;
	res = count;
out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct kobj_attribute scst_tg_group_id =
	__ATTR(group_id, S_IRUGO | S_IWUSR, scst_tg_group_id_show,
	       scst_tg_group_id_store);

static ssize_t scst_tg_preferred_show(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      char *buf)
{
	struct scst_target_group *tg;

	tg = container_of(kobj, struct scst_target_group, kobj);
	return scnprintf(buf, PAGE_SIZE, "%u\n", tg->preferred);
}

static ssize_t scst_tg_preferred_store(struct kobject *kobj,
				       struct kobj_attribute *attr,
				       const char *buf, size_t count)
{
	struct scst_target_group *tg;
	unsigned long preferred;
	char ch[8];
	int res;

	TRACE_ENTRY();
	tg = container_of(kobj, struct scst_target_group, kobj);
	snprintf(ch, sizeof(ch), "%.*s", min_t(int, count, sizeof(ch)-1), buf);
	res = strict_strtoul(ch, 0, &preferred);
	if (res)
		goto out;
	res = -EINVAL;
	if (preferred != 0 && preferred != 1)
		goto out;
	tg->preferred = preferred;
	res = count;
out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct kobj_attribute scst_tg_preferred =
	__ATTR(preferred, S_IRUGO | S_IWUSR, scst_tg_preferred_show,
	       scst_tg_preferred_store);

static struct { enum scst_tg_state s; const char *n; } scst_tg_state_names[] = {
	{ SCST_TG_STATE_OPTIMIZED,	"active"	},
	{ SCST_TG_STATE_NONOPTIMIZED,	"nonoptimized"	},
	{ SCST_TG_STATE_STANDBY,	"standby"	},
	{ SCST_TG_STATE_UNAVAILABLE,	"unavailable"	},
	{ SCST_TG_STATE_OFFLINE,	"offline"	},
	{ SCST_TG_STATE_TRANSITIONING,	"transitioning"	},
};

static ssize_t scst_tg_state_show(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  char *buf)
{
	struct scst_target_group *tg;
	int i;

	tg = container_of(kobj, struct scst_target_group, kobj);
	for (i = ARRAY_SIZE(scst_tg_state_names) - 1; i >= 0; i--)
		if (scst_tg_state_names[i].s == tg->state)
			break;

	return scnprintf(buf, PAGE_SIZE, "%s\n",
			 i >= 0 ? scst_tg_state_names[i].n : "???");
}

static int scst_tg_state_store_work_fn(char *cmd, struct scst_target_group *tg)
{
	char *p;
	int i, res;

	TRACE_ENTRY();

	p = strchr(cmd, '\n');
	if (p)
		*p = '\0';

	for (i = ARRAY_SIZE(scst_tg_state_names) - 1; i >= 0; i--)
		if (strcmp(scst_tg_state_names[i].n, cmd) == 0)
			break;

	res = -EINVAL;
	if (i < 0)
		goto out;
	res = scst_tg_set_state(tg, scst_tg_state_names[i].s);
out:
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_tg_state_store(struct kobject *kobj,
				  struct kobj_attribute *attr,
				  const char *buf, size_t count)
{
	char *cmd;
	int res;

	TRACE_ENTRY();

	res = -ENOMEM;
	cmd = kasprintf(GFP_KERNEL, "%.*s", (int)count, buf);
	if (!cmd)
		goto out;

	res = scst_tg_state_store_work_fn(cmd,
			container_of(kobj, struct scst_target_group, kobj));
out:
	if (res == 0)
		res = count;
	TRACE_EXIT_RES(res);
	return res;
}

static struct kobj_attribute scst_tg_state =
	__ATTR(state, S_IRUGO | S_IWUSR, scst_tg_state_show,
	       scst_tg_state_store);

static int scst_tg_mgmt_store_work_fn(char *cmd, struct scst_target_group *tg)
{
	char *p, *pp, *target_name;
	int res;

	TRACE_ENTRY();

	p = strchr(cmd, '\n');
	if (p)
		*p = '\0';

	res = -EINVAL;
	pp = cmd;
	p = scst_get_next_lexem(&pp);
	if (strcasecmp(p, "add") == 0) {
		target_name = scst_get_next_lexem(&pp);
		if (!*target_name)
			goto out;
		res = scst_tg_tgt_add(tg, target_name);
	} else if (strcasecmp(p, "del") == 0) {
		target_name = scst_get_next_lexem(&pp);
		if (!*target_name)
			goto out;
		res = scst_tg_tgt_remove_by_name(tg, target_name);
	}
out:
	TRACE_EXIT_RES(res);
	return res;
}

static const struct attribute *scst_tg_attrs[] = {
	&scst_tg_group_id.attr,
	&scst_tg_preferred.attr,
	&scst_tg_state.attr,
	NULL,
};

int scst_tg_sysfs_add(struct scst_dev_group *dg, struct scst_target_group *tg)
{
	int res;

	TRACE_ENTRY();
	res = kobject_add(&tg->kobj, dg->tg_kobj, "%s", tg->name);
	if (res)
		goto err;
	res = sysfs_create_files(&tg->kobj, scst_tg_attrs);
	if (res)
		goto err;
out:
	TRACE_EXIT_RES(res);
	return res;
err:
	scst_tg_sysfs_del(tg);
	goto out;
}

void scst_tg_sysfs_del(struct scst_target_group *tg)
{
	TRACE_ENTRY();
	sysfs_remove_files(&tg->kobj, scst_tg_attrs);
	kobject_del(&tg->kobj);
	TRACE_EXIT();
}

/**
 ** SCST sysfs device_groups/<dg>/target_groups directory implementation.
 **/

static int scst_dg_tgs_mgmt_store_work_fn(char *cmd, struct scst_dev_group *dg)
{
	char *p, *pp, *dev_name;
	int res;

	TRACE_ENTRY();

	WARN_ON(!dg);

	p = strchr(cmd, '\n');
	if (p)
		*p = '\0';

	res = -EINVAL;
	pp = cmd;
	p = scst_get_next_lexem(&pp);
	if (strcasecmp(p, "create") == 0 || strcasecmp(p, "add") == 0) {
		dev_name = scst_get_next_lexem(&pp);
		if (!*dev_name)
			goto out;
		res = scst_tg_add(dg, dev_name);
	} else if (strcasecmp(p, "del") == 0) {
		dev_name = scst_get_next_lexem(&pp);
		if (!*dev_name)
			goto out;
		res = scst_tg_remove_by_name(dg, dev_name);
	}
out:
	TRACE_EXIT_RES(res);
	return res;
}

static const struct attribute *scst_dg_tgs_attrs[] = {
	NULL,
};

/**
 ** SCST sysfs device_groups directory implementation.
 **/

int scst_dg_sysfs_add(struct kobject *parent, struct scst_dev_group *dg)
{
	int res;

	dg->dev_kobj = NULL;
	dg->tg_kobj = NULL;
	res = kobject_add(&dg->kobj, parent, "%s", dg->name);
	if (res)
		goto err;
	res = -EEXIST;
	dg->dev_kobj = kobject_create_and_add("devices", &dg->kobj);
	if (!dg->dev_kobj)
		goto err;
	res = sysfs_create_files(dg->dev_kobj, scst_dg_devs_attrs);
	if (res)
		goto err;
	dg->tg_kobj = kobject_create_and_add("target_groups", &dg->kobj);
	if (!dg->tg_kobj)
		goto err;
	res = sysfs_create_files(dg->tg_kobj, scst_dg_tgs_attrs);
	if (res)
		goto err;
out:
	return res;
err:
	scst_dg_sysfs_del(dg);
	goto out;
}

void scst_dg_sysfs_del(struct scst_dev_group *dg)
{
	if (dg->tg_kobj) {
		sysfs_remove_files(dg->tg_kobj, scst_dg_tgs_attrs);
		kobject_del(dg->tg_kobj);
		kobject_put(dg->tg_kobj);
		dg->tg_kobj = NULL;
	}
	if (dg->dev_kobj) {
		sysfs_remove_files(dg->dev_kobj, scst_dg_devs_attrs);
		kobject_del(dg->dev_kobj);
		kobject_put(dg->dev_kobj);
		dg->dev_kobj = NULL;
	}
	kobject_del(&dg->kobj);
}

static ssize_t scst_device_groups_mgmt_store_work_fn(char *cmd)
{
	int res;
	char *p, *pp, *group_name;

	TRACE_ENTRY();

	pp = cmd;
	p = strchr(cmd, '\n');
	if (p)
		*p = '\0';

	res = -EINVAL;
	p = scst_get_next_lexem(&pp);
	if (strcasecmp(p, "create") == 0 || strcasecmp(p, "add") == 0) {
		group_name = scst_get_next_lexem(&pp);
		if (!*group_name)
			goto out;
		res = scst_dg_add(scst_device_groups_kobj, group_name);
	} else if (strcasecmp(p, "del") == 0) {
		group_name = scst_get_next_lexem(&pp);
		if (!*group_name)
			goto out;
		res = scst_dg_remove(group_name);
	}
out:
	TRACE_EXIT_RES(res);
	return res;
}

static const struct attribute *scst_device_groups_attrs[] = {
	NULL,
};

/**
 ** SCST sysfs root directory implementation
 **/

static ssize_t scst_mgmt_show(struct device *device,
			      struct device_attribute *attr, char *buf)
{
	ssize_t count;
	static const char help[] =
/* scst_devt_mgmt or scst_devt_pass_through_mgmt */
"in device_driver/<devt> <devt_cmd>\n"
/* device/<dev>/filename */
"in device/<dev> <dev_cmd>\n"
/* scst_tgtt_mgmt */
"in target_driver/<tgtt> <tgtt_cmd>\n"
/* scst_tgt_mgmt */
"in target_driver/<tgtt>/<target> <tgt_cmd>\n"
/* scst_luns_mgmt */
"in target_driver/<tgtt>/<target>/luns <luns_cmd>\n"
/* scst_ini_group_mgmt */
"in target_driver/<tgtt>/<target>/ini_groups <acg_mgmt_cmd>\n"
"in target_driver/<tgtt>/<target>/ini_groups/<acg> <acg_cmd>\n"
/* scst_acg_luns_mgmt */
"in target_driver/<tgtt>/<target>/ini_groups/<acg>/luns <lun_cmd>\n"
/* scst_acg_ini_mgmt */
"in target_driver/<tgtt>/<target>/ini_groups/<acg>/initiators <acg_ini_cmd>\n"
/* scst_dg_mgmt */
"in device_groups [add|del] <device_group>\n"
/* scst_dg_dev_mgmt */
"in device_groups/<dg>/devices [add|del] <device>\n"
/* scst_tg_mgmt */
"in device_groups/<dg>/target_groups [add|del] <target_group>\n"
/* scst_tg_mgmt */
"in device_groups/<dg>/target_groups/<tg> [add|del] <target>\n"
"\n"
"devt_cmd syntax for virtual device types:\n"
"\n"
"add_device <device_name> [parameters]\n"
"del_device <device_name>\n"
"add_attribute <attribute> <value>\n"
"del_attribute <attribute> <value>\n"
"add_device_attribute <device_name> <attribute> <value>\n"
"del_device_attribute <device_name> <attribute> <value>\n"
"\n"
"devt_cmd syntax for pass-through device types:\n"
"\n"
"add_device H:C:I:L\n"
"del_device H:C:I:L\n"
"\n"
"dev_cmd syntax:\n"
"\n"
"set_filename <filename>\n"
"set_threads_num <n>\n"
"set_thread_pool_type <thread_pool_type>\n"
"\n"
"tgtt_cmd syntax:\n"
"\n"
"add_target <target_name> [parameters]\n"
"del_target <target_name>\n"
"<target-driver-specific-command-and-parameters>\n"
"\n"
"where parameters is one or more <name>=<value> pairs separated by ';'\n"
"\n"
"tgt_cmd syntax:\n"
"\n"
"enable\n"
"disable\n"
"set_cpu_mask <mask>\n"
"\n"
"lun_cmd syntax:\n"
"\n"
"add|del H:C:I:L <lun> [parameters]\n"
"add <vname> <lun> [parameters]\n"
"del <lun>\n"
"replace H:C:I:L <lun> [parameters]\n"
"replace <vname> <lun> [parameters]\n"
"clear\n"
"\n"
"where parameters is either 'read_only' or absent.\n"
"\n"
"acg_mgmt_cmd syntax:\n"
"\n"
"create <group_name>\n"
"del <group_name>\n"
"\n"
"acg_cmd syntax:\n"
"set_cpu_mask <mask>\n"
"\n"
"acg_ini_cmd syntax:\n"
"\n"
"add <initiator_name>\n"
"del <initiator_name>\n"
"move <initiator_name> <dest_group_name>\n"
"clear\n";

	TRACE_ENTRY();

	count = scnprintf(buf, PAGE_SIZE, help);

	TRACE_EXIT_RES(count);
	return count;
}

static enum mgmt_path_type __parse_path(char *path,
					struct scst_dev_type **devt,
					struct scst_device **dev,
					struct scst_tgt_template **tgtt,
					struct scst_tgt **tgt,
					struct scst_acg **acg,
					struct scst_dev_group **dg,
					struct scst_target_group **tg)
{
	char *comp[7];
	int i;
	enum mgmt_path_type res = PATH_NOT_RECOGNIZED;

	TRACE_ENTRY();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
	lockdep_assert_held(&scst_mutex);
#endif

	BUG_ON(!path || !devt || !tgtt || !tgt || !acg || !dg || !tg);

	*devt = NULL;
	*dev = NULL;
	*tgtt = NULL;
	*tgt = NULL;
	*acg = NULL;
	*dg = NULL;
	*tg = NULL;

	if (path[0] == '/')
		path++;
	comp[0] = path;
	for (i = 1; i < ARRAY_SIZE(comp); ++i) {
		comp[i] = strchr(comp[i - 1], '/');
		if (!comp[i])
			break;
		*comp[i]++ = '\0';
	}

	for (i = 0; i < ARRAY_SIZE(comp); ++i) {
		if (!comp[i])
			break;
		TRACE_DBG("comp[%d] = %s", i, comp[i]);
	}

	if (!comp[0])
		goto err;
	if (strcmp(comp[0], "device") == 0) {
		if (!comp[1])
			goto err;
		*dev = __scst_lookup_dev(comp[1]);
		if (!*dev)
			goto err;
		res = DEVICE_PATH;
		goto out;
	} else if (strcmp(comp[0], "device_driver") == 0 && !comp[2]) {
		if (!comp[1])
			goto err;
		*devt = __scst_lookup_devt(comp[1]);
		if (!*devt)
			goto err;
		res = DEVICE_TYPE_PATH;
		goto out;
	} else if (strcmp(comp[0], "target_driver") == 0) {
		if (!comp[1])
			goto err;
		*tgtt = __scst_lookup_tgtt(comp[1]);
		if (!*tgtt)
			goto err;
		if (!comp[2]) {
			res = TARGET_TEMPLATE_PATH;
			goto out;
		}
		*tgt = __scst_lookup_tgt(*tgtt, comp[2]);
		if (!*tgt)
			goto err;
		if (!comp[3]) {
			res = TARGET_PATH;
			goto out;
		}
		if (strcmp(comp[3], "luns") == 0) {
			res = TARGET_LUNS_PATH;
			goto out;
		} else if (strcmp(comp[3], "ini_groups") != 0)
			goto err;
		if (!comp[4]) {
			res = TARGET_INI_GROUPS_PATH;
			goto out;
		}
		if (comp[5] && comp[6])
			goto err;
		*acg = __scst_lookup_acg(*tgt, comp[4]);
		if (!*acg)
			goto err;
		if (!comp[5]) {
			res = ACG_PATH;
			goto out;
		} else if (strcmp(comp[5], "luns") == 0) {
			res = ACG_LUNS_PATH;
			goto out;
		} else if (strcmp(comp[5], "initiators") == 0) {
			res = ACG_INITIATOR_GROUPS_PATH;
			goto out;
		}
	} else if (strcmp(comp[0], "device_groups") == 0) {
		if (!comp[1]) {
			res = DGS_PATH;
			goto out;
		}
		*dg = __scst_lookup_dg_by_name(comp[1]);
		if (!*dg || !comp[2])
			goto err;
		if (strcmp(comp[2], "devices") == 0) {
			if (!comp[3]) {
				res = DGS_DEVS_PATH;
				goto out;
			}
		} else if (strcmp(comp[2], "target_groups") == 0) {
			if (!comp[3]) {
				res = TGS_PATH;
				goto out;
			}
			*tg = __scst_lookup_tg_by_name(*dg, comp[3]);
			if (!*tg || comp[4])
				goto err;
			res = TGS_TG_PATH;
		}
		goto err;
	}
out:
	TRACE_EXIT_RES(res);
err:
	return res;
}

static ssize_t scst_mgmt_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	ssize_t res;
	char *buffer, *path, *path_end, *cmd;
	enum mgmt_path_type mgmt_path_type;
	struct scst_dev_type *devt;
	struct scst_device *dev;
	struct scst_tgt_template *tgtt;
	struct scst_tgt *tgt;
	struct scst_acg *acg;
	struct scst_dev_group *dg;
	struct scst_target_group *tg;

	TRACE_ENTRY();

	TRACE_DBG("Processing cmd \"%.*s\"",
		  count >= 1 && buf[count - 1] == '\n'
		  ? (int)count - 1 : (int)count,
		  buf);

	res = -ENOMEM;
	buffer = kasprintf(GFP_KERNEL, "%.*s", (int)count, buf);
	if (!buffer)
		goto out;

	res = -EINVAL;
	if (strncmp(buffer, "in ", 3) != 0)
		goto out;

	path = buffer + 3;
	while (*path && isspace((u8)*path))
		path++;
	path_end = path;
	while (*path_end && !isspace((u8)*path_end))
		path_end++;
	*path_end++ = '\0';
	cmd = path_end;
	while (*cmd && isspace((u8)*cmd))
		cmd++;

	res = scst_suspend_activity(true);
	if (res)
		goto out;

	res = mutex_lock_interruptible(&scst_mutex);
	if (res)
		goto out_resume;

	mgmt_path_type = __parse_path(path, &devt, &dev, &tgtt, &tgt, &acg,
				      &dg, &tg);
	mutex_unlock(&scst_mutex);

	res = -EINVAL;
	switch (mgmt_path_type) {
	case DEVICE_PATH:
		res = scst_process_dev_mgmt_store(cmd, dev);
		break;
	case DEVICE_TYPE_PATH:
		if (devt->add_device)
			res = scst_process_devt_mgmt_store(cmd, devt);
		else
			res = scst_process_devt_pass_through_mgmt_store(cmd,
									devt);
		break;
	case TARGET_TEMPLATE_PATH:
		res = scst_process_tgtt_mgmt_store(cmd, tgtt);
		break;
	case TARGET_PATH:
		res = scst_process_tgt_mgmt_store(cmd, tgt);
		break;
	case TARGET_LUNS_PATH:
		res = __scst_process_luns_mgmt_store(cmd, tgt, tgt->default_acg,
						     true);
		break;
	case TARGET_INI_GROUPS_PATH:
		res = scst_process_ini_group_mgmt_store(cmd, tgt);
		break;
	case ACG_PATH:
		res = scst_process_acg_mgmt_store(cmd, acg);
		break;
	case ACG_LUNS_PATH:
		res = __scst_process_luns_mgmt_store(cmd, acg->tgt, acg, false);
		break;
	case ACG_INITIATOR_GROUPS_PATH:
		res = scst_process_acg_ini_mgmt_store(cmd, acg->tgt, acg);
		break;
	case DGS_PATH:
		res = scst_device_groups_mgmt_store_work_fn(cmd);
		break;
	case DGS_DEVS_PATH:
		res = scst_dg_devs_mgmt_store_work_fn(cmd, dg);
		break;
	case TGS_PATH:
		res = scst_dg_tgs_mgmt_store_work_fn(cmd, dg);
		break;
	case TGS_TG_PATH:
		res = scst_tg_mgmt_store_work_fn(cmd, tg);
		break;
	case PATH_NOT_RECOGNIZED:
		break;
	}

out_resume:
	scst_resume_activity();
out:
	kfree(buffer);
	if (res == 0)
		res = count;
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_threads_show(struct device *device,
				 struct device_attribute *attr, char *buf)
{
	int count;

	TRACE_ENTRY();

	count = sprintf(buf, "%d\n", scst_main_cmd_threads.nr_threads);

	TRACE_EXIT();
	return count;
}

static int scst_process_threads_store(int newtn)
{
	int res;
	long oldtn, delta;

	TRACE_ENTRY();

	TRACE_DBG("newtn %d", newtn);

	res = mutex_lock_interruptible(&scst_mutex);
	if (res != 0)
		goto out;

	oldtn = scst_main_cmd_threads.nr_threads;

	delta = newtn - oldtn;
	if (delta < 0)
		scst_del_threads(&scst_main_cmd_threads, -delta);
	else {
		res = scst_add_threads(&scst_main_cmd_threads, NULL, NULL, delta);
		if (res != 0)
			goto out_up;
	}

	PRINT_INFO("Changed cmd threads num: old %ld, new %d", oldtn, newtn);

out_up:
	mutex_unlock(&scst_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_threads_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int res;
	long newtn;

	TRACE_ENTRY();

	res = strict_strtol(buf, 0, &newtn);
	if (res != 0) {
		PRINT_ERROR("strict_strtol() for %s failed: %d ", buf, res);
		goto out;
	}
	if (newtn <= 0) {
		PRINT_ERROR("Illegal threads num value %ld", newtn);
		res = -EINVAL;
		goto out;
	}

	res = scst_process_threads_store(newtn);
	if (res == 0)
		res = count;
out:
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_setup_id_show(struct device *device,
				  struct device_attribute *attr, char *buf)
{
	int count;

	TRACE_ENTRY();

	count = sprintf(buf, "0x%x\n", scst_setup_id);

	TRACE_EXIT();
	return count;
}

static ssize_t scst_setup_id_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int res;
	unsigned long val;

	TRACE_ENTRY();

	res = strict_strtoul(buf, 0, &val);
	if (res != 0) {
		PRINT_ERROR("strict_strtoul() for %s failed: %d ", buf, res);
		goto out;
	}

	scst_setup_id = val;
	PRINT_INFO("Changed scst_setup_id to %x", scst_setup_id);

	res = count;

out:
	TRACE_EXIT_RES(res);
	return res;
}

static ssize_t scst_max_tasklet_cmd_show(struct device *device,
				  struct device_attribute *attr, char *buf)
{
	int count;

	TRACE_ENTRY();

	count = sprintf(buf, "%d\n", scst_max_tasklet_cmd);

	TRACE_EXIT();
	return count;
}

static ssize_t scst_max_tasklet_cmd_store(struct device *device,
	struct device_attribute *attr, const char *buf, size_t count)
{
	int res;
	unsigned long val;

	TRACE_ENTRY();

	res = strict_strtoul(buf, 0, &val);
	if (res != 0) {
		PRINT_ERROR("strict_strtoul() for %s failed: %d ", buf, res);
		goto out;
	}

	scst_max_tasklet_cmd = val;
	PRINT_INFO("Changed scst_max_tasklet_cmd to %d", scst_max_tasklet_cmd);

	res = count;

out:
	TRACE_EXIT_RES(res);
	return res;
}

static struct device_attribute scst_max_tasklet_cmd_attr =
	__ATTR(max_tasklet_cmd, S_IRUGO | S_IWUSR, scst_max_tasklet_cmd_show,
	       scst_max_tasklet_cmd_store);

static ssize_t scst_version_show(struct device *device,
				 struct device_attribute *attr, char *buf)
{
	return scnprintf(buf, PAGE_SIZE, "%s\n", SCST_VERSION_STRING);
}


static struct device_attribute scst_mgmt_attr =
	__ATTR(mgmt, S_IRUGO | S_IWUSR, scst_mgmt_show, scst_mgmt_store);

static struct device_attribute scst_threads_attr =
	__ATTR(threads, S_IRUGO | S_IWUSR, scst_threads_show,
	       scst_threads_store);

static struct device_attribute scst_setup_id_attr =
	__ATTR(setup_id, S_IRUGO | S_IWUSR, scst_setup_id_show,
	       scst_setup_id_store);

static struct device_attribute scst_version_attr =
	__ATTR(version, S_IRUGO, scst_version_show, NULL);

#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 34)
static struct device_attribute *scst_root_default_attrs[] = {
#else
static const struct device_attribute *scst_root_default_attrs[] = {
#endif
	&scst_mgmt_attr,
	&scst_threads_attr,
	&scst_setup_id_attr,
	&scst_max_tasklet_cmd_attr,
	&scst_version_attr,
	NULL
};

static void scst_release_device(struct device *device)
{
	TRACE_ENTRY();
	kfree(device);
	TRACE_EXIT();
}

/**
 ** Sysfs user info
 **/

static DEFINE_MUTEX(scst_sysfs_user_info_mutex);

/* All protected by scst_sysfs_user_info_mutex */
static LIST_HEAD(scst_sysfs_user_info_list);
static uint32_t scst_sysfs_info_cur_cookie;

/* scst_sysfs_user_info_mutex supposed to be held */
static struct scst_sysfs_user_info *scst_sysfs_user_find_info(uint32_t cookie)
{
	struct scst_sysfs_user_info *info, *res = NULL;

	TRACE_ENTRY();

	list_for_each_entry(info, &scst_sysfs_user_info_list,
			info_list_entry) {
		if (info->info_cookie == cookie) {
			res = info;
			break;
		}
	}

	TRACE_EXIT_HRES(res);
	return res;
}

/**
 * scst_sysfs_user_get_info() - get user_info
 *
 * Finds the user_info based on cookie and mark it as received the reply by
 * setting for it flag info_being_executed.
 *
 * Returns found entry or NULL.
 */
struct scst_sysfs_user_info *scst_sysfs_user_get_info(uint32_t cookie)
{
	struct scst_sysfs_user_info *res = NULL;

	TRACE_ENTRY();

	mutex_lock(&scst_sysfs_user_info_mutex);

	res = scst_sysfs_user_find_info(cookie);
	if (res != NULL) {
		if (!res->info_being_executed)
			res->info_being_executed = 1;
	}

	mutex_unlock(&scst_sysfs_user_info_mutex);

	TRACE_EXIT_HRES(res);
	return res;
}
EXPORT_SYMBOL_GPL(scst_sysfs_user_get_info);

/**
 ** Helper functionality to help target drivers and dev handlers support
 ** sending events to user space and wait for their completion in a safe
 ** manner. See samples how to use it in iscsi-scst or scst_user.
 **/

/**
 * scst_sysfs_user_add_info() - create and add user_info in the global list
 *
 * Creates an info structure and adds it in the info_list.
 * Returns 0 and out_info on success, error code otherwise.
 */
int scst_sysfs_user_add_info(struct scst_sysfs_user_info **out_info)
{
	int res = 0;
	struct scst_sysfs_user_info *info;

	TRACE_ENTRY();

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (info == NULL) {
		PRINT_ERROR("Unable to allocate sysfs user info (size %zd)",
			sizeof(*info));
		res = -ENOMEM;
		goto out;
	}

	mutex_lock(&scst_sysfs_user_info_mutex);

	while ((info->info_cookie == 0) ||
	       (scst_sysfs_user_find_info(info->info_cookie) != NULL))
		info->info_cookie = scst_sysfs_info_cur_cookie++;

	init_completion(&info->info_completion);

	list_add_tail(&info->info_list_entry, &scst_sysfs_user_info_list);
	info->info_in_list = 1;

	*out_info = info;

	mutex_unlock(&scst_sysfs_user_info_mutex);

out:
	TRACE_EXIT_RES(res);
	return res;
}
EXPORT_SYMBOL_GPL(scst_sysfs_user_add_info);

/**
 * scst_sysfs_user_del_info - delete and frees user_info
 */
void scst_sysfs_user_del_info(struct scst_sysfs_user_info *info)
{
	TRACE_ENTRY();

	mutex_lock(&scst_sysfs_user_info_mutex);

	if (info->info_in_list)
		list_del(&info->info_list_entry);

	mutex_unlock(&scst_sysfs_user_info_mutex);

	kfree(info);

	TRACE_EXIT();
	return;
}
EXPORT_SYMBOL_GPL(scst_sysfs_user_del_info);

/*
 * Returns true if the reply received and being processed by another part of
 * the kernel, false otherwise. Also removes the user_info from the list to
 * fix for the user space that it missed the timeout.
 */
static bool scst_sysfs_user_info_executing(struct scst_sysfs_user_info *info)
{
	bool res;

	TRACE_ENTRY();

	mutex_lock(&scst_sysfs_user_info_mutex);

	res = info->info_being_executed;

	if (info->info_in_list) {
		list_del(&info->info_list_entry);
		info->info_in_list = 0;
	}

	mutex_unlock(&scst_sysfs_user_info_mutex);

	TRACE_EXIT_RES(res);
	return res;
}

/**
 * scst_wait_info_completion() - wait an user space event's completion
 *
 * Waits for the info request been completed by user space at most timeout
 * jiffies. If the reply received before timeout and being processed by
 * another part of the kernel, i.e. scst_sysfs_user_info_executing()
 * returned true, waits for it to complete indefinitely.
 *
 * Returns status of the request completion.
 */
int scst_wait_info_completion(struct scst_sysfs_user_info *info,
	unsigned long timeout)
{
	int res, rc;

	TRACE_ENTRY();

	TRACE_DBG("Waiting for info %p completion", info);

	while (1) {
		rc = wait_for_completion_interruptible_timeout(
			&info->info_completion, timeout);
		if (rc > 0) {
			TRACE_DBG("Waiting for info %p finished with %d",
				info, rc);
			break;
		} else if (rc == 0) {
			if (!scst_sysfs_user_info_executing(info)) {
				PRINT_ERROR("Timeout waiting for user "
					"space event %p", info);
				res = -EBUSY;
				goto out;
			} else {
				/* Req is being executed in the kernel */
				TRACE_DBG("Keep waiting for info %p completion",
					info);
				wait_for_completion(&info->info_completion);
				break;
			}
		} else if (rc != -ERESTARTSYS) {
				res = rc;
				PRINT_ERROR("wait_for_completion() failed: %d",
					res);
				goto out;
		} else {
			TRACE_DBG("Waiting for info %p finished with %d, "
				"retrying", info, rc);
		}
	}

	TRACE_DBG("info %p, status %d", info, info->info_status);
	res = info->info_status;

out:
	TRACE_EXIT_RES(res);
	return res;
}
EXPORT_SYMBOL_GPL(scst_wait_info_completion);

static int scst_target_bus_match(struct device *dev, struct device_driver *drv)
{
	struct scst_tgt *tgt = scst_dev_to_tgt(dev);
	struct scst_tgt_template *tgtt = scst_drv_to_tgtt(drv);
	int res;

	TRACE_ENTRY();

#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
	lockdep_assert_held(&scst_mutex);
#endif

	res = __scst_lookup_tgtt(drv->name) == tgtt
		&& __scst_lookup_tgt(tgtt, dev_name(dev)) == tgt;

	TRACE_EXIT_RES(res);
	return res;
}

static struct bus_type scst_target_bus = {
	.name = "scst_target",
	.match = scst_target_bus_match,
};

static struct device *scst_device;

int __init scst_sysfs_init(void)
{
	int res;

	TRACE_ENTRY();

	res = scst_debugfs_init();
	if (res)
		goto out;

	res = bus_register(&scst_target_bus);
	if (res)
		goto out_cleanup_debugfs;

	res = bus_register(&scst_device_bus);
	if (res != 0)
		goto out_unregister_target_bus;

	res = -ENOMEM;
	scst_device = kzalloc(sizeof *scst_device, GFP_KERNEL);
	if (!scst_device) {
		PRINT_ERROR("%s", "Allocating memory for SCST device failed.");
		goto out_unregister_device_bus;
	}

	scst_device->release = scst_release_device;
#if LINUX_VERSION_CODE < KERNEL_VERSION(2, 6, 30)
	snprintf(scst_device->bus_id, BUS_ID_SIZE, "%s", "scst");
#else
	dev_set_name(scst_device, "%s", "scst");
#endif
	res = device_register(scst_device);
	if (res) {
		PRINT_ERROR("Registration of SCST device failed (%d).", res);
		goto out_free;
	}

	res = device_create_files(scst_device, scst_root_default_attrs);
	if (res) {
		PRINT_ERROR("%s", "Creating SCST device attributes failed.");
		goto out_unregister_device;
	}

	scst_device_groups_kobj = kobject_create_and_add("device_groups",
							 &scst_device->kobj);
	if (scst_device_groups_kobj == NULL)
		goto device_groups_kobj_error;

	if (sysfs_create_files(scst_device_groups_kobj,
			       scst_device_groups_attrs))
		goto device_groups_attrs_error;

	res = scst_main_create_debugfs_dir();
	if (res) {
		PRINT_ERROR("%s", "Creating SCST trace files failed.");
		goto out_remove_dg_files;
	}

	res = scst_main_create_debugfs_files(scst_get_main_debugfs_dir());
	if (res)
		goto out_remove_debugfs_dir;

out:
	TRACE_EXIT_RES(res);
	return res;

out_remove_debugfs_dir:
	scst_main_remove_debugfs_dir();
out_remove_dg_files:
	sysfs_remove_files(scst_device_groups_kobj, scst_device_groups_attrs);
device_groups_attrs_error:
	kobject_del(scst_device_groups_kobj);
	kobject_put(scst_device_groups_kobj);
device_groups_kobj_error:
	device_remove_files(scst_device, scst_root_default_attrs);
out_unregister_device:
	device_unregister(scst_device);
	scst_device = NULL;
out_free:
	kfree(scst_device);
out_unregister_device_bus:
	bus_unregister(&scst_device_bus);
out_unregister_target_bus:
	bus_unregister(&scst_target_bus);
out_cleanup_debugfs:
	scst_debugfs_cleanup();
	goto out;
}

void scst_sysfs_cleanup(void)
{
	TRACE_ENTRY();

	PRINT_INFO("%s", "Exiting SCST sysfs hierarchy...");

	scst_main_remove_debugfs_files(scst_get_main_debugfs_dir());
	scst_main_remove_debugfs_dir();

	sysfs_remove_files(scst_device_groups_kobj, scst_device_groups_attrs);
	kobject_del(scst_device_groups_kobj);
	kobject_put(scst_device_groups_kobj);

	device_remove_files(scst_device, scst_root_default_attrs);

	device_unregister(scst_device);

	bus_unregister(&scst_device_bus);

	bus_unregister(&scst_target_bus);

	scst_debugfs_cleanup();

	/*
	 * Wait until the release method of the sysfs root object has returned.
	 */
	msleep(20);

	PRINT_INFO("%s", "Exiting SCST sysfs hierarchy done");

	TRACE_EXIT();
}