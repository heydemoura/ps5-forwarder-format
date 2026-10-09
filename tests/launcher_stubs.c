/*
 * PS5 Forwarder Format - stand-ins for the console calls the launcher makes,
 * so launcher/launcher.c can run on a host for the protocol tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include <stdint.h>
#include <string.h>

struct launch_param
{
    uint32_t size;
    int32_t user_id;
    uint8_t reserved[24];
};

int sceUserServiceInitialize(void *params)
{
    (void)params;
    return 0;
}

int sceUserServiceGetForegroundUser(int32_t *user)
{
    *user = 0x17BFF889;
    return 0;
}

int sceSystemServiceLaunchApp(const char *title, char *const argv[], struct launch_param *param)
{
    (void)argv;
    (void)param;
    if (strcmp(title, "PPSA00001") == 0)
        return (int)0x80940010u; /* the caller is still running: queued */
    if (strcmp(title, "PPSA00002") == 0)
        return (int)0x80020010u; /* refused */
    return 0;
}
