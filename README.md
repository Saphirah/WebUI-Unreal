# WebUI-Unreal

Transparent Chromium HTML/CSS/JavaScript in UMG, with mouse/keyboard input, click-through areas and JSON communication between JavaScript, Blueprints and C++. Windows x64, Unreal Engine 5.7 and DirectX 12. The current browser host requires the CEF 128 libraries bundled with UE 5.7; newer UE versions must provide the same libraries.

## Install

1. Clone or download this repository. Copy `Plugins/UnrealWebUI` into `<YourProject>/Plugins/UnrealWebUI`.
2. Install Visual Studio 2022 or its Build Tools with the C++ toolchain and a Windows SDK. For a Blueprint-only project, add a C++ class so the project can build the source plugin.
3. Enable **Unreal Web UI** under **Edit > Plugins**, rebuild the project's Editor target and restart. The plugin's pre-build step compiles its browser host automatically using the engine's CEF libraries.
4. Under **Project Settings > Platforms > Windows**, set **Default RHI** to **DirectX 12** and restart if changed. Keep **Independent Browser** enabled on the widget.

## Widget setup

1. Place your web build, including assets, in `<YourProject>/Content/WebUI/`.
2. Create a **User Widget Blueprint**, add **Web UI > Web Interface** from the Designer palette, name it `Browser` and enable **Is Variable**. Make it fill the screen, e.g. with full-stretch anchors and zero offsets inside a Canvas Panel.
3. Set its **Initial URL** to `https://ue.local/WebUI/index.html`. For a development server, use e.g. `http://localhost:5173` and add that exact origin to **Trusted Origins**. Keep **Enable Bridge** enabled for your application.
4. In the Player Controller's **BeginPlay**, use **Create Widget** with your Widget Blueprint and the owning Player Controller, then **Add to Viewport**. Call **Focus** on its `Browser` with that controller to enable game-and-UI input and show the cursor.
5. Press **Play**. HTML elements are the controls inside the Web Interface; no separate UMG widget is needed per HTML button.

For transparent overlays, use `html,body{margin:0;background:transparent}` and keep **Transparent** and **Click Through** enabled. Use `pointer-events:none` on decorative HTML elements. Keep fullscreen UMG containers **Not Hit-Testable (Self Only)** so they do not consume background clicks. Call **Unfocus** to return to game-only input. Enable **Allow Text Selection** if needed; it defaults to off.

To navigate later, call **Load** with `WebUI/other.html` (relative to Content) or **Load URL** with an HTTP(S) URL. **Load HTML** accepts an HTML string. Set startup fields before creating the widget; changing **Initial URL** later does not navigate it.

## Bridge

In the Widget Blueprint, bind `Browser`'s **On Interface Event**. It provides **Name**, **Data** (`WebUIJson`) and **Callback**:

- If **Name** is `ready`, call `Browser`'s **Call** with Function `setStatus` and Data created by **Web UI > JSON > String** (`Connected to Unreal`).
- If **Callback** is nonempty, call **Call** with Function = **Callback** and Data = the received **Data** to reply. Use the JSON library's **Field**, **As String**, **As Number**, **Object** or **Parse** nodes for your own payloads.

Save this page as `Content/WebUI/index.html`:

```html
<!doctype html>
<html><head><meta charset="utf-8">
<style>html,body{margin:0;background:transparent}button{margin:24px}</style>
<script src="https://ue.local/__webui/webui.js"></script>
</head><body><button id="send">Send JSON</button>
<script>
webui.ready.then(async () => {
  ue.interface.setStatus = text => { document.querySelector('#send').textContent = text; };
  document.querySelector('#send').onclick = async () => {
    const reply = await webui.request('echo', {value: 42}, 5);
    console.log(reply);
  };
  await webui.broadcast('ready', {});
}).catch(console.error);
</script></body></html>
```

`webui.broadcast(name, data)` sends an event; `webui.request(name, data, timeoutSeconds)` waits for your callback reply. **Call** invokes `ue.interface[Function](Data)`. Wait for the page's `ready` event before calling its functions.

For C++, add `UnrealWebUI` and `UMG` to your module's dependencies and include `WebUIWidget.h`. With a `UWebUIWidget* Browser`, bind `OnInterfaceEvent.AddDynamic(this, &UMyWidget::HandleEvent)`. Declare the handler as a `UFUNCTION()` in your widget class header:

```cpp
UFUNCTION()
void HandleEvent(const FString& Name, const FWebUIJson& Data, const FString& Callback);
```

Implement the same reply logic in the `.cpp` file:

```cpp
void UMyWidget::HandleEvent(const FString& Name, const FWebUIJson& Data,
                           const FString& Callback)
{
    if (Name == TEXT("ready"))
        Browser->Call(TEXT("setStatus"), UWebUIJsonLibrary::String(TEXT("Connected to Unreal")));
    if (!Callback.IsEmpty()) Browser->Call(Callback, Data);
}
```

Bind before loading the page. In a C++ `UUserWidget` matching the Blueprint, declare `UPROPERTY(meta=(BindWidget)) TObjectPtr<UWebUIWidget> Browser;` and bind in `NativeConstruct()`. Navigation uses `Browser->Load(TEXT("WebUI/index.html"))` or `Browser->LoadURL(URL)`.

## Package

In **Project Settings > Packaging > Additional Non-Asset Directories to Package**, add `WebUI` so HTML, CSS, JavaScript and assets under Content are included. Package for **Windows** with DirectX 12. The plugin stages its host and bridge resources; the engine supplies its CEF runtime. Use local assets for builds that run without a development server.

## License

This project is MIT.
