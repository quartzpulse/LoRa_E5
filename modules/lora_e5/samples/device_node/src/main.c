/**
 * @file samples/device_node/src/main.c
 * @brief Duty-cycled sensor-node pattern: wake (timer), join, send, sleep,
 * repeat -- through the full lora_e5 library stack against a real
 * LoRa-E5-HF module, wired to esp32s3_devkitc's UART2
 * (boards/esp32s3_devkitc_procpu.overlay).
 *
 * "Deep Sleep" here is real ESP32 MCU deep sleep (sys_poweroff()), not
 * lora_e5_sleep() -- waking is a full reboot back into main(), so this is
 * deliberately a linear per-wake sequence, not a persistent event loop.
 * See docs/Device_State_Machine.md's "Architecture" section for the full
 * rationale. Uses lora_e5_resume_sync(), not start_sync()+join_sync():
 * this board's LoRa-E5 stays continuously powered across the ESP32's
 * deep sleep, so most wakes reach JOINED in tens of milliseconds via
 * the modem's own retained session instead of a full ~8s
 * RESET+CONFIG+JOIN sequence -- see docs/VERIFICATION_NEEDED.md's
 * "Resolved 2026-07-11" section for how this was confirmed and
 * lora_e5.h's lora_e5_resume_sync() doc comment for the API contract.
 *
 * OTAA credentials + region are NVS-backed (config/settings_lorawan.yaml,
 * scripts/gen_settings_module.py -- see scripts/README.md) instead of
 * compile-time constants: DEFAULT_OTAA_CFG below seeds the settings
 * store's in-RAM defaults on first boot (before settings_load() runs;
 * a saved NVS record for a given key overrides its default, an absent
 * one leaves the seeded default in place -- see load_provisioning()).
 * Only credentials/region/join_mode_otaa are wired here -- sub_band/
 * adr_enabled/data_rate/tx_power/confirmed_uplink/uplink_interval_sec
 * are commented out in the YAML because lora_e5.h has no public setter
 * for any of them yet (CLAUDE.md's "Known gaps" #2).
 *
 * Status is shown on the onboard WS2812 RGB LED (modules/status_led,
 * GPIO38/SPI3, boards/status_led_rgb.overlay) using a minimal, honest
 * subset of that library's network/MQTT-flavored state vocabulary --
 * device_node has no MQTT/OTA/provisioning, so only BOOTING /
 * CONNECTING_NETWORK / NETWORK_CONNECTED / HW_FAILURE / FATAL_ERROR are
 * used, not the full table. IMPORTANT: status_led_stop() (synchronous)
 * is called right before sys_poweroff(), not status_led_set_state() --
 * a WS2812 latches its last color on pure power and needs no refresh,
 * and GPIO38/SPI3 gets isolated (not actively driven low) during ESP32
 * deep sleep, so skipping the synchronous off would leave the LED lit
 * at whatever color for the entire sleep interval, silently defeating
 * the point of a duty-cycled battery node.
 *
 * The uplink payload carries real data: a BME680 (I2C0, GPIO1=SDA/
 * GPIO2=SCL, boards/esp32s3_devkitc_procpu.overlay) is read for
 * temperature/pressure/humidity/gas resistance right before sending.
 * [Guessing, unverified against real hardware -- no BME680 was
 * connected during this project's other bring-up sessions]: the
 * overlay assumes I2C address 0x76 (SDO grounded); see that file's
 * comment and docs/VERIFICATION_NEEDED.md if your board wires SDO
 * high instead. The sensor read degrading gracefully (payload falls
 * back to boot_count-only, 4 bytes, if the read fails) is deliberate,
 * not an oversight -- this sample's core LoRaWAN mission should not
 * block on a sensor that may not physically be present.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/poweroff.h>
#include <esp_sleep.h>

#include <lora_e5/lora_e5.h>
#include <status_led/status_led.h>

#include "settings_lorawan.h"

LOG_MODULE_REGISTER(lora_e5_device_node, LOG_LEVEL_INF);

/**
 * Confirmed-working OTAA credentials (real join + real uplink observed
 * in ChirpStack, same as samples/join) -- used ONLY to seed the NVS
 * settings store's defaults on a device's very first boot (empty
 * flash). Once settings_save_one() has written a real record (e.g. via
 * the "lorawan dev_eui <hex>" shell command, CONFIG_NVS_SETTINGS_LORAWAN_SHELL=y),
 * that record wins on every subsequent boot -- see load_provisioning().
 * Replace these three with your own device's values before flashing a
 * fresh device, same as samples/join.
 */
static const struct lora_e5_otaa_config default_otaa_cfg = {
	.dev_eui = { .bytes = { 0x26, 0xC5, 0x18, 0xF8, 0xEF, 0x84, 0x0E, 0x5D } },
	.app_eui = { .bytes = { 0x78, 0x36, 0xFC, 0xAF, 0xA7, 0x3B, 0x3E, 0xD3 } },
	.app_key = { .bytes = { 0x5B, 0xD1, 0x95, 0x9C, 0x42, 0x57, 0xCD, 0x92, 0x15, 0x30, 0x53, 0xFD, 0x66, 0xDC, 0xF9, 0x59 } },
};
#define DEFAULT_REGION_NAME "IN865"

/* Retained-memory boot/wake counter -- survives deep sleep (see
 * read_and_bump_boot_count()), does not gate any join/config decision
 * (docs/Device_State_Machine.md point 6), and is always the first 4
 * bytes of this sample's uplink payload (see build_uplink_payload())
 * so a real device event in ChirpStack has an actual, verifiable value
 * in it instead of a fixed placeholder -- each successful uplink's
 * payload should read one higher than the last, sensor data or not.
 */
#define RETAINED_MEM_MAGIC 0x4C453544u /* "LE5D" */

/* ------------------------------------------------------------------- */
/* BME680 environmental sensor                                          */
/* ------------------------------------------------------------------- */

/** @brief Fixed-point environmental reading, ready to append to the
 *  uplink payload. `valid` is false (all other fields zero) if the
 *  sensor isn't present/ready or the read failed -- see
 *  read_environment(). */
struct env_reading {
	bool valid;
	int16_t temp_c_x100;    /* degrees C * 100, e.g. 2345 = 23.45C */
	uint16_t press_kpa_x10; /* kPa * 10, e.g. 1013 = 101.3 kPa */
	uint16_t humidity_x100; /* %RH * 100, e.g. 4567 = 45.67% */
	uint32_t gas_res_ohm;   /* ohms, driver's native resolution */
};

/** @brief Converts a Zephyr struct sensor_value (val1 + val2/1e6) to a
 *  fixed-point integer scaled by `scale`, e.g. scale=100 on 23.45
 *  (val1=23, val2=450000) yields 2345. Integer-only (no float/double
 *  pulled into the link) via an int64_t intermediate to avoid overflow
 *  across the multiply. */
static int32_t sensor_value_scaled(const struct sensor_value *v, int32_t scale)
{
	int64_t scaled = (int64_t)v->val1 * scale +
			  ((int64_t)v->val2 * scale) / 1000000;

	return (int32_t)scaled;
}

/** @brief Reads the BME680 (temp/press/humidity/gas resistance),
 *  returning a zeroed, `valid=false` reading (not an error code) on
 *  any failure -- see this file's doc comment for why a missing/failed
 *  sensor degrades the uplink payload rather than blocking it. */
static struct env_reading read_environment(void)
{
	struct env_reading env = { 0 };
	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(bme680));
	struct sensor_value temp, press, humidity, gas_res;

	if (!device_is_ready(dev)) {
		LOG_WRN("BME680 not ready -- uplink will omit sensor data");
		return env;
	}

	if (sensor_sample_fetch(dev) != 0) {
		LOG_WRN("BME680 sensor_sample_fetch failed -- uplink will omit sensor data");
		return env;
	}

	if (sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &temp) != 0 ||
	    sensor_channel_get(dev, SENSOR_CHAN_PRESS, &press) != 0 ||
	    sensor_channel_get(dev, SENSOR_CHAN_HUMIDITY, &humidity) != 0 ||
	    sensor_channel_get(dev, SENSOR_CHAN_GAS_RES, &gas_res) != 0) {
		LOG_WRN("BME680 sensor_channel_get failed -- uplink will omit sensor data");
		return env;
	}

	env.valid = true;
	env.temp_c_x100 = (int16_t)sensor_value_scaled(&temp, 100);
	env.press_kpa_x10 = (uint16_t)sensor_value_scaled(&press, 10);
	env.humidity_x100 = (uint16_t)sensor_value_scaled(&humidity, 100);
	env.gas_res_ohm = (uint32_t)gas_res.val1;

	LOG_INF("BME680: temp=%d.%02dC press=%d.%01dkPa humidity=%d.%02d%% gas_res=%uohm",
		env.temp_c_x100 / 100, abs(env.temp_c_x100 % 100), env.press_kpa_x10 / 10,
		env.press_kpa_x10 % 10, env.humidity_x100 / 100, env.humidity_x100 % 100,
		(unsigned)env.gas_res_ohm);

	return env;
}

/** @brief Builds the uplink payload into `out` (must be at least
 *  UPLINK_PAYLOAD_MAX_LEN bytes) and returns the number of bytes
 *  written: boot_count is always the first 4 bytes (big-endian); if
 *  `env->valid`, 10 more bytes (temp/press/humidity/gas_res, all
 *  big-endian) are appended, otherwise the payload is just the 4
 *  bytes. */
#define UPLINK_PAYLOAD_MAX_LEN 14

static size_t build_uplink_payload(uint32_t boot_count, const struct env_reading *env,
				    uint8_t out[UPLINK_PAYLOAD_MAX_LEN])
{
	out[0] = (uint8_t)(boot_count >> 24);
	out[1] = (uint8_t)(boot_count >> 16);
	out[2] = (uint8_t)(boot_count >> 8);
	out[3] = (uint8_t)(boot_count);

	if (!env->valid) {
		return 4;
	}

	out[4] = (uint8_t)((uint16_t)env->temp_c_x100 >> 8);
	out[5] = (uint8_t)((uint16_t)env->temp_c_x100);
	out[6] = (uint8_t)(env->press_kpa_x10 >> 8);
	out[7] = (uint8_t)(env->press_kpa_x10);
	out[8] = (uint8_t)(env->humidity_x100 >> 8);
	out[9] = (uint8_t)(env->humidity_x100);
	out[10] = (uint8_t)(env->gas_res_ohm >> 24);
	out[11] = (uint8_t)(env->gas_res_ohm >> 16);
	out[12] = (uint8_t)(env->gas_res_ohm >> 8);
	out[13] = (uint8_t)(env->gas_res_ohm);

	return UPLINK_PAYLOAD_MAX_LEN;
}

static void event_cb(const struct lora_e5_app_event *event, void *user_data)
{
	ARG_UNUSED(user_data);

	switch (event->type) {
	case LORA_E5_APP_EVT_JOIN_SUCCESS:
		LOG_INF("JOIN_SUCCESS devaddr=%02X%02X%02X%02X netid=%06X",
			event->join.dev_addr.bytes[0], event->join.dev_addr.bytes[1],
			event->join.dev_addr.bytes[2], event->join.dev_addr.bytes[3],
			event->join.net_id);
		break;
	case LORA_E5_APP_EVT_JOIN_FAILED:
		LOG_ERR("JOIN_FAILED outcome=%d", (int)event->join.outcome);
		break;
	case LORA_E5_APP_EVT_TX_SUCCESS:
		LOG_INF("TX_SUCCESS confirmed=%d", event->tx.confirmed);
		break;
	case LORA_E5_APP_EVT_TX_FAILED:
		LOG_ERR("TX_FAILED reason=%d", (int)event->tx.fail_reason);
		break;
	case LORA_E5_APP_EVT_DOWNLINK_RECEIVED:
		LOG_INF("DOWNLINK port=%u len=%u rssi=%d snr=%d", event->downlink.port,
			(unsigned)event->downlink.len, event->downlink.rssi_dbm,
			event->downlink.snr_db);
		break;
	case LORA_E5_APP_EVT_SLEEP_ENTERED:
		LOG_INF("SLEEP_ENTERED");
		break;
	case LORA_E5_APP_EVT_WAKE_COMPLETE:
		LOG_INF("WAKE_COMPLETE");
		break;
	case LORA_E5_APP_EVT_STATE_CHANGED:
		LOG_INF("STATE_CHANGED -> %d", (int)event->state_changed);
		break;
	case LORA_E5_APP_EVT_ERROR:
		LOG_ERR("ERROR errno=%d", event->error_errno);
		break;
	}
}

static esp_sleep_wakeup_cause_t get_wakeup_cause(void)
{
	uint32_t causes = esp_sleep_get_wakeup_causes();

	if (causes == 0) {
		return ESP_SLEEP_WAKEUP_UNDEFINED;
	}
	return (esp_sleep_wakeup_cause_t)__builtin_ctz(causes);
}

/** @brief Diagnostic wake counter, persisted across deep sleep. Resets to
 *  1 on a non-timer wake (power-on / reset), since that means the SoC
 *  didn't come from a sleep cycle this counter was tracking. */
static uint32_t read_and_bump_boot_count(esp_sleep_wakeup_cause_t cause)
{
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(retainedmemdevice));
	uint32_t magic = 0;
	uint32_t count = 0;

	if (!device_is_ready(dev)) {
		LOG_WRN("retained_mem device not ready -- boot count unavailable");
		return 0;
	}

	if (cause == ESP_SLEEP_WAKEUP_TIMER) {
		retained_mem_read(dev, 0, (uint8_t *)&magic, sizeof(magic));
		if (magic == RETAINED_MEM_MAGIC) {
			retained_mem_read(dev, sizeof(magic), (uint8_t *)&count, sizeof(count));
		}
	}

	count++;
	magic = RETAINED_MEM_MAGIC;
	retained_mem_write(dev, 0, (uint8_t *)&magic, sizeof(magic));
	retained_mem_write(dev, sizeof(magic), (uint8_t *)&count, sizeof(count));

	return count;
}

/* ------------------------------------------------------------------- */
/* NVS-backed provisioning (config/settings_lorawan.yaml)                */
/* ------------------------------------------------------------------- */

static int hex_nibble(char c)
{
	if (c >= '0' && c <= '9') {
		return c - '0';
	}
	if (c >= 'A' && c <= 'F') {
		return c - 'A' + 10;
	}
	if (c >= 'a' && c <= 'f') {
		return c - 'a' + 10;
	}
	return -1;
}

static void bytes_to_hex(const uint8_t *bytes, size_t len, char *out, size_t out_len)
{
	static const char digits[] = "0123456789ABCDEF";
	size_t n = MIN(len, (out_len - 1) / 2);

	for (size_t i = 0; i < n; i++) {
		out[i * 2] = digits[bytes[i] >> 4];
		out[i * 2 + 1] = digits[bytes[i] & 0x0F];
	}
	out[n * 2] = '\0';
}

static int hex_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
	if (hex == NULL || strlen(hex) != out_len * 2) {
		return -EINVAL;
	}
	for (size_t i = 0; i < out_len; i++) {
		int hi = hex_nibble(hex[i * 2]);
		int lo = hex_nibble(hex[i * 2 + 1]);

		if (hi < 0 || lo < 0) {
			return -EINVAL;
		}
		out[i] = (uint8_t)((hi << 4) | lo);
	}
	return 0;
}

static int parse_region(const char *name, enum lora_e5_region *out)
{
	static const struct {
		const char *name;
		enum lora_e5_region region;
	} table[] = {
		{ "EU868", LORA_E5_REGION_EU868 },
		{ "US915", LORA_E5_REGION_US915 },
		{ "US915HYBRID", LORA_E5_REGION_US915HYBRID },
		{ "CN779", LORA_E5_REGION_CN779 },
		{ "EU433", LORA_E5_REGION_EU433 },
		{ "AU915", LORA_E5_REGION_AU915 },
		{ "AU915OLD", LORA_E5_REGION_AU915OLD },
		{ "CN470", LORA_E5_REGION_CN470 },
		{ "AS923", LORA_E5_REGION_AS923 },
		{ "KR920", LORA_E5_REGION_KR920 },
		{ "IN865", LORA_E5_REGION_IN865 },
		{ "RU864", LORA_E5_REGION_RU864 },
	};

	if (name == NULL) {
		return -EINVAL;
	}
	for (size_t i = 0; i < ARRAY_SIZE(table); i++) {
		if (strcmp(name, table[i].name) == 0) {
			*out = table[i].region;
			return 0;
		}
	}
	return -EINVAL;
}

/** @brief Seeds lorawan_config's in-RAM defaults from default_otaa_cfg
 *  BEFORE settings_load() runs. A key with a saved NVS record has
 *  settings_load() overwrite this default; a key with no record (a
 *  device's first boot, or one that was factory-reset) keeps it --
 *  same pattern scripts/README.md documents for the template's
 *  "shared" module. Does not itself touch NVS (no settings_save_one()
 *  call) -- these are just this boot's in-RAM starting values.
 */
static void seed_default_provisioning(void)
{
	char hex[33];

	bytes_to_hex(default_otaa_cfg.dev_eui.bytes, sizeof(default_otaa_cfg.dev_eui.bytes),
		     hex, sizeof(hex));
	strncpy(lorawan_config.dev_eui, hex, sizeof(lorawan_config.dev_eui) - 1);

	bytes_to_hex(default_otaa_cfg.app_eui.bytes, sizeof(default_otaa_cfg.app_eui.bytes),
		     hex, sizeof(hex));
	strncpy(lorawan_config.join_eui, hex, sizeof(lorawan_config.join_eui) - 1);

	bytes_to_hex(default_otaa_cfg.app_key.bytes, sizeof(default_otaa_cfg.app_key.bytes),
		     hex, sizeof(hex));
	strncpy(lorawan_config.app_key, hex, sizeof(lorawan_config.app_key) - 1);

	strncpy(lorawan_config.region, DEFAULT_REGION_NAME, sizeof(lorawan_config.region) - 1);

	/* Not yet read/branched on anywhere -- device_node is OTAA-only
	 * today. Seeded so "lorawan show" reports a sensible value if the
	 * shell is enabled, and so it's ready to wire up if ABP support is
	 * ever added here.
	 */
	lorawan_config.join_mode_otaa = true;
}

/** @brief Brings up the NVS settings store and resolves this boot's
 *  OTAA credentials + region from it (falling back to
 *  default_otaa_cfg/DEFAULT_REGION_NAME wherever no NVS record exists
 *  yet -- see seed_default_provisioning()). Must run before
 *  lora_e5_set_otaa()/set_region(). */
static int load_provisioning(struct lora_e5_otaa_config *out_otaa, enum lora_e5_region *out_region)
{
	int rc;

	rc = settings_subsys_init();
	if (rc != 0) {
		return rc;
	}

	settings_lorawan_init();
	seed_default_provisioning();
	settings_load();

	rc = hex_to_bytes(get_nvs_lorawan_deveui_t_dev_eui(), out_otaa->dev_eui.bytes,
			   sizeof(out_otaa->dev_eui.bytes));
	if (rc != 0) {
		LOG_ERR("Invalid dev_eui in NVS settings (%d)", rc);
		return rc;
	}

	rc = hex_to_bytes(get_nvs_lorawan_joineui_t_join_eui(), out_otaa->app_eui.bytes,
			   sizeof(out_otaa->app_eui.bytes));
	if (rc != 0) {
		LOG_ERR("Invalid join_eui in NVS settings (%d)", rc);
		return rc;
	}

	rc = hex_to_bytes(get_nvs_lorawan_appkey_t_app_key(), out_otaa->app_key.bytes,
			   sizeof(out_otaa->app_key.bytes));
	if (rc != 0) {
		LOG_ERR("Invalid app_key in NVS settings (%d)", rc);
		return rc;
	}

	rc = parse_region(get_nvs_lorawan_region_t_region(), out_region);
	if (rc != 0) {
		LOG_ERR("Invalid region '%s' in NVS settings (%d)",
			get_nvs_lorawan_region_t_region(), rc);
		return rc;
	}

	return 0;
}

static void go_to_sleep(void)
{
	const int wakeup_time_sec = CONFIG_DEVICE_NODE_WAKEUP_TIME_SEC;

	LOG_INF("Arming %ds timer wakeup, powering off", wakeup_time_sec);

	/* sys_poweroff() is called directly (not through the PM subsystem's
	 * own idle-triggered suspend), so the wakeup source must be armed by
	 * the application first -- same requirement documented in
	 * external/zephyr/samples/boards/espressif/deep_sleep.
	 */
	esp_sleep_enable_timer_wakeup((uint64_t)wakeup_time_sec * 1000000);

	/* Without this, sys_poweroff() cuts power before Zephyr's deferred
	 * logging thread drains its queue -- confirmed on real hardware:
	 * the TX_SUCCESS/TX_FAILED event log and the "Send confirmed" line
	 * below were silently lost on every cycle without it. LOG_PANIC()
	 * flushes pending log messages and switches logging synchronous for
	 * whatever's left.
	 */
	LOG_PANIC();
	sys_poweroff();
}

int main(void)
{
	esp_sleep_wakeup_cause_t cause = get_wakeup_cause();
	uint32_t boot_count = read_and_bump_boot_count(cause);

	LOG_INF("Wake cause=%d boot_count=%u", (int)cause, boot_count);

	/* LED failure is not fatal to the LoRaWAN mission -- if it doesn't
	 * come up, log it once and skip every other status_led_*() call
	 * this boot rather than risk cascading a display-only failure into
	 * treating the whole wake as failed.
	 */
	bool led_ok = (status_led_init() == 0);

	if (!led_ok) {
		LOG_WRN("status_led_init failed -- continuing without LED status");
	}
	if (led_ok) {
		status_led_set_state(STATUS_LED_BOOTING);
	}

	struct lora_e5_otaa_config loaded_otaa;
	enum lora_e5_region loaded_region;
	int rc;

	rc = load_provisioning(&loaded_otaa, &loaded_region);
	if (rc != 0) {
		LOG_ERR("load_provisioning failed (%d)", rc);
		if (led_ok) {
			status_led_clear_state(STATUS_LED_BOOTING);
			status_led_set_state(STATUS_LED_FATAL_ERROR);
		}
		goto sleep;
	}

	const struct lora_e5_hw_config hw = {
		.uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart2)),
		.reset_gpio = NULL, /* AT+RESET only -- no reset-gpios wired on this board */
	};

	rc = lora_e5_init(&hw);
	if (rc != 0) {
		LOG_ERR("lora_e5_init failed (%d)", rc);
		if (led_ok) {
			status_led_clear_state(STATUS_LED_BOOTING);
			status_led_set_state(STATUS_LED_HW_FAILURE);
		}
		goto sleep;
	}

	lora_e5_register_callback(event_cb, NULL);

	rc = lora_e5_set_otaa(&loaded_otaa);
	if (rc != 0) {
		LOG_ERR("lora_e5_set_otaa failed (%d)", rc);
		if (led_ok) {
			status_led_clear_state(STATUS_LED_BOOTING);
			status_led_set_state(STATUS_LED_FATAL_ERROR);
		}
		goto sleep;
	}

	rc = lora_e5_set_region(loaded_region);
	if (rc != 0) {
		LOG_ERR("lora_e5_set_region failed (%d)", rc);
		if (led_ok) {
			status_led_clear_state(STATUS_LED_BOOTING);
			status_led_set_state(STATUS_LED_FATAL_ERROR);
		}
		goto sleep;
	}

	if (led_ok) {
		status_led_clear_state(STATUS_LED_BOOTING);
		status_led_set_state(STATUS_LED_CONNECTING_NETWORK);
	}

	/* lora_e5_resume_sync(), not start_sync()+join_sync(): this board's
	 * LoRa-E5 stays continuously powered across the ESP32's own deep
	 * sleep (confirmed by the project owner), so the fast path --
	 * probe without AT+RESET, AT+JOIN directly -- reaches JOINED in
	 * tens of milliseconds instead of the ~8s full sequence, on every
	 * wake where the session survived. Safe to always call: if the
	 * module ever did lose power, this transparently falls back to
	 * the exact same full RESET+CONFIG+JOIN sequence start_sync()+
	 * join_sync() always ran -- see docs/VERIFICATION_NEEDED.md's
	 * "Resolved 2026-07-11" section for the measured numbers.
	 */
	rc = lora_e5_resume_sync(K_SECONDS(20));
	if (rc != 0) {
		LOG_ERR("lora_e5_resume_sync failed (%d)", rc);
		if (led_ok) {
			status_led_clear_state(STATUS_LED_CONNECTING_NETWORK);
			status_led_set_state(STATUS_LED_FATAL_ERROR);
		}
		goto sleep;
	}

	if (lora_e5_get_state() != LORA_E5_STATE_JOINED) {
		/* Fast path fell back to the full sequence and landed at
		 * READY (CLAUDE.md decision #2: resume_sync() never
		 * auto-joins on the fallback path either) -- finish the
		 * job exactly as lora_e5_join_sync() always did.
		 */
		rc = lora_e5_join_sync(K_SECONDS(20));
		if (rc != 0) {
			LOG_ERR("lora_e5_join_sync failed (%d)", rc);
			if (led_ok) {
				status_led_clear_state(STATUS_LED_CONNECTING_NETWORK);
				status_led_set_state(STATUS_LED_FATAL_ERROR);
			}
			goto sleep;
		}
	}

	if (led_ok) {
		status_led_clear_state(STATUS_LED_CONNECTING_NETWORK);
		status_led_set_state(STATUS_LED_NETWORK_CONNECTED);
	}

	struct env_reading env = read_environment();
	uint8_t uplink_payload[UPLINK_PAYLOAD_MAX_LEN];
	size_t payload_len = build_uplink_payload(boot_count, &env, uplink_payload);

	rc = lora_e5_send_sync(uplink_payload, payload_len, K_SECONDS(15));
	if (rc != 0) {
		LOG_ERR("lora_e5_send_sync failed (%d)", rc);
		if (led_ok) {
			status_led_clear_state(STATUS_LED_NETWORK_CONNECTED);
			status_led_set_state(STATUS_LED_FATAL_ERROR);
		}
	} else {
		LOG_INF("Send confirmed by the library (boot_count=%u, %u-byte payload, "
			"sensor_data=%d) -- cross-check ChirpStack's device event log to "
			"confirm the frame actually arrived with this same value",
			boot_count, (unsigned)payload_len, (int)env.valid);
	}

sleep:
	/* Synchronous, not status_led_set_state(STATUS_LED_SLEEP) -- see
	 * this file's doc comment for why the async form would risk
	 * leaving the LED lit through the whole sleep interval.
	 */
	if (led_ok) {
		status_led_stop();
	}

	/* Always sleep and retry next cycle rather than idling awake on
	 * failure -- a duty-cycled battery node should never sit at the idle
	 * thread burning power waiting for nothing.
	 */
	go_to_sleep();
	return 0;
}
