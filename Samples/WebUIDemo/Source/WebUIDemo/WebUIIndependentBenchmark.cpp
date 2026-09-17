#include "WebUIDemoMode.h"
#include "Async/Async.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include <atomic>
#include <future>
#include <chrono>

void AWebUIDemoHUD::RunIndependentHitBenchmark()
{
    if(bHitBenchmarkFinishing)return;
    bHitBenchmarkFinishing=true;
    auto Request=Browser->MakeAsyncHitRequester();
    const FString URL=Browser->GetURL();
    auto Stalling=std::make_shared<std::atomic<bool>>(true);
    Async(EAsyncExecution::Thread,[Request=MoveTemp(Request),Stalling,URL]{
        TArray<double> Times,PausedTimes;
        int32 WrongThread=0,Timeouts=0;
        FString CSV=TEXT("request,rtt_ms,game_thread_paused,on_game_thread\n");
        const double Start=FPlatformTime::Seconds();
        for(int32 I=0;I<300;++I){
            struct Reply{double Ms;bool GT;bool Paused;};
            auto Promise=std::make_shared<std::promise<Reply>>();auto Future=Promise->get_future();
            Request(((I*37)%97+1)/100.0,((I*53)%97+1)/100.0,[Promise,Stalling](bool,double Ms){
                Promise->set_value({Ms,IsInGameThread(),Stalling->load()});
            });
            if(Future.wait_for(std::chrono::seconds(2))!=std::future_status::ready){++Timeouts;break;}
            const auto R=Future.get();if(R.Ms<0){++Timeouts;break;}
            Times.Add(R.Ms);if(R.Paused)PausedTimes.Add(R.Ms);WrongThread+=R.GT?1:0;
            CSV+=FString::Printf(TEXT("%d,%.6f,%d,%d\n"),I+1,R.Ms,R.Paused,R.GT);
            FPlatformProcess::Sleep(0.005f); // Benchmark pacing on its own worker only.
        }
        const auto Stats=[](TArray<double> A){A.Sort();const auto Q=[&](double P){return A.IsEmpty()?0.0:A[FMath::Clamp(FMath::CeilToInt(A.Num()*P)-1,0,A.Num()-1)];};
            return FString::Printf(TEXT("p50=%.4f p95=%.4f p99=%.4f max=%.4f"),Q(.5),Q(.95),Q(.99),Q(1));};
        const bool Pass=Times.Num()==300&&WrongThread==0&&PausedTimes.Num()>0&&Timeouts==0;
        const FString Report=FString::Printf(TEXT("pass=%d backend=independent url=%s\nresponses=%d timeouts=%d game_thread_replies=%d replies_during_500ms_game_thread_pause=%d seconds=%.3f\nrtt_ms %s\npaused_rtt_ms %s\n"),
            Pass,*URL,Times.Num(),Timeouts,WrongThread,PausedTimes.Num(),FPlatformTime::Seconds()-Start,*Stats(Times),*Stats(PausedTimes));
        FFileHelper::SaveStringToFile(Report,*(FPaths::ProjectSavedDir()/TEXT("WebUIIndependentBenchmark.txt")));
        FFileHelper::SaveStringToFile(CSV,*(FPaths::ProjectSavedDir()/TEXT("WebUIIndependentBenchmark.csv")));
        AsyncTask(ENamedThreads::GameThread,[Report,Pass]{UE_LOG(LogTemp,Display,TEXT("WEBUI_INDEPENDENT_BENCHMARK %s"),*Report);FPlatformMisc::RequestExitWithStatus(false,Pass?0:1);});
    });
    // Deliberate test-only stall: no engine ticks, Slate ticks or Blueprint callbacks can run here.
    FPlatformProcess::Sleep(0.5f);
    Stalling->store(false);
}
