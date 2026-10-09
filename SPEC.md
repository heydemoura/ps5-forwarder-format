# PS5 Forwarder Format, version 1

A **forwarder** is a home-screen tile on a jailbroken PS5 that starts another
app (an emulator, usually) with launch arguments: "Zelda" on the home screen
that opens ProsperoEden with `--rom Zelda.nsp`. This document defines the
format so that forwarders made by any tool or emulator look and behave the same,
and any of them can read, edit or upgrade the others.

The key words MUST, MUST NOT, SHOULD and MAY are used as in RFC 2119.

The reference implementation is in this repository: the library
[`psfwd`](include/psfwd.h), the [launcher](launcher/launcher.c) and the
[forwarder program](forwarder/src/main.c).

## 1. Folder layout

A forwarder is one folder, named after its own title ID, placed where the
console's homebrew mounter (ShadowMountPlus) registers tiles, by default
`/data/homebrew/`:

```
PPSA99231/
├── eboot.bin              the forwarder program (section 6), byte for byte
├── sce_module/libc.prx    its runtime, byte for byte
├── forwarder.json         what to start (section 2)
└── sce_sys/
    ├── param.json         the tile's identity (section 3)
    ├── icon0.png          the tile picture (section 4), required
    ├── pic0.dds           background behind the focused tile, optional
    ├── pic1.dds           launch screen, optional
    └── snd0.at9           selection music, optional
```

`eboot.bin` and `sce_module/libc.prx` are the same for every forwarder and
MUST be copied from the [`template/`](template/) folder of the version of this
format the writer implements. Nothing else may be added at the top level
except files a writer needs for its own bookkeeping, which readers MUST
ignore.

A writer SHOULD build the folder beside its final place and rename it into
place, so a scan never sees a half-written tile.

## 2. forwarder.json

UTF-8 JSON, one object:

```json
{
  "format": "ps5-forwarder/1",
  "target": "PPSA99008",
  "args": ["--rom", "Zelda.nsp", "--exit-after-game"],
  "creator": "ProsperoEden 2.1"
}
```

| Key | Type | Required | Meaning |
|---|---|---|---|
| `format` | string | yes, for writers | `ps5-forwarder/1` for this version. |
| `target` | string | yes | The title ID of the app to start (section 5). |
| `args` | array of strings | no | Launch arguments passed to the target, in order. Absent means none. |
| `creator` | string | no | What made the forwarder, for display and support ("PS5SX2 1.4"). |

- At most 64 arguments, and at most 4096 bytes for all of them counting a
  terminating NUL each.
- Readers MUST ignore keys they do not know, so later versions can add some.
- Readers SHOULD accept forwarders from before this standard: no `format`
  key, and the older keys `rom` (string) and `exit_after_game` (boolean),
  which mean the arguments `--rom <rom>` and `--exit-after-game`, appended
  after `args`.
- The file is read each time the tile is opened, so it may be edited at any
  time.

The [JSON schema](schema/forwarder.schema.json) states the same rules.

## 3. sce_sys/param.json

The console reads the tile's name and identity here. A forwarder's
`param.json` MUST contain:

| Key | Value |
|---|---|
| `titleId` | The forwarder's title ID, the same as its folder name. |
| `conceptId` | The five digits of the title ID, as a string. |
| `contentId` | `UP9000-<titleId>_00-PS5FORWARDER0000` |
| `localizedParameters` | `{"defaultLanguage": "en-US", "en-US": {"titleName": "<name>"}}`, with more languages if wanted. |
| `applicationCategoryType` | `0` |
| `applicationDrmType` | `"free"` |
| `contentVersion` | `"01.000.000"` |
| `masterVersion` | `"01.00"` |
| `gameIntent` | `{"permittedIntents": [{"intentType": "launchActivity"}]}` |

and the remaining keys of a homebrew app (`ageLevel`, `attribute`,
`attribute2`, `attribute3`, `contentBadgeType`, `downloadDataSize`,
`pubtools`, `requiredSystemSoftwareVersion`, `sdkVersion`, `versionFileUri`)
as `psfwd_param_json()` writes them. The `PS5FORWARDER0000` content ID suffix
is how tools recognise a forwarder from its metadata alone.

## 4. Art

All art is optional except the icon, and is given in the console's own
formats:

| File | Format |
|---|---|
| `icon0.png` | PNG, exactly 512 x 512 pixels. Required. |
| `pic0.dds` | DDS with a DX10 header, BC7 (mode 6 is enough), 1920 x 1080 or 3840 x 2160. Shown behind the focused tile. |
| `pic1.dds` | The same format. Shown while the target starts. |
| `snd0.at9` | RIFF WAVE, ATRAC9 at 48 kHz, at most about 87 seconds. Loops while the tile is focused. |

`pic0.dds` and `pic1.dds` SHOULD both be present or both absent; writers
usually use one picture for both. An editor that is not changing a picture
MUST keep the one already there.

The name, pictures and music are copied by the console when the tile is
registered: after changing them, the tile has to be registered again for the
home screen to show them. `forwarder.json` needs no such step.

## 5. Title IDs

- A title ID is `PPSA`, `CUSA` or `LAPY` followed by five digits.
- A **forwarder's** own title ID SHOULD be chosen from `PPSA99200` to
  `PPSA99899`, MUST NOT be used by another app on the console, and MUST NOT be
  one of the reserved IDs below.
- A forwarder's `target` MUST NOT be its own title ID.

Reserved IDs: `PPSA99100` (the template), and the apps in the
[target registry](registry/targets.json), which emulator authors extend by
pull request.

## 6. The forwarder program

`template/eboot.bin` is the program every forwarder runs. On start it:

1. reads `/app0/forwarder.json` (section 2) and its own title ID from
   `/app0/sce_sys/param.json`, and refuses a missing or invalid target;
2. makes sure a launcher is running (section 8);
3. sends the launch request (section 7) and reads the reply;
4. on success waits up to 20 seconds to be closed (the console closes it as
   the target starts), on failure shows a notification starting with
   `Forwarder: ` and exits.

Every line it logs MUST start with `[ps5-forwarder <TITLE_ID>]`: section 9
relies on that tag being in the program.

## 7. The launcher

An app may not start another app on the PS5, so a small resident payload does
it on the forwarders' behalf: [`launcher/launcher.c`](launcher/launcher.c),
shipped built as `template/launcher.elf`. It listens on **127.0.0.1:10199**
only, and serves one connection at a time.

A connection that is closed without sending anything is a probe; the
launcher answers nothing.

### Request: 0x1020 bytes, little endian

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | magic `0x4C574650` (`PFWL`) |
| 0x04 | 4 | protocol version, `1` |
| 0x08 | 16 | target title ID, NUL-padded |
| 0x18 | 4 | argument count, at most 64 |
| 0x1C | 4 | argument bytes used, NULs included, at most 4096 |
| 0x20 | 4096 | the arguments, each NUL-terminated, one after another |

### Reply: 0x18 bytes, little endian

| Offset | Size | Field |
|---|---|---|
| 0x00 | 4 | magic `PFWL` |
| 0x04 | 4 | protocol version, `1` |
| 0x08 | 4 | stage: `0` started, `1` bad request, `2` nobody signed in, `3` the system refused |
| 0x0C | 4 | return code of the first launch attempt |
| 0x10 | 4 | return code of the second launch attempt |
| 0x14 | 4 | the user the target was started for |

### Behaviour

The launcher validates the request (magic, version, title ID, sizes), finds
the foreground user, and calls `sceSystemServiceLaunchApp(target, argv,
param)` with a 32-byte parameter block holding the user: first with its size
field `0`, then, if that fails, with `32`.

When the caller is itself the running app (a forwarder always is), the system
answers `0x80940010` to both attempts, queues the launch, closes the caller
and then starts the target. Clients MUST treat stage `3` with both codes
`0x80940010` as success ("queued").

It logs to `/data/forwarder-manager/launcher.log`. If the port is already
taken it exits, leaving the running launcher in charge. ps5-app-launcher
speaks the same protocol and may serve forwarders instead.

## 8. Starting the launcher

A client that finds nothing on 127.0.0.1:10199 starts the launcher through
**elfldr**, the console's payload loader, on 127.0.0.1:9021: it connects,
sends the whole `launcher.elf`, closes its sending side, reads until elfldr
closes the connection, and then waits for the launcher port to answer, for up
to three seconds. The forwarder program carries `launcher.elf` inside it, so
forwarders work with nothing loaded but elfldr. Apps using `psfwd` call
`psfwd_ensure_launcher()`.

## 9. Upgrading

A tool MAY give forwarders the current forwarder program, so they gain new
behaviour. It MUST only replace `eboot.bin` (and MAY replace
`sce_module/libc.prx`) in folders that hold a `forwarder.json` and whose
`eboot.bin` contains one of the forwarder program tags,
`[ps5-forwarder %s]` or `[prospero-forwarder %s]` (older builds). Anything
else is not a forwarder program and MUST be left alone. The new file SHOULD be
written beside the old and renamed over it.

## 10. Emulators: arguments and the registry

What `args` means is up to the target. Emulators SHOULD accept:

- `--rom <path>`: the game to open, a full path or a name the emulator
  resolves in its own library folder;
- `--exit-after-game`: return to the home screen when the game ends.

An emulator that wants forwarders made for it adds itself to
[`registry/targets.json`](registry/targets.json) with its title ID, name,
what it emulates and the arguments it accepts. An emulator that creates
forwarders itself (an "add to home screen" action) SHOULD use `psfwd`, set
`creator`, and pass arguments it accepts back to itself.

## 11. Versions

The `format` key carries the version. Readers MUST ignore unknown keys, and a
reader for version 1 SHOULD still read a forwarder of a later version when it
has a `target`. Changes that would break readers will use a new version
number, with the program and launcher in `template/` kept able to serve
forwarders of earlier versions.
