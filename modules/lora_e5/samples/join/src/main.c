/**
 * @file samples/join/src/main.c
 * @brief Minimal end-to-end proof: boot -> join -> send a test uplink
 * through the full lora_e5 library stack (public API -> FSM -> Modem
 * Manager -> AT Command Manager -> real UART) against a real
 * LoRa-E5-HF module, wired to esp32s3_devkitc's UART1
 * (boards/esp32s3_devkitc_procpu.overlay).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <lora_e5/lora_e5.h>

LOG_MODULE_REGISTER(lora_e5_join_sample, LOG_LEVEL_INF);

/**
 * DevEui confirmed live against this project's own real LoRa-E5-HF
 * (firmware V4.0.11, previously on /dev/ttyUSB1) -- see
 * docs/VERIFICATION_NEEDED.md's resolved "AT+ID SET syntax" entry.
 *
 * AppEui/AppKey below are PLACEHOLDERS -- AppKey is write-only on the
 * module (AT+KEY can never be queried back, per spec Table 4-2), so it
 * cannot be captured the same way DevEui was. Replace both with your
 * ChirpStack device profile's actual provisioned values before
 * flashing; joining will simply fail (JOIN_FAILED, logged below) with
 * these placeholders.
 */
static const struct lora_e5_otaa_config otaa_cfg = {
	.dev_eui = { .bytes = { 0x26, 0xC5, 0x18, 0xF8, 0xEF, 0x84, 0x0E, 0x5D } },
	.app_eui = { .bytes = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 } }, /* REPLACE ME */
	.app_key = { .bytes = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
				 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F } }, /* REPLACE ME */
};

static const uint8_t test_payload[] = { 0xDE, 0xAD, 0xBE, 0xEF };

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

int main(void)
{
	const struct lora_e5_hw_config hw = {
		.uart_dev = DEVICE_DT_GET(DT_NODELABEL(uart1)),
		.reset_gpio = NULL, /* AT+RESET only -- no reset-gpios wired on this board */
	};
	int rc;

	rc = lora_e5_init(&hw);
	if (rc != 0) {
		LOG_ERR("lora_e5_init failed (%d)", rc);
		return 0;
	}

	lora_e5_register_callback(event_cb, NULL);

	rc = lora_e5_set_otaa(&otaa_cfg);
	if (rc != 0) {
		LOG_ERR("lora_e5_set_otaa failed (%d)", rc);
		return 0;
	}

	rc = lora_e5_set_region(LORA_E5_REGION_IN865);
	if (rc != 0) {
		LOG_ERR("lora_e5_set_region failed (%d)", rc);
		return 0;
	}

	LOG_INF("Starting (reset/boot-settle/probe/config)...");
	rc = lora_e5_start_sync(K_SECONDS(10));
	if (rc != 0) {
		LOG_ERR("lora_e5_start_sync failed (%d)", rc);
		return 0;
	}
	LOG_INF("READY, state=%d", (int)lora_e5_get_state());

	LOG_INF("Joining...");
	rc = lora_e5_join_sync(K_SECONDS(20));
	if (rc != 0) {
		LOG_ERR("lora_e5_join_sync failed (%d)", rc);
		return 0;
	}
	LOG_INF("JOINED");

	LOG_INF("Sending test payload (%u bytes)...", (unsigned)sizeof(test_payload));
	rc = lora_e5_send_sync(test_payload, sizeof(test_payload), K_SECONDS(15));
	if (rc != 0) {
		LOG_ERR("lora_e5_send_sync failed (%d)", rc);
	} else {
		LOG_INF("Send confirmed by the library -- cross-check ChirpStack's "
			"device event log to confirm the frame actually arrived");
	}

	return 0;
}
