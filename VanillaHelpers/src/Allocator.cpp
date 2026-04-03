// This file is part of VanillaHelpers.
//
// VanillaHelpers is free software: you can redistribute it and/or modify it under the terms of the
// GNU Lesser General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// VanillaHelpers is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
// without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lessed General Public License along with
// VanillaHelpers. If not, see <https://www.gnu.org/licenses/>.

#include "Allocator.h"
#include "Common.h"
#include "Game.h"
#include "MinHook.h"
#include "Offsets.h"
#include "TexBridge.h"

#include <stdint.h>

namespace Allocator {

static Game::SMemAllocInternal_t SMemAllocInternal_o = nullptr;
static Game::SMemReallocInternal_t SMemReallocInternal_o = nullptr;
static void *SMemReallocInternal_Decode_o = nullptr;
static void *SMemFreeInternal_Decode_o = nullptr;
static void *SMemGetSizeInternal_Decode_o = nullptr;

static constexpr uint8_t MAX_SMALL_CLASS = 0x19; // 32 MiB
static constexpr uint32_t REGION_SIZE = 1U << MAX_SMALL_CLASS;
static constexpr uint32_t REGION_SIZE_NEG = 0U - REGION_SIZE;

// Store bit24 of size into header1 bit5.
// This preserves the extra size bit for 32 MiB small blocks.
static inline void UpdateSmallSizeBit(void *hdr, uint32_t size, uint32_t sizeClass) {
    if (!hdr || sizeClass > MAX_SMALL_CLASS)
        return;

    auto *h = reinterpret_cast<uint32_t *>(hdr);
    uint32_t v = h[1] & ~0x20;
    v |= ((size >> 24) & 1U) << 5;
    h[1] = v;
}

static void *__fastcall SMemAllocInternal_h(void *thisptr, void * /*edx*/, uint32_t sizeClass,
                                            uint32_t size, uint32_t commit) {
    void *p = SMemAllocInternal_o(thisptr, sizeClass, size, commit);
    TexBridge::LogFocusedMain0573Backend(thisptr, sizeClass, size, commit, p);
    if (p && commit)
        UpdateSmallSizeBit(p, size, sizeClass);
    return p;
}

static void *__fastcall SMemReallocInternal_h(void *thisptr, void * /*edx*/, void *blk,
                                              uint32_t sizeClass, uint32_t size) {
    void *p = SMemReallocInternal_o(thisptr, blk, sizeClass, size);
    UpdateSmallSizeBit(p, size, sizeClass);
    return p;
}

// The allocator decodes size by size = (hdr0 >> 8) for small blocks.
// We OR the saved bit24 (header1 bit5) back into the size.
static __declspec(naked) void SMemReallocInternal_Decode_h() {
    __asm {
        test    byte ptr [edi + 4], 0x20
        jz      skip
        or      esi, 0x1000000
    skip:
        jmp     dword ptr [SMemReallocInternal_Decode_o]
    }
}

static __declspec(naked) void SMemFreeInternal_Decode_h() {
    __asm {
        test    byte ptr [ebx + 4], 0x20
        jz      skip
        or      edx, 0x1000000
    skip:
        jmp     dword ptr [SMemFreeInternal_Decode_o]
    }
}

static __declspec(naked) void SMemGetSizeInternal_Decode_h() {
    __asm {
        test    byte ptr [ebx + 4], 0x20
        jz      skip
        or      esi, 0x1000000
    skip:
        jmp     dword ptr [SMemGetSizeInternal_Decode_o]
    }
}

static void PatchBiggerMemoryRegion() {
    // 32 MiB regions (128 regions -> 4 GiB virtual address space)
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_RESERVE_PUSH_SIZE + 1),
                       &REGION_SIZE, sizeof(REGION_SIZE));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_RESERVE_ACCOUNT_INC + 3),
                       &REGION_SIZE, sizeof(REGION_SIZE));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_RESERVE_ACCOUNT_DEC + 3),
                       &REGION_SIZE_NEG, sizeof(REGION_SIZE_NEG));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_VALIDATE_REGION_END_LEA + 2),
                       &REGION_SIZE, sizeof(REGION_SIZE));

    // Small vs large threshold = 0x19
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_ALLOC_A + 2),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_ALLOC_B + 2),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_REALLOC_A),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_REALLOC_B),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_FREE_A + 2),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_FREE_B + 2),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_FREE_C + 2),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_FREE_D + 2),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_ISVALID + 2),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
    Common::PatchBytes(reinterpret_cast<void *>(Offsets::PATCH_SMEM_SMALL_LARGE_CMP_GETSIZE + 2),
                       &MAX_SMALL_CLASS, sizeof(MAX_SMALL_CLASS));
}

void Initialize() { PatchBiggerMemoryRegion(); }

bool InstallHooks() {
    HOOK_FUNCTION(Offsets::FUN_SMEM_ALLOC_INTERNAL, SMemAllocInternal_h, SMemAllocInternal_o);
    HOOK_FUNCTION(Offsets::FUN_SMEM_REALLOC_INTERNAL, SMemReallocInternal_h, SMemReallocInternal_o);
    HOOK_FUNCTION(Offsets::FUN_SMEM_REALLOC_INTERNAL_DECODE, SMemReallocInternal_Decode_h,
                  SMemReallocInternal_Decode_o);
    HOOK_FUNCTION(Offsets::FUN_SMEM_FREE_INTERNAL_DECODE, SMemFreeInternal_Decode_h,
                  SMemFreeInternal_Decode_o);
    HOOK_FUNCTION(Offsets::FUN_SMEM_GET_SIZE_INTERNAL_DECODE, SMemGetSizeInternal_Decode_h,
                  SMemGetSizeInternal_Decode_o);

    return true;
}

} // namespace Allocator
