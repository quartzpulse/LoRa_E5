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
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/sys/poweroff.h>
#include <esp_sleep.h>

#include <lora_e5/lora_e5.h>

LOG_MODULE_REGISTER(lora_e5_device_node, LOG_LEVEL_INF);

/**
 * Same confirmed-working OTAA credentials as samples/join (real join +
 * real uplink observed in ChirpStack) -- see that sample's main.c for the
 * verification note. Replace all three with your own device's values if
 * reusing this sample against a different module or network server.
 */
static const struct lora_e5_otaa_config otaa_cfg = {
	.dev_eui = { .bytes = { 0x26, 0xC5, 0x18, 0xF8, 0xEF, 0x84, 0x0E, 0x5D } },
	.app_eui = { .bytes = { 0x78, 0x36, 0xFC, 0xAF, 0xA7, 0x3B, 0x3E, 0xD3 } },
	.app_key = { .bytes = { 0x5B, 0xD1, 0x95, 0x9C, 0x42, 0x57, 0xCD, 0x92, 0x15, 0x30, 0x53, 0xFD, 0x66, 0xDC, 0xF9, 0x59 } },
};

static const uint8_t test_payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };

/* Retained-memory boot/wake counter -- diagnostic only, does not gate any
 * join/config decision. See docs/Device_State_Machine.md point 6.
 */
#define RETAINED_MEM_MAGIC 0x4C453544u /* "LE5D" */

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

	const struct lora_e5_hw_config hw = {
		.uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart2)),
		.reset_gpio = NULL, /* AT+RESET only -- no reset-gpios wired on this board */
	};
	int rc;

	rc = lora_e5_init(&hw);
	if (rc != 0) {
		LOG_ERR("lora_e5_init failed (%d)", rc);
		goto sleep;
	}

	lora_e5_register_callback(event_cb, NULL);

	rc = lora_e5_set_otaa(&otaa_cfg);
	if (rc != 0) {
		LOG_ERR("lora_e5_set_otaa failed (%d)", rc);
		goto sleep;
	}

	rc = lora_e5_set_region(LORA_E5_REGION_IN865);
	if (rc != 0) {
		LOG_ERR("lora_e5_set_region failed (%d)", rc);
		goto sleep;
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
			goto sleep;
		}
	}

	rc = lora_e5_send_sync(test_payload, sizeof(test_payload), K_SECONDS(15));
	if (rc != 0) {
		LOG_ERR("lora_e5_send_sync failed (%d)", rc);
	} else {
		LOG_INF("Send confirmed by the library -- cross-check ChirpStack's "
			"device event log to confirm the frame actually arrived");
	}

sleep:
	/* Always sleep and retry next cycle rather than idling awake on
	 * failure -- a duty-cycled battery node should never sit at the idle
	 * thread burning power waiting for nothing.
	 */
	go_to_sleep();
	return 0;
}
