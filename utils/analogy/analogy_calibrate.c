/**
 * @file
 * Analogy for Linux, calibration program
 *
 * @note Copyright (C) 2014 Jorge A. Ramirez-Ortiz <jro@xenomai.org>
 *
 * from original code from the Comedi project
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

#include <sys/time.h>
#include <sys/resource.h>
#include <getopt.h>
#include <pthread.h>
#include <sys/mman.h>
#include <xeno_config.h>
#include <rtdm/analogy.h>
#include "analogy_calibrate.h"
#include "calibration_ni_m.h"

struct apply_calibration_params params = {.name = NULL,} ;
struct timespec calibration_start_time;
static const char *revision = "0.0.1";
a4l_desc_t descriptor;
FILE *cal = NULL;

static const struct option options[] = {
	{
#define help_opt	0
		.name = "help",
		.has_arg = 0,
		.flag = NULL,
	},
	{
#define device_opt	1
		.name = "device",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define output_opt	2
		.name = "output",
		.has_arg = 1,
		.flag = NULL,
	},
	{
#define apply_opt	3
		.name = "apply",
		.has_arg = 1,
		.flag = NULL,
	},
	{
		.name = NULL,
	}
};

static void
print_usage(void)
{
	fprintf(stderr, "Usage: analogy_calibrate \n"
	       "  --help 	     				: this menu \n"
	       "  --device /dev/analogyX			: analogy device to calibrate \n"
	       "  --output filename   				: calibration results \n"
	       "  --apply filename:subd,channel,range,aref 	: apply the calibration file \n"
	       "          ex: /home/foo/calib.rc:0,1,255,255 - use 255 for dont care \n"
	      );
}

static int
apply_calibration_set_globals(char *info)
{
	char *p;

	params.name = strtok(info, ":");
	p = strtok(NULL, ",");
	if (!p)
		error(EXIT, 0, "missing --apply parameter subd \n");
	params.subd = strtol(p, NULL, 0);

	p = strtok(NULL, ",");
	if (!p)
		error(EXIT, 0, "missing --apply parameter channel \n");
	params.channel = strtol(p, NULL, 0);

	p = strtok(NULL, ",");
	if (!p)
		error(EXIT, 0, "missing --apply parameter range \n");
	params.range = strtol(p, NULL, 0);

	p = strtok(NULL, "");
	if (!p)
		error(EXIT, 0, "missing --apply parameter aref \n");
	params.aref = strtol(p, NULL, 0);

	return 0;
}

static void __attribute__ ((constructor)) __analogy_calibrate_init(void)
{
	clock_gettime(CLOCK_MONOTONIC, &calibration_start_time);
}
int main(int argc, char *argv[])
{
	char *device = NULL, *file = NULL, *apply_info = NULL;
	int v, i, fd, err = 0;

	__debug("version: git commit %s, revision %s \n", GIT_STAMP, revision);

	for (;;) {
		i = -1;
		v = getopt_long_only(argc, argv, "", options, &i);
		if (v == EOF)
			break;
		switch (i) {
		case help_opt:
			print_usage();
			exit(0);
		case device_opt:
			device = optarg;
			break;
		case output_opt:
			file = optarg;
			cal = fopen(file, "w+");
			if (!cal)
				error(EXIT, errno, "calibration file");
			__debug("calibration output: %s \n", file);
			break;
		case apply_opt:
			apply_info = optarg;
			break;
		default:
			print_usage();
			exit(EXIT_FAILURE);
		}
	}

	if (apply_info)
		apply_calibration_set_globals(apply_info);

	fd = a4l_open(&descriptor, device);
	if (fd < 0)
		error(EXIT, 0, "open %s failed (%d)", device, fd);

	err = ni_m_board_supported(descriptor.driver_name);
	if (err)
		error(EXIT, 0, "board %s: driver %s not supported",
		      descriptor.board_name, descriptor.driver_name);

	/*
	 * TODO: modify the meaning of board/driver in the proc
	 */
	push_to_cal_file("[%s] \n",PLATFORM_STR);
	push_to_cal_file(DRIVER_STR" = %s;\n", descriptor.board_name);
	push_to_cal_file(BOARD_STR" = %s;\n", descriptor.driver_name);

	err = ni_m_software_calibrate();
	if (err)
		error(CONT, 0, "software calibration failed (%d)", err);

	err = ni_m_apply_calibration();
	if (err)
		error(CONT, 0, "applying calibration failed (%d)", err);

	a4l_close(&descriptor);

	return err;
}