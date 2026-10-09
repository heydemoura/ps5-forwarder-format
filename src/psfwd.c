/*
 * psfwd - PS5 Forwarder Format, version 1: the reference library.
 * Copyright (C) 2026 heydemoura
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "psfwd.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define MAGIC 0x4C574650u /* "PFWL" */
#define PROTOCOL_VERSION 1u
#define REQUEST_SIZE 0x1020
#define REPLY_SIZE 0x18
#define QUEUED_RC 0x80940010u /* the caller is still the running app */
#define MAX_JSON (64 * 1024)

/* IDs the format reserves: the emulators that publish one, and the template. */
static const char *const kReserved[] = {"PPSA99008", "PPSA99764", "PPSA99203",
                                        "PPSA50011", "PPSA00000", "PPSA99100"};

/* Tags every forwarder program logs with; a real app's eboot.bin carries none. */
static const char *const kProgramTags[] = {"[ps5-forwarder %s]", "[prospero-forwarder %s]"};

static void set_error(char *error, size_t size, const char *format, ...)
{
    if (error == NULL || size == 0)
        return;
    va_list args;
    va_start(args, format);
    (void)vsnprintf(error, size, format, args);
    va_end(args);
}

/* out = a + "/" + b. Returns 0 (and leaves out empty) when it does not fit. */
static int join(char *out, size_t size, const char *a, const char *b)
{
    const size_t la = strlen(a);
    const size_t lb = strlen(b);
    if (la + 1 + lb + 1 > size)
    {
        if (size > 0)
            out[0] = '\0';
        return 0;
    }
    memcpy(out, a, la);
    out[la] = '/';
    memcpy(out + la + 1, b, lb + 1);
    return 1;
}

/* ---- small text builder ------------------------------------------------- */

typedef struct text
{
    char *out;
    size_t size;
    size_t length;
    int overflow;
} text;

static void put(text *t, const char *s, size_t n)
{
    if (t->overflow || t->length + n + 1 > t->size)
    {
        t->overflow = 1;
        return;
    }
    memcpy(t->out + t->length, s, n);
    t->length += n;
    t->out[t->length] = '\0';
}

static void puts_(text *t, const char *s)
{
    put(t, s, strlen(s));
}

/* A JSON string, quoted and escaped. */
static void put_json_string(text *t, const char *s)
{
    puts_(t, "\"");
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; ++p)
    {
        char escaped[8];
        switch (*p)
        {
        case '"': puts_(t, "\\\""); break;
        case '\\': puts_(t, "\\\\"); break;
        case '\n': puts_(t, "\\n"); break;
        case '\r': puts_(t, "\\r"); break;
        case '\t': puts_(t, "\\t"); break;
        default:
            if (*p < 0x20)
            {
                (void)snprintf(escaped, sizeof(escaped), "\\u%04x", *p);
                puts_(t, escaped);
            }
            else
                put(t, (const char *)p, 1);
        }
    }
    puts_(t, "\"");
}

/* ---- file helpers --------------------------------------------------------- */

static int exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int make_dirs(const char *path)
{
    char buffer[1024];
    const size_t n = strlen(path);
    if (n == 0 || n >= sizeof(buffer))
        return 0;
    memcpy(buffer, path, n + 1);
    for (size_t i = 1; i <= n; ++i)
    {
        if (buffer[i] == '/' || buffer[i] == '\0')
        {
            const char saved = buffer[i];
            buffer[i] = '\0';
            if (mkdir(buffer, 0777) != 0 && errno != EEXIST)
                return 0;
            buffer[i] = saved;
        }
    }
    return is_dir(path);
}

static int write_file(const char *path, const void *data, size_t size)
{
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return 0;
    const unsigned char *bytes = (const unsigned char *)data;
    size_t done = 0;
    while (done < size)
    {
        const ssize_t n = write(fd, bytes + done, size - done);
        if (n <= 0)
        {
            close(fd);
            return 0;
        }
        done += (size_t)n;
    }
    return close(fd) == 0;
}

/* Reads up to max bytes into a malloc-free caller buffer; returns the length or -1. */
static long read_into(const char *path, char *out, size_t max)
{
    const int fd = open(path, O_RDONLY);
    if (fd < 0)
        return -1;
    size_t done = 0;
    for (;;)
    {
        if (done + 1 >= max)
            break;
        const ssize_t n = read(fd, out + done, max - 1 - done);
        if (n <= 0)
            break;
        done += (size_t)n;
    }
    close(fd);
    out[done] = '\0';
    return (long)done;
}

/* Copies a file of any size in chunks, creating it with mode. */
static int copy_file_mode(const char *from, const char *to, mode_t mode)
{
    const int in = open(from, O_RDONLY);
    if (in < 0)
        return 0;
    const int out = open(to, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0)
    {
        close(in);
        return 0;
    }
    char buffer[65536];
    int ok = 1;
    for (;;)
    {
        const ssize_t n = read(in, buffer, sizeof(buffer));
        if (n == 0)
            break;
        if (n < 0 || write(out, buffer, (size_t)n) != n)
        {
            ok = 0;
            break;
        }
    }
    close(in);
    return close(out) == 0 && ok;
}

/* Data files (art, JSON). */
static int copy_file(const char *from, const char *to)
{
    return copy_file_mode(from, to, 0666);
}

/* The forwarder program and its runtime must stay executable, or the
 * console refuses to start the tile (EACCES). open() leaves an existing
 * file's mode alone, so the mode is set explicitly as well. */
static int copy_program(const char *from, const char *to)
{
    return copy_file_mode(from, to, 0777) && chmod(to, 0777) == 0;
}

static int remove_tree(const char *path)
{
    DIR *dir = opendir(path);
    if (dir != NULL)
    {
        struct dirent *entry;
        while ((entry = readdir(dir)) != NULL)
        {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            char child[1024];
            (void)join(child, sizeof(child), path, entry->d_name);
            if (is_dir(child))
                remove_tree(child);
            else
                (void)unlink(child);
        }
        closedir(dir);
    }
    return rmdir(path) == 0 || !exists(path);
}

/* Whether a file contains a byte string, read in overlapping chunks. */
static int file_contains(const char *path, const char *needle)
{
    const size_t n = strlen(needle);
    const int fd = open(path, O_RDONLY);
    if (fd < 0 || n == 0)
    {
        if (fd >= 0)
            close(fd);
        return 0;
    }
    char buffer[65536];
    size_t kept = 0;
    int found = 0;
    for (;;)
    {
        const ssize_t got = read(fd, buffer + kept, sizeof(buffer) - kept);
        if (got <= 0)
            break;
        const size_t have = kept + (size_t)got;
        for (size_t i = 0; i + n <= have; ++i)
        {
            if (memcmp(buffer + i, needle, n) == 0)
            {
                found = 1;
                break;
            }
        }
        if (found)
            break;
        kept = n - 1 < have ? n - 1 : have;
        memmove(buffer, buffer + have - kept, kept);
    }
    close(fd);
    return found;
}

/* Whether two files have the same bytes. */
static int same_file(const char *a, const char *b)
{
    const int fa = open(a, O_RDONLY);
    const int fb = open(b, O_RDONLY);
    int same = fa >= 0 && fb >= 0;
    char ba[16384];
    char bb[16384];
    while (same)
    {
        const ssize_t na = read(fa, ba, sizeof(ba));
        ssize_t nb = 0;
        while (nb < na)
        {
            const ssize_t got = read(fb, bb + nb, (size_t)(na - nb));
            if (got <= 0)
                break;
            nb += got;
        }
        if (na != nb || memcmp(ba, bb, (size_t)na) != 0)
            same = 0;
        if (na <= 0)
        {
            char extra;
            if (same && read(fb, &extra, 1) > 0)
                same = 0;
            break;
        }
    }
    if (fa >= 0)
        close(fa);
    if (fb >= 0)
        close(fb);
    return same;
}

/* ---- IDs ---------------------------------------------------------------------- */

int psfwd_valid_title_id(const char *id)
{
    if (id == NULL || strlen(id) != 9)
        return 0;
    if (memcmp(id, "PPSA", 4) != 0 && memcmp(id, "CUSA", 4) != 0 && memcmp(id, "LAPY", 4) != 0)
        return 0;
    for (int i = 4; i < 9; ++i)
        if (id[i] < '0' || id[i] > '9')
            return 0;
    return 1;
}

static int reserved(const char *id)
{
    for (size_t i = 0; i < sizeof(kReserved) / sizeof(kReserved[0]); ++i)
        if (strcmp(id, kReserved[i]) == 0)
            return 1;
    return 0;
}

int psfwd_new_title_id(const char *root, char out[PSFWD_TITLE_ID_SIZE])
{
    unsigned seed = (unsigned)time(NULL) ^ (unsigned)getpid();
    for (int attempt = 0; attempt < 4096; ++attempt)
    {
        seed = seed * 1103515245u + 12345u;
        const int number = 99200 + (int)((seed >> 8) % 700u);
        char id[PSFWD_TITLE_ID_SIZE];
        (void)snprintf(id, sizeof(id), "PPSA%05d", number);
        if (reserved(id))
            continue;
        if (root != NULL && root[0] != '\0')
        {
            char path[1024];
            (void)join(path, sizeof(path), root, id);
            if (exists(path))
                continue;
        }
        memcpy(out, id, sizeof(id));
        return 1;
    }
    return 0;
}

/* ---- generating the files ----------------------------------------------------- */

size_t psfwd_forwarder_json(const psfwd_spec *spec, char *out, size_t size)
{
    text t = {out, size, 0, 0};
    if (size == 0)
        return 0;
    out[0] = '\0';
    puts_(&t, "{\n  \"format\": ");
    put_json_string(&t, PSFWD_FORMAT_TAG);
    puts_(&t, ",\n  \"target\": ");
    put_json_string(&t, spec->target != NULL ? spec->target : "");
    puts_(&t, ",\n  \"args\": [");
    for (int i = 0; i < spec->argc; ++i)
    {
        puts_(&t, i == 0 ? "\n    " : ",\n    ");
        put_json_string(&t, spec->argv[i]);
    }
    puts_(&t, spec->argc > 0 ? "\n  ]" : "]");
    if (spec->creator != NULL && spec->creator[0] != '\0')
    {
        puts_(&t, ",\n  \"creator\": ");
        put_json_string(&t, spec->creator);
    }
    puts_(&t, "\n}\n");
    return t.overflow ? 0 : t.length;
}

size_t psfwd_param_json(const psfwd_spec *spec, char *out, size_t size)
{
    text t = {out, size, 0, 0};
    if (size == 0)
        return 0;
    out[0] = '\0';
    const char *id = spec->title_id != NULL ? spec->title_id : "";
    const char *concept = strlen(id) == 9 ? id + 4 : id;
    const char *name = spec->name != NULL && spec->name[0] != '\0' ? spec->name : "PS5 App";
    char line[160];

    puts_(&t, "{\n  \"ageLevel\": {\n    \"default\": 0\n  },\n");
    puts_(&t, "  \"applicationCategoryType\": 0,\n  \"applicationDrmType\": \"free\",\n");
    puts_(&t, "  \"attribute\": 0,\n  \"attribute2\": 0,\n  \"attribute3\": 0,\n");
    puts_(&t, "  \"conceptId\": ");
    put_json_string(&t, concept);
    puts_(&t, ",\n  \"contentBadgeType\": 1,\n");
    (void)snprintf(line, sizeof(line), "  \"contentId\": \"UP9000-%s_00-PS5FORWARDER0000\",\n", id);
    puts_(&t, line);
    puts_(&t, "  \"contentVersion\": \"01.000.000\",\n  \"downloadDataSize\": 256,\n");
    puts_(&t, "  \"gameIntent\": {\n    \"permittedIntents\": [\n      {\n"
              "        \"intentType\": \"launchActivity\"\n      }\n    ]\n  },\n");
    puts_(&t, "  \"localizedParameters\": {\n    \"defaultLanguage\": \"en-US\",\n"
              "    \"en-US\": {\n      \"titleName\": ");
    put_json_string(&t, name);
    puts_(&t, "\n    }\n  },\n");
    puts_(&t, "  \"masterVersion\": \"01.00\",\n");
    puts_(&t, "  \"pubtools\": {\n    \"creationDate\": \"2026-08-22 00:00:00\",\n"
              "    \"loudnessSnd0\": \"-28.00\",\n    \"toolVersion\": \"2.00\"\n  },\n");
    puts_(&t, "  \"requiredSystemSoftwareVersion\": \"0x0000000000000000\",\n");
    puts_(&t, "  \"sdkVersion\": \"0x0000000000000000\",\n  \"titleId\": ");
    put_json_string(&t, id);
    puts_(&t, ",\n  \"versionFileUri\": \"\"\n}\n");
    return t.overflow ? 0 : t.length;
}

/* ---- writing -------------------------------------------------------------------- */

static int place_art(const char *stage_sys, const char *old_sys, int editing, psfwd_blob blob,
                     const char *name)
{
    char target[1024];
    (void)join(target, sizeof(target), stage_sys, name);
    if (blob.data != NULL && blob.size > 0)
        return write_file(target, blob.data, blob.size);
    char prior[1024];
    (void)join(prior, sizeof(prior), old_sys, name);
    if (editing && exists(prior))
        return copy_file(prior, target);
    return 1; /* optional, absent */
}

int psfwd_write(const char *root, const char *template_dir, const psfwd_spec *spec, char *error,
                size_t error_size)
{
    if (spec == NULL || !psfwd_valid_title_id(spec->title_id))
    {
        set_error(error, error_size, "the forwarder's title ID must be PPSA and five digits");
        return 0;
    }
    if (!psfwd_valid_title_id(spec->target))
    {
        set_error(error, error_size, "the target must be a title ID like PPSA99008");
        return 0;
    }
    if (strcmp(spec->target, spec->title_id) == 0)
    {
        set_error(error, error_size, "the target is the forwarder's own title ID");
        return 0;
    }
    if (spec->argc < 0 || spec->argc > PSFWD_MAX_ARGS)
    {
        set_error(error, error_size, "too many launch arguments");
        return 0;
    }
    size_t args_bytes = 0;
    for (int i = 0; i < spec->argc; ++i)
        args_bytes += strlen(spec->argv[i]) + 1;
    if (args_bytes > PSFWD_ARGS_SIZE)
    {
        set_error(error, error_size, "the launch arguments are too long");
        return 0;
    }

    char final_dir[1024], old_sys[1024], stage[1024], stage_sys[1024], stage_module[1024];
    char path[1024], from[1024];
    if (strlen(root) > 900 || !join(final_dir, sizeof(final_dir), root, spec->title_id))
    {
        set_error(error, error_size, "the forwarders folder path is too long");
        return 0;
    }
    (void)join(old_sys, sizeof(old_sys), final_dir, "sce_sys");
    (void)join(path, sizeof(path), final_dir, "forwarder.json");
    const int editing = exists(path);
    (void)join(path, sizeof(path), old_sys, "icon0.png");
    if ((spec->icon0_png.data == NULL || spec->icon0_png.size == 0) && !(editing && exists(path)))
    {
        set_error(error, error_size, "a tile icon (icon0.png) is required");
        return 0;
    }

    /* Stage beside the final folder, so a scan never catches a partial tile. */
    char staging_name[PSFWD_TITLE_ID_SIZE + 16];
    (void)snprintf(staging_name, sizeof(staging_name), ".staging-%s", spec->title_id);
    (void)join(stage, sizeof(stage), root, staging_name);
    (void)join(stage_sys, sizeof(stage_sys), stage, "sce_sys");
    (void)join(stage_module, sizeof(stage_module), stage, "sce_module");
    remove_tree(stage);
    if (!make_dirs(root) || !make_dirs(stage_sys) || !make_dirs(stage_module))
    {
        set_error(error, error_size, "cannot create folders under %s", root);
        remove_tree(stage);
        return 0;
    }

    static const char *const program[][2] = {{"eboot.bin", "eboot.bin"},
                                             {"sce_module/libc.prx", "sce_module/libc.prx"}};
    for (size_t i = 0; i < 2; ++i)
    {
        (void)join(from, sizeof(from), template_dir, program[i][0]);
        (void)join(path, sizeof(path), stage, program[i][1]);
        if (!copy_program(from, path))
        {
            set_error(error, error_size, "missing template file %s", from);
            remove_tree(stage);
            return 0;
        }
    }

    static char body[MAX_JSON];
    size_t length = psfwd_forwarder_json(spec, body, sizeof(body));
    (void)join(path, sizeof(path), stage, "forwarder.json");
    if (length == 0 || !write_file(path, body, length))
    {
        set_error(error, error_size, "cannot write forwarder.json");
        remove_tree(stage);
        return 0;
    }
    length = psfwd_param_json(spec, body, sizeof(body));
    (void)join(path, sizeof(path), stage_sys, "param.json");
    if (length == 0 || !write_file(path, body, length))
    {
        set_error(error, error_size, "cannot write sce_sys/param.json");
        remove_tree(stage);
        return 0;
    }

    if (!place_art(stage_sys, old_sys, editing, spec->icon0_png, "icon0.png") ||
        !place_art(stage_sys, old_sys, editing, spec->pic0_dds, "pic0.dds") ||
        !place_art(stage_sys, old_sys, editing, spec->pic1_dds, "pic1.dds") ||
        !place_art(stage_sys, old_sys, editing, spec->snd0_at9, "snd0.at9"))
    {
        set_error(error, error_size, "cannot write the pictures or music");
        remove_tree(stage);
        return 0;
    }

    remove_tree(final_dir);
    if (rename(stage, final_dir) != 0)
    {
        set_error(error, error_size, "cannot publish the forwarder: %s", strerror(errno));
        remove_tree(stage);
        return 0;
    }
    return 1;
}

int psfwd_remove(const char *root, const char *title_id)
{
    if (!psfwd_valid_title_id(title_id))
        return 0;
    char path[1024];
    (void)join(path, sizeof(path), root, title_id);
    return remove_tree(path);
}

/* ---- reading: a small JSON reader for forwarder.json and param.json ----------- */

typedef struct reader
{
    const char *s;
    size_t at;
    size_t n;
} reader;

static void space(reader *r)
{
    while (r->at < r->n && (r->s[r->at] == ' ' || r->s[r->at] == '\t' || r->s[r->at] == '\r' ||
                            r->s[r->at] == '\n'))
        ++r->at;
}

static int eat(reader *r, char c)
{
    space(r);
    if (r->at < r->n && r->s[r->at] == c)
    {
        ++r->at;
        return 1;
    }
    return 0;
}

static int emit(char *out, size_t cap, size_t *len, unsigned char c)
{
    if (out == NULL)
        return 1; /* skipping */
    if (*len + 1 >= cap)
        return 0;
    out[(*len)++] = (char)c;
    return 1;
}

/* A string into out (may be NULL to skip it). */
static int read_string(reader *r, char *out, size_t cap)
{
    space(r);
    if (r->at >= r->n || r->s[r->at] != '"')
        return 0;
    ++r->at;
    size_t len = 0;
    while (r->at < r->n && r->s[r->at] != '"')
    {
        unsigned char c = (unsigned char)r->s[r->at++];
        if (c == '\\' && r->at < r->n)
        {
            const char e = r->s[r->at++];
            if (e == 'u')
            {
                if (r->at + 4 > r->n)
                    return 0;
                char hex[5] = {r->s[r->at], r->s[r->at + 1], r->s[r->at + 2], r->s[r->at + 3], 0};
                const unsigned code = (unsigned)strtoul(hex, NULL, 16);
                r->at += 4;
                if (code < 0x80)
                {
                    if (!emit(out, cap, &len, (unsigned char)code))
                        return 0;
                }
                else if (code < 0x800)
                {
                    if (!emit(out, cap, &len, (unsigned char)(0xC0 | (code >> 6))) ||
                        !emit(out, cap, &len, (unsigned char)(0x80 | (code & 0x3F))))
                        return 0;
                }
                else if (!emit(out, cap, &len, (unsigned char)(0xE0 | (code >> 12))) ||
                         !emit(out, cap, &len, (unsigned char)(0x80 | ((code >> 6) & 0x3F))) ||
                         !emit(out, cap, &len, (unsigned char)(0x80 | (code & 0x3F))))
                    return 0;
                continue;
            }
            c = e == 'n' ? '\n' : e == 't' ? '\t' : e == 'r' ? '\r' : e == 'b' ? '\b'
                : e == 'f' ? '\f' : (unsigned char)e;
        }
        if (!emit(out, cap, &len, c))
            return 0;
    }
    if (r->at >= r->n)
        return 0;
    ++r->at;
    if (out != NULL)
        out[len] = '\0';
    return 1;
}

/* Skips any value. */
static int skip_value(reader *r)
{
    space(r);
    if (r->at >= r->n)
        return 0;
    const char c = r->s[r->at];
    if (c == '"')
        return read_string(r, NULL, 0);
    if (c == '{' || c == '[')
    {
        const char close_c = c == '{' ? '}' : ']';
        ++r->at;
        if (eat(r, close_c))
            return 1;
        do
        {
            if (c == '{' && (!read_string(r, NULL, 0) || !eat(r, ':')))
                return 0;
            if (!skip_value(r))
                return 0;
        } while (eat(r, ','));
        return eat(r, close_c);
    }
    /* number, true, false, null */
    while (r->at < r->n && r->s[r->at] != ',' && r->s[r->at] != '}' && r->s[r->at] != ']' &&
           r->s[r->at] != ' ' && r->s[r->at] != '\n' && r->s[r->at] != '\r' && r->s[r->at] != '\t')
        ++r->at;
    return 1;
}

static int read_bool(reader *r, int *value)
{
    space(r);
    if (r->n - r->at >= 4 && memcmp(r->s + r->at, "true", 4) == 0)
    {
        r->at += 4;
        *value = 1;
        return 1;
    }
    if (r->n - r->at >= 5 && memcmp(r->s + r->at, "false", 5) == 0)
    {
        r->at += 5;
        *value = 0;
        return 1;
    }
    return 0;
}

static int add_arg(psfwd_info *info, size_t *used, const char *arg)
{
    const size_t n = strlen(arg) + 1;
    if (info->argc >= PSFWD_MAX_ARGS || *used + n > sizeof(info->args))
        return 0;
    memcpy(info->args + *used, arg, n);
    info->argv[info->argc++] = info->args + *used;
    *used += n;
    return 1;
}

int psfwd_is_forwarder(const char *dir)
{
    char path[1024];
    (void)join(path, sizeof(path), dir, "forwarder.json");
    if (!exists(path))
        return 0;
    (void)join(path, sizeof(path), dir, "eboot.bin");
    return exists(path);
}

/* The tile name from param.json: the default language's titleName. */
static void read_name(const char *dir, psfwd_info *info)
{
    static char param[MAX_JSON];
    char path[1024];
    (void)join(path, sizeof(path), dir, "sce_sys/param.json");
    const long n = read_into(path, param, sizeof(param));
    if (n <= 0)
        return;
    reader r = {param, 0, (size_t)n};
    char language[32] = "en-US";
    if (!eat(&r, '{'))
        return;
    do
    {
        char key[64];
        if (!read_string(&r, key, sizeof(key)) || !eat(&r, ':'))
            return;
        if (strcmp(key, "localizedParameters") != 0)
        {
            if (!skip_value(&r))
                return;
            continue;
        }
        /* Two passes over the object: the default language first. */
        const size_t start = r.at;
        for (int pass = 0; pass < 2; ++pass)
        {
            r.at = start;
            if (!eat(&r, '{'))
                return;
            do
            {
                char lang[64];
                if (!read_string(&r, lang, sizeof(lang)) || !eat(&r, ':'))
                    return;
                if (pass == 0 && strcmp(lang, "defaultLanguage") == 0)
                {
                    if (!read_string(&r, language, sizeof(language)))
                        return;
                    continue;
                }
                if (pass == 1 && strcmp(lang, language) == 0 && eat(&r, '{'))
                {
                    do
                    {
                        char field[64];
                        if (!read_string(&r, field, sizeof(field)) || !eat(&r, ':'))
                            return;
                        if (strcmp(field, "titleName") == 0)
                        {
                            (void)read_string(&r, info->name, sizeof(info->name));
                            return;
                        }
                        if (!skip_value(&r))
                            return;
                    } while (eat(&r, ','));
                    return;
                }
                if (!skip_value(&r))
                    return;
            } while (eat(&r, ','));
        }
        return;
    } while (eat(&r, ','));
}

int psfwd_read(const char *dir, psfwd_info *info)
{
    memset(info, 0, sizeof(*info));
    if (!psfwd_is_forwarder(dir))
        return 0;
    static char config[MAX_JSON];
    char path[1024];
    (void)join(path, sizeof(path), dir, "forwarder.json");
    const long n = read_into(path, config, sizeof(config));
    if (n <= 0)
        return 0;

    /* The folder name is the forwarder's title ID. */
    const char *slash = strrchr(dir, '/');
    (void)snprintf(info->title_id, sizeof(info->title_id), "%s", slash != NULL ? slash + 1 : dir);

    reader r = {config, 0, (size_t)n};
    size_t used = 0;
    char legacy_rom[1024] = "";
    int legacy_exit = 0;
    if (!eat(&r, '{'))
        return 0;
    if (!eat(&r, '}'))
    {
        do
        {
            char key[64];
            if (!read_string(&r, key, sizeof(key)) || !eat(&r, ':'))
                return 0;
            if (strcmp(key, "target") == 0)
            {
                if (!read_string(&r, info->target, sizeof(info->target)))
                    return 0;
            }
            else if (strcmp(key, "format") == 0)
            {
                char tag[64];
                if (!read_string(&r, tag, sizeof(tag)))
                    return 0;
                if (strncmp(tag, "ps5-forwarder/", 14) == 0)
                    info->format = atoi(tag + 14);
            }
            else if (strcmp(key, "creator") == 0)
            {
                if (!read_string(&r, info->creator, sizeof(info->creator)))
                    return 0;
            }
            else if (strcmp(key, "args") == 0)
            {
                if (!eat(&r, '['))
                    return 0;
                if (!eat(&r, ']'))
                {
                    do
                    {
                        char arg[PSFWD_ARGS_SIZE];
                        if (!read_string(&r, arg, sizeof(arg)) || !add_arg(info, &used, arg))
                            return 0;
                    } while (eat(&r, ','));
                    if (!eat(&r, ']'))
                        return 0;
                }
            }
            else if (strcmp(key, "rom") == 0) /* before the standard */
            {
                if (!read_string(&r, legacy_rom, sizeof(legacy_rom)))
                    return 0;
            }
            else if (strcmp(key, "exit_after_game") == 0) /* before the standard */
            {
                if (!read_bool(&r, &legacy_exit))
                    return 0;
            }
            else if (!skip_value(&r))
                return 0;
        } while (eat(&r, ','));
        if (!eat(&r, '}'))
            return 0;
    }
    if (legacy_rom[0] != '\0' && (!add_arg(info, &used, "--rom") || !add_arg(info, &used, legacy_rom)))
        return 0;
    if (legacy_exit && !add_arg(info, &used, "--exit-after-game"))
        return 0;

    read_name(dir, info);
    (void)join(path, sizeof(path), dir, "sce_sys/icon0.png");
    info->has_icon = exists(path);
    (void)join(path, sizeof(path), dir, "sce_sys/pic0.dds");
    info->has_backgrounds = exists(path);
    (void)join(path, sizeof(path), dir, "sce_sys/snd0.at9");
    info->has_music = exists(path);
    return 1;
}

/* ---- upgrading ------------------------------------------------------------------- */

int psfwd_upgrade(const char *root, const char *template_dir)
{
    char current[1024];
    (void)join(current, sizeof(current), template_dir, "eboot.bin");
    if (!exists(current))
        return 0;
    DIR *dir = opendir(root);
    if (dir == NULL)
        return 0;
    int upgraded = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL)
    {
        if (entry->d_name[0] == '.')
            continue;
        char folder[1024], eboot[1100], staged[1100];
        (void)join(folder, sizeof(folder), root, entry->d_name);
        if (!is_dir(folder) || !psfwd_is_forwarder(folder))
            continue;
        (void)join(eboot, sizeof(eboot), folder, "eboot.bin");
        if (same_file(eboot, current))
        {
            /* Already current; make sure it can still be started. */
            struct stat st;
            if (stat(eboot, &st) == 0 && (st.st_mode & 0111) != 0111 && chmod(eboot, 0777) == 0)
                ++upgraded;
            continue;
        }
        int program = 0;
        for (size_t i = 0; i < sizeof(kProgramTags) / sizeof(kProgramTags[0]) && !program; ++i)
            program = file_contains(eboot, kProgramTags[i]);
        if (!program)
            continue; /* not a forwarder program: leave it alone */
        (void)join(staged, sizeof(staged), folder, "eboot.bin.new");
        if (copy_program(current, staged) && rename(staged, eboot) == 0)
            ++upgraded;
        else
            (void)unlink(staged);
    }
    closedir(dir);
    return upgraded;
}

/* ---- the launcher client ----------------------------------------------------------- */

static int connect_local(unsigned short port, int timeout_seconds)
{
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct timeval timeout = {timeout_seconds, 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0)
    {
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const unsigned char *data, size_t size)
{
    size_t done = 0;
    while (done < size)
    {
        const ssize_t n = write(fd, data + done, size - done);
        if (n <= 0)
            return 0;
        done += (size_t)n;
    }
    return 1;
}

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static uint32_t get_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

int psfwd_launcher_running(void)
{
    const int fd = connect_local(PSFWD_LAUNCHER_PORT, 2);
    if (fd < 0)
        return 0;
    close(fd); /* an empty connection is a probe */
    return 1;
}

int psfwd_ensure_launcher(const unsigned char *elf, size_t elf_size)
{
    if (psfwd_launcher_running())
        return 1;
    if (elf == NULL || elf_size == 0)
        return 0;
    const int fd = connect_local(PSFWD_ELFLDR_PORT, 5);
    if (fd < 0)
        return 0;
    const int sent = send_all(fd, elf, elf_size);
    (void)shutdown(fd, SHUT_WR); /* elfldr runs what it got once we close */
    char sink[256];
    while (read(fd, sink, sizeof(sink)) > 0)
    {
    }
    close(fd);
    if (!sent)
        return 0;
    for (int attempt = 0; attempt < 30; ++attempt)
    {
        usleep(100 * 1000);
        if (psfwd_launcher_running())
            return 1;
    }
    return 0;
}

int psfwd_launch(const char *target, int argc, const char *const *argv,
                 psfwd_launch_result *result)
{
    psfwd_launch_result local;
    if (result == NULL)
        result = &local;
    memset(result, 0, sizeof(*result));
    static unsigned char request[REQUEST_SIZE];
    memset(request, 0, sizeof(request));
    if (!psfwd_valid_title_id(target) || argc < 0 || argc > PSFWD_MAX_ARGS)
    {
        result->status = PSFWD_BAD_REQUEST;
        return 0;
    }
    put_u32(request + 0x00, MAGIC);
    put_u32(request + 0x04, PROTOCOL_VERSION);
    memcpy(request + 0x08, target, strlen(target));
    size_t used = 0;
    for (int i = 0; i < argc; ++i)
    {
        const size_t n = strlen(argv[i]) + 1;
        if (used + n > PSFWD_ARGS_SIZE)
        {
            result->status = PSFWD_BAD_REQUEST;
            return 0;
        }
        memcpy(request + 0x20 + used, argv[i], n);
        used += n;
    }
    put_u32(request + 0x18, (uint32_t)argc);
    put_u32(request + 0x1C, (uint32_t)used);

    const int fd = connect_local(PSFWD_LAUNCHER_PORT, 10);
    if (fd < 0)
    {
        result->status = PSFWD_NO_LAUNCHER;
        return 0;
    }
    if (!send_all(fd, request, sizeof(request)))
    {
        close(fd);
        result->status = PSFWD_NO_REPLY;
        return 0;
    }
    unsigned char reply[REPLY_SIZE];
    size_t got = 0;
    while (got < sizeof(reply))
    {
        const ssize_t n = read(fd, reply + got, sizeof(reply) - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (got != sizeof(reply) || get_u32(reply) != MAGIC)
    {
        result->status = PSFWD_NO_REPLY;
        return 0;
    }
    result->stage = get_u32(reply + 0x08);
    result->first_rc = get_u32(reply + 0x0C);
    result->second_rc = get_u32(reply + 0x10);
    result->user = (int32_t)get_u32(reply + 0x14);
    switch (result->stage)
    {
    case 0: result->status = PSFWD_LAUNCHED; break;
    case 1: result->status = PSFWD_BAD_REQUEST; break;
    case 2: result->status = PSFWD_NO_USER; break;
    case 3:
        result->status = result->first_rc == QUEUED_RC && result->second_rc == QUEUED_RC
                             ? PSFWD_QUEUED
                             : PSFWD_REFUSED;
        break;
    default: result->status = PSFWD_BAD_REQUEST; break;
    }
    return result->status == PSFWD_LAUNCHED || result->status == PSFWD_QUEUED;
}

const char *psfwd_launch_status_text(enum psfwd_launch_status status)
{
    switch (status)
    {
    case PSFWD_LAUNCHED: return "started";
    case PSFWD_QUEUED: return "will start once this app closes";
    case PSFWD_NO_LAUNCHER: return "no launcher is running";
    case PSFWD_BAD_REQUEST: return "the launcher refused the request";
    case PSFWD_NO_USER: return "nobody is signed in";
    case PSFWD_REFUSED: return "the system refused to start the app";
    case PSFWD_NO_REPLY: return "the launcher did not answer";
    }
    return "unknown";
}
