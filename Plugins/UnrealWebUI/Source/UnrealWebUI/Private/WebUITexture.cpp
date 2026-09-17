#include "WebUITexture.h"
#include "IWebBrowserWindow.h"
#include "Slate/SlateTextures.h"
#include "Async/Async.h"
#include "RenderingThread.h"

void FWebUITextureReadback::Start(TSharedPtr<IWebBrowserWindow> Window, TOptional<FIntRect> Region, FDone Done)
{
    // The widget retains this window and flushes commands before closing it. Avoid changing
    // the window's non-thread-safe shared-pointer refcount on the render thread.
    IWebBrowserWindow* BrowserWindow = Window.Get();
    StartResource([BrowserWindow]{return BrowserWindow->GetTexture();},Region,MoveTemp(Done));
}
void FWebUITextureReadback::StartResource(TFunction<FSlateShaderResource*()> GetResource,TOptional<FIntRect> Region,FDone Done)
{
    Callback=MoveTemp(Done);
    auto Self=AsShared();
    ENQUEUE_RENDER_COMMAND(WebUIReadbackStart)([Self, GetResource=MoveTemp(GetResource), Region](FRHICommandListImmediate& Cmd)
    {
        auto* Resource = GetResource();
        if (!Resource || Resource->GetType() != ESlateShaderResource::NativeTexture)
        { FWebUIPixels Result; Result.Error = TEXT("Browser texture is not ready."); Self->Finish(MoveTemp(Result)); return; }
        FTextureRHIRef Texture = static_cast<TSlateTexture<FTextureRHIRef>*>(Resource)->GetTypedResource();
        if (!Texture || Texture->GetFormat() != PF_B8G8R8A8)
        { FWebUIPixels Result; Result.Error = TEXT("Expected a BGRA8 browser texture."); Self->Finish(MoveTemp(Result)); return; }
        Self->bShared = EnumHasAnyFlags(Texture->GetFlags(), ETextureCreateFlags::Shared);
        const FIntPoint TextureSize(Texture->GetSizeX(), Texture->GetSizeY());
        Self->Size = Region.IsSet() ? Region.GetValue().Size() : TextureSize;
        FResolveRect Rect;
        if (Region.IsSet())
        {
            const FIntRect R = Region.GetValue();
            if (R.Min.X < 0 || R.Min.Y < 0 || R.Max.X > TextureSize.X || R.Max.Y > TextureSize.Y || R.Width() <= 0 || R.Height() <= 0)
            { FWebUIPixels Result; Result.Error = TEXT("Pixel is outside the browser texture."); Self->Finish(MoveTemp(Result)); return; }
            // The main UE 5.7 CEF surface uses top-left coordinates, including accelerated paint.
            // Popup surfaces have separate engine transforms and are not sampled here.
            Rect = FResolveRect(R.Min.X, R.Min.Y, R.Max.X, R.Max.Y);
        }
        Self->Readback = MakeUnique<FRHIGPUTextureReadback>(TEXT("WebUIReadback"));
        Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::Unknown, ERHIAccess::CopySrc));
        Self->Readback->EnqueueCopy(Cmd, Texture, Rect);
        Cmd.Transition(FRHITransitionInfo(Texture, ERHIAccess::CopySrc, ERHIAccess::SRVMask));
        Self->bSubmitted = true;
    });
}
void FWebUITextureReadback::Poll()
{
    auto Self = AsShared();
    ENQUEUE_RENDER_COMMAND(WebUIReadbackPoll)([Self](FRHICommandListImmediate& Cmd)
    {
        if (!Self->bSubmitted || !Self->Readback) return;
        if (!Self->Readback->IsReady())
        {
            // Do not discard a GPU resource still in flight. Destruction waits for its fence.
            return;
        }
        FWebUIPixels Result; Result.Size = Self->Size; Result.bShared = Self->bShared;
        int32 Pitch = 0;
        const FColor* Data = static_cast<FColor*>(Self->Readback->Lock(Pitch));
        if (Data && Pitch >= Self->Size.X)
        {
            Result.Pixels.SetNumUninitialized(Self->Size.X * Self->Size.Y);
            for (int32 Y = 0; Y < Self->Size.Y; ++Y)
            {
                FMemory::Memcpy(Result.Pixels.GetData() + Y * Self->Size.X, Data + Y * Pitch, Self->Size.X * sizeof(FColor));
            }
        }
        else Result.Error = TEXT("GPU readback could not be mapped.");
        Self->Readback->Unlock(); Self->Readback.Reset(); Self->bSubmitted = false;
        Self->Finish(MoveTemp(Result));
    });
}
void FWebUITextureReadback::Finish(FWebUIPixels Result)
{
    auto Done = MoveTemp(Callback);
    AsyncTask(ENamedThreads::GameThread, [Done = MoveTemp(Done), Result = MoveTemp(Result)]() mutable
    { if (Done) Done(MoveTemp(Result)); });
}
