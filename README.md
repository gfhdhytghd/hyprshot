# HyprCapture

HyprCapture is a Hyprland-only screenshot tool split into a compositor plugin and a Qt layer-shell helper. The plugin captures frozen compositor artifacts and launches the helper; the helper provides the selection overlay, output rendering, clipboard integration, and result thumbnail.

> [!IMPORTANT]
> `hyprpm` builds the compositor plugin and installs the helper to `~/.local/bin/hyprcapture-ui`. Set `plugin:hyprcapture:helper` only when you want to override that default helper path.

> [!WARNING]
> Hyprland plugins run inside the compositor process. Install plugins only from sources you trust.

> [!WARNING]
> This software is 99% vibe coded with OpenAI CodeX, but have been manual audited, warn in case you mind it.

## Features

- Fullscreen, region, and window capture modes
- Immediate frozen desktop image behind the overlay
- Display-resolution output for fullscreen and region captures
- Compositor-side window artifacts, including windows that are occluded or partly off-screen
- Window output backgrounds: follow system, white, black, real background, or transparent
- Optional window border and shadow removal
- Optional image watermarks from PNG, JPG/JPEG, SVG, or built-in presets
- Save-to-file and clipboard output
- Stable Wayland clipboard writes through `wl-copy` when available, with Qt clipboard fallback
- macOS-style result thumbnail with open, copy, show in folder, delete, and close actions
- Thumbnail swipe gestures: swipe right to close, swipe down to delete and restore the previous clipboard snapshot

<img width="1464" height="834" alt="image" src="https://github.com/user-attachments/assets/8aa9b11a-e98e-4057-bdf5-dc3bfb6cf8b6" />


## Installation

### Install with `hyprpm`

Use `hyprpm` for the compositor plugin:

```sh
hyprpm update
hyprpm add https://github.com/gfhdhytghd/HyprCapture
hyprpm enable hyprcapture
hyprpm reload
```

If you use Hyprland's permission system, allow `hyprpm` in your config:

```conf
permission = /usr/(bin|local/bin)/hyprpm, plugin, allow
```

Do not also manually `hyprctl plugin load` the same `.so` if you manage it through `hyprpm`.

### Helper install path

The `hyprpm` manifest installs the helper automatically:

```toml
build = [
    "cmake -S . -B build-hyprpm -DCMAKE_BUILD_TYPE=Release",
    "cmake --build build-hyprpm --target hyprcapture hyprcapture-ui",
    "install -Dm755 build-hyprpm/hyprcapture-ui \"$HOME/.local/bin/hyprcapture-ui\""
]
```

The plugin default helper lookup is:

1. `HYPRCAPTURE_HELPER`
2. `$HOME/.local/bin/hyprcapture-ui`
3. `hyprcapture-ui` from `PATH`

Only configure `plugin:hyprcapture:helper` if you want to use a custom helper path:

```conf
plugin {
    hyprcapture {
        helper = /home/you/.local/bin/hyprcapture-ui
    }
}
```

### Manual helper install

Requirements:

- Hyprland development headers for the exact Hyprland build you are running
- `cmake`
- `pkg-config`
- a C++23-capable compiler
- Qt 6 Core, Gui, and Widgets
- LayerShellQt
- `wl-clipboard` for persistent Wayland clipboard ownership

Build and install the helper:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release
cmake --build build-release --target hyprcapture-ui
install -Dm755 build-release/hyprcapture-ui "$HOME/.local/bin/hyprcapture-ui"
```

For development without installing, point `helper` at the build-tree executable or launch Hyprland with:

```sh
HYPRCAPTURE_HELPER=/path/to/hyprcapture-ui Hyprland
```

### Manual build and reload

For local development, the plugin output is `build-cmake/libhyprcapture.so`.

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

Safe reload shape:

```sh
hyprctl plugin unload "$(pwd)/build-cmake/libhyprcapture.so"
hyprctl plugin unload "$(pwd)/build-hyprpm/libhyprcapture.so"
hyprctl plugin load "$(pwd)/build-cmake/libhyprcapture.so"
hyprctl plugin list
```

`plugin not loaded` is expected when the unloaded path is not the active copy.

Build outputs:

- Plugin: `build-cmake/libhyprcapture.so`
- Helper: `build-cmake/hyprcapture-ui`
- Config test: `build-cmake/hyprcapture-config-test`

## Usage

### Dispatchers

```conf
bind = SUPER SHIFT, S, hyprcapture:open
bind = SUPER SHIFT, W, hyprcapture:open,window
bind = SUPER SHIFT, F, hyprcapture:quick,fullscreen
```

| Dispatcher | Description |
| --- | --- |
| `hyprcapture:open` | Open the overlay using `default_mode`. |
| `hyprcapture:open,<mode>` | Open the overlay in `region`, `fullscreen`, or `window` mode. |
| `hyprcapture:quick` | Capture immediately using `default_mode`. |
| `hyprcapture:quick,<mode>` | Capture immediately in `region`, `fullscreen`, or `window` mode. |
| `hyprcapture:cancel` | Reserved dispatcher; currently returns success without changing an active helper. |

### Overlay

- Region mode: drag a rectangle, then release or press Enter.
- Fullscreen mode: captures according to `fullscreen_scope`.
- Window mode: hover a window and press Enter or click it.
- Fushion mode: the toolbar keeps the fullscreen action and configuration controls; drag anywhere to capture a region, or single-click a window to capture that window.
- Esc cancels the helper.
- The toolbar is anchored near the bottom of the screen and only shows controls relevant to the active mode.

### Thumbnail

The thumbnail appears after capture when `show_thumbnail = 1`.

- Left click opens the saved image.
- Drag starts a file drag for targets that accept image files.
- Right click opens the thumbnail menu.
- Swipe right with a touchpad or two-dimensional wheel to close.
- Swipe down to delete the file and restore the clipboard snapshot captured before the screenshot.

## Configuration

All user-facing settings live under `plugin:hyprcapture`.

Example:

```conf
plugin {
    hyprcapture {
        default_mode = region
        fullscreen_scope = all
        window_background = follow-system
        window_border = keep
        window_shadow = keep
        save = 1
        clipboard = 1
        show_thumbnail = 1
        fushion_mode = 0
        save_dir = ~/Pictures/Screenshots
        filename_template = Screenshot-%Y-%m-%d-%H%M%S.png
        include_cursor = 0
        thumbnail_timeout_ms = 5000
        watermark =
        watermark_position = central
        watermark_width = 20%
        watermark_offset = 0 0
    }
}
```

### Capture options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `default_mode` | string | `region` | Default mode for `hyprcapture:open` and `hyprcapture:quick`. Supports `region`, `fullscreen`, and `window`. |
| `fullscreen_scope` | string | `all` | Fullscreen capture scope. Supports `all`, `current`, and `per-monitor`. |
| `window_background` | string | `follow-system` | Background behind transparent window pixels. Supports `follow-system`, `white`, `black`, `real`, and `transparent`. |
| `window_border` | string | `keep` | Window border policy. Supports `keep` and `remove`. |
| `window_shadow` | string | `keep` | Window shadow policy. Supports `keep` and `remove`. |
| `include_cursor` | bool | `0` | Parsed and forwarded by the plugin/helper; cursor compositing is not currently rendered into the output. |
| `fushion_mode` | bool | `0` | Fuse region and window interactions in one overlay: drag to capture a region, or single-click a window to capture that window. The toolbar keeps the fullscreen action and configuration controls; fullscreen multi-monitor scope is shown only when multiple monitors are present. |

### Output options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `save` | bool | `1` | Save the output image to `save_dir`. |
| `clipboard` | bool | `1` | Copy the output image to the clipboard. Uses `wl-copy` when available so the clipboard survives helper exit. |
| `show_thumbnail` | bool | `1` | Show the result thumbnail after capture. |
| `save_dir` | string | `~/Pictures/Screenshots` | Output directory. `~` is expanded against `HOME`. |
| `filename_template` | string | `Screenshot-%Y-%m-%d-%H%M%S.png` | `strftime` template for saved screenshot filenames. |
| `thumbnail_timeout_ms` | int | `5000` | Thumbnail auto-close timeout in milliseconds. Use `0` to keep it open until user action. |
| `helper` | string | empty | Optional helper override. By default the plugin tries `HYPRCAPTURE_HELPER`, then `$HOME/.local/bin/hyprcapture-ui`, then `hyprcapture-ui` from `PATH`. |

### Watermark options

| Option | Type | Default | Description |
| --- | --- | --- | --- |
| `watermark` | string | empty | Disabled when empty. Set to a PNG, JPG/JPEG, or SVG path, or use built-in `activate-linux` / `hypercam2`. Transparency is preserved for PNG/SVG. |
| `watermark_position` | string | `central` | Supports `up-left`, `up-middle`, `up-right`, `left-middle`, `central`, `right-middle`, `down-left`, `down-middle`, and `down-right`. Common aliases like `center`, `top-center`, and `right-meddle` are accepted. |
| `watermark_width` | string | `20%` | Watermark width. Use pixels like `320` / `320px`, or screenshot-width percent like `18%`. |
| `watermark_offset` | string | `0 0` | X/Y offset from the selected position. Vec2-like values such as `12 -8`, `2% -4%`, or `12px, -8px` are accepted. Percent X is relative to screenshot width; percent Y is relative to screenshot height. |

## Development

Useful commands:

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake --target hyprcapture hyprcapture-ui hyprcapture-config-test
ctest --test-dir build-cmake --output-on-failure
./build-cmake/hyprcapture-ui --help
```

Temporary compositor artifacts and thumbnail/clipboard scratch files are written under `/dev/shm/hyprcapture` when available, with `/tmp/hyprcapture` as fallback. They are intentionally not placed under `/run/user/$UID`.

## Notes

- The repository includes a root [`hyprpm.toml`](hyprpm.toml) manifest, which is expected by `hyprpm`.
- `window_background = real` uses compositor-captured real background data when available and falls back to reconstructing from the frozen desktop snapshot.
- Do not run `hyprpm update` or reload the plugin while an active screenshot overlay is being used.
