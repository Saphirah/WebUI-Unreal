#include "Misc/AutomationTest.h"
#include "WebUIJson.h"
#include "WebUIResources.h"
#include "Misc/Paths.h"

#if WITH_DEV_AUTOMATION_TESTS
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebUIJsonTest, "UnrealWebUI.JSON.RoundTrip", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebUIJsonTest::RunTest(const FString&)
{
    FWebUIJson J; FString Error;
    TestTrue(TEXT("Parse mixed nested data"), UWebUIJsonLibrary::Parse(TEXT("{\"text\":\"hello 世界\\n\\\"\",\"items\":[true,null,1.25]}"), J, Error));
    bool Found = false;
    auto Items = UWebUIJsonLibrary::Field(J, TEXT("items"), Found);
    TestTrue(TEXT("Object field exists"), Found);
    TestEqual(TEXT("Array length"), UWebUIJsonLibrary::Length(Items), 3);
    auto Null = UWebUIJsonLibrary::At(Items, 1, Found);
    TestEqual(TEXT("Null distinguished"), UWebUIJsonLibrary::Type(Null), EWebUIJsonType::Null);
    UWebUIJsonLibrary::At(Items, -1, Found); TestFalse(TEXT("Negative index rejected"), Found);
    FWebUIJson RoundTrip;
    TestTrue(TEXT("Serialized output reparses"), UWebUIJsonLibrary::Parse(UWebUIJsonLibrary::Stringify(J), RoundTrip, Error));
    TestFalse(TEXT("Malformed data rejected"), UWebUIJsonLibrary::Parse(TEXT("{bad}"), J, Error));
    TestEqual(TEXT("Failed parse clears output"), UWebUIJsonLibrary::Type(J), EWebUIJsonType::Invalid);
    TestFalse(TEXT("Trailing data rejected"), UWebUIJsonLibrary::Parse(TEXT("{}{}"), J, Error));
    for (const FString& Text : {FString(TEXT("null")), FString(TEXT("true")), FString(TEXT("42")), FString(TEXT("\"x\""))})
    {
        TestTrue(TEXT("Scalar root"), UWebUIJsonLibrary::Parse(Text, J, Error));
        TestEqual(TEXT("Scalar serialization"), UWebUIJsonLibrary::Stringify(J), Text);
    }
    FString Deep; for (int I = 0; I < 65; ++I) Deep += TEXT("["); Deep += TEXT("0"); for (int I = 0; I < 65; ++I) Deep += TEXT("]");
    TestFalse(TEXT("Depth limit"), UWebUIJsonLibrary::Parse(Deep, J, Error));
    auto Original = UWebUIJsonLibrary::Object({{TEXT("a"), UWebUIJsonLibrary::Number(1)}});
    auto Changed = UWebUIJsonLibrary::WithField(Original, TEXT("a"), UWebUIJsonLibrary::Number(2));
    double Number;
    UWebUIJsonLibrary::AsNumber(UWebUIJsonLibrary::Field(Original, TEXT("a"), Found), Number);
    TestEqual(TEXT("Object updates do not mutate aliases"), Number, 1.0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWebUIPathTest, "UnrealWebUI.Resources.Paths", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWebUIPathTest::RunTest(const FString&)
{
    FString Out;
    TestTrue(TEXT("Nested relative asset"), WebUIResources::Resolve(FPaths::ProjectContentDir(), TEXT("WebUI/assets/app.js"), Out));
    for (const FString& Bad : {FString(TEXT("../secret")), FString(TEXT("/absolute")), FString(TEXT("C:/secret")), FString(TEXT("WebUI/../../secret")), FString(TEXT("WebUI/.. /secret")), FString(TEXT("file.txt:stream"))})
        TestFalse(TEXT("Traversal/absolute/ADS rejected"), WebUIResources::Resolve(FPaths::ProjectContentDir(), Bad, Out));
    TestEqual(TEXT("Encode path segments"), WebUIResources::ContentURL(TEXT("UI/a b.js")), FString(TEXT("https://ue.local/UI/a%20b.js")));
    TestEqual(TEXT("JS MIME"), WebUIResources::MimeType(TEXT("app.mjs")), FString(TEXT("text/javascript")));
    return true;
}
#endif
