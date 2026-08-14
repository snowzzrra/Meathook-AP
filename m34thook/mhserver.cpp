#include "mh_defs.hpp"

#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <string>
#include <vector>
#include "game_exe_interface.hpp"

#include <meathook_interface.h>
#include <windows.h>
#include "cmdsystem.hpp"
#include "doomoffs.hpp"
#include "meathook.h"
#include "cmdsystem.hpp"
#include "idtypeinfo.hpp"
#include "eventdef.hpp"
#include "scanner_core.hpp"
#include "idLib.hpp"
#include "idStr.hpp"
#include "clipboard_helpers.hpp"
#include "mh_ap_runtime.hpp"

class RpcServer
{
    DWORD m_ThreadId;

    static DWORD WINAPI RpcListener(LPVOID Data);
public:
    RpcServer() {
        CreateThread(NULL, 0, RpcListener, this, 0, &m_ThreadId);
    }
};

DWORD WINAPI RpcServer::RpcListener(LPVOID Data)
{
    RPC_STATUS status;
    const char* pszProtocolSequence = "ncacn_np";
    unsigned char* pszSecurity = NULL;
    const char* pszEndpoint = "\\pipe\\meathook_interface_rpc";
    unsigned int    cMinCalls = 1;
    unsigned int    fDontWait = FALSE;

    status = RpcServerUseProtseqEpA((unsigned char*)pszProtocolSequence,
                                    RPC_C_LISTEN_MAX_CALLS_DEFAULT,
                                    (unsigned char*)pszEndpoint,
                                    pszSecurity);

    if (status) return 0;

    status = RpcServerRegisterIf(meathook_interface_v1_0_s_ifspec,
                                 NULL,
                                 NULL);

    if (status) return 0;

    status = RpcServerListen(cMinCalls,
                             RPC_C_LISTEN_MAX_CALLS_DEFAULT,
                             fDontWait);

    if (status) return 0;

    return 0;
}

/******************************************************/
/*         MIDL allocate and free                     */
/******************************************************/

void __RPC_FAR* __RPC_USER midl_user_allocate(size_t len)
{
    return(malloc(len));
}

void __RPC_USER midl_user_free(void __RPC_FAR* ptr)
{
    free(ptr);
}

const static RpcServer gRpcServer;

void KeepAlive(
    /* [string][in] */ int* Size)
{
}

void ExecuteConsoleCommand(
    /* [string][in] */ unsigned char* pszString
    )
{
    idCmd::execute_command_text((char *)pszString);
}

extern std::vector<std::string> gActiveEncounterNames;
void GetActiveEncounter(
    /* [string][in] */ int *Size,
    /* [string][in] */ unsigned char* pszString
    )
{
    gActiveEncounterNames.clear();
    idCmd::execute_command_text("mh_active_encounter");
    // Send over the entire list of active encounters, or only send one for backwards compatibility.
    pszString[0] = 0;
    if (gActiveEncounterNames.empty() == false) {
        strcpy_s((char*)pszString, *Size, gActiveEncounterNames[0].c_str());
    }

    if (*Size > 260) {
        for (size_t i = 1; i < gActiveEncounterNames.size(); i += 1) {
            strcat_s((char*)pszString, *Size, ";");
            strcat_s((char*)pszString, *Size, gActiveEncounterNames[i].c_str());
        }
    }

    OutputDebugStringA("MH Interface Active encounter:");
    for (size_t i = 0; i < gActiveEncounterNames.size(); i += 1) {
        OutputDebugStringA(gActiveEncounterNames[i].c_str());
        OutputDebugStringA("\n");
    }

    *Size = (int)strlen((const char*)pszString);
}

void OverloadMemory(void* Memory, void* Data, size_t Size);
void WriteOverloadMemorySize(void* Memory, size_t Size);
void WriteOverloadMemory(void* Memory, void* Data, size_t Size, size_t Offset);
std::string gOverrideName;
extern std::string gLastLoadedEntities;
char gOverrideBuffer[256];
const char* gOverrideFileName = nullptr;
unsigned long long gBufferReload = 0;
void PushEntitiesFile(
    /* [size_is][in] */ unsigned char* pBuffer,
    boolean Start,
    int Size)
{
    if (Start != false) {
        strcpy_s(gOverrideBuffer, (char*)pBuffer);
        gOverrideFileName = gOverrideBuffer;
        OutputDebugStringA("Overriding the next load: ");
        OutputDebugStringA(gOverrideBuffer);
        OutputDebugStringA("\n");
        idCmd::execute_command_text("mh_force_reload");
    } else {

        // TODO: Cause an automatic reload.
    }
}

void UploadData(
    /* [in] */ int Size,
    /* [in] */ int Offset,
    /* [size_is][in] */ unsigned char* pBuffer)
{
}

void GetEntitiesFile(
    /* [out][in] */ int* Size,
    /* [size_is][out] */ unsigned char* pBuffer)
{
    gOverrideName = gLastLoadedEntities;

    char TempPath[MAX_PATH];
    char TempFile[MAX_PATH];
    GetTempPathA(sizeof(TempPath), TempPath);
    GetTempFileNameA(TempPath, "Temp", 0, TempFile);
    strcat_s(TempPath, TempFile);
    char String[MAX_PATH];
    sprintf_s(String, "mh_dumpmap %s", TempFile);
    idCmd::execute_command_text(String);
    memcpy(pBuffer, TempFile, strlen(TempFile) + 1);
    *Size = (int)strlen(TempFile);
}

void cmd_mh_spawninfo(idCmdArgs* args);
void GetSpawnInfo(
    /* [out][in] */ int* Size,
    /* [size_is][out] */ unsigned char* pBuffer)
{
    idCmd::execute_command_text("getviewpos");

    if (!OpenClipboard(NULL))
        return;
    char *cbhandle = (char*)GetClipboardData(CF_TEXT);
    strcpy_s((char*)pBuffer, *Size, cbhandle);
    CloseClipboard();
}

void GetCurrentCheckpoint(
    /* [string][in] */ int* Size,
    /* [string][in] */ unsigned char* pszString
)
{
    if (*Size <= 0)
        return;
    APRuntimeSnapshot snapshot{};
    mh_ap::get_runtime_snapshot(&snapshot);
    pszString[0] = 0;
    if (*Size > 0)
        strcpy_s((char*)pszString, *Size, snapshot.checkpoint);
    *Size = (int)strlen((const char*)pszString);
}

void GetAPRuntimeInfo(
    /* [out] */ APRuntimeInfo* info)
{
    mh_ap::get_runtime_info(info);
}

void GetRuntimeSnapshot(
    /* [out] */ APRuntimeSnapshot* snapshot)
{
    mh_ap::get_runtime_snapshot(snapshot);
}

void ApplyDeathLink(
    /* [string][in] */ char* request_id,
    /* [out] */ APDeathLinkStatus* status)
{
    if (status == nullptr)
        return;
    *status = mh_ap::apply_deathlink(request_id);
}

void GetAPEventsSince(
    /* [in] */ hyper after_sequence,
    /* [in] */ unsigned long max_count,
    /* [out] */ APEventBatch* batch)
{
    mh_ap::get_events_since(static_cast<std::uint64_t>(after_sequence), max_count, batch);
}

void GetInventoryItemCount(char* decl_name, APInventoryIntResult* result)
{
    mh_ap::get_inventory_item_count(decl_name, result);
}

void HasPlayerPerk(char* decl_name, APInventoryBoolResult* result)
{
    mh_ap::has_player_perk(decl_name, result);
}

void IsPlayerPerkActive(char* decl_name, APInventoryBoolResult* result)
{
    mh_ap::is_player_perk_active(decl_name, result);
}

void GetEquippedWeapon(APEquippedWeaponResult* result)
{
    mh_ap::get_equipped_weapon(result);
}
