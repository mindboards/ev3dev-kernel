/*
 * tty line discipline for LEGO Mindstorms EV3 UART sensors
 *
 * Copyright (C) 2014 David Lechner <david@lechnology.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.

 * This program is distributed "as is" WITHOUT ANY WARRANTY of any
 * kind, whether express or implied; without even the implied warranty
 * of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/bitops.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/tty.h>
#include <linux/legoev3/legoev3_ports.h>
#include <linux/legoev3/legoev3_uart.h>

#ifdef DEBUG
#define debug_pr(fmt, ...) printk(fmt, ##__VA_ARGS__)
#else
#define debug_pr(fmt, ...) while(0) { }
#endif

#define LEGOEV3_UART_BUFFER_SIZE	256
#define LEGOEV3_UART_SENSOR_DATA_SIZE	32

#define LEGOEV3_UART_MSG_TYPE_MASK	0xC0
#define LEGOEV3_UART_CMD_SIZE(byte)	(1 << ((byte >> 3) & 0x7))
#define LEGOEV3_UART_MSG_CMD_MASK	0x07
#define LEGOEV3_UART_MAX_DATA_ERR	6

#define LEGOEV3_UART_TYPE_UNKNOWN	125
#define LEGOEV3_UART_SPEED_MIN		2400
#define LEGOEV3_UART_SPEED_MID		57600
#define LEGOEV3_UART_SPEED_MAX		460800

#define LEGOEV3_UART_SEND_ACK_DELAY		10 /* msec */
#define LEGOEV3_UART_SET_BITRATE_DELAY		10 /* msec */
#define LEGOEV3_UART_DATA_KEEP_ALIVE_TIMEOUT	100 /* msec */

#define LEGOEV3_UART_DEVICE_TYPE_NAME_SIZE	30

enum legoev3_uart_msg_type {
	LEGOEV3_UART_MSG_TYPE_SYS	= 0x00,
	LEGOEV3_UART_MSG_TYPE_CMD	= 0x40,
	LEGOEV3_UART_MSG_TYPE_INFO	= 0x80,
	LEGOEV3_UART_MSG_TYPE_DATA	= 0xC0,
};

enum legoev3_uart_sys {
	LEGOEV3_UART_SYS_SYNC		= 0x0,
	LEGOEV3_UART_SYS_NACK		= 0x2,
	LEGOEV3_UART_SYS_ACK		= 0x4,
	LEGOEV3_UART_SYS_ESC		= 0x6,
};

enum legoev3_uart_cmd {
	LEGOEV3_UART_CMD_TYPE		= 0x0,
	LEGOEV3_UART_CMD_MODES		= 0x1,
	LEGOEV3_UART_CMD_SPEED		= 0x2,
	LEGOEV3_UART_CMD_SELECT		= 0x3,
	LEGOEV3_UART_CMD_WRITE		= 0x4,
};

enum legoev3_uart_info {
	LEGOEV3_UART_INFO_NAME		= 0x00,
	LEGOEV3_UART_INFO_RAW		= 0x01,
	LEGOEV3_UART_INFO_PCT		= 0x02,
	LEGOEV3_UART_INFO_SI		= 0x03,
	LEGOEV3_UART_INFO_UNITS		= 0x04,
	LEGOEV3_UART_INFO_FORMAT	= 0x80,
};

#define LEGOEV3_UART_INFO_BIT_CMD_TYPE		0
#define LEGOEV3_UART_INFO_BIT_CMD_MODES		1
#define LEGOEV3_UART_INFO_BIT_CMD_SPEED		2
#define LEGOEV3_UART_INFO_BIT_INFO_NAME		3
#define LEGOEV3_UART_INFO_BIT_INFO_RAW		4
#define LEGOEV3_UART_INFO_BIT_INFO_PCT		5
#define LEGOEV3_UART_INFO_BIT_INFO_SI		6
#define LEGOEV3_UART_INFO_BIT_INFO_UNITS	7
#define LEGOEV3_UART_INFO_BIT_INFO_FORMAT	8

enum legoev3_uart_info_flags {
	LEGOEV3_UART_INFO_FLAG_CMD_TYPE		= BIT(LEGOEV3_UART_INFO_BIT_CMD_TYPE),
	LEGOEV3_UART_INFO_FLAG_CMD_MODES	= BIT(LEGOEV3_UART_INFO_BIT_CMD_MODES),
	LEGOEV3_UART_INFO_FLAG_CMD_SPEED	= BIT(LEGOEV3_UART_INFO_BIT_CMD_SPEED),
	LEGOEV3_UART_INFO_FLAG_INFO_NAME	= BIT(LEGOEV3_UART_INFO_BIT_INFO_NAME),
	LEGOEV3_UART_INFO_FLAG_INFO_RAW		= BIT(LEGOEV3_UART_INFO_BIT_INFO_RAW),
	LEGOEV3_UART_INFO_FLAG_INFO_PCT		= BIT(LEGOEV3_UART_INFO_BIT_INFO_PCT),
	LEGOEV3_UART_INFO_FLAG_INFO_SI		= BIT(LEGOEV3_UART_INFO_BIT_INFO_SI),
	LEGOEV3_UART_INFO_FLAG_INFO_UNITS	= BIT(LEGOEV3_UART_INFO_BIT_INFO_UNITS),
	LEGOEV3_UART_INFO_FLAG_INFO_FORMAT	= BIT(LEGOEV3_UART_INFO_BIT_INFO_FORMAT),
	LEGOEV3_UART_INFO_FLAG_ALL_INFO		= LEGOEV3_UART_INFO_FLAG_INFO_NAME
						| LEGOEV3_UART_INFO_FLAG_INFO_RAW
						| LEGOEV3_UART_INFO_FLAG_INFO_PCT
						| LEGOEV3_UART_INFO_FLAG_INFO_SI
						| LEGOEV3_UART_INFO_FLAG_INFO_UNITS
						| LEGOEV3_UART_INFO_FLAG_INFO_FORMAT,
	LEGOEV3_UART_INFO_FLAG_REQUIRED		= LEGOEV3_UART_INFO_FLAG_CMD_TYPE
						| LEGOEV3_UART_INFO_FLAG_CMD_MODES
						| LEGOEV3_UART_INFO_FLAG_INFO_NAME
						| LEGOEV3_UART_INFO_FLAG_INFO_FORMAT,
};

/**
 * struct legoev3_uart_data - Discipline data for EV3 UART Sensor communication
 * @tty: Pointer to the tty device that the sensor is connected to
 * @sensor: The real sensor device.
 * @send_ack_work: Used to send ACK after a delay.
 * @change_bitrate_work: Used to change the baud rate after a delay.
 * @keep_alive_timer: Sends a NACK every 100usec when a sensor is connected.
 * @keep_alive_tasklet: Does the actual sending of the NACK.
 * @mode_info: Array of information about each mode of the sensor
 * @type: The type of sensor that we are connected to. *
 * @num_modes: The number of modes that the sensor has. (1-8)
 * @num_view_modes: Number of modes that can be used for data logging. (1-8)
 * @mode: The current mode.
 * @info_flags: Flags indicating what information has already been read
 * 	from the sensor.
 * @buffer: Byte array to store received data in between receive_buf interrupts.
 * @write_ptr: The current position in the buffer.
 * @data_watchdog: Watchdog timer for receiving DATA messages.
 * @num_data_err: Number of bad reads when receiving DATA messages.
 * @synced: Flag indicating communications are synchronized with the sensor.
 * @info_done: Flag indicating that all mode info has been received and it is
 * 	OK to start receiving DATA messages.
 * @data_rec: Flag that indicates that good DATA message has been received
 * 	since last watchdog timeout.
 */
struct legoev3_uart_port_data {
	struct tty_struct *tty;
	struct legoev3_port_device *sensor;
	struct legoev3_uart_sensor_platform_data pdata;
	struct delayed_work send_ack_work;
	struct delayed_work change_bitrate_work;
	struct hrtimer keep_alive_timer;
	struct tasklet_struct keep_alive_tasklet;
	struct legoev3_uart_mode_info mode_info[LEGOEV3_UART_MODE_MAX + 1];
	u8 type;
	u8 num_modes;
	u8 num_view_modes;
	u8 mode;
	speed_t new_baud_rate;
	long unsigned info_flags;
	u8 buffer[LEGOEV3_UART_BUFFER_SIZE];
	unsigned write_ptr;
	char *last_err;
	unsigned num_data_err;
	unsigned synced:1;
	unsigned info_done:1;
	unsigned data_rec:1;
};

u8 legoev3_uart_set_msg_hdr(u8 type, const unsigned long size, u8 cmd)
{
	u8 size_code = (find_last_bit(&size, sizeof(unsigned long)) & 0x7) << 3;

	return (type & LEGOEV3_UART_MSG_TYPE_MASK) | size_code
		| (cmd & LEGOEV3_UART_MSG_CMD_MASK);
}

struct indexed_device_attribute {
	struct device_attribute dev_attr;
	int index;
};
#define to_indexed_dev_attr(_dev_attr) \
	container_of(_dev_attr, struct indexed_device_attribute, dev_attr)

#define INDEXED_ATTR(_name, _mode, _show, _store, _index)		\
	{								\
		.dev_attr = __ATTR(_name, _mode, _show, _store),	\
		.index = _index						\
	}

#define INDEXED_DEVICE_ATTR(_name, _mode, _show, _store, _index)	\
struct indexed_device_attribute indexed_dev_attr_##_name		\
	= INDEXED_ATTR(_name, _mode, _show, _store, _index)

static ssize_t legoev3_uart_show_type_id(struct device *dev,
					 struct device_attribute *attr,
					 char *buf)
{
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;

	return sprintf(buf, "%d\n", port->type);
}

static ssize_t legoev3_uart_show_mode(struct device *dev,
				      struct device_attribute *attr,
				      char *buf)
{
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;
	int i;
	unsigned count = 0;
	int mode = port->mode;

	for (i = 0; i < port->num_modes; i++) {
		if (i == mode)
			count += sprintf(buf + count, "[");
		count += sprintf(buf + count, "%s", pdata->mode_info[i].name);
		if (i == mode)
			count += sprintf(buf + count, "]");
		count += sprintf(buf + count, "%c", ' ');
	}
	if (count == 0)
		return -ENXIO;
	buf[count - 1] = '\n';

	return count;
}

static ssize_t legoev3_uart_store_mode(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;
	int i, err;

	for (i = 0; i < port->num_modes; i++) {
		if (sysfs_streq(buf, pdata->mode_info[i].name)) {
			err = legoev3_uart_set_mode(port->tty, i);
			if (err)
				return err;
			return count;
		}
	}
	return -EINVAL;
}

/* common definition for the min/max properties (float data)*/
#define LEGOEV3_UART_SHOW_F(name)						\
static ssize_t legoev3_uart_show_##name(struct device *dev,			\
					struct device_attribute *attr,		\
					char *buf)				\
{										\
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;	\
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;		\
	int value = pdata->mode_info[port->mode].name;				\
	int dp = pdata->mode_info[port->mode].decimals;				\
										\
	return sprintf(buf, "%d\n", legoev3_uart_ftoi(value, dp));		\
}

LEGOEV3_UART_SHOW_F(raw_min)
LEGOEV3_UART_SHOW_F(raw_max)
LEGOEV3_UART_SHOW_F(pct_min)
LEGOEV3_UART_SHOW_F(pct_max)
LEGOEV3_UART_SHOW_F(si_min)
LEGOEV3_UART_SHOW_F(si_max)

static ssize_t legoev3_uart_show_si_units(struct device *dev,
                                          struct device_attribute *attr,
                                          char *buf)
{
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;

	return sprintf(buf, "%s\n",pdata->mode_info[port->mode].units);
}

static ssize_t legoev3_uart_show_dp(struct device *dev,
				    struct device_attribute *attr,
				    char *buf)
{
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;

	return sprintf(buf, "%d\n", pdata->mode_info[port->mode].decimals);
}

static ssize_t legoev3_uart_show_num_values(struct device *dev,
					    struct device_attribute *attr,
					    char *buf)
{
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;

	return sprintf(buf, "%d\n", port->mode_info[port->mode].data_sets);
}

int legoev3_uart_raw_s8_value(struct legoev3_uart_sensor_platform_data *pdata,
                              int index)
{
	int mode = legoev3_uart_get_mode(pdata->tty);

	return *(s8 *)(pdata->mode_info[mode].raw_data + index);
}

int legoev3_uart_raw_s16_value(struct legoev3_uart_sensor_platform_data *pdata,
                               int index)
{
	int mode = legoev3_uart_get_mode(pdata->tty);

	return *(s16 *)(pdata->mode_info[mode].raw_data + index * 2);
}

int legoev3_uart_raw_s32_value(struct legoev3_uart_sensor_platform_data *pdata,
                               int index)
{
	int mode = legoev3_uart_get_mode(pdata->tty);

	return *(s32 *)(pdata->mode_info[mode].raw_data + index * 4);
}

int legoev3_uart_raw_float_value(struct legoev3_uart_sensor_platform_data *pdata,
                                 int index)
{
	int mode = legoev3_uart_get_mode(pdata->tty);

	return legoev3_uart_ftoi(
		*(u32 *)(pdata->mode_info[mode].raw_data + index * 4),
		pdata->mode_info[mode].decimals);
}

static ssize_t legoev3_uart_show_value(struct device *dev,
                                       struct device_attribute *attr,
                                       char *buf)
{
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;
	struct indexed_device_attribute *ind_attr = to_indexed_dev_attr(attr);
	int count = -ENXIO;

	if (ind_attr->index >= pdata->mode_info[port->mode].data_sets)
		return count;

	switch (pdata->mode_info[port->mode].format) {
	case LEGOEV3_UART_DATA_8:
		count = sprintf(buf, "%d\n",
			legoev3_uart_raw_s8_value(pdata, ind_attr->index));
		break;
	case LEGOEV3_UART_DATA_16:
		count = sprintf(buf, "%d\n",
			legoev3_uart_raw_s16_value(pdata, ind_attr->index));
		break;
	case LEGOEV3_UART_DATA_32:
		count = sprintf(buf, "%d\n",
			legoev3_uart_raw_s32_value(pdata, ind_attr->index));
		break;
	case LEGOEV3_UART_DATA_FLOAT:
		count = sprintf(buf, "%d\n",
			legoev3_uart_raw_float_value(pdata, ind_attr->index));
		break;
	}

	return count;
}

static ssize_t legoev3_uart_show_bin_data_format(struct device *dev,
                                                 struct device_attribute *attr,
                                                 char *buf)
{
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;
	int count = -ENXIO;

	switch (pdata->mode_info[port->mode].format) {
	case LEGOEV3_UART_DATA_8:
		count = sprintf(buf, "%s\n", "s8");
		break;
	case LEGOEV3_UART_DATA_16:
		count = sprintf(buf, "%s\n", "s16");
		break;
	case LEGOEV3_UART_DATA_32:
		count = sprintf(buf, "%s\n", "s32");
		break;
	case LEGOEV3_UART_DATA_FLOAT:
		count = sprintf(buf, "%s\n", "float");
		break;
	}

	return count;
}

static ssize_t legoev3_uart_read_bin_data(struct file *file, struct kobject *kobj,
                                          struct bin_attribute *attr,
                                          char *buf, loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;
	size_t size = attr->size;

	if (off >= size || !count)
		return 0;
	size -= off;
	if (count < size)
		size = count;
	memcpy(buf + off, pdata->mode_info[port->mode].raw_data, size);

	return size;
}

static ssize_t legoev3_uart_write_bin_data(struct file *file ,struct kobject *kobj,
                                           struct bin_attribute *attr,
                                           char *buf, loff_t off, size_t count)
{
	struct device *dev = container_of(kobj, struct device, kobj);
	struct legoev3_uart_sensor_platform_data *pdata = dev->platform_data;
	struct legoev3_uart_port_data *port = pdata->tty->disc_data;
	char data[32 + 2];
	int size, i, err;

	if (off != 0 || count > 32)
		return -EINVAL;
	if (count == 0)
		return count;
	memset(data + 1, 0, 32);
	memcpy(data + 1, buf, count);
	if (count <= 2)
		size = count;
	else if (count <= 4)
		size = 4;
	else if (count <= 8)
		size = 8;
	else if (count <= 16)
		size = 16;
	else
		size = 32;
	data[0] = legoev3_uart_set_msg_hdr(LEGOEV3_UART_MSG_TYPE_CMD, size,
					   LEGOEV3_UART_CMD_WRITE);
	data[size + 1] = 0xFF;
	for (i = 0; i <= size; i++)
		data[size + 1] ^= data[i];
	set_bit(TTY_DO_WRITE_WAKEUP, &port->tty->flags);
	err = port->tty->ops->write(port->tty, data, size + 2);
	if (err < 0)
		return err;

	return count;
}

DEVICE_ATTR(type_id, S_IRUGO , legoev3_uart_show_type_id, NULL);
DEVICE_ATTR(mode, S_IRUGO | S_IWUGO, legoev3_uart_show_mode, legoev3_uart_store_mode);
DEVICE_ATTR(raw_min, S_IRUGO , legoev3_uart_show_raw_min, NULL);
DEVICE_ATTR(raw_max, S_IRUGO , legoev3_uart_show_raw_max, NULL);
DEVICE_ATTR(pct_min, S_IRUGO , legoev3_uart_show_pct_min, NULL);
DEVICE_ATTR(pct_max, S_IRUGO , legoev3_uart_show_pct_max, NULL);
DEVICE_ATTR(si_min, S_IRUGO , legoev3_uart_show_si_min, NULL);
DEVICE_ATTR(si_max, S_IRUGO , legoev3_uart_show_si_max, NULL);
DEVICE_ATTR(si_units, S_IRUGO , legoev3_uart_show_si_units, NULL);
DEVICE_ATTR(dp, S_IRUGO , legoev3_uart_show_dp, NULL);
DEVICE_ATTR(num_values, S_IRUGO , legoev3_uart_show_num_values, NULL);
DEVICE_ATTR(bin_data_format, S_IRUGO , legoev3_uart_show_bin_data_format, NULL);

/*
 * Technically, it is possible to have 32 8-bit values, but known sensors so
 * far are less than 8, so we only expose 8 values to prevent overcrowding.
 */
INDEXED_DEVICE_ATTR(value0, S_IRUGO , legoev3_uart_show_value, NULL, 0);
INDEXED_DEVICE_ATTR(value1, S_IRUGO , legoev3_uart_show_value, NULL, 1);
INDEXED_DEVICE_ATTR(value2, S_IRUGO , legoev3_uart_show_value, NULL, 2);
INDEXED_DEVICE_ATTR(value3, S_IRUGO , legoev3_uart_show_value, NULL, 3);
INDEXED_DEVICE_ATTR(value4, S_IRUGO , legoev3_uart_show_value, NULL, 4);
INDEXED_DEVICE_ATTR(value5, S_IRUGO , legoev3_uart_show_value, NULL, 5);
INDEXED_DEVICE_ATTR(value6, S_IRUGO , legoev3_uart_show_value, NULL, 6);
INDEXED_DEVICE_ATTR(value7, S_IRUGO , legoev3_uart_show_value, NULL, 7);

static struct bin_attribute dev_bin_attr_bin_data = {
	.attr	= {
		.name	= "bin_data",
		.mode	= S_IRUGO,
	},
	.size	= LEGOEV3_UART_SENSOR_DATA_SIZE,
	.read	= legoev3_uart_read_bin_data,
	.write	= legoev3_uart_write_bin_data,
};

static struct attribute *legoev3_uart_sensor_attrs[] = {
	&dev_attr_type_id.attr,
	&dev_attr_mode.attr,
	&dev_attr_raw_min.attr,
	&dev_attr_raw_max.attr,
	&dev_attr_pct_min.attr,
	&dev_attr_pct_max.attr,
	&dev_attr_si_min.attr,
	&dev_attr_si_max.attr,
	&dev_attr_si_units.attr,
	&dev_attr_dp.attr,
	&dev_attr_bin_data_format.attr,
	&dev_attr_num_values.attr,
	&indexed_dev_attr_value0.dev_attr.attr,
	&indexed_dev_attr_value1.dev_attr.attr,
	&indexed_dev_attr_value2.dev_attr.attr,
	&indexed_dev_attr_value3.dev_attr.attr,
	&indexed_dev_attr_value4.dev_attr.attr,
	&indexed_dev_attr_value5.dev_attr.attr,
	&indexed_dev_attr_value6.dev_attr.attr,
	&indexed_dev_attr_value7.dev_attr.attr,
	NULL
};

struct attribute_group legoev3_uart_sensor_attr_grp = {
	.attrs	= legoev3_uart_sensor_attrs,
};

const struct attribute_group *legoev3_uart_sensor_device_type_attr_groups[] = {
	&legoev3_port_device_type_attr_grp,
	&legoev3_uart_sensor_attr_grp,
	NULL
};

static struct device_type legoev3_uart_sensor_device_type = {
	.name	= "ev3-uart-sensor",
	.groups	= legoev3_uart_sensor_device_type_attr_groups,
};

static struct legoev3_uart_mode_info legoev3_uart_default_mode_info = {
	.raw_max	= 0x447fc000,	/* 1023.0 */
	.pct_max	= 0x42c80000,	/*  100.0 */
	.si_max		= 0x3f800000,	/*    1.0 */
	.figures	= 4,
};

static inline int legoev3_uart_msg_size(u8 header)
{
	int size;

	if (!(header & LEGOEV3_UART_MSG_TYPE_MASK)) /* SYNC, NACK, ACK */
		return 1;

	size = LEGOEV3_UART_CMD_SIZE(header);
	size += 2; /* header and checksum */
	if ((header & LEGOEV3_UART_MSG_TYPE_MASK) == LEGOEV3_UART_MSG_TYPE_INFO)
		size++; /* extra command byte */

	return size;
}

int legoev3_uart_write_byte(struct tty_struct *tty, const u8 byte)
{
	int ret;

	set_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
	ret = tty_put_char(tty, byte);
	if (tty->ops->flush_chars)
		tty->ops->flush_chars(tty);

	return ret;
}

int legoev3_uart_get_mode(struct tty_struct *tty)
{
	struct legoev3_uart_port_data *port;

	if (!tty)
		return -ENODEV;

	port = tty->disc_data;

	return port->mode;
}
EXPORT_SYMBOL_GPL(legoev3_uart_get_mode);

int legoev3_uart_set_mode(struct tty_struct *tty, const u8 mode)
{
	struct legoev3_uart_port_data *port;
	const int data_size = 3;
	u8 data[data_size];
	int ret;

	if (!tty)
		return -ENODEV;

	port = tty->disc_data;
	if (mode >= port->num_modes)
		return -EINVAL;

	data[0] = legoev3_uart_set_msg_hdr(LEGOEV3_UART_MSG_TYPE_CMD,
					   data_size - 2,
					   LEGOEV3_UART_CMD_SELECT);
	data[1] = mode;
	data[2] = 0xFF ^ data[0] ^ data[1];

	set_bit(TTY_DO_WRITE_WAKEUP, &tty->flags);
	ret = tty->ops->write(tty, data, data_size);
	if (ret < 0)
		return ret;

	return 0;
}
EXPORT_SYMBOL_GPL(legoev3_uart_set_mode);

static void legoev3_uart_send_ack(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct legoev3_uart_port_data *port =
		container_of(dwork, struct legoev3_uart_port_data, send_ack_work);
	struct legoev3_port_device *sensor;
	int err;

	if (!port->sensor && port->type <= LEGOEV3_UART_TYPE_MAX) {
		port->pdata.tty = port->tty;
		port->pdata.mode_info = port->mode_info;
		port->pdata.num_modes = port->num_modes;
		port->pdata.num_view_modes = port->num_view_modes;
		sensor = legoev3_port_device_register(
				"ev3-uart-sensor",
				-1, /* TODO: get input port ID here */
				&legoev3_uart_sensor_device_type,
				port->type,
				&port->pdata,
				sizeof(struct legoev3_uart_sensor_platform_data),
				port->tty->dev);
		if (IS_ERR(sensor)) {
			dev_err(port->tty->dev, "Could not register UART sensor on tty %s",
					port->tty->name);
			return;
		}
		err = sysfs_create_bin_file(&sensor->dev.kobj, &dev_bin_attr_bin_data);
		if (err < 0) {
			dev_err(&sensor->dev, "Could not register binary attribute.");
			legoev3_port_device_unregister(sensor);
			return;
		}
		port->sensor = sensor;

	} else
		dev_err(port->tty->dev, "Reconnected due to: %s\n", port->last_err);

	legoev3_uart_write_byte(port->tty, LEGOEV3_UART_SYS_ACK);
	schedule_delayed_work(&port->change_bitrate_work,
	                      msecs_to_jiffies(LEGOEV3_UART_SET_BITRATE_DELAY));
}

static void legoev3_uart_change_bitrate(struct work_struct *work)
{
	struct delayed_work *dwork = to_delayed_work(work);
	struct legoev3_uart_port_data *port =
		container_of(dwork, struct legoev3_uart_port_data, change_bitrate_work);
	struct ktermios old_termios = *port->tty->termios;

	tty_wait_until_sent(port->tty, 0);
	mutex_lock(&port->tty->termios_mutex);
	tty_encode_baud_rate(port->tty, port->new_baud_rate, port->new_baud_rate);
	if (port->tty->ops->set_termios)
			port->tty->ops->set_termios(port->tty, &old_termios);
	mutex_unlock(&port->tty->termios_mutex);
	if (port->info_done) {
		hrtimer_start(&port->keep_alive_timer,
			ktime_set(0, LEGOEV3_UART_DATA_KEEP_ALIVE_TIMEOUT / 2
				     * 1000000),
			HRTIMER_MODE_REL);
	}
}

static void legoev3_uart_send_keep_alive(unsigned long data)
{
	struct tty_struct *tty = (void *)data;

	/* NACK is sent as a keep-alive */
	legoev3_uart_write_byte(tty, LEGOEV3_UART_SYS_NACK);
}

enum hrtimer_restart legoev3_uart_keep_alive_timer_callback(struct hrtimer *timer)
{
	struct legoev3_uart_port_data *port =
		container_of(timer, struct legoev3_uart_port_data, keep_alive_timer);

	if (!port->synced || !port->info_done)
		return HRTIMER_NORESTART;

	hrtimer_forward_now(timer, ktime_set(0,
			    LEGOEV3_UART_DATA_KEEP_ALIVE_TIMEOUT * 1000000));
	if (!port->data_rec) {
		port->last_err = "No data since last keep-alive.";
		port->num_data_err++;
	}
	port->data_rec = 0;

	tasklet_schedule(&port->keep_alive_tasklet);

	return port->num_data_err > LEGOEV3_UART_MAX_DATA_ERR
		? HRTIMER_NORESTART : HRTIMER_RESTART;
}

static int legoev3_uart_open(struct tty_struct *tty)
{
	struct ktermios old_termios = *tty->termios;
	struct legoev3_uart_port_data *port;

	port = kzalloc(sizeof(struct legoev3_uart_port_data), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	port->tty = tty;
	port->type = LEGOEV3_UART_TYPE_UNKNOWN;
	port->new_baud_rate = LEGOEV3_UART_SPEED_MIN;
	INIT_DELAYED_WORK(&port->send_ack_work, legoev3_uart_send_ack);
	INIT_DELAYED_WORK(&port->change_bitrate_work, legoev3_uart_change_bitrate);
	hrtimer_init(&port->keep_alive_timer, HRTIMER_BASE_MONOTONIC, HRTIMER_MODE_REL);
	port->keep_alive_timer.function = legoev3_uart_keep_alive_timer_callback;
	tasklet_init(&port->keep_alive_tasklet, legoev3_uart_send_keep_alive,
		     (unsigned long)tty);
	tty->disc_data = port;

	/* set baud rate and other port settings */
	mutex_lock(&tty->termios_mutex);
	tty->termios->c_iflag &=
		 ~(IGNBRK	/* disable ignore break */
		| BRKINT	/* disable break causes interrupt */
		| PARMRK	/* disable mark parity errors */
		| ISTRIP	/* disable clear high bit of input characters */
		| INLCR		/* disable translate NL to CR */
		| IGNCR		/* disable ignore CR */
		| ICRNL		/* disable translate CR to NL */
		| IXON);	/* disable enable XON/XOFF flow control */

	/* disable postprocess output characters */
	tty->termios->c_oflag &= ~OPOST;

	tty->termios->c_lflag &=
		 ~(ECHO		/* disable echo input characters */
		| ECHONL	/* disable echo new line */
		| ICANON	/* disable erase, kill, werase, and rprnt
				   special characters */
		| ISIG		/* disable interrupt, quit, and suspend special
				   characters */
		| IEXTEN);	/* disable non-POSIX special characters */

	/* 2400 baud, 8bits, no parity, 1 stop */
	tty->termios->c_cflag = B2400 | CS8 | CREAD | HUPCL | CLOCAL;
	tty->ops->set_termios(tty, &old_termios);
	tty->ops->tiocmset(tty, 0, ~0); /* clear all */
	mutex_unlock(&tty->termios_mutex);

	tty->receive_room = 65536;

	/* flush any existing data in the buffer */
	if (tty->ldisc->ops->flush_buffer)
		tty->ldisc->ops->flush_buffer(tty);
	tty_driver_flush_buffer(tty);

	return 0;
}

static void legoev3_uart_close(struct tty_struct *tty)
{
	struct legoev3_uart_port_data *port = tty->disc_data;

	if (port->sensor) {
		sysfs_remove_bin_file(&port->sensor->dev.kobj,
				      &dev_bin_attr_bin_data);
		legoev3_port_device_unregister(port->sensor);
	}
	cancel_delayed_work_sync(&port->send_ack_work);
	cancel_delayed_work_sync(&port->change_bitrate_work);
	hrtimer_cancel(&port->keep_alive_timer);
	tasklet_kill(&port->keep_alive_tasklet);
	tty->disc_data = NULL;
	kfree(port);
}

static int legoev3_uart_ioctl(struct tty_struct *tty, struct file *file,
			      unsigned int cmd, unsigned long arg)
{
	return tty_mode_ioctl(tty, file, cmd, arg);
}

static void legoev3_uart_receive_buf(struct tty_struct *tty,
				     const unsigned char *cp,
				     char *fp, int count)
{
	struct legoev3_uart_port_data *port = tty->disc_data;
	int i = 0;
	int j, speed;
	u8 cmd, cmd2, type, mode, msg_type, msg_size, chksum;

#ifdef DEBUG
	printk("received: ");
	for (i = 0; i < count; i++)
		printk("0x%02x ", cp[i]);
	printk(" (%d)\n", count);
	i=0;
#endif

	/*
	 * To get in sync with the data stream from the sensor, we look
	 * for a valid TYPE command.
	 */
	while (!port->synced) {
		if (i + 2 >= count)
			return;
		cmd = cp[i++];
		if (cmd != (LEGOEV3_UART_MSG_TYPE_CMD | LEGOEV3_UART_CMD_TYPE))
			continue;
		type = cp[i];
		if (!type || type > LEGOEV3_UART_TYPE_MAX)
			continue;
		chksum = 0xFF ^ cmd ^ type;
		if (cp[i+1] != chksum)
			continue;
		port->num_modes = 1;
		port->num_view_modes = 1;
		for (j = 0; j <= LEGOEV3_UART_MODE_MAX; j++)
			port->mode_info[j] = legoev3_uart_default_mode_info;
		port->type = type;
		port->info_flags = LEGOEV3_UART_INFO_FLAG_CMD_TYPE;
		port->synced = 1;
		port->info_done = 0;
		port->write_ptr = 0;
		port->data_rec = 0;
		port->num_data_err = 0;
		i += 2;
	}
	if (!port->synced)
		return;

	/*
	 * Once we are synced, we keep reading data until we have read
	 * a complete command.
	 */
	while (i < count) {
		if (port->write_ptr >= LEGOEV3_UART_BUFFER_SIZE)
			goto err_invalid_state;
		port->buffer[port->write_ptr++] = cp[i++];
	}

	/*
	 * Process all complete messages that have been received.
	 */
	while ((msg_size = legoev3_uart_msg_size(port->buffer[0]))
	        <= port->write_ptr)
	{
#ifdef DEBUG
		printk("processing: ");
		for (i = 0; i < port->write_ptr; i++)
			printk("0x%02x ", port->buffer[i]);
		printk(" (%d)\n", port->write_ptr);
		printk("msg_size:%d\n", msg_size);
#endif
		/*
		 * The IR sensor (type 33) sends a checksum (0xFF) after SYNC
		 * (0x00). If these two bytes get split between two interrupts,
		 * then it will throw us off and prevent the sensor from being
		 * recognized. So, if the first byte is 0xFF, we just ignore it
		 * and continue with our loop.
		 */
		if (port->buffer[0] == 0xFF) {
			msg_size = 1;
			goto err_split_sync_checksum;
		}
		msg_type = port->buffer[0] & LEGOEV3_UART_MSG_TYPE_MASK;
		cmd = port->buffer[0] & LEGOEV3_UART_MSG_CMD_MASK;
		mode = cmd;
		cmd2 = port->buffer[1];
		if (msg_size > 1) {
			chksum = 0xFF;
			for (i = 0; i < msg_size - 1; i++)
				chksum ^= port->buffer[i];
			debug_pr("chksum:%d, actual:%d\n",
			         chksum, port->buffer[msg_size - 1]);
			/*
			 * The LEGO EV3 color sensor (type 29) sends bad checksums
			 * for RGB-RAW data (mode 4). The check here could be
			 * improved if someone can find a pattern.
			 */
			if (chksum != port->buffer[msg_size - 1]
			    && port->type != 29 && port->buffer[0] != 0xDC)
			{
				port->last_err = "Bad checksum.";
				if (port->info_done) {
					port->num_data_err++;
					goto err_bad_data_msg_checksum;
				}
				else
					goto err_invalid_state;
			}
		}
		switch (msg_type) {
		case LEGOEV3_UART_MSG_TYPE_SYS:
			debug_pr("SYS:%d\n", port->buffer[0] & LEGOEV3_UART_MSG_CMD_MASK);
			switch(cmd) {
			case LEGOEV3_UART_SYS_SYNC:
				/* IR sensor (type 33) sends checksum after SYNC */
				if (msg_size > 1 && (cmd ^ cmd2) == 0xFF)
					msg_size++;
				break;
			case LEGOEV3_UART_SYS_ACK:
				if (!port->num_modes) {
					port->last_err = "Received ACK before all mode INFO.";
					goto err_invalid_state;
				}
				if ((port->info_flags & LEGOEV3_UART_INFO_FLAG_REQUIRED)
				    != LEGOEV3_UART_INFO_FLAG_REQUIRED)
				{
					port->last_err = "Did not receive all required INFO.";
					goto err_invalid_state;
				}
				schedule_delayed_work(&port->send_ack_work,
				                      msecs_to_jiffies(LEGOEV3_UART_SEND_ACK_DELAY));
				port->info_done = 1;
				break;
			}
			break;
		case LEGOEV3_UART_MSG_TYPE_CMD:
			debug_pr("CMD:%d\n", cmd);
			switch (cmd) {
			case LEGOEV3_UART_CMD_MODES:
				if (test_and_set_bit(LEGOEV3_UART_INFO_BIT_CMD_MODES,
						     &port->info_flags))
				{
					port->last_err = "Received duplicate modes INFO.";
					goto err_invalid_state;
				}
				if (!cmd2 || cmd2 > LEGOEV3_UART_MODE_MAX) {
					port->last_err = "Number of modes is out of range.";
					goto err_invalid_state;
				}
				port->num_modes = cmd2 + 1;
				if (msg_size > 3)
					port->num_view_modes = port->buffer[2] + 1;
				else
					port->num_view_modes = port->num_modes;
				debug_pr("num_modes:%d, num_view_modes:%d\n",
					 port->num_modes, port->num_view_modes);
				break;
			case LEGOEV3_UART_CMD_SPEED:
				if (test_and_set_bit(LEGOEV3_UART_INFO_BIT_CMD_SPEED,
						     &port->info_flags))
				{
					port->last_err = "Received duplicate speed INFO.";
					goto err_invalid_state;
				}
				speed = *(int*)(port->buffer + 1);
				if (speed < LEGOEV3_UART_SPEED_MIN
				    || speed > LEGOEV3_UART_SPEED_MAX)
				{
					port->last_err = "Speed is out of range.";
					goto err_invalid_state;
				}
				port->new_baud_rate = speed;
				debug_pr("speed:%d\n", speed);
				break;
			default:
				port->last_err = "Unknown command.";
				goto err_invalid_state;
			}
			break;
		case LEGOEV3_UART_MSG_TYPE_INFO:
			debug_pr("INFO:%d, mode:%d\n", cmd2, mode);
			switch (cmd2) {
			case LEGOEV3_UART_INFO_NAME:
				port->info_flags &= ~LEGOEV3_UART_INFO_FLAG_ALL_INFO;
				if (port->buffer[2] < 'A' || port->buffer[2] > 'z') {
					port->last_err = "Invalid name INFO.";
					goto err_invalid_state;
				}
				/*
				 * Name may not have null terminator and we
				 * are done with the checksum at this point
				 * so we are writing 0 over the checksum to
				 * ensure a null terminator for the string
				 * functions.
				 */
				port->buffer[msg_size - 1] = 0;
				if (strlen(port->buffer + 2) > LEGOEV3_UART_NAME_SIZE) {
					port->last_err = "Name is too long.";
					goto err_invalid_state;
				}
				snprintf(port->mode_info[mode].name,
				         LEGOEV3_UART_NAME_SIZE + 1, "%s",
				         port->buffer + 2);
				port->mode = mode;
				port->info_flags |= LEGOEV3_UART_INFO_FLAG_INFO_NAME;
				debug_pr("mode %d name:%s\n",
				       mode, port->mode_info[mode].name);
				break;
			case LEGOEV3_UART_INFO_RAW:
				if (port->mode != mode) {
					port->last_err = "Received INFO for incorrect mode.";
					goto err_invalid_state;
				}
				if (test_and_set_bit(LEGOEV3_UART_INFO_BIT_INFO_RAW,
						     &port->info_flags))
				{
					port->last_err = "Received duplicate raw scaling INFO.";
					goto err_invalid_state;
				}
				port->mode_info[mode].raw_min =
						*(unsigned *)(port->buffer + 2);
				port->mode_info[mode].raw_max =
						*(unsigned *)(port->buffer + 6);
				debug_pr("mode %d raw_min:%08x, raw_max:%08x\n",
				       mode, port->mode_info[mode].raw_min,
				       port->mode_info[mode].raw_max);
				break;
			case LEGOEV3_UART_INFO_PCT:
				if (port->mode != mode) {
					port->last_err = "Received INFO for incorrect mode.";
					goto err_invalid_state;
				}
				if (test_and_set_bit(LEGOEV3_UART_INFO_BIT_INFO_PCT,
						     &port->info_flags))
				{
					port->last_err = "Received duplicate percent scaling INFO.";
					goto err_invalid_state;
				}
				port->mode_info[mode].pct_min =
						*(unsigned *)(port->buffer + 2);
				port->mode_info[mode].pct_max =
						*(unsigned *)(port->buffer + 6);
				debug_pr("mode %d pct_min:%08x, pct_max:%08x\n",
				       mode, port->mode_info[mode].pct_min,
				       port->mode_info[mode].pct_max);
				break;
			case LEGOEV3_UART_INFO_SI:
				if (port->mode != mode) {
					port->last_err = "Received INFO for incorrect mode.";
					goto err_invalid_state;
				}
				if (test_and_set_bit(LEGOEV3_UART_INFO_BIT_INFO_SI,
						     &port->info_flags))
				{
					port->last_err = "Received duplicate SI scaling INFO.";
					goto err_invalid_state;
				}
				port->mode_info[mode].si_min =
						*(unsigned *)(port->buffer + 2);
				port->mode_info[mode].si_max =
						*(unsigned *)(port->buffer + 6);
				debug_pr("mode %d si_min:%08x, si_max:%08x\n",
				       mode, port->mode_info[mode].si_min,
				       port->mode_info[mode].si_max);
				break;
			case LEGOEV3_UART_INFO_UNITS:
				if (port->mode != mode) {
					port->last_err = "Received INFO for incorrect mode.";
					goto err_invalid_state;
				}
				if (test_and_set_bit(LEGOEV3_UART_INFO_BIT_INFO_UNITS,
						     &port->info_flags))
				{
					port->last_err = "Received duplicate SI units INFO.";
					goto err_invalid_state;
				}
				/*
				 * Units may not have null terminator and we
				 * are done with the checksum at this point
				 * so we are writing 0 over the checksum to
				 * ensure a null terminator for the string
				 * functions.
				 */
				port->buffer[msg_size - 1] = 0;
				snprintf(port->mode_info[mode].units,
					 LEGOEV3_UART_UNITS_SIZE + 1, "%s",
					 port->buffer + 2);
				debug_pr("mode %d units:%s\n",
				       mode, port->mode_info[mode].units);
				break;
			case LEGOEV3_UART_INFO_FORMAT:
				if (port->mode != mode) {
					port->last_err = "Received INFO for incorrect mode.";
					goto err_invalid_state;
				}
				if (test_and_set_bit(LEGOEV3_UART_INFO_BIT_INFO_FORMAT,
						     &port->info_flags))
				{
					port->last_err = "Received duplicate format INFO.";
					goto err_invalid_state;
				}
				port->mode_info[mode].data_sets = port->buffer[2];
				if (!port->mode_info[mode].data_sets) {
					port->last_err = "Invalid number of data sets.";
					goto err_invalid_state;
				}
				if (msg_size < 7) {
					port->last_err = "Invalid format message size.";
					goto err_invalid_state;
				}
				if ((port->info_flags & LEGOEV3_UART_INFO_FLAG_REQUIRED)
						!= LEGOEV3_UART_INFO_FLAG_REQUIRED) {
					port->last_err = "Did not receive all required INFO.";
					goto err_invalid_state;
				}
				port->mode_info[mode].format = port->buffer[3];
				if (port->mode) {
					port->mode--;
					port->mode_info[mode].figures = port->buffer[4];
					port->mode_info[mode].decimals = port->buffer[5];
					/* TODO: copy IR Seeker hack from lms2012 */
				}
				debug_pr("mode %d data_sets:%d, format:%d, figures:%d, decimals:%d\n",
				       mode, port->mode_info[mode].data_sets,
				       port->mode_info[mode].format,
				       port->mode_info[mode].figures,
				       port->mode_info[mode].decimals);
				break;
			}
			break;
		case LEGOEV3_UART_MSG_TYPE_DATA:
			debug_pr("DATA:%d\n", port->buffer[0] & LEGOEV3_UART_MSG_CMD_MASK);
			if (!port->info_done) {
				port->last_err = "Received DATA before INFO was complete.";
				goto err_invalid_state;
			}
			port->mode = mode;
			memcpy(port->mode_info[mode].raw_data,
			       port->buffer + 1, msg_size - 2);
			port->data_rec = 1;
			if (port->num_data_err)
				port->num_data_err--;
			break;
		}

err_bad_data_msg_checksum:
		if (port->info_done && port->num_data_err > LEGOEV3_UART_MAX_DATA_ERR)
			goto err_invalid_state;
err_split_sync_checksum:
		/*
		 * If there is leftover data, we move it to the beginning
		 * of the buffer.
		 */
		for (i = 0; i + msg_size < port->write_ptr; i++)
			port->buffer[i] = port->buffer[i + msg_size];
		port->write_ptr = i;
	}
	return;

err_invalid_state:
	port->synced = 0;
	port->new_baud_rate = LEGOEV3_UART_SPEED_MIN;
	schedule_delayed_work(&port->change_bitrate_work,
	                      msecs_to_jiffies(LEGOEV3_UART_SET_BITRATE_DELAY));
}

static void legoev3_uart_write_wakeup(struct tty_struct *tty)
{
	debug_pr("%s\n", __func__);
}

static struct tty_ldisc_ops legoev3_uart_ldisc = {
	.magic			= TTY_LDISC_MAGIC,
	.name			= "n_legoev3",
	.open			= legoev3_uart_open,
	.close			= legoev3_uart_close,
	.ioctl			= legoev3_uart_ioctl,
	.receive_buf		= legoev3_uart_receive_buf,
	.write_wakeup		= legoev3_uart_write_wakeup,
	.owner			= THIS_MODULE,
};

static int __init legoev3_uart_init(void)
{
	int err;

	err = tty_register_ldisc(N_LEGOEV3, &legoev3_uart_ldisc);
	if (err) {
		pr_err("Could not register LEGOEV3 line discipline. (%d)\n",
			err);
		return err;
	}

	pr_info("Registered LEGOEV3 line discipline. (%d)\n", N_LEGOEV3);

	return 0;
}
module_init(legoev3_uart_init);

static void __exit legoev3_uart_exit(void)
{
	int err;

	err = tty_unregister_ldisc(N_LEGOEV3);
	if (err)
		pr_err("Could not unregister LEGOEV3 line discipline. (%d)\n",
			err);
}
module_exit(legoev3_uart_exit);

MODULE_DESCRIPTION("tty line discipline for LEGO Mindstorms EV3 sensors");
MODULE_AUTHOR("David Lechner <david@lechnology.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS_LDISC(N_LEGOEV3);
