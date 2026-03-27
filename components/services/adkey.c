/*
 *  ADC resistor ladder buttons for media controls.
 *
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/timers.h"
#include "driver/adc.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "soc/adc_channel.h"

#include "audio_controls.h"
#include "adkey.h"
#include "platform_config.h"

static const char *TAG = "adkey";

#define ADKEY_POLL_MS           20
#define ADKEY_STABLE_SAMPLES    3
#define ADKEY_HYSTERESIS        80

/*
 * Initial raw ADC centers for a 22k pull-up ladder with 3k/6.2k/9.1k to GND.
 * These are intentionally approximate and tuned through debug logs on hardware.
 */
#define ADKEY_IDLE_THRESHOLD    3000
#define ADKEY_3K_CENTER         975
#define ADKEY_6K2_CENTER        1775
#define ADKEY_9K1_CENTER        2350

typedef enum {
	ADKEY_STATE_IDLE = 0,
	ADKEY_STATE_PREV,
	ADKEY_STATE_TOGGLE,
	ADKEY_STATE_NEXT,
} adkey_state_t;

static struct {
	bool initialized;
	bool enabled;
	adc1_channel_t channel;
	TimerHandle_t timer;
	adkey_state_t stable_state;
	adkey_state_t pending_state;
	uint8_t stable_count;
} adkey;

static bool adkey_gpio_to_channel(int gpio, adc1_channel_t *channel) {
#ifdef CONFIG_IDF_TARGET_ESP32S3
	switch (gpio) {
	case 1: *channel = ADC1_CHANNEL_0; return true;
	case 2: *channel = ADC1_CHANNEL_1; return true;
	case 3: *channel = ADC1_CHANNEL_2; return true;
	case 4: *channel = ADC1_CHANNEL_3; return true;
	case 5: *channel = ADC1_CHANNEL_4; return true;
	case 6: *channel = ADC1_CHANNEL_5; return true;
	case 7: *channel = ADC1_CHANNEL_6; return true;
	case 8: *channel = ADC1_CHANNEL_7; return true;
	case 9: *channel = ADC1_CHANNEL_8; return true;
	case 10: *channel = ADC1_CHANNEL_9; return true;
	default: return false;
	}
#else
	(void) gpio;
	(void) channel;
	return false;
#endif
}

static const char *adkey_state_name(adkey_state_t state) {
	switch (state) {
	case ADKEY_STATE_PREV:
		return "PREV";
	case ADKEY_STATE_TOGGLE:
		return "TOGGLE";
	case ADKEY_STATE_NEXT:
		return "NEXT";
	default:
		return "IDLE";
	}
}

static int adkey_state_center(adkey_state_t state) {
	switch (state) {
	case ADKEY_STATE_PREV:
		return ADKEY_3K_CENTER;
	case ADKEY_STATE_TOGGLE:
		return ADKEY_6K2_CENTER;
	case ADKEY_STATE_NEXT:
		return ADKEY_9K1_CENTER;
	default:
		return ADKEY_IDLE_THRESHOLD;
	}
}

static adkey_state_t adkey_classify_raw(int raw, adkey_state_t current) {
	int dist_3k, dist_6k2, dist_9k1;
	adkey_state_t candidate;

	if (raw >= ADKEY_IDLE_THRESHOLD) return ADKEY_STATE_IDLE;

	dist_3k = abs(raw - ADKEY_3K_CENTER);
	dist_6k2 = abs(raw - ADKEY_6K2_CENTER);
	dist_9k1 = abs(raw - ADKEY_9K1_CENTER);

	candidate = ADKEY_STATE_PREV;
	if (dist_6k2 < dist_3k && dist_6k2 <= dist_9k1) candidate = ADKEY_STATE_TOGGLE;
	else if (dist_9k1 < dist_3k && dist_9k1 < dist_6k2) candidate = ADKEY_STATE_NEXT;

	if (current == ADKEY_STATE_IDLE) {
		if (raw >= ADKEY_IDLE_THRESHOLD - ADKEY_HYSTERESIS) return ADKEY_STATE_IDLE;
		return candidate;
	}

	if (candidate != current && abs(raw - adkey_state_center(current)) <= ADKEY_HYSTERESIS) {
		return current;
	}

	return candidate;
}

static void adkey_dispatch(adkey_state_t previous, adkey_state_t next) {
	actrls_handler handler;

	switch (previous) {
	case ADKEY_STATE_PREV:
		handler = get_ctrl_handler(ACTRLS_PREV);
		if (handler) handler(false);
		break;
	case ADKEY_STATE_TOGGLE:
		handler = get_ctrl_handler(ACTRLS_TOGGLE);
		if (handler) handler(false);
		break;
	case ADKEY_STATE_NEXT:
		handler = get_ctrl_handler(ACTRLS_NEXT);
		if (handler) handler(false);
		break;
	default:
		break;
	}

	switch (next) {
	case ADKEY_STATE_PREV:
		handler = get_ctrl_handler(ACTRLS_PREV);
		if (handler) handler(true);
		break;
	case ADKEY_STATE_TOGGLE:
		handler = get_ctrl_handler(ACTRLS_TOGGLE);
		if (handler) handler(true);
		break;
	case ADKEY_STATE_NEXT:
		handler = get_ctrl_handler(ACTRLS_NEXT);
		if (handler) handler(true);
		break;
	default:
		break;
	}
}

static void adkey_poll(TimerHandle_t timer) {
	int raw;
	adkey_state_t candidate;

	(void) timer;

	if (!adkey.enabled) return;

	raw = adc1_get_raw(adkey.channel);
	candidate = adkey_classify_raw(raw, adkey.stable_state);

	if (candidate != adkey.pending_state) {
		adkey.pending_state = candidate;
		adkey.stable_count = 1;
#ifdef CONFIG_ADKEY_DEBUG
		ESP_LOGI(TAG, "raw=%d candidate=%s stable=%s", raw,
				 adkey_state_name(candidate), adkey_state_name(adkey.stable_state));
#endif
		return;
	}

	if (adkey.stable_count < ADKEY_STABLE_SAMPLES) adkey.stable_count++;

	if (adkey.stable_count < ADKEY_STABLE_SAMPLES || candidate == adkey.stable_state) return;

#ifdef CONFIG_ADKEY_DEBUG
	ESP_LOGI(TAG, "raw=%d state=%s", raw, adkey_state_name(candidate));
	ESP_LOGI(TAG, "state %s -> %s", adkey_state_name(adkey.stable_state), adkey_state_name(candidate));
#endif
	adkey_dispatch(adkey.stable_state, candidate);
	adkey.stable_state = candidate;
}

void adkey_init(void) {
	int gpio = CONFIG_ADKEY_GPIO;
	char *bat_config;
	int battery_channel = -1;

	if (adkey.initialized || gpio < 0) return;

	if (!adkey_gpio_to_channel(gpio, &adkey.channel)) {
		ESP_LOGE(TAG, "GPIO %d is not a supported ADC1 input on this target", gpio);
		return;
	}

	bat_config = config_alloc_get_default(NVS_TYPE_STR, "bat_config", NULL, 0);
	if (bat_config) {
		PARSE_PARAM(bat_config, "channel", '=', battery_channel);
		free(bat_config);
	}

	if (battery_channel == adkey.channel) {
		ESP_LOGE(TAG, "ADKEY GPIO %d conflicts with bat_config ADC1 channel %d", gpio, battery_channel);
		return;
	}

	gpio_set_pull_mode((gpio_num_t) gpio, GPIO_FLOATING);
	adc1_config_width(ADC_WIDTH_BIT_12);
	adc1_config_channel_atten(adkey.channel, ADC_ATTEN_DB_6);

	adkey.stable_state = ADKEY_STATE_IDLE;
	adkey.pending_state = ADKEY_STATE_IDLE;
	adkey.stable_count = 0;
	adkey.timer = xTimerCreate("adkey", pdMS_TO_TICKS(ADKEY_POLL_MS), pdTRUE, NULL, adkey_poll);
	if (!adkey.timer) {
		ESP_LOGE(TAG, "cannot create timer");
		return;
	}

	xTimerStart(adkey.timer, portMAX_DELAY);
	adkey.initialized = true;
	adkey.enabled = true;

	ESP_LOGI(TAG, "ADKEY on GPIO %d ADC1 channel %d poll=%dms stable=%d hysteresis=%d",
			 gpio, adkey.channel, ADKEY_POLL_MS, ADKEY_STABLE_SAMPLES, ADKEY_HYSTERESIS);
	ESP_LOGI(TAG, "ADKEY centers idle>=%d 3k=%d 6.2k=%d 9.1k=%d",
			 ADKEY_IDLE_THRESHOLD, ADKEY_3K_CENTER, ADKEY_6K2_CENTER, ADKEY_9K1_CENTER);
}
