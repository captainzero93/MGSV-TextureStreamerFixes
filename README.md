# TextureStreamer

Infinite Heaven native plugin that fixes MGSV's texture streaming budget and the small texture pool stall.
VRAM is read from the game's own swapchain.

Target: MGSV TPP retail 1.0.15.4 EN, `mgsvtpp.exe` PE header TimeDateStamp `0x6A4CB898`
(unmodified exe SHA256 `085c2f82d1c963c40b3d2d55786661dfee2b18cbbf388a710c00fa76c5e9bb45`)
(`version_info.txt`: `tpp_steam_mst_en_day3900mgo_patch_0707_1632`). On any other exe it logs a warning and applies only the hooks and patches whose original bytes match.

Current revision: V016 (`TEXTURESTREAMER,V0_16_20260924`). Requires Infinite Heaven. Does not need IHHook.

Every address below is a retail 1.0.15.4 EN address (preferred base `0x140000000`), read in Ghidra
(program `mgsvtpp.exe`) unless another source is given. Names in quotes come from the old mgsv_mod source
or the 2015 dev decomp. Retail functions are unnamed (`FUN_`).

## Contents

1. What the mod does
2. Install and build
3. Settings
4. Load chain
5. Safety rules in the code
6. What it attaches to
7. Byte patches
8. The small storage pool and the fallback
9. Not patched, and why
10. Address reference
11. Logs
12. Test results
13. Revision history
14. Credits

## 1. What the mod does

MGSV sizes its texture streaming budget from a VRAM estimate. It then clamps that to 3584 MB and to whatever
texture budget the game has requested (1800 MB on an RTX 4070), and forces a low-quality "degrade" mode below
800 MB. It also queues at most 16 texture upgrades per frame. Separately, the lowest mip of every streamed
texture lives in a "small" storage pool fixed at 262.5 MB when the game starts. With a texture-heavy file that
pool fills, and the streamer stops until space frees: a halt of several seconds while driving and an iDroid
that loads forever. Vanilla does the same.

The mod:

1. reads the real VRAM of the adapter the game renders on,
2. asks the streamer to reconfigure to that budget, using the game's own reconfigure path,
3. removes the 3584 MB cap and the two forced-degrade conditions,
4. raises the upgrades-per-frame limit from 16 to `upgradePerFrame` (default 32),
5. retries a failed small pool allocation in the large pool instead of stalling.

On a 12 GB RTX 4070 in Afghanistan: requested budget 1800 -> 4095 MB, texture cache 1387 -> 3492 MB,
streaming storage 436 -> 963 MB, large pool 586 -> 1304 MB. 4095 MB is the ceiling because the game stores
these values as 32-bit (section 9.3).

It does not touch the 4880 handle cap or the create-time sizing. Section 9 explains why.

## 2. Install and build

**Install:** with the game closed, copy these into the game root (the folder with `mgsvtpp.exe`):

- `mod\modules\TextureStreamer_Core.lua`
- `plugins\TextureStreamer.dll`
- `plugins\TextureStreamer.lua` (settings, restart the game after editing)

**Build:** run `BUILD_TEXTURESTREAMER_V016_CHECKED.cmd`. It calls `Build-TextureStreamer-V016.ps1`, which:

- checks the source layout and that the marker `TEXTURESTREAMER,V0_16_20260924` is in `dllmain.cpp`,
- finds MSBuild with `vswhere` and does a clean Release x64 rebuild of `TextureStreamer.sln`,
- checks the marker is in the built DLL,
- stages the release layout in `verified-v016\` (never overwrites an existing output folder) and prints the DLL's SHA256.

MinHook is vendored in `minhook\` (TsudaKageyu/minhook commit `8af6b4a`), so no prebuilt `libMinHook` is needed.
Code style: `clang-format` config from MGSV_HookSample (Allman braces, column limit 160).

## 3. Settings

`plugins\TextureStreamer.lua`. The DLL reads it as literal `key = value` lines and never runs it.
Restart the game after editing.

| Key | Default | Effect |
|---|---|---|
| `debugWindow` | `false` | Live debug console. Use windowed or borderless, it steals focus from exclusive fullscreen. |
| `debugLog` | `false` | Pool usage every 5 s, stall lines, fallback lines and `TextureStreamer_boot.log`. For bug reports. |
| `raiseBudget` | `true` | `false` passes the game's own values through (monitor only, for comparing against vanilla). |
| `patchDispatch` | `true` | Remove the 3584 MB cap and the forced degrade in the streamer update (7.1). |
| `patchUpgrade` | `true` | Raise upgrades per frame to `upgradePerFrame` (7.2). |
| `smallPoolFallback` | `true` | Small pool full: place the block in the large pool instead of stalling (section 8). |
| `lockVramMax` | `false` | Report 4095 MB (uint32 max) instead of the adapter's VRAM. |
| `upgradePerFrame` | `32` | 16 to 127, game default 16. Clamped to that range. |
| `hookPresent`, `hookUpdate`, `hookGetAvail`, `hookRequestConfig` | `true` | Per-hook switches for isolating a crash. `hookUpdate` also needs `hookRequestConfig`. |

## 4. Load chain

1. IH loads `mod\modules\TextureStreamer_Core.lua` as an external module.
2. The Core module calls `package.loadlib("<game>\plugins\TextureStreamer.dll", "luaopen_TextureStreamer")` inside `pcall`, then calls the result.
   It logs to `plugins\TextureStreamer_loader.log` and `ih_log.txt`.
3. `DllMain` pins the DLL (`GET_MODULE_HANDLE_EX_FLAG_PIN`) so it is never unloaded: in V008 Lua freed
   the loadlib handle at shutdown, which would unmap the hook code while game threads can still run it.
   It then starts `InitThread`. `luaopen_TextureStreamer` does nothing except exist for loadlib.
4. `InitThread`:
   - opens `TextureStreamer.log` beside `mgsvtpp.exe` (the previous run is kept as `TextureStreamer.prev.log`),
   - reads `plugins\TextureStreamer.lua`,
   - reads `FileHeader.TimeDateStamp` from the loaded exe's PE header and logs it with `SizeOfImage`. The linker sets TimeDateStamp per build, so an official update changes it, while hex edits to code or header flags (such as Large Address Aware) don't. SizeOfImage is logged only: the anti-tamper wrapper (Denuvo) bloats it, so it isn't a reliable build id. A TimeDateStamp other than `0x6A4CB898` logs a warning, and the 1.0.15.4 EN address set is still tried. The per-site byte checks (section 5) decide what gets written. Until V014 this was a SHA256 of the whole file that refused to load on any hex-edited exe,
   - installs the crash logger, the Present hook, then the streamer hooks and patches. MinHook hooks are queued and applied together.

The Core module rewrites its whole loader log with `"w"` for each line (lines kept in a table and joined with
`table.concat`), as `InfCore.WriteLog` does, and never calls `file:flush`. Two game Lua quirks force this:

- The game's `fopen` callback (`0x141A62B80`) only honours `"w"`. Any other mode, `"a"` included, opens read-only, so appends are silently lost.
- The game's Lua `file:flush` (`0x141A30930`) calls the C runtime `fflush` directly on the handle, but `io.open`
  returns the game's own stream object (opened through `0x141A62B80`), not a C `FILE`. V005 to V007 flushed that
  handle, and the next `file:write` crashed at `0x141A62F26` (`CALL [RAX+0x10]` on the stream's vtable, Event
  Viewer fault offset `0x1a62f26`).

IH runs modules at about 5.6 s into startup (V002 `ih_log.txt`). By then the streamer has already been built
(the first update always logs a non-null storage manager), so everything here works on the running streamer,
not at creation.

## 5. Safety rules in the code

- **Build check:** the PE TimeDateStamp is compared to 1.0.15.4 EN and a mismatch is logged as a warning, not refused. What gets written is decided by the per-site byte checks below, so a hook or patch whose bytes differ (another build, or a hex edit at that site) is skipped.
- **Hooks:** every hook target is compared to the 16 bytes read from Ghidra before MinHook touches it. If they differ, the hook is skipped and the log shows both.
- **Byte patches:** every byte patch has the original bytes it expects. A group of patches is written all together or not at all.
- **Threads suspended during patches:** addresses are resolved and memory allocated first, then other threads are suspended while patch bytes are written. If any thread is stopped inside a patch site, the group is skipped.
- **Only raise, never lower:** the budget the mod sends is only ever raised.
- **No handle cap changes** (9.1).

## 6. What it attaches to

### 6.1 Present wrapper (source of VRAM)

Found by pattern, not by fixed address. The pattern is from IHHook (0x-FADED, commit `0e282a0`, 21/09/2026).

| Address | Bytes | Instruction | Meaning |
|---|---|---|---|
| `0x14024CEFC` | `E8 8F 8A 7A 01 85 C0 75 12 38 43 69` | `CALL 0x1419F5990; TEST EAX,EAX; JNZ +0x12; CMP [RBX+0x69],AL` | Pattern `E8 ? ? ? ? 85 C0 75 12 38 43 69`, unique match. |
| `0x1419F5990` | `48 8B 49 18` | `MOV RCX,[RCX+0x18]` | Loads the `IDXGISwapChain*` from the wrapper's arg + 0x18. |
| `0x1419F5994` | `45 33 C0` | `XOR R8D,R8D` | Flags = 0. |
| `0x1419F5997` | `48 8B 01` | `MOV RAX,[RCX]` | Swapchain vtable. |
| `0x1419F599A` | `48 FF 60 40` | `JMP [RAX+0x40]` | Vtable slot 8, `IDXGISwapChain::Present`. |

On the first call the hook takes the swapchain, then:
`GetDevice(IDXGIDevice)` → `GetAdapter` → `DXGI_ADAPTER_DESC.DedicatedVideoMemory` and
`IDXGIAdapter3::QueryVideoMemoryInfo(LOCAL).Budget`. It keeps the larger of the two. The game is told
`min(that, 0xFFFFFFFF)`, or `0xFFFFFFFF` with `lockVramMax = true`. No `CreateDXGIFactory1` is called, and the
adapter is the one the game actually renders on (the old mod took adapter 0 from DllMain, which can be the wrong
GPU on laptops).

### 6.2 Hooks on the streamer and storage manager

| Hook | Address | First 16 bytes (checked) | What the game function does | What the hook does |
|---|---|---|---|---|
| "RequestTextureStorageConfiguration" | `0x1402A91D0` (`FUN_1402a91d0`) | `8B 02 39 41 60 75 0D 0F B7 42 04 66 39 41 64 75` | `(tsm, config*)`. If `config` differs from the applied config at `tsm+0x60`, copies the 8-byte config to `tsm+0x68` (pending) and returns 1. | Copies the caller's config, raises its size to the VRAM value if lower, passes the copy on. The caller's struct is not modified. |
| "GetAvailableStorageMemorySize" | `0x1402A8F60` (`FUN_1402a8f60`) | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 E8` | Adapter dedicated VRAM minus 10% (12% if the adapter name contains Radeon/AMD). Floor 512 MB. | Returns the VRAM value once known, the game's own value before that. |
| Streamer per-frame update ("dispatch") | `0x14021A460` (`FUN_14021a460`) | `40 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48` | Runs every frame with `this = DgTextureStreamer`. Drives the streamer state machine, including state 5 (reconfigure, 6.3). | Keeps the streamer pointer. Once VRAM is known, sends one budget request through the original RequestTextureStorageConfiguration. After the update, logs the budget whenever the applied config changes. |
| "TextureStorageManager::AllocBlock" | `0x1402A6D40` (`FUN_1402a6d40`) | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41` | `(tsm, size, type)`. Allocates a storage block (8.1). | Type 0 fails with `0xFFFFFFFF`: retries as type 2 in the large pool (8.2). Counts every failure per type. |

`FUN_1402a91d0` disassembly:

```
1402a91d0  8B 02           MOV EAX,[RDX]            ; requested size
1402a91d2  39 41 60        CMP [RCX+0x60],EAX       ; applied size
1402a91d5  75 0D           JNZ 1402a91e4
1402a91d7  0F B7 42 04     MOVZX EAX,word [RDX+4]   ; requested flag
1402a91db  66 39 41 64     CMP [RCX+0x64],AX        ; applied flag
1402a91df  75 03           JNZ 1402a91e4
1402a91e1  32 C0           XOR AL,AL                ; same config, nothing to do
1402a91e3  C3              RET
1402a91e4  48 8B 02        MOV RAX,[RDX]
1402a91e7  48 89 41 68     MOV [RCX+0x68],RAX       ; pending = requested
1402a91eb  B0 01           MOV AL,1
1402a91ed  C3              RET
```

The request is sent from inside the update, before the original runs, on the game's own thread. The same
update then sees the pending config, as it would after an in-game settings change.

### 6.3 Reconfigure path used (state 5 in `FUN_14021a460`)

Decompile of `0x14021A74F`..`0x14021A87C`:

```
if (pending config at tsm+0x68 != applied at tsm+0x60, and FUN_1402a79c0() == FUN_1402a79a0(tsm))
    avail  = GetAvailableStorageMemorySize()
    budget = min(avail, 3584MB)                 <- patch 1 removes 3584MB
    budget = min(budget, pending size)          <- our request raises this
    degrade = budget < 800MB                    <- patch 2 removes
    if (pending flag != 0) degrade = true       <- patch 3 removes
    overhead = budget/12 rounded to 32KB, storage = budget - 0x10680000 - overhead ...
    if FUN_1402a86f0(tsm, storage)              ; free old storage, allocate new; on failure it
        write +0x30a54..+0x30a70, +0x30a80      ;   reallocates the old size and returns 0
```

`FUN_1402a86f0` falls back to the previous storage size if the allocation fails. Then `+0x60` keeps the
old value and the log shows no "Applied config" change. This path only resizes the large pool
(the dev decomp has `ReinitializeLarge` and no small equivalent).

## 7. Byte patches

All original bytes are checked again at runtime before writing.

### 7.1 "Update budget clamps" (setting `patchDispatch`, one group)

| Address | Original | Instruction | Patched | Effect |
|---|---|---|---|---|
| `0x14021A772` | `BB 00 00 00 E0` | `MOV EBX,0xE0000000` | `BB FF FF FF FF` | Cap becomes 4095 MB. The `CMP RAX,RBX; CMOVC EBX,EAX` that follows keeps working, so a 64-bit avail above 4 GB saturates instead of wrapping. |
| `0x14021A797` | `44 0F 42 F8` | `CMOVC R15D,EAX` | `90 90 90 90` | No degrade when the budget is under 800 MB (`CMP EBX,0x32000000` above it). |
| `0x14021A7A0` | `74 10` | `JZ 0x14021A7B2` | `EB 10` | Always skips `MOVZX R15D,AL`, the forced degrade when the pending config's flag word is set. |

Context around the group (`0x14021A76D`..`0x14021A7B2`):

```
14021a76d  E8 ..           CALL FUN_1402a8f60        ; GetAvailableStorageMemorySize
14021a772  BB 00 00 00 E0  MOV EBX,0xE0000000        ; patch 1
14021a777  48 3B C3        CMP RAX,RBX
14021a77a  0F 42 D8        CMOVC EBX,EAX             ; EBX = min(avail, cap)
14021a77d  48 83 C7 68     ADD RDI,0x68              ; RDI = tsm + 0x68 (pending config)
14021a781  74 25           JZ 14021a7a8              ; never taken for a valid tsm
14021a783  39 1F           CMP [RDI],EBX
14021a785  0F 42 1F        CMOVC EBX,[RDI]           ; EBX = min(EBX, pending size)
14021a788  45 0F B6 FF     MOVZX R15D,R15B
14021a78c  81 FB 00 00 00 32  CMP EBX,0x32000000     ; 800MB
14021a792  B8 01 00 00 00  MOV EAX,1
14021a797  44 0F 42 F8     CMOVC R15D,EAX            ; patch 2
14021a79b  66 83 7F 04 00  CMP word [RDI+4],0        ; pending flag
14021a7a0  74 10           JZ 14021a7b2              ; patch 3
14021a7a2  44 0F B6 F8     MOVZX R15D,AL
14021a7a6  EB 0A           JMP 14021a7b2
14021a7a8  B8 00 00 40 38  MOV EAX,0x38400000        ; 900MB, dead path (RDI never 0)
14021a7ad  3B D8           CMP EBX,EAX
14021a7af  0F 47 D8        CMOVA EBX,EAX
```

The old mod also NOPed the `CMOVA` at `0x14021A7AF`. That path only runs when `tsm + 0x68 == 0`, which
cannot happen, so it is left alone.

### 7.2 Upgrades per frame ("doListupChangegraders", setting `patchUpgrade`)

| Address | Original | Instruction | Patched | Effect |
|---|---|---|---|---|
| `0x1402216E0` | `83 BB B8 00 00 00 10` | `CMP dword [RBX+0xB8],0x10` | `83 BB B8 00 00 00 20` (value = `upgradePerFrame`, 16..127) | Upgrade queue limit per frame. The next instruction is `JNC 0x14022178C` (`0F 83 9F 00 00 00`). |

From the dev decomp (`doListupChangegraders` @ dev-exe `0x1401ff000`): the count at `+0xB8` indexes a
64-entry `short` array at `+0x38` (`0x38 + 64*2 = 0xB8`), and each insert also checks `count == 0x40`.
Any value up to 127 therefore stops at 64 and cannot overflow the array.

The operand is an imm8, so 127 is the maximum. Old versions wrote a 10-byte imm32 form above 127, which
overwrote that `JNC`.

## 8. The small storage pool and the fallback

The storage manager has two block pools: small at `tsm+0x40` and large at `tsm+0x48`. The runtime
reconfigure only resizes the large one. The small pool is sized once at init from the constant `0x10680000`
(262.5 MB), which "Reserve" writes to `+0x30a58` and passes to Create, before IH loads.

With a texture-heavy file (RTX 4070, budget raised to 4095 MB) the large pool never ran short, while the small
pool went from 231 to 0 MB free. The update state then sat at 0 and every counter froze: the 5 s vehicle halt
and the endless iDroid load. The baseline run with `raiseBudget`, `patchDispatch` and `patchUpgrade` all false
froze the same way (small pool 0/262 MB, state 0, large pool 487/586 MB free), so it is vanilla behaviour, not
caused by the budget changes.

### 8.1 Allocation path

| Address | What it is |
|---|---|
| `0x1402A6D40` | `TextureStorageManager::AllocBlock(tsm, size, type)`. Type 0: small pool, returns `handle \| 0xAC910000`. Type 1: large pool from the top, `\| 0xAC920000`. Type 2: large pool, `\| 0xAC920000`. Other: large, `\| 0xAC940000`. Fails with `0xFFFFFFFF` (no room), or `0xFFFFFFFE` with a GC flag ORed into `tsm+0x2C` (1 for type 0, 6 for the large pool types) when enough total space is free but fragmented. On failure it keeps the largest failed request size per type at `tsm+0x30/34/38`, and clears it on success. |
| `0x1402A6A00` | block allocator used by AllocBlock (`+0x0` base, `+0x8` total bytes, `+0xC` block size, `+0x10` log2 block size, `+0x18` allocated blocks, `+0x20` handle table, 8 bytes per entry with `+4` start block and `+6` block count as `ushort`) |
| `0x14021AB30` | only caller. Stores the handle in the texture's slot for its level (`+0x24 + level*4`), bumps the per-level counters `streamer+0x3efa0..`, registers the block id, and on failure sets `0x20000000` in `streamer+0x26324` and retries later |
| `0x140219D80` | free. Takes the handle from the slot, size by handle bits, block type from `0x1402A7970`, frees with `0x1402A7680`, takes the counters off by the slot's level |
| `0x1402A79F0` / `0x1402A7A20` | block address / size: `(handle & 0xF0000) == 0x10000` means small pool, anything else large |
| `0x1402A7680` / `0x1402A7E20` | free block / register block id, pool picked by handle bits |
| `0x1402A7970` | block type: `0x20000` -> 2, `0x40000` -> 3, else 0. Type 3 goes to separate counters `+0x3efc4/+0x3efc8` |
| `0x1402A7450` / `0x1402A6EC0` | compaction of one pool, moves blocks and notifies by the registered id |

The AllocBlock type is the texture's quality level slot. Level 0, the lowest mip every streamed texture keeps,
is what fills the small pool. From how the counters move in test runs, levels 0 and 1 stay in storage while
level 2 blocks are short-lived: "shown lv2" reached 1259 MB while level 2 storage stayed under 10 MB.

### 8.2 The fallback (setting `smallPoolFallback`)

Hook on `0x1402A6D40`. When a type 0 call returns `0xFFFFFFFF`, it calls the original again with type 2 and
returns that handle. Every function that reads the handle picks the pool from its bits, so the texture lives in
the large pool. Type 2 tags it `0xAC92`, which reads back as block type 2, not 3, so freeing it takes the level 0
counters back down (a `0xAC94` handle would leak them into the type 3 counters). If the large pool also refuses,
the original `0xFFFFFFFF` is returned and the game behaves as vanilla.

`0xFFFFFFFE` (fragmented, GC pending) is left alone so the game's own compaction runs.

The compaction move callback has not been traced. Three test runs with the fallback active showed no problems
(section 12).

## 9. Not patched, and why

### 9.1 Handle cap 0x1310 (4880)

From the dev decomp (`Gr_dx11_win64\DgTextureStreamer.cpp`, 2015 prototype) and retail at the sites below,
4880 is compiled into the `DgTextureStreamer` object:

- `sequence_list<DgIncrementalTextureResource::Proxy*, 4880>` and `sequence_allocator<node, 4881>`: fixed-size templates stored inside the object (constructor dev-exe `0x1401f49d0`).
- An 8-byte array at `+0x69178` with 0x1310 entries, followed directly by its count at `+0x729f8` (`0x69178 + 0x1310*8 = 0x729f8`).
- Handle checks `handle < 0x1310` in `GetProxy`, `IsLoaded`, `Dispose` and `Request/UnrequestTextureDetailByDgTexture`. 0x1310 is the invalid-handle value.

Retail sites the old mod patched:

| Address | Bytes | Context | Result of raising it |
|---|---|---|---|
| `0x14022159E` ("doDegrade") | `3D 10 13 00 00` | `MOV EAX,[R15+0x729f8]; CMP EAX,0x1310; JZ; MOV [R15+RAX*8+0x69178],RBP; INC [R15+0x729f8]` | Entry 0x1310 lands on the count itself, then writes run past the object's own fields (`+0x72b00`, `+0x72b10`, `+0x72b18` are read every frame). Memory corruption. |
| `0x140220F63`, `0x140220FFD` ("allocateHandlableProxy") | `81 FA 10 13 00 00` | Proxy index `== 0x1310` → free it and retry | Only moves which index is skipped. Index 0x1310 then gets used as a handle everywhere else treats as invalid. No capacity gained. |

"DoUpgrade2", `CancelUpgrade`, `DoLoadEntry`, `DoUpgradeEntry` and `PostCreation` use the same
`+0x69178` / `+0x729f8` bound as doDegrade (`0x140222010` fills that array). Retail has 10 `CMP EAX,0x1310` and
6 `CMP EDX,0x1310` sites. None can be raised without moving arrays that are compiled into the object's layout,
so the handle multiplier is gone for good. Loading earlier would not change this.

The "search tags" count at `+0x69174` (offset from older sources) tracked the level 0 block count almost exactly
in testing, so it looks like one entry per streamed texture. The heavy test file peaked around 3800 of 4880.
A much heavier texture pack could still reach the cap.

### 9.2 Create-time sizing

"Reserve" is `FUN_14021dee0` (first bytes `4C 8B DC 57 48 83 EC 50 49 C7 43 C8 FE FF FF FF`).
It runs once, from the streamer's init (`FUN_14021d2f0`). It:

- clamps storage at `0x14021DF74`: `B9 00 00 00 E0 48 3B C1 0F 42 C8 BA 00 00 40 38 3B CA 0F 47 CA`, i.e. min(avail, 3584MB, 900MB),
- builds the Create parameters `{total, 0x10680000, storage, 0x1310, 0x8000}`,
- calls "Create" `0x1402A8790`, which is a `JMP 0x14459D000` into the anti-tamper region (`E9 6B 48 2F 04`).

All of this has finished before IH loads the plugin: the first update always logs a non-null storage manager.
The plugin hooks none of these. Its budget comes through the reconfigure path in 6.3, and the small pool
limit is handled by the fallback in section 8. Enlarging the small pool itself would need a loader that runs
before the streamer is created (dinput8 or IHHook), which this plugin avoids so it only depends on IH.

### 9.3 VRAM above 4 GB (64-bit values)

The 4095 MB ceiling comes from the game storing every budget value as 32-bit. Widening it is not one variable:

- The budget fields (`+0x30a54..+0x30a70`) sit packed 4 bytes apart inside the streamer object, so there is no room to make them 64-bit in place. They would have to move, with every instruction that reads or writes them rewritten, across the streamer and the storage manager.
- The storage manager computes block addresses in 32 bits (`0x1402A79F0`: `(uint)(start block * block size) + base`), and block numbers are 16-bit `ushort`s in the handle table. A pool over 4 GB would wrap and point at the wrong memory.
- It is not the bottleneck. On the heavy test file the large pool never passed about 420 MB used out of 1304 MB, and the cache budget is already 3492 MB. What ran out was the fixed small pool (section 8) and potentially the 4880 cap (9.1), and more VRAM helps with neither.

### 9.4 Old mgsv_mod problems

- Most of its addresses were from a different build: the streamer ones about `+0x4D0` off (`+0x4E0` in the update function), and a few pointed into the anti-tamper junk code. On current retail they land in the middle of functions, which hung the first port on the loading screen.
- It raised the 0x1310 handle cap (9.1).
- It called `CreateDXGIFactory1` from `DllMain` and took adapter 0.
- It cut the dedicated VRAM value to 32 bits, so an 8 GB card's dedicated memory wrapped to 0.
- Upgrades per frame above 127 wrote 10 bytes over a 7-byte instruction.
- Byte patches went in with no check of which exe was running.
- It never touched the small pool, so the stall remained.

## 10. Address reference

```
Hooks
0x14024CEFC  call site for pattern E8 ? ? ? ? 85 C0 75 12 38 43 69
0x1419F5990  gn::swapchain::Present wrapper, swapchain at arg+0x18, VRAM read on first frame
0x14021A460  streamer per-frame update, sends the VRAM budget request once
0x1402A8F60  GetAvailableStorageMemorySize, returns real VRAM instead of the estimate
0x1402A91D0  RequestTextureStorageConfiguration, raises any budget request up to VRAM
0x1402A6D40  TextureStorageManager::AllocBlock, small pool full -> retry in large pool

Patches
0x14021A772  BB 00 00 00 E0 -> BB FF FF FF FF   3584 MB cap -> uint32 max
0x14021A797  44 0F 42 F8    -> 90 90 90 90      degrade under 800 MB
0x14021A7A0  74 10          -> EB 10            degrade on config flag
0x1402216E0  83 BB B8 00 00 00 10 -> ..20       upgrades per frame 16 -> 32

Storage manager (read only)
0x14021AB30  texture block alloc, only caller of AllocBlock, one handle per mip level
0x140219D80  texture block free, counters taken off by mip level
0x1402A6A00  block allocator both pools use
0x1402A79F0  block address, pool picked from handle bits
0x1402A7A20  block size, pool picked from handle bits
0x1402A7680  free block, pool picked from handle bits
0x1402A7E20  register block id, pool picked from handle bits
0x1402A7970  block type from handle (0xAC92 -> 2, 0xAC94 -> 3)
0x1402A7450  pool compaction
0x1402A6EC0  compaction of one block run

Reference only, not patched
0x14021D2F0  streamer init, calls Reserve
0x14021DEE0  streamer init sizing ("Reserve"), sets the 262.5 MB small pool, runs before IH loads
0x14021DF74  init clamp min(avail, 3584 MB, 900 MB)
0x1402A8790  Create, a jmp into 0x14459D000 (anti-tamper region)
0x1402A86F0  storage resize (falls back to old size on failure)
0x14021F9A0  per-level shown bytes update
0x140222010  release list fill (+0x69178 array, count +0x729f8), shown bytes update
0x14021A7AF  CMOVA on the dead 900 MB path
0x14022159E  doDegrade 0x1310 array bound, leave it alone
0x140220F63  allocateHandlableProxy reserved index 0x1310
0x140220FFD  same
0x141A30930  Lua file:flush -> CRT fflush on game stream, avoid
0x141A62B80  Lua io fopen callback, only "w" honoured
0x141A62F26  where a write crashes after a flush

Offsets
DgTextureStreamer +0x26324 flags (0x20000000 = block allocation failed this frame)
                  +0x26328 update state
                  +0x26388 storage manager
                  +0x30a54 vramSize, +0x30a58 small pool size, +0x30a60 storage,
                  +0x30a64 clamped store, +0x30a70 cache budget, +0x30a80 degrade flag
                  +0x30a74/78/7c bytes of textures by mip level shown
                  +0x3efa0/a4/a8 blocks, +0x3efac/b0/b4 bytes allocated per level
                  +0x3efb8/bc/c0 per-frame alloc fail counts (cleared every update)
                  +0x3efc4/c8 type 3 blocks / bytes
                  +0x69174 search tags (older sources), +0x69178 release list, +0x729f8 its count
Storage manager   +0x2c GC flags, +0x30/34/38 largest failed request per type
                  +0x40 small pool, +0x48 large pool
                  +0x60 applied config {uint32 size, uint16 flag}, +0x68 pending config
Texture entry     +0x8 new level, +0xb current level, +0x18/1c/20 size per level,
                  +0x24/28/2c handle per level
Handles           0xAC91xxxx small pool, 0xAC92xxxx / 0xAC94xxxx large pool
```

## 11. Logs

- `plugins\TextureStreamer_loader.log`: written by the Core Lua module, loadlib result. Always written.
- `TextureStreamer.log` beside `mgsvtpp.exe` (previous run in `TextureStreamer.prev.log`).
- `TextureStreamer_boot.log` beside `mgsvtpp.exe`, only with `debugLog = true`.

**With `debugLog = false` (default)**, `TextureStreamer.log` has the settings, build and address set, every hook
and patch result, the VRAM line, the applied budget and any crash report. The stall monitor returns straight
away, so no per-frame timing runs. The fallback still works and counts, it just doesn't log.

**With `debugLog = true`** it adds:

- `[Usage] periodic` every 5 s from the streamer update, `[Usage] STALL streamer update took N ms` when one
  update takes over 50 ms, and `[Usage] STALL frame gap N ms` when two Presents are more than 250 ms apart,
  with whether the streamer update was running at the time. `[Usage]` shows update state, storage, cache budget,
  degrade flag, small/large pool free (total - (allocated blocks + 1) * block size, the same check AllocBlock
  uses), and the two 4880-entry list counts.
- `[Blocks]` after each `[Usage]`:
  - `shown lv0/1/2`: `streamer+0x30a74/78/7c`, bytes of textures by the level they currently show
    (updated in `0x14021F9A0` and `0x140222010`). Not pool usage: a texture showing level 2 can still hold its level 0 block.
  - `allocated lvN count (MB)`: `streamer+0x3efa0/a4/a8` and `+0x3efac/b0/b4`, blocks and bytes held in
    each level slot. Level 0 includes blocks the fallback put in the large pool.
  - `type3`: `+0x3efc4/c8`, taken off when a freed block reads back as type 3. Stays 0 with the fallback.
  - `full` and `gc wait`: AllocBlock returning `0xFFFFFFFF` / `0xFFFFFFFE` per level, counted in the hook
    since start, before any retry. The game's own counters at `+0x3efb8..c0` are cleared every update.
  - `small->large N (M MB, failed F)`: fallback count, total MB requested (not reduced on free), and retries the large pool refused.
- `[Storage] Small pool full, N KB placed in the large pool` for the first 5 fallbacks, then every 500th.
- `TextureStreamer_boot.log`: raw Win32 writes (tick, thread id, step), from `InitThread settings loaded`
  onwards: `InitThread done`, the first call of each hook (`hkPresent`, `hkUpdate`, `hkGetAvail`,
  `hkRequestConfig`), the DXGI steps in the Present hook and `... original returned`. The last line before the
  log stops is where a crash happened.

To isolate a crash, turn hooks and patches off one at a time: `hookPresent`, `hookUpdate`, `hookGetAvail`,
`hookRequestConfig`, `patchDispatch`, `patchUpgrade`, `smallPoolFallback`. `hookUpdate` also needs `hookRequestConfig`.

`TextureStreamer.log`, in order:

1. `[DLL] InitThread started. TEXTURESTREAMER,V0_16_20260924`
2. `[Settings] Loaded ...\plugins\TextureStreamer.lua`, then one line with every setting
3. `[AddressSet] mgsvtpp.exe TimeDateStamp 0x6A4CB898 SizeOfImage 0x...`, then `Selected EN 1.0.15.4 address set.`
4. `[CRASH] Unhandled-exception crash logger installed.`
5. `[Vram] Present pattern @0x14024CEFC -> gn::swapchain::Present 0x1419F5990`, `[Hook] Present: OK`
6. For each of the four hooks in 6.2: `@0x... bytes [...] matches`, then `: OK`
7. `[Patch] Update budget clamps: applied`, `[Patch] doListupChangegraders upgrade per frame: applied`, `[Streamer] Upgrade per frame: 32 (was 16)`
8. `[DLL] MH_ApplyQueued -> 0`, `[DLL] InitThread done.`

Then, once the game is running (9 and 10 can come in either order):

9. `[Vram] Adapter '...' dedicated=... budget=... using=... MB`, `[Vram] Resolved on first Present: N MB, reported to game M MB`
10. `[Streamer] First update: streamer=... tsm=... (storage created before plugin load)`
11. `[Streamer] Requested budget N MB (was M MB) -> pending`
12. `[Streamer] Applied config ... MB: vramSize ..., clampedStore ..., storage ..., cacheBudget ..., degradeFlag ...`.
    The first one is logged on the first update, so it can show the game's own M MB before the request goes through.

What it means:

- **An `Applied config N MB` line with the requested N:** the reconfigure landed. `degradeFlag` 2 is normal mode with a cache budget. 1 is degrade mode (cache budget 0), which the patches in 7.1 prevent.
- **Line 11 but never an `Applied config N MB`:** the request is pending but the game hasn't applied it. Either the gate in 6.3 (`FUN_1402a79c0() == FUN_1402a79a0(tsm)`) is not being met, or the storage allocation failed and fell back.
- **`Budget already N MB, no request`:** the game's own config was already at or above the VRAM value.
- **Any `MISMATCH` or `SKIPPED`:** nothing was written at that site.
- **`WARNING: exe is not the target 1.0.15.4 EN build`:** different TimeDateStamp. Check which hooks and patches say `matches` and which say `MISMATCH`; only the matching ones were applied.
- **No `[Vram] Resolved` line:** the Present hook never ran.
- **A `[CRASH]` block:** faulting address, nearest hooked address, registers and game return addresses on the stack.

## 12. Test results

RTX 4070 (12 GB), Afghanistan, texture-heavy file, driving and calling in items from the iDroid.

| Run | Result |
|---|---|
| V010, budget raised, no fallback | Small pool 0/262 MB, state stuck at 0, 5 s halt driving, iDroid loads forever. Large pool about 1200 MB free. |
| V011, `raiseBudget`/`patchDispatch`/`patchUpgrade` false | Same freeze. Small pool 0/262 MB, large pool 487/586 MB free. |
| V012, fallback on | 163 small->large, 0 failed. No halts, no low res fallback textures. Large pool bottomed at 890/1304 MB free. |
| V012, second run | 42 small->large, 0 failed. Same result. |
| V013, fallback on, new counters | 74 small->large (6 MB), 0 failed, `full 74/0/0`, `gc wait 0/0/0`, `type3` 0. Level 0 allocated 267 MB against a 262 MB pool, matching the 6 MB moved. Large pool used (349 MB) matches level 1 (335 MB) + level 2 (8 MB) + moved level 0 (6 MB). Level 0 count went 3786 -> 3773, so level 0 blocks keep being freed and replaced. |

## 13. Revision history

| Revision | Change |
|---|---|
| V001-V011 | Port of mgsv_mod to an IH plugin on MGSV_HookSample: Ghidra-checked addresses, SHA256 gate, byte checks, vendored MinHook, swapchain VRAM, Lua loader fixes (4), DLL pinning, usage/stall logging, `raiseBudget` switch for vanilla comparison. |
| V012 | Small pool fallback (AllocBlock hook). Settings log line fixed. |
| V013 | `[Blocks]` line with the game's own per-level counters. |
| V014 | Release build: `debugLog = false` by default. |
| V015 | Build check by PE header instead of SHA256, so hex-edited exes are accepted. |
| V016 | Build check is a logged warning only, on TimeDateStamp. SizeOfImage no longer compared (anti-tamper changes it). |

## 14. Credits

- ClearEdge: the swapchain route for VRAM (`GetDevice` → `GetAdapter`, `IDXGIAdapter3`, no factory, no thread).
- 0x-FADED (IHHook): the Present pattern.
- Infinite Heaven: plugin loading. MGSV_HookSample: project base.
- MinHook (TsudaKageyu).
