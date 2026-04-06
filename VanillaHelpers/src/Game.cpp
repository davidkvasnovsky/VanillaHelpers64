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

#include "Game.h"

#include "Offsets.h"

namespace Game {

// ── Lua function pointers (populated by Init) ─────────────────────────────
namespace Lua {
lua_pushnil_t PushNil = nullptr;
lua_isnumber_t IsNumber = nullptr;
lua_tonumber_t ToNumber = nullptr;
lua_pushnumber_t PushNumber = nullptr;
lua_isstring_t IsString = nullptr;
lua_tostring_t ToString = nullptr;
lua_pushstring_t PushString = nullptr;
lua_gettable_t GetTable = nullptr;
lua_type_t Type = nullptr;
lua_next_t Next = nullptr;
lua_settop_t SetTop = nullptr;
lua_error_t Error = nullptr;
} // namespace Lua

// ── Game function pointers (populated by Init) ────────────────────────────
FrameScript_RegisterFunction_t FrameScript_RegisterFunction = nullptr;
GetGUIDFromName_t GetGUIDFromName = nullptr;
ClntObjMgrObjectPtr_t ClntObjMgrObjectPtr = nullptr;
RenderObjectBlips_t RenderObjectBlips = nullptr;
GxTexOwnerUpdate_t GxTexOwnerUpdate = nullptr;
TextureGetGxTex_t TextureGetGxTex = nullptr;
TextureDestroy_t TextureDestroy = nullptr;
GxRsSet_t GxRsSet = nullptr;
GxPrimLockVertexPtrs_t GxPrimLockVertexPtrs = nullptr;
GxPrimDrawElements_t GxPrimDrawElements = nullptr;
GxPrimUnlockVertexPtrs_t GxPrimUnlockVertexPtrs = nullptr;
TextureCreate_t TextureCreate = nullptr;
WorldPosToMinimapFrameCoords_t WorldPosToMinimapFrameCoords = nullptr;
SStrPack_t SStrPack = nullptr;
CWorld_QueryMapObjIDs_t CWorld_QueryMapObjIDs = nullptr;
ClntObjMgrGetActivePlayer_t ClntObjMgrGetActivePlayer = nullptr;
CGUnit_C_CanAssist_t CGUnit_C_CanAssist = nullptr;
FrameScript_Execute_t FrameScript_Execute = nullptr;
CVar_Register_t CVar_Register = nullptr;
ClntObjMgrEnumVisibleObjects_t ClntObjMgrEnumVisibleObjects = nullptr;
CGUnit_C_RefreshMount_t CGUnit_C_RefreshMount = nullptr;
SFile_Open_t SFile_Open = nullptr;
SFile_Read_t SFile_Read = nullptr;
SFile_Close_t SFile_Close = nullptr;
CGUnit_C_UpdateDisplayInfo_t CGUnit_C_UpdateDisplayInfo = nullptr;
DBCache_ItemStats_C_GetRecord_t DBCache_ItemStats_C_GetRecord = nullptr;

// ── Data pointers (populated by Init) ─────────────────────────────────────
C3Vector* s_blipVertices = nullptr;
TexCoord* texCoords = nullptr;
C3Vector* normal = nullptr;
unsigned short* vertIndices = nullptr;
const float* BLIP_HALF = nullptr;
const WowClientDB<FactionTemplate>* g_factionTemplateDB = nullptr;
void** g_itemDBCache = nullptr;

// ── Init: bind all pointers from Offsets ──────────────────────────────────
void Init() {
    Lua::PushNil = reinterpret_cast<Lua::lua_pushnil_t>(Offsets::LUA_PUSH_NIL);
    Lua::IsNumber = reinterpret_cast<Lua::lua_isnumber_t>(Offsets::LUA_IS_NUMBER);
    Lua::ToNumber = reinterpret_cast<Lua::lua_tonumber_t>(Offsets::LUA_TO_NUMBER);
    Lua::PushNumber = reinterpret_cast<Lua::lua_pushnumber_t>(Offsets::LUA_PUSH_NUMBER);
    Lua::IsString = reinterpret_cast<Lua::lua_isstring_t>(Offsets::LUA_IS_STRING);
    Lua::ToString = reinterpret_cast<Lua::lua_tostring_t>(Offsets::LUA_TO_STRING);
    Lua::PushString = reinterpret_cast<Lua::lua_pushstring_t>(Offsets::LUA_PUSH_STRING);
    Lua::GetTable = reinterpret_cast<Lua::lua_gettable_t>(Offsets::LUA_GET_TABLE);
    Lua::Type = reinterpret_cast<Lua::lua_type_t>(Offsets::LUA_TYPE);
    Lua::Next = reinterpret_cast<Lua::lua_next_t>(Offsets::LUA_NEXT);
    Lua::SetTop = reinterpret_cast<Lua::lua_settop_t>(Offsets::LUA_SET_TOP);
    Lua::Error = reinterpret_cast<Lua::lua_error_t>(Offsets::LUA_ERROR);

    FrameScript_RegisterFunction = reinterpret_cast<FrameScript_RegisterFunction_t>(Offsets::FUN_REGISTER_LUA_FUNCTION);
    GetGUIDFromName = reinterpret_cast<GetGUIDFromName_t>(Offsets::FUN_GET_GUID_FROM_NAME);
    ClntObjMgrObjectPtr = reinterpret_cast<ClntObjMgrObjectPtr_t>(Offsets::FUN_CLNT_OBJ_MGR_OBJECT_PTR);
    RenderObjectBlips = reinterpret_cast<RenderObjectBlips_t>(Offsets::FUN_RENDER_OBJECT_BLIP);
    GxTexOwnerUpdate = reinterpret_cast<GxTexOwnerUpdate_t>(Offsets::FUN_GX_TEX_OWNER_UPDATE);
    TextureGetGxTex = reinterpret_cast<TextureGetGxTex_t>(Offsets::FUN_TEXTURE_GET_GX_TEX);
    TextureDestroy = reinterpret_cast<TextureDestroy_t>(Offsets::FUN_TEXTURE_DESTROY);
    GxRsSet = reinterpret_cast<GxRsSet_t>(Offsets::FUN_GX_RS_SET);
    GxPrimLockVertexPtrs = reinterpret_cast<GxPrimLockVertexPtrs_t>(Offsets::FUN_GX_PRIM_LOCK_VERTEX_PTRS);
    GxPrimDrawElements = reinterpret_cast<GxPrimDrawElements_t>(Offsets::FUN_GX_PRIM_DRAW_ELEMENTS);
    GxPrimUnlockVertexPtrs = reinterpret_cast<GxPrimUnlockVertexPtrs_t>(Offsets::FUN_GX_PRIM_UNLOCK_VERTEX_PTRS);
    TextureCreate = reinterpret_cast<TextureCreate_t>(Offsets::FUN_TEXTURE_CREATE);
    WorldPosToMinimapFrameCoords =
        reinterpret_cast<WorldPosToMinimapFrameCoords_t>(Offsets::FUN_WORLD_POS_TO_MINIMAP_FRAME_COORDS);
    SStrPack = reinterpret_cast<SStrPack_t>(Offsets::FUN_S_STR_PACK);
    CWorld_QueryMapObjIDs = reinterpret_cast<CWorld_QueryMapObjIDs_t>(Offsets::FUN_CWORLD_QUERY_MAP_OBJ_IDS);
    ClntObjMgrGetActivePlayer =
        reinterpret_cast<ClntObjMgrGetActivePlayer_t>(Offsets::FUN_CLNT_OBJ_MGR_GET_ACTIVE_PLAYER);
    CGUnit_C_CanAssist = reinterpret_cast<CGUnit_C_CanAssist_t>(Offsets::FUN_CGUNIT_C_CAN_ASSIST);
    FrameScript_Execute = reinterpret_cast<FrameScript_Execute_t>(Offsets::FUN_FRAME_SCRIPT_EXECUTE);
    CVar_Register = reinterpret_cast<CVar_Register_t>(Offsets::FUN_CVAR_REGISTER);
    ClntObjMgrEnumVisibleObjects =
        reinterpret_cast<ClntObjMgrEnumVisibleObjects_t>(Offsets::FUN_CLNT_OBJ_MGR_ENUM_VISIBLE_OBJECTS);
    CGUnit_C_RefreshMount = reinterpret_cast<CGUnit_C_RefreshMount_t>(Offsets::FUN_CGUNIT_C_REFRESH_MOUNT);
    SFile_Open = reinterpret_cast<SFile_Open_t>(Offsets::FUN_SFILE_OPEN);
    SFile_Read = reinterpret_cast<SFile_Read_t>(Offsets::FUN_SFILE_READ);
    SFile_Close = reinterpret_cast<SFile_Close_t>(Offsets::FUN_SFILE_CLOSE);
    CGUnit_C_UpdateDisplayInfo =
        reinterpret_cast<CGUnit_C_UpdateDisplayInfo_t>(Offsets::FUN_CGUNIT_C_UPDATE_DISPLAY_INFO);
    DBCache_ItemStats_C_GetRecord =
        reinterpret_cast<DBCache_ItemStats_C_GetRecord_t>(Offsets::FUN_DBCACHE_ITEM_STATS_C_GET_RECORD);

    s_blipVertices = reinterpret_cast<C3Vector*>(Offsets::CONST_BLIP_VERTICES);
    texCoords = reinterpret_cast<TexCoord*>(Offsets::CONST_TEX_COORDS);
    normal = reinterpret_cast<C3Vector*>(Offsets::CONST_NORMAL_VEC3);
    vertIndices = reinterpret_cast<unsigned short*>(Offsets::CONST_VERT_INDICES);
    BLIP_HALF = reinterpret_cast<const float*>(Offsets::CONST_BLIP_HALF);
    g_factionTemplateDB = reinterpret_cast<const WowClientDB<FactionTemplate>*>(Offsets::CONST_FACTION_TEMPLATE_DB);
    g_itemDBCache = reinterpret_cast<void**>(Offsets::VAR_ITEMDB_CACHE);
}

// ── CStatus ───────────────────────────────────────────────────────────────
CStatus::CStatus()
    : vftable(reinterpret_cast<void*>(Offsets::VFTABLE_CSTATUS)),
      
      m_head{.prevLink=&m_head, .next=reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(&m_head) | 1U)}
      {}

CStatus::~CStatus() {
    reinterpret_cast<CStatus_Destructor_t>(Offsets::FUN_CSTATUS_DESTRUCTOR)(this);
}

CGxTexFlags::CGxTexFlags(
    EGxTexFilter filter,
    uint32_t wrapU,
    uint32_t wrapV,
    uint32_t forceMipTracking,
    uint32_t generateMipMaps,
    uint32_t renderTarget,
    uint32_t maxAnisotropy,
    uint32_t unknownFlag
) {
    reinterpret_cast<CGxTexFlags_Constructor_t>(Offsets::FUN_CGX_TEX_FLAGS_CONSTRUCTOR)(
        this, filter, wrapU, wrapV, forceMipTracking, generateMipMaps, renderTarget, maxAnisotropy, unknownFlag
    );
}

void DrawMinimapTexture(HTEXTURE__* texture, C2Vector minimapPosition, float scale, bool gray) {
    CImVector color = {.b=0xFF, .g=0xFF, .r=0xFF, .a=0xFF}; // White
    if (gray) {
        color = {.b=0xFF, .g=0xB0, .r=0xB0, .a=0xB0}; //  Dark Gray
    }

    C3Vector vertices[4];
    for (auto i = 0; i < 4; i++) {
        vertices[i].x = minimapPosition.x + scale * s_blipVertices[i].x;
        vertices[i].y = minimapPosition.y + scale * s_blipVertices[i].y;
        vertices[i].z = scale * s_blipVertices[i].z;
    }

    CStatus status;
    CGxTex* gxTex = TextureGetGxTex(texture, 1, &status);
    if (!status.ok()) {
        return;
    }

    GxRsSet(GxRs_Texture0, gxTex);

    GxPrimLockVertexPtrs(
        4, vertices, 12, normal, 0, &color, 0, nullptr, 0, reinterpret_cast<C2Vector*>(texCoords), 8, nullptr, 0
    );

    GxPrimDrawElements(EGxPrim::GxPrim_TriangleStrip, 4, vertIndices);

    GxPrimUnlockVertexPtrs();
}

} // namespace Game
