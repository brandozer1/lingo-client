# Lingo client

The lingo client is a small program that runs lingos. It answers on its port, 6969, each running lingo at its own
route, so any program on the computer can use them, and anyone else the computer's policy lets in. A lingo on its route
is an installed program, and like a program it may do only what the computer allows: files in the lingo client's own folder freely,
and files elsewhere, commands and the screen when the person says yes. It comes bare, running its lingos and nothing
more until it is set up: signed in to Onsay, Onsay can reach its lingos too, and its lingos can ask Onsay for anything
with `lingo(...)`.

A lingo is a `.lingo` file: its program, written in Lingo, a small language made for AI to read and write, or a native
library for the computer, like `windows`, and its `openapi.json`, which says what each of its routes takes and needs
([`docs/lingo.md`](docs/lingo.md)). The lingo client is written in C and built with Zig for Windows and Linux. It is
Apache 2.0 licensed and made to be built on: add lingos, or wrap it in your own app.

## Layout

| Folder | What it is |
|--------|------------|
| `core/` | The lingo client: its settings, its policy, its command line, its messages with Onsay, and building and testing it. See its README |
| `lingos/` | The device lingos it ships, in C: `windows` (Windows itself) and `native` (any Windows function, by its layout) |
| `lingo-library/` | The standard lingos, written in Lingo, for any computer: `text`, `request`, `agent` and more. See its README |
| `docs/` | `lingo.md`: the language, and what a lingo and its `.lingo` file are |

## Download

Every release is on the [releases page](https://github.com/brandozer1/lingo-client/releases): the lingo client and its
lingos for Windows, the lingo client for Linux, and `install.sh`, which puts it on a Linux server and keeps it running:

```
curl -fsSL https://github.com/brandozer1/lingo-client/releases/latest/download/install.sh | sh
```

It runs bare. `| sh -s -- --sign-in` also signs it in to Onsay.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
