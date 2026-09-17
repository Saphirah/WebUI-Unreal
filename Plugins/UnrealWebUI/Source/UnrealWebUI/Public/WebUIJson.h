#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "Dom/JsonValue.h"
#include "WebUIJson.generated.h"

UENUM(BlueprintType)
enum class EWebUIJsonType : uint8 { Invalid, Null, String, Number, Boolean, Array, Object };

/** Immutable JSON value. Invalid is distinct from JSON null. Numbers are IEEE-754 doubles. */
USTRUCT(BlueprintType)
struct UNREALWEBUI_API FWebUIJson
{
    GENERATED_BODY()
    TSharedPtr<FJsonValue> Value;
    FWebUIJson() = default;
    explicit FWebUIJson(TSharedPtr<FJsonValue> InValue) : Value(MoveTemp(InValue)) {}
};

UCLASS()
class UNREALWEBUI_API UWebUIJsonLibrary : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="Web UI|JSON")
    static bool Parse(const FString& Text, FWebUIJson& Result, FString& Error);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FString Stringify(const FWebUIJson& Json);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static EWebUIJsonType Type(const FWebUIJson& Json);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FWebUIJson Null();
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FWebUIJson String(const FString& Value);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FWebUIJson Number(double Value);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FWebUIJson Boolean(bool Value);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FWebUIJson Array(const TArray<FWebUIJson>& Values);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FWebUIJson Object(const TMap<FString, FWebUIJson>& Fields);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FWebUIJson Field(const FWebUIJson& Json, const FString& Key, bool& Found);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FWebUIJson At(const FWebUIJson& Json, int32 Index, bool& Found);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static TArray<FString> Keys(const FWebUIJson& Json);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static int32 Length(const FWebUIJson& Json);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static bool AsString(const FWebUIJson& Json, FString& Value);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static bool AsNumber(const FWebUIJson& Json, double& Value);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static bool AsBoolean(const FWebUIJson& Json, bool& Value);
    UFUNCTION(BlueprintPure, Category="Web UI|JSON")
    static FWebUIJson WithField(const FWebUIJson& Json, const FString& Key, const FWebUIJson& Value);
};
