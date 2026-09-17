#pragma once
#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/HUD.h"
#include "GameFramework/PlayerController.h"
#include "WebUIWidget.h"
#include "Blueprint/UserWidget.h"
#include "WebUIDemoMode.generated.h"
UCLASS()
class UWebUIDemoScreen : public UUserWidget
{
    GENERATED_BODY()
};
UCLASS()
class AWebUIDemoController : public APlayerController
{
    GENERATED_BODY()
public:
    int32 GameClicks = 0;
    virtual bool InputKey(const FInputKeyEventArgs& Params) override;
};
UCLASS()
class AWebUIDemoHUD : public AHUD
{
    GENERATED_BODY()
    UPROPERTY() TObjectPtr<class UUserWidget> Host;
    UPROPERTY() TObjectPtr<UWebUIWidget> Browser;
    virtual void BeginPlay() override;
    UFUNCTION() void Message(const FString& Name, const FWebUIJson& Data, const FString& Callback);
    UFUNCTION() void Pixels(const TArray<FColor>& Data, FIntPoint Size, const FString& Error);
    UFUNCTION() void Error(const FString& Message);
    int32 WebClicks = 0;
    uint64 PerformancePaintStart = 0;
    uint64 PerformanceFrameStart = 0;
    double PerformanceStart = 0;
    bool bClearPath = false, bPanelPath = false;
    int32 DOMStep = 0;
    bool bDOMPassed = true;
    void ClickDOMPointer(bool bExpectedWeb);
    void TestPointer(bool bPanel);
    void ClickPointer(bool bPanel);
    void FinishPointerTest();
    UFUNCTION() void SetupHitBenchmark();
    UFUNCTION() void SetupABBenchmark();
    void TickABBenchmark();
    void ABMessage(const FString&,const FWebUIJson&);
    TMap<int64,double> ABPending;
    TArray<double> ABNativeRTT;
    int32 ABBurst=1,ABSent=0;
    double ABSeconds=0,ABPaintHz=0,ABGameHz=0;
    void TickHitBenchmark();
    void RunIndependentHitBenchmark();
    void ReceiveHitProbe(int32 Request, bool bHit);
    void FinishHitBenchmark(const FWebUIJson& DOMStats);
    FTimerHandle HitBenchmarkTimer;
    double HitBenchmarkStart = 0, NextHitProbe = 0, HitProbeSent = 0;
    double HitProbeEnqueueMs = 0;
    uint64 HitBenchmarkFrame = 0, HitProbeFrame = 0;
    int32 HitProbeSequence = 0, PendingHitProbe = 0, HitTimeouts = 0, HitWrongThread = 0;
    bool bHitBenchmarkFinishing = false;
    TArray<double> HitRoundTrips, HitEnqueueTimes;
    FString HitRawSamples = TEXT("request,sent_seconds,enqueue_ms,reply_ms,frame_delta,on_game_thread\n");
};
UCLASS()
class AWebUIDemoMode : public AGameModeBase
{
    GENERATED_BODY()
public:
    AWebUIDemoMode();
};
