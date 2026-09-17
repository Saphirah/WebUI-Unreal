#include "WebUIResources.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

bool WebUIResources::Resolve(const FString& Root, const FString& Relative, FString& Out)
{
    Out.Reset();
    FString Clean = Relative;
    Clean.ReplaceInline(TEXT("\\"), TEXT("/"));
    if (Clean.IsEmpty() || Clean.StartsWith(TEXT("/")) || Clean.Contains(TEXT(":"))) return false;
    for (TCHAR C : Clean) if (C == 0 || C < 32) return false;
    TArray<FString> Parts; Clean.ParseIntoArray(Parts, TEXT("/"), false);
    for (const auto& Part : Parts)
        if (Part == TEXT("..") || Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" "))) return false;
    FString Base = FPaths::ConvertRelativePathToFull(Root);
    FPaths::NormalizeDirectoryName(Base);
    Out = FPaths::ConvertRelativePathToFull(Base / Clean);
    FPaths::NormalizeFilename(Out);
    if (!Out.StartsWith(Base + TEXT("/"), ESearchCase::IgnoreCase)) { Out.Reset(); return false; }
    return true;
}
FString WebUIResources::ContentURL(const FString& Relative)
{
    TArray<FString> Segments; Relative.Replace(TEXT("\\"), TEXT("/")).ParseIntoArray(Segments, TEXT("/"));
    for (auto& Segment : Segments) Segment = FGenericPlatformHttp::UrlEncode(Segment);
    return TEXT("https://ue.local/") + FString::Join(Segments, TEXT("/"));
}
FString WebUIResources::MimeType(const FString& Path)
{
    static const TMap<FString, FString> Types = {
        {TEXT("html"),TEXT("text/html")},{TEXT("htm"),TEXT("text/html")},{TEXT("css"),TEXT("text/css")},
        {TEXT("js"),TEXT("text/javascript")},{TEXT("mjs"),TEXT("text/javascript")},{TEXT("json"),TEXT("application/json")},
        {TEXT("wasm"),TEXT("application/wasm")},{TEXT("png"),TEXT("image/png")},{TEXT("jpg"),TEXT("image/jpeg")},
        {TEXT("jpeg"),TEXT("image/jpeg")},{TEXT("svg"),TEXT("image/svg+xml")},{TEXT("webp"),TEXT("image/webp")},
        {TEXT("gif"),TEXT("image/gif")},{TEXT("ico"),TEXT("image/x-icon")},{TEXT("woff"),TEXT("font/woff")},
        {TEXT("woff2"),TEXT("font/woff2")},{TEXT("ttf"),TEXT("font/ttf")},{TEXT("webm"),TEXT("video/webm")},
        {TEXT("mp4"),TEXT("video/mp4")},{TEXT("ogg"),TEXT("audio/ogg")},{TEXT("wav"),TEXT("audio/wav")},
        {TEXT("txt"),TEXT("text/plain")},{TEXT("pdf"),TEXT("application/pdf")}};
    const FString* Found = Types.Find(FPaths::GetExtension(Path).ToLower());
    return Found ? *Found : TEXT("application/octet-stream");
}
class FWebUIResource : public IWebBrowserSchemeHandler
{
    TArray<uint8> Bytes;
    int32 Offset = 0, Status = 404;
    FString Mime = TEXT("text/plain");
    bool bHead = false;
public:
    bool ProcessRequest(const FString& Verb, const FString& Url, const FSimpleDelegate& Ready) override
    {
        bHead = Verb == TEXT("HEAD");
        if (Verb != TEXT("GET") && !bHead) Status = 405;
        else
        {
            FString Relative = Url.Mid(17); // https://ue.local/
            int32 Index;
            if (Relative.FindChar('?', Index)) Relative.LeftInline(Index);
            if (Relative.FindChar('#', Index)) Relative.LeftInline(Index);
            Relative = FGenericPlatformHttp::UrlDecode(Relative);
            if (Relative.IsEmpty()) Relative = TEXT("index.html");
            FString Path;
            bool Valid;
            if (Relative == TEXT("__webui/webui.js"))
            {
                auto Plugin = IPluginManager::Get().FindPlugin(TEXT("UnrealWebUI"));
                Valid = Plugin.IsValid();
                if (Valid) Path = Plugin->GetBaseDir() / TEXT("Resources/webui.js");
            }
            else Valid = WebUIResources::Resolve(FPaths::ProjectContentDir(), Relative, Path);
            if (!Valid) Status = 403;
            else
            {
                // Unreal's file layer reads both loose and mounted UFS/pak files.
                const int64 Size = IFileManager::Get().FileSize(*Path);
                if (Size > 64 * 1024 * 1024) Status = 413;
                else if (Size >= 0 && FFileHelper::LoadFileToArray(Bytes, *Path)) { Status = 200; Mime = WebUIResources::MimeType(Path); }
            }
        }
        Ready.ExecuteIfBound(); return true;
    }
    void GetResponseHeaders(IHeaders& H) override
    {
        H.SetStatusCode(Status); H.SetMimeType(*Mime); H.SetContentLength(Bytes.Num());
        H.SetHeader(TEXT("Cache-Control"), TEXT("no-cache"));
        H.SetHeader(TEXT("X-Content-Type-Options"), TEXT("nosniff"));
        H.SetHeader(TEXT("Access-Control-Allow-Origin"), TEXT("https://ue.local"));
    }
    bool ReadResponse(uint8* Out, int32 Count, int32& Read, const FSimpleDelegate&) override
    {
        Read = bHead ? 0 : FMath::Min(FMath::Max(0, Count), Bytes.Num() - Offset);
        if (!Read) return false;
        FMemory::Memcpy(Out, Bytes.GetData() + Offset, Read); Offset += Read; return true;
    }
    void Cancel() override { Bytes.Reset(); Offset = 0; }
};
TUniquePtr<IWebBrowserSchemeHandler> FWebUIResourceFactory::Create(FString Verb, FString Url)
{
    return MakeUnique<FWebUIResource>();
}
