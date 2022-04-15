# wmenu - dmenu for Wayland

An efficient dynamic menu for supported Wayland compositors (requires
`wlr_layer_shell_v1` support).

## Installation

Dependencies:

- cairo
- pango
- wayland
- xkbcommon
- scdoc (optional)

```
$ meson build
$ ninja -C build
# ninja -C build install
```

## Usage

See wmenu(1)

## Contributing

Send patches and questions to [~adnano/wmenu-devel](https://lists.sr.ht/~adnano/wmenu-devel).

Subscribe to release announcements on [~adnano/wmenu-announce](https://lists.sr.ht/~adnano/wmenu-announce).

## Credits

This project started as a fork of [dmenu-wl](https://github.com/nyyManni/dmenu-wayland).
However, most of the code was rewritten from scratch.
