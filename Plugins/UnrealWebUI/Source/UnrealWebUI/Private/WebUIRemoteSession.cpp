#include "WebUIRemoteSession.h"
#include "WebUIResources.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Misc/FileHelper.h"
#include "Misc/Base64.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Dom/JsonObject.h"
#include "HAL/PlatformProcess.h"
#include "HAL/FileManager.h"
#include "RenderingThread.h"
#include "Slate/SlateTextures.h"
#include "RHICommandList.h"
#include "ID3D12DynamicRHI.h"
#include "Windows/WindowsHWrapper.h"
#include <d3d12.h>
#include <wrl/client.h>
#include "../../Shared/WebUIChannel.h"

using Microsoft::WRL::ComPtr;
namespace {
TSharedRef<FJsonObject> Packet(const FString& Op){auto D=MakeShared<FJsonObject>();D->SetStringField(TEXT("op"),Op);return D;}
FString Encode(const TSharedRef<FJsonObject>& D){FString S;auto W=TJsonWriterFactory<>::Create(&S);FJsonSerializer::Serialize(D,W);return S;}
struct FRemoteSlot {
    int32 Id=0,Generation=0;
    FIntPoint Size;
    FSlateTexture2DRHIRef* Texture=nullptr;
    TAtomic<bool> Ready{false};
    ~FRemoteSlot(){if(Texture)BeginCleanup(Texture);}
};
struct FRetiredSlot {
    TSharedPtr<FRemoteSlot,ESPMode::ThreadSafe> Slot;
    FGPUFenceRHIRef Fence;
    TAtomic<bool> Released{false};
};
}
struct FWebUIRemoteSession::FImpl {
    WebUIIPC::Channel Pipe;
    std::thread Watchdog;
    std::mutex WatchMutex;
    std::condition_variable WatchWake;
    std::atomic<bool> Closed{false};
    FProcHandle Process;
    HANDLE Job=nullptr;
    FString ResourceRoot,Error;
    TMap<FString,FString> HTMLResources;
    mutable FCriticalSection Mutex;
    TQueue<FString,EQueueMode::Mpsc> Events;
    TArray<FVector4> Regions;
    FIntPoint RegionSize=FIntPoint::ZeroValue;
    double RegionTime=0;
    bool CompleteRegions=false;
    TMap<int32,TArray<FVector4>> IncrementalRegions;
    bool ABBenchmark=false;
    bool IncrementalDOM=false;
    bool DeltaValid=false;
    TMap<int32,uint64> Frames;
    int32 FrameGeneration=0;
    uint64 ReceivedFrames=0;
    int32 QuerySequence=0;
    struct FQuery {double Start;TFunction<void(bool,double)> Callback;};
    TMap<int32,FQuery> Queries;
    FIntPoint RequestedSize=FIntPoint::ZeroValue;
    int32 Generation=0;
    TArray<TSharedPtr<FRemoteSlot,ESPMode::ThreadSafe>> Slots;
    TArray<TSharedPtr<FRetiredSlot,ESPMode::ThreadSafe>> Retiring;
    TSharedPtr<FRemoteSlot,ESPMode::ThreadSafe> Current;
    TAtomic<bool> Allocating{false};
};
FWebUIRemoteSession::FWebUIRemoteSession():Impl(MakeUnique<FImpl>()){}
FWebUIRemoteSession::~FWebUIRemoteSession(){Close();}
void FWebUIRemoteSession::Close(){
    if(Impl->Closed.exchange(true))return;
    Impl->Pipe.Send("{\"op\":\"close\"}");
    Impl->Pipe.Stop();
    Impl->WatchWake.notify_all();if(Impl->Watchdog.joinable())Impl->Watchdog.join();
    if(Impl->Job){CloseHandle(Impl->Job);Impl->Job=nullptr;}
    if(Impl->Process.IsValid())FPlatformProcess::CloseProc(Impl->Process);
}
bool FWebUIRemoteSession::Start(const FString& URL,int32 FPS,bool Bridge,bool Selection){
    Impl->ABBenchmark=FPlatformMisc::GetEnvironmentVariable(TEXT("WEBUI_AB"))==TEXT("1");
    Impl->IncrementalDOM=FPlatformMisc::GetEnvironmentVariable(TEXT("WEBUI_INCREMENTAL_DOM"))!=TEXT("0");
    if(RHIGetInterfaceType()!=ERHIInterfaceType::D3D12){Impl->Error=TEXT("Independent browser currently requires D3D12.");return false;}
    const auto Plugin=IPluginManager::Get().FindPlugin(TEXT("UnrealWebUI"));
    Impl->ResourceRoot=Plugin->GetBaseDir()/TEXT("Resources");
    const FString Executable=FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir()/TEXT("Binaries/Win64/WebUIHost.exe"));
    if(!FPaths::FileExists(Executable)){Impl->Error=TEXT("WebUIHost.exe is missing. Run Scripts/BuildHost.ps1.");return false;}
    const FString PipeName=TEXT("\\\\.\\pipe\\WebUI-")+FGuid::NewGuid().ToString(EGuidFormats::Digits);
    Impl->Pipe.Start(*PipeName,true,[this](std::string Message){Receive(MoveTemp(Message));});
    const LUID Luid=GetID3D12DynamicRHI()->RHIGetDevice(0)->GetAdapterLuid();
    FString Runtime=FPaths::ConvertRelativePathToFull(FPaths::EngineDir()/TEXT("Binaries/ThirdParty/CEF3/Win64/128.4.13+ge76af7e+chromium-128.0.6613.138"));
    if(!FPaths::FileExists(Runtime/TEXT("libcef.dll")))Runtime+=TEXT("+v2");
    if(!FPaths::FileExists(Runtime/TEXT("libcef.dll"))){Impl->Error=TEXT("Matching CEF 128 runtime is missing.");return false;}
    const FString Cache=FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()/TEXT("WebUIHost")/FGuid::NewGuid().ToString(EGuidFormats::Digits));
    FString Args=FString::Printf(TEXT("--pipe=\"%s\" --runtime=\"%s\" --resources=\"%s\" --cache=\"%s\" --url=\"about:blank\" --fps=%d --adapter=%u,%u"),
        *PipeName,*Runtime,*FPaths::ConvertRelativePathToFull(Impl->ResourceRoot),*Cache,FMath::Clamp(FPS,1,360),static_cast<uint32>(Luid.HighPart),Luid.LowPart);
    if(!Bridge)Args+=TEXT(" --no-bridge");if(Selection)Args+=TEXT(" --allow-selection");
    Impl->Process=FPlatformProcess::CreateProc(*Executable,*Args,true,true,true,nullptr,0,*Runtime,nullptr);
    if(!Impl->Process.IsValid()){Impl->Error=TEXT("Could not start WebUIHost.");return false;}
    Impl->Job=CreateJobObjectW(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION Limits{};Limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(Impl->Job){SetInformationJobObject(Impl->Job,JobObjectExtendedLimitInformation,&Limits,sizeof(Limits));AssignProcessToJobObject(Impl->Job,Impl->Process.Get());}
    Impl->Watchdog=std::thread([this]{
        bool ReportedExit=false;
        for(;;){
            {std::unique_lock<std::mutex> Lock(Impl->WatchMutex);Impl->WatchWake.wait_for(Lock,std::chrono::milliseconds(50),[this]{return Impl->Closed.load();});}
            const bool Closed=Impl->Closed;const bool Exited=!FPlatformProcess::IsProcRunning(Impl->Process);
            TArray<TFunction<void(bool,double)>> Expired;
            {FScopeLock Lock(&Impl->Mutex);for(auto It=Impl->Queries.CreateIterator();It;++It){
                if(Closed||Exited||FPlatformTime::Seconds()-It.Value().Start>=2.0){Expired.Add(MoveTemp(It.Value().Callback));It.RemoveCurrent();}}}
            for(auto& Callback:Expired)if(Callback)Callback(false,-1.0);
            if(Exited&&!Closed&&!ReportedExit){ReportedExit=true;auto D=Packet(TEXT("error"));D->SetStringField(TEXT("message"),TEXT("Independent Chromium host exited."));Impl->Events.Enqueue(Encode(D));}
            if(Closed)break;
        }
    });
    // The load is queued until the host reports browser creation.
    auto D=Packet(TEXT("initial"));D->SetStringField(TEXT("url"),URL);Impl->Events.Enqueue(Encode(D));
    return true;
}
void FWebUIRemoteSession::Send(const TSharedRef<FJsonObject>& D){const FString Text=Encode(D);Impl->Pipe.Send(TCHAR_TO_UTF8(*Text));}
void FWebUIRemoteSession::Command(const FString& Op){Send(Packet(Op));}
void FWebUIRemoteSession::Eval(const FString& Script){auto D=Packet(TEXT("eval"));D->SetStringField(TEXT("script"),Script);Send(D);}
void FWebUIRemoteSession::Load(const FString& URL){auto D=Packet(TEXT("load"));D->SetStringField(TEXT("url"),URL);Send(D);}
void FWebUIRemoteSession::LoadHTML(const FString& HTML,const FString& URL){
    {FScopeLock Lock(&Impl->Mutex);Impl->HTMLResources.Reset();Impl->HTMLResources.Add(URL,HTML);}
    auto D=Packet(TEXT("html"));D->SetStringField(TEXT("url"),URL);Send(D);
}
void FWebUIRemoteSession::Focus(bool Value){auto D=Packet(TEXT("focus"));D->SetBoolField(TEXT("focus"),Value);Send(D);}
void FWebUIRemoteSession::Receive(std::string Message){
    const double ReceivedAt=Impl->ABBenchmark?FPlatformTime::Seconds():0;
    const FString Text=UTF8_TO_TCHAR(Message.c_str());TSharedPtr<FJsonObject> D;
    if(!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text),D)||!D)return;
    FString Op;if(!D->TryGetStringField(TEXT("op"),Op))return;
    if(Impl->ABBenchmark&&Op==TEXT("event")&&D->GetStringField(TEXT("name"))==TEXT("abMouse")){
        TSharedPtr<FJsonObject> Data;
        if(FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(D->GetStringField(TEXT("data"))),Data)&&Data){
            Data->SetNumberField(TEXT("nativeReceivedSeconds"),ReceivedAt);D->SetStringField(TEXT("data"),Encode(Data.ToSharedRef()));
            Impl->Events.Enqueue(Encode(D.ToSharedRef()));return;
        }
    }
    if(Op==TEXT("regionsDelta")){
        if(!Impl->IncrementalDOM)return;
        FScopeLock Lock(&Impl->Mutex);
        auto Reject=[this]{Impl->CompleteRegions=false;Impl->DeltaValid=false;Impl->IncrementalRegions.Reset();Impl->Regions.Reset();};
        const TArray<TSharedPtr<FJsonValue>> *Removed=nullptr,*Updates=nullptr;
        bool Reset=false,Complete=false;double Width=0,Height=0;
        if(!D->TryGetArrayField(TEXT("removed"),Removed)||!D->TryGetArrayField(TEXT("updates"),Updates)||
           Removed->Num()>8192||Updates->Num()>8192||!D->TryGetBoolField(TEXT("reset"),Reset)||
           !D->TryGetBoolField(TEXT("complete"),Complete)||!D->TryGetNumberField(TEXT("width"),Width)||
           !D->TryGetNumberField(TEXT("height"),Height)||!(Width>=1&&Width<=32768&&Height>=1&&Height<=32768)){Reject();return;}
        auto ReadID=[](double ID){return ID>=1&&ID<=MAX_int32&&ID==FMath::FloorToDouble(ID);};
        if(!Reset&&!Impl->DeltaValid)return;
        if(Reset){Impl->IncrementalRegions.Reset();Impl->DeltaValid=true;}
        for(const auto& Item:*Removed){double ID=0;if(!Item->TryGetNumber(ID)||!ReadID(ID)){Reject();return;}Impl->IncrementalRegions.Remove(static_cast<int32>(ID));}
        for(const auto& Item:*Updates){
            if(Item->Type!=EJson::Object){Reject();return;}const auto E=Item->AsObject();
            const TArray<TSharedPtr<FJsonValue>>* Values=nullptr;double ID=0;
            if(!E->TryGetNumberField(TEXT("id"),ID)||!ReadID(ID)||!E->TryGetArrayField(TEXT("rects"),Values)||Values->Num()>8192){Reject();return;}
            TArray<FVector4> Rects;
            for(const auto& Value:*Values){
                if(Value->Type!=EJson::Array){Reject();return;}const auto& A=Value->AsArray();double X,Y,Z,W;
                if(A.Num()!=4||!A[0]->TryGetNumber(X)||!A[1]->TryGetNumber(Y)||!A[2]->TryGetNumber(Z)||!A[3]->TryGetNumber(W)||
                   !(X>=0&&Y>=0&&Z<=1&&W<=1&&X<Z&&Y<W)){Reject();return;}
                Rects.Add(FVector4(X,Y,Z,W));
            }
            Impl->IncrementalRegions.Add(static_cast<int32>(ID),MoveTemp(Rects));
            if(Impl->IncrementalRegions.Num()>8192){Reject();return;}
        }
        Impl->Regions.Reset();for(const auto& E:Impl->IncrementalRegions){
            if(Impl->Regions.Num()+E.Value.Num()>8192){Reject();return;}Impl->Regions.Append(E.Value);}
        Impl->RegionTime=FPlatformTime::Seconds();Impl->RegionSize=FIntPoint(static_cast<int32>(Width),static_cast<int32>(Height));
        Impl->CompleteRegions=Complete;return;
    }
    if(Op==TEXT("loading")){FScopeLock Lock(&Impl->Mutex);Impl->CompleteRegions=false;Impl->DeltaValid=false;Impl->IncrementalRegions.Reset();}
    if(Op==TEXT("regions")){
        const auto* List=static_cast<const TArray<TSharedPtr<FJsonValue>>*>(nullptr);
        if(!D->TryGetArrayField(TEXT("regions"),List)||List->Num()>8192)return;
        TArray<FVector4> Regions;
        for(const auto& Value:*List){const auto& A=Value->AsArray();if(A.Num()!=4)continue;
            FVector4 R(A[0]->AsNumber(),A[1]->AsNumber(),A[2]->AsNumber(),A[3]->AsNumber());
            if(R.X>=0&&R.Y>=0&&R.Z<=1&&R.W<=1&&R.X<R.Z&&R.Y<R.W)Regions.Add(R);}
        FScopeLock Lock(&Impl->Mutex);Impl->Regions=MoveTemp(Regions);Impl->RegionTime=FPlatformTime::Seconds();
        Impl->RegionSize=FIntPoint(D->GetIntegerField(TEXT("width")),D->GetIntegerField(TEXT("height")));
        Impl->CompleteRegions=D->GetBoolField(TEXT("complete"));return;
    }
    if(Op==TEXT("hit")){
        TFunction<void(bool,double)> Callback;double Start=0;
        {FScopeLock Lock(&Impl->Mutex);const int32 Id=D->GetIntegerField(TEXT("id"));if(auto* Q=Impl->Queries.Find(Id)){Start=Q->Start;Callback=MoveTemp(Q->Callback);Impl->Queries.Remove(Id);}}
        if(Callback)Callback(D->GetBoolField(TEXT("hit")),(FPlatformTime::Seconds()-Start)*1000.0);return;
    }
    if(Op==TEXT("frame")){
        FScopeLock Lock(&Impl->Mutex);Impl->FrameGeneration=D->GetIntegerField(TEXT("generation"));
        Impl->Frames.Add(D->GetIntegerField(TEXT("slot")),FCString::Strtoui64(*D->GetStringField(TEXT("sequence")),nullptr,10));++Impl->ReceivedFrames;return;
    }
    if(Op==TEXT("resource")){
        auto Reply=Packet(TEXT("resource"));Reply->SetNumberField(TEXT("id"),D->GetIntegerField(TEXT("id")));
        FString URL=D->GetStringField(TEXT("url")),Path;
        {FScopeLock Lock(&Impl->Mutex);if(auto* HTML=Impl->HTMLResources.Find(URL)){
            FTCHARToUTF8 UTF8(**HTML);Reply->SetNumberField(TEXT("status"),200);Reply->SetStringField(TEXT("mime"),TEXT("text/html"));
            Reply->SetStringField(TEXT("body"),FBase64::Encode(reinterpret_cast<const uint8*>(UTF8.Get()),UTF8.Length()));Send(Reply);return;}}
        FString Relative=URL;Relative.RemoveFromStart(TEXT("https://ue.local/"));int32 Cut;
        if(Relative.FindChar('?',Cut))Relative.LeftInline(Cut);if(Relative.FindChar('#',Cut))Relative.LeftInline(Cut);
        bool Valid=false;
        if(Relative==TEXT("__webui/webui.js")){Path=Impl->ResourceRoot/TEXT("webui.js");Valid=true;}
        else Valid=WebUIResources::Resolve(FPaths::ProjectContentDir(),Relative,Path);
        TArray<uint8> Bytes;const int64 Length=Valid?IFileManager::Get().FileSize(*Path):-1;
        const bool OK=Length>=0&&Length<10*1024*1024&&FFileHelper::LoadFileToArray(Bytes,*Path);
        Reply->SetNumberField(TEXT("status"),OK?200:404);Reply->SetStringField(TEXT("mime"),WebUIResources::MimeType(Path));
        Reply->SetStringField(TEXT("body"),OK?FBase64::Encode(Bytes):FString());Send(Reply);return;
    }
    Impl->Events.Enqueue(Text);
}
bool FWebUIRemoteSession::DequeueEvent(FString& Event){return Impl->Events.Dequeue(Event);}
FString FWebUIRemoteSession::LastError() const{FScopeLock Lock(&Impl->Mutex);return Impl->Error;}
uint64 FWebUIRemoteSession::FrameCount() const{FScopeLock Lock(&Impl->Mutex);return Impl->ReceivedFrames;}
bool FWebUIRemoteSession::HitTest(FVector2D UV) const{
    if(UV.X<0||UV.Y<0||UV.X>=1||UV.Y>=1)return false;
    FScopeLock Lock(&Impl->Mutex);
    if(!Impl->CompleteRegions || Impl->RegionSize!=Impl->RequestedSize || FPlatformTime::Seconds()-Impl->RegionTime>0.25)return true;
    for(const auto& R:Impl->Regions)if(UV.X>=R.X&&UV.Y>=R.Y&&UV.X<R.Z&&UV.Y<R.W)return true;
    return false;
}
int32 FWebUIRemoteSession::QueryHitAsync(double U,double V,TFunction<void(bool,double)> Callback){
    if(Impl->Closed){if(Callback)Callback(false,-1);return 0;}
    auto D=Packet(TEXT("hit"));int32 Id;
    {FScopeLock Lock(&Impl->Mutex);Id=++Impl->QuerySequence;Impl->Queries.Add(Id,{FPlatformTime::Seconds(),MoveTemp(Callback)});}
    D->SetNumberField(TEXT("id"),Id);D->SetNumberField(TEXT("u"),U);D->SetNumberField(TEXT("v"),V);Send(D);return Id;
}
FSlateShaderResource* FWebUIRemoteSession::Texture() const{return Impl->Current&&Impl->Current->Ready?Impl->Current->Texture:nullptr;}
TFunction<FSlateShaderResource*()> FWebUIRemoteSession::CaptureTexture() const{auto Slot=Impl->Current;return [Slot]()->FSlateShaderResource*{return Slot&&Slot->Ready?Slot->Texture:nullptr;};}
FIntPoint FWebUIRemoteSession::Size() const{FScopeLock Lock(&Impl->Mutex);return Impl->RequestedSize;}
void FWebUIRemoteSession::Allocate(FIntPoint Size){
    Impl->Allocating=true;{FScopeLock Lock(&Impl->Mutex);Impl->RequestedSize=Size;}++Impl->Generation;
    auto New=MakeShared<TArray<TSharedPtr<FRemoteSlot,ESPMode::ThreadSafe>>,ESPMode::ThreadSafe>();
    for(int32 I=0;I<3;++I){auto Slot=MakeShared<FRemoteSlot,ESPMode::ThreadSafe>();Slot->Id=I;Slot->Generation=Impl->Generation;Slot->Size=Size;
        Slot->Texture=new FSlateTexture2DRHIRef(FTextureRHIRef(),Size.X,Size.Y);New->Add(Slot);}
    Impl->Slots=*New;
    auto Resize=Packet(TEXT("resize"));Resize->SetNumberField(TEXT("width"),Size.X);Resize->SetNumberField(TEXT("height"),Size.Y);Send(Resize);
    auto Self=AsShared();
    ENQUEUE_RENDER_COMMAND(WebUIAllocateRing)([Self,New,Size](FRHICommandListImmediate& Cmd){
        auto* RHI=GetID3D12DynamicRHI();auto Pool=Packet(TEXT("pool"));Pool->SetNumberField(TEXT("generation"),(*New)[0]->Generation);
        Pool->SetNumberField(TEXT("width"),Size.X);Pool->SetNumberField(TEXT("height"),Size.Y);TArray<TSharedPtr<FJsonValue>> Entries;
        for(const auto& Slot:*New){
            const auto Desc=FRHITextureCreateDesc::Create2D(TEXT("WebUISharedRing"),Size,PF_B8G8R8A8)
                .SetFlags(TexCreate_Shared|TexCreate_ShaderResource|TexCreate_RenderTargetable).SetInitialState(ERHIAccess::Present);
            auto Texture=RHICreateTexture(Desc);Slot->Texture->SetRHIRef(Texture,Size.X,Size.Y);
            HANDLE Shared=nullptr,Remote=nullptr;
            HRESULT HR=RHI->RHIGetDevice(0)->CreateSharedHandle(RHI->RHIGetResource(Texture),nullptr,GENERIC_ALL,nullptr,&Shared);
            if(FAILED(HR)||!DuplicateHandle(GetCurrentProcess(),Shared,Self->Impl->Process.Get(),&Remote,0,false,DUPLICATE_SAME_ACCESS)){
                if(Shared)CloseHandle(Shared);{FScopeLock Lock(&Self->Impl->Mutex);Self->Impl->Error=TEXT("Creating shared D3D12 ring handle failed.");}Self->Impl->Allocating=false;return;}
            CloseHandle(Shared);auto E=MakeShared<FJsonObject>();E->SetNumberField(TEXT("id"),Slot->Id);
            E->SetStringField(TEXT("handle"),FString::Printf(TEXT("%llu"),reinterpret_cast<uint64>(Remote)));Entries.Add(MakeShared<FJsonValueObject>(E));Slot->Ready=true;
        }
        Pool->SetArrayField(TEXT("slots"),Entries);Self->Send(Pool);Self->Impl->Allocating=false;
    });
}
void FWebUIRemoteSession::Tick(FIntPoint Size){
    Size.X=FMath::Clamp(Size.X,1,8192);Size.Y=FMath::Clamp(Size.Y,1,8192);
    if(Size!=Impl->RequestedSize&&!Impl->Allocating)Allocate(Size);
    TMap<int32,uint64> Frames;int32 Generation;
    {FScopeLock Lock(&Impl->Mutex);Generation=Impl->FrameGeneration;Frames=MoveTemp(Impl->Frames);Impl->Frames.Reset();}
    if(Generation==Impl->Generation&&!Frames.IsEmpty()){
        int32 Best=INDEX_NONE;uint64 Sequence=0;
        for(auto Pair:Frames)if(Pair.Value>Sequence){Best=Pair.Key;Sequence=Pair.Value;}
        if(Impl->Slots.IsValidIndex(Best)&&Impl->Slots[Best]->Ready){
            auto Old=Impl->Current;Impl->Current=Impl->Slots[Best];
            auto New=Impl->Current;
            ENQUEUE_RENDER_COMMAND(WebUIAcquireRing)([New](FRHICommandListImmediate& Cmd){Cmd.Transition(FRHITransitionInfo(New->Texture->GetRHIRef(),ERHIAccess::Present,ERHIAccess::SRVMask));});
            if(Old&&Old!=New){auto Retirement=MakeShared<FRetiredSlot,ESPMode::ThreadSafe>();Retirement->Slot=Old;Impl->Retiring.Add(Retirement);
                ENQUEUE_RENDER_COMMAND(WebUIReleaseRing)([Old,Retirement](FRHICommandListImmediate& Cmd){
                    Cmd.Transition(FRHITransitionInfo(Old->Texture->GetRHIRef(),ERHIAccess::SRVMask,ERHIAccess::Present));
                    Retirement->Fence=RHICreateGPUFence(TEXT("WebUIConsumerDone"));Cmd.WriteGPUFence(Retirement->Fence);
                });}
            for(auto Pair:Frames)if(Pair.Key!=Best){auto D=Packet(TEXT("release"));D->SetNumberField(TEXT("slot"),Pair.Key);D->SetNumberField(TEXT("generation"),Generation);Send(D);}
        }
    }
    auto Self=AsShared();
    for(int32 I=Impl->Retiring.Num()-1;I>=0;--I){auto Slot=Impl->Retiring[I];if(Slot->Released){Impl->Retiring.RemoveAtSwap(I);continue;}
        ENQUEUE_RENDER_COMMAND(WebUIPollRingFence)([Self,Slot](FRHICommandListImmediate&){
            if(!Slot->Released&&Slot->Fence&&Slot->Fence->Poll()){
                auto D=Packet(TEXT("release"));D->SetNumberField(TEXT("slot"),Slot->Slot->Id);D->SetNumberField(TEXT("generation"),Slot->Slot->Generation);
                Self->Send(D);Slot->Released=true;
            }
        });
    }
}
int32 FWebUIRemoteSession::Modifiers(const FInputEvent& E){
    int32 M=0;if(E.AreCapsLocked())M|=1;if(E.IsShiftDown())M|=2;if(E.IsControlDown())M|=4;if(E.IsAltDown())M|=8;if(E.IsCommandDown())M|=128;return M;
}
void FWebUIRemoteSession::Mouse(const FString& Kind,const FGeometry& G,const FPointerEvent& E,int32 Count){
    const FVector2D P=G.AbsoluteToLocal(E.GetScreenSpacePosition());const FVector2D S=G.GetLocalSize();
    auto D=Packet(TEXT("mouse"));D->SetStringField(TEXT("kind"),Kind);D->SetNumberField(TEXT("x"),FMath::RoundToInt(P.X/FMath::Max(1.0,S.X)*Impl->RequestedSize.X));
    D->SetNumberField(TEXT("y"),FMath::RoundToInt(P.Y/FMath::Max(1.0,S.Y)*Impl->RequestedSize.Y));
    int32 M=Modifiers(E);if(E.IsMouseButtonDown(EKeys::LeftMouseButton))M|=16;if(E.IsMouseButtonDown(EKeys::MiddleMouseButton))M|=32;if(E.IsMouseButtonDown(EKeys::RightMouseButton))M|=64;
    D->SetNumberField(TEXT("modifiers"),M);D->SetNumberField(TEXT("button"),E.GetEffectingButton()==EKeys::RightMouseButton?2:E.GetEffectingButton()==EKeys::MiddleMouseButton?1:0);
    D->SetNumberField(TEXT("count"),Count);D->SetNumberField(TEXT("delta"),FMath::RoundToInt(E.GetWheelDelta()*120));Send(D);
}
void FWebUIRemoteSession::Key(int32 Type,int32 Code,int32 Native,int32 Modifiers,bool System){auto D=Packet(TEXT("key"));D->SetNumberField(TEXT("type"),Type);D->SetNumberField(TEXT("key"),Code);D->SetNumberField(TEXT("native"),Native);D->SetNumberField(TEXT("modifiers"),Modifiers);D->SetBoolField(TEXT("system"),System);Send(D);}
