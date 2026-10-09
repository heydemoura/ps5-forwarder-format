/*
 * PS5 Forwarder Format - the forwarder program (every tile's eboot.bin).
 * Copyright (C) 2026 heydemoura
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Every forwarder runs this same program. It reads forwarder.json next to it
 * and asks the launcher on 127.0.0.1:10199 to start the target with the
 * arguments listed there: an app may not start another app itself. When no
 * launcher is running it starts the one it carries (launcher/, embedded at
 * build time) through elfldr first, so nothing has to be loaded beforehand.
 * See SPEC.md, "The forwarder program".
 *
 * Its lines go to the console log; a failure also raises a notification.
 */
#include "psfwd.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The launcher payload, generated from template/launcher.elf. */
#include "launcher_payload.inc"

struct notification
{
    uint8_t reserved[45];
    char message[3075];
};

int sceKernelSendNotificationRequest(uint32_t device, void *request, size_t size, int blocking);
int sceKernelDebugOutText(int channel, const char *text);

static char g_title[PSFWD_TITLE_ID_SIZE] = "PPSA00000";

/* Every line carries this tag: SPEC.md, "Upgrading", relies on it. */
static void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void log_line(const char *format, ...)
{
    char text[1024];
    char line[1100];
    va_list args;
    va_start(args, format);
    (void)vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    (void)snprintf(line, sizeof(line), "[ps5-forwarder %s] %s\n", g_title, text);
    (void)sceKernelDebugOutText(0, line);
}

static void fail(const char *message) __attribute__((noreturn));
static void fail(const char *message)
{
    log_line("FAILED: %s", message);
    static struct notification request;
    memset(&request, 0, sizeof(request));
    (void)snprintf(request.message, sizeof(request.message), "Forwarder: %s", message);
    (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    exit(1);
}

/* The tile's own title ID, from its param.json. */
static void read_own_title(void)
{
    static char param[64 * 1024];
    FILE *file = fopen("/app0/sce_sys/param.json", "rb");
    if (file == NULL)
        return;
    const size_t n = fread(param, 1, sizeof(param) - 1, file);
    fclose(file);
    param[n] = '\0';
    const char *key = strstr(param, "\"titleId\"");
    const char *colon = key != NULL ? strchr(key + 9, ':') : NULL;
    const char *open = colon != NULL ? strchr(colon, '"') : NULL;
    const char *close = open != NULL ? strchr(open + 1, '"') : NULL;
    if (open != NULL && close != NULL && close - open - 1 == 9)
    {
        memcpy(g_title, open + 1, 9);
        g_title[9] = '\0';
    }
}

int main(void)
{
    read_own_title();

    static psfwd_info config;
    if (!psfwd_read("/app0", &config))
        fail("forwarder.json is missing next to eboot.bin, or is not valid");
    log_line("config: /app0/forwarder.json (format %d%s%s)", config.format,
             config.creator[0] != '\0' ? ", made by " : "", config.creator);
    if (config.target[0] == '\0')
        fail("target is required");
    if (!psfwd_valid_title_id(config.target))
        fail("target must be a title ID like PPSA99008");
    if (strcmp(config.target, g_title) == 0)
        fail("target is this forwarder's own title ID");

    char listed[PSFWD_ARGS_SIZE + 3 * PSFWD_MAX_ARGS + 16];
    size_t used = 0;
    listed[0] = '\0';
    for (int i = 0; i < config.argc; ++i)
        used += (size_t)snprintf(listed + used, sizeof(listed) - used, " [%s]", config.argv[i]);
    log_line("starting %s with%s", config.target, config.argc == 0 ? " no arguments" : listed);

    if (psfwd_launcher_running())
        log_line("a launcher is already running");
    else
    {
        log_line("no launcher on 127.0.0.1:%d; sending the built-in one (%zu bytes) to elfldr",
                 PSFWD_LAUNCHER_PORT, sizeof(kLauncherPayload));
        if (!psfwd_ensure_launcher(kLauncherPayload, sizeof(kLauncherPayload)))
            fail("no launcher is running, and it could not be started through elfldr");
        log_line("built-in launcher is up");
    }

    log_line("sending the request");
    psfwd_launch_result result;
    const int ok = psfwd_launch(config.target, config.argc, config.argv, &result);
    log_line("launcher reply: stage=%u first_rc=0x%08X second_rc=0x%08X user=0x%08X (%s)",
             result.stage, result.first_rc, result.second_rc, (unsigned)result.user,
             psfwd_launch_status_text(result.status));
    if (!ok)
    {
        char message[200];
        (void)snprintf(message, sizeof(message), "could not start %s: %s (0x%08X, 0x%08X)",
                       config.target, psfwd_launch_status_text(result.status), result.first_rc,
                       result.second_rc);
        fail(message);
    }

    /* The system closes this tile as the target starts. */
    log_line("launch requested; waiting to be closed");
    for (int i = 0; i < 200; ++i)
        usleep(100 * 1000);
    log_line("still open after 20 s; closing");
    return 0;
}
