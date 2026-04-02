# VanillaHelpers64
 
 VanillaHelpers64 is a helper library for Vanilla WoW 1.12 that provides additional functionality.
 
 Based on **[VanillaHelpers v1.1.2](https://github.com/isfir/VanillaHelpers)**, it adds a **64-bit texture
 streaming server** to reduce out-of-memory crashes with HD texture packs by moving heavy texture 
residency pressure out of the 32-bit WoW client.
 
 This is intended to be a stand-in replacement for VanillaHelpers.
 
 ## What is new in VanillaHelpers64
 
 VanillaHelpers64 adds a companion **64-bit TextureServer** and a WoW-side texture bridge that:
 
 - intercepts selected texture loads
 - caches decoded texture data in a 64-bit helper process
 - swaps many heavy textures from `D3DPOOL_MANAGED` to `D3DPOOL_DEFAULT`
 - keeps the original managed texture available so WoW can be restored back to it when a swapped texture 
is evicted or destroyed
 - reuploads textures on demand instead of keeping large managed system-memory copies inside WoW
 - helps reduce OOM pressure in crowded cities and heavy HD texture scenes
 
 Current implementation focus:
 
 - large DXT textures, generally `512x256` and above
 - selected large world doodads, buildings, creatures, character assets, capes, shoulders, weapons, and 
armor pieces
 - expanding support for large uncompressed textures in targeted asset groups
 - stabilizing the live texture lifecycle around reuse, eviction, and destruction while keeping the real 
swap path enabled
 
 ## Installation
 
 It is recommended to use **[VanillaFixes](https://github.com/hannesmann/vanillafixes)** to load this mod.
 
 Steps:
 
 1. Install VanillaFixes if not already installed
 2. Download latest release from [Releases](https://github.com/paokkerkir/VanillaHelpers64/releases)
 3. Copy `VanillaHelpers.dll` to your game directory
 4. Copy `TextureServer64.exe` to your game directory
 5. Add `VanillaHelpers.dll` to the `dlls.txt` file
 6. Start the game with `WoW.exe`
 
 If using a popular private server launcher, select **Ignore updates** for `VanillaHelpers.dll`.
 
 ## Features
 
 - **File Operations:** Read/write files from the game's environment
 - **Minimap Blips:** Customize unit markers on the minimap
 - **Memory Allocator:** Double the custom allocator's region size from 16 MiB to 32 MiB (128 regions), 
raising allocator capacity from 2 GiB to 4 GiB
 - **High-Resolution Textures:** Increase the maximum supported texture size to 1024x1024 (from 512x512)
 - **High-Resolution Character Skins:** Support up to 4x higher-resolution skin and armor textures
 - **Character Morph:** Change character appearances, mounts, and visible items
 - **64-bit Texture Streaming:** Use a 64-bit companion server to cache and stream selected HD textures 
with lower 32-bit WoW memory pressure
 
 ## TextureServer64
 
 `TextureServer64.exe` is the 64-bit companion process used by VanillaHelpers64 to keep heavy texture work
 and cache pressure outside the 32-bit WoW client.
 
 ### What it does
 
 - receives texture requests from `VanillaHelpers.dll`
 - decodes supported BLP/TGA texture data
 - keeps decoded texture payloads in a 64-bit LRU cache
 - exposes decoded textures to the WoW client through shared memory slots
 - serves as the backing source for re-uploading `D3DPOOL_DEFAULT` textures after eviction, path reuse, or
 cache loss
 
 ### Why it exists
 
 WoW 1.12 is a 32-bit process, so large HD texture packs can exhaust its address space even when the 
machine still has plenty of RAM available.
 
 TextureServer64 moves the heavy texture cache into a 64-bit process, where much larger caches are 
possible without pushing WoW toward OOM.
 
 ### Current behavior
 
 - targeted textures are sent to the server when intercepted by the helper
 - the server returns decoded texture payloads through shared memory
 - the helper can replace selected managed textures with `D3DPOOL_DEFAULT` textures in WoW
 - the helper tracks swapped textures across install, reuse, eviction, overwrite, and destroy paths
 - when a swapped texture is evicted or torn down, the helper attempts to restore the original managed 
binding before releasing the helper-owned default-pool texture
 - if the server has evicted a cached texture, the helper can recover by re-reading the raw asset and 
re-requesting it
 - logging in `TexBridge.log` is focused on high-signal lifecycle events such as install, track, stale 
tracking, release, and texture destroy
 
 ### Current stability status
 
 The system is actively being stabilized against WoW's internal texture reuse behavior.
 
 Recent debugging established that:
 
 - WoW can recycle `HTEXTURE` and `CGxTex` objects while old D3D texture pointers are still dangerous if 
helper cleanup timing is wrong
 - a real texture destroy/free hook is now installed on WoW's destroy path
 - destroy events now clear helper tracking and restore the managed binding before releasing helper-owned 
default textures when possible
 - this was added specifically to prevent recycled `CGxTex` objects from later dereferencing a released 
swapped texture
 
 This means the real OOM-reduction path remains enabled, but the project should still be considered **work
 in progress** until longer runtime validation confirms the remaining crash family is gone.
 
 ### Cache sizing
 
 By default, TextureServer64 auto-sizes its cache to:
 
 - **35% of physical RAM**
 - **minimum 4 GiB**
 - **maximum 32 GiB**
 
 You can still override this manually with `--cache-mb N`.
 
 ### Command line
 
 `TextureServer64.exe [--threads N] [--cache-mb N] [--visible]`
 
 - `--threads N` - worker thread count (`0` = auto)
 - `--cache-mb N` - manual cache size override in MiB
 - `--visible` - keep the console open for live server logging
 
 ### Notes
 
 - the server is primarily a texture cache and upload source, not a renderer
 - the biggest current benefit is reducing 32-bit texture residency pressure in WoW
 - performance gains are more about avoiding memory-related stalls and crashes than raw FPS increases
 - the helper writes a runtime log in the WoW folder: `TexBridge.log`
 
 ## Usage
 
 The library registers these Lua functions:
 
 - `WriteFile(filename, mode, content)`
 - `content = ReadFile(filename)`
 - `SetUnitBlip(unit [, texture [, scale]])`
 - `SetObjectTypeBlip(type [, texture [, scale]])`
 - `SetUnitDisplayID(unitToken [, displayID])`
 - `RemapDisplayID(oldDisplayID(s) [, newDisplayID])`
 - `SetUnitMountDisplayID(unitToken [, mountDisplayID])`
 - `RemapMountDisplayID(oldDisplayID(s) [, factionIndexedDisplayIDs])`
 - `SetUnitVisibleItemID(unitToken, inventorySlot [, itemID])`
 - `RemapVisibleItemID(oldItemID(s), inventorySlot [, newItemID])`
 - `displayID, nativeDisplayID, mountDisplayID = UnitDisplayInfo(unitToken)`
 - `itemDisplayID = GetItemDisplayID(itemID)`
 
 To enable higher-resolution character skins, add a text file at `VanillaHelpers/ResizeCharacterSkin.txt` 
inside your MPQ. The file should contain a single number: `2` or `4` (the scale multiplier).
 
 To keep the TextureServer64 console window visible, add an empty file named `TexBridgeVisible.txt` in the
 WoW folder.
 
 See the source code for implementation details.
 
 ## Memory Usage Note
 
 When using high-resolution textures, particularly 4x character skins, the game will use significantly 
more RAM and might run out of memory in crowded areas.
 
 VanillaHelpers64 is intended to improve this by offloading selected texture caching and residency 
handling to a 64-bit companion process, but it is still a work in progress and does not yet eliminate all 
memory pressure or crash cases.
 
 To further reduce issues, it is recommended to:
 
 - run the game on a 64-bit operating system
 - use DXVK
 - ensure the game executable is Large Address Aware (LAA) flagged to access more than 2 GiB of RAM
 
 If running on Linux, it's recommended to use a Wine build with WoW64 mode and apply the usual 
address-space patch to make more addresses available in the 32-bit address space.
