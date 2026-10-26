# Lingo Client

A lightweight, very portable web server that runs lingos. It signs in or listens, and when a call comes to one of its
routes it runs the lingo there and sends the answer back. Think of it as a small operating system of its own, with its
own programs: a route is a program, and lingos are programs.

The client is the engine in [`core/`](core/), written in C and built with Zig down to native code for Windows and
Linux. It ships two sets of lingos: [`lingos/`](lingos/), the device toolsets (`windows` for Windows itself, `native`
to call any OS or library function by its layout), and [`lingo-library/`](lingo-library/), the standard lingos written
in Lingo (`text`, `list`, `math`, `time` and more). The language itself is in [`docs/lingo.md`](docs/lingo.md).

This is the open base that the Onsay products are built on, and that anyone can build their own product on. It is Apache
2.0 licensed and made to be extended: add lingos, open routes, wrap it in your own app. Onsay Desktop is one such wrapper
(an agent), and Onsay IO is another (a face). The lingo function, the INS and the index live in the onsay repo and plug
in on top; they are not part of the client.

## Layout

| Folder | What it is |
|--------|------------|
| `core/` | The engine: compiles and runs Lingo, signs in, hosts routes, keeps a catalog. See its README |
| `lingos/` | The lingos the client ships: `windows` (Windows itself) and `native` (call any OS or library function by layout) |
| `lingo-library/` | The standard lingos, written in Lingo, for any computer: `text`, `list`, `math`, `time` and more |
| `docs/` | `lingo.md`, the language and what a lingo is |

## Build and test

Build the core and its lingos with [`core/build.ps1`](core/build.ps1) (it needs Zig, one download, no installer), then
run the tests as [`core/README.md`](core/README.md) describes. Everything builds from this one repository, with nothing
else checked out beside it.

## License

Apache License 2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
