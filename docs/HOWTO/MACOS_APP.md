# Build a single self-contained macOS app

One command turns this repo plus your own retail Zero Hour files into
`dist/Generals Zero Hour.app`: no Homebrew packages, no Vulkan SDK, no `run.sh`, no
`~/GeneralsX` folder. Drag it to `/Applications`.

## Requirements

- Apple Silicon Mac, macOS 15+
- Xcode Command Line Tools (`xcode-select --install`) - nothing else
- A retail Zero Hour folder (see [GETTING_THE_GAME_FILES.md](GETTING_THE_GAME_FILES.md)):
  it must contain the `*.big` archives and `ZH_Generals/`

## Build

```sh
./scripts/build/macos/make-app-zh.sh --game-data /path/to/GeneralsZH
```

`--game-data` defaults to `~/GeneralsX/GeneralsZH`. Other options: `--out DIR`
(default `dist/`), `--name NAME`, `--skip-build` (repackage without recompiling).

The first run downloads and builds the toolchain into `.toolchain/` (git-ignored) and
takes a while; later runs are incremental. Logs: `logs/make_app_*.log`.

## What lives where

| What | Where |
|---|---|
| Game, libraries, retail data | inside the `.app` (data is an APFS clone: no extra disk space on the same volume) |
| Saves, `Options.ini`, replays | `~/Library/Application Support/GeneralsX/GeneralsZH/` |
| Logs | `~/Library/Logs/GeneralsX/GeneralsXZH.log` (+ `.prev.log`) |
| Shader / font caches | `~/Library/Caches/GeneralsX/` |
| Build toolchain | `<repo>/.toolchain/` - `rm -rf` it to remove |

## First launch

The app is ad-hoc signed, not notarized. Right-click > Open the first time, or allow it
under System Settings > Privacy & Security.

## Notes

- The app embeds your retail game files: keep it to yourself.
- SagePatch extras are not included yet (they need `DYLD_INSERT_LIBRARIES`).
- The developer loop (`build-macos-zh.sh` + `deploy-macos-zh.sh` + `run.sh`) still works.
