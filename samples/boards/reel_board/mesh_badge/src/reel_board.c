/*
 * Copyright (c) 2018 Phytec Messtechnik GmbH
 * Copyright (c) 2018 Intel Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr.h>
#include <device.h>
#include <gpio.h>
#include <display/cfb.h>
#include <misc/printk.h>
#include <flash.h>
#include <sensor.h>

#include <string.h>
#include <stdio.h>

#include <bluetooth/bluetooth.h>
#include <bluetooth/mesh/access.h>

#include "mesh.h"
#include "board.h"

enum font_size {
	FONT_BIG = 0,
	FONT_MEDIUM = 1,
	FONT_SMALL = 2,
};

enum screen_ids {
	SCREEN_MAIN = 0,
	SCREEN_SENSORS = 1,
	SCREEN_STATS = 2,
	SCREEN_LAST,
};

struct font_info {
	u8_t columns;
} fonts[] = {
	[FONT_BIG] =    { .columns = 12 },
	[FONT_MEDIUM] = { .columns = 16 },
	[FONT_SMALL] =  { .columns = 25 },
};

#define LONG_PRESS_TIMEOUT K_SECONDS(1)

#define STAT_COUNT 128

#define EDGE (GPIO_INT_EDGE | GPIO_INT_DOUBLE_EDGE)

#ifdef SW0_GPIO_FLAGS
#define PULL_UP SW0_GPIO_FLAGS
#else
#define PULL_UP 0
#endif

static struct device *epd_dev;
static bool pressed;
static bool presseds[11];
static u8_t screen_id = SCREEN_MAIN;
static struct device *gpio;
static struct device *gpios[11];
static struct k_delayed_work epd_work;
static struct k_delayed_work long_press_work;
static char str_buf[256];

static struct {
	struct device *dev;
	const char *name;
	u32_t pin;
} leds[] = {
	{ .name = LED0_GPIO_CONTROLLER, .pin = LED0_GPIO_PIN, },
	{ .name = LED1_GPIO_CONTROLLER, .pin = LED1_GPIO_PIN, },
	{ .name = LED2_GPIO_CONTROLLER, .pin = LED2_GPIO_PIN, },
	{ .name = LED3_GPIO_CONTROLLER, .pin = LED3_GPIO_PIN, },
};

struct k_delayed_work led_timer;

static size_t print_line(enum font_size font_size, int row, const char *text,
			 size_t len, bool center)
{
	u8_t font_height, font_width;
	u8_t line[fonts[FONT_SMALL].columns + 1];
	int pad;

	cfb_framebuffer_set_font(epd_dev, font_size);

	len = min(len, fonts[font_size].columns);
	memcpy(line, text, len);
	line[len] = '\0';

	if (center) {
		pad = (fonts[font_size].columns - len) / 2;
	} else {
		pad = 0;
	}

	cfb_get_font_size(epd_dev, font_size, &font_width, &font_height);

	if (cfb_print(epd_dev, line, font_width * pad, font_height * row)) {
		printk("Failed to print a string\n");
	}

	return len;
}

static size_t get_len(enum font_size font, const char *text)
{
	const char *space = NULL;
	size_t i;

	for (i = 0; i <= fonts[font].columns; i++) {
		switch (text[i]) {
		case '\n':
		case '\0':
			return i;
		case ' ':
			space = &text[i];
			break;
		default:
			continue;
		}
	}

	/* If we got more characters than fits a line, and a space was
	 * encountered, fall back to the last space.
	 */
	if (space) {
		return space - text;
	}

	return fonts[font].columns;
}

void board_blink_leds(void)
{
	k_delayed_work_submit(&led_timer, K_MSEC(100));
}

void board_show_text(const char *text, bool center, s32_t duration)
{
	int i;

	cfb_framebuffer_clear(epd_dev, false);

	for (i = 0; i < 3; i++) {
		size_t len;

		while (*text == ' ' || *text == '\n') {
			text++;
		}

		len = get_len(FONT_BIG, text);
		if (!len) {
			break;
		}

		text += print_line(FONT_BIG, i, text, len, center);
		if (!*text) {
			break;
		}
	}

	cfb_framebuffer_finalize(epd_dev);

	if (duration != K_FOREVER) {
		k_delayed_work_submit(&epd_work, duration);
	}
}

static struct stat {
	u16_t addr;
	char name[9];
	u8_t min_hops;
	u8_t max_hops;
	u16_t hello_count;
	u16_t heartbeat_count;
} stats[STAT_COUNT] = {
	[0 ... (STAT_COUNT - 1)] = {
		.min_hops = BT_MESH_TTL_MAX,
		.max_hops = 0,
	},
};

static u32_t stat_count;

#define NO_UPDATE -1

static int add_hello(u16_t addr, const char *name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(stats); i++) {
		struct stat *stat = &stats[i];

		if (!stat->addr) {
			stat->addr = addr;
			strncpy(stat->name, name, sizeof(stat->name) - 1);
			stat->hello_count = 1;
			stat_count++;
			return i;
		}

		if (stat->addr == addr) {
			/* Update name, incase it has changed */
			strncpy(stat->name, name, sizeof(stat->name) - 1);

			if (stat->hello_count < 0xffff) {
				stat->hello_count++;
				return i;
			}

			return NO_UPDATE;
		}
	}

	return NO_UPDATE;
}

static int add_heartbeat(u16_t addr, u8_t hops)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(stats); i++) {
		struct stat *stat = &stats[i];

		if (!stat->addr) {
			stat->addr = addr;
			stat->heartbeat_count = 1;
			stat->min_hops = hops;
			stat->max_hops = hops;
			stat_count++;
			return i;
		}

		if (stat->addr == addr) {
			if (hops < stat->min_hops) {
				stat->min_hops = hops;
			} else if (hops > stat->max_hops) {
				stat->max_hops = hops;
			}

			if (stat->heartbeat_count < 0xffff) {
				stat->heartbeat_count++;
				return i;
			}

			return NO_UPDATE;
		}
	}

	return NO_UPDATE;
}

void board_add_hello(u16_t addr, const char *name)
{
	u32_t sort_i;

	sort_i = add_hello(addr, name);
	if (sort_i != NO_UPDATE) {
	}
}

void board_add_heartbeat(u16_t addr, u8_t hops)
{
	u32_t sort_i;

	sort_i = add_heartbeat(addr, hops);
	if (sort_i != NO_UPDATE) {
	}
}

static void show_statistics(void)
{
	int top[4] = { -1, -1, -1, -1 };
	int len, i, line = 0;
	struct stat *stat;
	char str[32];

	cfb_framebuffer_clear(epd_dev, false);

	len = snprintk(str, sizeof(str),
		       "Own Address: 0x%04x", mesh_get_addr());
	print_line(FONT_SMALL, line++, str, len, false);

	len = snprintk(str, sizeof(str),
		       "Node Count:  %u", stat_count + 1);
	print_line(FONT_SMALL, line++, str, len, false);

	/* Find the top sender */
	for (i = 0; i < ARRAY_SIZE(stats); i++) {
		int j;

		stat = &stats[i];
		if (!stat->addr) {
			break;
		}

		if (!stat->hello_count) {
			continue;
		}

		for (j = 0; j < ARRAY_SIZE(top); j++) {
			if (top[j] < 0) {
				top[j] = i;
				break;
			}

			if (stat->hello_count <= stats[top[j]].hello_count) {
				continue;
			}

			/* Move other elements down the list */
			if (j < ARRAY_SIZE(top) - 1) {
				memmove(&top[j + 1], &top[j],
					((ARRAY_SIZE(top) - j - 1) *
					 sizeof(top[j])));
			}

			top[j] = i;
			break;
		}
	}

	if (stat_count > 0) {
		len = snprintk(str, sizeof(str), "Most messages from:");
		print_line(FONT_SMALL, line++, str, len, false);

		for (i = 0; i < ARRAY_SIZE(top); i++) {
			if (top[i] < 0) {
				break;
			}

			stat = &stats[top[i]];

			len = snprintk(str, sizeof(str), "%-3u 0x%04x %s",
				       stat->hello_count, stat->addr,
				       stat->name);
			print_line(FONT_SMALL, line++, str, len, false);
		}
	}

	cfb_framebuffer_finalize(epd_dev);
}

static void show_sensors_data(s32_t interval)
{
	struct sensor_value val[3];
	u8_t line = 0;
	u16_t len = 0;

	cfb_framebuffer_clear(epd_dev, false);

	/* hdc1010 */
	if (get_hdc1010_val(val)) {
		goto _error_get;
	}

	len = snprintf(str_buf, sizeof(str_buf), "Temperature:%d.%d C\n",
		       val[0].val1, val[0].val2 / 100000);
	print_line(FONT_SMALL, line++, str_buf, len, false);

	len = snprintf(str_buf, sizeof(str_buf), "Humidity:%d%%\n",
		       val[1].val1);
	print_line(FONT_SMALL, line++, str_buf, len, false);

	/* mma8652 */
	if (get_mma8652_val(val)) {
		goto _error_get;
	}

	len = snprintf(str_buf, sizeof(str_buf), "AX :%10.3f\n",
		       sensor_value_to_double(&val[0]));
	print_line(FONT_SMALL, line++, str_buf, len, false);

	len = snprintf(str_buf, sizeof(str_buf), "AY :%10.3f\n",
		       sensor_value_to_double(&val[1]));
	print_line(FONT_SMALL, line++, str_buf, len, false);

	len = snprintf(str_buf, sizeof(str_buf), "AZ :%10.3f\n",
		       sensor_value_to_double(&val[2]));
	print_line(FONT_SMALL, line++, str_buf, len, false);

	/* apds9960 */
	if (get_apds9960_val(val)) {
		goto _error_get;
	}

	len = snprintf(str_buf, sizeof(str_buf), "Light :%d\n", val[0].val1);
	print_line(FONT_SMALL, line++, str_buf, len, false);
	len = snprintf(str_buf, sizeof(str_buf), "Proximity:%d\n", val[1].val1);
	print_line(FONT_SMALL, line++, str_buf, len, false);

	cfb_framebuffer_finalize(epd_dev);

	k_delayed_work_submit(&epd_work, interval);

	return;

_error_get:
	printk("Failed to get sensor data or print a string\n");
}

static void show_main(void)
{
	char buf[CONFIG_BT_DEVICE_NAME_MAX];
	int i;

	strncpy(buf, bt_get_name(), sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	/* Convert commas to newlines */
	for (i = 0; buf[i] != '\0'; i++) {
		if (buf[i] == ',') {
			buf[i] = '\n';
		}
	}

	board_show_text(buf, true, K_FOREVER);
}

static void epd_update(struct k_work *work)
{
	switch (screen_id) {
	case SCREEN_STATS:
		show_statistics();
		return;
	case SCREEN_SENSORS:
		show_sensors_data(K_SECONDS(2));
		return;
	case SCREEN_MAIN:
		show_main();
		return;
	}
}

static void long_press(struct k_work *work)
{
	/* Treat as release so actual release doesn't send messages */
	pressed = false;
	screen_id = (screen_id + 1) % SCREEN_LAST;
	printk("Change screen to id = %d\n", screen_id);
	board_refresh_display();
}

static bool button_is_pressed(void)
{
	u32_t val;

	gpio_pin_read(gpio, SW0_GPIO_PIN, &val);

	return !val;
}

/*static const char* key_controllers[] = {
	SW1_GPIO_CONTROLLER,
	SW2_GPIO_CONTROLLER,
	SW3_GPIO_CONTROLLER,
	SW4_GPIO_CONTROLLER,
	SW5_GPIO_CONTROLLER,
	SW6_GPIO_CONTROLLER,
	SW7_GPIO_CONTROLLER,
	SW8_GPIO_CONTROLLER,
	SW9_GPIO_CONTROLLER,
	SWOK_GPIO_CONTROLLER,
	SWX_GPIO_CONTROLLER,
};

static const int key_pins[] = {
	SW1_GPIO_PIN,
	SW2_GPIO_PIN,
	SW3_GPIO_PIN,
	SW4_GPIO_PIN,
	SW5_GPIO_PIN,
	SW6_GPIO_PIN,
	SW7_GPIO_PIN,
	SW8_GPIO_PIN,
	SW9_GPIO_PIN,
	SWOK_GPIO_PIN,
	SWX_GPIO_PIN,
};

static bool button_n_is_pressed(int n)
{
	u32_t val;

	gpio_pin_read(gpios[n], key_pins[n], &val);

	return !val;
}*/

static void button_interrupt(struct device *dev, struct gpio_callback *cb,
			     u32_t pins)
{
	if (button_is_pressed() == pressed) {
		return;
	}

	pressed = !pressed;
	printk("Button %s\n", pressed ? "pressed" : "released");

	if (pressed) {
		k_delayed_work_submit(&long_press_work, LONG_PRESS_TIMEOUT);
		return;
	}

	k_delayed_work_cancel(&long_press_work);

	if (!mesh_is_initialized()) {
		return;
	}

	/* Short press for views */
	switch (screen_id) {
	case SCREEN_SENSORS:
	case SCREEN_STATS:
		return;
	case SCREEN_MAIN:
		if (pins & BIT(SW0_GPIO_PIN)) {
			mesh_send_hello();
		}
		return;
	default:
		return;
	}
}

/*#define n_callback(num) (button_##num##_interrupt)

#define n_callback_(num) \
static void button_##num##_interrupt(struct device *dev, struct gpio_callback *cb, u32_t pins) { \
	if (button_n_is_pressed(num) == presseds[num]) { \
		return; \
	} \
	presseds[num] = !presseds[num];\
	printk("Button %d %s\n", num+1, presseds[num] ? "pressed" : "released");\
	switch (screen_id) {\
	case SCREEN_SENSORS:\
	case SCREEN_STATS:\
		return;\
	case SCREEN_MAIN:\
		if (presseds[num]) {\
			if (num == 0)\
				mesh_send_string("I'm Good!");\
			else if (num == 1)\
				mesh_send_string("Send help!");\
			else if (num == 2)\
				mesh_send_string("I'm in danger!");\
			else if (num == 3)\
				mesh_send_string("I've found him/help");\
			else if (num == 4)\
				mesh_send_string("RUN FOR YOUR LIVES");\
			else if (num == 5)\
				mesh_send_string("WE ARE FUCKED");\
		}\
		return;\
	default:\
		return;\
	}\
}

n_callback_(0)
n_callback_(1)
n_callback_(2)
n_callback_(3)
n_callback_(4)
n_callback_(5)
n_callback_(6)
n_callback_(7)
n_callback_(8)
n_callback_(9)
n_callback_(10)


static int configure_buttons(void) {
	static struct gpio_callback button_cbs[11];

	for (int i = 0; i < 11; ++i) {
		gpios[i] = device_get_binding(key_controllers[i]);
		if (!gpios[i]) {
			return -ENODEV;
		}

		int pin = key_pins[i];

		gpio_pin_configure(gpios[i], pin, (GPIO_DIR_IN | GPIO_INT |  PULL_UP | EDGE));

		switch(i) {
			case 0: gpio_init_callback(&button_cbs[i], n_callback(0), BIT(pin)); break;
			case 1: gpio_init_callback(&button_cbs[i], n_callback(1), BIT(pin)); break;
			case 2: gpio_init_callback(&button_cbs[i], n_callback(2), BIT(pin)); break;
			case 3: gpio_init_callback(&button_cbs[i], n_callback(3), BIT(pin)); break;
			case 4: gpio_init_callback(&button_cbs[i], n_callback(4), BIT(pin)); break;
			case 5: gpio_init_callback(&button_cbs[i], n_callback(5), BIT(pin)); break;
			case 6: gpio_init_callback(&button_cbs[i], n_callback(6), BIT(pin)); break;
			case 7: gpio_init_callback(&button_cbs[i], n_callback(7), BIT(pin)); break;
			case 8: gpio_init_callback(&button_cbs[i], n_callback(8), BIT(pin)); break;
			case 9: gpio_init_callback(&button_cbs[i], n_callback(9), BIT(pin)); break;
			case 10: gpio_init_callback(&button_cbs[i], n_callback(10), BIT(pin)); break;
		}
		gpio_add_callback(gpios[i], &button_cbs[i]);

		gpio_pin_enable_callback(gpios[i], pin);
	}

	return 0;
}*/

static int configure_button(void)
{
	static struct gpio_callback button_cb;

	gpio = device_get_binding(SW0_GPIO_CONTROLLER);
	if (!gpio) {
		return -ENODEV;
	}

	gpio_pin_configure(gpio, SW0_GPIO_PIN,
			   (GPIO_DIR_IN | GPIO_INT |  PULL_UP | EDGE));

	gpio_init_callback(&button_cb, button_interrupt, BIT(SW0_GPIO_PIN));
	gpio_add_callback(gpio, &button_cb);

	gpio_pin_enable_callback(gpio, SW0_GPIO_PIN);

	return 0;// || configure_buttons();
}

static void led_timeout(struct k_work *work)
{
	static int led_cntr;
	int i;

	/* Disable all LEDs */
	for (i = 0; i < ARRAY_SIZE(leds); i++) {
		gpio_pin_write(leds[i].dev, leds[i].pin, 1);
	}

	/* Stop after 5 iterations */
	if (led_cntr > (ARRAY_SIZE(leds) * 5)) {
		led_cntr = 0;
		return;
	}

	/* Select and enable current LED */
	i = led_cntr++ % ARRAY_SIZE(leds);
	gpio_pin_write(leds[i].dev, leds[i].pin, 0);

	k_delayed_work_submit(&led_timer, K_MSEC(100));
}

static int configure_leds(void)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(leds); i++) {
		leds[i].dev = device_get_binding(leds[i].name);
		if (!leds[i].dev) {
			printk("Failed to get %s device\n", leds[i].name);
			return -ENODEV;
		}

		gpio_pin_configure(leds[i].dev, leds[i].pin, GPIO_DIR_OUT);
		gpio_pin_write(leds[i].dev, leds[i].pin, 1);

	}

	k_delayed_work_init(&led_timer, led_timeout);
	return 0;
}

static int erase_storage(void)
{
	struct device *dev;

	dev = device_get_binding(DT_FLASH_DEV_NAME);

	return flash_erase(dev, FLASH_AREA_STORAGE_OFFSET,
			   FLASH_AREA_STORAGE_SIZE);
}

void board_refresh_display(void)
{
	k_delayed_work_submit(&epd_work, K_NO_WAIT);
}

int board_init(void)
{
	epd_dev = device_get_binding(DT_SSD1673_DEV_NAME);
	if (epd_dev == NULL) {
		printk("SSD1673 device not found\n");
		return -ENODEV;
	}

	if (cfb_framebuffer_init(epd_dev)) {
		printk("Framebuffer initialization failed\n");
		return -EIO;
	}

	cfb_framebuffer_clear(epd_dev, true);

	if (configure_button()) {
		printk("Failed to configure button\n");
		return -EIO;
	}

	if (configure_leds()) {
		printk("LED init failed\n");
		return -EIO;
	}

	k_delayed_work_init(&epd_work, epd_update);
	k_delayed_work_init(&long_press_work, long_press);

	pressed = button_is_pressed();
	// for (int i = 0; i < 11; ++i)
	// 	presseds[i] = button_n_is_pressed(i);
	if (pressed) {
		printk("Erasing storage\n");
		board_show_text("Resetting Device", false, K_SECONDS(4));
		erase_storage();
	}

	return 0;
}
