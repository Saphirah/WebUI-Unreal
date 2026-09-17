#include "WebUIDemoMode.h"
#include "Framework/Application/SlateApplication.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/CommandLine.h"
#include "TimerManager.h"

void AWebUIDemoHUD::SetupABBenchmark(){
    if(Browser->GetURL().IsEmpty()||Browser->GetURL()==TEXT("about:blank"))return;
    if(bHitBenchmarkFinishing)return;bHitBenchmarkFinishing=true;
    FParse::Value(FCommandLine::Get(),TEXT("WebUIABBurst="),ABBurst);ABBurst=FMath::Clamp(ABBurst,1,32);
    Browser->ExecuteJavaScript(TEXT(R"JS((()=>{
      let frames=[],active=false,start=0;
      function frame(t){if(active)frames.push(t);requestAnimationFrame(frame);}requestAnimationFrame(frame);
      document.addEventListener('pointermove',e=>{if(active)ue5('abMouse',{x:Math.round(e.clientX),y:Math.round(e.clientY)});},true);
      ue.interface.abFinish=()=>{
        active=false;const elapsed=performance.now()-start,s=window.__webuiRegionStats;
        const a=s.samples.slice().sort((a,b)=>a-b),g=frames.slice(1).map((t,i)=>t-frames[i]).sort((a,b)=>a-b);
        ue5('abStats',{domMsPerSecond:s.ms*1000/elapsed,readsPerSecond:s.reads*1000/elapsed,bytesPerSecond:s.bytes*1000/elapsed,
          domP95:a[Math.max(0,Math.ceil(a.length*.95)-1)]||0,rafHz:frames.length*1000/elapsed,rafP95:g[Math.max(0,Math.ceil(g.length*.95)-1)]||0,
          elements:document.querySelectorAll('*').length,title:document.title});
      };
      webui.ready.then(()=>setTimeout(()=>{const s=window.__webuiRegionStats;s.ms=s.reads=s.bytes=s.scans=0;s.samples=[];active=true;start=performance.now();ue5('abBegin');},3000));
    })())JS"));
}
void AWebUIDemoHUD::TickABBenchmark(){
    const double Now=FPlatformTime::Seconds();
    if(Now-HitBenchmarkStart>=8){
        GetWorldTimerManager().ClearTimer(HitBenchmarkTimer);ABSeconds=Now-HitBenchmarkStart;
        ABPaintHz=(Browser->GetPaintCount()-PerformancePaintStart)/ABSeconds;ABGameHz=(GFrameCounter-HitBenchmarkFrame)/ABSeconds;
        Browser->Call(TEXT("abFinish"),UWebUIJsonLibrary::Null());return;
    }
    const auto Geometry=Browser->GetCachedGeometry();const FVector2D Size=Geometry.GetLocalSize();
    auto& Slate=FSlateApplication::Get();const TSet<FKey> Buttons;
    for(int32 N=0;N<ABBurst;++N){
        const int32 I=ABSent++,X=40+I%800,Y=40+(I/800)%20*35;
        ABPending.Add(static_cast<int64>(X)*65536+Y,FPlatformTime::Seconds());
        const FVector2D P=Geometry.LocalToAbsolute(Size*FVector2D(double(X)/Browser->GetTextureWidth(),double(Y)/Browser->GetTextureHeight()));
        const FVector2D Previous=Slate.GetCursorPos();Slate.SetCursorPos(P);
        Slate.ProcessMouseMoveEvent(FPointerEvent(0,P,Previous,Buttons,EKeys::Invalid,0,FModifierKeysState()),false);
    }
}
void AWebUIDemoHUD::ABMessage(const FString& Name,const FWebUIJson& Data){
    if(Name==TEXT("abBegin")){
        HitRawSamples=TEXT("x,y,rtt_ms,native_rtt_ms\n");
        HitBenchmarkStart=FPlatformTime::Seconds();HitBenchmarkFrame=GFrameCounter;PerformancePaintStart=Browser->GetPaintCount();
        GetWorldTimerManager().SetTimer(HitBenchmarkTimer,this,&AWebUIDemoHUD::TickABBenchmark,1.0f/90.0f,true);return;
    }
    if(Name==TEXT("abMouse")){
        bool Found;double X=0,Y=0;
        UWebUIJsonLibrary::AsNumber(UWebUIJsonLibrary::Field(Data,TEXT("x"),Found),X);UWebUIJsonLibrary::AsNumber(UWebUIJsonLibrary::Field(Data,TEXT("y"),Found),Y);
        const int64 Key=static_cast<int64>(X)*65536+static_cast<int32>(Y);
        if(auto* Sent=ABPending.Find(Key)){const double RTT=(FPlatformTime::Seconds()-*Sent)*1000;HitRoundTrips.Add(RTT);
            double NativeAt=0;UWebUIJsonLibrary::AsNumber(UWebUIJsonLibrary::Field(Data,TEXT("nativeReceivedSeconds"),Found),NativeAt);
            const double NativeRTT=(NativeAt-*Sent)*1000;if(NativeAt>0)ABNativeRTT.Add(NativeRTT);
            HitRawSamples+=FString::Printf(TEXT("%d,%d,%.6f,%.6f\n"),static_cast<int32>(X),static_cast<int32>(Y),RTT,NativeRTT);ABPending.Remove(Key);}return;
    }
    if(Name==TEXT("abStats")){
        ABNativeRTT.Sort();auto NQ=[this](double P){return ABNativeRTT.IsEmpty()?0.0:ABNativeRTT[FMath::Clamp(FMath::CeilToInt(P*ABNativeRTT.Num())-1,0,ABNativeRTT.Num()-1)];};
        HitRoundTrips.Sort();auto Q=[this](double P){return HitRoundTrips.IsEmpty()?0.0:HitRoundTrips[FMath::Clamp(FMath::CeilToInt(P*HitRoundTrips.Num())-1,0,HitRoundTrips.Num()-1)];};
        const FString Report=FString::Printf(TEXT("{\"sent\":%d,\"received\":%d,\"burst\":%d,\"seconds\":%.4f,\"paintHz\":%.4f,\"gameHz\":%.4f,\"mouseP50\":%.4f,\"mouseP95\":%.4f,\"mouseP99\":%.4f,\"page\":%s}"),
            ABSent,HitRoundTrips.Num(),ABBurst,ABSeconds,ABPaintHz,ABGameHz,Q(.5),Q(.95),Q(.99),*UWebUIJsonLibrary::Stringify(Data));
        FString FullReport=Report.LeftChop(1)+FString::Printf(TEXT(",\"nativeP50\":%.6f,\"nativeP95\":%.6f}"),NQ(.5),NQ(.95));
        FFileHelper::SaveStringToFile(FullReport,*(FPaths::ProjectSavedDir()/TEXT("WebUIAB.json")));
        FFileHelper::SaveStringToFile(HitRawSamples,*(FPaths::ProjectSavedDir()/TEXT("WebUIAB.csv")));
        UE_LOG(LogTemp,Display,TEXT("WEBUI_AB %s"),*Report);FPlatformMisc::RequestExitWithStatus(false,HitRoundTrips.IsEmpty()?1:0);
    }
}
