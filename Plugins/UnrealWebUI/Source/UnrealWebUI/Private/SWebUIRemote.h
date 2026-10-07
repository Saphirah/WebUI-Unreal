#pragma once
#include "WebUIRemoteSession.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SViewport.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Application/IInputProcessor.h"
#include "CoreGlobals.h"

class FWebUIRemoteViewport : public ISlateViewport {
    TSharedRef<FWebUIRemoteSession,ESPMode::ThreadSafe> Session;
public:
    TWeakPtr<SWidget> Owner;
    explicit FWebUIRemoteViewport(TSharedRef<FWebUIRemoteSession,ESPMode::ThreadSafe> In):Session(In){}
    FIntPoint GetSize() const override{return Session->Size();}
    FSlateShaderResource* GetViewportRenderTargetTexture() const override{return Session->Texture();}
    bool RequiresVsync() const override{return false;}
    FReply OnMouseButtonDown(const FGeometry& G,const FPointerEvent& E) override{Session->Mouse(TEXT("down"),G,E);Session->Focus(true);auto W=Owner.Pin();return W?FReply::Handled().CaptureMouse(W.ToSharedRef()).SetUserFocus(W.ToSharedRef()):FReply::Handled();}
    FReply OnMouseButtonUp(const FGeometry& G,const FPointerEvent& E) override{Session->Mouse(TEXT("up"),G,E);return FReply::Handled().ReleaseMouseCapture();}
    FReply OnMouseButtonDoubleClick(const FGeometry& G,const FPointerEvent& E) override{Session->Mouse(TEXT("down"),G,E,2);return FReply::Handled();}
    FReply OnMouseMove(const FGeometry&,const FPointerEvent&) override{return FReply::Handled();}
    FReply OnMouseWheel(const FGeometry& G,const FPointerEvent& E) override{Session->Mouse(TEXT("wheel"),G,E);return FReply::Handled();}
    FReply OnKeyDown(const FGeometry&,const FKeyEvent& E) override{Session->Key(0,E.GetKeyCode(),E.GetCharacter(),Session->Modifiers(E),E.IsAltDown());return FReply::Handled();}
    FReply OnKeyUp(const FGeometry&,const FKeyEvent& E) override{Session->Key(2,E.GetKeyCode(),E.GetCharacter(),Session->Modifiers(E),E.IsAltDown());return FReply::Handled();}
    FReply OnKeyChar(const FGeometry&,const FCharacterEvent& E) override{Session->Key(3,E.GetCharacter(),0,Session->Modifiers(E));return FReply::Handled();}
    FReply OnFocusReceived(const FFocusEvent&) override{Session->Focus(true);return FReply::Handled();}
    void OnFocusLost(const FFocusEvent&) override{Session->Focus(false);}
};
class SWebUIRemote : public SCompoundWidget {
    TSharedPtr<FWebUIRemoteSession,ESPMode::ThreadSafe> Session;
    TSharedPtr<SViewport> Viewport;
    TSharedPtr<FWebUIRemoteViewport> Interface;
    class FMouseObserver : public IInputProcessor {
        uint64 LastFrame = MAX_uint64;
        FVector2D LastPosition = FVector2D::ZeroVector;
        bool HasPosition = false;
    public:
        TWeakPtr<SWebUIRemote> Widget;
        void Tick(const float,FSlateApplication& App,TSharedRef<ICursor>) override{
            if(LastFrame==GFrameCounter)return;
            auto W=Widget.Pin();if(!W)return;
            const auto& G=W->GetCachedGeometry();const auto S=G.GetLocalSize();
            if(S.X<=0||S.Y<=0)return;
            LastFrame=GFrameCounter;
            // Sample the latest position once per game frame, including over click-through areas.
            // Button/wheel events still carry their own position and are sent immediately.
            const FVector2D Position=App.GetCursorPos();
            const FPointerEvent E(0,Position,HasPosition?LastPosition:Position,
                App.GetPressedMouseButtons(),EKeys::Invalid,0,App.GetModifierKeys());
            const auto P=G.AbsoluteToLocal(Position);
            const bool Inside=P.X>=0&&P.Y>=0&&P.X<S.X&&P.Y<S.Y;
            // Captured drags must keep receiving moves even beyond the browser rectangle.
            W->Session->Mouse(Inside||W->Viewport->HasMouseCapture()?TEXT("move"):TEXT("leave"),G,E);
            LastPosition=Position;HasPosition=true;
        }
    };
    TSharedPtr<FMouseObserver> Observer;
public:
    SLATE_BEGIN_ARGS(SWebUIRemote){}SLATE_END_ARGS()
    void Construct(const FArguments&,TSharedRef<FWebUIRemoteSession,ESPMode::ThreadSafe> In){
        Session=In;ChildSlot[SAssignNew(Viewport,SViewport).EnableGammaCorrection(false).EnableBlending(true).IgnoreTextureAlpha(false)];
        Interface=MakeShared<FWebUIRemoteViewport>(In);Interface->Owner=Viewport;Viewport->SetViewportInterface(Interface.ToSharedRef());
        Observer=MakeShared<FMouseObserver>();Observer->Widget=SharedThis(this);FSlateApplication::Get().RegisterInputPreProcessor(Observer);
    }
    ~SWebUIRemote(){if(FSlateApplication::IsInitialized())FSlateApplication::Get().UnregisterInputPreProcessor(Observer);}
    bool SupportsKeyboardFocus() const override{return true;}
    FReply OnFocusReceived(const FGeometry&,const FFocusEvent&) override{return FReply::Handled().SetUserFocus(Viewport.ToSharedRef());}
};
