// ReSharper disable CppClangTidyReadabilityUseConcisePreprocessorDirectives
#include "stdafx.h"

#include "common.hpp"
#include "logging.hpp"

#include "game_funcs.hpp"

#include "gamevars.hpp"
#include "mgs2_linkvarbuf.hpp"
#include "mgs3_linkvarbuf.hpp"
#include "input_handler.hpp"
#include <Xinput.h> //not using xinput itself, just the VK_PAD defs
/*
#if defined(RELEASE_BUILD)
#define RELEASE_CLEARED
#undef RELEASE_BUILD
#endif
*/
namespace
{
    int* GM_LoadRequest = nullptr;

#if !defined(RELEASE_BUILD)
    void* null_fn(int, int) { return nullptr; }

    void HookReturn(uint8_t* addr)
    {
        static SafetyHookInline hook;
        hook = safetyhook::create_inline(addr, null_fn);
    }

    using GM_GetArea_t = const char* ();
    using NewCharaCallback = std::function<void* (int, int)>;

    GM_GetArea_t* GM_GetArea = nullptr;
    NewCharaCallback* pending = nullptr;

    std::unordered_map<std::string, std::unordered_map<uint32_t, NewCharaCallback>> return_hooks;

    struct ObserveEntry {
        std::function<void()> callback;
        SafetyHookMid hook;
    };

    std::unordered_map<std::string, std::unordered_map<uint32_t, ObserveEntry>> observe_hooks;

    SafetyHookInline GM_GetCharaID_hook;

    void* override_stub(int name, int map)
    {
        NewCharaCallback* cb = pending;
        pending = nullptr;
        if (cb && *cb) return (*cb)(name, map);
        return nullptr;
    }

    void* hooked_GM_GetCharaID(int nID)
    {
        if (const char* stage = GM_GetArea())
        {
            auto osit = observe_hooks.find(stage);
            if (osit != observe_hooks.end())
            {
                auto oiit = osit->second.find(static_cast<uint32_t>(nID));
                if (oiit != osit->second.end() && !oiit->second.hook)
                {
                    void* fn = GM_GetCharaID_hook.call<void*>(nID);
                    auto cb = oiit->second.callback;
                    static std::function<void()>* s_cb = nullptr;
                    s_cb = &oiit->second.callback;
                    oiit->second.hook = safetyhook::create_mid(fn, [](SafetyHookContext&) { if (s_cb) (*s_cb)(); });
                    return fn;
                }
            }

            auto sit = return_hooks.find(stage);
            if (sit != return_hooks.end())
            {
                auto iit = sit->second.find(static_cast<uint32_t>(nID));
                if (iit != sit->second.end())
                {
                    pending = &iit->second;
                    return reinterpret_cast<void*>(override_stub);
                }
            }
        }
        return GM_GetCharaID_hook.call<void*>(nID);
    }
#endif

    constexpr unsigned int STRCODE_SCENERIO_GCX = GameVars::GV_StrCode("scenerio");

    // Swap to the select stage under scenerio.gcx and flag a load, the same request the game's own
    // debug helper issues. Set by whichever game hooked, since the linkvars differ.
    void (*EnterDeveloperMenu)() = nullptr;

    constexpr int kSoftResetFrames = 20;

    uint32_t* PadDirectStatus = nullptr;
    int* GM_PadResetDisable = nullptr;
    uint32_t SoftResetChord = 0;
    void (*SoftReset)() = nullptr;

    // The port packs flags above the PS2's 16 bit button word, so the game's own == test never matches.
    uint32_t PadButtons()
    {
        return PadDirectStatus ? (*PadDirectStatus & 0xFFFF) : 0;
    }

    // Exact match like the PS2, so a seventh button aborts.
    bool SoftResetChordHeld()
    {
        return Shared_Gamefuncs::SoftResetChordEnabled && SoftResetChord != 0 && PadButtons() == SoftResetChord;
    }

    int SubtitleHideFrames = 0;
    void (__fastcall* GM_JimakuHide)() = nullptr;
    void** JimakuWork = nullptr;
    // The caption daemon is resident, so an interrupted line survives the stage change.
    void HideCaptions()
    {
        if (GM_JimakuHide && JimakuWork && *JimakuWork)
        {
            GM_JimakuHide();
        }
    }

    uint32_t* PauseLevel = nullptr;

    void TickSoftResetChord()
    {
        static int held = 0;
        static bool fired = false;

        if (!SoftResetChordHeld() || (GM_PadResetDisable && *GM_PadResetDisable != 0))
        {
            held = 0;
            fired = false;
            return;
        }

        if (fired || ++held <= kSoftResetFrames)
        {
            return;
        }

        // Stacking a request on a live load pulls a file out from under the loader thread.
        if (GM_LoadRequest && *GM_LoadRequest != 0)
        {
            return;
        }

        // The shell's own Reset Game is only reachable from the pause menu, so it never runs with the
        // actor system stopped for a codec. Resetting out of one leaves later loads unable to run.
        if (PauseLevel && (*PauseLevel & 0x1))
        {
            return;
        }

        fired = true;
        spdlog::info("Soft reset: returning to title.");

        if (SoftReset)
        {
            SoftReset();
        }
    }

    // Present keeps ticking through the load, unlike ActGame, whose tail the load path skips.
    void ServiceSoftResetImpl()
    {
        if (SubtitleHideFrames <= 0)
        {
            return;
        }

        --SubtitleHideFrames;
        HideCaptions();
    }

    // Changing stage out from under a live movie stream hangs, so follow the two actors that hold one.
    // titlescr.c destroys its own actor on Start, so its lifetime is exactly the attract screen.
    void* TitleScreenWork = nullptr;
    void* MovieStreamWork = nullptr;
    SafetyHookInline NewTitleScrMan_hook {};
    SafetyHookInline NewMpegPssMovieStr_hook {};
    SafetyHookInline GV_DestroyActor_hook {};

    void* __fastcall hooked_NewTitleScrMan(int name, int map)
    {
        TitleScreenWork = NewTitleScrMan_hook.call<void*>(name, map);
        return TitleScreenWork;
    }

    void* __fastcall hooked_NewMpegPssMovieStr(int name, int map)
    {
        MovieStreamWork = NewMpegPssMovieStr_hook.call<void*>(name, map);
        return MovieStreamWork;
    }

    void __fastcall hooked_GV_DestroyActor(void* work)
    {
        if (work == TitleScreenWork)
        {
            TitleScreenWork = nullptr;
        }

        if (work == MovieStreamWork)
        {
            MovieStreamWork = nullptr;
        }

        GV_DestroyActor_hook.call(work);
    }

    std::atomic<bool> DevMenuRequested { false };

    // Polled from Present, so it only asks; the game's own frame runs it.
    void ServiceDevMenuRequest()
    {
        if (!DevMenuRequested || !EnterDeveloperMenu)
        {
            return;
        }

        if ((GM_LoadRequest && *GM_LoadRequest != 0) || (GM_PadResetDisable && *GM_PadResetDisable != 0))
        {
            return;
        }

        DevMenuRequested = false;

        const char* stage = g_GameVars.GetCurrentStage();
        const bool titleStage = stage && std::strcmp(stage, "n_title") == 0;

        // The attract screen and the movie each leave the jump half done: it reaches the menu, then the
        // next stage load dies on a freed actor.
        if (TitleScreenWork || MovieStreamWork || (!titleStage && g_GameVars.GetGameMode() == "Menu"))
        {
            spdlog::info("Developer menu: ignored on '{:s}'.", stage ? stage : "?");
            return;
        }

        EnterDeveloperMenu();
    }

    // Bluepoint's own chord restarts through the shell menu and ignores GM_PadResetDisable.
    void DisableShellSoftReset(const char* pattern)
    {
        if (uint8_t* bpChord = Memory::PatternScan(baseModule, pattern, "BP_CheckForSoftReset"))
        {
            Memory::PatchBytes(reinterpret_cast<uintptr_t>(bpChord), "\xC3", 1);
            spdlog::info("Soft reset: shell restart chord disabled at {:s}+{:X}", sExeName.c_str(),
                reinterpret_cast<uintptr_t>(bpChord) - reinterpret_cast<uintptr_t>(baseModule));
        }
    }

    // ActGame's tail is reached from every screen, menus included.
    void HookActGameTail(const char* pattern, const char* name)
    {
        if (uint8_t* tail = Memory::PatternScan(baseModule, pattern, name))
        {
            static SafetyHookMid ActGameTail_hook {};
            ActGameTail_hook = safetyhook::create_mid(tail + 6, [](SafetyHookContext&)
                {
                    TickSoftResetChord();
                    ServiceDevMenuRequest();
                });
            LOG_HOOK(ActGameTail_hook, name);
        }
    }

    void RegisterDevMenuHotkey()
    {
        if (!Shared_Gamefuncs::DevMenuHotkey || !EnterDeveloperMenu)
        {
            return;
        }

        g_InputHandler.RegisterHotkey(Shared_Gamefuncs::DevMenuHotkey, "Return to Developer Menu", []()
        {
            DevMenuRequested = true;
        });
    }
}

namespace MGS2_GameFuncs
{

    /*
using NewItemChange_t = void* (__fastcall*)(int a1);
using NewItemChange2_t = void* (__fastcall*)(int a1);

static NewItemChange_t  NewItemChange = nullptr;
static NewItemChange2_t NewItemChange2 = nullptr;


    uint8_t* act154 = Memory::PatternScan(baseModule, "48 89 5C 24 ?? 57 48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 33 FF 48 8B D9", "NewMenuPrimControl() -> Act_154");

    NewItemChange2 = reinterpret_cast<NewItemChange2_t>(ResolveCall(act154 + 0x5F));
    NewItemChange = reinterpret_cast<NewItemChange_t>(ResolveCall(act154 + 0xB6));
    */

}

void MGS2_GameFuncs::HookGameFuncs()
{
    using namespace Shared_Gamefuncs;
    using namespace MGS2_GameFuncs;
    using namespace MGS2_LinkVarBuf;
    using namespace MGS2Stages;
    spdlog::info("MGS2_GameFuncs: Hooking game functions.");
    GM_SeSet = reinterpret_cast<GM_SeSet_t>(Memory::PatternScan(baseModule, "83 F9 ?? 74 ?? ?? 83 E1", "GM_SeSet"));
    spdlog::info("MGS2_GameFuncs: GM_SeSet address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_SeSet - (uintptr_t)baseModule);

    uint8_t* GM_ItemNum_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 85 C0 7E ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? EB", "GM_ItemNum call site");
    GM_ItemNum = reinterpret_cast<GM_ItemNum_t>(Memory::ResolveCall(GM_ItemNum_scan));
    spdlog::info("MGS2_GameFuncs: GM_ItemNum address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_ItemNum - (uintptr_t)baseModule);

    uint8_t* GM_WeaponNum_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 85 C0 79 ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 0F 88", "GM_WeaponNum call site");
    GM_WeaponNum = reinterpret_cast<GM_WeaponNum_t>(Memory::ResolveCall(GM_WeaponNum_scan));
    spdlog::info("MGS2_GameFuncs: GM_WeaponNum address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_WeaponNum - (uintptr_t)baseModule);


    uint8_t* L2D_GetObject_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 8B 8B ?? ?? ?? ?? BA ?? ?? ?? ?? 48 89 83 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 8B ?? ?? ?? ?? BA ?? ?? ?? ?? 48 89 83 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8B 8B", "L2D_GetObject call site");
    L2D_GetObject = reinterpret_cast<L2D_GetObject_t>(Memory::ResolveCall(L2D_GetObject_scan));
    spdlog::info("MGS2_GameFuncs: L2D_GetObject address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)L2D_GetObject - (uintptr_t)baseModule);

    uint8_t* L2D_GetParts_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 48 85 C0 74 ?? 8B 93 ?? ?? ?? ?? 0F 28 DE", "L2D_GetParts call site");
    L2D_GetParts = reinterpret_cast<L2D_GetParts_t>(Memory::ResolveCall(L2D_GetParts_scan));
    spdlog::info("MGS2_GameFuncs: L2D_GetParts address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)L2D_GetParts - (uintptr_t)baseModule);

    uint8_t* WriteString_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? C7 87 ?? ?? ?? ?? ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? C7 87 ?? ?? ?? ?? ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 41 B8 ?? ?? ?? ?? C7 87 ?? ?? ?? ?? ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? C7 87 ?? ?? ?? ?? ?? ?? ?? ?? 48 8B CF E8 ?? ?? ?? ?? 41 B8", "WriteString call site");
    WriteString = reinterpret_cast<WriteString_t>(Memory::ResolveCall(WriteString_scan));
    spdlog::info("MGS2_GameFuncs: WriteString address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)WriteString - (uintptr_t)baseModule);

    uint8_t* GM_GetDGGroupID_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 48 8B CF 89 83 ?? ?? ?? ?? E8 ?? ?? ?? ?? FF 8F ?? ?? ?? ?? 48 8B 5C 24 ?? 48 83 C4 20 5F C3 48 83 C4 20 5F E9 ?? ?? ?? ?? CC 48 89 5C 24 ?? 57 48 83 EC 40 48 8B FA 0F 29 74 24 ?? 48 8B CF 0F 29 7C 24 ?? 49 8B D0 49 8B D8 E8 ?? ?? ?? ?? 48 C7 87 ?? ?? ?? ?? 44 00 00 00 0F 57 F6 F3 0F 10 3D ?? ?? ?? ?? F3 0F 10 2D ?? ?? ?? ?? F3 0F 10 25 ?? ?? ?? ?? F3 0F 10 1D ?? ?? ?? ?? F3 0F 10 15 ?? ?? ?? ?? F3 0F 11 2D ?? ?? ?? ?? F3 0F 11 25 ?? ?? ?? ?? F3 0F 11 1D ?? ?? ?? ?? F3 0F 11 15 ?? ?? ?? ?? F3 0F 11 2D ?? ?? ?? ?? F3 0F 11 25 ?? ?? ?? ?? F3 0F 11 1D ?? ?? ?? ?? F3 0F 11 15 ?? ?? ?? ?? F3 0F 11 2D ?? ?? ?? ?? F3 0F 11 25 ?? ?? ?? ?? F3 0F 11 1D ?? ?? ?? ?? F3 0F 11 15 ?? ?? ?? ?? F3 0F 11 2D ?? ?? ?? ?? F3 0F 11 25 ?? ?? ?? ?? F3 0F 11 1D ?? ?? ?? ?? F3 0F 11 15 ?? ?? ?? ?? F3 0F 10 43 ?? F3 0F 59 C6 ?? ?? ?? ?? F3 0F 59 C7 F3 0F 2C C0 66 89 05 ?? ?? ?? ?? F3 0F 10 43 ?? F3 0F 59 C6 F3 0F 58 43 ?? C7 05 ?? ?? ?? ?? 00 10 FF 8F C6 05 ?? ?? ?? ?? FF C6 05 ?? ?? ?? ?? FF C6 05 ?? ?? ?? ?? FF F3 0F 59 C7 C6 05 ?? ?? ?? ?? 5A", "GM_GetDGGroupID call site");
    GM_GetDGGroupID = reinterpret_cast<GM_GetDGGroupID_t>(Memory::ResolveCall(GM_GetDGGroupID_scan));
    spdlog::info("MGS2_GameFuncs: GM_GetDGGroupID address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_GetDGGroupID - (uintptr_t)baseModule);

    uint8_t* UpdateVectors_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? FF 8F ?? ?? ?? ?? 48 8B 5C 24 ?? 48 83 C4 20 5F C3 48 83 C4 20 5F E9 ?? ?? ?? ?? CC 48 89 5C 24 ?? 57 48 83 EC 40 48 8B FA 0F 29 74 24 ?? 48 8B CF 0F 29 7C 24 ?? 49 8B D0 49 8B D8 E8 ?? ?? ?? ?? 48 C7 87 ?? ?? ?? ?? 44 00 00 00 0F 57 F6 F3 0F 10 3D ?? ?? ?? ?? F3 0F 10 2D ?? ?? ?? ?? F3 0F 10 25 ?? ?? ?? ?? F3 0F 10 1D ?? ?? ?? ?? F3 0F 10 15 ?? ?? ?? ?? F3 0F 11 2D ?? ?? ?? ?? F3 0F 11 25 ?? ?? ?? ?? F3 0F 11 1D ?? ?? ?? ?? F3 0F 11 15 ?? ?? ?? ?? F3 0F 11 2D ?? ?? ?? ?? F3 0F 11 25 ?? ?? ?? ?? F3 0F 11 1D ?? ?? ?? ?? F3 0F 11 15 ?? ?? ?? ?? F3 0F 11 2D ?? ?? ?? ?? F3 0F 11 25 ?? ?? ?? ?? F3 0F 11 1D ?? ?? ?? ?? F3 0F 11 15 ?? ?? ?? ?? F3 0F 11 2D ?? ?? ?? ?? F3 0F 11 25 ?? ?? ?? ?? F3 0F 11 1D ?? ?? ?? ?? F3 0F 11 15 ?? ?? ?? ?? F3 0F 10 43 ?? F3 0F 59 C6 ?? ?? ?? ?? F3 0F 59 C7 F3 0F 2C C0 66 89 05 ?? ?? ?? ?? F3 0F 10 43 ?? F3 0F 59 C6 F3 0F 58 43 ?? C7 05 ?? ?? ?? ?? 00 10 FF 8F C6 05 ?? ?? ?? ?? FF C6 05 ?? ?? ?? ?? FF C6 05 ?? ?? ?? ?? FF F3 0F 59 C7 C6 05 ?? ?? ?? ?? 5A", "UpdateVectors call site");
    UpdateVectors_4 = reinterpret_cast<UpdateVectors_t>(Memory::ResolveCall(UpdateVectors_scan));
    spdlog::info("MGS2_GameFuncs: UpdateVectors address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)UpdateVectors_4 - (uintptr_t)baseModule);

    uint8_t* GV_DestroyActor_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 33 C0 48 8B 74 24 ?? 48 83 C4 ?? 5F C3 48 8B 74 24", "GV_DestroyActor call site");
    GV_DestroyActor = reinterpret_cast<GV_DestroyActor_t>(Memory::ResolveCall(GV_DestroyActor_scan));
    spdlog::info("MGS2_GameFuncs: GV_DestroyActor address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GV_DestroyActor - (uintptr_t)baseModule);

    // n_title's chara table entry for NewTitleScrMan, so we can watch the attract screen come and go.
    if (uint8_t* titleChara = Memory::PatternScan(baseModule, "A5 14 FC 00 00 00 00 00", "MGS 2: NewTitleScrMan chara entry"))
    {
        void* NewTitleScrMan = *reinterpret_cast<void**>(titleChara + 8);
        NewTitleScrMan_hook = safetyhook::create_inline(NewTitleScrMan, hooked_NewTitleScrMan);
        GV_DestroyActor_hook = safetyhook::create_inline(reinterpret_cast<void*>(GV_DestroyActor), hooked_GV_DestroyActor);
        spdlog::info("MGS2_GameFuncs: NewTitleScrMan address is {:s}+{:X}", sExeName.c_str(),
            reinterpret_cast<uintptr_t>(NewTitleScrMan) - reinterpret_cast<uintptr_t>(baseModule));
    }

    if (uint8_t* movieChara = Memory::PatternScan(baseModule, "CE BE FF 00 00 00 00 00", "MGS 2: NewMpegPssMovieStr chara entry"))
    {
        void* NewMpegPssMovieStr = *reinterpret_cast<void**>(movieChara + 8);
        NewMpegPssMovieStr_hook = safetyhook::create_inline(NewMpegPssMovieStr, hooked_NewMpegPssMovieStr);
        spdlog::info("MGS2_GameFuncs: NewMpegPssMovieStr address is {:s}+{:X}", sExeName.c_str(),
            reinterpret_cast<uintptr_t>(NewMpegPssMovieStr) - reinterpret_cast<uintptr_t>(baseModule));
    }




    {
        uint8_t* GM_SetArea_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 48 8B 1D ?? ?? ?? ?? 48 83 C3 1C 48 8B CB E8 ?? ?? ?? ?? 4C 8B C0 4C 2B C3 66 0F 1F 84 00", "GM_SetArea call site");
        GM_SetArea = reinterpret_cast<GM_SetArea_t>(Memory::ResolveCall(GM_SetArea_scan));
        spdlog::info("MGS2_GameFuncs: GM_SetArea address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_SetArea - (uintptr_t)baseModule);


        uint8_t* GCL_ChangeSenerioCode_scan = Memory::PatternScan(baseModule, "8B C8 E8 ?? ?? ?? ?? 33 C0 48 8B 4C 24", "GCL_ChangeSenerioCode call site");
        GCL_ChangeSenerioCode = reinterpret_cast<GCL_ChangeSenerioCode_t>(Memory::ResolveCall(GCL_ChangeSenerioCode_scan + 2));
        spdlog::info("MGS2_GameFuncs: GCL_ChangeSenerioCode address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GCL_ChangeSenerioCode - (uintptr_t)baseModule);

        GM_LoadRequest = reinterpret_cast<int*>(Memory::GetRelativeOffset(Memory::PatternScan(baseModule, "8B 05 ?? ?? ?? ?? A8 10 74 ?? E8", "MGS2: GM_PlayerStatus") + 2));
        spdlog::info("MGS2_GameFuncs: GM_LoadRequest address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_LoadRequest - (uintptr_t)baseModule);

        EnterDeveloperMenu = []()
        {
            using namespace Shared_Gamefuncs;
            using namespace MGS2_LinkVarBuf;
            using namespace MGS2Stages;

            if (!GM_SetArea || !GCL_ChangeSenerioCode || !GM_LoadRequest)
            {
                return;
            }

            GM_SetArea(GM_SaveArea, SELECT);
            GCL_ChangeSenerioCode(STRCODE_SCENERIO_GCX);
            GM_Result = 9999;
            *GM_LoadRequest = 0x0002 | 0x1;
        };


        // BP_ResetToTitle does the lot, but leaves the pause set, which the load gate then blocks on.
        using VoidFn_t = void(__fastcall*)();
        static VoidFn_t BP_ResetToTitle = nullptr;
        static VoidFn_t BP_RequestResetToTitle = nullptr;
        static uint32_t* GV_PauseLevel = nullptr;
        static int* GM_PauseRequest = nullptr;

        // The Master Collection menu's own reset item: request, then reset. ActGame sees the request
        // still set on the next paused frame and finishes with the pause-off.
        if (uint8_t* mcReset = Memory::PatternScan(baseModule, "48 8D 8C 24 D0 00 00 00 E8 ?? ?? ?? ?? 39 B4 24 D0 00 00 00 75 ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ??", "MGS 2: shell menu reset item"))
        {
            BP_RequestResetToTitle = reinterpret_cast<VoidFn_t>(Memory::ResolveCall(mcReset + 0x16));
            spdlog::info("MGS2_GameFuncs: BP_RequestResetToTitle address is {:s}+{:X}", sExeName.c_str(),
                reinterpret_cast<uintptr_t>(BP_RequestResetToTitle) - reinterpret_cast<uintptr_t>(baseModule));
        }

        BP_ResetToTitle = reinterpret_cast<VoidFn_t>(Memory::PatternScan(baseModule, "48 83 EC 28 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 8B 88 BC 00 00 00 E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? B9 20 07 18 00", "MGS 2: BP_ResetToTitle"));
        spdlog::info("MGS2_GameFuncs: BP_ResetToTitle address is {:s}+{:X}", sExeName.c_str(), reinterpret_cast<uintptr_t>(BP_ResetToTitle) - reinterpret_cast<uintptr_t>(baseModule));

        // The subtitle daemon is resident, so interrupting a codec leaves its line on screen.
        if (uint8_t* jimakuHide = Memory::PatternScan(baseModule, "48 8B 05 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? FF FF FF FF 81 48 50 00 01 00 00 C3", "MGS 2: GM_JimakuHide"))
        {
            GM_JimakuHide = reinterpret_cast<void(__fastcall*)()>(jimakuHide);
            JimakuWork = reinterpret_cast<void**>(Memory::GetRipRelativeAddress(jimakuHide, 3, 7));
        }

        if (uint8_t* pauseScan = Memory::PatternScan(baseModule, "F6 05 ?? ?? ?? ?? 02 74 26 E8 ?? ?? ?? ?? 85 C0 74 05 E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? 85 C0 74 0F E8 ?? ?? ?? ?? C7 05 ?? ?? ?? ?? 02 00 00 00", "MGS 2: ActGame() paused branch"))
        {
            GV_PauseLevel = reinterpret_cast<uint32_t*>(Memory::GetRipRelativeAddress(pauseScan, 2, 7));
            PauseLevel = GV_PauseLevel;
            GM_PauseRequest = reinterpret_cast<int*>(Memory::GetRipRelativeAddress(pauseScan + 0x25, 2, 10));

            // Starve the overlay's pad feed on chord frames or pause + L1/R1 opens it under us.
            static uint8_t* resumeAt = pauseScan + 0x17;
            static SafetyHookMid MCPadFeed_hook {};
            MCPadFeed_hook = safetyhook::create_mid(pauseScan + 0x12, [](SafetyHookContext& ctx)
                {
                    if (SoftResetChordHeld())
                    {
                        ctx.rip = reinterpret_cast<uintptr_t>(resumeAt);
                    }
                });
            LOG_HOOK(MCPadFeed_hook, "MGS 2: pause menu pad feed | Soft reset");
        }

        if (uint8_t* chordScan = Memory::PatternScan(baseModule, "81 3D ?? ?? ?? ?? 0F 09 00 00 75 19 39 1D", "MGS 2: ActGame() pad reset chord"))
        {
            PadDirectStatus = reinterpret_cast<uint32_t*>(Memory::GetRipRelativeAddress(chordScan, 2, 10));
            GM_PadResetDisable = reinterpret_cast<int*>(Memory::GetRipRelativeAddress(chordScan + 0xC, 2, 6));
            SoftResetChord = 0x90F;
            spdlog::info("MGS2_GameFuncs: GV_PadDataDirect status is {:s}+{:X}, GM_PadResetDisable is {:s}+{:X}",
                sExeName.c_str(), reinterpret_cast<uintptr_t>(PadDirectStatus) - reinterpret_cast<uintptr_t>(baseModule),
                sExeName.c_str(), reinterpret_cast<uintptr_t>(GM_PadResetDisable) - reinterpret_cast<uintptr_t>(baseModule));
        }

        SoftReset = []()
        {
            if (!BP_ResetToTitle || !GV_PauseLevel || !GM_PauseRequest)
            {
                return;
            }

            // ActGame's reset branch only runs when PAUSE is the only bit set, and it drops the
            // pause-off otherwise.
            *GV_PauseLevel = 0x2u;

            if (BP_RequestResetToTitle)
            {
                BP_RequestResetToTitle();
            }

            BP_ResetToTitle();
            *GM_PauseRequest = 2;

            // The caption daemon outlives the stage, and keeps redrawing until it actually goes.
            SubtitleHideFrames = 240;
        };

        if (Shared_Gamefuncs::SoftResetChordEnabled)
        {
            DisableShellSoftReset("48 83 EC 38 48 8B 0D ?? ?? ?? ?? 0F 29 74 24 20 E8 ?? ?? ?? ?? F3 0F 10 35 ?? ?? ?? ?? BA 0F 09 00 00 33 C9");
        }
        HookActGameTail("FF 81 E4 00 00 00 E8 ?? ?? ?? ?? B8 01 00 00 00 48 83 C4 20 5B C3", "MGS 2: ActGame() tail | Soft reset chord");


        MAKE_HOOK_MID(baseModule, "E9 ?? ?? ?? ?? C7 43 ?? 01 00 00 00 E9", "GM_StartDaemon() -> Act() | Developer menu / soft reset", {
            if (StartInDebugMode)
            {
                static bool startup = true;
                if (startup)
                {
                    startup = false;
                    EnterDeveloperMenu();
                    return;
                }
            }
                      });

        if (StartInDebugMode)
        {
            RegisterDevMenuHotkey();
        }
    }


#if !defined(RELEASE_BUILD)

    /*
    return_hooks[MGS2Stages::D082P01][0x000381AC];

    return_hooks[MGS2Stages::D082P01][0x000381AC] = [](int name, int map) -> void* {
        spdlog::info("returning nullptr for NewChara, name {:X} map {:X}", name, map);
        return nullptr;
    };


    observe_hooks[MGS2Stages::N_TITLE][0x0FC14A5].callback = []() {
        spdlog::info("NewTitleScrMan called");
        };
    */




#endif


}


void MG1_Gamefuncs::HookGameFuncs()
{
    using namespace Shared_Gamefuncs;
    using namespace MG1_Gamefuncs;

#if !defined(RELEASE_BUILD)
                //return_hooks[MG1Stages::MG1][0xDD5EB6];      //  sub_1400D2C80    // mg_draw / init_game
    //return_hooks[MG1Stages::MG2][0x0FA91C];      //  sub_1400D20F0
    //return_hooks[MG1Stages::MG2][0x588DA3];      //  sub_1400D2C80
    //return_hooks[MG1Stages::MG2][0x5B316E];      //  _NewSaveVariable
    //return_hooks[MG1Stages::MG2][0x6B237D];      //  _NewGclAssert
    //return_hooks[MG1Stages::MG2][0x7BC389];      //  sub_140071BE0
    //return_hooks[MG1Stages::MG2][0x7EEDB2];      //  sub_1400CAF30
    //return_hooks[MG1Stages::MG2][0x9474FF];      //  sub_1400D1960
    //return_hooks[MG1Stages::MG2][0x99F754];      //  sub_1400CB370
    //return_hooks[MG1Stages::MG2][0x9BC19A];      //  _VRCLR_SetStars_1
    //return_hooks[MG1Stages::MG2][0x0A833FE];     //  sub_1400CE210
    //return_hooks[MG1Stages::MG2][0x0AAF706];     //  GM_COM_InventoryChangeMode
    //return_hooks[MG1Stages::MG2][0x0BCF6FF];     //  sub_1400D1890
            //return_hooks[MG1Stages::MG2][0x0D8361E];     //  j_GM_RealTimeClockGet        crashes game
    //return_hooks[MG1Stages::MG2][0x0DAF423];     //  sub_140071980
    //return_hooks[MG1Stages::MG2][0x0DC83C5];     //  _GM_LoadPack
    //return_hooks[MG1Stages::MG2][0x0DD5EB7];     //  sub_1400D68E0
    //return_hooks[MG1Stages::MG2][0x0E76D74];     //  NewGclVariableMove
    //return_hooks[MG1Stages::MG2][0x0E78C6D];     //  sub_1400CE540

#endif
    
}


void MGS3_Gamefuncs::HookGameFuncs()
{
    using namespace Shared_Gamefuncs;
    using namespace MGS3_Gamefuncs;
    using namespace MGS3_LinkVarBuf;
    using namespace MGS3Stages;


    {
        uint8_t* GM_SetArea_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? E8 ?? ?? ?? ?? E8", "GM_SetArea call site");
        GM_SetArea = reinterpret_cast<GM_SetArea_t>(Memory::ResolveCall(GM_SetArea_scan));
        spdlog::info("MGS3_GameFuncs: GM_SetArea address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_SetArea - (uintptr_t)baseModule);

        uint8_t* GCL_ChangeSenerioCode_scan = Memory::PatternScan(baseModule, "83 0D ?? ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? B9 ?? ?? ?? ?? 48 83 C4 ?? E9", "GCL_ChangeSenerioCode call site");
        GCL_ChangeSenerioCode = reinterpret_cast<GCL_ChangeSenerioCode_t>(Memory::ResolveCall(GCL_ChangeSenerioCode_scan + 0xC));
        spdlog::info("MGS3_GameFuncs: GCL_ChangeSenerioCode address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GCL_ChangeSenerioCode - (uintptr_t)baseModule);


        // or dword ptr [rip+disp32], imm8 - the imm8 makes this 7 bytes, so the displacement is
        // not the last field and GetRelativeOffset would land a byte low.
        GM_LoadRequest = reinterpret_cast<int*>(Memory::GetRipRelativeAddress(Memory::PatternScan(baseModule, "83 0D ?? ?? ?? ?? ?? B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? B9 ?? ?? ?? ?? 48 83 C4 ?? E9", "MGS3: GM_PlayerStatus"), 2, 7));
        spdlog::info("MGS3_GameFuncs: GM_LoadRequest address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_LoadRequest - (uintptr_t)baseModule);

        EnterDeveloperMenu = []()
        {
            using namespace Shared_Gamefuncs;
            using namespace MGS3_LinkVarBuf;
            using namespace MGS3Stages;

            if (!GM_SetArea || !GCL_ChangeSenerioCode || !GM_LoadRequest)
            {
                return;
            }

            GM_SetArea(GM_SaveArea, SELECT);
            GCL_ChangeSenerioCode(STRCODE_SCENERIO_GCX);
            GM_Result = 9999;
            *GM_LoadRequest = 0x3;
        };


        // What the pause menu's own Exit tail-jumps to. It clears GV_PAUSE_STOP but not the menu bits.
        using VoidFn_t = void(__fastcall*)();
        using GV_PauseOffActorSystem_t = void(__fastcall*)(uint32_t bits);
        static VoidFn_t GM_ReturnToTitle = nullptr;
        static GV_PauseOffActorSystem_t GV_PauseOffActorSystem = nullptr;
        static int* GM_PauseRequest = nullptr;

        GM_ReturnToTitle = reinterpret_cast<VoidFn_t>(Memory::PatternScan(baseModule, "48 83 EC 28 B9 01 00 00 00 C7 05 ?? ?? ?? ?? 00 00 00 00 E8 ?? ?? ?? ?? 48 8B 05 ?? ?? ?? ?? 48 8D 15 ?? ?? ?? ?? 8B 48 10 E8", "MGS 3: GM_ReturnToTitle"));
        spdlog::info("MGS3_GameFuncs: GM_ReturnToTitle address is {:s}+{:X}", sExeName.c_str(), reinterpret_cast<uintptr_t>(GM_ReturnToTitle) - reinterpret_cast<uintptr_t>(baseModule));

        if (uint8_t* pauseOff = Memory::PatternScan(baseModule, "F7 D1 21 0D ?? ?? ?? ?? C3 CC CC CC CC CC CC CC 09 0D ?? ?? ?? ?? C3", "MGS 3: GV_PauseOffActorSystem"))
        {
            GV_PauseOffActorSystem = reinterpret_cast<GV_PauseOffActorSystem_t>(pauseOff);
            PauseLevel = reinterpret_cast<uint32_t*>(Memory::GetRipRelativeAddress(pauseOff + 2, 2, 6));
        }

        if (uint8_t* gateScan = Memory::PatternScan(baseModule, "39 1D ?? ?? ?? ?? 74 4F E8 ?? ?? ?? ?? 40 84 C6 75 45 E8 ?? ?? ?? ?? 85 C0 74 3C B9 0E 00 00 00", "MGS 3: ActGame() load gate"))
        {
            GM_PauseRequest = reinterpret_cast<int*>(Memory::GetRipRelativeAddress(gateScan + 0x20, 3, 7));
        }

        if (uint8_t* chordScan = Memory::PatternScan(baseModule, "81 3D ?? ?? ?? ?? 09 0F 00 00 75 19 39 1D", "MGS 3: ActGame() pad reset chord"))
        {
            PadDirectStatus = reinterpret_cast<uint32_t*>(Memory::GetRipRelativeAddress(chordScan, 2, 10));
            GM_PadResetDisable = reinterpret_cast<int*>(Memory::GetRipRelativeAddress(chordScan + 0xC, 2, 6));
            SoftResetChord = 0xF09;
            spdlog::info("MGS3_GameFuncs: GV_PadDataDirect status is {:s}+{:X}, GM_PadResetDisable is {:s}+{:X}",
                sExeName.c_str(), reinterpret_cast<uintptr_t>(PadDirectStatus) - reinterpret_cast<uintptr_t>(baseModule),
                sExeName.c_str(), reinterpret_cast<uintptr_t>(GM_PadResetDisable) - reinterpret_cast<uintptr_t>(baseModule));
        }

        SoftReset = []()
        {
            if (!GM_ReturnToTitle || !GV_PauseOffActorSystem || !GM_PauseRequest)
            {
                return;
            }

            GV_PauseOffActorSystem(0xE);
            *GM_PauseRequest = 0;
            GM_ReturnToTitle();
        };

        if (Shared_Gamefuncs::SoftResetChordEnabled)
        {
            DisableShellSoftReset("48 83 EC 38 48 8B 0D ?? ?? ?? ?? 0F 29 74 24 20 E8 ?? ?? ?? ?? F3 0F 10 35 ?? ?? ?? ?? BA 09 0F 00 00 33 C9");
        }
        HookActGameTail("89 1D ?? ?? ?? ?? E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? E8", "MGS 3: ActGame() tail | Soft reset chord");

        if (uint8_t* Act_addr = Memory::PatternScan(baseModule, "48 83 EC ?? 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 ?? 48 89 5C 24", "GM_StartDaemon() -> Act() | Developer menu / soft reset"))
        {
            static SafetyHookMid Act_Startup_hook {};


            Act_Startup_hook = safetyhook::create_mid(Act_addr + 0x406, [](SafetyHookContext& ctx)
                {
                    if (StartInDebugMode)
                    {
                        static bool startup = true;
                        if (startup)
                        {
                            startup = false;
                            EnterDeveloperMenu();
                            return;
                        }
                    }
                });

            LOG_HOOK(Act_Startup_hook, "GM_StartDaemon() -> Act()+0x406 | Developer menu / soft reset");
        }

        if (StartInDebugMode)
        {
            RegisterDevMenuHotkey();
        }
    }


}



void Shared_Gamefuncs::HookFuncs()
{

#if !defined(RELEASE_BUILD)

    spdlog::info("Shared_GameFuncs: Hooking CHARA stage function table.");

    uint8_t* GM_GetArea_scan = Memory::PatternScan(baseModule, "E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 4C 8D 0D ?? ?? ?? ?? 4C 8B D0", "GM_GetArea call site");
    GM_GetArea = reinterpret_cast<GM_GetArea_t*>(Memory::ResolveCall(GM_GetArea_scan));
    spdlog::info("Shared_GameFuncs: GM_GetArea address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_GetArea - (uintptr_t)baseModule);

    uint8_t* GM_GetCharaID_scan = Memory::PatternScan(baseModule, eGameType & MGS2 ? "E8 ?? ?? ?? ?? 48 8B F0 48 85 C0 75 ?? 8D 46 ?? 48 83 C4" : "E8 ?? ?? ?? ?? 48 8B D8 48 85 C0 75 ?? 48 8D 43 ?? 48 83 C4 ?? 5B", "GM_GetCharaID call site");
    GM_GetCharaID_hook = safetyhook::create_inline(Memory::ResolveCall(GM_GetCharaID_scan), hooked_GM_GetCharaID);
    spdlog::info("Shared_GameFuncs: GM_GetCharaID address is {:s}+{:X}", sExeName.c_str(), (uintptr_t)GM_GetCharaID_hook.target() - (uintptr_t)baseModule);

#endif




    switch (eGameType)
    {
    case MGS2:
        MGS2_GameFuncs::HookGameFuncs();
        break;
    case MGS3:
        MGS3_Gamefuncs::HookGameFuncs();
        break;
    case MG:
        MG1_Gamefuncs::HookGameFuncs();
        break;
    default:
        return;

    }
}

#if defined (RELEASE_CLEARED)
#define RELEASE_BUILD
#endif

void Shared_Gamefuncs::ServiceSoftReset()
{
    ServiceSoftResetImpl();
}
