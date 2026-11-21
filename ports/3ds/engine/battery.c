/* The battery level for the icon on the top screen (gpu.c): the power
 * controller's percentage (mcu::HWC) when the service is there, else the
 * power service's 0..5 steps (ptm:u). Asked every two seconds. */
#include "engine.h"
#include <3ds.h>

static bool have_ptm, have_mcu;
static u64 last_ask;
static int last_pct = -1;
static bool last_charging;

void battery_init(void)
{
	have_ptm = R_SUCCEEDED(ptmuInit());
	have_mcu = R_SUCCEEDED(mcuHwcInit());
}

bool battery_get(int *percent, bool *charging)
{
	u64 now = svcGetSystemTick();
	if (last_pct < 0 || now - last_ask > 2 * SYSCLOCK_ARM11) {
		last_ask = now;
		u8 v = 0, steps = 0;
		int pct = -1;
		bool got_steps = have_ptm && R_SUCCEEDED(PTMU_GetBatteryLevel(&steps));
		/* (the percentage only when it fits the power service's step: in
		 * the emulator the controller answers with the step, 5) */
		if (have_mcu && R_SUCCEEDED(MCUHWC_GetBatteryLevel(&v)) && v > 0 && v <= 100 &&
		    (!got_steps || (v >= steps * 20 - 45 && v <= steps * 20 + 25)))
			pct = v;
		else if (got_steps)
			pct = steps >= 5 ? 100 : steps * 20; /* (0..5) */
		static bool told;
		if (!told) {
			told = true;
			log_only("battery: mcu %d (%u), ptm %d (%u)\n", have_mcu, v, got_steps, steps);
		}
		u8 c = 0;
		last_charging = have_ptm && R_SUCCEEDED(PTMU_GetBatteryChargeState(&c)) && c;
		if (pct >= 0)
			last_pct = pct;
	}
	*percent = last_pct;
	*charging = last_charging;
	return last_pct >= 0;
}
