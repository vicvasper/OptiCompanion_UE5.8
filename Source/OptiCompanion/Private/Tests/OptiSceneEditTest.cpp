#include "Misc/AutomationTest.h"
#include "OptiActions.h"
#include "Components/PointLightComponent.h"
#include "Engine/PointLight.h"
#include "Engine/World.h"

#if WITH_DEV_AUTOMATION_TESTS

/** Scene actions must find the right components, switch them A <-> B in memory and survive serialization. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOptiSceneEditTest, "OptiCompanion.Scene.EditRoundTrip",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FOptiSceneEditTest::RunTest(const FString& Parameters)
{
	UWorld* World = UWorld::CreateWorld(EWorldType::Game, false, TEXT("OptiSceneEditTest"));
	ON_SCOPE_EXIT { World->DestroyWorld(false); };

	auto SpawnLight = [World](float Radius)
	{
		APointLight* Light = World->SpawnActor<APointLight>();
		UPointLightComponent* Component = CastChecked<UPointLightComponent>(Light->GetLightComponent());
		Component->SetMobility(EComponentMobility::Movable);
		Component->SetAttenuationRadius(Radius);
		Component->SetCastShadows(true);
		return Component;
	};
	UPointLightComponent* Small = SpawnLight(300.f);
	UPointLightComponent* Big = SpawnLight(5000.f);

	const FOptiAction* NoShadow = OptiActions::Find(TEXT("scene.small_lights_no_shadow"));
	const FOptiAction* Radius = OptiActions::Find(TEXT("scene.light_radius"));
	if (!TestNotNull(TEXT("small-lights action exists"), NoShadow) || !TestNotNull(TEXT("radius action exists"), Radius))
	{
		return false;
	}

	FOptiCVarSet A, B;
	TestTrue(TEXT("Scene action is runnable when targets exist"), NoShadow->BuildVariants(A, B, World));
	TestFalse(TEXT("Scene action needs a world"), NoShadow->BuildVariants(A, B, nullptr));

	const TArray<FOptiSceneEdit> Edits = OptiScene::Collect(*NoShadow, World);
	TestEqual(TEXT("Only the small light is targeted"), Edits.Num(), 1);
	TestTrue(TEXT("It is the small light"), Edits.Num() == 1 && Edits[0].Component.Get() == Small);

	OptiScene::SetVariant(Edits, true);
	TestFalse(TEXT("B switches its shadow off"), Small->CastShadows != 0);
	TestTrue(TEXT("The big light is untouched"), Big->CastShadows != 0);
	OptiScene::SetVariant(Edits, false);
	TestTrue(TEXT("A restores the shadow"), Small->CastShadows != 0);

	const TArray<FOptiSceneEdit> RadiusEdits = OptiScene::Collect(*Radius, World);
	TestEqual(TEXT("Only the oversized light is targeted"), RadiusEdits.Num(), 1);
	OptiScene::SetVariant(RadiusEdits, true);
	TestEqual(TEXT("Radius shrinks by 40%"), Big->AttenuationRadius, 3000.f, 0.5f);
	OptiScene::SetVariant(RadiusEdits, false);
	TestEqual(TEXT("Radius is restored"), Big->AttenuationRadius, 5000.f, 0.5f);

	const TArray<FOptiSceneEdit> Loaded = OptiScene::Deserialize(OptiScene::Serialize(Edits));
	TestEqual(TEXT("Serialization keeps every edit"), Loaded.Num(), Edits.Num());
	TestTrue(TEXT("Serialized edits resolve to the same component"), Loaded.Num() == 1 && Loaded[0].Component.Get() == Small);
	return true;
}

#endif
