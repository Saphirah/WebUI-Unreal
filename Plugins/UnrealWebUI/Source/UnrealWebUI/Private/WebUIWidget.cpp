#include "WebUIWidget.h"
#include "WebUIResources.h"
#include "WebUITexture.h"
#include "WebBrowserModule.h"
#include "IWebBrowserSingleton.h"
#include "IWebBrowserWindow.h"
#include "SWebBrowser.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Engine/GameViewportClient.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/SNullWidget.h"
#include "RenderingThread.h"
#include "HAL/IConsoleManager.h"
#include "RHI.h"
#include "SWebUIRemote.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

int32 GWebUIBrowserCount = 0;
static TAutoConsoleVariable<int32> CVarWebUILogPerformance(TEXT("webui.LogPerformance"), 0,
    TEXT("Log browser redraw notifications, game frame rate and active input settings every five seconds."));

class SWebUIRoot : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SWebUIRoot) {} SLATE_END_ARGS()
    TWeakObjectPtr<UWebUIWidget> Owner;
    void Construct(const FArguments&, UWebUIWidget* InOwner, TSharedRef<SWidget> Child)
    { Owner = InOwner; SetVisibility(EVisibility::SelfHitTestInvisible); ChildSlot[Child]; }
    void Tick(const FGeometry& Geometry, double Time, float Delta) override
    { SCompoundWidget::Tick(Geometry, Time, Delta); if (Owner.IsValid()) Owner->TickBrowser(Geometry); }
    TOptional<TSharedRef<SWidget>> OnMapCursor(const FCursorReply& Reply) const override
    {
        if (Owner.IsValid() && Owner->bCustomCursors)
        {
            if (auto* World = Owner->GetWorld())
                if (auto* Viewport = World->GetGameViewport())
                    if (auto Mapped = Viewport->MapCursor(nullptr, Reply); Mapped.IsSet()) return Mapped;
            return SNullWidget::NullWidget;
        }
        return SCompoundWidget::OnMapCursor(Reply);
    }
};

UWebUIWidget::UWebUIWidget(const FObjectInitializer& ObjectInitializer) : Super(ObjectInitializer)
{
    // UWidget reapplies this property to the returned root after RebuildWidget.
    // A Visible root would itself remain in the hit grid after its browser child opts out.
    SetVisibility(ESlateVisibility::SelfHitTestInvisible);
}
TSharedRef<SWidget> UWebUIWidget::RebuildWidget()
{
    if (IsDesignTime()) return SNew(STextBlock).Text(FText::FromString(TEXT("Web Interface - Chromium / D3D12")));
    if (!FSlateApplication::IsInitialized()) return SNullWidget::NullWidget;
    const ERHIInterfaceType RHI = RHIGetInterfaceType();
    if (RHI != ERHIInterfaceType::D3D11 && RHI != ERHIInterfaceType::D3D12)
        OnError.Broadcast(TEXT("GPU sharing requires the D3D12 or D3D11 RHI."));
    if(bIndependentBrowser)
    {
        Remote=MakeShared<FWebUIRemoteSession,ESPMode::ThreadSafe>();
        if(!Remote->Start(InitialURL,BrowserFrameRate,bEnableBridge,bAllowTextSelection))
        {OnError.Broadcast(Remote->LastError());Remote.Reset();return SNullWidget::NullWidget;}
        Browser=SNew(SWebUIRemote,Remote.ToSharedRef());
        Root=SNew(SWebUIRoot,this,Browser.ToSharedRef());
        return Root.ToSharedRef();
    }
    const auto* GPU = IConsoleManager::Get().FindConsoleVariable(TEXT("r.CEFGPUAcceleration"));
    if (GPU && GPU->GetInt() == 0) OnError.Broadcast(TEXT("r.CEFGPUAcceleration is disabled. Enable it before CEF initializes."));
    FCreateBrowserWindowSettings Settings;
    Settings.InitialURL = TEXT("about:blank"); // Bind before navigating to application code.
    Settings.bUseTransparency = bTransparent;
    Settings.BackgroundColor = bTransparent ? FColor::Transparent : FColor::White;
    Settings.BrowserFrameRate = FMath::Clamp(BrowserFrameRate, 1, 120);
    UE_LOG(LogTemp, Display, TEXT("WebUI browser requested FPS=%d (creation setting)"), Settings.BrowserFrameRate);
    Settings.bThumbMouseButtonNavigation = false;
    auto* Singleton = IWebBrowserModule::Get().GetSingleton();
    Window = Singleton ? Singleton->CreateBrowserWindow(Settings) : nullptr;
    if (!Window) { OnError.Broadcast(TEXT("CEF browser creation failed.")); return SNullWidget::NullWidget; }
    PaintCount = 0;
    ++GWebUIBrowserCount;
    PerformanceTime = FPlatformTime::Seconds(); PerformanceFrame = GFrameCounter; PerformancePaint = 0;
    Window->OnNeedsRedraw().AddUObject(this, &UWebUIWidget::CountPaint);
    if (bEnableBridge)
    {
        Bridge = NewObject<UWebUIBridge>(this); Bridge->Owner = this;
        Window->BindUObject(TEXT("webui"), Bridge, true);
    }
    LegacyBrowser = SNew(SWebBrowser, Window)
        .ShowControls(false).ShowAddressBar(false).ShowInitialThrobber(false)
        .SupportsTransparency(bTransparent).SupportsThumbMouseButtonNavigation(false)
        .OnLoadCompleted(FSimpleDelegate::CreateUObject(this, &UWebUIWidget::Loaded))
        .OnLoadStarted_Lambda([Weak = TWeakObjectPtr<UWebUIWidget>(this)]
        { if (auto* Self = Weak.Get()) {
            Self->bHaveAlpha = false; Self->bHaveDOMHit = false; Self->PendingDOMRequest = 0;
            Self->NextDOMQuery = 0; Self->OnLoadStarted.Broadcast();
        } })
        .OnLoadError_Lambda([Weak = TWeakObjectPtr<UWebUIWidget>(this)]
        { if (auto* Self = Weak.Get()) Self->OnError.Broadcast(FString::Printf(TEXT("Browser load failed (%d)."), Self->Window->GetLoadError())); })
        .OnUrlChanged_Lambda([Weak = TWeakObjectPtr<UWebUIWidget>(this)](const FText& URL)
        { if (auto* Self = Weak.Get()) Self->OnURLChanged.Broadcast(URL.ToString()); })
        .OnBeforeNavigation_Lambda([Weak = TWeakObjectPtr<UWebUIWidget>(this)](const FString& URL, const FWebNavigationRequest&)
        { auto* Self = Weak.Get(); return !Self || (Self->bEnableBridge && !Self->IsTrustedURL(URL)); })
        .OnBeforePopup_Lambda([](FString, FString) { return true; })
        .OnConsoleMessage_Lambda([](const FString& Message, const FString&, int32 Line, EWebBrowserConsoleLogSeverity)
        { UE_LOG(LogTemp, Log, TEXT("WebUI JS [%d]: %s"), Line, *Message); })
        .OnCreateWindow_Lambda([](const TWeakPtr<IWebBrowserWindow>& NewWindow, const TWeakPtr<IWebBrowserPopupFeatures>&)
        {
#if !UE_BUILD_SHIPPING
            if (auto Child = NewWindow.Pin())
            {
                auto Tools = SNew(SWindow).Title(FText::FromString(TEXT("Web UI DevTools"))).ClientSize(FVector2D(1100, 750));
                Tools->SetContent(SNew(SWebBrowser, Child).ShowControls(false).ShowAddressBar(false));
                FSlateApplication::Get().AddWindow(Tools); return true;
            }
#endif
            return false;
        });
    Browser=LegacyBrowser;
    LegacyBrowser->BindInputMethodSystem(FSlateApplication::Get().GetTextInputMethodSystem());
    Root = SNew(SWebUIRoot, this, Browser.ToSharedRef());
    if (bHasPendingHTML) Window->LoadString(PendingHTML, InitialURL);
    else LoadURL(InitialURL);
    return Root.ToSharedRef();
}
void UWebUIWidget::ReleaseSlateResources(bool bReleaseChildren)
{
    ++Generation;
    PendingDOMRequest = 0; bHaveDOMHit = false; NextDOMQuery = 0;
    if (LegacyBrowser) LegacyBrowser->UnbindInputMethodSystem();
    if (Window) Window->OnNeedsRedraw().RemoveAll(this);
    if (Window) --GWebUIBrowserCount;
    if (Window && Bridge) Window->UnbindUObject(TEXT("webui"), Bridge, true);
    if (Bridge) Bridge->Owner.Reset();
    // Drain queued readers before releasing the browser's Slate resources.
    FlushRenderingCommands();
    AlphaRead.Reset(); ExplicitRead.Reset();
    if (Window) Window->CloseBrowser(true, false);
    if(Remote)Remote->Close();
    Root.Reset(); Browser.Reset(); LegacyBrowser.Reset(); Window.Reset(); Remote.Reset(); Bridge = nullptr; bHaveAlpha = false;bRemoteLoaded=false;
    Super::ReleaseSlateResources(bReleaseChildren);
}
bool UWebUIWidget::IsTrustedURL(const FString& URL) const
{
    if (URL == TEXT("about:blank") || URL.StartsWith(TEXT("https://ue.local/"), ESearchCase::IgnoreCase)) return true;
    for (FString Origin : TrustedOrigins)
    {
        Origin.RemoveFromEnd(TEXT("/"));
        if (!Origin.IsEmpty() && (URL.Equals(Origin, ESearchCase::IgnoreCase) || URL.StartsWith(Origin + TEXT("/"), ESearchCase::IgnoreCase))) return true;
    }
    if (URL.StartsWith(TEXT("file:///")))
    {
        FString Path = FGenericPlatformHttp::UrlDecode(URL.Mid(8));
        int32 Hash; if (Path.FindChar('#', Hash)) Path.LeftInline(Hash);
        FPaths::NormalizeFilename(Path);
        for (const FString& Dir : { FPaths::ProjectDir() / TEXT("UI"), FPaths::ProjectContentDir() })
        {
            FString Base = FPaths::ConvertRelativePathToFull(Dir); FPaths::NormalizeDirectoryName(Base);
            if (Path.StartsWith(Base + TEXT("/"), ESearchCase::IgnoreCase) && !Path.Contains(TEXT(".."))) return true;
        }
    }
    return false;
}
bool UWebUIWidget::Load(const FString& Relative)
{
    FString Path;
    if (!WebUIResources::Resolve(FPaths::ProjectContentDir(), Relative, Path)) { OnError.Broadcast(TEXT("Invalid Content-relative path.")); return false; }
    return LoadURL(WebUIResources::ContentURL(Relative));
}
bool UWebUIWidget::LoadURL(const FString& URL)
{
    if (URL.StartsWith(TEXT("pak://"))) return Load(URL.Mid(6));
    if (bEnableBridge && !IsTrustedURL(URL)) { OnError.Broadcast(TEXT("Navigation blocked: add the application's exact origin to TrustedOrigins, or disable the bridge.")); return false; }
    InitialURL = URL; bHasPendingHTML = false; PendingScripts.Reset();
    if (Window) Window->LoadURL(URL);
    if (Remote) Remote->Load(URL);
    return true;
}
bool UWebUIWidget::LoadHTML(const FString& HTML, const FString& BaseURL)
{
    if (bEnableBridge && !IsTrustedURL(BaseURL)) { OnError.Broadcast(TEXT("Untrusted HTML base URL.")); return false; }
    InitialURL = BaseURL; PendingHTML = HTML; bHasPendingHTML = true; PendingScripts.Reset();
    if (Window) Window->LoadString(HTML, BaseURL);
    if (Remote) Remote->LoadHTML(HTML,BaseURL);
    return true;
}
bool UWebUIWidget::LoadFile(const FString& Relative, bool bFromContent)
{
    FString Path;
    if (!WebUIResources::Resolve(bFromContent ? FPaths::ProjectContentDir() : FPaths::ProjectDir() / TEXT("UI"), Relative, Path)) return false;
    FString URL = TEXT("file:///") + FGenericPlatformHttp::UrlEncode(Path);
    URL.ReplaceInline(TEXT("%2F"), TEXT("/")); URL.ReplaceInline(TEXT("%3A"), TEXT(":"));
    return LoadURL(URL);
}
bool UWebUIWidget::LoadContent(const FString& Relative, const TArray<FString>& Scripts)
{
    FString Path, HTML;
    if (!WebUIResources::Resolve(FPaths::ProjectContentDir(), Relative, Path) || !FFileHelper::LoadFileToString(HTML, *Path)) return false;
    TArray<FString> LoadedScripts;
    for (const auto& Script : Scripts)
    {
        FString Text;
        if (!WebUIResources::Resolve(FPaths::ProjectContentDir(), Script, Path) || !FFileHelper::LoadFileToString(Text, *Path)) return false;
        LoadedScripts.Add(MoveTemp(Text));
    }
    if (!LoadHTML(HTML, WebUIResources::ContentURL(Relative))) return false;
    PendingScripts = MoveTemp(LoadedScripts); return true;
}
void UWebUIWidget::Loaded()
{
    SetAllowTextSelection(bAllowTextSelection);
    if (bEnableBridge)
    {
        FString JS;
        const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealWebUI"));
        if (Plugin && FFileHelper::LoadFileToString(JS, *(Plugin->GetBaseDir() / TEXT("Resources/webui.js")))) ExecuteJavaScript(JS);
        else OnError.Broadcast(TEXT("Could not load the JavaScript bridge resource."));
    }
    for (const auto& Script : PendingScripts) ExecuteJavaScript(Script);
    PendingScripts.Reset(); OnLoadCompleted.Broadcast();
}
void UWebUIWidget::SetAllowTextSelection(bool bAllow)
{
    bAllowTextSelection = bAllow;
    FString JS;
    const auto Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealWebUI"));
    if (!Plugin || !FFileHelper::LoadFileToString(JS, *(Plugin->GetBaseDir() / TEXT("Resources/selection.js"))))
    { OnError.Broadcast(TEXT("Could not load text-selection policy.")); return; }
    JS += FString::Printf(TEXT("\nwindow.__webuiSelection.setEnabled(%s);"), bAllow ? TEXT("false") : TEXT("true"));
    ExecuteJavaScript(JS);
}
bool UWebUIWidget::Call(const FString& Function, const FWebUIJson& Data)
{
    if ((!Window&&!Remote) || !bEnableBridge || Function.IsEmpty() || !Data.Value) return false;
    const FString Name = UWebUIJsonLibrary::Stringify(UWebUIJsonLibrary::String(Function));
    const FString Value = UWebUIJsonLibrary::Stringify(Data);
    ExecuteJavaScript(FString::Printf(TEXT("(()=>{const o=window.ue&&ue.interface,k=%s;if(o&&Object.prototype.hasOwnProperty.call(o,k)&&typeof o[k]==='function')o[k](%s);else console.error('WebUI: unknown function',k);})()"), *Name, *Value));
    return true;
}
void UWebUIWidget::Receive(const FString& Name, const FString& JSON, const FString& Callback)
{
    if (!bEnableBridge || (!Window&&!Remote) || !IsTrustedURL(GetURL())) return;
    if (Name.IsEmpty() || Name.Len() > 256 || Callback.Len() > 256) { OnError.Broadcast(TEXT("Invalid bridge event or callback name.")); return; }
    FWebUIJson Value; FString Error;
    if (!UWebUIJsonLibrary::Parse(JSON.IsEmpty() ? TEXT("null") : JSON, Value, Error)) { OnError.Broadcast(Error); return; }
    OnInterfaceEvent.Broadcast(Name, Value, Callback);
}
void UWebUIBridge::Broadcast(const FString& Name, const FString& Data, const FString& Callback)
{ if (Owner.IsValid()) Owner->Receive(Name, Data, Callback); }
void UWebUIBridge::HitTestResult(int32 Request, bool bHit)
{ if (Owner.IsValid()) Owner->ReceiveHitTest(Request, bHit); }
void UWebUIWidget::ReceiveHitTest(int32 Request, bool bHit)
{
    if (Request < 0 && OnHitTestProbeReply.IsBound() && bEnableBridge && Window && IsTrustedURL(Window->GetUrl()))
    { OnHitTestProbeReply.Broadcast(Request, bHit); return; }
    if (!bEnableBridge || !bUseDOMHitTesting || !Window || !IsTrustedURL(Window->GetUrl()) ||
        Request == 0 || Request != PendingDOMRequest) return;
    PendingDOMRequest = 0;
    if (FPlatformTime::Seconds() - DOMRequestTime > 1.0) return;
    bHaveDOMHit = true; bDOMHit = bHit;
    DOMPixel = PendingDOMPixel; DOMGeometrySize = PendingDOMGeometrySize; DOMTextureSize = PendingDOMTextureSize;
    DOMSampleTime = FPlatformTime::Seconds(); // Bound reply age above; allow time for the next query.
}
void UWebUIWidget::ExecuteJavaScript(const FString& Script) { if(Remote)Remote->Eval(Script);else if (Window) Window->ExecuteJavascript(Script); }
void UWebUIWidget::Reload() { if(Remote)Remote->Command(TEXT("reload"));else if (Window) Window->Reload(); }
void UWebUIWidget::StopLoad() { if(Remote)Remote->Command(TEXT("stop"));else if (Window) Window->StopLoad(); }
void UWebUIWidget::GoBack() { if(Remote)Remote->Command(TEXT("back"));else if (Window) Window->GoBack(); }
void UWebUIWidget::GoForward() { if(Remote)Remote->Command(TEXT("forward"));else if (Window) Window->GoForward(); }
FString UWebUIWidget::GetURL() const { return Window ? Window->GetUrl() : InitialURL; }
bool UWebUIWidget::IsLoaded() const { return Remote?bRemoteLoaded:Window && Window->GetDocumentLoadingState() == EWebBrowserDocumentState::Completed; }
void UWebUIWidget::Focus(APlayerController* PC)
{
    if (!PC || !Browser || (!Window&&!Remote)) return;
    FInputModeGameAndUI Mode; Mode.SetWidgetToFocus(Browser); Mode.SetHideCursorDuringCapture(false);
    PC->SetInputMode(Mode); PC->bShowMouseCursor = true;
    FSlateApplication::Get().SetKeyboardFocus(Browser); if(Remote)Remote->Focus(true);else Window->OnFocus(true, false);
}
void UWebUIWidget::Unfocus(APlayerController* PC)
{
    if (Window) Window->OnFocus(false, false);
    if (Remote) Remote->Focus(false);
    if (PC) { PC->SetInputMode(FInputModeGameOnly()); PC->bShowMouseCursor = false; }
    FSlateApplication::Get().SetAllUserFocusToGameViewport();
}
void UWebUIWidget::ResetMousePosition(APlayerController* PC)
{ if (PC) { int32 W, H; PC->GetViewportSize(W, H); PC->SetMouseLocation(W / 2, H / 2); } }
int32 UWebUIWidget::GetTextureWidth() const { if(Remote)return Remote->Size().X;return Window && Window->GetTexture() ? Window->GetTexture()->GetWidth() : 0; }
int32 UWebUIWidget::GetTextureHeight() const { if(Remote)return Remote->Size().Y;return Window && Window->GetTexture() ? Window->GetTexture()->GetHeight() : 0; }
uint64 UWebUIWidget::GetPaintCount() const {return Remote?Remote->FrameCount():PaintCount;}
UWebUIWidget::FAsyncHitRequester UWebUIWidget::MakeAsyncHitRequester(){
    auto Session=Remote;
    return [Session](double U,double V,FAsyncHitCallback Callback){if(Session)Session->QueryHitAsync(U,V,MoveTemp(Callback));else Callback(false,-1);};
}
bool UWebUIWidget::RequestPixels(TOptional<FIntPoint> Pixel)
{
    if ((!Window&&!Remote) || ExplicitRead) return false;
    ExplicitRead = MakeShared<FWebUITextureReadback, ESPMode::ThreadSafe>();
    TOptional<FIntRect> Region;
    if (Pixel.IsSet()) Region = FIntRect(Pixel.GetValue(), Pixel.GetValue() + FIntPoint(1, 1));
    auto Done = [Weak = TWeakObjectPtr<UWebUIWidget>(this), Epoch = Generation](FWebUIPixels Result)
    {
        if (auto* Self = Weak.Get(); Self && Self->Generation == Epoch)
        {
            Self->ExplicitRead.Reset(); Self->bSharedTexture = Result.bShared;
            Self->OnPixelsRead.Broadcast(Result.Pixels, Result.Size, Result.Error);
        }
    };
    if(Remote)ExplicitRead->StartResource(Remote->CaptureTexture(),Region,MoveTemp(Done));
    else ExplicitRead->Start(Window,Region,MoveTemp(Done));
    return true;
}
bool UWebUIWidget::ReadTexturePixels() { return RequestPixels({}); }
bool UWebUIWidget::ReadTexturePixel(int32 X, int32 Y) { return RequestPixels(FIntPoint(X, Y)); }
void UWebUIWidget::TickBrowser(const FGeometry& Geometry)
{
    if(Remote&&Browser)
    {
        Remote->Tick(Geometry.GetLocalSize().IntPoint());
        FString Event;
        while(Remote->DequeueEvent(Event)){
            TSharedPtr<FJsonObject> D;if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Event),D)||!D)continue;
            const FString Op=D->GetStringField(TEXT("op"));
            if(Op==TEXT("created")){if(bHasPendingHTML)Remote->LoadHTML(PendingHTML,InitialURL);else Remote->Load(InitialURL);}
            else if(Op==TEXT("loaded")){
                if(D->GetStringField(TEXT("url"))==TEXT("about:blank")&&InitialURL!=TEXT("about:blank"))continue;
                InitialURL=D->GetStringField(TEXT("url"));bRemoteLoaded=true;Loaded();
            }
            else if(Op==TEXT("loading")){bRemoteLoaded=false;OnLoadStarted.Broadcast();}
            else if(Op==TEXT("url")){const FString URL=D->GetStringField(TEXT("url"));if(URL!=TEXT("about:blank")){InitialURL=URL;OnURLChanged.Broadcast(URL);}}
            else if(Op==TEXT("error"))OnError.Broadcast(D->GetStringField(TEXT("message")));
            else if(Op==TEXT("event")&&bEnableBridge&&IsTrustedURL(D->GetStringField(TEXT("originUrl"))))
                Receive(D->GetStringField(TEXT("name")),D->GetStringField(TEXT("data")),D->GetStringField(TEXT("callback")));
        }
        if(ExplicitRead)ExplicitRead->Poll();
        const FVector2D Size=Geometry.GetLocalSize();
        const FVector2D P=Geometry.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos());
        const bool WantInput=!bClickThrough||Remote->HitTest(FVector2D(P.X/FMath::Max(1.0,Size.X),P.Y/FMath::Max(1.0,Size.Y)));
        if(FSlateApplication::Get().GetPressedMouseButtons().IsEmpty())Browser->SetVisibility(WantInput?EVisibility::Visible:EVisibility::HitTestInvisible);
        bSharedTexture=Remote->Texture()!=nullptr;
        return;
    }
    if (!Window || !Browser) return;
    const double PerformanceNow = FPlatformTime::Seconds();
    if (PerformanceNow - PerformanceTime >= 5.0)
    {
        const double Seconds = PerformanceNow - PerformanceTime;
        if (CVarWebUILogPerformance.GetValueOnGameThread())
            UE_LOG(LogTemp, Display, TEXT("WebUI performance: delivered=%.1f/s engine=%.1f/s requested=%d bridge=%d clickThrough=%d texture=%dx%d"),
                (PaintCount - PerformancePaint) / Seconds, (GFrameCounter - PerformanceFrame) / Seconds,
                BrowserFrameRate, bEnableBridge, bClickThrough, GetTextureWidth(), GetTextureHeight());
        PerformanceTime = PerformanceNow; PerformanceFrame = GFrameCounter; PerformancePaint = PaintCount;
    }
    if (ExplicitRead) ExplicitRead->Poll();
    if (AlphaRead) AlphaRead->Poll();
    const bool bDOM = bUseDOMHitTesting && bEnableBridge && Bridge != nullptr;
    if (!bClickThrough || (!bDOM && !bTransparent)) { Browser->SetVisibility(EVisibility::Visible); return; }
    const FVector2D Local = Geometry.AbsoluteToLocal(FSlateApplication::Get().GetCursorPos());
    const FVector2D Size = Geometry.GetLocalSize();
    const FIntPoint TexSize(GetTextureWidth(), GetTextureHeight());
    if (Size.X <= 0 || Size.Y <= 0 || TexSize.X <= 0 || TexSize.Y <= 0) return;
    const FIntPoint Pixel(FMath::FloorToInt(Local.X / Size.X * TexSize.X), FMath::FloorToInt(Local.Y / Size.Y * TexSize.Y));
    const bool Inside = Local.X >= 0 && Local.Y >= 0 && Local.X < Size.X && Local.Y < Size.Y;
    // Preserve browser mouse capture through the end of a drag, including outside opaque UI.
    const bool Dragging = FSlateApplication::Get().GetPressedMouseButtons().Num() > 0;
    if (bDOM)
    {
        const double Now = FPlatformTime::Seconds();
        const bool FreshDOM = bHaveDOMHit && DOMPixel == Pixel && DOMGeometrySize == Size &&
            DOMTextureSize == TexSize && Now - DOMSampleTime < 0.75;
        const bool WantsInput = Inside && (!FreshDOM || bDOMHit);
        if (!Dragging)
        {
            const bool HadInput = Browser->GetVisibility().IsHitTestVisible();
            Browser->SetVisibility(WantsInput ? EVisibility::Visible : EVisibility::HitTestInvisible);
            if (HadInput != WantsInput)
            {
                // A stationary cursor must acquire/leave :hover after the asynchronous
                // classification changes visibility; no physical mouse move is required.
                const FVector2D Position = FSlateApplication::Get().GetCursorPos();
                const TSet<FKey> Buttons;
                const FPointerEvent Event(0, Position, Position, Buttons, EKeys::Invalid, 0, FModifierKeysState());
                if (WantsInput) Window->OnMouseMove(Geometry, Event, false);
                else Window->OnMouseLeave(Event);
            }
        }
        // Background Chromium processes can take several hundred milliseconds to reply.
        // Keep one request in flight long enough to avoid starving classification.
        if (PendingDOMRequest && Now - DOMRequestTime > 1.0) PendingDOMRequest = 0;
        if (Inside && !PendingDOMRequest && Now >= NextDOMQuery)
        {
            DOMSequence = DOMSequence == MAX_int32 ? 1 : DOMSequence + 1;
            PendingDOMRequest = DOMSequence; PendingDOMPixel = Pixel;
            PendingDOMGeometrySize = Size; PendingDOMTextureSize = TexSize;
            DOMRequestTime = Now; NextDOMQuery = Now + 1.0 / 30.0;
            ExecuteJavaScript(FString::Printf(TEXT("window.webui&&window.webui._queryHitTest&&window.webui._queryHitTest(%d,%.9f,%.9f)"),
                PendingDOMRequest, Local.X / Size.X, Local.Y / Size.Y));
        }
        // No GPU readback is needed for DOM hit testing. Explicit pixel reads still work.
        return;
    }
    const bool Fresh = bHaveAlpha && AlphaRegion.Contains(Pixel) && AlphaSize == TexSize && FPlatformTime::Seconds() - AlphaTime < 0.15;
    const uint8 Alpha = Fresh ? AlphaPixels[(Pixel.Y - AlphaRegion.Min.Y) * AlphaRegion.Width() + Pixel.X - AlphaRegion.Min.X].A : 255;
    if (!Dragging)
        Browser->SetVisibility(!Inside || (Fresh && Alpha / 255.0f <= AlphaThreshold) ? EVisibility::HitTestInvisible : EVisibility::Visible);
    if (Inside && !AlphaRead)
    {
        AlphaRead = MakeShared<FWebUITextureReadback, ESPMode::ThreadSafe>();
        // A small tile tolerates normal pointer movement during asynchronous GPU readback.
        // At most 16 KiB is transferred, independent of viewport resolution.
        const FIntRect Region(FIntPoint(FMath::Max(0, Pixel.X - 32), FMath::Max(0, Pixel.Y - 32)),
                              FIntPoint(FMath::Min(TexSize.X, Pixel.X + 32), FMath::Min(TexSize.Y, Pixel.Y + 32)));
        AlphaRead->Start(Window, Region, [Weak = TWeakObjectPtr<UWebUIWidget>(this), Region, TexSize, Epoch = Generation](FWebUIPixels Result)
        {
            if (auto* Self = Weak.Get(); Self && Self->Generation == Epoch)
            {
                Self->AlphaRead.Reset(); Self->bHaveAlpha = Result.Pixels.Num() == Region.Area();
                if (Self->bHaveAlpha)
                {
                    Self->AlphaPixels = MoveTemp(Result.Pixels); Self->AlphaRegion = Region; Self->AlphaSize = TexSize;
                    Self->AlphaTime = FPlatformTime::Seconds(); Self->bSharedTexture = Result.bShared;
                }
            }
        });
    }
}
