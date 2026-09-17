#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <atomic>
#include <fstream>
#include <iomanip>
#include <functional>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#include "include/cef_app.h"
#include "include/cef_browser.h"
#include "include/cef_client.h"
#include "include/cef_parser.h"
#include "include/cef_render_handler.h"
#include "include/cef_request_handler.h"
#include "include/cef_resource_handler.h"
#include "include/cef_scheme.h"
#include "include/cef_task.h"
#include "../Shared/WebUIChannel.h"

using Microsoft::WRL::ComPtr;
using Dict = CefRefPtr<CefDictionaryValue>;
static WebUIIPC::Channel Pipe;
static std::atomic<bool> Quit{false};
static CefRefPtr<CefBrowser> Browser;
static std::wstring Resources;
static std::wstring Runtime;
static bool EnableBridge = true, AllowSelection = false;
static bool IncrementalDOM=false,ABExperiment=false;
static int ViewWidth = 1280, ViewHeight = 720, TargetFPS = 120;
static std::string InitialURL, AdapterLuid;
static uint64_t FrameSequence = 0;
// Opt-in diagnostics. No disk I/O occurs in the accelerated-paint callback.
static std::wstring GPUProfilePath;
static double ProfileAfter=0;
static std::string ProfileURL;
static std::vector<std::string> ProfileRows;
static std::thread ProfileWriter;
static uint64_t ProfileDropped=0;
static double Milliseconds(){LARGE_INTEGER t,f;QueryPerformanceCounter(&t);QueryPerformanceFrequency(&f);return 1000.0*double(t.QuadPart)/double(f.QuadPart);}
static ComPtr<ID3D11Query> ProfileDisjoint,ProfileBegin,ProfileEnd;
static void Send(Dict d) {
    auto value=CefValue::Create(); value->SetDictionary(d);
    Pipe.Send(CefWriteJSON(value,JSON_WRITER_DEFAULT).ToString());
}
static Dict Message(const char* op) { auto d=CefDictionaryValue::Create(); d->SetString("op",op); return d; }
static void Error(const std::string& reason) { auto d=Message("error");d->SetString("message",reason);Send(d); }
class Task : public CefTask {
    std::function<void()> fn;
public: explicit Task(std::function<void()> f):fn(std::move(f)){}
    void Execute() override {fn();}
    IMPLEMENT_REFCOUNTING(Task);
};
static void UI(std::function<void()> f) { CefPostTask(TID_UI,new Task(std::move(f))); }
static std::string ReadResource(const char* name) {
    std::ifstream input(Resources+L"/"+CefString(name).ToWString(),std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input),{});
}

class NativeFunction : public CefV8Handler {
public:
    bool Execute(const CefString&, CefRefPtr<CefV8Value>, const CefV8ValueList& arguments,
                 CefRefPtr<CefV8Value>& result, CefString&) override {
        if(arguments.size()!=1 || !arguments[0]->IsString()) return false;
        const auto json=arguments[0]->GetStringValue();
        if(json.length()>16*1024*1024) return false;
        auto message=CefProcessMessage::Create("webui-native");
        message->GetArgumentList()->SetString(0,json);
        CefV8Context::GetCurrentContext()->GetFrame()->SendProcessMessage(PID_BROWSER,message);
        result=CefV8Value::CreateBool(true); return true;
    }
    IMPLEMENT_REFCOUNTING(NativeFunction);
};
class App : public CefApp, public CefRenderProcessHandler, public CefBrowserProcessHandler {
public:
    CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {return this;}
    void OnBeforeChildProcessLaunch(CefRefPtr<CefCommandLine> command) override {
        command->AppendSwitchWithValue("runtime",Runtime);
        command->AppendSwitchWithValue("resources",Resources);
        command->AppendSwitchWithValue("adapter",AdapterLuid);
    }
    CefRefPtr<CefRenderProcessHandler> GetRenderProcessHandler() override {return this;}
    void OnContextCreated(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,CefRefPtr<CefV8Context> context) override {
        if(frame->IsMain()) context->GetGlobal()->SetValue("__webuiNative",CefV8Value::CreateFunction("__webuiNative",new NativeFunction),V8_PROPERTY_ATTRIBUTE_READONLY);
    }
    void OnBeforeCommandLineProcessing(const CefString&,CefRefPtr<CefCommandLine> cmd) override {
        cmd->AppendSwitch("enable-gpu");cmd->AppendSwitch("enable-gpu-compositing");
        cmd->AppendSwitchWithValue("use-angle","d3d11");
        if(!AdapterLuid.empty()) cmd->AppendSwitchWithValue("use-adapter-luid",AdapterLuid);
        cmd->AppendSwitch("enable-begin-frame-scheduling");
    }
    IMPLEMENT_REFCOUNTING(App);
};

// UFS/pak reads are brokered through Unreal's I/O worker, never its game thread.
class Resource : public CefResourceHandler {
public:
    std::string body,mime="application/octet-stream";
    int status=200; size_t offset=0;
    CefRefPtr<CefCallback> callback;
    bool Open(CefRefPtr<CefRequest> request,bool& handle_request,CefRefPtr<CefCallback> cb) override;
    void GetResponseHeaders(CefRefPtr<CefResponse> response,int64_t& length,CefString&) override {
        response->SetStatus(status);response->SetMimeType(mime);length=body.size();
    }
    bool Read(void* out,int size,int& read,CefRefPtr<CefResourceReadCallback>) override {
        read=static_cast<int>(std::min<size_t>(size,body.size()-offset));
        if(read){memcpy(out,body.data()+offset,read);offset+=read;}return read>0;
    }
    void Cancel() override {}
    IMPLEMENT_REFCOUNTING(Resource);
};
static std::mutex ResourceMutex;
static int ResourceSequence=0;
static std::map<int,CefRefPtr<Resource>> PendingResources;
static std::string HTMLOverrideURL;
bool Resource::Open(CefRefPtr<CefRequest> request,bool& handle_request,CefRefPtr<CefCallback> cb) {
    callback=cb;handle_request=false;
    auto d=Message("resource");
    {std::lock_guard<std::mutex> lock(ResourceMutex);d->SetInt("id",++ResourceSequence);PendingResources[ResourceSequence]=this;}
    d->SetString("url",request->GetURL());Send(d);return true;
}
class ResourceFactory : public CefSchemeHandlerFactory {
public:
    CefRefPtr<CefResourceHandler> Create(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame>,const CefString&,CefRefPtr<CefRequest>) override {return new Resource;}
    IMPLEMENT_REFCOUNTING(ResourceFactory);
};
class HTMLRequest : public CefResourceRequestHandler {
public:
    CefRefPtr<CefResourceHandler> GetResourceHandler(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame>,CefRefPtr<CefRequest>) override{return new Resource;}
    IMPLEMENT_REFCOUNTING(HTMLRequest);
};

struct Slot {int id=0,width=0,height=0;bool free=true;ComPtr<ID3D11Texture2D> texture;};
static ComPtr<ID3D11Device> Device;
static ComPtr<ID3D11DeviceContext> Context;
static std::map<int,Slot> Slots;
static int PoolGeneration=0;
static bool InitializeDevice() {
    ComPtr<IDXGIFactory1> factory;CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    ComPtr<IDXGIAdapter1> selected;
    uint32_t high=0,low=0;sscanf_s(AdapterLuid.c_str(),"%u,%u",&high,&low);
    for(UINT i=0;;++i){ComPtr<IDXGIAdapter1> a;if(factory->EnumAdapters1(i,&a)==DXGI_ERROR_NOT_FOUND)break;
        DXGI_ADAPTER_DESC1 desc{};a->GetDesc1(&desc);
        if(desc.AdapterLuid.LowPart==low && static_cast<uint32_t>(desc.AdapterLuid.HighPart)==high){selected=a;break;}}
    D3D_FEATURE_LEVEL level;
    return SUCCEEDED(D3D11CreateDevice(selected.Get(),selected?D3D_DRIVER_TYPE_UNKNOWN:D3D_DRIVER_TYPE_HARDWARE,
        nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&Device,&level,&Context));
}
class Client : public CefClient,public CefRenderHandler,public CefLifeSpanHandler,public CefLoadHandler,public CefDisplayHandler,public CefRequestHandler {
public:
    CefRefPtr<CefRequestHandler> GetRequestHandler() override{return this;}
    CefRefPtr<CefResourceRequestHandler> GetResourceRequestHandler(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame>,CefRefPtr<CefRequest> request,bool,bool,const CefString&,bool&) override{
        std::lock_guard<std::mutex> lock(ResourceMutex);if(!HTMLOverrideURL.empty()&&request->GetURL().ToString()==HTMLOverrideURL)return new HTMLRequest;return nullptr;
    }
    CefRefPtr<CefRenderHandler> GetRenderHandler() override{return this;}
    CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override{return this;}
    CefRefPtr<CefLoadHandler> GetLoadHandler() override{return this;}
    CefRefPtr<CefDisplayHandler> GetDisplayHandler() override{return this;}
    bool OnConsoleMessage(CefRefPtr<CefBrowser>,cef_log_severity_t level,const CefString& message,const CefString& source,int line) override{
        if(level>=LOGSEVERITY_ERROR)Error("JavaScript: "+message.ToString()+" ("+source.ToString()+":"+std::to_string(line)+")");return false;
    }
    void GetViewRect(CefRefPtr<CefBrowser>,CefRect& r) override{r=CefRect(0,0,ViewWidth,ViewHeight);}
    bool GetScreenInfo(CefRefPtr<CefBrowser>,CefScreenInfo& i) override{
        i.device_scale_factor=1;i.rect=CefRect(0,0,ViewWidth,ViewHeight);i.available_rect=i.rect;return true;
    }
    void OnPaint(CefRefPtr<CefBrowser>,PaintElementType,const RectList&,const void*,int,int) override {
        static bool warned=false;if(!warned){warned=true;Error("CEF used CPU painting; shared GPU rendering is unavailable.");}
    }
    void OnAcceleratedPaint(CefRefPtr<CefBrowser>,PaintElementType type,const RectList&,const CefAcceleratedPaintInfo& info) override {
        if(type!=PET_VIEW || !Device) return;
        const bool profile=!GPUProfilePath.empty()&&ProfileAfter>0&&Milliseconds()>=ProfileAfter&&ProfileRows.size()<120;
        const double entered=profile?Milliseconds():0;
        ComPtr<ID3D11Device1> device1;Device.As(&device1);
        ComPtr<ID3D11Texture2D> source;
        if(FAILED(device1->OpenSharedResource1(info.shared_texture_handle,IID_PPV_ARGS(&source)))){Error("Opening CEF shared texture failed.");return;}
        D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
        Slot* target=nullptr;
        for(auto& [id,slot]:Slots)if(slot.free && slot.width==static_cast<int>(desc.Width) && slot.height==static_cast<int>(desc.Height)){target=&slot;break;}
        if(!target){if(profile)++ProfileDropped;return;} // No free consumer buffer: drop, never wait for Unreal.
        target->free=false;
        const double opened=profile?Milliseconds():0;
        if(profile&&!ProfileDisjoint){D3D11_QUERY_DESC d{D3D11_QUERY_TIMESTAMP_DISJOINT,0};Device->CreateQuery(&d,&ProfileDisjoint);
            d.Query=D3D11_QUERY_TIMESTAMP;Device->CreateQuery(&d,&ProfileBegin);Device->CreateQuery(&d,&ProfileEnd);}
        const bool timestamps=profile&&ProfileDisjoint&&ProfileBegin&&ProfileEnd;
        if(timestamps){Context->Begin(ProfileDisjoint.Get());Context->End(ProfileBegin.Get());}
        const double copyStart=profile?Milliseconds():0;
        Context->CopyResource(target->texture.Get(),source.Get());
        const double copySubmitted=profile?Milliseconds():0;
        if(timestamps){Context->End(ProfileEnd.Get());Context->End(ProfileDisjoint.Get());}
        // CEF 128 only lends its source during this callback. Complete the acquisition
        // here, in the standalone host, before returning it to CEF's pool.
        D3D11_QUERY_DESC qd{D3D11_QUERY_EVENT,0};ComPtr<ID3D11Query> query;
        if(FAILED(Device->CreateQuery(&qd,&query))){Error("GPU copy query failed.");return;}
        Context->End(query.Get());const double flushStart=profile?Milliseconds():0;Context->Flush();
        const double waitStart=profile?Milliseconds():0;
        HRESULT hr;
        while((hr=Context->GetData(query.Get(),nullptr,0,0))==S_FALSE && !Quit) SwitchToThread();
        if(FAILED(hr)){Error("GPU acquisition failed.");return;}
        if(profile){
            const double done=Milliseconds();double gpu=-1;
            if(timestamps){D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};UINT64 begin=0,end=0;
                if(Context->GetData(ProfileDisjoint.Get(),&disjoint,sizeof(disjoint),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK&&!disjoint.Disjoint&&disjoint.Frequency&&
                   Context->GetData(ProfileBegin.Get(),&begin,sizeof(begin),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK&&
                   Context->GetData(ProfileEnd.Get(),&end,sizeof(end),D3D11_ASYNC_GETDATA_DONOTFLUSH)==S_OK)gpu=1000.0*double(end-begin)/double(disjoint.Frequency);}
            std::ostringstream row;row<<std::fixed<<std::setprecision(6)<<ProfileRows.size()+1<<','<<desc.Width<<','<<desc.Height<<','<<opened-entered<<','<<copySubmitted-copyStart<<','<<flushStart-copySubmitted<<','<<waitStart-flushStart<<','<<done-waitStart<<','<<done-entered<<','<<gpu<<','<<ProfileDropped<<'\n';
            ProfileRows.push_back(row.str());
            if(ProfileRows.size()==120)ProfileWriter=std::thread([rows=ProfileRows,path=GPUProfilePath]{
                std::ofstream file(path);file<<"sample,width,height,open_ms,copy_submit_ms,query_setup_ms,flush_ms,wait_ms,acquire_ms,gpu_copy_ms,dropped\n";for(const auto& r:rows)file<<r;});
        }
        auto d=Message("frame");d->SetInt("slot",target->id);d->SetInt("generation",PoolGeneration);
        d->SetString("sequence",std::to_string(++FrameSequence));Send(d);
    }
    void OnAfterCreated(CefRefPtr<CefBrowser> browser) override{Browser=browser;Send(Message("created"));}
    void OnBeforeClose(CefRefPtr<CefBrowser>) override{Browser=nullptr;Quit=true;}
    bool OnBeforePopup(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame>,const CefString&,const CefString&,
        CefLifeSpanHandler::WindowOpenDisposition,bool,const CefPopupFeatures&,CefWindowInfo&,CefRefPtr<CefClient>&,CefBrowserSettings&,
        CefRefPtr<CefDictionaryValue>&,bool*) override{return true;}
    void OnLoadStart(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,TransitionType) override{
        if(frame->IsMain())Send(Message("loading"));
    }
    void OnLoadEnd(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,int) override{
        if(!frame->IsMain())return;
        if(!GPUProfilePath.empty()&&frame->GetURL()!="about:blank"){ProfileURL=frame->GetURL();ProfileAfter=Milliseconds()+1000;}
        std::string bootstrap="globalThis.ue=globalThis.ue||{};ue.webui={broadcast:(name,data,callback)=>__webuiNative(JSON.stringify({op:'event',name,data,callback})),hittestresult:(id,hit)=>__webuiNative(JSON.stringify({op:'hit',id,hit}))};\n";
        bootstrap+=ReadResource("webui.js");
        if(!EnableBridge) bootstrap+="\nue.webui.broadcast=()=>Promise.reject(new Error('Public bridge disabled'));";
        bootstrap+="\n"+ReadResource("selection.js")+"\nwindow.__webuiSelection.setEnabled("+(AllowSelection?std::string("false"):std::string("true"))+");";
        bootstrap+="\nwindow.__webuiIncremental="+std::string(IncrementalDOM?"true":"false")+";window.__webuiAB="+(ABExperiment?"true":"false")+";\n"+ReadResource(ABExperiment||IncrementalDOM?"regions-delta.js":"regions.js");
        frame->ExecuteJavaScript(bootstrap,frame->GetURL(),0);
        auto d=Message("loaded");d->SetString("url",frame->GetURL());Send(d);
    }
    void OnLoadError(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,ErrorCode code,const CefString& text,const CefString&) override{
        if(frame->IsMain() && code!=ERR_ABORTED)Error(text.ToString());
    }
    void OnAddressChange(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,const CefString& url) override{
        if(frame->IsMain()){auto d=Message("url");d->SetString("url",url);Send(d);}
    }
    bool OnProcessMessageReceived(CefRefPtr<CefBrowser>,CefRefPtr<CefFrame> frame,CefProcessId,CefRefPtr<CefProcessMessage> message) override{
        if(message->GetName()!="webui-native" || !frame->IsMain())return false;
        auto v=CefParseJSON(message->GetArgumentList()->GetString(0),JSON_PARSER_RFC);
        if(v && v->GetType()==VTYPE_DICTIONARY){auto d=v->GetDictionary();const auto op=d->GetString("op");
            if(op=="event"||op=="hit"||op=="regions"||op=="regionsDelta"){d->SetString("originUrl",frame->GetURL());Send(d);}}return true;
    }
    IMPLEMENT_REFCOUNTING(Client);
};

static void Receive(std::string json) {
    auto v=CefParseJSON(json,JSON_PARSER_RFC);if(!v || v->GetType()!=VTYPE_DICTIONARY)return;
    auto d=v->GetDictionary();const auto op=d->GetString("op").ToString();
    if(op=="resource") {
        CefRefPtr<Resource> resource;
        {std::lock_guard<std::mutex> lock(ResourceMutex);auto it=PendingResources.find(d->GetInt("id"));if(it!=PendingResources.end()){resource=it->second;PendingResources.erase(it);}}
        if(resource){resource->status=d->GetInt("status");resource->mime=d->GetString("mime");auto data=CefBase64Decode(d->GetString("body"));
            if(data){resource->body.resize(data->GetSize());data->GetData(resource->body.data(),resource->body.size(),0);}resource->callback->Continue();}
        return;
    }
    UI([d,op] {
        if(op=="close"){if(Browser)Browser->GetHost()->CloseBrowser(true);else Quit=true;return;}
        if(op=="pool"){
            Slots.clear();PoolGeneration=d->GetInt("generation");auto list=d->GetList("slots");
            ComPtr<ID3D11Device1> device1;Device.As(&device1);
            for(size_t i=0;i<list->GetSize();++i){auto item=list->GetDictionary(i);Slot s;s.id=item->GetInt("id");s.width=d->GetInt("width");s.height=d->GetInt("height");
                const HANDLE handle=reinterpret_cast<HANDLE>(std::stoull(item->GetString("handle").ToString()));
                HRESULT hr=device1->OpenSharedResource1(handle,IID_PPV_ARGS(&s.texture));CloseHandle(handle);
                if(FAILED(hr)){Error("Opening Unreal ring texture failed: "+std::to_string(hr));return;}Slots.emplace(s.id,std::move(s));}
            if(Browser)Browser->GetHost()->Invalidate(PET_VIEW);return;
        }
        if(op=="release"){auto it=Slots.find(d->GetInt("slot"));if(d->GetInt("generation")==PoolGeneration && it!=Slots.end())it->second.free=true;return;}
        if(op=="resize"){ViewWidth=std::max(1,d->GetInt("width"));ViewHeight=std::max(1,d->GetInt("height"));if(Browser)Browser->GetHost()->WasResized();return;}
        if(!Browser)return;
        auto host=Browser->GetHost();auto frame=Browser->GetMainFrame();
        if(op=="resize"){ViewWidth=std::max(1,d->GetInt("width"));ViewHeight=std::max(1,d->GetInt("height"));host->WasResized();}
        else if(op=="load" || op=="html"){
            {std::lock_guard<std::mutex> lock(ResourceMutex);HTMLOverrideURL=op=="html"?d->GetString("url").ToString():std::string();}
            frame->LoadURL(d->GetString("url"));
        }
        else if(op=="eval")frame->ExecuteJavaScript(d->GetString("script"),frame->GetURL(),0);
        else if(op=="hit")frame->ExecuteJavaScript("if(window.webui)webui._queryHitTest("+std::to_string(d->GetInt("id"))+","+std::to_string(d->GetDouble("u"))+","+std::to_string(d->GetDouble("v"))+");",frame->GetURL(),0);
        else if(op=="focus")host->SetFocus(d->GetBool("focus"));
        else if(op=="reload")Browser->Reload();
        else if(op=="back")Browser->GoBack();
        else if(op=="forward")Browser->GoForward();
        else if(op=="stop")Browser->StopLoad();
        else if(op=="mouse"){
            CefMouseEvent e;e.x=d->GetInt("x");e.y=d->GetInt("y");e.modifiers=d->GetInt("modifiers");
            const auto kind=d->GetString("kind").ToString();
            if(kind=="move")host->SendMouseMoveEvent(e,false);
            else if(kind=="leave")host->SendMouseMoveEvent(e,true);
            else if(kind=="wheel")host->SendMouseWheelEvent(e,0,d->GetInt("delta"));
            else host->SendMouseClickEvent(e,static_cast<cef_mouse_button_type_t>(d->GetInt("button")),kind=="up",d->GetInt("count"));
        }
        else if(op=="key"){
            CefKeyEvent e;e.type=static_cast<cef_key_event_type_t>(d->GetInt("type"));e.windows_key_code=d->GetInt("key");
            e.native_key_code=d->GetInt("native");e.modifiers=d->GetInt("modifiers");e.is_system_key=d->GetBool("system");host->SendKeyEvent(e);
        }
    });
}

int APIENTRY wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR,int) {
    int argc=0;auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    for(int i=1;i<argc;++i){std::wstring argument=argv[i];if(argument.rfind(L"--runtime=",0)==0)Runtime=argument.substr(10);}
    LocalFree(argv);
    if(!Runtime.empty())SetDllDirectoryW(Runtime.c_str());
    auto command=CefCommandLine::CreateCommandLine();command->InitFromString(GetCommandLineW());
    std::wstring runtime=command->GetSwitchValue("runtime").ToWString();
    // Delay-loaded CEF resolves against the engine's versioned runtime directory.
    // Subprocesses inherit the DLL search directory through their explicit --runtime argument.
    if(!runtime.empty())SetDllDirectoryW(runtime.c_str());
    Resources=command->GetSwitchValue("resources").ToWString();AdapterLuid=command->GetSwitchValue("adapter").ToString();
    CefMainArgs args(instance);auto app=CefRefPtr<App>(new App);
    const int subprocess=CefExecuteProcess(args,app,nullptr);if(subprocess>=0)return subprocess;
    wchar_t flag[8];IncrementalDOM=!(GetEnvironmentVariableW(L"WEBUI_INCREMENTAL_DOM",flag,8)==1&&flag[0]==L'0');
    ABExperiment=GetEnvironmentVariableW(L"WEBUI_AB",flag,8)==1&&flag[0]==L'1';
    wchar_t profilePath[32768];const DWORD profileLength=GetEnvironmentVariableW(L"WEBUI_GPU_PROFILE",profilePath,32768);
    if(profileLength>0&&profileLength<32768){GPUProfilePath.assign(profilePath,profileLength);ProfileRows.reserve(120);}
    EnableBridge=!command->HasSwitch("no-bridge");AllowSelection=command->HasSwitch("allow-selection");
    InitialURL=command->GetSwitchValue("url").ToString();if(InitialURL.empty())InitialURL="about:blank";
    if(command->HasSwitch("fps"))TargetFPS=std::stoi(command->GetSwitchValue("fps").ToString());
    CefSettings settings;settings.no_sandbox=true;settings.windowless_rendering_enabled=true;settings.multi_threaded_message_loop=true;
    CefString(&settings.locale)="en-US";
    CefString(&settings.resources_dir_path)=runtime;CefString(&settings.locales_dir_path)=runtime+L"/Resources/locales";
    CefString(&settings.root_cache_path)=command->GetSwitchValue("cache");
    settings.log_severity=LOGSEVERITY_WARNING;
    std::wstring executable(32768,L'\0');executable.resize(GetModuleFileNameW(nullptr,executable.data(),static_cast<DWORD>(executable.size())));
    CefString(&settings.browser_subprocess_path)=executable;
    if(!InitializeDevice())return 3;
    if(!CefInitialize(args,settings,app,nullptr))return 4;
    Pipe.Start(command->GetSwitchValue("pipe").ToWString(),false,Receive);
    CefRegisterSchemeHandlerFactory("https","ue.local",new ResourceFactory);
    UI([]{CefWindowInfo wi;wi.SetAsWindowless(nullptr);wi.shared_texture_enabled=true;
        CefBrowserSettings bs;bs.windowless_frame_rate=TargetFPS;bs.background_color=0;
        CefBrowserHost::CreateBrowser(wi,new Client,InitialURL,bs,nullptr,nullptr);});
    while(!Quit){Sleep(10);if(!Pipe.connected){static int disconnected=0;if(++disconnected>500){UI([]{if(Browser)Browser->GetHost()->CloseBrowser(true);else Quit=true;});}}}
    Pipe.Stop();if(ProfileWriter.joinable())ProfileWriter.join();CefShutdown();Slots.clear();ProfileDisjoint.Reset();ProfileBegin.Reset();ProfileEnd.Reset();Context.Reset();Device.Reset();return 0;
}
