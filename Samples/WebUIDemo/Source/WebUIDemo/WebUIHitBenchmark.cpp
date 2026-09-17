#include "WebUIDemoMode.h"
#include "WebUIWidget.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "TimerManager.h"

void AWebUIDemoHUD::SetupHitBenchmark()
{
    Browser->ExecuteJavaScript(TEXT(R"JS((()=>{
      if(window.__webuiHitBenchmark) return;
      window.__webuiHitBenchmark=true; window.__webuiHitTimes=[];
      const hit=webui.hitTest;
      webui.hitTest=function(x,y){const t=performance.now(); const result=hit(x,y);
        window.__webuiHitTimes.push(performance.now()-t); return result;};
      webui.ready.then(()=>setTimeout(()=>ue5('hitBenchmarkReady'),3000));
    })())JS"));
}

void AWebUIDemoHUD::TickHitBenchmark()
{
    if (bHitBenchmarkFinishing) return;
    const double Now = FPlatformTime::Seconds();
    if (PendingHitProbe)
    {
        if (Now - HitProbeSent < 2.0) return;
        ++HitTimeouts; PendingHitProbe = 0;
    }
    if (HitProbeSequence >= 300 || Now - HitBenchmarkStart >= 30.0)
    {
        bHitBenchmarkFinishing = true;
        GetWorldTimerManager().ClearTimer(HitBenchmarkTimer);
        Browser->ExecuteJavaScript(TEXT(R"JS((()=>{
          const a=window.__webuiHitTimes.sort((a,b)=>a-b),q=p=>a[Math.min(a.length-1,Math.ceil(a.length*p)-1)]||0;
          ue5('hitBenchmarkDOM',{count:a.length,p50:q(.5),p95:q(.95),p99:q(.99),max:q(1),visibility:document.visibilityState,
            title:document.title,elements:document.querySelectorAll('*').length,url:location.href,readyState:document.readyState});
        })())JS"));
        return;
    }
    if (Now < NextHitProbe) return;
    PendingHitProbe = -(++HitProbeSequence);
    HitProbeSent = Now; HitProbeFrame = GFrameCounter; NextHitProbe = Now + 1.0 / 30.0;
    // Sweep positions without touching the OS cursor. Use the production JS and native reply endpoint.
    const double U = ((HitProbeSequence * 37) % 97 + 1) / 100.0;
    const double V = ((HitProbeSequence * 53) % 97 + 1) / 100.0;
    Browser->ExecuteJavaScript(FString::Printf(TEXT("webui._queryHitTest(%d,%.9f,%.9f)"), PendingHitProbe, U, V));
    HitProbeEnqueueMs = (FPlatformTime::Seconds() - Now) * 1000.0;
}

void AWebUIDemoHUD::ReceiveHitProbe(int32 Request, bool)
{
    if (Request != PendingHitProbe) return;
    const double RTT = (FPlatformTime::Seconds() - HitProbeSent) * 1000.0;
    const bool OnGameThread = IsInGameThread();
    HitWrongThread += OnGameThread ? 0 : 1;
    HitRoundTrips.Add(RTT); HitEnqueueTimes.Add(HitProbeEnqueueMs);
    HitRawSamples += FString::Printf(TEXT("%d,%.6f,%.6f,%.6f,%llu,%d\n"), -Request,
        HitProbeSent - HitBenchmarkStart, HitProbeEnqueueMs, RTT, GFrameCounter - HitProbeFrame, OnGameThread);
    PendingHitProbe = 0;
}

void AWebUIDemoHUD::FinishHitBenchmark(const FWebUIJson& DOMStats)
{
    const double Seconds = FPlatformTime::Seconds() - HitBenchmarkStart;
    const auto Describe = [](TArray<double> Samples) {
        Samples.Sort();
        const auto Q = [&Samples](double P) { return Samples.IsEmpty() ? 0.0 : Samples[FMath::Clamp(FMath::CeilToInt(Samples.Num()*P)-1,0,Samples.Num()-1)]; };
        return FString::Printf(TEXT("p50=%.4f p95=%.4f p99=%.4f max=%.4f"), Q(.5), Q(.95), Q(.99), Q(1));
    };
    const FString Report = FString::Printf(TEXT("url=%s\nrequested=%d responses=%d timeouts=%d non_game_thread_replies=%d engine_hz=%.2f seconds=%.2f\nrtt_ms %s\nenqueue_ms %s\ndom_ms %s\n"),
        *Browser->GetURL(), HitProbeSequence, HitRoundTrips.Num(), HitTimeouts, HitWrongThread,
        (GFrameCounter - HitBenchmarkFrame) / Seconds, Seconds, *Describe(HitRoundTrips), *Describe(HitEnqueueTimes),
        *UWebUIJsonLibrary::Stringify(DOMStats));
    FFileHelper::SaveStringToFile(Report, *(FPaths::ProjectSavedDir()/TEXT("WebUIHitBenchmark.txt")));
    FFileHelper::SaveStringToFile(HitRawSamples, *(FPaths::ProjectSavedDir()/TEXT("WebUIHitBenchmark.csv")));
    UE_LOG(LogTemp, Display, TEXT("WEBUI_HIT_BENCHMARK %s"), *Report);
    FPlatformMisc::RequestExitWithStatus(false, HitRoundTrips.IsEmpty() ? 1 : 0);
}
