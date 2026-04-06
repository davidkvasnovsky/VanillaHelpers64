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

#pragma once

#include <cstdint>

// All 149 offsets in a single X-macro list.  This generates:
//   - extern declarations (this header)
//   - definitions with compiled-in defaults (Offsets.cpp)
//   - INI file loader lookup table (Offsets.cpp)
//
// Non-externalizable offsets embedded in inline assembly:
//   Texture.cpp:   [edi + 0x138] HTEXTURE async field, [edi + 0x155] lock byte
//   Blips.cpp:     [edi + 0xbc7660/0xbc7664] party unit GUID
//   TexBridge.cpp: 0x00866650 texture table, 0x573 size class, 0x00C51C58 allocator

#define OFFSET_LIST                                                                                                    \
    X(PATCH_MINIMAP_RENDER_PARTY_LISTING, 0x4ED6BE)                                                                    \
    X(PATCH_MINIMAP_TRACK_UPDATE_CHANGED_GATE, 0x4EB4CC)                                                               \
    X(PATCH_MINIMAP_TRACK_UPDATE_PRE_SHOW_GATE, 0x4EB54C)                                                              \
    X(PATCH_MINIMAP_TRACK_UPDATE_APPEND_TO_TOOLTIP_BUFFER, 0x4EB6F4)                                                   \
    X(PATCH_TEXTURE_SIZE_1, 0x4484C5)                                                                                  \
    X(PATCH_TEXTURE_SIZE_2, 0x4484D0)                                                                                  \
    X(PATCH_ASYNC_TEXTURE_WAIT_ATOMIC, 0x44AD6F)                                                                       \
    X(PATCH_ASYNC_TEXTURE_WAIT_ATOMIC_UNLINK, 0x44AD7F)                                                                \
    X(PATCH_ASYNC_TEXTURE_WAIT_ATOMIC_ALREADY, 0x44ADEB)                                                               \
    X(PATCH_ASYNC_BUFFER_ADDR_TEX_LOAD_IMAGE, 0x449728)                                                                \
    X(PATCH_ASYNC_BUFFER_ADDR_BLP_PATH, 0x44A441)                                                                      \
    X(PATCH_ASYNC_BUFFER_ADDR_PUMP, 0x448CA4)                                                                          \
    X(PATCH_ASYNC_BUFFER_SIZE_TEX_LOAD_IMAGE, 0x44971D)                                                                \
    X(PATCH_ASYNC_BUFFER_SIZE_BLP_PATH, 0x44A436)                                                                      \
    X(PATCH_ASYNC_BUFFER_SIZE_PUMP, 0x448C37)                                                                          \
    X(PATCH_TEXPOOL_INIT_POOL_HEAD, 0x447C40)                                                                          \
    X(PATCH_TEXPOOL_INIT_POOL_COUNT, 0x447C45)                                                                         \
    X(PATCH_TEXPOOL_CLEANUP_START_EDI, 0x447C97)                                                                       \
    X(PATCH_TEXPOOL_CLEANUP_COUNT, 0x447C9C)                                                                           \
    X(PATCH_TEXPOOL_ALLOC_PREV, 0x448531)                                                                              \
    X(PATCH_TEXPOOL_ALLOC_HEAD, 0x448538)                                                                              \
    X(PATCH_TEXPOOL_FREE_PARAM_1, 0x4487B7)                                                                            \
    X(PATCH_TEXPOOL_FREE_PARAM_2, 0x4487C1)                                                                            \
    X(PATCH_TEXPOOL_ITERA_PREV_LOAD, 0x448ECC)                                                                         \
    X(PATCH_TEXPOOL_ITERA_OUTER_CNT, 0x448ED1)                                                                         \
    X(PATCH_TEXPOOL_ITERA_INNER_CNT, 0x448EE3)                                                                         \
    X(PATCH_TEXPOOL_ITERB_PREV_LOAD, 0x449019)                                                                         \
    X(PATCH_TEXPOOL_ITERB_OUTER_CNT, 0x449021)                                                                         \
    X(PATCH_TEXPOOL_ITERB_INNER_CMP, 0x449097)                                                                         \
    X(PATCH_TEXPOOL_ITERB_ROW_STEP, 0x4490A5)                                                                          \
    X(PATCH_TEXPOOL_STAT_PREV_BASE, 0x44B527)                                                                          \
    X(PATCH_TEXPOOL_STAT_PARAM_BASE, 0x44B597)                                                                         \
    X(PATCH_TEXPOOL_STAT_OUTER_CNT, 0x44B520)                                                                          \
    X(PATCH_TEXPOOL_STAT_TOTAL_END, 0x44B5B7)                                                                          \
    X(PATCH_MIPBITS_ALLOC_PUSH, 0x448BDC)                                                                              \
    X(PATCH_MIPBITS_ALLOC_MOV_EDX, 0x448BE1)                                                                           \
    X(PATCH_CHAR_ATLAS_GATE_DIM_CMP, 0x4778D2)                                                                         \
    X(PATCH_CHAR_ATLAS_CREATE_SIZE_PUSH, 0x47792F)                                                                     \
    X(PATCH_CHAR_ATLAS_CREATE_SIZE_MOV_EDX, 0x477934)                                                                  \
    X(PATCH_CHAR_COMPOS_COPY_STRIDE, 0x477C9E)                                                                         \
    X(PATCH_CHAR_COMPOS_MASKED_STRIDE, 0x477DCE)                                                                       \
    X(PATCH_CHAR_COMPOS_BLEND_STRIDE, 0x477F40)                                                                        \
    X(PATCH_CHAR_ATLAS_CLEAR_COLOR_R, 0x477A0F)                                                                        \
    X(PATCH_CHAR_ATLAS_CLEAR_COLOR_G, 0x477A14)                                                                        \
    X(PATCH_CHAR_ATLAS_TABLE_SIDE_PUSH, 0x4760A7)                                                                      \
    X(PATCH_CHAR_ATLAS_TABLE_SIDE_MOV_EDX, 0x4760AC)                                                                   \
    X(PATCH_TEX_DIM_CLAMP_W, 0x44AFB8)                                                                                 \
    X(PATCH_TEX_DIM_CLAMP_H, 0x44AFC4)                                                                                 \
    X(PATCH_CHAR_ATLAS_FREE_SIDE_PUSH_W, 0x476791)                                                                     \
    X(PATCH_CHAR_ATLAS_FREE_SIDE_PUSH_H, 0x476796)                                                                     \
    X(PATCH_SMEM_RESERVE_PUSH_SIZE, 0x64505B)                                                                          \
    X(PATCH_SMEM_RESERVE_ACCOUNT_INC, 0x645073)                                                                        \
    X(PATCH_SMEM_RESERVE_ACCOUNT_DEC, 0x6450B3)                                                                        \
    X(PATCH_SMEM_VALIDATE_REGION_END_LEA, 0x6450CD)                                                                    \
    X(PATCH_SMEM_SMALL_LARGE_CMP_ALLOC_A, 0x645918)                                                                    \
    X(PATCH_SMEM_SMALL_LARGE_CMP_ALLOC_B, 0x645961)                                                                    \
    X(PATCH_SMEM_SMALL_LARGE_CMP_REALLOC_A, 0x645C33)                                                                  \
    X(PATCH_SMEM_SMALL_LARGE_CMP_REALLOC_B, 0x645C45)                                                                  \
    X(PATCH_SMEM_SMALL_LARGE_CMP_FREE_A, 0x645EC7)                                                                     \
    X(PATCH_SMEM_SMALL_LARGE_CMP_FREE_B, 0x645F0A)                                                                     \
    X(PATCH_SMEM_SMALL_LARGE_CMP_FREE_C, 0x645F66)                                                                     \
    X(PATCH_SMEM_SMALL_LARGE_CMP_FREE_D, 0x645FAD)                                                                     \
    X(PATCH_SMEM_SMALL_LARGE_CMP_ISVALID, 0x6461E3)                                                                    \
    X(PATCH_SMEM_SMALL_LARGE_CMP_GETSIZE, 0x645838)                                                                    \
                                                                                                                       \
    X(FUN_CGX_TEX_FLAGS_CONSTRUCTOR, 0x58A980)                                                                         \
    X(FUN_CLNT_OBJ_MGR_ENUM_VISIBLE_OBJECTS, 0x468380)                                                                 \
    X(FUN_CLNT_OBJ_MGR_OBJECT_PTR, 0x468460)                                                                           \
    X(FUN_CSTATUS_DESTRUCTOR, 0x419E30)                                                                                \
    X(FUN_GET_GUID_FROM_NAME, 0x515970)                                                                                \
    X(FUN_GX_PRIM_DRAW_ELEMENTS, 0x58A2E0)                                                                             \
    X(FUN_GX_PRIM_LOCK_VERTEX_PTRS, 0x58A2A0)                                                                          \
    X(FUN_GX_PRIM_UNLOCK_VERTEX_PTRS, 0x58A340)                                                                        \
    X(FUN_GX_RS_SET, 0x589E80)                                                                                         \
    X(FUN_GX_TEX_OWNER_UPDATE, 0x594B90)                                                                               \
    X(FUN_LOAD_SCRIPT_FUNCTIONS, 0x490250)                                                                             \
    X(FUN_OBJECT_ENUM_PROC, 0x4EAA90)                                                                                  \
    X(FUN_REGISTER_LUA_FUNCTION, 0x704120)                                                                             \
    X(FUN_RENDER_OBJECT_BLIP, 0x4EBC00)                                                                                \
    X(FUN_TEXTURE_DESTROY, 0x448670)                                                                                   \
    X(FUN_TEXTURE_CREATE, 0x449D90)                                                                                    \
    X(FUN_TEXTURE_GET_GX_TEX, 0x44ACF0)                                                                                \
    X(FUN_TEXTURE_ALLOC_MAIN, 0x645640)                                                                                \
    X(FUN_TEXTURE_ALLOC_BACKEND, 0x645910)                                                                             \
    X(FUN_WORLD_POS_TO_MINIMAP_FRAME_COORDS, 0x4EAA30)                                                                 \
    X(FUN_S_STR_PACK, 0x64A760)                                                                                        \
    X(FUN_INVALID_FUNCTION_PTR_CHECK, 0x42A320)                                                                        \
    X(FUN_CWORLD_QUERY_MAP_OBJ_IDS, 0x670540)                                                                          \
    X(FUN_CLNT_OBJ_MGR_GET_ACTIVE_PLAYER, 0x468550)                                                                    \
    X(FUN_CGUNIT_C_CAN_ASSIST, 0x6066F0)                                                                               \
    X(FUN_FRAME_SCRIPT_INITIALIZE, 0x7039E0)                                                                           \
    X(FUN_FRAME_SCRIPT_EXECUTE, 0x704CD0)                                                                              \
    X(FUN_CVAR_REGISTER, 0x63DB90)                                                                                     \
    X(FUN_INITIALIZE_GLOBAL, 0x402350)                                                                                 \
    X(FUN_CGUNIT_C_CREATE_UNIT_MOUNT, 0x607A00)                                                                        \
    X(FUN_CGUNIT_C_REFRESH_MOUNT, 0x5FFA50)                                                                            \
    X(FUN_TEXTURE_BLIT_COPY, 0x477C80)                                                                                 \
    X(FUN_TEXTURE_BLIT_MASKED, 0x477DB0)                                                                               \
    X(FUN_TEXTURE_BLIT_BLEND, 0x477F20)                                                                                \
    X(FUN_SFILE_OPEN, 0x6477A0)                                                                                        \
    X(FUN_SFILE_READ, 0x648460)                                                                                        \
    X(FUN_SFILE_CLOSE, 0x648730)                                                                                       \
    X(FUN_CGOBJECT_C_SET_BLOCK, 0x6142E0)                                                                              \
    X(FUN_CGUNIT_C_UPDATE_DISPLAY_INFO, 0x60ABE0)                                                                      \
    X(FUN_CGUNIT_C_DESTRUCTOR, 0x5FB5E0)                                                                               \
    X(FUN_CGPLAYER_C_APPLY_INV_COMPONENT_TO_MODEL, 0x5ED700)                                                           \
    X(FUN_CLEAR_APPEARANCE_SLOT, 0x478BD0)                                                                             \
    X(FUN_CGGAMEUI_SHUTDOWN, 0x490BD0)                                                                                 \
    X(FUN_DBCACHE_ITEM_STATS_C_GET_RECORD, 0x55BA30)                                                                   \
    X(FUN_SMEM_ALLOC_INTERNAL, 0x645910)                                                                               \
    X(FUN_SMEM_REALLOC_INTERNAL, 0x645C10)                                                                             \
    X(FUN_SMEM_FREE_PAYLOAD, 0x645790)                                                                                 \
    X(FUN_SMEM_FREE_WRAPPER, 0x646430)                                                                                 \
    X(FUN_SMEM_REALLOC_INTERNAL_DECODE, 0x645C31)                                                                      \
    X(FUN_SMEM_FREE_INTERNAL_DECODE, 0x645EC7)                                                                         \
    X(FUN_SMEM_GET_SIZE_INTERNAL_DECODE, 0x645846)                                                                     \
    X(FUN_RETAINED_PAYLOAD_READ, 0x6510A0)                                                                             \
    X(FUN_RETURN_RETAINED_PAYLOAD, 0x650950)                                                                           \
                                                                                                                       \
    X(LUA_PUSH_NIL, 0x6F37F0)                                                                                          \
    X(LUA_IS_NUMBER, 0x6F34D0)                                                                                         \
    X(LUA_TO_NUMBER, 0x6F3620)                                                                                         \
    X(LUA_PUSH_NUMBER, 0x6F3810)                                                                                       \
    X(LUA_IS_STRING, 0x6F3510)                                                                                         \
    X(LUA_TO_STRING, 0x6F3690)                                                                                         \
    X(LUA_PUSH_STRING, 0x6F3890)                                                                                       \
    X(LUA_GET_TABLE, 0x6F3A40)                                                                                         \
    X(LUA_TYPE, 0x6F3400)                                                                                              \
    X(LUA_NEXT, 0x6F4450)                                                                                              \
    X(LUA_SET_TOP, 0x6F3080)                                                                                           \
    X(LUA_ERROR, 0x6F4940)                                                                                             \
                                                                                                                       \
    X(CONST_BLIP_VERTICES, 0xBC8230)                                                                                   \
    X(CONST_NORMAL_VEC3, 0xBC829C)                                                                                     \
    X(CONST_TEX_COORDS, 0xBC77F0)                                                                                      \
    X(CONST_VERT_INDICES, 0x807A2C)                                                                                    \
    X(CONST_BLIP_HALF, 0xBC7630)                                                                                       \
    X(CONST_FACTION_TEMPLATE_DB, 0xC0DD34)                                                                             \
    X(CONST_CHARACTER_SKIN_ATLAS_COORDINATES, 0xB42450)                                                                \
                                                                                                                       \
    X(VAR_ASYNC_BUFFER_USAGE_COUNTER, 0xB05D18)                                                                        \
    X(VAR_ITEMDB_CACHE, 0xC0E2A0)                                                                                      \
                                                                                                                       \
    X(VFTABLE_CSTATUS, 0x7FFA10)

namespace Offsets {

#define X(name, default_val) extern uintptr_t name;
OFFSET_LIST
#undef X

// Load offset overrides from an INI file (dllDir + "offsets.ini").
// Returns true if the file was found and parsed.
// Missing file is not an error — compiled-in defaults are used.
auto LoadFromFile(const char* dllDir) -> bool;

} // namespace Offsets
