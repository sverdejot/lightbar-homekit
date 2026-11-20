// SPDX-License-Identifier: MIT
#pragma once

#include <platform/CHIPDeviceLayer.h>

class AppTask {
public:
	static AppTask &Instance()
	{
		static AppTask sAppTask;
		return sAppTask;
	};

	CHIP_ERROR StartApp();

	/* Called from the Matter thread when cluster attributes change. */
	void OnOffChanged(bool on);
	void LevelChanged(uint8_t level);

private:
	CHIP_ERROR Init();
};
