#pragma once
#include "CoreMinimal.h"
#include "IWebBrowserSchemeHandler.h"

namespace WebUIResources
{
    bool Resolve(const FString& Root, const FString& Relative, FString& Out);
    FString ContentURL(const FString& Relative);
    FString MimeType(const FString& Path);
}
class FWebUIResourceFactory : public IWebBrowserSchemeHandlerFactory
{
public:
    virtual TUniquePtr<IWebBrowserSchemeHandler> Create(FString Verb, FString Url) override;
};
