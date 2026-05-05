// Copyright Epic Games, Inc. All Rights Reserved.

#include "CoreMinimal.h"
#include "Misc/AutomationTest.h"
#include "MCPRequestRouter.h"
#include "Services/IMCPService.h"

#if WITH_DEV_AUTOMATION_TESTS

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FMCPDocQualityTest,
    "SpecialAgent.Docs.ToolDescriptionsMeetQualityBar",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FMCPDocQualityTest::RunTest(const FString&)
{
    static const TCHAR* DeprecatedNeedles[] = {
        TEXT("EditorLevelLibrary"),
        TEXT("EditorAssetLibrary"),
        TEXT("EditorFilterLibrary"),
        TEXT("EditorLevelUtils"),
    };

    FMCPRequestRouter Router;
    int32 Failures = 0;

    for (const auto& Pair : Router.GetServicesForTest())
    {
        const FString& Prefix = Pair.Key;
        const TArray<FMCPToolInfo> Tools = Pair.Value->GetAvailableTools();
        for (const FMCPToolInfo& T : Tools)
        {
            const FString Where = FString::Printf(TEXT("%s/%s"), *Prefix, *T.Name);

            if (T.Description.Len() < 80)
            {
                AddError(FString::Printf(TEXT("%s: description < 80 chars (got %d)"), *Where, T.Description.Len()));
                ++Failures;
            }
            if (!T.Description.Contains(TEXT("Params:")))
            {
                AddError(FString::Printf(TEXT("%s: missing 'Params:' (use 'Params: (none)' for zero-arg tools)"), *Where));
                ++Failures;
            }
            if (!T.Description.Contains(TEXT("Workflow:")) && !T.Description.Contains(TEXT("Warning:")))
            {
                AddError(FString::Printf(TEXT("%s: missing 'Workflow:' or 'Warning:'"), *Where));
                ++Failures;
            }
            for (const TCHAR* Needle : DeprecatedNeedles)
            {
                if (T.Description.Contains(Needle))
                {
                    AddError(FString::Printf(TEXT("%s: description mentions deprecated symbol '%s'"), *Where, Needle));
                    ++Failures;
                }
            }
        }
    }

    return Failures == 0;
}

#endif // WITH_DEV_AUTOMATION_TESTS
