#pragma once
#include "CoreMinimal.h"
#include "Input/Events.h"
#include "Layout/Geometry.h"
#include "Templates/SharedPointer.h"
#include <string>
class FSlateShaderResource;
class FJsonObject;

class FWebUIRemoteSession : public TSharedFromThis<FWebUIRemoteSession,ESPMode::ThreadSafe> {
public:
    FWebUIRemoteSession();
    ~FWebUIRemoteSession();
    void Close();
    bool Start(const FString& URL,int32 FPS,bool Bridge,bool Selection);
    void Tick(FIntPoint Size);
    void Send(const TSharedRef<FJsonObject>& Message);
    void Command(const FString& Operation);
    void Eval(const FString& Script);
    void Load(const FString& URL);
    void LoadHTML(const FString& HTML,const FString& URL);
    void Mouse(const FString& Kind,const FGeometry&,const FPointerEvent&,int32 Count=1);
    void Key(int32 Type,int32 Code,int32 Native,int32 Modifiers,bool System=false);
    void Focus(bool Value);
    // Callback runs on the pipe-reader thread. It must not touch UObjects or Slate.
    int32 QueryHitAsync(double U,double V,TFunction<void(bool,double)> Callback);
    bool HitTest(FVector2D UV) const;
    bool DequeueEvent(FString& Event);
    FSlateShaderResource* Texture() const;
    TFunction<FSlateShaderResource*()> CaptureTexture() const;
    FIntPoint Size() const;
    uint64 FrameCount() const;
    FString LastError() const;
    static int32 Modifiers(const FInputEvent&);
private:
    struct FImpl;
    TUniquePtr<FImpl> Impl;
    void Receive(std::string Message);
    void Allocate(FIntPoint Size);
};
