/* Emulator-only adaptation: the external oscillator's electrical POST needs
 * a continuously gated oscillator model. The board supplies timed 10 MHz
 * bursts for shot acquisition instead. All acquisition, scoring, persistence
 * and network code still comes from Software/ESP32/main unchanged.
 *
 * This file is copied into the disposable build, never the hardware firmware.
 */
#ifndef FT_EMULATOR_BUILD
#error "board_boot.c may only be built for the emulator"
#endif
#include <stdbool.h>
#include <stdio.h>

bool __wrap_POST_counters(void)
{
    puts("\nEMULATOR: external oscillator POST bypassed; shot counters use virtual 10 MHz input\n");
    return true;
}
