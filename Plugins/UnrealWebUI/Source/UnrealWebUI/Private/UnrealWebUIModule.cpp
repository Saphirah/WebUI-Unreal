#include "Modules/ModuleManager.h"
#include "WebBrowserModule.h"
#include "IWebBrowserSingleton.h"
#include "WebUIResources.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "Windows/WindowsHWrapper.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformProcess.h"

extern int32 GWebUIBrowserCount;
static TAutoConsoleVariable<int32> CVarWebUIPumpEveryFrame(TEXT("webui.PumpEveryFrame"), 1,
    TEXT("CEF scheduling: 0=engine default; 1=also pump at frame start; 2=also pump before Slate. Main thread only, while WebUI is active."));

class FUnrealWebUIModule : public IModuleInterface
{
    TUniquePtr<FWebUIResourceFactory> Resources;
    FDelegateHandle PumpHandle;
    FDelegateHandle SlatePumpHandle;
    void PumpBeforeSlate(float) { if (CVarWebUIPumpEveryFrame.GetValueOnGameThread() >= 2) Pump(); }
    void Pump()
    {
        if (GWebUIBrowserCount == 0 || CVarWebUIPumpEveryFrame.GetValueOnGameThread() == 0) return;
        static bool bInPump = false;
        if (bInPump) return;
        TGuardValue<bool> Guard(bInPump, true);
        // UE 5.7 initializes CEF on the game thread with multi_threaded_message_loop=false.
        // Resolve only the engine's already-loaded CEF; do not load a second runtime.
        using FPump = void (*)();
        if (const HMODULE CEF = GetModuleHandleW(L"libcef.dll"))
            if (const auto Work = reinterpret_cast<FPump>(FPlatformProcess::GetDllExport(CEF, TEXT("cef_do_message_loop_work")))) Work();
    }
public:
    void StartupModule() override
    {
        if (IsRunningDedicatedServer() || IsRunningCommandlet()) return;
        Resources = MakeUnique<FWebUIResourceFactory>();
        if (auto* Browser = IWebBrowserModule::Get().GetSingleton())
        {
            PumpHandle = FCoreDelegates::OnBeginFrame.AddRaw(this, &FUnrealWebUIModule::Pump);
            if (FSlateApplication::IsInitialized())
                SlatePumpHandle = FSlateApplication::Get().OnPreTick().AddRaw(this, &FUnrealWebUIModule::PumpBeforeSlate);
            Browser->RegisterSchemeHandlerFactory(TEXT("https"), TEXT("ue.local"), Resources.Get());
#if !UE_BUILD_SHIPPING
            Browser->SetDevToolsShortcutEnabled(true);
#endif
        }
    }
    void ShutdownModule() override
    {
        FCoreDelegates::OnBeginFrame.Remove(PumpHandle);
        if (FSlateApplication::IsInitialized()) FSlateApplication::Get().OnPreTick().Remove(SlatePumpHandle);
        if (Resources && FModuleManager::Get().IsModuleLoaded(TEXT("WebBrowser")))
            if (auto* Browser = IWebBrowserModule::Get().GetSingleton()) Browser->UnregisterSchemeHandlerFactory(Resources.Get());
        Resources.Reset();
    }
};
IMPLEMENT_MODULE(FUnrealWebUIModule, UnrealWebUI)
