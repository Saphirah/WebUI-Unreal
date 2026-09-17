#include "WebUIJson.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonSerializer.h"

bool UWebUIJsonLibrary::Parse(const FString& Text, FWebUIJson& Result, FString& Error)
{
    Result = {}; Error.Reset();
    if (Text.Len() > 1024 * 1024) { Error = TEXT("JSON exceeds the 1 Mi-character limit."); return false; }
    // Bound nesting before entering the recursive engine parser. Ignore brackets in strings.
    int32 Depth = 0; bool Quoted = false, Escaped = false;
    for (TCHAR C : Text)
    {
        if (Quoted) { if (Escaped) Escaped = false; else if (C == '\\') Escaped = true; else if (C == '"') Quoted = false; }
        else if (C == '"') Quoted = true;
        else if (C == '[' || C == '{') { if (++Depth > 64) { Error = TEXT("JSON nesting exceeds 64."); return false; } }
        else if (C == ']' || C == '}') --Depth;
    }
    // UE's reader expects an object/array document. A single-element envelope supports scalar roots.
    auto Reader = TJsonReaderFactory<>::Create(TEXT("[") + Text + TEXT("]"));
    TArray<TSharedPtr<FJsonValue>> Envelope;
    if (!FJsonSerializer::Deserialize(Reader, Envelope) || Envelope.Num() != 1)
    { Error = Reader->GetErrorMessage(); if (Error.IsEmpty()) Error = TEXT("Expected exactly one JSON value."); return false; }
    Result.Value = Envelope[0];
    return true;
}
FString UWebUIJsonLibrary::Stringify(const FWebUIJson& Json)
{
    if (!Json.Value.IsValid()) return TEXT("");
    FString Out;
    auto Writer = TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
    // Avoid UE's identifier-writing overload for scalar roots.
    TArray<TSharedPtr<FJsonValue>> Envelope { Json.Value };
    if (!FJsonSerializer::Serialize(Envelope, Writer)) return TEXT("");
    Out = Out.Mid(1, Out.Len() - 2);
    // Valid as a JS expression as well as JSON, including older JS parsers.
    Out.ReplaceInline(TEXT("\u2028"), TEXT("\\u2028"));
    Out.ReplaceInline(TEXT("\u2029"), TEXT("\\u2029"));
    return Out;
}
EWebUIJsonType UWebUIJsonLibrary::Type(const FWebUIJson& Json)
{
    if (!Json.Value) return EWebUIJsonType::Invalid;
    switch (Json.Value->Type)
    {
    case EJson::Null: return EWebUIJsonType::Null;
    case EJson::String: return EWebUIJsonType::String;
    case EJson::Number: return EWebUIJsonType::Number;
    case EJson::Boolean: return EWebUIJsonType::Boolean;
    case EJson::Array: return EWebUIJsonType::Array;
    case EJson::Object: return EWebUIJsonType::Object;
    default: return EWebUIJsonType::Invalid;
    }
}
FWebUIJson UWebUIJsonLibrary::Null() { return FWebUIJson(MakeShared<FJsonValueNull>()); }
FWebUIJson UWebUIJsonLibrary::String(const FString& V) { return FWebUIJson(MakeShared<FJsonValueString>(V)); }
FWebUIJson UWebUIJsonLibrary::Number(double V) { return FMath::IsFinite(V) ? FWebUIJson(MakeShared<FJsonValueNumber>(V)) : FWebUIJson(); }
FWebUIJson UWebUIJsonLibrary::Boolean(bool V) { return FWebUIJson(MakeShared<FJsonValueBoolean>(V)); }
FWebUIJson UWebUIJsonLibrary::Array(const TArray<FWebUIJson>& Values)
{
    TArray<TSharedPtr<FJsonValue>> Out;
    for (const auto& V : Values) { if (!V.Value) return {}; Out.Add(V.Value); }
    return FWebUIJson(MakeShared<FJsonValueArray>(Out));
}
FWebUIJson UWebUIJsonLibrary::Object(const TMap<FString, FWebUIJson>& Fields)
{
    auto Out = MakeShared<FJsonObject>();
    for (const auto& Pair : Fields) { if (!Pair.Value.Value) return {}; Out->SetField(Pair.Key, Pair.Value.Value); }
    return FWebUIJson(MakeShared<FJsonValueObject>(Out));
}
FWebUIJson UWebUIJsonLibrary::Field(const FWebUIJson& J, const FString& Key, bool& Found)
{
    Found = false;
    if (Type(J) != EWebUIJsonType::Object) return {};
    auto V = J.Value->AsObject()->TryGetField(Key); Found = V.IsValid(); return FWebUIJson(V);
}
FWebUIJson UWebUIJsonLibrary::At(const FWebUIJson& J, int32 I, bool& Found)
{
    Found = Type(J) == EWebUIJsonType::Array && J.Value->AsArray().IsValidIndex(I);
    return Found ? FWebUIJson(J.Value->AsArray()[I]) : FWebUIJson();
}
TArray<FString> UWebUIJsonLibrary::Keys(const FWebUIJson& J)
{
    TArray<FString> Out;
    if (Type(J) == EWebUIJsonType::Object)
        for (const auto& Entry : J.Value->AsObject()->Values) Out.Add(FString(Entry.Key));
    return Out;
}
int32 UWebUIJsonLibrary::Length(const FWebUIJson& J)
{
    if (Type(J) == EWebUIJsonType::Array) return J.Value->AsArray().Num();
    if (Type(J) == EWebUIJsonType::Object) return J.Value->AsObject()->Values.Num();
    return 0;
}
bool UWebUIJsonLibrary::AsString(const FWebUIJson& J, FString& V) { V.Reset(); return Type(J) == EWebUIJsonType::String && J.Value->TryGetString(V); }
bool UWebUIJsonLibrary::AsNumber(const FWebUIJson& J, double& V) { V = 0; return Type(J) == EWebUIJsonType::Number && J.Value->TryGetNumber(V); }
bool UWebUIJsonLibrary::AsBoolean(const FWebUIJson& J, bool& V) { V = false; return Type(J) == EWebUIJsonType::Boolean && J.Value->TryGetBool(V); }
FWebUIJson UWebUIJsonLibrary::WithField(const FWebUIJson& J, const FString& Key, const FWebUIJson& V)
{
    if (Type(J) != EWebUIJsonType::Object || !V.Value) return {};
    auto Out = MakeShared<FJsonObject>(); Out->Values = J.Value->AsObject()->Values; Out->SetField(Key, V.Value);
    return FWebUIJson(MakeShared<FJsonValueObject>(Out));
}
