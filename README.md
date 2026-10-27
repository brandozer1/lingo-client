# Lingo client

The lingo client is a small program that runs lingos. It answers on its port, 6969, each lingo at its own route, so
any program on the computer can use them. A lingo on its route is an installed program. It comes bare, running its
lingos and nothing more until it is set up: signed in to Onsay, Onsay can reach its lingos too, and its lingos can ask
Onsay for anything with `lingo(...)`.

A lingo is a program written in Lingo, a small language made for AI to read and write ([`docs/lingo.md`](docs/lingo.md)),
or a native library for the computer, like `windows`. The lingo client is written in C and built with Zig for Windows
and Linux. It is Apache 2.0 licensed and made to be built on: add lingos, or wrap it in your own app.

## Layout

| Folder | What it is |
|--------|------------|
| `core/` | The lingo client: its settings, its command line, its messages with Onsay, and building and testing it. See its README |
| `lingos/` | The device lingos it ships: `windows` (Windows itself) and `native` (any Windows function, by its layout) |
| `lingo-library/` | The standard lingos, written in Lingo, for any computer: `text`, `request`, `agent` and more. See its README |
| `docs/` | `lingo.md`: the language, and what a lingo is |

## Download

Every release is on the [releases page](https://github.com/brandozer1/lingo-client/releases): the lingo client and its
lingos for Windows, the lingo client for Linux, and `install.sh`, which puts it on a Linux server and keeps it running:

```
curl -fsSL https://github.com/brandozer1/lingo-client/releases/latest/download/install.sh | sh
```

It runs bare. `| sh -s -- --network --sign-in` also answers other computers and signs it in to Onsay.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
