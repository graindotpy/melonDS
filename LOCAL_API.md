# Local data server

This fork can serve read-only access to the emulated DS's main RAM over HTTP
on `127.0.0.1`, so tools running on the same computer (such as a Nuzlocke
tracker in a browser tab) can follow what is happening in the game.

Turn it on with **Config → Local data server**. The setting is saved and the
server starts with melonDS from then on. It only listens on the loopback
address, so other machines on the network can't reach it.

## Endpoints

`GET /status` returns JSON describing what is running:

```json
{"api":1,"instance":0,"active":true,"running":true,"frame":5120,
 "game":{"code":"CPUE","title":"POKEMON PL","version":0}}
```

`active` is true once a game (or the firmware) has booted, and stays true while
paused. `running` is false while paused. `game` is null when no cart is
inserted.

`GET /read?ranges=ADDR:LEN[,ADDR:LEN...]` returns the bytes of each range back
to back as `application/octet-stream`. `ADDR` is hex (`0x` optional) and must
fall inside main RAM, `0x02000000` to `0x02FFFFFF`. `LEN` is decimal, or hex
with a `0x` prefix. A request can ask for up to 64 ranges and 64 KiB in total.
It returns 503 when nothing is running.

Every response carries an `X-Melonds-Frame` header with the frame the data was
read on. Reads happen between frames on the emulation thread, so all the ranges
in one request come from the same frame.

```js
const res = await fetch('http://127.0.0.1:31124/read?ranges=02101D2C:4')
const base = new DataView(await res.arrayBuffer()).getUint32(0, true)
```

## Settings

These live in `melonDS.toml` under `[LocalAPI]`:

| Key | Default | Meaning |
| --- | --- | --- |
| `Enabled` | `false` | Start the server |
| `Port` | `31124` | Port for the first instance; instance *n* uses `Port + n` |
| `AllowedOrigins` | `"*"` | Comma-separated web origins allowed to read, or `*` for any |

Requests whose `Host` header isn't `127.0.0.1`, `localhost` or `[::1]` are
refused, which stops websites from reaching the server through DNS rebinding.
To limit which sites can read memory, set `AllowedOrigins`, for example
`"http://localhost:5173,https://nuzlocke.app"`.
