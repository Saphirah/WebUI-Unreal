#pragma once
#include "CoreMinimal.h"
#include "RHIGPUReadback.h"
class IWebBrowserWindow;
class FSlateShaderResource;

struct FWebUIPixels
{
    TArray<FColor> Pixels;
    FIntPoint Size = FIntPoint::ZeroValue;
    bool bShared = false;
    FString Error;
};

/** One in-flight readback. GPU state is accessed only on the render thread. */
class FWebUITextureReadback : public TSharedFromThis<FWebUITextureReadback, ESPMode::ThreadSafe>
{
public:
    using FDone = TFunction<void(FWebUIPixels)>;
    void Start(TSharedPtr<IWebBrowserWindow> Window, TOptional<FIntRect> Region, FDone Done);
    void StartResource(TFunction<FSlateShaderResource*()> GetResource, TOptional<FIntRect> Region, FDone Done);
    void Poll();
private:
    TUniquePtr<FRHIGPUTextureReadback> Readback;
    FIntPoint Size = FIntPoint::ZeroValue;
    bool bShared = false;
    bool bSubmitted = false;
    FDone Callback;
    void Finish(FWebUIPixels Result);
};
