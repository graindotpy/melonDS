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

`GET /watch?ranges=ADDR:LEN[,ADDR:LEN...]` starts recording those ranges (up
to 1 KiB in total) on every frame. Whenever their bytes differ from the frame
before, the frame and the new bytes are added to a log of the last 4096
changes. A new watch replaces the old one and clears the log; `/watch` with no
ranges stops watching. It returns JSON with the watch's id and the `since`
value to pass to `/changes`:

```json
{"watch":1,"since":41,"ranges":3,"bytes":24}
```

`GET /changes?since=SEQ` returns the logged changes after `SEQ`, oldest first,
up to 256 at a time. `data` is the watched ranges' bytes back to back, in hex.
The first change after a new watch is the starting value. Pass the last `seq`
you got as the next `since`. `more` is true when there are more to fetch, and
`missed` is true when changes after `since` have already dropped out of the log.

```json
{"watch":1,"seq":43,"frame":5200,"missed":false,"more":false,
 "changes":[{"seq":42,"frame":5120,"data":"0b000000"},{"seq":43,"frame":5188,"data":"00000000"}]}
```

Watching lets a tool that polls a few times a second still see values that
only lasted a frame or two, such as a Pokémon's HP hitting zero just before
the next one is sent out on fast-forward.

`/status` reports `"api":2` when these two endpoints are available.

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
