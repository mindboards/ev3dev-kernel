/*
 * Support for the input and output ports on the LEGO Mindstorms EV3
 *
 * Copyright (C) 2013-2014 David Lechner <david@lechnology.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef __LINUX_LEGOEV3_PORTS_H
#define __LINUX_LEGOEV3_PORTS_H

#include <linux/device.h>
#include <linux/hrtimer.h>
#include <linux/platform_device.h>
#include <linux/legoev3/ev3_input_port.h>
#include <linux/legoev3/ev3_output_port.h>

#include <mach/legoev3.h>

#define LEGOEV3_PORT_NAME_SIZE 30

struct legoev3_ports_platform_data {
	struct ev3_input_port_platform_data  input_port_data[NUM_EV3_PORT_IN];
	struct ev3_output_port_platform_data output_port_data[NUM_EV3_PORT_OUT];
};

struct legoev3_port_device {
	char name[LEGOEV3_PORT_NAME_SIZE + 1];
	int id;
	int type_id;
	struct device dev;
};

static inline struct legoev3_port_device
*to_legoev3_port_device(struct device *dev)
{
	return dev ? container_of(dev, struct legoev3_port_device, dev) : NULL;
}

extern struct legoev3_port_device
*legoev3_port_device_register(const char *, int, struct device_type *, int,
			      void *, size_t, struct device *);
extern void legoev3_port_device_unregister(struct legoev3_port_device *);

#define NXT_TOUCH_SENSOR_TYPE_ID	1
#define NXT_LIGHT_SENSOR_TYPE_ID	2
#define NXT_ANALOG_SENSOR_TYPE_ID	3
#define NXT_COLOR_SENSOR_TYPE_ID	4
#define EV3_TOUCH_SENSOR_TYPE_ID	16
#define LEGOEV3_TYPE_ID_UNKNOWN		125

struct legoev3_port_device_id {
	char name[LEGOEV3_PORT_NAME_SIZE];
	int type_id __attribute__((aligned(sizeof(int))));
};

#define LEGOEV3_PORT_DEVICE_ID(_type_name, _type_id)	\
	{						\
		.name = _type_name,			\
		.type_id = _type_id,			\
	}

struct legoev3_port_driver {
	int (*probe)(struct legoev3_port_device *);
	int (*remove)(struct legoev3_port_device *);
	void (*shutdown)(struct legoev3_port_device *);
	struct device_driver driver;
	const struct legoev3_port_device_id *id_table;
};

static inline struct legoev3_port_driver
*to_legoev3_port_driver(struct device_driver *drv)
{
	return drv ? container_of(drv, struct legoev3_port_driver, driver) : NULL;
}

extern int legoev3_register_port_driver(struct legoev3_port_driver *);
extern void legoev3_unregister_port_driver(struct legoev3_port_driver *);
#define legoev3_port_driver(driver) \
module_driver(driver, legoev3_register_port_driver, legoev3_unregister_port_driver);

extern struct attribute_group legoev3_port_device_type_attr_grp;
extern struct bus_type legoev3_bus_type;

#endif /* __LINUX_LEGOEV3_PORTS_H */
