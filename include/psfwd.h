/*
 * psfwd - PS5 Forwarder Format, version 1: the reference library.
 * Copyright (C) 2026 heydemoura
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A forwarder is a home-screen tile that starts another app (an emulator,
 * usually) with launch arguments. This library lets any app - Forwarder
 * Manager, or an emulator offering "add this game to the home screen" - create,
 * read, upgrade and remove forwarders in the standard format described in
 * SPEC.md, and talk to the forwarder launcher.
 *
 * Plain C99 on POSIX file and socket calls: no other dependency, so it builds
 * into PS5 apps, payloads and host tools alike. Compile src/psfwd.c with your
 * sources and include this header. Nothing here allocates on the heap.
 */
#ifndef PSFWD_H
#define PSFWD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the format -------------------------------------------------------- */

#define PSFWD_FORMAT_VERSION 1
#define PSFWD_FORMAT_TAG "ps5-forwarder/1" /* forwarder.json "format" */

#define PSFWD_TITLE_ID_SIZE 16  /* "PPSA99123" plus room; always NUL-terminated */
#define PSFWD_NAME_SIZE 256     /* the tile name, UTF-8 */
#define PSFWD_CREATOR_SIZE 128  /* who made the forwarder, UTF-8 */
#define PSFWD_MAX_ARGS 64       /* launch arguments */
#define PSFWD_ARGS_SIZE 4096    /* their bytes, NULs included */

/* Where forwarders live by default, and where their program comes from. */
#define PSFWD_DEFAULT_ROOT "/data/homebrew"

/* A byte buffer the caller owns. */
typedef struct psfwd_blob
{
    const unsigned char *data;
    size_t size;
} psfwd_blob;

/* What psfwd_write() needs. Strings are UTF-8 and NUL-terminated. */
typedef struct psfwd_spec
{
    const char *title_id;    /* the forwarder's own ID: PPSA + five digits */
    const char *name;        /* shown under the tile */
    const char *target;      /* the title ID of the app to start */
    int argc;                /* launch arguments passed to the target */
    const char *const *argv; /* argc strings */
    const char *creator;     /* optional, e.g. "PS5SX2 1.4"; NULL to leave out */

    /* Pictures and music, already in their console formats (SPEC.md, "Art").
     * An empty blob keeps what an existing forwarder has; a new forwarder
     * needs icon0_png. */
    psfwd_blob icon0_png; /* 512x512 PNG */
    psfwd_blob pic0_dds;  /* background behind the focused tile, BC7 DDS */
    psfwd_blob pic1_dds;  /* launch screen, BC7 DDS */
    psfwd_blob snd0_at9;  /* selection music, RIFF ATRAC9 */
} psfwd_spec;

/* What psfwd_read() finds. */
typedef struct psfwd_info
{
    char title_id[PSFWD_TITLE_ID_SIZE];
    char name[PSFWD_NAME_SIZE];
    char target[PSFWD_TITLE_ID_SIZE];
    char creator[PSFWD_CREATOR_SIZE]; /* "" when not recorded */
    int format;                       /* 1, or 0 for a forwarder from before the standard */
    int argc;
    const char *argv[PSFWD_MAX_ARGS]; /* point into args */
    char args[PSFWD_ARGS_SIZE];
    int has_icon;        /* sce_sys/icon0.png */
    int has_backgrounds; /* sce_sys/pic0.dds */
    int has_music;       /* sce_sys/snd0.at9 */
} psfwd_info;

/* PPSA, CUSA or LAPY followed by five digits. */
int psfwd_valid_title_id(const char *title_id);

/* A free forwarder ID in the PPSA99200-PPSA99899 range (SPEC.md, "Title
 * IDs"), not used under root and not reserved. Returns 1 and fills out. */
int psfwd_new_title_id(const char *root, char out[PSFWD_TITLE_ID_SIZE]);

/* Creates the forwarder root/<title_id>, or replaces it, from template_dir
 * (a folder holding eboot.bin and sce_module/libc.prx: the template/ folder
 * of this repository). The new folder is built beside the old one and renamed
 * into place, so a tile is never half written. Returns 1, or 0 with a reason
 * in error. */
int psfwd_write(const char *root, const char *template_dir, const psfwd_spec *spec, char *error,
                size_t error_size);

/* Reads the forwarder in dir. Returns 1, or 0 when dir is not a forwarder.
 * Understands forwarders from before the standard (no "format", and the old
 * "rom" / "exit_after_game" keys, which come back as arguments). */
int psfwd_read(const char *dir, psfwd_info *info);

/* dir holds a forwarder.json and an eboot.bin. */
int psfwd_is_forwarder(const char *dir);

/* Removes root/<title_id>. Returns 1 when it is gone. */
int psfwd_remove(const char *root, const char *title_id);

/* Gives every forwarder under root the forwarder program in template_dir,
 * leaving anything that is not a forwarder program alone (SPEC.md,
 * "Upgrading"). Returns how many were changed. */
int psfwd_upgrade(const char *root, const char *template_dir);

/* The file bodies psfwd_write() writes, for tools that build folders
 * elsewhere. Return the length written (excluding the NUL), or 0 when out is
 * too small. */
size_t psfwd_forwarder_json(const psfwd_spec *spec, char *out, size_t size);
size_t psfwd_param_json(const psfwd_spec *spec, char *out, size_t size);

/* ---- the launcher ---------------------------------------------------------
 * An app may not start another app, so forwarders ask a resident payload on
 * 127.0.0.1:10199 (SPEC.md, "The launcher"). */

#ifndef PSFWD_LAUNCHER_PORT
#define PSFWD_LAUNCHER_PORT 10199
#endif
#ifndef PSFWD_ELFLDR_PORT
#define PSFWD_ELFLDR_PORT 9021
#endif

/* Something answers on the launcher port. */
int psfwd_launcher_running(void);

/* Makes sure a launcher runs: when none answers, sends elf (the launcher
 * payload, template/launcher.elf) to elfldr on 127.0.0.1:9021 and waits up to
 * three seconds for it. Returns 1 when a launcher answers. */
int psfwd_ensure_launcher(const unsigned char *elf, size_t elf_size);

enum psfwd_launch_status
{
    PSFWD_LAUNCHED = 0,     /* the system started the target */
    PSFWD_QUEUED = 1,       /* the system will start it once the caller closes */
    PSFWD_NO_LAUNCHER = 2,  /* nothing answers on the launcher port */
    PSFWD_BAD_REQUEST = 3,  /* the launcher refused the request */
    PSFWD_NO_USER = 4,      /* nobody is signed in */
    PSFWD_REFUSED = 5,      /* the system refused to start the target */
    PSFWD_NO_REPLY = 6,     /* the launcher did not answer */
};

typedef struct psfwd_launch_result
{
    enum psfwd_launch_status status;
    uint32_t stage;     /* the launcher's reply stage */
    uint32_t first_rc;  /* its return codes */
    uint32_t second_rc;
    int32_t user;
} psfwd_launch_result;

/* Asks the launcher to start target with argv. */
int psfwd_launch(const char *target, int argc, const char *const *argv,
                 psfwd_launch_result *result);

/* A short English description of a status. */
const char *psfwd_launch_status_text(enum psfwd_launch_status status);

#ifdef __cplusplus
}
#endif

#endif /* PSFWD_H */
