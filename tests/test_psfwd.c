/*
 * PS5 Forwarder Format - host tests for the reference library.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Builds forwarders in a temporary folder and checks the files against the
 * format, then exercises the launcher protocol against launcher/launcher.c
 * built for the host (see tests/Makefile). Run with `make test`.
 */
#include "psfwd.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_failures = 0;
#define CHECK(cond)                                                                       \
    do                                                                                    \
    {                                                                                     \
        if (!(cond))                                                                      \
        {                                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);               \
            ++g_failures;                                                                 \
        }                                                                                 \
    } while (0)

static char g_root[256];
static char g_template[256];

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    fputs(text, f);
    fclose(f);
}

static long read_text(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL)
        return -1;
    const size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    out[n] = '\0';
    return (long)n;
}

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void path_of(char *out, const char *a, const char *b)
{
    snprintf(out, 512, "%s/%s", a, b);
}

static void test_ids(void)
{
    CHECK(psfwd_valid_title_id("PPSA99123"));
    CHECK(psfwd_valid_title_id("CUSA00001"));
    CHECK(!psfwd_valid_title_id("PPSA9912"));
    CHECK(!psfwd_valid_title_id("XXXX99123"));
    CHECK(!psfwd_valid_title_id("PPSA9912a"));
    char id[PSFWD_TITLE_ID_SIZE];
    CHECK(psfwd_new_title_id(g_root, id));
    CHECK(psfwd_valid_title_id(id));
    const int number = atoi(id + 4);
    CHECK(number >= 99200 && number <= 99899);
}

static void test_write_read(void)
{
    const char *argv[] = {"--rom", "Some \"Game\".nsp", "--exit-after-game"};
    const unsigned char png[] = {0x89, 'P', 'N', 'G', 1, 2, 3};
    const unsigned char dds[] = {'D', 'D', 'S', ' ', 9};
    psfwd_spec spec = {0};
    spec.title_id = "PPSA99300";
    spec.name = "Zelda: Tears";
    spec.target = "PPSA99008";
    spec.argc = 3;
    spec.argv = argv;
    spec.creator = "psfwd tests";
    spec.icon0_png = (psfwd_blob){png, sizeof(png)};
    spec.pic0_dds = (psfwd_blob){dds, sizeof(dds)};
    spec.pic1_dds = (psfwd_blob){dds, sizeof(dds)};
    char error[256] = "";
    CHECK(psfwd_write(g_root, g_template, &spec, error, sizeof(error)));
    if (error[0] != '\0')
        fprintf(stderr, "  write error: %s\n", error);

    char dir[512], path[512], body[8192];
    path_of(dir, g_root, "PPSA99300");
    path_of(path, dir, "forwarder.json");
    CHECK(read_text(path, body, sizeof(body)) > 0);
    CHECK(strstr(body, "\"format\": \"ps5-forwarder/1\"") != NULL);
    CHECK(strstr(body, "\"target\": \"PPSA99008\"") != NULL);
    CHECK(strstr(body, "\"Some \\\"Game\\\".nsp\"") != NULL);
    CHECK(strstr(body, "\"creator\": \"psfwd tests\"") != NULL);
    path_of(path, dir, "sce_sys/param.json");
    CHECK(read_text(path, body, sizeof(body)) > 0);
    CHECK(strstr(body, "\"titleId\": \"PPSA99300\"") != NULL);
    CHECK(strstr(body, "\"conceptId\": \"99300\"") != NULL);
    CHECK(strstr(body, "UP9000-PPSA99300_00-PS5FORWARDER0000") != NULL);
    CHECK(strstr(body, "\"titleName\": \"Zelda: Tears\"") != NULL);
    struct stat st;
    path_of(path, dir, "eboot.bin");
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0111) == 0111);
    path_of(path, dir, "sce_module/libc.prx");
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0111) == 0111);
    path_of(path, dir, "sce_sys/snd0.at9");
    CHECK(!exists(path));
    path_of(path, g_root, ".staging-PPSA99300");
    CHECK(!exists(path));

    psfwd_info info;
    CHECK(psfwd_read(dir, &info));
    CHECK(strcmp(info.title_id, "PPSA99300") == 0);
    CHECK(strcmp(info.name, "Zelda: Tears") == 0);
    CHECK(strcmp(info.target, "PPSA99008") == 0);
    CHECK(strcmp(info.creator, "psfwd tests") == 0);
    CHECK(info.format == 1);
    CHECK(info.argc == 3);
    CHECK(info.argc == 3 && strcmp(info.argv[1], "Some \"Game\".nsp") == 0);
    CHECK(info.has_icon && info.has_backgrounds && !info.has_music);

    /* An edit without art keeps the art it had. */
    spec.name = "Renamed";
    spec.icon0_png = (psfwd_blob){0};
    spec.pic0_dds = (psfwd_blob){0};
    spec.pic1_dds = (psfwd_blob){0};
    spec.argc = 0;
    CHECK(psfwd_write(g_root, g_template, &spec, error, sizeof(error)));
    CHECK(psfwd_read(dir, &info));
    CHECK(strcmp(info.name, "Renamed") == 0);
    CHECK(info.argc == 0);
    CHECK(info.has_icon && info.has_backgrounds);

    /* A new forwarder needs an icon; a bad target is refused. */
    spec.title_id = "PPSA99301";
    CHECK(!psfwd_write(g_root, g_template, &spec, error, sizeof(error)));
    spec.icon0_png = (psfwd_blob){png, sizeof(png)};
    spec.target = "PPSA99301";
    CHECK(!psfwd_write(g_root, g_template, &spec, error, sizeof(error)));
    spec.target = "nonsense";
    CHECK(!psfwd_write(g_root, g_template, &spec, error, sizeof(error)));
}

static void test_legacy(void)
{
    char dir[512], path[512];
    path_of(dir, g_root, "PPSA99400");
    mkdir(dir, 0777);
    path_of(path, dir, "sce_sys");
    mkdir(path, 0777);
    path_of(path, dir, "eboot.bin");
    write_text(path, "old program [prospero-forwarder %s] old");
    path_of(path, dir, "forwarder.json");
    write_text(path, "{\"target\": \"PPSA99764\", \"rom\": \"Melee.iso\", "
                     "\"exit_after_game\": true, \"extra\": {\"a\": [1, 2]}}");
    path_of(path, dir, "sce_sys/param.json");
    write_text(path, "{\"localizedParameters\": {\"en-US\": {\"titleName\": \"Melee\"}, "
                     "\"defaultLanguage\": \"en-US\"}, \"titleId\": \"PPSA99400\"}");
    psfwd_info info;
    CHECK(psfwd_read(dir, &info));
    CHECK(info.format == 0);
    CHECK(strcmp(info.target, "PPSA99764") == 0);
    CHECK(strcmp(info.name, "Melee") == 0);
    CHECK(info.argc == 3);
    CHECK(info.argc == 3 && strcmp(info.argv[0], "--rom") == 0 &&
          strcmp(info.argv[1], "Melee.iso") == 0 && strcmp(info.argv[2], "--exit-after-game") == 0);
}

static const char *path_of_upgraded(void)
{
    static char path[512];
    snprintf(path, sizeof(path), "%s/PPSA99400/eboot.bin", g_root);
    return path;
}

static void test_upgrade(void)
{
    char dir[512], path[512], body[256];
    /* A real app: no forwarder.json. */
    path_of(dir, g_root, "PPSA99008");
    mkdir(dir, 0777);
    path_of(path, dir, "eboot.bin");
    write_text(path, "emulator");
    /* A folder with a forwarder.json but some other program. */
    path_of(dir, g_root, "PPSA99500");
    mkdir(dir, 0777);
    path_of(path, dir, "eboot.bin");
    write_text(path, "someone else's app");
    path_of(path, dir, "forwarder.json");
    write_text(path, "{\"target\": \"PPSA99008\"}");

    /* PPSA99300 has the current program, PPSA99400 an older one. */
    CHECK(psfwd_upgrade(g_root, g_template) == 1);
    path_of(path, g_root, "PPSA99400/eboot.bin");
    read_text(path, body, sizeof(body));
    CHECK(strstr(body, "current program") != NULL);
    path_of(path, g_root, "PPSA99008/eboot.bin");
    read_text(path, body, sizeof(body));
    CHECK(strcmp(body, "emulator") == 0);
    path_of(path, g_root, "PPSA99500/eboot.bin");
    read_text(path, body, sizeof(body));
    CHECK(strcmp(body, "someone else's app") == 0);
    CHECK(psfwd_upgrade(g_root, g_template) == 0);
    struct stat st;
    CHECK(stat(path_of_upgraded(), &st) == 0 && (st.st_mode & 0111) == 0111);
    /* A current program that lost its execute bits is repaired. */
    chmod(path_of_upgraded(), 0644);
    CHECK(psfwd_upgrade(g_root, g_template) == 1);
    CHECK(stat(path_of_upgraded(), &st) == 0 && (st.st_mode & 0111) == 0111);
}

static void test_remove(void)
{
    char path[512];
    CHECK(psfwd_remove(g_root, "PPSA99400"));
    path_of(path, g_root, "PPSA99400");
    CHECK(!exists(path));
    CHECK(!psfwd_remove(g_root, "../etc"));
}

/* The protocol against launcher/launcher.c built for the host. */
static void test_launcher(const char *launcher_binary)
{
    CHECK(!psfwd_launcher_running());
    psfwd_launch_result result;
    const char *argv[] = {"--rom", "game.nsp"};
    CHECK(!psfwd_launch("PPSA99008", 2, argv, &result));
    CHECK(result.status == PSFWD_NO_LAUNCHER);

    const pid_t pid = fork();
    if (pid == 0)
    {
        execl(launcher_binary, launcher_binary, (char *)NULL);
        _exit(127);
    }
    for (int i = 0; i < 50 && !psfwd_launcher_running(); ++i)
        usleep(20000);
    CHECK(psfwd_launcher_running());
    CHECK(psfwd_ensure_launcher(NULL, 0)); /* already running: nothing to send */

    CHECK(psfwd_launch("PPSA99008", 2, argv, &result));
    CHECK(result.status == PSFWD_LAUNCHED);
    CHECK(result.user == 0x17BFF889);
    /* The stub refuses PPSA00001 with the code a still-running caller gets. */
    CHECK(psfwd_launch("PPSA00001", 0, NULL, &result));
    CHECK(result.status == PSFWD_QUEUED);
    /* ... and PPSA00002 outright. */
    CHECK(!psfwd_launch("PPSA00002", 0, NULL, &result));
    CHECK(result.status == PSFWD_REFUSED);
    CHECK(!psfwd_launch("BAD", 0, NULL, &result));
    CHECK(result.status == PSFWD_BAD_REQUEST);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
}

int main(int argc, char **argv)
{
    snprintf(g_root, sizeof(g_root), "/tmp/psfwd-test-%d/homebrew", (int)getpid());
    snprintf(g_template, sizeof(g_template), "/tmp/psfwd-test-%d/template", (int)getpid());
    char path[512];
    snprintf(path, sizeof(path), "/tmp/psfwd-test-%d", (int)getpid());
    mkdir(path, 0777);
    mkdir(g_root, 0777);
    mkdir(g_template, 0777);
    path_of(path, g_template, "sce_module");
    mkdir(path, 0777);
    path_of(path, g_template, "eboot.bin");
    write_text(path, "current program [ps5-forwarder %s] current");
    path_of(path, g_template, "sce_module/libc.prx");
    write_text(path, "libc");

    test_ids();
    test_write_read();
    test_legacy();
    test_upgrade();
    test_remove();
    if (argc > 1)
        test_launcher(argv[1]);

    snprintf(path, sizeof(path), "rm -rf /tmp/psfwd-test-%d", (int)getpid());
    if (system(path) != 0)
        fprintf(stderr, "could not clean up\n");
    if (g_failures == 0)
        printf("psfwd tests: all passed\n");
    return g_failures == 0 ? 0 : 1;
}
