// SPDX-License-Identifier: MIT
/*
 * Proprietary lamp TX through MPSL timeslots.
 *
 * Each command is sent as N short timeslots ("groups"). In every timeslot the
 * radio is configured as an nRF24L01+ compatible transmitter (Nordic 2 Mbit,
 * 5-byte address 0x5555555555, static 17-byte payload, no CRC, no PCF) and the
 * packet is sent REPS_PER_GROUP times on each of the 4 channels the bar listens
 * on. Between timeslots the 802.15.4 driver gets the radio back, so the Zigbee
 * link keeps working.
 */
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <string.h>
#include <errno.h>

#include <mpsl_timeslot.h>
#include <hal/nrf_radio.h>

#include "lamp_radio.h"

LOG_MODULE_REGISTER(lamp_radio, LOG_LEVEL_INF);

#define PAYLOAD_LEN       17
#define REPS_PER_GROUP    2
#define GROUP_GAP_MS      8
#define DEFAULT_GROUPS    12
#define TIMESLOT_LEN_US   2500
#define TIMESLOT_WAIT_MS  60

static const uint8_t preamble[8] = {0x53, 0x39, 0x14, 0xDD, 0x1C, 0x49, 0x34, 0x12};
static const uint8_t channels[4] = {6, 15, 43, 68};

struct lamp_msg {
	uint8_t cmd;
	uint8_t opt;
};

K_MSGQ_DEFINE(lamp_msgq, sizeof(struct lamp_msg), 16, 1);

static uint32_t remote_id = CONFIG_LIGHTBAR_REMOTE_ID;
static uint8_t groups = DEFAULT_GROUPS;
static uint8_t counter;

/* Packet buffer read by the radio EasyDMA inside the timeslot. */
static uint8_t tx_packet[PAYLOAD_LEN];

enum ts_state { TS_IDLE, TS_PENDING, TS_DONE, TS_FAILED };
static volatile enum ts_state ts_state;
static mpsl_timeslot_session_id_t session_id = 0xFF;
static mpsl_timeslot_signal_return_param_t ts_ret;

static mpsl_timeslot_request_t ts_req = {
	.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST,
	.params.earliest = {
		.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED,
		.priority = MPSL_TIMESLOT_PRIORITY_NORMAL,
		.length_us = TIMESLOT_LEN_US,
		.timeout_us = 50000,
	},
};

static uint16_t crc16(const uint8_t *d, size_t n)
{
	uint16_t crc = 0xFFFE; /* poly 0x1021, init 0xFFFE, no reflection, no xorout */

	for (size_t i = 0; i < n; i++) {
		crc ^= (uint16_t)d[i] << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
		}
	}
	return crc;
}

/* Runs inside the timeslot (MPSL TIMER0 IRQ context). ~150 us per packet. */
static void radio_tx_group(void)
{
	NRF_RADIO->INTENCLR = 0xFFFFFFFF;
	NRF_RADIO->POWER = 1;

	NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit << RADIO_MODE_MODE_Pos;
	NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Fast << RADIO_MODECNF0_RU_Pos) |
			      (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);
	NRF_RADIO->PCNF0 = 0; /* no S0, LENGTH or S1 fields: raw static payload */
	NRF_RADIO->PCNF1 = (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos) |
			   (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
			   (4 << RADIO_PCNF1_BALEN_Pos) |
			   (PAYLOAD_LEN << RADIO_PCNF1_STATLEN_Pos) |
			   (PAYLOAD_LEN << RADIO_PCNF1_MAXLEN_Pos);
	/* nRF24 address 0x5555555555, bit-reversed per byte as nrf_to_nrf does */
	NRF_RADIO->BASE0 = 0xAAAAAAAA;
	NRF_RADIO->PREFIX0 = 0xAA;
	NRF_RADIO->TXADDRESS = 0;
	NRF_RADIO->CRCCNF = 0;
	NRF_RADIO->TXPOWER = RADIO_TXPOWER_TXPOWER_Pos8dBm << RADIO_TXPOWER_TXPOWER_Pos;
	NRF_RADIO->PACKETPTR = (uint32_t)tx_packet;
	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk | RADIO_SHORTS_END_DISABLE_Msk;

	for (int rep = 0; rep < REPS_PER_GROUP; rep++) {
		for (int c = 0; c < ARRAY_SIZE(channels); c++) {
			NRF_RADIO->FREQUENCY = channels[c];
			NRF_RADIO->EVENTS_DISABLED = 0;
			NRF_RADIO->TASKS_TXEN = 1;
			while (NRF_RADIO->EVENTS_DISABLED == 0) {
			}
		}
	}
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->SHORTS = 0;
}

static mpsl_timeslot_signal_return_param_t *ts_callback(mpsl_timeslot_session_id_t sid,
							 uint32_t signal)
{
	ARG_UNUSED(sid);

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		radio_tx_group();
		ts_state = TS_DONE;
		ts_ret.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		return &ts_ret;
	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		ts_state = TS_FAILED;
		return NULL;
	default:
		return NULL;
	}
}

static void build_packet(uint8_t cmd, uint8_t opt)
{
	memcpy(tx_packet, preamble, sizeof(preamble));
	tx_packet[8] = remote_id >> 16;
	tx_packet[9] = remote_id >> 8;
	tx_packet[10] = remote_id;
	tx_packet[11] = 0xFF;
	tx_packet[12] = counter++;
	tx_packet[13] = cmd;
	tx_packet[14] = opt;
	uint16_t c = crc16(tx_packet, 15);
	tx_packet[15] = c >> 8;
	tx_packet[16] = c & 0xFF;
}

/* Returns true if the group was transmitted. */
static bool send_group(void)
{
	ts_state = TS_PENDING;
	int32_t err = mpsl_timeslot_request(session_id, &ts_req);

	if (err) {
		LOG_ERR("timeslot request failed: %d", err);
		return false;
	}
	for (int i = 0; i < TIMESLOT_WAIT_MS && ts_state == TS_PENDING; i++) {
		k_sleep(K_MSEC(1));
	}
	return ts_state == TS_DONE;
}

/* MPSL APIs must be called from a cooperative (non-preemptible) thread. */
static void lamp_thread(void)
{
	struct lamp_msg msg;
	int32_t err = mpsl_timeslot_session_open(ts_callback, &session_id);

	if (err) {
		LOG_ERR("timeslot session open failed: %d", err);
		return;
	}
	counter = (uint8_t)sys_rand32_get();
	LOG_INF("lamp radio ready, remote id %06X", remote_id);

	while (k_msgq_get(&lamp_msgq, &msg, K_FOREVER) == 0) {
		int ok = 0;

		build_packet(msg.cmd, msg.opt);
		for (int g = 0; g < groups; g++) {
			ok += send_group();
			k_sleep(K_MSEC(GROUP_GAP_MS));
		}
		LOG_INF("sent cmd=%02X opt=%02X cnt=%u groups %d/%d", msg.cmd, msg.opt,
			tx_packet[12], ok, groups);
	}
}

K_THREAD_DEFINE(lamp_thread_id, 1024, lamp_thread, NULL, NULL, NULL,
		K_PRIO_COOP(CONFIG_MPSL_THREAD_COOP_PRIO), 0, 0);

int lamp_send(uint8_t cmd, uint8_t opt)
{
	struct lamp_msg msg = {.cmd = cmd, .opt = opt};

	return k_msgq_put(&lamp_msgq, &msg, K_NO_WAIT) ? -ENOMEM : 0;
}

void lamp_set_remote_id(uint32_t id)
{
	remote_id = id & 0xFFFFFF;
}

uint32_t lamp_get_remote_id(void)
{
	return remote_id;
}

void lamp_set_groups(uint8_t g)
{
	groups = CLAMP(g, 1, 100);
}

uint8_t lamp_get_groups(void)
{
	return groups;
}
