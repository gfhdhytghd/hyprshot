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

When testing from the build tree before installation, set:

```ini
plugin {
    hyprshot {
        helper = /home/wilf/data/hyprshot/build-cmake/hyprshot-ui
    }
}
```

## Build

```sh
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Debug
cmake --build build-cmake
ctest --test-dir build-cmake --output-on-failure
```

Do not run `hyprpm update` from an active screenshot/overview state. Treat plugin reload as a live compositor unload/load.
