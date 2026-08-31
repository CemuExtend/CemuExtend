#pragma once

void LatteTiming_setCustomVsyncFrequency(sint32 frequency);
void LatteTiming_disableCustomVsyncFrequency();
bool LatteTiming_getCustomVsyncFrequency(sint32& customFrequency);
void LatteTiming_setGuestCustomVsyncFrequency(sint32 frequency);
void LatteTiming_disableGuestCustomVsyncFrequency();
bool LatteTiming_getGuestCustomVsyncFrequency(sint32& customFrequency);
sint32 LatteTiming_getEffectiveVsyncFrequency();

void LatteTiming_EnableHostDrivenVSync();
