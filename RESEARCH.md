# TextureStreamer research

Target: MGSV TPP retail 1.0.15.4 EN, `mgsvtpp.exe` PE header TimeDateStamp `0x6A4CB898`
(unmodified exe SHA256 `085c2f82d1c963c40b3d2d55786661dfee2b18cbbf388a710c00fa76c5e9bb45`). Revision V016.

Addresses are retail 1.0.15.4 EN (preferred base `0x140000000`), read in Ghidra unless another source is given.
Names in quotes come from the old mgsv_mod source or the 2015 dev decomp. Retail functions are unnamed (`FUN_`).

## 1. What the mod does

MGSV sizes its texture streaming budget from a VRAM estimate, clamps it to 3584 MB and to the texture budget the
game has requested, and forces a low-quality "degrade" mode below 800 MB. It queues at most 16 texture upgrades
per frame.

The mod:

1. reads the real VRAM of the adapter the game renders on,
2. asks the streamer to reconfigure to that budget through the game's own reconfigure path,
3. removes the 3584 MB cap and the two forced-degrade conditions,
4. raises upgrades per frame from 16 to `upgradePerFrame` (default 32),
5. retries a failed small pool allocation in the large pool.

It does not touch the handle cap or the create-time sizing (section 7).

## 2. Load chain

1. IH loads `mod\modules\TextureStreamer_Core.lua` as an external module.
2. The Core module calls `package.loadlib("<game>\plugins\TextureStreamer.dll", "luaopen_TextureStreamer")`, then calls the result.
   It logs to `plugins\TextureStreamer_loader.log` and `ih_log.txt`.
3. `DllMain` pins the DLL (`GET_MODULE_HANDLE_EX_FLAG_PIN`) so it is never unloaded. Without it Lua freed the
   loadlib handle at shutdown, unmapping hook code game threads could still run. It then starts `InitThread`.
   `luaopen_TextureStreamer` only exists for loadlib.
4. `InitThread`:
   - opens `TextureStreamer.log` beside `mgsvtpp.exe` (previous run kept as `TextureStreamer.prev.log`),
   - reads `plugins\TextureStreamer.lua` as literal `key = value` lines (never run),
   - logs the exe's PE `TimeDateStamp` and `SizeOfImage`. A TimeDateStamp other than `0x6A4CB898` logs a warning and the 1.0.15.4 EN address set is still tried. SizeOfImage is not compared, the anti-tamper wrapper (Denuvo) bloats it,
   - installs the crash logger, the Present hook, then the streamer hooks and patches. MinHook hooks are queued and applied together.

The Core module rewrites its whole loader log with `"w"` for each line, as `InfCore.WriteLog` does, and never
calls `file:flush`:

- The game's `fopen` (`0x141A62B80`) only honours `"w"`. Any other mode, `"a"` included, opens read-only, so appends are lost.
- The game's Lua `file:flush` (`0x141A30930`) calls the C runtime `fflush` on the handle, but `io.open` returns the
  game's own stream object (opened through `0x141A62B80`), not a C `FILE`. Flushing it made the next `file:write`
  crash at `0x141A62F26` (`CALL [RAX+0x10]` on the stream's vtable, Event Viewer fault offset `0x1a62f26`).

IH runs modules about 5.6 s into startup. The streamer is already built by then, so everything here works on the
running streamer, not at creation.

## 3. Safety rules

- The PE TimeDateStamp check only warns. The per-site checks decide what gets written.
- Every hook target is compared to the 16 bytes read from Ghidra before MinHook touches it. On a mismatch the hook is skipped and the log shows both.
- Every byte patch has the original bytes it expects. A patch group is written all together or not at all.
- Other threads are suspended while patch bytes are written. If a thread is stopped inside a patch site, the group is skipped.
- The budget is only ever raised, never lowered.

## 4. What it attaches to

### 4.1 Present wrapper (VRAM source)

Found by pattern. The pattern is from IHHook (0x-FADED, commit `0e282a0`).

| Address | Bytes | Instruction | Meaning |
|---|---|---|---|
| `0x14024CEFC` | `E8 8F 8A 7A 01 85 C0 75 12 38 43 69` | `CALL 0x1419F5990; TEST EAX,EAX; JNZ +0x12; CMP [RBX+0x69],AL` | Pattern `E8 ? ? ? ? 85 C0 75 12 38 43 69`, unique match |
| `0x1419F5990` | `48 8B 49 18` | `MOV RCX,[RCX+0x18]` | `IDXGISwapChain*` from arg + 0x18 |
| `0x1419F5994` | `45 33 C0` | `XOR R8D,R8D` | Flags = 0 |
| `0x1419F5997` | `48 8B 01` | `MOV RAX,[RCX]` | Swapchain vtable |
| `0x1419F599A` | `48 FF 60 40` | `JMP [RAX+0x40]` | Vtable slot 8, `IDXGISwapChain::Present` |

On the first call: `GetDevice(IDXGIDevice)` → `GetAdapter` → `DXGI_ADAPTER_DESC.DedicatedVideoMemory` and
`IDXGIAdapter3::QueryVideoMemoryInfo(LOCAL).Budget`, keeping the larger. The game is told `min(that, 0xFFFFFFFF)`,
or `0xFFFFFFFF` with `lockVramMax = true`. No `CreateDXGIFactory1`.

### 4.2 Streamer and storage manager hooks

| Hook | Address | First 16 bytes | Game function | Hook |
|---|---|---|---|---|
| "RequestTextureStorageConfiguration" | `0x1402A91D0` | `8B 02 39 41 60 75 0D 0F B7 42 04 66 39 41 64 75` | `(tsm, config*)`. If `config` differs from the applied config at `tsm+0x60`, copies the 8-byte config to `tsm+0x68` (pending), returns 1 | Copies the caller's config, raises its size to VRAM if lower, passes the copy on |
| "GetAvailableStorageMemorySize" | `0x1402A8F60` | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 E8` | Dedicated VRAM minus 10% (12% for Radeon/AMD), floor 512 MB | Returns VRAM once known, the game's value before that |
| Streamer per-frame update ("dispatch") | `0x14021A460` | `40 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48` | Runs every frame with `this = DgTextureStreamer`, drives the state machine including state 5 (4.3) | Keeps the streamer pointer, sends one budget request once VRAM is known, logs the budget when the applied config changes |
| "TextureStorageManager::AllocBlock" | `0x1402A6D40` | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41` | `(tsm, size, type)`, allocates a storage block (6.1) | Type 0 returning `0xFFFFFFFF`: retries as type 2 (6.2) |

`FUN_1402a91d0`:

```
1402a91d0  8B 02           MOV EAX,[RDX]            ; requested size
1402a91d2  39 41 60        CMP [RCX+0x60],EAX       ; applied size
1402a91d5  75 0D           JNZ 1402a91e4
1402a91d7  0F B7 42 04     MOVZX EAX,word [RDX+4]   ; requested flag
1402a91db  66 39 41 64     CMP [RCX+0x64],AX        ; applied flag
1402a91df  75 03           JNZ 1402a91e4
1402a91e1  32 C0           XOR AL,AL                ; same config
1402a91e3  C3              RET
1402a91e4  48 8B 02        MOV RAX,[RDX]
1402a91e7  48 89 41 68     MOV [RCX+0x68],RAX       ; pending = requested
1402a91eb  B0 01           MOV AL,1
1402a91ed  C3              RET
```

The request is sent from inside the update, before the original runs, on the game's thread. The same update then
applies the pending config, as after an in-game settings change.

### 4.3 Reconfigure path (state 5 in `FUN_14021a460`, `0x14021A74F`..`0x14021A87C`)

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

If `FUN_1402a86f0` fails, `+0x60` keeps the old value and no "Applied config" change is logged. This path only
resizes the large pool (the dev decomp has `ReinitializeLarge`, no small equivalent).

## 5. Byte patches

### 5.1 Update budget clamps (`patchDispatch`, one group)

| Address | Original | Instruction | Patched | Effect |
|---|---|---|---|---|
| `0x14021A772` | `BB 00 00 00 E0` | `MOV EBX,0xE0000000` | `BB FF FF FF FF` | Cap becomes 4095 MB. The following `CMP RAX,RBX; CMOVC EBX,EAX` saturates a 64-bit avail above 4 GB instead of wrapping |
| `0x14021A797` | `44 0F 42 F8` | `CMOVC R15D,EAX` | `90 90 90 90` | No degrade under 800 MB (`CMP EBX,0x32000000` above it) |
| `0x14021A7A0` | `74 10` | `JZ 0x14021A7B2` | `EB 10` | Skips `MOVZX R15D,AL`, the forced degrade when the pending flag word is set |

`0x14021A76D`..`0x14021A7B2`:

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

The old mod also NOPed the `CMOVA` at `0x14021A7AF`. That path only runs when `tsm + 0x68 == 0`, which cannot
happen, so it is left alone.

### 5.2 Upgrades per frame ("doListupChangegraders", `patchUpgrade`)

| Address | Original | Instruction | Patched | Effect |
|---|---|---|---|---|
| `0x1402216E0` | `83 BB B8 00 00 00 10` | `CMP dword [RBX+0xB8],0x10` | `83 BB B8 00 00 00 20` (`upgradePerFrame`, 16..127) | Upgrade queue limit per frame. Next instruction is `JNC 0x14022178C` (`0F 83 9F 00 00 00`) |

Dev decomp (`doListupChangegraders`, dev-exe `0x1401ff000`): the count at `+0xB8` indexes a 64-entry `short` array
at `+0x38` (`0x38 + 64*2 = 0xB8`), and each insert also checks `count == 0x40`, so any value up to 127 stops at 64.
The operand is imm8, so 127 is the maximum. The old mod wrote a 10-byte imm32 form above 127, overwriting the `JNC`.

## 6. Small storage pool and fallback

The storage manager has two block pools: small at `tsm+0x40`, large at `tsm+0x48`. Only the large one is resized
at runtime. The small pool is sized once at init from `0x10680000` (262.5 MB), which "Reserve" writes to `+0x30a58`
and passes to Create, before IH loads.

With a texture-heavy file (RTX 4070, budget 4095 MB) the small pool went from 231 to 0 MB free while the large pool
had plenty. The update state then sat at 0 and every counter froze: a 5 s halt while driving, and the iDroid
loading forever. With `raiseBudget`, `patchDispatch` and `patchUpgrade` all off it froze the same way (small pool
0/262 MB, large pool 487/586 MB free), so it is vanilla behaviour (does not matter un-modded).

### 6.1 Allocation path

| Address | What it is |
|---|---|
| `0x1402A6D40` | `TextureStorageManager::AllocBlock(tsm, size, type)`. Type 0: small pool, `handle \| 0xAC910000`. Type 1: large pool from the top, `\| 0xAC920000`. Type 2: large pool, `\| 0xAC920000`. Other: large, `\| 0xAC940000`. Fails with `0xFFFFFFFF` (no room), or `0xFFFFFFFE` with a GC flag ORed into `tsm+0x2C` (1 for type 0, 6 for large) when enough total space is free but fragmented. Keeps the largest failed request per type at `tsm+0x30/34/38`, cleared on success |
| `0x1402A6A00` | block allocator (`+0x0` base, `+0x8` total, `+0xC` block size, `+0x10` log2 block size, `+0x18` allocated blocks, `+0x20` handle table, 8 bytes per entry, `+4` start block and `+6` block count as `ushort`) |
| `0x14021AB30` | only caller. Stores the handle in the texture's slot for its level (`+0x24 + level*4`), adds to `streamer+0x3efa0..`, registers the block id; on failure sets `0x20000000` in `streamer+0x26324` and retries later |
| `0x140219D80` | free. Handle from the slot, size by handle bits, block type from `0x1402A7970`, frees with `0x1402A7680`, takes the counters off by the slot's level |
| `0x1402A79F0` / `0x1402A7A20` | block address / size: `(handle & 0xF0000) == 0x10000` is small pool, anything else large |
| `0x1402A7680` / `0x1402A7E20` | free block / register block id, pool from handle bits |
| `0x1402A7970` | block type: `0x20000` -> 2, `0x40000` -> 3, else 0. Type 3 uses separate counters `+0x3efc4/+0x3efc8` |
| `0x1402A7450` / `0x1402A6EC0` | compaction of one pool, moves blocks and notifies by registered id |

The AllocBlock type is the texture's quality level slot. Level 0, the lowest mip every streamed texture keeps, is
what fills the small pool. In test runs levels 0 and 1 stay in storage while level 2 blocks are short-lived
("shown lv2" reached 1259 MB with level 2 storage under 10 MB).

### 6.2 Fallback (`smallPoolFallback`)

When a type 0 call returns `0xFFFFFFFF`, the hook calls the original again with type 2 and returns that handle.
Every function that reads a handle picks the pool from its bits, so the block lives in the large pool. Type 2 tags
it `0xAC92`, which reads back as block type 2, so freeing it takes the level 0 counters back down (`0xAC94` would
leak them into the type 3 counters). If the large pool also refuses, the original `0xFFFFFFFF` is returned.

`0xFFFFFFFE` (fragmented, GC pending) is left alone so the game's compaction runs.

Not traced: the compaction move callback. No problems in three test runs.

Results on the texture-heavy file:

| Run | Result |
|---|---|
| Fallback off (V010) | Small pool 0/262 MB, state stuck at 0, halts and iDroid hang |
| Vanilla values (V011) | Same freeze, large pool 487/586 MB free |
| Fallback on (V012) | 163 small->large, 0 failed, no halts, no low res fallback. Large pool bottomed at 890/1304 MB free |
| Fallback on (V012, second run) | 42 small->large, 0 failed |
| Fallback on (V013) | 74 small->large (6 MB), 0 failed, `full 74/0/0`, `gc wait 0/0/0`, `type3` 0. Level 0 allocated 267 MB against a 262 MB pool. Large pool used (349 MB) = level 1 (335) + level 2 (8) + moved level 0 (6). Level 0 count 3786 -> 3773, so level 0 blocks keep being freed |

## 7. Not patched

### 7.1 Handle cap 0x1310 (4880)

From the dev decomp (`Gr_dx11_win64\DgTextureStreamer.cpp`) and the retail sites below, 4880 is compiled into
the `DgTextureStreamer` object:

- `sequence_list<DgIncrementalTextureResource::Proxy*, 4880>` and `sequence_allocator<node, 4881>`, fixed-size templates inside the object (constructor dev-exe `0x1401f49d0`).
- An 8-byte array at `+0x69178` with 0x1310 entries, followed directly by its count at `+0x729f8` (`0x69178 + 0x1310*8 = 0x729f8`).
- `handle < 0x1310` checks in `GetProxy`, `IsLoaded`, `Dispose` and `Request/UnrequestTextureDetailByDgTexture`. 0x1310 is the invalid-handle value.

Retail sites the old mod patched:

| Address | Bytes | Context | Result of raising it |
|---|---|---|---|
| `0x14022159E` ("doDegrade") | `3D 10 13 00 00` | `MOV EAX,[R15+0x729f8]; CMP EAX,0x1310; JZ; MOV [R15+RAX*8+0x69178],RBP; INC [R15+0x729f8]` | Entry 0x1310 lands on the count, then writes run past the object's own fields (`+0x72b00`, `+0x72b10`, `+0x72b18`, read every frame). Memory corruption |
| `0x140220F63`, `0x140220FFD` ("allocateHandlableProxy") | `81 FA 10 13 00 00` | Proxy index `== 0x1310` → free and retry | Only moves which index is skipped; 0x1310 then gets used as a handle everywhere else treats as invalid. No capacity gained |

"DoUpgrade2", `CancelUpgrade`, `DoLoadEntry`, `DoUpgradeEntry` and `PostCreation` use the same `+0x69178` /
`+0x729f8` bound (`0x140222010` fills the array). Retail has 10 `CMP EAX,0x1310` and 6 `CMP EDX,0x1310` sites.

Raising it would mean moving those arrays out of the object and rewriting every access and the list/allocator code
built around the fixed sizes. Create also takes `0x1310` as a parameter before IH loads, so it would need an early
loader (dinput8 or IHHook). The block allocator's 14-bit link fields cap each pool at about 16k entries regardless.

"Search tags" (`+0x69174`, offset from older sources) tracked the level 0 block count almost exactly in testing,
so it looks like one entry per streamed texture. The heavy test file peaked around 3800 of 4880.

### 7.2 Create-time sizing

"Reserve" is `FUN_14021dee0` (`4C 8B DC 57 48 83 EC 50 49 C7 43 C8 FE FF FF FF`), run once from the streamer's
init (`FUN_14021d2f0`). It:

- clamps storage at `0x14021DF74`: `B9 00 00 00 E0 48 3B C1 0F 42 C8 BA 00 00 40 38 3B CA 0F 47 CA`, min(avail, 3584MB, 900MB),
- builds the Create parameters `{total, 0x10680000, storage, 0x1310, 0x8000}`,
- calls "Create" `0x1402A8790`, a `JMP 0x14459D000` into the anti-tamper region (`E9 6B 48 2F 04`).

This has finished before IH loads the plugin: the first update always logs a non-null storage manager. None of it
is hooked. The budget goes through 4.3 and the small pool limit through 6.2.

### 7.3 VRAM above 4 GB

- The budget fields (`+0x30a54..+0x30a70`) are packed 4 bytes apart, so they can't be widened in place. Every read and write across the streamer and storage manager would need rewriting.
- Block addresses are computed in 32 bits (`0x1402A79F0`: `(uint)(start block * block size) + base`) and block numbers are 16-bit. A pool over 4 GB would wrap.
- The large pool never passed about 420 MB used out of 1304 MB in testing, and the cache budget is already 3492 MB.

## 8. Offsets

```
DgTextureStreamer +0x26324 flags (0x20000000 = block allocation failed)
                  +0x26328 update state
                  +0x26388 storage manager
                  +0x30a54 vramSize, +0x30a58 small pool size, +0x30a60 storage,
                  +0x30a64 clamped store, +0x30a70 cache budget, +0x30a80 degrade flag
                  +0x30a74/78/7c bytes of textures by level shown
                  +0x3efa0/a4/a8 blocks, +0x3efac/b0/b4 bytes allocated per level
                  +0x3efb8/bc/c0 per-frame alloc fail counts (cleared every update)
                  +0x3efc4/c8 type 3 blocks / bytes
                  +0x69174 search tags, +0x69178 release list, +0x729f8 its count
Storage manager   +0x2c GC flags, +0x30/34/38 largest failed request per type
                  +0x40 small pool, +0x48 large pool
                  +0x60 applied config {uint32 size, uint16 flag}, +0x68 pending config
Texture entry     +0x8 new level, +0xb current level, +0x18/1c/20 size per level,
                  +0x24/28/2c handle per level
Handles           0xAC91xxxx small pool, 0xAC92xxxx / 0xAC94xxxx large pool
```

## 9. Logs

- `plugins\TextureStreamer_loader.log`: Core module, loadlib result.
- `TextureStreamer.log` beside `mgsvtpp.exe`, previous run in `TextureStreamer.prev.log`.
- `TextureStreamer_boot.log` beside `mgsvtpp.exe`, only with `debugLog = true`.

With `debugLog = false` (default) the log has settings, build and address set, hook and patch results, VRAM,
the applied budget and any crash report. With `debugLog = true` it adds:

- `[Usage]` every 5 s: update state, storage, cache budget, degrade flag, small/large pool free
  (total - (allocated blocks + 1) * block size, the same check AllocBlock uses), and the two 4880-entry list counts.
  `[Usage] STALL` when an update takes over 50 ms or two Presents are more than 250 ms apart.
- `[Blocks]` after each `[Usage]`:
  - `shown lv0/1/2`: `+0x30a74/78/7c`, bytes by level shown (`0x14021F9A0`, `0x140222010`). Not pool usage.
  - `allocated lvN count (MB)`: `+0x3efa0..b4`. Level 0 includes blocks moved to the large pool.
  - `type3`: `+0x3efc4/c8`. Stays 0 with the fallback.
  - `full` / `gc wait`: AllocBlock `0xFFFFFFFF` / `0xFFFFFFFE` per level since start, counted in the hook before any retry.
  - `small->large N (M MB, failed F)`: fallback count, total MB requested (not reduced on free), large pool refusals.
- `[Storage] Small pool full, N KB placed in the large pool` for the first 5 fallbacks, then every 500th.
- Boot log: tick, thread id, step from `InitThread settings loaded` on, the first call of each hook, the DXGI steps.
  The last line before it stops is where a crash happened.

To isolate a crash, turn off one at a time: `hookPresent`, `hookUpdate`, `hookGetAvail`, `hookRequestConfig`,
`patchDispatch`, `patchUpgrade`, `smallPoolFallback`. `hookUpdate` also needs `hookRequestConfig`.

Expected `TextureStreamer.log`:

1. `[DLL] InitThread started. TEXTURESTREAMER,V0_16_20260924`
2. `[Settings] Loaded ...`, then one line with every setting
3. `[AddressSet] mgsvtpp.exe TimeDateStamp 0x6A4CB898 SizeOfImage 0x...`, `Selected EN 1.0.15.4 address set.`
4. `[CRASH] Unhandled-exception crash logger installed.`
5. `[Vram] Present pattern @0x14024CEFC -> gn::swapchain::Present 0x1419F5990`, `[Hook] Present: OK`
6. For each of the four hooks in 4.2: `@0x... bytes [...] matches`, `: OK`
7. `[Patch] Update budget clamps: applied`, `[Patch] doListupChangegraders upgrade per frame: applied`, `[Streamer] Upgrade per frame: 32 (was 16)`
8. `[DLL] MH_ApplyQueued -> 0`, `[DLL] InitThread done.`

Then, in game (9 and 10 in either order):

9. `[Vram] Adapter '...' dedicated=... budget=... using=... MB`, `[Vram] Resolved on first Present: N MB, reported to game M MB`
10. `[Streamer] First update: streamer=... tsm=... (storage created before plugin load)`
11. `[Streamer] Requested budget N MB (was M MB) -> pending`
12. `[Streamer] Applied config ... MB: vramSize ..., clampedStore ..., storage ..., cacheBudget ..., degradeFlag ...`.
    The first is logged on the first update, so it can show the game's own value before the request lands.

Reading it:

- `Applied config N MB` with the requested N: the reconfigure landed. `degradeFlag` 2 is normal, 1 is degrade mode (cache budget 0).
- Line 11 but no `Applied config N MB`: the request is pending. Either the gate in 4.3 is not met, or the storage allocation failed and fell back.
- `Budget already N MB, no request`: the game's config was already at or above VRAM.
- `MISMATCH` or `SKIPPED`: nothing was written at that site.
- `WARNING: exe is not the target 1.0.15.4 EN build`: different TimeDateStamp. Only the hooks and patches that say `matches` were applied.
- No `[Vram] Resolved`: the Present hook never ran.
- `loadlib failed` in the loader log while `TextureStreamer.log` shows the hooks: the DLL is loaded and working (everything runs from DllMain). Seen once, cause unknown.
