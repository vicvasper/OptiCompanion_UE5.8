#include "OptiApply.h"
#include "OptiCompanion.h"
#include "OptiNotebook.h"

#include "Engine/Blueprint.h"
#include "GameFramework/Actor.h"

#include "Misc/AutomationTest.h"
#include "Tests/AutomationCommon.h"
#include "Tests/AutomationEditorCommon.h"

#if WITH_DEV_AUTOMATION_TESTS

namespace
{
	DEFINE_LATENT_AUTOMATION_COMMAND_ONE_PARAMETER(FOptiCheckPlayCosts, FAutomationTestBase*, Test);

	bool FOptiCheckPlayCosts::Update()
	{
		TSharedPtr<FOptiCompanion> Companion = FOptiCompanion::Get();
		if (!Companion.IsValid())
		{
			Test->AddError(TEXT("The companion is not running."));
			return true;
		}
		const FOptiBlueprintCost* Heavy = Companion->GetPlayCosts().FindByPredicate([](const FOptiBlueprintCost& Cost)
		{
			return Cost.ClassName == TEXT("BP_Trap_HeavyProp_C");
		});
		Test->TestNotNull(TEXT("BP_Trap_HeavyProp was measured in Play"), Heavy);
		if (Heavy)
		{
			Test->AddInfo(FString::Printf(TEXT("BP_Trap_HeavyProp: %.3f ms/frame over %d instances, %d captures"), Heavy->MsPerFrame, Heavy->Instances, Heavy->Captures));
			Test->TestTrue(TEXT("its Tick has a cost"), Heavy->MsPerFrame > 0.0);
		}
		return true;
	}
}

/**
 * Plays the trap level for a while and checks that the Blueprint with GetAllActorsOfClass on Tick is found.
 * Needs /Game/OptiTrap/TrapLevel (Opti.BuildTrapLevel); slow, so it lives in the stress filter.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOptiPlayBlueprintCostTest, "OptiCompanion.Play.BlueprintCosts",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::StressFilter)

bool FOptiPlayBlueprintCostTest::RunTest(const FString& Parameters)
{
	FAutomationEditorCommonUtils::LoadMap(TEXT("/Game/OptiTrap/TrapLevel"));
	// Smell and choose, but never change your Blueprints or notebook from a test.
	if (IConsoleVariable* DryRun = IConsoleManager::Get().FindConsoleVariable(TEXT("opti.Play.DryRun")))
	{
		DryRun->Set(1, ECVF_SetByCode);
	}
	ADD_LATENT_AUTOMATION_COMMAND(FStartPIECommand(false));
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(40.f)); // warm-up, capture, analysis and the Tick Interval test
	ADD_LATENT_AUTOMATION_COMMAND(FEndPlayMapCommand());
	ADD_LATENT_AUTOMATION_COMMAND(FWaitLatentCommand(5.f));  // a capture cut short by the end of Play is still analysed
	ADD_LATENT_AUTOMATION_COMMAND(FOptiCheckPlayCosts(this));
	return true;
}

/** Applying a Blueprint finding changes the class defaults, and undo puts them back. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOptiBlueprintApplyTest, "OptiCompanion.Play.ApplyTickInterval",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FOptiBlueprintApplyTest::RunTest(const FString& Parameters)
{
	const TCHAR* Path = TEXT("/Game/OptiTrap/BP_Trap_HeavyProp.BP_Trap_HeavyProp");
	UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, Path, nullptr, LOAD_NoWarn | LOAD_Quiet);
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		AddInfo(TEXT("No trap Blueprint in this project (run Opti.BuildTrapLevel); nothing to test."));
		return true;
	}
	AActor* Defaults = Blueprint->GeneratedClass->GetDefaultObject<AActor>();
	const float Original = Defaults->PrimaryActorTick.TickInterval;
	const bool bWasDirty = Blueprint->GetOutermost()->IsDirty();

	FOptiFinding Finding;
	Finding.Kind = EOptiFindingKind::Optimization;
	Finding.ActionId = TEXT("blueprint.tick_interval");
	Finding.Blueprint = Path;
	Finding.From = { { TEXT("TickInterval"), FString::SanitizeFloat(Original) } };
	Finding.To = { { TEXT("TickInterval"), TEXT("0.1") } };

	FText Error;
	TestTrue(TEXT("apply succeeds"), OptiApply::Apply(Finding, Error));
	TestEqual(TEXT("class default after apply"), Defaults->PrimaryActorTick.TickInterval, 0.1f, 0.001f);
	TestTrue(TEXT("matches after apply"), OptiApply::BlueprintMatches(Finding, true));
	TestTrue(TEXT("undo succeeds"), OptiApply::Undo(Finding, Error));
	TestEqual(TEXT("class default after undo"), Defaults->PrimaryActorTick.TickInterval, Original, 0.001f);
	if (!bWasDirty)
	{
		Blueprint->GetOutermost()->SetDirtyFlag(false); // the test changed nothing in the end
	}
	return true;
}

#endif
