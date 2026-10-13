// SPDX-License-Identifier: GPL-3.0+
// (AI-assisted) No host Ethernet adapter in the isolated BIOS boot application.
// DEV9 register/HDD implementations remain real; networking starts disabled.
#include "DEV9/net.h"
#include "common/Console.h"
void InitNet()
{
	Console.Warning("PS4 VM Boot: host Ethernet is unavailable.");
	EmuConfig.DEV9.EthEnable = false;
}
void ReconfigureLiveNet(const Pcsx2Config&) { EmuConfig.DEV9.EthEnable = false; }
void TermNet() {}
void ad_reset() {}
void tx_put(NetPacket*) {}
