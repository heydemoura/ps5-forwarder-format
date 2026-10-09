# PS5 Forwarder Format

A shared standard for **forwarders** on a jailbroken PS5: home-screen tiles
that start another app (usually an emulator) with launch arguments, so a game
can sit on the home screen as its own tile.

With one format, a forwarder made by any tool or emulator works the same,
needs nothing loaded beforehand but elfldr, and can be read, edited and
upgraded by any other. This repository holds:

| | |
|---|---|
| [`SPEC.md`](SPEC.md) | The format, version 1: folder layout, `forwarder.json`, `param.json`, art, title IDs, the launcher protocol, starting the launcher, upgrading. |
| [`include/psfwd.h`](include/psfwd.h), [`src/psfwd.c`](src/psfwd.c) | **psfwd**, the reference library: create, read, upgrade and remove forwarders; start the launcher; ask it to launch an app. C99, no dependencies, no heap. |
| [`template/`](template/) | The files every forwarder shares, prebuilt: `eboot.bin` (the forwarder program), `sce_module/libc.prx` (its runtime) and `launcher.elf` (the launcher payload). |
| [`launcher/`](launcher/launcher.c) | The launcher payload's source: starts apps for forwarders on 127.0.0.1:10199. |
| [`forwarder/`](forwarder/src/main.c) | The forwarder program's source. It carries the launcher and starts it through elfldr when none is running. |
| [`schema/`](schema/forwarder.schema.json) | JSON schema for `forwarder.json`. |
| [`registry/`](registry/targets.json) | Apps forwarders start, with their title IDs and arguments. Add yours by pull request. |

Everything is open source (GPL-3.0-or-later), including the launcher, which
replaces the closed-source ps5-app-launcher while speaking the same protocol.

## For emulator authors: "add this game to the home screen"

Add this repository as a submodule, compile `src/psfwd.c` with your sources,
put `include/` on the include path, and ship `template/` with your app (or
fetch it from a release). Then:

```c
#include "psfwd.h"

/* icon_png: a 512x512 PNG you already have (a cover, say). */
int add_to_home_screen(const char *game_path, const char *game_name,
                       const unsigned char *icon_png, size_t icon_size)
{
    char id[PSFWD_TITLE_ID_SIZE];
    if (!psfwd_new_title_id(PSFWD_DEFAULT_ROOT, id))
        return 0;

    const char *argv[] = {"--rom", game_path, "--exit-after-game"};
    psfwd_spec spec = {0};
    spec.title_id = id;
    spec.name = game_name;
    spec.target = "PPSA99008";            /* your own title ID */
    spec.argc = 3;
    spec.argv = argv;
    spec.creator = "MyEmulator 1.2";
    spec.icon0_png.data = icon_png;
    spec.icon0_png.size = icon_size;

    char error[256];
    return psfwd_write(PSFWD_DEFAULT_ROOT, "/app0/assets/psfwd-template", &spec,
                       error, sizeof(error));
}
```

Writing under `/data` needs the app to have filesystem access (on most setups
through a jailbreak daemon such as Lapy). The tile appears once the homebrew
mounter (ShadowMountPlus) registers it.

Accept `--rom <path>` and `--exit-after-game` on your side (SPEC.md, section
10), and add your title ID to [`registry/targets.json`](registry/targets.json).

Other calls: `psfwd_read()` to list and edit forwarders, `psfwd_remove()`,
`psfwd_upgrade()` to give existing forwarders the current program, and
`psfwd_ensure_launcher()` / `psfwd_launch()` to start an app the way a
forwarder does.

## For tool authors without C

The format is plain files: copy `template/eboot.bin` and
`template/sce_module/libc.prx` into a folder named after a free title ID, write
`forwarder.json` and `sce_sys/param.json` as SPEC.md describes, add a 512x512
`sce_sys/icon0.png`, and place the folder in `/data/homebrew/`.

## Building

```sh
make test                      # host tests: library and launcher protocol
make launcher PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk   # template/launcher.elf
make embed                     # forwarder/src/launcher_payload.inc from it
```

### Building the template

The forwarder program is a small PS5 app built with
[ps5-native-app-boilerplate](https://github.com/blackbearreloaded/ps5-native-app-boilerplate).
From a boilerplate checkout that contains this repository (Forwarder Manager
keeps it at `external/ps5-forwarder-format`), after `make launcher` and
`make embed` here:

```sh
APP_SOURCE_DIR=external/ps5-forwarder-format/forwarder/src \
APP_PARAM=external/ps5-forwarder-format/forwarder/param.json \
APP_INCLUDE_PATHS=external/ps5-forwarder-format/include \
APP_SCE_SYS=sce_sys APP_ASSETS= bash tools/build.sh Folder
cp dist/PPSA99100/eboot.bin external/ps5-forwarder-format/template/eboot.bin
cp dist/PPSA99100/sce_module/libc.prx external/ps5-forwarder-format/template/sce_module/
```

Forwarder Manager's `make` does all of this.

## Users

- [Forwarder Manager](https://github.com/heydemoura/ps5-forwarder-manager): creates and
  edits forwarders on the console, and upgrades existing ones.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
