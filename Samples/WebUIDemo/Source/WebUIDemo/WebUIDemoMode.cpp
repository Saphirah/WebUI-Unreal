#include "WebUIDemoMode.h"
#include "Blueprint/UserWidget.h"
#include "Blueprint/WidgetTree.h"
#include "GameFramework/PlayerController.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TimerManager.h"
#include "Misc/CommandLine.h"
#include "ImageUtils.h"
#include "Framework/Application/SlateApplication.h"
#include "InputKeyEventArgs.h"
#include "Layout/WidgetPath.h"

AWebUIDemoMode::AWebUIDemoMode() { HUDClass = AWebUIDemoHUD::StaticClass(); PlayerControllerClass = AWebUIDemoController::StaticClass(); }
bool AWebUIDemoController::InputKey(const FInputKeyEventArgs& Params)
{
    if (Params.Key == EKeys::LeftMouseButton && Params.Event == IE_Pressed) ++GameClicks;
    return Super::InputKey(Params);
}
void AWebUIDemoHUD::BeginPlay()
{
    Super::BeginPlay();
    if (FParse::Param(FCommandLine::Get(), TEXT("WebUIClickSmoke"))||FParse::Param(FCommandLine::Get(),TEXT("WebUIAB")))
        FSlateApplication::Get().UsePlatformCursorForCursorUser(false); // Virtual cursor; never move the user's OS pointer.
    Host = CreateWidget<UUserWidget>(GetOwningPlayerController(), UWebUIDemoScreen::StaticClass());
    Host->WidgetTree = NewObject<UWidgetTree>(Host);
    Browser = Host->WidgetTree->ConstructWidget<UWebUIWidget>();
    FParse::Value(FCommandLine::Get(), TEXT("WebUIFPS="), Browser->BrowserFrameRate);
    if(FParse::Param(FCommandLine::Get(),TEXT("WebUILegacy")))Browser->bIndependentBrowser=false;
    Host->WidgetTree->RootWidget = Browser;
    Browser->OnInterfaceEvent.AddDynamic(this, &AWebUIDemoHUD::Message);
    Browser->OnPixelsRead.AddDynamic(this, &AWebUIDemoHUD::Pixels);
    Browser->OnError.AddDynamic(this, &AWebUIDemoHUD::Error);
    const bool HitBenchmark = FParse::Param(FCommandLine::Get(), TEXT("WebUIHitBenchmark"));
    const bool AB=FParse::Param(FCommandLine::Get(),TEXT("WebUIAB"));
    if(AB)Browser->OnLoadCompleted.AddDynamic(this,&AWebUIDemoHUD::SetupABBenchmark);
    if (HitBenchmark)
    {
        Browser->bClickThrough = false; // Isolate request/reply cost from production polling and GPU alpha readbacks.
        Browser->OnLoadCompleted.AddDynamic(this, &AWebUIDemoHUD::SetupHitBenchmark);
        Browser->OnHitTestProbeReply.AddUObject(this, &AWebUIDemoHUD::ReceiveHitProbe);
    }
    FString HitURL;
    if ((HitBenchmark||AB) && FParse::Value(FCommandLine::Get(), TEXT("WebUIHitURL="), HitURL))
    {
        Browser->TrustedOrigins.Add(TEXT("https://valyon.studio"));
        Browser->TrustedOrigins.Add(TEXT("https://www.valyon.studio"));
        Browser->LoadURL(HitURL);
    }
    else Browser->Load(AB ? TEXT("WebUI/ab.html") : HitBenchmark ? TEXT("WebUI/hit-benchmark.html") :
        FParse::Param(FCommandLine::Get(), TEXT("WebUIPerformance")) ? TEXT("WebUI/performance.html") :
        FParse::Param(FCommandLine::Get(), TEXT("WebUISelectionSmoke")) ? TEXT("WebUI/selection.html") :
        FParse::Param(FCommandLine::Get(), TEXT("WebUIDOMSmoke")) ? TEXT("WebUI/pointer-events.html") :
        FParse::Param(FCommandLine::Get(), TEXT("WebUIReact")) ? TEXT("WebUIReact/index.html") : TEXT("WebUI/index.html"));
    Host->AddToViewport();
    Browser->Focus(GetOwningPlayerController());
    GetOwningPlayerController()->bEnableClickEvents = true;
}
void AWebUIDemoHUD::Message(const FString& Name, const FWebUIJson& Data, const FString& Callback)
{
    if(Name.StartsWith(TEXT("ab"))){ABMessage(Name,Data);return;}
    UE_LOG(LogTemp, Display, TEXT("WEBUI_EVENT %s %s"), *Name, *UWebUIJsonLibrary::Stringify(Data));
    if (Name == TEXT("pointerDown")) ++WebClicks;
    if (Name == TEXT("hitBenchmarkReady"))
    {
        if(Browser->bIndependentBrowser){RunIndependentHitBenchmark();return;}
        HitBenchmarkStart = FPlatformTime::Seconds(); HitBenchmarkFrame = GFrameCounter;
        GetWorldTimerManager().SetTimer(HitBenchmarkTimer, this, &AWebUIDemoHUD::TickHitBenchmark, 0.001f, true);
    }
    if (Name == TEXT("hitBenchmarkDOM")) FinishHitBenchmark(Data);
    if (Name == TEXT("performanceStart"))
    {
        PerformancePaintStart = Browser->GetPaintCount(); PerformanceStart = FPlatformTime::Seconds();
        PerformanceFrameStart = GFrameCounter;
        Browser->Call(TEXT("measure"), UWebUIJsonLibrary::Null());
    }
    if (Name == TEXT("performanceResult"))
    {
        const double Seconds = FPlatformTime::Seconds() - PerformanceStart;
        const FString Report = FString::Printf(TEXT("paintHz=%.2f engineHz=%.2f seconds=%.2f requested=%d page=%s"),
            (Browser->GetPaintCount() - PerformancePaintStart) / Seconds, (GFrameCounter - PerformanceFrameStart) / Seconds, Seconds, Browser->BrowserFrameRate,
            *UWebUIJsonLibrary::Stringify(Data));
        UE_LOG(LogTemp, Display, TEXT("WEBUI_PERFORMANCE %s"), *Report);
        FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("WebUIPerformance.txt")));
        FPlatformMisc::RequestExit(false);
    }
    if (Name == TEXT("selectionResult"))
    {
        bool Found, Pass = false;
        UWebUIJsonLibrary::AsBoolean(UWebUIJsonLibrary::Field(Data, TEXT("pass"), Found), Pass);
        FFileHelper::SaveStringToFile(UWebUIJsonLibrary::Stringify(Data), *(FPaths::ProjectSavedDir() / TEXT("WebUISelectionSmoke.txt")));
        FPlatformMisc::RequestExitWithStatus(false, Pass ? 0 : 1);
    }
    if (!Callback.IsEmpty()) Browser->Call(Callback, Data);
    if (Name == TEXT("domStep"))
    {
        bool Found, ExpectedWeb = false; double U = 0, V = 0;
        UWebUIJsonLibrary::AsNumber(UWebUIJsonLibrary::Field(Data, TEXT("u"), Found), U);
        UWebUIJsonLibrary::AsNumber(UWebUIJsonLibrary::Field(Data, TEXT("v"), Found), V);
        UWebUIJsonLibrary::AsBoolean(UWebUIJsonLibrary::Field(Data, TEXT("web"), Found), ExpectedWeb);
        const FGeometry Geometry = Browser->GetCachedGeometry();
        const FVector2D Position = Geometry.LocalToAbsolute(Geometry.GetLocalSize() * FVector2D(U, V));
        auto& Slate = FSlateApplication::Get(); Slate.SetCursorPos(Position);
        const TSet<FKey> Buttons;
        Slate.ProcessMouseMoveEvent(FPointerEvent(0, Position, Position, Buttons, EKeys::Invalid, 0, FModifierKeysState()), true);
        FTimerHandle Timer;
        GetWorldTimerManager().SetTimer(Timer, [this, ExpectedWeb] { ClickDOMPointer(ExpectedWeb); }, 1.0f, false);
    }
    if (Name == TEXT("domResult"))
    {
        bool Found, Pass = false;
        UWebUIJsonLibrary::AsBoolean(UWebUIJsonLibrary::Field(Data, TEXT("pass"), Found), Pass);
        bDOMPassed &= Pass;
        if (++DOMStep < 9) Browser->Call(TEXT("prepareStep"), UWebUIJsonLibrary::Number(DOMStep));
        else
        {
            const int32 GameClicks = CastChecked<AWebUIDemoController>(GetOwningPlayerController())->GameClicks;
            bDOMPassed &= GameClicks == 4 && WebClicks == 5;
            const FString Report = FString::Printf(TEXT("pass=%d steps=%d gameClicks=%d webClicks=%d"), bDOMPassed, DOMStep, GameClicks, WebClicks);
            FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("WebUIDOMSmoke.txt")));
            UE_LOG(LogTemp, Display, TEXT("WEBUI_DOM %s"), *Report);
            FPlatformMisc::RequestExitWithStatus(false, bDOMPassed ? 0 : 1);
        }
    }
    if (Name == TEXT("ready"))
    {
        Browser->Call(TEXT("setStatus"), UWebUIJsonLibrary::String(TEXT("Connected to Unreal Engine")));
        FTimerHandle Timer;
        GetWorldTimerManager().SetTimer(Timer, [Weak = TWeakObjectPtr<UWebUIWidget>(Browser)] { if (Weak.IsValid()) Weak->ReadTexturePixels(); }, 2.0f, false);
        if (FParse::Param(FCommandLine::Get(), TEXT("WebUIDOMSmoke")))
        {
            FTimerHandle TimerDOM;
            GetWorldTimerManager().SetTimer(TimerDOM, [this] { Browser->Call(TEXT("prepareStep"), UWebUIJsonLibrary::Number(0)); }, 3.0f, false);
        }
        else if (FParse::Param(FCommandLine::Get(), TEXT("WebUIClickSmoke")))
        {
            Browser->ExecuteJavaScript(TEXT("document.addEventListener('pointerdown',()=>ue5('pointerDown'),true)"));
            FTimerHandle PointerTimer;
            GetWorldTimerManager().SetTimer(PointerTimer, [this] { TestPointer(false); }, 3.0f, false);
        }
    }
}
void AWebUIDemoHUD::Pixels(const TArray<FColor>& Data, FIntPoint Size, const FString& ErrorMessage)
{
    int32 Transparent = 0, Opaque = 0;
    for (const FColor& C : Data) { if (C.A == 0) ++Transparent; if (C.A == 255) ++Opaque; }
    const FString Report = FString::Printf(TEXT("shared=%d size=%dx%d transparent=%d opaque=%d error=%s"), Browser->HasSharedGPUTexture(), Size.X, Size.Y, Transparent, Opaque, *ErrorMessage);
    UE_LOG(LogTemp, Display, TEXT("WEBUI_GPU %s"), *Report);
    FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("WebUISmoke.txt")));
    if (Size.X > 0 && Size.Y > 0 && Data.Num() == Size.X * Size.Y)
    {
        TArray64<uint8> PNG; FImageUtils::PNGCompressImageArray(Size.X, Size.Y, Data, PNG);
        FFileHelper::SaveArrayToFile(PNG, *(FPaths::ProjectSavedDir() / TEXT("WebUISmoke.png")));
    }
    if (FParse::Param(FCommandLine::Get(), TEXT("WebUISmoke")) && !FParse::Param(FCommandLine::Get(), TEXT("WebUIClickSmoke"))) FPlatformMisc::RequestExit(false);
}
void AWebUIDemoHUD::Error(const FString& Message) { UE_LOG(LogTemp, Error, TEXT("WEBUI_ERROR %s"), *Message); }
void AWebUIDemoHUD::ClickDOMPointer(bool bExpectedWeb)
{
    auto& Slate = FSlateApplication::Get();
    const FVector2D Position = Slate.GetCursorPos();
    const FWidgetPath Path = Slate.LocateWindowUnderMouse(Position, Slate.GetInteractiveTopLevelWindows());
    const bool InBrowser = Path.ContainsWidget(&Browser->TakeWidget().Get());
    bDOMPassed &= Path.IsValid() && InBrowser == bExpectedWeb;
    UE_LOG(LogTemp, Display, TEXT("WEBUI_DOM_PATH step=%d expected=%d actual=%d"), DOMStep, bExpectedWeb, InBrowser);
    TSet<FKey> Buttons { EKeys::LeftMouseButton };
    Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(0, Position, Position, Buttons, EKeys::LeftMouseButton, 0, FModifierKeysState()));
    Buttons.Reset();
    Slate.ProcessMouseButtonUpEvent(FPointerEvent(0, Position, Position, Buttons, EKeys::LeftMouseButton, 0, FModifierKeysState()));
    FTimerHandle Timer;
    GetWorldTimerManager().SetTimer(Timer, [this] { Browser->Call(TEXT("inspectStep"), UWebUIJsonLibrary::Null()); }, 0.3f, false);
}
void AWebUIDemoHUD::TestPointer(bool bPanel)
{
    auto& Slate = FSlateApplication::Get();
    const FGeometry Geometry = Browser->GetCachedGeometry();
    const FVector2D Local = bPanel ? FVector2D(200, 160) : Geometry.GetLocalSize() * 0.8;
    const FVector2D Position = Geometry.LocalToAbsolute(Local);
    Slate.SetCursorPos(Position);
    TSet<FKey> Buttons;
    Slate.ProcessMouseMoveEvent(FPointerEvent(0, Position, Position, Buttons, EKeys::Invalid, 0, FModifierKeysState()), true);
    FTimerHandle Timer;
    GetWorldTimerManager().SetTimer(Timer, [this, bPanel] { ClickPointer(bPanel); }, 2.0f, false);
}
void AWebUIDemoHUD::ClickPointer(bool bPanel)
{
    auto& Slate = FSlateApplication::Get();
    const FVector2D Position = Slate.GetCursorPos();
    FWidgetPath Path = Slate.LocateWindowUnderMouse(Position, Slate.GetInteractiveTopLevelWindows());
    const bool ContainsBrowser = Path.ContainsWidget(&Browser->TakeWidget().Get());
    if (bPanel) bPanelPath = ContainsBrowser; else bClearPath = !ContainsBrowser;
    UE_LOG(LogTemp, Display, TEXT("WEBUI_HIT panel=%d browser=%d path=%d"), bPanel, ContainsBrowser, Path.IsValid());
    TSet<FKey> Buttons { EKeys::LeftMouseButton };
    Slate.ProcessMouseButtonDownEvent(nullptr, FPointerEvent(0, Position, Position, Buttons, EKeys::LeftMouseButton, 0, FModifierKeysState()));
    Buttons.Reset();
    Slate.ProcessMouseButtonUpEvent(FPointerEvent(0, Position, Position, Buttons, EKeys::LeftMouseButton, 0, FModifierKeysState()));
    FTimerHandle Timer;
    if (!bPanel) GetWorldTimerManager().SetTimer(Timer, [this] { TestPointer(true); }, 0.5f, false);
    else GetWorldTimerManager().SetTimer(Timer, this, &AWebUIDemoHUD::FinishPointerTest, 0.5f, false);
}
void AWebUIDemoHUD::FinishPointerTest()
{
    const int32 GameClicks = CastChecked<AWebUIDemoController>(GetOwningPlayerController())->GameClicks;
    const bool bPass = bClearPath && bPanelPath && GameClicks == 1 && WebClicks == 1;
    const FString Report = FString::Printf(TEXT("pass=%d clearPath=%d panelPath=%d gameClicks=%d webClicks=%d"), bPass, bClearPath, bPanelPath, GameClicks, WebClicks);
    UE_LOG(LogTemp, Display, TEXT("WEBUI_CLICK %s"), *Report);
    FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir() / TEXT("WebUIClickSmoke.txt")));
    FPlatformMisc::RequestExitWithStatus(false, bPass ? 0 : 1);
}
