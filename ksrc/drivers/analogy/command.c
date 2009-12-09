/**
 * @file
 * Analogy for Linux, command related features
 *
 * Copyright (C) 1997-2000 David A. Schleef <ds@schleef.org>
 * Copyright (C) 2008 Alexis Berlemont <alexis.berlemont@free.fr>
 *
 * Xenomai is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifndef DOXYGEN_CPP

#include <linux/module.h>
#include <linux/ioport.h>
#include <linux/mman.h>
#include <asm/io.h>
#include <asm/errno.h>

#include <analogy/context.h>
#include <analogy/device.h>

/* --- Command descriptor management functions --- */

int a4l_fill_cmddesc(a4l_cxt_t * cxt, a4l_cmd_t * desc, void *arg)
{
	int ret = 0;
	unsigned int *tmpchans = NULL;

	ret = rtdm_safe_copy_from_user(cxt->user_info, 
				       desc, arg, sizeof(a4l_cmd_t));
	if (ret != 0)
		goto out_cmddesc;

	if (desc->nb_chan == 0) {
		ret = -EINVAL;
		goto out_cmddesc;
	}

	tmpchans = rtdm_malloc(desc->nb_chan * sizeof(unsigned int));
	if (tmpchans == NULL) {
		ret = -ENOMEM;
		goto out_cmddesc;
	}

	ret = rtdm_safe_copy_from_user(cxt->user_info,
				       tmpchans,
				       desc->chan_descs,
				       desc->nb_chan * sizeof(unsigned long));
	if (ret != 0)
		goto out_cmddesc;

	desc->chan_descs = tmpchans;

	__a4l_dbg(1, core_dbg, "a4l_fill_cmddesc: desc dump\n");
	__a4l_dbg(1, core_dbg, "\t->idx_subd=%u\n", desc->idx_subd);
	__a4l_dbg(1, core_dbg, "\t->flags=%lu\n", desc->flags);
	__a4l_dbg(1, core_dbg, "\t->nb_chan=%u\n", desc->nb_chan);
	__a4l_dbg(1, core_dbg, "\t->chan_descs=0x%x\n", *desc->chan_descs);
	__a4l_dbg(1, core_dbg, "\t->data_len=%u\n", desc->data_len);
	__a4l_dbg(1, core_dbg, "\t->pdata=0x%p\n", desc->data);

      out_cmddesc:

	if (ret != 0) {
		if (tmpchans != NULL)
			rtdm_free(tmpchans);
		desc->chan_descs = NULL;
	}

	return ret;
}

void a4l_free_cmddesc(a4l_cmd_t * desc)
{
	if (desc->chan_descs != NULL)
		rtdm_free(desc->chan_descs);
}

int a4l_check_cmddesc(a4l_cxt_t * cxt, a4l_cmd_t * desc)
{
	int ret = 0;
	a4l_dev_t *dev = a4l_get_dev(cxt);

	__a4l_dbg(1, core_dbg, 
		     "a4l_check_cmddesc: minor=%d\n", a4l_get_minor(cxt));

	if (desc->idx_subd >= dev->transfer.nb_subd) {
		__a4l_err("a4l_check_cmddesc: "
			  "subdevice index out of range (%u >= %u)\n",
			  desc->idx_subd, dev->transfer.nb_subd);
		return -EINVAL;
	}

	if (dev->transfer.subds[desc->idx_subd]->flags & A4L_SUBD_UNUSED) {
		__a4l_err("a4l_check_cmddesc: "
			  "subdevice type incoherent\n");
		return -EIO;
	}

	if (!(dev->transfer.subds[desc->idx_subd]->flags & A4L_SUBD_CMD)) {
		__a4l_err("a4l_check_cmddesc: operation not supported, "
			  "synchronous only subdevice\n");
		return -EIO;
	}

	if (test_bit(A4L_TSF_BUSY, &(dev->transfer.status[desc->idx_subd])))
		return -EBUSY;

	if (ret != 0) {
		__a4l_err("a4l_check_cmddesc: subdevice busy\n");
		return ret;
	}

	return a4l_check_chanlist(dev->transfer.subds[desc->idx_subd],
				     desc->nb_chan, desc->chan_descs);
}

/* --- Command checking functions --- */

int a4l_check_generic_cmdcnt(a4l_cmd_t * desc)
{
	unsigned int tmp1, tmp2;

	/* Makes sure trigger sources are trivially valid */
	tmp1 =
	    desc->start_src & ~(TRIG_NOW | TRIG_INT | TRIG_EXT | TRIG_FOLLOW);
	tmp2 = desc->start_src & (TRIG_NOW | TRIG_INT | TRIG_EXT | TRIG_FOLLOW);
	if (tmp1 != 0 || tmp2 == 0) {
		__a4l_err("a4l_check_cmddesc: start_src, weird trigger\n");
		return -EINVAL;
	}

	tmp1 = desc->scan_begin_src & ~(TRIG_TIMER | TRIG_EXT | TRIG_FOLLOW);
	tmp2 = desc->scan_begin_src & (TRIG_TIMER | TRIG_EXT | TRIG_FOLLOW);
	if (tmp1 != 0 || tmp2 == 0) {
		__a4l_err("a4l_check_cmddesc: scan_begin_src, , weird trigger\n");
		return -EINVAL;
	}

	tmp1 = desc->convert_src & ~(TRIG_TIMER | TRIG_EXT | TRIG_NOW);
	tmp2 = desc->convert_src & (TRIG_TIMER | TRIG_EXT | TRIG_NOW);
	if (tmp1 != 0 || tmp2 == 0) {
		__a4l_err("a4l_check_cmddesc: convert_src, weird trigger\n");
		return -EINVAL;
	}

	tmp1 = desc->scan_end_src & ~(TRIG_COUNT);
	if (tmp1 != 0) {
		__a4l_err("a4l_check_cmddesc: scan_end_src, weird trigger\n");
		return -EINVAL;
	}

	tmp1 = desc->stop_src & ~(TRIG_COUNT | TRIG_NONE);
	tmp2 = desc->stop_src & (TRIG_COUNT | TRIG_NONE);
	if (tmp1 != 0 || tmp2 == 0) {
		__a4l_err("a4l_check_cmddesc: stop_src, weird trigger\n");
		return -EINVAL;
	}

	/* Makes sure trigger sources are unique */
	if (desc->start_src != TRIG_NOW &&
	    desc->start_src != TRIG_INT &&
	    desc->start_src != TRIG_EXT && desc->start_src != TRIG_FOLLOW) {
		__a4l_err("a4l_check_cmddesc: start_src, "
			  "only one trigger should be set\n");
		return -EINVAL;
	}

	if (desc->scan_begin_src != TRIG_TIMER &&
	    desc->scan_begin_src != TRIG_EXT &&
	    desc->scan_begin_src != TRIG_FOLLOW) {
		__a4l_err("a4l_check_cmddesc: scan_begin_src, "
			  "only one trigger should be set\n");
		return -EINVAL;
	}

	if (desc->convert_src != TRIG_TIMER &&
	    desc->convert_src != TRIG_EXT && desc->convert_src != TRIG_NOW) {
		__a4l_err("a4l_check_cmddesc: convert_src, "
			  "only one trigger should be set\n");
		return -EINVAL;
	}

	if (desc->stop_src != TRIG_COUNT && desc->stop_src != TRIG_NONE) {
		__a4l_err("a4l_check_cmddesc: stop_src, "
			  "only one trigger should be set\n");
		return -EINVAL;
	}

	/* Makes sure arguments are trivially compatible */
	tmp1 = desc->start_src & (TRIG_NOW | TRIG_FOLLOW | TRIG_INT);
	tmp2 = desc->start_arg;
	if (tmp1 != 0 && tmp2 != 0) {
		__a4l_err("a4l_check_cmddesc: no start_arg expected\n");
		return -EINVAL;
	}

	tmp1 = desc->scan_begin_src & TRIG_FOLLOW;
	tmp2 = desc->scan_begin_arg;
	if (tmp1 != 0 && tmp2 != 0) {
		__a4l_err("a4l_check_cmddesc: no scan_begin_arg expected\n");
		return -EINVAL;
	}

	tmp1 = desc->convert_src & TRIG_NOW;
	tmp2 = desc->convert_arg;
	if (tmp1 != 0 && tmp2 != 0) {
		__a4l_err("a4l_check_cmddesc: no convert_arg expected\n");
		return -EINVAL;
	}

	tmp1 = desc->stop_src & TRIG_NONE;
	tmp2 = desc->stop_arg;
	if (tmp1 != 0 && tmp2 != 0) {
		__a4l_err("a4l_check_cmddesc: no stop_arg expected\n");
		return -EINVAL;
	}

	return 0;
}

int a4l_check_specific_cmdcnt(a4l_cxt_t * cxt, a4l_cmd_t * desc)
{
	unsigned int tmp1, tmp2;
	a4l_dev_t *dev = a4l_get_dev(cxt);
	a4l_cmd_t *cmd_mask = dev->transfer.subds[desc->idx_subd]->cmd_mask;

	if (cmd_mask == NULL)
		return 0;

	if (cmd_mask->start_src != 0) {
		tmp1 = desc->start_src & ~(cmd_mask->start_src);
		tmp2 = desc->start_src & (cmd_mask->start_src);
		if (tmp1 != 0 || tmp2 == 0) {
			__a4l_err("a4l_check_cmddesc: start_src, "
				  "trigger unsupported\n");
			return -EINVAL;
		}
	}

	if (cmd_mask->scan_begin_src != 0) {
		tmp1 = desc->scan_begin_src & ~(cmd_mask->scan_begin_src);
		tmp2 = desc->scan_begin_src & (cmd_mask->scan_begin_src);
		if (tmp1 != 0 || tmp2 == 0) {
			__a4l_err("a4l_check_cmddesc: scan_begin_src, "
				  "trigger unsupported\n");
			return -EINVAL;
		}
	}

	if (cmd_mask->convert_src != 0) {
		tmp1 = desc->convert_src & ~(cmd_mask->convert_src);
		tmp2 = desc->convert_src & (cmd_mask->convert_src);
		if (tmp1 != 0 || tmp2 == 0) {
			__a4l_err("a4l_check_cmddesc: convert_src, "
				  "trigger unsupported\n");
			return -EINVAL;
		}
	}

	if (cmd_mask->scan_end_src != 0) {
		tmp1 = desc->scan_end_src & ~(cmd_mask->scan_end_src);
		if (tmp1 != 0) {
			__a4l_err("a4l_check_cmddesc: scan_end_src, "
				  "trigger unsupported\n");
			return -EINVAL;
		}
	}

	if (cmd_mask->stop_src != 0) {
		tmp1 = desc->stop_src & ~(cmd_mask->stop_src);
		tmp2 = desc->stop_src & (cmd_mask->stop_src);
		if (tmp1 != 0 || tmp2 == 0) {
			__a4l_err("a4l_check_cmddesc: stop_src, "
				  "trigger unsupported\n");
			return -EINVAL;
		}
	}

	return 0;
}

/* --- IOCTL / FOPS function --- */

int a4l_ioctl_cmd(a4l_cxt_t * cxt, void *arg)
{
	int ret = 0, simul_flag = 0;
	a4l_cmd_t *cmd_desc = NULL;
	a4l_dev_t *dev = a4l_get_dev(cxt);

	__a4l_dbg(1, core_dbg, 
		  "a4l_ioctl_cmd: minor=%d\n", a4l_get_minor(cxt));

	/* Basically check the device */
	if (!test_bit(A4L_DEV_ATTACHED, &dev->flags)) {
		__a4l_err("a4l_ioctl_cmd: cannot command "
			  "an unattached device\n");
		return -EINVAL;
	}

	/* Allocates the command */
	cmd_desc = (a4l_cmd_t *) rtdm_malloc(sizeof(a4l_cmd_t));
	if (cmd_desc == NULL)
		return -ENOMEM;
	memset(cmd_desc, 0, sizeof(a4l_cmd_t));

	/* Gets the command */
	ret = a4l_fill_cmddesc(cxt, cmd_desc, arg);
	if (ret != 0)
		goto out_ioctl_cmd;

	/* Checks the command */
	ret = a4l_check_cmddesc(cxt, cmd_desc);
	if (ret != 0)
		goto out_ioctl_cmd;

	ret = a4l_check_generic_cmdcnt(cmd_desc);
	if (ret != 0)
		goto out_ioctl_cmd;

	ret = a4l_check_specific_cmdcnt(cxt, cmd_desc);
	if (ret != 0)
		goto out_ioctl_cmd;

	__a4l_dbg(1, core_dbg, 
		  "a4l_ioctl_cmd: 1st cmd checks passed\n");

	/* Tests the command with the cmdtest function */
	if (dev->transfer.subds[cmd_desc->idx_subd]->do_cmdtest != NULL)
		ret = dev->transfer.subds[cmd_desc->idx_subd]->
			do_cmdtest(dev->transfer.subds[cmd_desc->idx_subd], 
				   cmd_desc);
	if (ret != 0) {
		__a4l_err("a4l_ioctl_cmd: driver's cmd_test failed\n");
		goto out_ioctl_cmd;
	}

	__a4l_dbg(1, core_dbg, 
		  "a4l_ioctl_cmd: driver's cmd checks passed\n");

	if (cmd_desc->flags & A4L_CMD_SIMUL) {
		simul_flag = 1;
		goto out_ioctl_cmd;
	}

	/* Sets the concerned subdevice as busy */
	ret = a4l_reserve_transfer(cxt, cmd_desc->idx_subd);
	if (ret < 0)
		goto out_ioctl_cmd;

	/* Gets the transfer system ready */
	a4l_init_transfer(cxt, cmd_desc);

	/* Eventually launches the command */
	ret = dev->transfer.subds[cmd_desc->idx_subd]->
		do_cmd(dev->transfer.subds[cmd_desc->idx_subd], 
		       cmd_desc);

	if (ret != 0) {
		a4l_cancel_transfer(cxt, cmd_desc->idx_subd);
		goto out_ioctl_cmd;
	}

      out_ioctl_cmd:
	if (ret != 0 || simul_flag == 1) {
		a4l_free_cmddesc(cmd_desc);
		rtdm_free(cmd_desc);
	}

	return ret;
}

#endif /* !DOXYGEN_CPP */
