#pragma once
#include "CoreMinimal.h"
#include "Components/Widget.h"
#include "WebUIJson.h"
#include "WebUIWidget.generated.h"

class SWebBrowser;
class SWebUIRoot;
class IWebBrowserWindow;
class FWebUITextureReadback;
class UWebUIBridge;
class FWebUIRemoteSession;
class APlayerController;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FWebUIEvent, const FString&, Name, const FWebUIJson&, Data, const FString&, Callback);
DECLARE_DYNAMIC_MULTICAST_DELEGATE(FWebUILoaded);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWebUIError, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FWebUIPixelResult, const TArray<FColor>&, Pixels, FIntPoint, Size, const FString&, Error);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FWebUIURLChanged, const FString&, URL);
DECLARE_MULTICAST_DELEGATE_TwoParams(FWebUIHitTestProbeReply, int32, bool);

UCLASS(meta=(DisplayName="Web Interface"))
class UNREALWEBUI_API UWebUIWidget : public UWidget
{
    GENERATED_BODY()
public:
    /** Independent CEF host, I/O threads and shared GPU ring. Recreate the widget after changing. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Web UI") bool bIndependentBrowser = true;
    using FAsyncHitCallback = TFunction<void(bool,double)>;
    using FAsyncHitRequester = TFunction<void(double,double,FAsyncHitCallback)>;
    FAsyncHitRequester MakeAsyncHitRequester();
    uint64 GetPaintCount() const;
    // Diagnostics use negative IDs; regular DOM routing uses positive IDs.
    FWebUIHitTestProbeReply OnHitTestProbeReply;
    UWebUIWidget(const FObjectInitializer& ObjectInitializer);
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Web UI") FString InitialURL = TEXT("about:blank");
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Web UI", meta=(ClampMin="1", ClampMax="120")) int32 BrowserFrameRate = 60;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Web UI") bool bTransparent = true;
    /** Allow text selection. When false, also blocks selection in editable fields.
     * Use SetAllowTextSelection to change this after loading a page. */
    UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Web UI|Input") bool bAllowTextSelection = false;
    UFUNCTION(BlueprintCallable, Category="Web UI|Input") void SetAllowTextSelection(bool bAllow);
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Web UI") bool bClickThrough = true;
    /** Prefer Chromium DOM hit testing: pointer-events:none passes through, transparent controls remain interactive.
     * Requires the bridge. Disable to use legacy GPU alpha testing. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Web UI|Input") bool bUseDOMHitTesting = true;
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Web UI", meta=(ClampMin="0", ClampMax="1")) float AlphaThreshold = 0.01f;
    /** Use the game viewport's registered Unreal software cursors, suppressing native cursors. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Web UI") bool bCustomCursors = false;
    /** Disable before constructing a widget used to browse arbitrary remote pages. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Web UI|Bridge") bool bEnableBridge = true;
    /** Explicit trusted origins, e.g. http://localhost:5173 for Vite development. */
    UPROPERTY(EditAnywhere, BlueprintReadWrite, Category="Web UI|Bridge") TArray<FString> TrustedOrigins;
    UPROPERTY(BlueprintAssignable, Category="Web UI") FWebUIEvent OnInterfaceEvent;
    UPROPERTY(BlueprintAssignable, Category="Web UI") FWebUILoaded OnLoadCompleted;
    UPROPERTY(BlueprintAssignable, Category="Web UI") FWebUILoaded OnLoadStarted;
    UPROPERTY(BlueprintAssignable, Category="Web UI") FWebUIError OnError;
    UPROPERTY(BlueprintAssignable, Category="Web UI") FWebUIURLChanged OnURLChanged;
    UPROPERTY(BlueprintAssignable, Category="Web UI|Texture") FWebUIPixelResult OnPixelsRead;

    UFUNCTION(BlueprintCallable, Category="Web UI") bool Load(const FString& ContentRelativePath);
    UFUNCTION(BlueprintCallable, Category="Web UI") bool LoadURL(const FString& URL);
    UFUNCTION(BlueprintCallable, Category="Web UI") bool LoadHTML(const FString& HTML, const FString& BaseURL = TEXT("https://ue.local/index.html"));
    UFUNCTION(BlueprintCallable, Category="Web UI") bool LoadFile(const FString& RelativePath, bool bFromContent = false);
    UFUNCTION(BlueprintCallable, Category="Web UI") bool LoadContent(const FString& RelativePath, const TArray<FString>& Scripts);
    UFUNCTION(BlueprintCallable, Category="Web UI|Bridge") bool Call(const FString& Function, const FWebUIJson& Data);
    UFUNCTION(BlueprintCallable, Category="Web UI") void ExecuteJavaScript(const FString& Script);
    UFUNCTION(BlueprintCallable, Category="Web UI") void Reload();
    UFUNCTION(BlueprintCallable, Category="Web UI") void StopLoad();
    UFUNCTION(BlueprintCallable, Category="Web UI") void GoBack();
    UFUNCTION(BlueprintCallable, Category="Web UI") void GoForward();
    UFUNCTION(BlueprintPure, Category="Web UI") FString GetURL() const;
    UFUNCTION(BlueprintPure, Category="Web UI") bool IsLoaded() const;
    /** Game-and-UI mode permits transparent regions to reach the game viewport. */
    UFUNCTION(BlueprintCallable, Category="Web UI|Input") void Focus(APlayerController* Controller);
    UFUNCTION(BlueprintCallable, Category="Web UI|Input") void Unfocus(APlayerController* Controller);
    UFUNCTION(BlueprintCallable, Category="Web UI|Input") void ResetMousePosition(APlayerController* Controller);
    UFUNCTION(BlueprintPure, Category="Web UI|Texture") int32 GetTextureWidth() const;
    UFUNCTION(BlueprintPure, Category="Web UI|Texture") int32 GetTextureHeight() const;
    /** Async results arrive through OnPixelsRead. False means another explicit read is pending. */
    UFUNCTION(BlueprintCallable, Category="Web UI|Texture") bool ReadTexturePixels();
    UFUNCTION(BlueprintCallable, Category="Web UI|Texture") bool ReadTexturePixel(int32 X, int32 Y);
    /** Last observed shared-texture flag; evidence of the accelerated surface, not a frame-time benchmark. */
    UFUNCTION(BlueprintPure, Category="Web UI|Texture") bool HasSharedGPUTexture() const { return bSharedTexture; }

    virtual void ReleaseSlateResources(bool bReleaseChildren) override;
    void TickBrowser(const FGeometry& Geometry);
    bool IsTrustedURL(const FString& URL) const;
    void Receive(const FString& Name, const FString& JSON, const FString& Callback);
    void ReceiveHitTest(int32 Request, bool bHit);
protected:
    virtual TSharedRef<SWidget> RebuildWidget() override;
#if WITH_EDITOR
    virtual const FText GetPaletteCategory() override { return FText::FromString(TEXT("Web UI")); }
#endif
private:
    UPROPERTY(Transient) TObjectPtr<UWebUIBridge> Bridge;
    TSharedPtr<IWebBrowserWindow> Window;
    TSharedPtr<SWidget> Browser;
    TSharedPtr<SWebBrowser> LegacyBrowser;
    TSharedPtr<FWebUIRemoteSession,ESPMode::ThreadSafe> Remote;
    bool bRemoteLoaded=false;
    TSharedPtr<SWebUIRoot> Root;
    TSharedPtr<FWebUITextureReadback, ESPMode::ThreadSafe> AlphaRead;
    TSharedPtr<FWebUITextureReadback, ESPMode::ThreadSafe> ExplicitRead;
    FString PendingHTML;
    bool bHasPendingHTML = false;
    bool bSharedTexture = false;
    bool bHaveAlpha = false;
    TArray<FColor> AlphaPixels;
    FIntRect AlphaRegion;
    FIntPoint AlphaSize = FIntPoint::ZeroValue;
    double AlphaTime = 0;
    uint64 Generation = 0;
    int32 DOMSequence = 0;
    int32 PendingDOMRequest = 0;
    bool bDOMHit = true;
    bool bHaveDOMHit = false;
    FIntPoint DOMPixel = FIntPoint::ZeroValue;
    FIntPoint PendingDOMPixel = FIntPoint::ZeroValue;
    FVector2D DOMGeometrySize = FVector2D::ZeroVector;
    FVector2D PendingDOMGeometrySize = FVector2D::ZeroVector;
    FIntPoint DOMTextureSize = FIntPoint::ZeroValue;
    FIntPoint PendingDOMTextureSize = FIntPoint::ZeroValue;
    double DOMSampleTime = 0;
    double DOMRequestTime = 0;
    double NextDOMQuery = 0;
    TArray<FString> PendingScripts;
    uint64 PaintCount = 0;
    double PerformanceTime = 0;
    uint64 PerformanceFrame = 0, PerformancePaint = 0;
    void CountPaint() { ++PaintCount; }
    void Loaded();
    bool RequestPixels(TOptional<FIntPoint> Pixel);
};

/** Only this narrow object is exposed to JavaScript. */
UCLASS()
class UNREALWEBUI_API UWebUIBridge : public UObject
{
    GENERATED_BODY()
public:
    TWeakObjectPtr<UWebUIWidget> Owner;
    UFUNCTION() void Broadcast(const FString& Name, const FString& Data, const FString& Callback);
    UFUNCTION() void HitTestResult(int32 Request, bool bHit);
};
