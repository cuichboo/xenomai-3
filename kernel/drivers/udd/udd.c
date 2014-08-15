/*
 * This file is part of the Xenomai project.
 *
 * Copyright (C) 2014 Philippe Gerum <rpm@xenomai.org>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <rtdm/cobalt.h>
#include <rtdm/driver.h>
#include <rtdm/udd.h>

struct udd_context {
	u32 event_count;
};

static int udd_open(struct rtdm_fd *fd, int oflags)
{
	struct udd_context *context;
	struct udd_device *udd;
	int ret;

	udd = container_of(rtdm_fd_device(fd), struct udd_device, __reserved.device);
	if (udd->ops.open) {
		ret = udd->ops.open(udd, oflags);
		if (ret)
			return ret;
	}

	context = rtdm_fd_to_private(fd);
	context->event_count = 0;

	return 0;
}

static void udd_close(struct rtdm_fd *fd)
{
	struct udd_device *udd;

	udd = container_of(rtdm_fd_device(fd), struct udd_device, __reserved.device);
	if (udd->ops.close)
		udd->ops.close(udd);
}

static int udd_ioctl_rt(struct rtdm_fd *fd,
			unsigned int request, void __user *arg)
{
	struct udd_signotify signfy;
	struct udd_reserved *ur;
	struct udd_device *udd;
	int ret = 0;

	udd = container_of(rtdm_fd_device(fd), struct udd_device, __reserved.device);
	if (udd->ops.ioctl) {
		ret = udd->ops.ioctl(udd, request, arg);
		if (ret != -ENOSYS)
			return ret;
	}

	ur = &udd->__reserved;

	switch (request) {
	case UDD_RTIOC_IRQSIG:
		ret = rtdm_safe_copy_from_user(fd, &signfy, arg, sizeof(signfy));
		if (ret)
			return ret;
		/* Early check, we'll redo at each signal issue. */
		if (signfy.pid <= 0)
			ur->signfy.pid = -1;
		else {
			if (signfy.sig < SIGRTMIN || signfy.sig > SIGRTMAX)
				return -EINVAL;
			if (cobalt_thread_find_local(signfy.pid) == NULL)
				return -EINVAL;
			ur->signfy = signfy;
		}
		break;
	case UDD_RTIOC_IRQEN:
		if (udd->irq == UDD_IRQ_NONE)
			return -EIO;
		udd_post_irq_enable(udd->irq);
		break;
	case UDD_RTIOC_IRQDIS:
		if (udd->irq == UDD_IRQ_NONE)
			return -EIO;
		udd_post_irq_disable(udd->irq);
		break;
	}

	return ret;
}

static ssize_t udd_read_rt(struct rtdm_fd *fd,
			   void __user *buf, size_t len)
{
	struct udd_context *context;
	struct udd_reserved *ur;
	struct udd_device *udd;
	ssize_t ret;
	u32 count;

	if (len != sizeof(count))
		return -EINVAL;

	udd = container_of(rtdm_fd_device(fd), struct udd_device, __reserved.device);
	if (udd->irq == UDD_IRQ_NONE)
		return -EIO;

	ur = &udd->__reserved;
	context = rtdm_fd_to_private(fd);

	for (;;) {
		if (atomic_read(&ur->event) != context->event_count)
			break;
		ret = rtdm_event_wait(&ur->pulse);
		if (ret)
			return ret;
	}

	count = atomic_read(&ur->event);
	context->event_count = count;
	ret = rtdm_copy_to_user(fd, buf, &count, sizeof(count));

	return ret ?: sizeof(count);
}

static ssize_t udd_write_rt(struct rtdm_fd *fd,
			    const void __user *buf, size_t len)
{
	int ret;
	u32 val;

	if (len != sizeof(val))
		return -EINVAL;

	ret = rtdm_safe_copy_from_user(fd, &val, buf, sizeof(val));
	if (ret)
		return ret;

	ret = udd_ioctl_rt(fd, val ? UDD_RTIOC_IRQEN : UDD_RTIOC_IRQDIS, NULL);

	return ret ?: len;
}

static int udd_select(struct rtdm_fd *fd, struct xnselector *selector,
		      unsigned int type, unsigned int index)
{
	struct udd_device *udd;

	udd = container_of(rtdm_fd_device(fd), struct udd_device, __reserved.device);
	if (udd->irq == UDD_IRQ_NONE)
		return -EIO;

	return rtdm_event_select(&udd->__reserved.pulse,
				 selector, type, index);
}

static int udd_irq_handler(rtdm_irq_t *irqh)
{
	struct udd_device *udd;
	int ret;

	/*
	 * CAUTION: irqh might live outside of the udd_device struct
	 * (i.e. UDD_IRQ_CUSTOM), so we can't assume the latter is
	 * the container of the former.
	 */
	udd = rtdm_irq_get_arg(irqh, struct udd_device);
	ret = udd->ops.interrupt(udd);
	if (ret == RTDM_IRQ_HANDLED)
		udd_notify_event(udd);

	return ret;
}

static int mapper_open(struct rtdm_fd *fd, int oflags)
{
	int minor = rtdm_fd_minor(fd);
	struct udd_device *udd;

	/*
	 * Check that we are opening a mapper instance pointing at a
	 * valid memory region. e.g. UDD creates the companion device
	 * "foo,mapper" on the fly when registering the main device
	 * "foo". Userland may then open("/dev/foo,mapper@0", ...)
	 * followed by a call to mmap() for mapping the memory region
	 * #0 as declared in the mem_regions[] array of the main
	 * device.
	 *
	 * We support sparse region arrays, so the device minor shall
	 * match the mem_regions[] index exactly.
	 */
	if (minor < 0 || minor >= UDD_NR_MAPS)
		return -EIO;

	udd = container_of(rtdm_fd_device(fd), struct udd_device, __reserved.mapper);
	if (udd->mem_regions[minor].type == UDD_MEM_NONE)
		return -EIO;

	return 0;
}

static void mapper_close(struct rtdm_fd *fd)
{
	/* nop */
}

static int mapper_mmap(struct rtdm_fd *fd, struct vm_area_struct *vma)
{
	struct udd_memregion *rn;
	struct udd_device *udd;
	size_t len;
	int ret;

	udd = container_of(rtdm_fd_device(fd), struct udd_device, __reserved.mapper);
	if (udd->ops.mmap)
		/* Offload to client driver if handler is present. */
		return udd->ops.mmap(udd, vma);

	/* Otherwise DIY using the RTDM helpers. */

	len = vma->vm_end - vma->vm_start;
	rn = udd->mem_regions + rtdm_fd_minor(fd);
	if (rn->len < len)
		/* Can't map that much, bail out. */
		return -EINVAL;

	switch (rn->type) {
	case UDD_MEM_PHYS:
		ret = rtdm_mmap_iomem(vma, rn->addr);
		break;
	case UDD_MEM_LOGICAL:
		ret = rtdm_mmap_kmem(vma, (void *)rn->addr);
		break;
	case UDD_MEM_VIRTUAL:
		ret = rtdm_mmap_vmem(vma, (void *)rn->addr);
		break;
	default:
		ret = -EINVAL;	/* Paranoid, can't happen. */
	}

	return ret;
}

void udd_notify_event(struct udd_device *udd)
{
	struct udd_reserved *ur = &udd->__reserved;
	union sigval sival;

	atomic_inc(&ur->event);
	rtdm_event_signal(&ur->pulse);

	if (ur->signfy.pid > 0) {
		sival.sival_int = atomic_read(&ur->event);
		cobalt_sigqueue(ur->signfy.pid, ur->signfy.sig, &sival);
	}
}
EXPORT_SYMBOL_GPL(udd_notify_event);

static inline int check_memregion(struct udd_device *udd,
				  struct udd_memregion *rn)
{
	/* We allow sparse region arrays. */
	if (rn->type == UDD_MEM_NONE)
		return 0;

	if (rn->name == NULL)
		return -EINVAL;

	if (rn->addr == 0)
		return -EINVAL;

	if (rn->len == 0)
		return -EINVAL;

	udd->__reserved.nr_maps++;

	return 0;
}

static inline int register_mapper(struct udd_device *udd)
{
	struct rtdm_device *dev = &udd->__reserved.mapper;
	struct udd_reserved *ur = &udd->__reserved;

	ur->mapper_name = kasformat("%s,mapper", udd->device_name);
	if (ur->mapper_name == NULL)
		return -ENOMEM;

	memset(dev, 0, sizeof(*dev));
	dev->struct_version = RTDM_DEVICE_STRUCT_VER;
	dev->device_flags = RTDM_NAMED_DEVICE;
	dev->context_size = 0;
	dev->ops = (struct rtdm_fd_ops){
		.open = mapper_open,
		.close = mapper_close,
		.mmap = mapper_mmap,
	};
	dev->device_class = RTDM_CLASS_MEMORY;
	dev->device_sub_class = RTDM_SUBCLASS_GENERIC;
	knamecpy(dev->device_name, ur->mapper_name);
	dev->driver_name = "mapper";
	dev->driver_version = RTDM_DRIVER_VER(1, 0, 0);
	dev->peripheral_name = "UDD mapper";
	dev->proc_name = ur->mapper_name;
	dev->provider_name = "Philippe Gerum <rpm@xenomai.org>";

	return rtdm_dev_register(dev);
}

int udd_register_device(struct udd_device *udd)
{
	struct rtdm_device *dev = &udd->__reserved.device;
	struct udd_reserved *ur = &udd->__reserved;
	int ret, n;

	if (!realtime_core_enabled())
		return -ENXIO;

	if (udd->irq != UDD_IRQ_NONE && udd->ops.interrupt == NULL)
		return -EINVAL;

	for (n = 0, ur->nr_maps = 0; n < UDD_NR_MAPS; n++) {
		ret = check_memregion(udd, udd->mem_regions + n);
		if (ret)
			return ret;
	}

	memset(dev, 0, sizeof(*dev));
	dev->struct_version = RTDM_DEVICE_STRUCT_VER;
	dev->device_flags = RTDM_NAMED_DEVICE;
	dev->context_size = sizeof(struct udd_context);
	dev->ops = (struct rtdm_fd_ops){
		.open = udd_open,
		.ioctl_rt = udd_ioctl_rt,
		.read_rt = udd_read_rt,
		.write_rt = udd_write_rt,
		.close = udd_close,
		.select = udd_select,
	};
	dev->device_class = RTDM_CLASS_UDD;
	dev->device_sub_class = udd->device_subclass;
	knamecpy(dev->device_name, udd->device_name);
	dev->driver_name = "udd";
	dev->driver_version = udd->driver_version;
	dev->peripheral_name = udd->device_description;
	dev->proc_name = udd->device_name;
	dev->provider_name = udd->driver_author;

	if (ur->nr_maps > 0) {
		ret = register_mapper(udd);
		if (ret)
			return ret;
	} else
		ur->mapper_name = NULL;

	atomic_set(&ur->event, 0);
	rtdm_event_init(&ur->pulse, 0);
	ur->signfy.pid = -1;

	if (udd->irq != UDD_IRQ_NONE && udd->irq != UDD_IRQ_CUSTOM) {
		ret = rtdm_irq_request(&ur->irqh, udd->irq,
				       udd_irq_handler, 0,
				       dev->device_name, udd);
		if (ret)
			goto fail_irq_request;
	}

	ret = rtdm_dev_register(&ur->device);
	if (ret)
		goto fail_dev_register;

	return 0;

fail_dev_register:
	rtdm_irq_free(&ur->irqh);
fail_irq_request:
	rtdm_dev_unregister(&ur->mapper, 0);
	kfree(ur->mapper_name);

	return ret;
}
EXPORT_SYMBOL_GPL(udd_register_device);

int udd_unregister_device(struct udd_device *udd,
			  unsigned int poll_delay)
{
	struct udd_reserved *ur = &udd->__reserved;

	if (!realtime_core_enabled())
		return -ENXIO;

	rtdm_event_destroy(&ur->pulse);

	if (udd->irq != UDD_IRQ_NONE && udd->irq != UDD_IRQ_CUSTOM)
		rtdm_irq_free(&ur->irqh);

	rtdm_dev_unregister(&ur->mapper, poll_delay);

	if (ur->mapper_name)
		kfree(ur->mapper_name);

	return rtdm_dev_unregister(&ur->device, poll_delay);
}
EXPORT_SYMBOL_GPL(udd_unregister_device);

struct irqswitch_work {
	struct ipipe_work_header work; /* Must be first. */
	int irq;
	int enabled;
};

static void lostage_irqswitch_line(struct ipipe_work_header *work)
{
	struct irqswitch_work *rq;

	/*
	 * This runs from secondary mode, we may flip the IRQ state
	 * now.
	 */
	rq = container_of(work, struct irqswitch_work, work);
	if (rq->enabled)
		ipipe_enable_irq(rq->irq);
	else
		ipipe_disable_irq(rq->irq);
}

static void switch_irq_line(int irq, int enable)
{
	struct irqswitch_work switchwork = {
		.work = {
			.size = sizeof(switchwork),
			.handler = lostage_irqswitch_line,
		},
		.irq = irq,
		.enabled = enable,
	};

	/*
	 * Not pretty, but we may not traverse the kernel code for
	 * enabling/disabling IRQ lines from primary mode. So we have
	 * to send a deferrable root request (i.e. low-level APC) to
	 * be callable from real-time context.
	 */
	ipipe_post_work_root(&switchwork, work);
}

void udd_post_irq_enable(int irq)
{
	switch_irq_line(irq, 1);
}
EXPORT_SYMBOL_GPL(udd_post_irq_enable);

void udd_post_irq_disable(int irq)
{
	switch_irq_line(irq, 0);
}
EXPORT_SYMBOL_GPL(udd_post_irq_disable);

MODULE_LICENSE("GPL");