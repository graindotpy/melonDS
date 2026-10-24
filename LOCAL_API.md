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
changes. A watch with different ranges replaces the old one and clears the log, and
the first change after it is the starting value. Asking for the same ranges
again keeps the log for anyone already reading it and returns a `since` of the
newest change, so the new reader starts from now. `/watch` with no ranges stops watching. It returns JSON with the watch's id and the `since`
value to pass to `/changes`:

```json
{"watch":1,"since":41,"ranges":3,"bytes":24}
```

`GET /changes?since=SEQ` returns the logged changes after `SEQ`, oldest first,
up to 256 at a time. `data` is the watched ranges' bytes back to back, in hex.
Pass `next` (the
last `seq` returned, or your `since` when nothing was) as the next `since`;
`seq` at the top level is the newest change in the whole log. `more` is true
when there are more to fetch, and `missed` is true when changes after `since`
have already dropped out of the log. Turning the server off drops the watch.

```json
{"watch":1,"seq":43,"next":43,"frame":5200,"missed":false,"more":false,
 "changes":[{"seq":42,"frame":5120,"data":"0b000000"},{"seq":43,"frame":5188,"data":"00000000"}]}
```

Watching lets a tool that polls a few times a second still see values that
only lasted a frame or two, such as a Pokémon's HP hitting zero just before
the next one is sent out on fast-forward.

`/status` reports `"api":2` when these two endpoints are available.

`GET /perf` returns how well emulation kept up over the last ten minutes, one
entry per second, newest last, along with the settings that affect speed.
`/status` reports `"api":3` when it's available.

```json
{"settings":{"jit":true,"renderer":"software","threaded3D":true,"limitFPS":true,
  "audioSync":false,"overclock":false,"console":"ds"},
 "seconds":[{"time":"2026-10-24T16:50:17Z","frame":487,"fps":60,"game":50,
  "frames3D":50,"cpu":99.8,"cpuMax":100,"gxStall":0,"dma":0.2,"emuMs":3.7,"emuMaxMs":6.4,"drawMs":0.4,
  "slowestMs":19.7,"fastForward":0,"overclock":false}]}
```

| Field | Meaning |
| --- | --- |
| `fps` | Frames emulated that second; 60 is full speed |
| `game` | % of those frames where the game read the buttons, which it does once per update |
| `frames3D` | % of frames where the game finished a 3D frame |
| `cpu`, `cpuMax` | How busy the DS's main CPU (ARM9) was running code, on average and in its busiest frame |
| `gxStall`, `dma` | % of the ARM9's time spent waiting on the 3D hardware or on DMA instead |
| `emuMs`, `emuMaxMs` | PC time to emulate a frame, on average and at most; 16.7 is a whole frame at 60fps |
| `drawMs` | PC time to draw a frame |
| `slowestMs` | Longest gap between two frames, waiting included |
| `fastForward` | % of frames run on fast-forward |

`fps` under 60 with high `emuMs` means the PC can't keep up. `fps` at 60 with
`game` at 50 and `cpu` near 100 means the game itself is slowing down: the DS's
CPU can't finish an update in one frame, so the game updates every other frame,
as it would on a real DS. High `gxStall` instead means the game is waiting on
the 3D hardware, which the overclock below doesn't help with. The window title
shows `game` and `cpu` for the last second.

**Config → Overclock DS CPU** runs the DS's ARM9 at 133MHz, the DSi's speed,
instead of 67MHz, which removes that kind of slowdown. It's saved as
`DSOverclock` under `[Emu]`. Savestates load whichever way it's set, but one
saved with it on won't load properly in a melonDS build without it.

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
