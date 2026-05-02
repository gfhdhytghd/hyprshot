# hyprshot

Hyprland-only screenshot tool, split into a compositor plugin and a Qt helper.

## Current MVP

- `hyprshot` plugin registers dispatchers and configuration defaults.
- `hyprshot-ui` provides the fullscreen overlay, mode bar, region selection, and macOS-style result thumbnail.
- The compositor-side frozen pixel artifact path is represented by the shared session protocol and is the next implementation layer.

## Hyprland config

```ini
plugin {
    hyprshot {
        default_mode = region
        fullscreen_scope = all
        region_scope = global
        window_background = follow-system
        window_border = keep
        window_shadow = keep
        save = 1
        clipboard = 1
        show_thumbnail = 1
        save_dir = ~/Pictures/Screenshots
        filename_template = Screenshot-%Y-%m-%d-%H%M%S.png
        include_cursor = 0
        thumbnail_timeout_ms = 5000
        helper = hyprshot-ui
    }
}

bind = SUPER SHIFT, S, hyprshot:open
```

## Install the helper

`hyprpm` installs the compositor plugin `.so`, but it does not install the companion Qt helper. Build and install the helper into a directory visible from the Hyprland session:

```sh
cmake -S . -B build-release -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-release --target hyprshot-ui
cmake --install build-release --prefix "$HOME/.local"
```

Make sure `~/.local/bin` is in the environment that launches Hyprland. If not, use an absolute helper path:

```ini
plugin {
    hyprshot {
        helper = /home/you/.local/bin/hyprshot-ui
    }
}
```

For development without installing, point `helper` at the build-tree executable or launch Hyprland with `HYPRSHOT_HELPER=/path/to/hyprshot-ui`.

## Build

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

Do not run `hyprpm update` from an active screenshot/overview state. Treat plugin reload as a live compositor unload/load.
