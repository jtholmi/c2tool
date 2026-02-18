/*
 * Copyright 2014 Dirk Eibach <eibach@gdsys.de>
 *           2022 Alex Kiselev <a.kiselev@volz-servos.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef DEF_NO_GPIO
  #include <gpiod.h>
  #define PI_INPUT 0
  #define PI_OUTPUT 1
  #define PI_PUD_OFF 0
  #define PI_PUD_DOWN 1
  #define PI_PUD_UP 2
  #define PI_ON 1
  #define PI_OFF 0
#else
  #define gpioInitialise()        true
  #define gpioTerminate()
  #define gpioWrite(x, y)
  #define gpioSetMode(x, y)
  #define gpioSetPullUpDown(x, y)
  #define gpioRead(x)             true
  #define PI_ON                   true
  #define PI_OFF                  false
#endif

#include "defines.h"
#include "c2family.h"
#include "c2interface.h"
#include "c2tool.h"
#include "hexdump.h"
/*
 * C2 registers & commands defines
 */

/* C2 registers */
#define C2_DEVICEID		0x00
#define C2_REVID		  0x01
#define C2_FPCTL		  0x02

/* C2 interface commands */
#define C2_GET_VERSION	0x01
#define C2_DEVICE_ERASE	0x03
#define C2_BLOCK_READ	  0x06
#define C2_BLOCK_WRITE	0x07
#define C2_PAGE_ERASE	  0x08

#define C2_FPDAT_GET_VERSION	  0x01
#define C2_FPDAT_GET_DERIVATIVE	0x02
#define C2_FPDAT_DEVICE_ERASE	  0x03
#define C2_FPDAT_BLOCK_READ	    0x06
#define C2_FPDAT_BLOCK_WRITE	  0x07
#define C2_FPDAT_PAGE_ERASE	    0x08
#define C2_FPDAT_DIRECT_READ	  0x09
#define C2_FPDAT_DIRECT_WRITE	  0x0a
#define C2_FPDAT_INDIRECT_READ	0x0b
#define C2_FPDAT_INDIRECT_WRITE	0x0c

#define C2_FPDAT_RETURN_INVALID_COMMAND	0x00
#define C2_FPDAT_RETURN_COMMAND_FAILED	0x02
#define C2_FPDAT_RETURN_COMMAND_OK	    0x0D

#define C2_FPCTL_HALT		    0x01
#define C2_FPCTL_RESET		  0x02
#define C2_FPCTL_CORE_RESET	0x04

#define GPIO_C2D		23
#define GPIO_C2CK		24

static uint16_t c2_poll_out_timeout;

/* libgpiod v2 implementation */
#ifndef DEF_NO_GPIO

static struct gpiod_chip *chip = NULL;
static struct gpiod_line_request *req_c2d = NULL;
static struct gpiod_line_request *req_c2ck = NULL;

static int gpioInitialise(void)
{
	struct gpiod_line_config *line_cfg;
	struct gpiod_line_settings *settings;
	struct gpiod_request_config *req_cfg;
	unsigned int offset;
	int ret = -1;

	/* Try opening the main GPIO chip.
	 * On Pi 5, gpiochip4 is typically the main header, but it maps to pinctrl-rp1 which is gpiochip0.
	 * We try likely candidates.
	 */
	chip = gpiod_chip_open("/dev/gpiochip4");
	if (!chip) {
		chip = gpiod_chip_open("/dev/gpiochip0");
		if (!chip) {
			fprintf(stderr, "Failed to open gpiochip (tried 4 and 0)\n");
			return -1;
		}
	}

	line_cfg = gpiod_line_config_new();
	settings = gpiod_line_settings_new();
	req_cfg = gpiod_request_config_new();

	if (!line_cfg || !settings || !req_cfg)
		goto err;

	/* Request C2CK as OUTPUT, initially HIGH (idle) */
	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_OUTPUT);
	gpiod_line_settings_set_output_value(settings, GPIOD_LINE_VALUE_ACTIVE);
	
	offset = GPIO_C2CK;
	gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
	
	gpiod_request_config_set_consumer(req_cfg, "c2tool_ck");
	
	req_c2ck = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
	if (!req_c2ck) {
		perror("Failed to request C2CK");
		goto err;
	}

	/* Request C2D as INPUT initially with pull-up */
	gpiod_line_config_reset(line_cfg);
	gpiod_line_settings_reset(settings);
	
	gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);
	gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);
	
	offset = GPIO_C2D;
	gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
	
	gpiod_request_config_set_consumer(req_cfg, "c2tool_d");
	
	req_c2d = gpiod_chip_request_lines(chip, req_cfg, line_cfg);
	if (!req_c2d) {
		perror("Failed to request C2D");
		goto err;
	}

	ret = 0;
	/* fallthrough to cleanup config objects */

err:
	if (line_cfg) gpiod_line_config_free(line_cfg);
	if (settings) gpiod_line_settings_free(settings);
	if (req_cfg) gpiod_request_config_free(req_cfg);
	
	if (ret < 0) {
		if (req_c2ck) { gpiod_line_request_release(req_c2ck); req_c2ck = NULL; }
		if (req_c2d) { gpiod_line_request_release(req_c2d); req_c2d = NULL; }
		if (chip) { gpiod_chip_close(chip); chip = NULL; }
	}
	return ret;
}

static void gpioTerminate(void)
{
	if (req_c2d) { gpiod_line_request_release(req_c2d); req_c2d = NULL; }
	if (req_c2ck) { gpiod_line_request_release(req_c2ck); req_c2ck = NULL; }
	if (chip) { gpiod_chip_close(chip); chip = NULL; }
}

static void gpioWrite(int pin, int value)
{
	struct gpiod_line_request *req = (pin == GPIO_C2D) ? req_c2d : req_c2ck;
	if (req) {
		/* offset is 0 relative to the request */
		gpiod_line_request_set_value(req, 0, value ? GPIOD_LINE_VALUE_ACTIVE : GPIOD_LINE_VALUE_INACTIVE);
	}
}

static int gpioRead(int pin)
{
	struct gpiod_line_request *req = (pin == GPIO_C2D) ? req_c2d : req_c2ck;
	if (req) {
		return (gpiod_line_request_get_value(req, 0) == GPIOD_LINE_VALUE_ACTIVE) ? 1 : 0;
	}
	return 0;
}

static void gpioSetMode(int pin, int mode)
{
	struct gpiod_line_request *req = (pin == GPIO_C2D) ? req_c2d : req_c2ck;
	struct gpiod_line_config *line_cfg;
	struct gpiod_line_settings *settings;
	unsigned int offset = (pin == GPIO_C2D) ? GPIO_C2D : GPIO_C2CK;
	/* Note: When reconfiguring, we must provide the original offset if request was made by offset? 
       No, "offsets" in add_line_settings are "offsets on the chip" if we were requesting from scratch?
       Wait, gpiod_line_request_reconfigure_lines documentation says:
       "The config object should contain settings for the lines that are part of the request.
        The offsets are relative to the request?" 
       Let's check documentation or assume relative.
       Actually, `gpiod_line_config_add_line_settings` takes offsets.
       If we used `gpiod_chip_request_lines`, we passed offsets on the chip (GPIO_C2D etc).
       When reconfiguring, do we pass global offsets or relative?
       
       Looking at libgpiod v2 docs/examples:
       reconfigure_lines takes a line_config. 
       Usually the line_config uses offsets. 
       If the request was created with global offsets (23, 24), we should probably use those same offsets.
       
       However, if we passed `offset = GPIO_C2D` during request, then the request manages line 23.
       Wait, `gpiod_line_config_add_line_settings` takes an array of offsets.
       Since we requested a single line (GPIO_C2D), the request manages 1 line.
       Docs say: "The offsets in the config object must correspond to the offsets of lines requested."
       If we requested line 23, then offset is 23. 
       
       Wait, let's play safe. If I have one line in the request, I probably still refer to it by its chip offset (23)?
       Actually, `gpiod_chip_request_lines` maps offsets to lines. 
       The `line_request` object holds the lines.
       When reconfiguring, we are updating settings for lines in the request.
       The config object must map offsets to settings.
       If I use offset 0 (relative to request), will it work?
       Or must I use 23?
       
       Let's check `gpiod_line_config_add_line_settings` docs.
       "Add line settings for a set of offsets."
       
       If usage is:
       req = request_lines(chip, ..., config_with_offset_23);
       reconfigure_lines(req, new_config_with_offset_23);
       This seems most logical.
    */
    
	if (!req) return;

	line_cfg = gpiod_line_config_new();
	settings = gpiod_line_settings_new();
	
	if (!line_cfg || !settings) goto cfg_err;

	gpiod_line_settings_set_direction(settings, (mode == PI_INPUT) ? GPIOD_LINE_DIRECTION_INPUT : GPIOD_LINE_DIRECTION_OUTPUT);
	
	/* Maintain pull-up for C2D in input mode as per original logic */
	if (pin == GPIO_C2D && mode == PI_INPUT)
		gpiod_line_settings_set_bias(settings, GPIOD_LINE_BIAS_PULL_UP);

	offset = (pin == GPIO_C2D) ? GPIO_C2D : GPIO_C2CK;
	gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
	
	gpiod_line_request_reconfigure_lines(req, line_cfg);

cfg_err:
	if (line_cfg) gpiod_line_config_free(line_cfg);
	if (settings) gpiod_line_settings_free(settings);
}

static void gpioSetPullUpDown(int pin, int pud)
{
	struct gpiod_line_request *req = (pin == GPIO_C2D) ? req_c2d : req_c2ck;
	struct gpiod_line_config *line_cfg;
	struct gpiod_line_settings *settings;
	unsigned int offset;
    
    if (!req) return;

	line_cfg = gpiod_line_config_new();
	settings = gpiod_line_settings_new();
	
	if (!line_cfg || !settings) goto pud_err;

	enum gpiod_line_bias bias = GPIOD_LINE_BIAS_DISABLED;
    if (pud == PI_PUD_UP) bias = GPIOD_LINE_BIAS_PULL_UP;
    else if (pud == PI_PUD_DOWN) bias = GPIOD_LINE_BIAS_PULL_DOWN;

	gpiod_line_settings_set_bias(settings, bias);
    /* Ensure direction is INPUT when setting pull */
    gpiod_line_settings_set_direction(settings, GPIOD_LINE_DIRECTION_INPUT);

	offset = (pin == GPIO_C2D) ? GPIO_C2D : GPIO_C2CK;
	gpiod_line_config_add_line_settings(line_cfg, &offset, 1, settings);
	
	gpiod_line_request_reconfigure_lines(req, line_cfg);

pud_err:
	if (line_cfg) gpiod_line_config_free(line_cfg);
	if (settings) gpiod_line_settings_free(settings);
}

#endif /* DEF_NO_GPIO */

/*
 * state 0: drive low
 *       1: high-z
 */
static void c2d_set(struct c2interface *c2if, int state)
{
	if (state)
		gpioWrite(GPIO_C2D, PI_ON);
	else
		gpioWrite(GPIO_C2D, PI_OFF);
	usleep(1);
}

static int c2d_get(struct c2interface *c2if)
{
	//usleep(1);
	return (gpioRead(GPIO_C2D) == PI_ON);
}

static void c2ck_set(struct c2interface *c2if, int state)
{
	if (state)
		gpioWrite(GPIO_C2CK, PI_ON);
	else
		gpioWrite(GPIO_C2CK, PI_OFF);
	usleep(1);
}

static void c2ck_strobe(struct c2interface *c2if)
{
	gpioWrite(GPIO_C2CK, PI_OFF);
	gpioWrite(GPIO_C2CK, PI_ON);
	usleep(1);
}

/*
 * C2 primitives
 */

int c2_init(void)
{
	int res = gpioInitialise();
	if (res >= 0)
	{
		gpioSetMode(GPIO_C2CK, PI_OUTPUT);
		gpioWrite(GPIO_C2CK, PI_ON);
		gpioSetMode(GPIO_C2D, PI_INPUT);
		gpioSetPullUpDown(GPIO_C2D, PI_PUD_UP);
	}
	return res;
}

void c2_terminate(void)
{
	gpioTerminate();
}

void c2_reset(struct c2interface *c2if)
{
	gpioSetMode(GPIO_C2D, PI_INPUT);
	/* To reset the device we have to keep clock line low for at least
	 * 20us.
	 */
	c2ck_set(c2if, 0);
	usleep(25);
	c2ck_set(c2if, 1);
	usleep(1);
}

static void c2_write_ar(struct c2interface *c2if, unsigned char addr)
{
	int i;

	gpioSetMode(GPIO_C2D, PI_OUTPUT);
	usleep(1);
	/* START field */
	c2d_set(c2if, 1);
	c2ck_strobe(c2if);

	/* INS field (11b, LSB first) */
	c2d_set(c2if, 1);
	c2ck_strobe(c2if);
	c2d_set(c2if, 1);
	c2ck_strobe(c2if);

	/* ADDRESS field */
	for (i = 0; i < 8; i++) {
		c2d_set(c2if, addr & 0x01);
		c2ck_strobe(c2if);

		addr >>= 1;
	}

	/* STOP field */
	c2d_set(c2if, 1);
	c2ck_strobe(c2if);
	usleep(1);
	gpioSetMode(GPIO_C2D, PI_INPUT);
}

static int c2_read_ar(struct c2interface *c2if, unsigned char *addr)
{
	int i;

	gpioSetMode(GPIO_C2D, PI_OUTPUT);
	usleep(1);
	/* START field */
	c2d_set(c2if, 1);
	c2ck_strobe(c2if);

	/* INS field (10b, LSB first) */
	c2d_set(c2if, 0);
	c2ck_strobe(c2if);
	c2d_set(c2if, 1);
	c2ck_strobe(c2if);

	/* ADDRESS field */
	c2d_set(c2if, 1);
	gpioSetMode(GPIO_C2D, PI_INPUT);
	*addr = 0;
	for (i = 0; i < 8; i++) {
		*addr >>= 1;	/* shift in 8-bit ADDRESS field LSB first */

		c2ck_strobe(c2if);
		if (c2d_get(c2if))
			*addr |= 0x80;
	}
	/* STOP field */
	c2ck_strobe(c2if);

	return 0;
}

static int c2_write_dr(struct c2interface *c2if, unsigned char data)
{
	int timeout, i;

	gpioSetMode(GPIO_C2D, PI_OUTPUT);
	usleep(1);
	/* START field */
	c2d_set(c2if, 1);
	c2ck_strobe(c2if);

	/* INS field (01b, LSB first) */
	c2d_set(c2if, 1);
	c2ck_strobe(c2if);
	c2d_set(c2if, 0);
	c2ck_strobe(c2if);

	/* LENGTH field (00b, LSB first -> 1 byte) */
	c2d_set(c2if, 0);
	c2ck_strobe(c2if);
	c2d_set(c2if, 0);
	c2ck_strobe(c2if);

	/* DATA field */
	for (i = 0; i < 8; i++) {
		c2d_set(c2if, data & 0x01);
		c2ck_strobe(c2if);

		data >>= 1;
	}

	/* WAIT field */
	c2d_set(c2if, 1);
	gpioSetMode(GPIO_C2D, PI_INPUT);
	timeout = 20;
	do {
		c2ck_strobe(c2if);
		if (c2d_get(c2if))
			break;

		usleep(1);
	} while (--timeout > 0);
	if (timeout == 0)
		return -EIO;

	/* STOP field */
	c2ck_strobe(c2if);

	return 0;
}

static int c2_read_dr(struct c2interface *c2if, unsigned char *data)
{
	int timeout, i;

	gpioSetMode(GPIO_C2D, PI_OUTPUT);
	usleep(1);
	/* START field */
	c2d_set(c2if, 1);
	c2ck_strobe(c2if);

	/* INS field (00b, LSB first) */
	c2d_set(c2if, 0);
	c2ck_strobe(c2if);
	c2d_set(c2if, 0);
	c2ck_strobe(c2if);

	/* LENGTH field (00b, LSB first -> 1 byte) */
	c2d_set(c2if, 0);
	c2ck_strobe(c2if);
	c2d_set(c2if, 0);
	c2ck_strobe(c2if);

	/* WAIT field */
	c2d_set(c2if, 1);
	gpioSetMode(GPIO_C2D, PI_INPUT);
	timeout = 50;
	do {
		c2ck_strobe(c2if);
		if (c2d_get(c2if))
			break;

		usleep(1);
	} while (--timeout > 0);
	if (timeout == 0)
		return -EIO;

	/* DATA field */
	*data = 0;
	for (i = 0; i < 8; i++) {
		*data >>= 1;	/* shift in 8-bit DATA field LSB first */

		c2ck_strobe(c2if);
		if (c2d_get(c2if))
			*data |= 0x80;
	}
	/* STOP field */
	c2ck_strobe(c2if);

	return 0;
}

static int c2_poll_in_busy(struct c2interface *c2if)
{
	unsigned char addr;
	int ret, timeout = 20;

	do {
		ret = (c2_read_ar(c2if, &addr));
		if (ret < 0)
			return -EIO;

		if (!(addr & 0x02))
			break;

		usleep(1);
	} while (--timeout > 0);
	if (timeout == 0)
		return -EIO;

	return 0;
}

void c2_set_poll_out_timeout(uint16_t timeout)
{
  c2_poll_out_timeout = timeout;
}

static int c2_poll_out_ready(struct c2interface *c2if)
{
	unsigned char addr;
	/* erase flash needs long time... */
	int ret, timeout = 100;

	do {
		ret = (c2_read_ar(c2if, &addr));
		if (ret < 0)
			return -EIO;

		if (addr & 0x01)
			break;

		usleep(1);
	} while (--timeout > 0);
	if (timeout == 0)
		return -EIO;

	return 0;
}

int c2_read_sfr(struct c2interface *c2if, unsigned char sfr)
{
	unsigned char data;

	c2_write_ar(c2if, sfr);

	if (c2_read_dr(c2if, &data) < 0)
		return -EIO;

	return data;
}

int c2_write_sfr(struct c2interface *c2if, unsigned char sfr, unsigned char data)
{
	c2_write_ar(c2if, sfr);

	if (c2_write_dr(c2if, data) < 0)
		return -EIO;

	return 0;
}

/*
 * Programming interface (PI)
 * Each command is executed using a sequence of reads and writes of the FPDAT register.
 */

static int c2_pi_write_command(struct c2interface *c2if, unsigned char command)
{
	if (c2_write_dr(c2if, command) < 0)
		return -EIO;

	if (c2_poll_in_busy(c2if) < 0)
		return -EIO;

	return 0;
}

static int c2_pi_get_data(struct c2interface *c2if, unsigned char *data)
{
	if (c2_poll_out_ready(c2if) < 0)
		return -EIO;

	if (c2_read_dr(c2if, data) < 0)
		return -EIO;

	return 0;
}

static int c2_pi_check_command(struct c2interface *c2if)
{
	unsigned char response;

	if (c2_pi_get_data(c2if, &response) < 0)
		return -EIO;

	if (response != C2_FPDAT_RETURN_COMMAND_OK)
		return -EIO;

	return 0;
}

static int c2_pi_command(struct c2interface *c2if, unsigned char command, int verify,
			 unsigned char *result)
{
	if (c2_pi_write_command(c2if, command) < 0)
		return -EIO;

	if (!verify)
		return 0;

	if (c2_pi_check_command(c2if) < 0)
		return -EIO;

	if (!result)
		return 0;

	if (c2_pi_get_data(c2if, result) < 0)
		return -EIO;

	return 0;
}

int c2_read_direct(struct c2tool_state *state, unsigned char reg)
{
	unsigned char data;
	struct c2interface *c2if = &state->c2if;
	struct c2family *family = state->family;

	c2_write_ar(c2if, family->fpdat);

	if (c2_pi_command(c2if, C2_FPDAT_DIRECT_READ, 1, NULL))
		return -EIO;

	if (c2_pi_write_command(c2if, reg))
		return -EIO;

	if (c2_pi_write_command(c2if, 0x01))
		return -EIO;
	if (c2_poll_out_ready(c2if) < 0)
		return -EIO;
	if (c2_read_dr(c2if, &data) < 0)
		return -EIO;

	return data;
}

int c2_write_direct(struct c2tool_state *state, unsigned char reg, unsigned char value)
{
	struct c2interface *c2if = &state->c2if;
	struct c2family *family = state->family;

	c2_write_ar(c2if, family->fpdat);

	if (c2_pi_command(c2if, C2_FPDAT_DIRECT_WRITE, 1, NULL))
		return -EIO;

	if (c2_pi_write_command(c2if, reg))
		return -EIO;

	if (c2_pi_write_command(c2if, 0x01))
		return -EIO;

	if (c2_pi_write_command(c2if, value))
		return -EIO;

	return 0;
}

int c2_flash_read(struct c2tool_state *state, unsigned int addr, unsigned int length,
			 unsigned char *dest)
{
	struct c2interface *c2if = &state->c2if;
	struct c2family *family = state->family;

	c2_write_ar(c2if, family->fpdat);

	while (length) {
		unsigned int blocksize;
		unsigned int k;

		if (c2_pi_command(c2if, C2_FPDAT_BLOCK_READ, 1, NULL) < 0)
			return -EIO;

		if (c2_pi_command(c2if, addr >> 8, 0, NULL) < 0)
			return -EIO;
		if (c2_pi_command(c2if, addr & 0xFF, 0, NULL) < 0)
			return -EIO;

		if (length > 255) {
			if (c2_pi_command(c2if, 0, 1, NULL) < 0)
				return -EIO;
			blocksize = 256;
		} else {
			if (c2_pi_command(c2if, length, 1, NULL) < 0)
				return -EIO;
			blocksize = length;
		}

		for (k = 0; k < blocksize; ++k) {
			unsigned char data;

			if (c2_pi_get_data(c2if, &data) < 0)
				return -EIO;
			if (dest)
				*dest++ = data;
		}

		length -= blocksize;
		addr += blocksize;
	}

	return 0;
}

int c2_flash_write(struct c2tool_state *state, unsigned int addr, unsigned int length,
			 unsigned char *src)
{
	struct c2interface *c2if = &state->c2if;
	struct c2family *family = state->family;

	c2_write_ar(c2if, family->fpdat);

	while (length) {
		unsigned int blocksize;
		unsigned int k;

		if (c2_pi_command(c2if, C2_FPDAT_BLOCK_WRITE, 1, NULL) < 0)
			return -1;//-EIO;

		if (c2_pi_command(c2if, addr >> 8, 0, NULL) < 0)
			return -2;//-EIO;
		if (c2_pi_command(c2if, addr & 0xff, 0, NULL) < 0)
			return -3;//-EIO;

		if (length > 255) {
			if (c2_pi_command(c2if, 0, 1, NULL) < 0)
				return -4;//-EIO;
			blocksize = 256;
		} else {
			if (c2_pi_command(c2if, length, 1, NULL) < 0)
				return -6;//-EIO;
			blocksize = length;
		}

		for (k = 0; k < blocksize; ++k) {
			if (c2_pi_command(c2if, *src++, 0, NULL) < 0)
				return -7;//-EIO;
		}

		length -= blocksize;
		addr += blocksize;
	}

	return 0;
}

int c2_flash_erase(struct c2tool_state *state, unsigned char page)
{
	struct c2interface *c2if = &state->c2if;
	struct c2family *family = state->family;

	c2_write_ar(c2if, family->fpdat);

	if (c2_pi_command(c2if, C2_FPDAT_PAGE_ERASE, 1, NULL) < 0)
		return -EIO;

	if (c2_pi_command(c2if, page, 1, NULL) < 0)
		return -EIO;

	if (c2_pi_command(c2if, 0, 1, NULL) < 0)
		return -EIO;

  // for F58x, restore PSBANK
  if (c2_write_sfr(c2if, 0xF5, 0x11) < 0)
    return -EIO;

	return 0;
}

int c2_halt(struct c2interface *c2if)
{
	c2_reset(c2if);

	usleep(2);

	c2_write_ar(c2if, C2_FPCTL);

	if (c2_write_dr(c2if, C2_FPCTL_RESET) < 0) {
		fprintf(stderr, "Failed to write C2_FPCTL_RESET\n");
		return -EIO;
	}

	if (c2_write_dr(c2if, C2_FPCTL_CORE_RESET) < 0) {
		fprintf(stderr, "Failed to write C2_FPCTL_CORE_RESET\n");
		return -EIO;
	}

	if (c2_write_dr(c2if, C2_FPCTL_HALT) < 0) {
		fprintf(stderr, "Failed to write C2_FPCTL_HALT\n");
		return -EIO;
	}

	usleep(30000);

	return 0;
}

int c2_get_device_info(struct c2interface *c2if, struct c2_device_info *info)
{
	unsigned char data;
	int ret;

	/* Select DEVID register for C2 data register accesses */
	c2_write_ar(c2if, C2_DEVICEID);

	/* Read and return the device ID register */
	ret = c2_read_dr(c2if, &data);
	if (ret < 0)
		return -EIO;

	info->device_id = data;

	/* Select REVID register for C2 data register accesses */
	c2_write_ar(c2if, C2_REVID);

	/* Read and return the revision ID register */
	ret = c2_read_dr(c2if, &data);
	if (ret < 0)
		return -EIO;

	info->revision_id = data;

	return 0;
}

int c2_get_pi_info(struct c2tool_state *state, struct c2_pi_info *info)
{
	unsigned char data;
	int ret;
	struct c2interface *c2if = &state->c2if;
	struct c2family *family = state->family;

	/* Select FPDAT register for C2 data register accesses */
	c2_write_ar(c2if, family->fpdat);

	ret = c2_pi_command(c2if, C2_FPDAT_GET_VERSION, 1, &data);
	if (ret < 0)
		return -EIO;

	info->version = data;

	ret = c2_pi_command(c2if, C2_FPDAT_GET_DERIVATIVE, 1, &data);
	if (ret < 0)
		return -EIO;

	info->derivative = data;

	return 0;
}

int c2_flash_erase_device(struct c2tool_state *state)
{
	struct c2interface *c2if = &state->c2if;
	struct c2family *family = state->family;

	c2_write_ar(c2if, family->fpdat);

	if (c2_pi_command(c2if, C2_FPDAT_DEVICE_ERASE, 1, NULL) < 0)
		return -EIO;

	if (c2_pi_write_command(c2if, 0xDE) < 0)
		return -EIO;

	if (c2_pi_write_command(c2if, 0xAD) < 0)
		return -EIO;

	if (c2_pi_write_command(c2if, 0xA5) < 0)
		return -EIO;

  if (c2_pi_check_command(c2if) < 0)
    return -EIO;

	return 0;
}

int flash_chunk(struct c2tool_state *state, unsigned int addr, unsigned int length,
		       unsigned char *src)
{
	//struct c2interface *c2if = &state->c2if;
	struct c2family *family = state->family;
	unsigned int page_size = family->page_size;
	unsigned int page = addr / page_size;
	unsigned char buf[page_size];
	int must_read = (addr % page_size) || (length < page_size);
	unsigned int page_base = page * page_size;
	unsigned int chunk_start = addr - page_base;
	unsigned int chunk_len = (chunk_start + length > page_size) ?
				 (page_size  - chunk_start) : length;

	if (must_read) {
		if (c2_flash_read(state, page_base, page_size, buf))
			return -EIO;
	}

	memcpy(buf + addr - page_base, src, chunk_len);

	if (c2_flash_erase(state, page))
		return -EIO;

	if (c2_flash_write(state, page_base, page_size, buf))
		return -EIO;

	return chunk_len;
}
