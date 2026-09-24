# TextureStreamer: how it works

24/09/2026, revision V014. Target: MGSV TPP retail 1.0.15.4 EN, `mgsvtpp.exe` SHA256
`085c2f82d1c963c40b3d2d55786661dfee2b18cbbf388a710c00fa76c5e9bb45`.

Every address below is a retail 1.0.15.4 EN address (preferred base `0x140000000`). Labels:

- **VERIFIED**: read in Ghidra (project `MGSV`, program `mgsvtpp.exe`) or the dev decomp. Source given.
- **SUSPECTED**: consistent with the code but not proven at runtime.
- Names in quotes come from the old mgsv_mod source or the dev decomp. Retail functions are unnamed (`FUN_`).

## 1. What the mod does

MGSV sizes its texture streaming budget from a VRAM estimate. It then clamps that to 3584 MB and to whatever
texture budget the game has requested, and forces a low-quality "degrade" mode below 800 MB. It also queues at
most 16 texture upgrades per frame.

The mod:

1. reads the real VRAM of the adapter the game renders on,
2. asks the streamer to reconfigure to that budget, using the game's own reconfigure path,
3. removes the 3584 MB cap and the two forced-degrade conditions,
4. raises the upgrades-per-frame limit from 16 to `upgradePerFrame` (default 32).

It does not touch handle counts or the storage multiplier. Section 6 explains why.

## 2. Load chain

1. IH loads `mod\modules\TextureStreamer_Core.lua` as an external module.
2. The Core module calls `package.loadlib("<game>\plugins\TextureStreamer.dll", "luaopen_TextureStreamer")`, then calls the result.
   It logs to `plugins\TextureStreamer_loader.log` and `ih_log.txt`.
3. `DllMain` pins the DLL (`GET_MODULE_HANDLE_EX_FLAG_PIN`) so it is never unloaded: in V008 Lua freed
   the loadlib handle at shutdown, which would unmap the hook code while game threads can still run it.
   It then starts `InitThread`. `luaopen_TextureStreamer` does nothing except exist for loadlib.
4. `InitThread`:
   - opens `TextureStreamer.log` beside `mgsvtpp.exe` (the previous run is kept as `TextureStreamer.prev.log`),
   - reads `plugins\TextureStreamer.lua` as literal `key = value` lines (the file is never run),
   - hashes `mgsvtpp.exe` with SHA256. Only an exact match selects the address set. Any other exe installs nothing,
   - installs the Present hook, then the streamer hooks and patches. MinHook hooks are queued and applied together.

The Core module rewrites its whole loader log with `"w"` for each line, as `InfCore.WriteLog` does, and
never calls `file:flush`. VERIFIED in Ghidra: the game's `fopen` (`0x141A62B80`) only honours `"w"`; any
other mode, `"a"` included, opens read-only, so appends are silently lost. VERIFIED in Ghidra: the game's Lua `file:flush` (`0x141A30930`) calls the C
runtime `fflush` directly on the handle, but `io.open` returns the game's own stream object (opened
through `0x141A62B80`), not a C `FILE`. V005 to V007 flushed that handle, and the next `file:write`
crashed at `0x141A62F26` (`CALL [RAX+0x10]` on the stream's vtable, Event Viewer fault offset
`0x1a62f26`).

IH runs modules at about 5.6 s into startup (V002 `ih_log.txt`). By then the streamer has already been built,
so everything here works on the running streamer, not at creation.

## 3. Safety rules in the code

- **Hooks:** every hook target is compared to the 16 bytes read from Ghidra before MinHook touches it. If they differ, the hook is skipped and the log shows both.
- **Byte patches:** every byte patch has the original bytes it expects. A group of patches is written all together or not at all.
- **Threads suspended during patches:** other threads are suspended while patch bytes are written. If any thread is stopped inside a patch site, the group is skipped.
- **Only raise, never lower:** the budget the mod sends is only ever raised, never lowered.

## 4. What it attaches to

### 4.1 Present wrapper (source of VRAM)

Found by pattern, not by fixed address. The pattern is from IHHook (0x-FADED, commit `0e282a0`, 21/09/2026).

| Address | Bytes | Instruction | Meaning |
|---|---|---|---|
| `0x14024CEFC` | `E8 8F 8A 7A 01 85 C0 75 12 38 43 69` | `CALL 0x1419F5990; TEST EAX,EAX; JNZ +0x12; CMP [RBX+0x69],AL` | Pattern `E8 ? ? ? ? 85 C0 75 12 38 43 69`, unique match. |
| `0x1419F5990` | `48 8B 49 18` | `MOV RCX,[RCX+0x18]` | Loads the `IDXGISwapChain*` from the wrapper's arg + 0x18. |
| `0x1419F5994` | `45 33 C0` | `XOR R8D,R8D` | Flags = 0. |
| `0x1419F5997` | `48 8B 01` | `MOV RAX,[RCX]` | Swapchain vtable. |
| `0x1419F599A` | `48 FF 60 40` | `JMP [RAX+0x40]` | Vtable slot 8, `IDXGISwapChain::Present`. |

VERIFIED (Ghidra). On the first call the hook takes the swapchain, then:
`GetDevice(IDXGIDevice)` → `GetAdapter` → `DXGI_ADAPTER_DESC.DedicatedVideoMemory` and
`IDXGIAdapter3::QueryVideoMemoryInfo(LOCAL).Budget`. It keeps the larger of the two. The game is told
`min(that, 0xFFFFFFFF)`, or `0xFFFFFFFF` with `lockVramMax = true`. No `CreateDXGIFactory1` is called.

### 4.2 Hooks on the streamer

| Hook | Address | First 16 bytes (checked) | What the game function does | What the hook does |
|---|---|---|---|---|
| "RequestTextureStorageConfiguration" | `0x1402A91D0` (`FUN_1402a91d0`) | `8B 02 39 41 60 75 0D 0F B7 42 04 66 39 41 64 75` | `(tsm, config*)`. If `config` differs from the applied config at `tsm+0x60`, copies the 8-byte config to `tsm+0x68` (pending) and returns 1. | Copies the caller's config, raises its size to the VRAM value if lower, passes the copy on. The caller's struct is not modified. |
| "GetAvailableStorageMemorySize" | `0x1402A8F60` (`FUN_1402a8f60`) | `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 E8` | Adapter dedicated VRAM minus 10% (12% if the adapter name contains Radeon/AMD). Floor 512 MB. | Returns the VRAM value once known, the game's own value before that. |
| Streamer per-frame update ("dispatch") | `0x14021A460` (`FUN_14021a460`) | `40 55 56 57 41 54 41 55 41 56 41 57 48 8B EC 48` | Runs every frame with `this = DgTextureStreamer`. Drives the streamer state machine, including state 5 (reconfigure, below). | Keeps the streamer pointer. Once VRAM is known, sends one budget request through the original RequestTextureStorageConfiguration. After the update, logs the budget whenever the applied config changes. |

All VERIFIED (Ghidra bytes and decompile). `FUN_1402a91d0` disassembly:

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

### 4.3 Reconfigure path used (state 5 in `FUN_14021a460`)

VERIFIED (Ghidra decompile, `0x14021A74F`..`0x14021A87C`):

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
old value and the log shows no "Applied config" change.

## 5. Byte patches

All original bytes are VERIFIED in Ghidra and checked again at runtime before writing.

### 5.1 "Update budget clamps" (setting `patchDispatch`, one group)

| Address | Original | Instruction | Patched | Effect |
|---|---|---|---|---|
| `0x14021A772` | `BB 00 00 00 E0` | `MOV EBX,0xE0000000` | `BB FF FF FF FF` | Cap becomes 4095 MB. The `CMP RAX,RBX; CMOVC EBX,EAX` that follows keeps working, so a 64-bit avail above 4 GB saturates instead of wrapping. |
| `0x14021A797` | `44 0F 42 F8` | `CMOVC R15D,EAX` | `90 90 90 90` | No degrade when the budget is under 800 MB (`CMP EBX,0x32000000` above it). |
| `0x14021A7A0` | `74 10` | `JZ 0x14021A7B2` | `EB 10` | Always skips `MOVZX R15D,AL`, the forced degrade when the pending config's flag word is set. |

Context around the group, from Ghidra (`0x14021A76D`..`0x14021A7B2`):

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
cannot happen, so V011 leaves it alone.

### 5.2 Upgrades per frame ("doListupChangegraders", always on)

| Address | Original | Instruction | Patched | Effect |
|---|---|---|---|---|
| `0x1402216E0` | `83 BB B8 00 00 00 10` | `CMP dword [RBX+0xB8],0x10` | `83 BB B8 00 00 00 20` (value = `upgradePerFrame`, 16..127) | Upgrade queue limit per frame. The next instruction is `JNC 0x14022178C` (`0F 83 9F 00 00 00`). |

VERIFIED (dev decomp `doListupChangegraders` @ dev-exe `0x1401ff000`): the count at `+0xB8` indexes a
64-entry `short` array at `+0x38` (`0x38 + 64*2 = 0xB8`), and each insert also checks `count == 0x40`.
Any value up to 127 therefore stops at 64 and cannot overflow the array.

The operand is an imm8, so 127 is the maximum. Old versions wrote a 10-byte imm32 form above 127, which
overwrote that `JNC`.

## 6. The small storage pool and the V012 fallback (24/09/2026)

The storage manager has two block pools: small at `tsm+0x40` and large at `tsm+0x48`. The runtime
reconfigure only resizes the large one (dev decomp has `ReinitializeLarge`, no small equivalent).
The small pool is sized once at init from the constant `0x10680000` (262.5 MB), which "Reserve" writes
to `+0x30a58` and passes to Create, before IH loads.

V010 log with a texture-heavy file (4070, budget raised to 4095 MB): the large pool never ran short,
the small pool went from 231 to 0 MB free as type 0 use climbed to 230 MB, then the update state sat
at 0 and every counter froze (the 5 s vehicle halt and the endless iDroid load).

V011 baseline with `raiseBudget`, `patchDispatch` and `patchUpgrade` all false: the same freeze. The
small pool hits 0/262 MB, t0 sits at 230 MB, state 0, while the large pool has 487/586 MB free.
VERIFIED from the log: vanilla behaviour, not caused by the mod.

### 6.1 Allocation path (retail 1.0.15.4 EN, read in Ghidra, nothing changed in the project)

| Address | What it is |
|---|---|
| `0x1402A6D40` | `TextureStorageManager::AllocBlock(tsm, size, type)`. Type 0: small pool, returns `handle \| 0xAC910000`. Type 1: large pool from the top, `\| 0xAC920000`. Type 2: large pool, `\| 0xAC920000`. Other: large, `\| 0xAC940000`. Fails with `0xFFFFFFFF` (no room), or `0xFFFFFFFE` with a GC flag in `tsm+0x2C` when enough total space is free but fragmented. |
| `0x1402A6A00` | block allocator used by AllocBlock (`+0x8` total, `+0xC` block size, `+0x18` allocated blocks) |
| `0x14021AB30` | only caller. Stores the handle in the texture's slot for its type (`+0x24 + type*4`), bumps type counters `streamer+0x3efa0..`, on failure sets `0x20000000` in `streamer+0x26324` and retries later |
| `0x140219D80` | free. Takes the handle from the slot, size by handle bits, block type from `0x1402A7970`, frees with `0x1402A7680`, takes the counters off by the slot's type |
| `0x1402A79F0` / `0x1402A7A20` | block address / size: `(handle & 0xF0000) == 0x10000` means small pool, anything else large |
| `0x1402A7680` / `0x1402A7E20` | free block / register block id, pool picked by handle bits |
| `0x1402A7970` | block type: `0x20000` -> 2, `0x40000` -> 3, else 0. Type 3 goes to separate counters `+0x3efc4/+0x3efc8` |
| `0x1402A7450` / `0x1402A6EC0` | compaction of one pool, moves blocks and notifies by the registered id |

### 6.2 The fallback (setting `smallPoolFallback`)

Hook on `0x1402A6D40`, expected bytes `48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 41`. When a type 0
call returns `0xFFFFFFFF`, it calls the original again with type 2 and returns that handle. Every
function that reads the handle picks the pool from its bits, so the texture lives in the large pool.
Type 2 tags it `0xAC92`, which reads back as block type 2, not 3, so freeing it takes the type 0
counters back down (a `0xAC94` handle would leak them into the type 3 counters).

`0xFFFFFFFE` (fragmented, GC pending) is left alone so the game's own compaction runs.

The AllocBlock type is the texture's quality level slot (`+0x24 + level*4` holds its handle), so level 0,
the lowest mip every streamed texture keeps, is what fills the small pool.

V012 in game (texture-heavy file, 4070): the small pool hit 0 MB free, 163 allocations went to the
large pool with 0 failures, the streamer kept running, no halts and no low res fallback.

Still to confirm in game (SUSPECTED fine, not traced): the compaction move callback and anything that
compares t0 use against the small pool size. t0 can now go past 262 MB.

Log: `[Storage] Small pool full, N KB placed in the large pool` for the first 5, then every 500th,
and `small->large N (failed N)` on every `[Usage]` line.

## 7. Not patched, and why

### 7.1 Handle cap 0x1310 (4880)

VERIFIED (dev decomp `Gr_dx11_win64\DgTextureStreamer.cpp`, 2015 prototype, plus retail at the two sites
below). 4880 is compiled into the `DgTextureStreamer` object:

- `sequence_list<DgIncrementalTextureResource::Proxy*, 4880>` and `sequence_allocator<node, 4881>`: fixed-size templates stored inside the object (constructor dev-exe `0x1401f49d0`).
- An 8-byte array at `+0x69178` with 0x1310 entries, followed directly by its count at `+0x729f8` (`0x69178 + 0x1310*8 = 0x729f8`).
- Handle checks `handle < 0x1310` in `GetProxy`, `IsLoaded`, `Dispose` and `Request/UnrequestTextureDetailByDgTexture`. 0x1310 is the invalid-handle value.

Retail sites the old mod patched:

| Address | Bytes | Context | Result of raising it |
|---|---|---|---|
| `0x14022159E` ("doDegrade") | `3D 10 13 00 00` | `MOV EAX,[R15+0x729f8]; CMP EAX,0x1310; JZ; MOV [R15+RAX*8+0x69178],RBP; INC [R15+0x729f8]` | Entry 0x1310 lands on the count itself, then writes run past the object's own fields (`+0x72b00`, `+0x72b10`, `+0x72b18` are read every frame). Memory corruption. |
| `0x140220F63`, `0x140220FFD` ("allocateHandlableProxy") | `81 FA 10 13 00 00` | Proxy index `== 0x1310` → free it and retry | Only moves which index is skipped. Index 0x1310 then gets used as a handle everywhere else treats as invalid. No capacity gained. |

"DoUpgrade2", `CancelUpgrade`, `DoLoadEntry`, `DoUpgradeEntry` and `PostCreation` use the same
`+0x69178` / `+0x729f8` bound as doDegrade. Retail has 10 `CMP EAX,0x1310` and 6 `CMP EDX,0x1310` sites.
None can be raised without moving arrays that are compiled into the object's layout, so the handle
multiplier is gone for good. Loading earlier would not change this.

### 7.2 Create-time sizing

VERIFIED (Ghidra): "Reserve" is `FUN_14021dee0` (first bytes `4C 8B DC 57 48 83 EC 50 49 C7 43 C8 FE FF FF FF`).
It runs once, from the streamer's init (`FUN_14021d2f0`). It:

- clamps storage at `0x14021DF74`: `B9 00 00 00 E0 48 3B C1 0F 42 C8 BA 00 00 40 38 3B CA 0F 47 CA`, i.e. min(avail, 3584MB, 900MB),
- builds the Create parameters `{total, 0x10680000, storage, 0x1310, 0x8000}`,
- calls "Create" `0x1402A8790`, which is a `JMP 0x14459D000` into the anti-tamper region (`E9 6B 48 2F 04`).

SUSPECTED: this has all finished before IH loads the plugin. The first "First update" log line settles it:
a non-null `tsm` means the storage manager already existed. V011 hooks none of these. Its budget comes
through the reconfigure path in 4.3.

## 8. What to check in the logs

**Release logging (V014).** `debugLog` in `plugins\TextureStreamer.lua` is false by default. Off, `TextureStreamer.log` still has the settings, build and address set, every hook and patch result, the VRAM line, the applied budget and any crash report. `[Usage]`, `[Blocks]`, `STALL` and `[Storage]` lines and `TextureStreamer_boot.log` need `debugLog = true`. With it on, the boot log starts at `InitThread settings loaded`; the DllMain steps are no longer written, since settings are not read yet at that point.

**Stall and usage lines (V013, debugLog only).** `[Usage] periodic` is logged every 5 s from the streamer update.
`[Usage] STALL streamer update took N ms` is logged when one update takes over 50 ms, and
`[Usage] STALL frame gap N ms` when two Presents are more than 250 ms apart, with whether the
streamer update was running at the time. `[Usage]` shows update state, storage, cache budget,
degrade flag, small/large pool free (allocator layout `+0x8` total, `+0xC` block size, `+0x18`
allocated blocks, matches the retail free check in `0x1402A6D40`), and the two 4880-entry list counts.

Each `[Usage]` line is followed by `[Blocks]`:

- `shown lv0/1/2`: `streamer+0x30a74/78/7c`, bytes of textures by the level they currently show
  (updated in `0x14021F9A0` and `0x140222010`). V012 and earlier logged these as "used t0/t1/t2".
  They are not pool usage. A texture showing level 2 can still hold its level 0 block.
- `allocated lvN count (MB)`: `streamer+0x3efa0/a4/a8` and `+0x3efac/b0/b4`, blocks and bytes held in
  each level slot (added in `0x14021AB30`, removed in `0x140219D80`). Level 0 includes blocks the
  fallback put in the large pool.
- `type3`: `+0x3efc4/c8`, taken off when a freed block reads back as type 3 (`0xAC94` handle).
- `full` and `gc wait`: AllocBlock returning `0xFFFFFFFF` / `0xFFFFFFFE` per level, counted in the hook
  since start, before any retry. The game's own counters at `+0x3efb8..c0` are cleared every update.
- `small->large N (M MB, failed F)`: fallback count, total MB requested (not reduced on free), and
  retries the large pool refused.

The DLL log is `TextureStreamer.log` beside `mgsvtpp.exe`. The loader log is `plugins\TextureStreamer_loader.log`.
`TextureStreamer_boot.log` beside the exe records steps with raw Win32 writes (tick, thread id, step):
the DllMain steps (including `DllMain pinned`), the luaopen steps, `InitThread entered`,
`InitThread log opened` and `InitThread done`. Each hook also writes its first call (`hkPresent first call`,
`hkUpdate first call`, `hkGetAvail first call`, `hkRequestConfig first call`, the DXGI steps in the
Present hook, and `... original returned`). The last line before the log stops is where a crash happened.

To isolate a crash, turn hooks and patches off one at a time in `plugins\TextureStreamer.lua`:
`hookPresent`, `hookUpdate`, `hookGetAvail`, `hookRequestConfig`, `patchDispatch`, `patchUpgrade`.
`hookUpdate` also needs `hookRequestConfig`.

`TextureStreamer.log`, in order:

1. `[DLL] InitThread started. TEXTURESTREAMER,V0_14_20260924`
2. `[Settings] debugWindow=0 patchDispatch=1 lockVramMax=0 upgradePerFrame=32`
3. `[AddressSet] mgsvtpp.exe SHA256 085c2f82...`, then `Selected EN 1.0.15.4 address set.`
4. `[Vram] Present pattern @0x14024CEFC -> gn::swapchain::Present 0x1419F5990`, `[Hook] Present: OK`
5. For each of the three streamer hooks: `@0x... bytes [...] matches`, then `: OK`
6. `[Patch] Update budget clamps: applied`, `[Patch] doListupChangegraders upgrade per frame: applied`, `[Streamer] Upgrade per frame: 32 (was 16)`
7. `[DLL] MH_ApplyQueued -> 0`

Then, once the game is running (8 and 9 can come in either order):

8. `[Vram] Adapter '...' dedicated=... budget=... using=... MB`, `[Vram] Resolved on first Present: N MB`
9. `[Streamer] First update: streamer=... tsm=... (storage created before plugin load)`
10. `[Streamer] Requested budget N MB (was M MB) -> pending`
11. `[Streamer] Applied config ... MB: vramSize ..., clampedStore ..., storage ..., cacheBudget ..., degradeFlag ...`.
    The first one is logged on the first update, so it can show the game's own M MB before the request goes through.

What it means:

- **An `Applied config N MB` line with the requested N:** the reconfigure landed. `degradeFlag` 2 is normal mode with a cache budget. 1 is degrade mode (cache budget 0), which the patches in 5.1 should prevent.
- **Line 10 but never an `Applied config N MB`:** the request is pending but the game hasn't applied it. Either the gate in 4.3 (`FUN_1402a79c0() == FUN_1402a79a0(tsm)`) is not being met, or the storage allocation failed and fell back.
- **`Budget already N MB, no request`:** the game's own config was already at or above the VRAM value.
- **Any `MISMATCH` or `SKIPPED`:** nothing was written at that site. Send the log.
- **No `[Vram] Resolved` line:** the Present hook never ran.

Settings are in `plugins\TextureStreamer.lua`. Restart the game after editing.
