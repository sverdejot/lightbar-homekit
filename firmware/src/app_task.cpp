// SPDX-License-Identifier: MIT
/*
 * Matter dimmable light -> Xiaomi Mi Computer Monitor Light Bar (MJGJD01YL).
 *
 * Apple Home / Siri control the OnOff and LevelControl clusters over Thread.
 * Changes are debounced and translated into the lamp's proprietary commands,
 * which lamp_radio.c sends in MPSL timeslots alongside Thread and BLE.
 */
#include "app_task.h"
#include "lamp_radio.h"

#include "app/matter_init.h"
#include "app/task_executor.h"

#include <app-common/zap-generated/attributes/Accessors.h>
#include <app-common/zap-generated/ids/Attributes.h>
#include <app-common/zap-generated/ids/Clusters.h>
#include <app/ConcreteAttributePath.h>
#include <app/clusters/identify-server/identify-server.h>
#include <app/server/Server.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include <cstdlib>
#include <cstring>

LOG_MODULE_DECLARE(app, CONFIG_CHIP_APP_LOG_LEVEL);

using namespace ::chip;
using namespace ::chip::app;
using namespace ::chip::DeviceLayer;

namespace
{
constexpr EndpointId kLightEndpointId = 1;
constexpr int kApplyDelayMs = 300;

const struct gpio_dt_spec sLed = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
volatile bool sThreadAttached;

/* Desired state (from Matter) and what we believe the lamp shows. */
volatile bool sWantOn;
volatile uint8_t sWantLevel = 254;
struct {
	bool on;
	int bri; /* 0..15, -1 unknown */
} sLamp = { false, -1 };

int LevelToBri(uint8_t level)
{
	if (level <= 1) {
		return 0;
	}
	return ((level - 1) * 15 + 126) / 253;
}

void SetAbsolute(uint8_t cmd, int value)
{
	/* Saturate to the lowest end, then step up (lamperez/xiaomi-lightbar-nrf24) */
	lamp_send(cmd, 0xF0);
	lamp_send(cmd, static_cast<uint8_t>(value));
}

void ApplyWorkHandler(k_work *)
{
	bool wantOn = sWantOn;
	int wantBri = LevelToBri(sWantLevel);

	if (wantOn != sLamp.on) {
		lamp_send(LAMP_CMD_TOGGLE, 0);
		sLamp.on = wantOn;
		LOG_INF("lamp -> %s", wantOn ? "ON" : "OFF");
	}
	if (sLamp.on && wantBri != sLamp.bri) {
		SetAbsolute(LAMP_CMD_BRIGHT, wantBri);
		sLamp.bri = wantBri;
		LOG_INF("lamp bri -> %d", wantBri);
	}
}

K_WORK_DELAYABLE_DEFINE(sApplyWork, ApplyWorkHandler);

void IdentifyStart(Identify *)
{
	/* Blink the lamp: two toggles */
	lamp_send(LAMP_CMD_TOGGLE, 0);
	lamp_send(LAMP_CMD_TOGGLE, 0);
}

void IdentifyStop(Identify *) {}

Identify sIdentify = { kLightEndpointId, IdentifyStart, IdentifyStop,
		       Clusters::Identify::IdentifyTypeEnum::kVisibleIndicator };

void MatterEventHandler(const ChipDeviceEvent *event, intptr_t)
{
	if (event->Type == DeviceEventType::kThreadStateChange ||
	    event->Type == DeviceEventType::kCommissioningComplete) {
		sThreadAttached = ConnectivityMgr().IsThreadAttached();
		LOG_INF("thread attached: %d", sThreadAttached);
	}
}

/* LED: slow blink until attached to the Thread network, then solid. */
void LedThread()
{
	gpio_pin_configure_dt(&sLed, GPIO_OUTPUT_INACTIVE);
	for (bool blink = false;; blink = !blink) {
		gpio_pin_set_dt(&sLed, sThreadAttached ? 1 : blink);
		k_sleep(K_MSEC(500));
	}
}

K_THREAD_DEFINE(led_thread_id, 512, LedThread, NULL, NULL, NULL, K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/* Remote id persisted in settings: lamp/id */
int LampSettingsSet(const char *name, size_t len, settings_read_cb readCb, void *cbArg)
{
	if (!strcmp(name, "id") && len == sizeof(uint32_t)) {
		uint32_t id;
		if (readCb(cbArg, &id, sizeof(id)) == sizeof(id)) {
			lamp_set_remote_id(id);
		}
	}
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(lamp, "lamp", NULL, LampSettingsSet, NULL, NULL);
} /* namespace */

void AppTask::OnOffChanged(bool on)
{
	sWantOn = on;
	k_work_reschedule(&sApplyWork, K_MSEC(kApplyDelayMs));
}

void AppTask::LevelChanged(uint8_t level)
{
	sWantLevel = level;
	k_work_reschedule(&sApplyWork, K_MSEC(kApplyDelayMs));
}

/* ---------------------------------------------------------- Matter hooks */

void MatterPostAttributeChangeCallback(const ConcreteAttributePath &path, uint8_t type, uint16_t size,
				       uint8_t *value)
{
	if (path.mClusterId == Clusters::OnOff::Id && path.mAttributeId == Clusters::OnOff::Attributes::OnOff::Id) {
		AppTask::Instance().OnOffChanged(*value);
	} else if (path.mClusterId == Clusters::LevelControl::Id &&
		   path.mAttributeId == Clusters::LevelControl::Attributes::CurrentLevel::Id) {
		AppTask::Instance().LevelChanged(*value);
	}
}

void emberAfOnOffClusterInitCallback(EndpointId endpoint)
{
	/* Assume the lamp matches the persisted state after a reboot; don't toggle it. */
	bool on = false;
	Clusters::OnOff::Attributes::OnOff::Get(endpoint, &on);
	sWantOn = on;
	sLamp.on = on;

	Clusters::LevelControl::Attributes::CurrentLevel::TypeInfo::Type level;
	if (Clusters::LevelControl::Attributes::CurrentLevel::Get(endpoint, level) ==
		    Protocols::InteractionModel::Status::Success &&
	    !level.IsNull()) {
		sWantLevel = level.Value();
	}
}

/* ------------------------------------------------------------------ app */

CHIP_ERROR AppTask::Init()
{
	ReturnErrorOnFailure(Nrf::Matter::PrepareServer());
	/* Matter only loads its own settings subtree; load ours explicitly. */
	settings_load_subtree("lamp");
	ReturnErrorOnFailure(Nrf::Matter::RegisterEventHandler(MatterEventHandler, 0));
	return Nrf::Matter::StartServer();
}

CHIP_ERROR AppTask::StartApp()
{
	ReturnErrorOnFailure(Init());

	while (true) {
		Nrf::DispatchNextTask();
	}

	return CHIP_NO_ERROR;
}

/* ---------------------------------------------------------------- shell */

static int CmdToggle(const struct shell *sh, size_t, char **)
{
	lamp_send(LAMP_CMD_TOGGLE, 0);
	sLamp.on = !sLamp.on;
	shell_print(sh, "toggle queued, lamp believed %s", sLamp.on ? "ON" : "OFF");
	return 0;
}

static int CmdBri(const struct shell *sh, size_t, char **argv)
{
	int v = CLAMP(atoi(argv[1]), 0, 15);
	SetAbsolute(LAMP_CMD_BRIGHT, v);
	sLamp.bri = v;
	return 0;
}

static int CmdId(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		uint32_t id = strtoul(argv[1], NULL, 16) & 0xFFFFFF;
		lamp_set_remote_id(id);
		settings_save_one("lamp/id", &id, sizeof(id));
	}
	shell_print(sh, "remote id %06X", lamp_get_remote_id());
	return 0;
}

static int CmdState(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 1) {
		sLamp.on = !strcmp(argv[1], "on");
	}
	shell_print(sh, "lamp believed %s bri=%d | matter on=%d level=%d | thread=%d", sLamp.on ? "ON" : "OFF",
		    sLamp.bri, sWantOn, sWantLevel, sThreadAttached);
	return 0;
}

static int CmdBootloader(const struct shell *sh, size_t, char **)
{
	shell_print(sh, "rebooting into UF2 bootloader");
	k_sleep(K_MSEC(100));
	NRF_POWER->GPREGRET = 0x57; /* Adafruit bootloader: DFU_MAGIC_UF2_RESET */
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

static int CmdPair(const struct shell *sh, size_t, char **)
{
	/* The bar accepts a new remote id when it gets a reset command within
	 * ~20 s of being powered on (same as long-pressing the original remote).
	 */
	for (int i = 0; i < 3; i++) {
		lamp_send(LAMP_CMD_RESET, 0);
	}
	shell_print(sh, "reset sent with id %06X; the bar should flash once", lamp_get_remote_id());
	return 0;
}

static int CmdDfu(const struct shell *sh, size_t, char **)
{
	shell_print(sh, "rebooting into serial DFU bootloader");
	k_sleep(K_MSEC(100));
	NRF_POWER->GPREGRET = 0x4E; /* Adafruit bootloader: DFU_MAGIC_SERIAL_ONLY_RESET */
	sys_reboot(SYS_REBOOT_COLD);
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(lamp_cmds, SHELL_CMD_ARG(toggle, NULL, "Toggle lamp", CmdToggle, 1, 0),
			       SHELL_CMD_ARG(bri, NULL, "Brightness 0-15", CmdBri, 2, 0),
			       SHELL_CMD_ARG(id, NULL, "Remote id [hex]", CmdId, 1, 1),
			       SHELL_CMD_ARG(pair, NULL, "Pair bar to current id (power-cycle bar first)", CmdPair, 1, 0),
			       SHELL_CMD_ARG(state, NULL, "Show state / set belief [on|off]", CmdState, 1, 1),
			       SHELL_CMD_ARG(bootloader, NULL, "Reboot into UF2 bootloader", CmdBootloader, 1, 0),
			       SHELL_CMD_ARG(dfu, NULL, "Reboot into serial DFU (for big images)", CmdDfu, 1, 0),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(lamp, &lamp_cmds, "Xiaomi lightbar", NULL);
