# DLSS 5 DX11 Bridge

A ReShade add-on that lets a DLSS 5 Neural Rendering add-on — which only hooks
DirectX 12 — run inside a game that renders with DirectX 11.

Tested on Baldur's Gate 3 (DX11 build), with DLAA and with every DLSS quality
preset. Nothing here is specific to that game.

## What it does

A DLSS 5 add-on works by detouring `NVSDK_NGX_D3D12_CreateFeature` and
`NVSDK_NGX_D3D12_EvaluateFeature` and inserting its neural-rendering pass into
them. A D3D11 game never calls those functions, so the add-on sits idle forever
showing "waiting for game DLSS".

This bridge intercepts the game's own `NVSDK_NGX_D3D11_EvaluateFeature_C`,
forwards it untouched, and then reproduces the same DLSS contract on a second
NGX session running on its own D3D12 device. That D3D12 evaluate is a genuine
NGX call, so the DLSS 5 add-on detours it and does its work. The result is
copied back into the game's own output texture.

The DLSS 5 add-on is not modified or patched in any way. It simply starts
receiving the calls it was always waiting for.

Per frame:

1. copy the game's Color and MotionVectors into shared textures
2. convert the game's depth into a shared `R32_FLOAT` texture with a compute
   shader — `CopyResource` cannot, the formats are in different typeless
   families. Which view format is legal depends on the game's depth format, so
   it is read from the texture rather than assumed
3. signal a fence shared between the D3D11 and D3D12 queues
4. run the D3D12 evaluate, which is where the DLSS 5 add-on inserts itself
5. signal back, and copy the result into the game's output

Every size, offset and scalar is read from the game's own NGX parameter block
and forwarded verbatim, so upscaling presets work as well as DLAA.

## Requirements

In the game folder, alongside the game executable:

| File | Where from |
| --- | --- |
| `dxgi.dll` — ReShade 6.8+ **with add-on support** | reshade.me, full version |
| a DLSS 5 Neural Rendering ReShade add-on | its own author |
| `nvngx_dlssnr.dll` | shipped with that add-on |
| `dlss5-dx11-bridge.addon64` | this package |

The DLSS 5 add-on's own neural-rendering toggle has to be enabled, either in
its ReShade overlay panel or in `ReShade.ini`.

The bridge itself needs a D3D11 game with native DLSS, a GPU and driver that
support D3D12, and `ID3D11Device5` for cross-API shared fences.

## Install

Drop `dlss5-dx11-bridge.addon64` next to ReShade. On first run it writes
`dlss5-dx11-bridge.cfg` with working defaults; nothing needs configuring.

To remove it, delete the file.

Nothing on disk is patched. The only writes to foreign code are 14 bytes at
three function entry points, in memory, restored around every call.

## Configuration

`dlss5-dx11-bridge.cfg` is re-read while the game runs, so values can be
changed without restarting. Changes that only take effect on a new NGX feature
trigger a rebuild automatically.

| Key | Default | Meaning |
| --- | --- | --- |
| `stage` | 3 | How much of the bridge runs. `0` fully inert, `1` the input copies only, `2` also the depth conversion, `3` everything. Useful for isolating a problem: if `stage=0` still misbehaves, the bridge is not the cause. |
| `mode` | 2 | `0` never writes to the game, `1` transport only with no DLSS, `2` the full path. |
| `skip_game` | 1 | Do not forward the game's own DLSS evaluate. Its result is overwritten anyway, so running it is wasted work. Suppressed only while the bridge is healthy and already delivering. |
| `flags` | 107 | `DLSS.Feature.Create.Flags` for the bridge's feature. |
| `subrects` | 1 | Fallback for `DLSS.Enable.Output.Subrects`, used only when the game does not set one of its own. |
| `reset_every` | 0 | `1` forces the NGX Reset flag every frame, discarding temporal history. Diagnostic only. |
| `pixels` | 0 | `1` reads pixels back to the CPU for debugging. Stalls the GPU hard. |

## Log

`dlss5-dx11-bridge.log` records the contract read from the game, which
resource-sharing direction the driver accepted, the result of every NGX call,
and a timing line every 600 frames:

```
[bridge] 600 frames: bridge CPU 7.96 ms/frame | frame interval 10.67 ms (93.7 fps) | bridge is 74% of the frame
```

The CPU figure is mostly time spent waiting for the GPU, not work. Read it
next to the frame interval rather than on its own.

## Performance

Measured on one machine, same scene, camera still:

| | Frame interval | fps |
| --- | --- | --- |
| bridge inert | 8.33 ms | 120.0 (capped) |
| plumbing only, no evaluate | 8.33 ms | 120.0 (capped) |
| full bridge, game's DLSS suppressed | 10.66 ms | 93.7 |
| full bridge, both DLSS passes running | 12.22 ms | 81.9 |

The copies, the depth conversion and both cross-API fences cost nothing
measurable — with the evaluate disabled the frame time is unchanged. The whole
cost is the DLSS and neural-rendering work itself.

`skip_game=1` is worth about 1.5 ms per frame.

## Related

[dlss5-d3d12-fix](https://github.com/NIGos/dlss5-d3d12-fix) fixes a different
failure of the same add-on: a DirectX 12 game whose DLSS output carries a mip
chain, which that add-on requires to be single-mip and silently refuses. If the
panel says STANDBY/FAILED rather than waiting for the game's DLSS, that is the
one to use.

## Building

Windows SDK and MSVC. No external dependencies; the ReShade add-on API is
reached through `GetProcAddress` and the NGX interfaces are declared inline.

From the `src` folder. `bridge.h` and `bridge.inc` are pulled in by the `.cpp`
and are not compiled separately.

```
rc /nologo version.rc
cl /nologo /LD /EHsc /O2 /MT dlss5-dx11-bridge.cpp ^
   /link /OUT:dlss5-dx11-bridge.addon64 version.res kernel32.lib user32.lib
```

The version lives in two places that have to stay in step: `BRIDGE_VERSION` in
the `.cpp`, and the numbers in `version.rc`. The first is what the log prints,
the second is what ReShade's overlay shows.

## Reporting a problem

Post `dlss5-dx11-bridge.log`. It is written to answer the usual questions
without a conversation:

- the exact build, with its compile date
- the host executable and Windows version
- **which of the required files are actually present next to the add-on** —
  the most common cause of "it does nothing" is a missing `renodx-dlss5.addon64`
  or `nvngx_dlssnr.dll`
- every other ReShade add-on in the folder, so conflicts are visible
- the GPU and driver
- the NGX capabilities this GPU will agree to. `SuperSamplingDenoising.Available`
  is reported among them, but it describes Ray Reconstruction rather than
  neural rendering, so a `0` there does not by itself mean the feature is
  unavailable
- **every module exporting the NGX D3D11 API, and which of them were hooked** —
  one line per layer, with the entry-point addresses
- if none were found, every loaded module exposing NGX or Streamline
- if they were hooked but nobody called them within 60 seconds, an explicit
  note saying so — that is a different problem from failing to hook, and the
  log distinguishes them
- whether `sl.interposer.dll` is in the process, because DLSS driven through
  Streamline never reaches the functions this add-on hooks

## Confirmed working

Reported by users, on five unrelated engines:

- **Baldur's Gate 3** (Divinity 4.0) — tested in depth here, native DLSS, DLAA
  and every quality preset
- **Fallout 4** (Creation) — DLSS supplied by a third-party injector rather
  than by the game
- **7 Days to Die** (Unity)
- **The Legend of Heroes: Trails beyond the Horizon** (Falcom) — needed both
  fixes in 1.0.4 and 1.0.5, and is the reason they exist
- **S.T.A.L.K.E.R. Anomaly** (X-Ray)

Fallout 4 matters for a second reason: it shows the bridge picks up DLSS that
another mod provides, not only DLSS built into the game.

Nothing here targets a particular game. Every module exporting the NGX D3D11
API is hooked, and every size, format and offset is read from the parameter
block the caller passes. Where it has failed so far
it has been because something was hardcoded from the one game it was written
against — see 1.0.4 — so reports from new titles are useful even when they work.

## Known limits

- The game's DLSS runs once and the bridge's runs once; with `skip_game=1` only
  the bridge's does. There is no path that avoids a second NGX session.
- Only tested on one game and one GPU.
- Resolution changes and DLSS preset changes are handled by rebuilding, but
  alt-tab and exclusive-fullscreen transitions are not specifically handled.
- Verbose logging is always on.

If anything goes wrong the bridge disables itself and the game renders on its
own; it never leaves a broken frame on screen deliberately.
