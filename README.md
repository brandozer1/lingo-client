# Lingo Client

A small, very portable program that runs lingos on a computer, so the lingos there can be used by you and by Onsay,
wherever you are.

The client is the engine in [`core/`](core/), written in C and built with Zig down to native code for Windows and
Linux. It ships two sets of lingos: [`lingos/`](lingos/), the device toolsets (`windows` for Windows itself, `native`
to call any OS or library function by its layout), and [`lingo-library/`](lingo-library/), the standard lingos written
in Lingo (`text`, `list`, `math`, `time` and more). The language itself is in [`docs/lingo.md`](docs/lingo.md).

It is Apache 2.0 licensed and made to be extended: add lingos, or wrap it in your own app.

## Layout

| Folder | What it is |
|--------|------------|
| `core/` | The engine: compiles and runs Lingo, signs in, and tells Onsay which lingos are here. See its README |
| `lingos/` | The lingos the client ships: `windows` (Windows itself) and `native` (call any OS or library function by layout) |
| `lingo-library/` | The standard lingos, written in Lingo, for any computer: `text`, `list`, `math`, `time` and more |
| `docs/` | `lingo.md`, the language and what a lingo is |

## Build and test

Build the core and its lingos with [`core/build.ps1`](core/build.ps1) (it needs Zig, one download, no installer), then
run the tests as [`core/README.md`](core/README.md) describes. Everything builds from this one repository, with nothing
else checked out beside it.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
