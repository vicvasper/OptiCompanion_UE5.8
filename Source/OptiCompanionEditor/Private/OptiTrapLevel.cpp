#include "OptiTrapLevel.h"
#include "OptiTrapAssets.h"
#include "OptiCompanion.h"
#include "OptiInventory.h"
#include "OptiProbe.h"
#include "Misc/Paths.h"

#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Editor.h"
#include "Engine/DirectionalLight.h"
#include "Engine/ExponentialHeightFog.h"
#include "Engine/PointLight.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/StaticMesh.h"
#include "Engine/StaticMeshActor.h"
#include "Components/SkyAtmosphereComponent.h"
#include "Components/VolumetricCloudComponent.h"
#include "FileHelpers.h"
#include "LevelEditorViewport.h"
#include "Math/RandomStream.h"
#include "Engine/Blueprint.h"
#include "Materials/Material.h"

/**
 * A small level that is badly optimized on purpose, so anyone can check that the fly finds real problems.
 * Every trap matches an action in the catalog:
 *
 *  - 36 small point lights with dynamic shadows          -> scene.small_lights_no_shadow
 *  - 12 lights with a 60 m radius                        -> scene.light_radius
 *  - no light has a draw distance                        -> scene.light_draw_distance, lighting.local_light_distance
 *  - every light scatters and shadows volumetric fog     -> scene.lights_volumetric_shadow, atmosphere.fog_*
 *  - 900 tiny props casting shadows, drawn to the horizon -> scene.tiny_objects_no_shadow, scene.cull_small_objects
 *  - real-time sky light capture                         -> scene.skylight_realtime_capture
 *  - volumetric clouds and sky atmosphere                -> atmosphere.cloud_*, atmosphere.sky_samples
 *  - unbound post process with heavy depth of field, motion blur and bloom -> post.*
 *
 * And heavy assets, the kind a real project accumulates (Inventory page and save reflex):
 *  - 24 copies of a 260k-triangle mesh with no LODs and no Nanite, not instanced -> scene.dense_meshes_lod1, geometry.*
 *  - a material with hundreds of instructions and dependent texture reads
 *  - a dozen stacked layers of translucent, per-pixel-lit glass           -> effects.translucency_*
 *  - a 4K texture that never streams and an uncompressed 16-bit HDR texture
 *  - BP_Trap_HeavyProp: dense meshes, three shadowed lights per copy and GetAllActorsOfClass on Tick (costs in Play)
 */
namespace OptiTrap
{
	namespace
	{
		constexpr int32 Seed = 1709;
		const TCHAR* LevelPath = TEXT("/Game/OptiTrap/TrapLevel");

		UStaticMesh* Mesh(const TCHAR* Path)
		{
			return LoadObject<UStaticMesh>(nullptr, Path);
		}

		AStaticMeshActor* SpawnMesh(UWorld* World, UStaticMesh* StaticMesh, const FVector& Location, const FVector& Scale, const FString& Label)
		{
			AStaticMeshActor* Actor = World->SpawnActor<AStaticMeshActor>(Location, FRotator::ZeroRotator);
			Actor->GetStaticMeshComponent()->SetStaticMesh(StaticMesh);
			Actor->SetActorScale3D(Scale);
			Actor->SetActorLabel(Label);
			return Actor;
		}

		void SpawnLight(UWorld* World, const FVector& Location, float Radius, const FLinearColor& Color, const FString& Label)
		{
			APointLight* Light = World->SpawnActor<APointLight>(Location, FRotator::ZeroRotator);
			Light->SetActorLabel(Label);
			UPointLightComponent* Component = CastChecked<UPointLightComponent>(Light->GetLightComponent());
			Component->SetMobility(EComponentMobility::Movable);
			Component->SetIntensityUnits(ELightUnits::Candelas);
			Component->SetIntensity(Radius > 1000.f ? 60.f : 25.f);
			Component->SetLightColor(Color);
			Component->SetAttenuationRadius(Radius);
			Component->SetCastShadows(true);
			Component->SetVolumetricScatteringIntensity(2.f);
			Component->SetCastVolumetricShadow(true);
		}
	}

	bool Build()
	{
		if (!GEditor || GEditor->PlayWorld)
		{
			UE_LOG(LogOptiCompanion, Warning, TEXT("Stop playing in the editor before building the trap level."));
			return false;
		}
		// Never throw away the user's work: offer to save anything dirty first.
		if (!FEditorFileUtils::SaveDirtyPackages(true, true, true))
		{
			return false;
		}

		UWorld* World = UEditorLoadingAndSavingUtils::NewBlankMap(false);
		if (!World)
		{
			return false;
		}
		FRandomStream Rng(Seed);

		// Sky, sun, fog and clouds.
		ADirectionalLight* Sun = World->SpawnActor<ADirectionalLight>(FVector(0, 0, 1000), FRotator(-35.f, 40.f, 0.f));
		Sun->SetActorLabel(TEXT("Sun"));
		Sun->GetComponent()->SetMobility(EComponentMobility::Movable);
		World->SpawnActor<ASkyAtmosphere>()->SetActorLabel(TEXT("SkyAtmosphere"));
		World->SpawnActor<AVolumetricCloud>()->SetActorLabel(TEXT("VolumetricClouds"));

		ASkyLight* Sky = World->SpawnActor<ASkyLight>(FVector(0, 0, 500), FRotator::ZeroRotator);
		Sky->SetActorLabel(TEXT("SkyLight_RealTimeCapture"));
		Sky->GetLightComponent()->SetMobility(EComponentMobility::Movable);
		Sky->GetLightComponent()->SetRealTimeCapture(true);

		AExponentialHeightFog* Fog = World->SpawnActor<AExponentialHeightFog>(FVector(0, 0, 0), FRotator::ZeroRotator);
		Fog->SetActorLabel(TEXT("Fog_Volumetric"));
		Fog->GetComponent()->SetVolumetricFog(true);
		Fog->GetComponent()->SetFogDensity(0.12f);

		// Ground and some pillars to catch light and shadow.
		UStaticMesh* Plane = Mesh(TEXT("/Engine/BasicShapes/Plane.Plane"));
		UStaticMesh* Cube = Mesh(TEXT("/Engine/BasicShapes/Cube.Cube"));
		UStaticMesh* Cylinder = Mesh(TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
		UStaticMesh* Sphere = Mesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
		if (!Plane || !Cube || !Cylinder || !Sphere)
		{
			UE_LOG(LogOptiCompanion, Error, TEXT("Engine basic shapes are missing; cannot build the trap level."));
			return false;
		}
		SpawnMesh(World, Plane, FVector::ZeroVector, FVector(400.f, 400.f, 1.f), TEXT("Ground"));
		for (int32 Index = 0; Index < 140; ++Index)
		{
			const FVector Location(Rng.FRandRange(-9000.f, 9000.f), Rng.FRandRange(-9000.f, 9000.f), 300.f);
			SpawnMesh(World, Cylinder, Location, FVector(1.5f, 1.5f, 6.f), FString::Printf(TEXT("Pillar_%02d"), Index));
		}

		// Trap: hundreds of tiny props, all casting shadows and drawn at any distance.
		for (int32 Index = 0; Index < 2600; ++Index)
		{
			const FVector Location(Rng.FRandRange(-15000.f, 15000.f), Rng.FRandRange(-15000.f, 15000.f), 12.f);
			const float Size = Rng.FRandRange(0.12f, 0.3f);
			SpawnMesh(World, Rng.GetFraction() < 0.5f ? Cube : Sphere, Location, FVector(Size), FString::Printf(TEXT("TinyProp_%03d"), Index));
		}

		// Trap: many small shadow-casting lights, a dozen with absurd radii, none with a draw distance.
		for (int32 Index = 0; Index < 100; ++Index)
		{
			const FVector Location(-7500.f + (Index % 10) * 1700.f, -7500.f + (Index / 10) * 1700.f, 250.f);
			const FLinearColor Color = FLinearColor::MakeFromHSV8(static_cast<uint8>(Rng.RandHelper(255)), 120, 255);
			SpawnLight(World, Location, 450.f, Color, FString::Printf(TEXT("SmallLight_%02d"), Index));
		}
		for (int32 Index = 0; Index < 24; ++Index)
		{
			const FVector Location(Rng.FRandRange(-8000.f, 8000.f), Rng.FRandRange(-8000.f, 8000.f), 600.f);
			SpawnLight(World, Location, 6000.f, FLinearColor(1.f, 0.85f, 0.7f), FString::Printf(TEXT("HugeRadiusLight_%02d"), Index));
		}

		// Trap: an unbound post process volume with expensive, maxed-out effects.
		APostProcessVolume* Post = World->SpawnActor<APostProcessVolume>();
		Post->SetActorLabel(TEXT("PostProcess_Heavy"));
		Post->bUnbound = true;
		FPostProcessSettings& Settings = Post->Settings;
		Settings.bOverride_MotionBlurAmount = true;
		Settings.MotionBlurAmount = 1.f;
		Settings.bOverride_DepthOfFieldFstop = true;
		Settings.DepthOfFieldFstop = 1.2f;
		Settings.bOverride_DepthOfFieldFocalDistance = true;
		Settings.DepthOfFieldFocalDistance = 2500.f;
		Settings.bOverride_BloomIntensity = true;
		Settings.BloomIntensity = 2.f;

		// Heavy assets, placed in front of the camera.
		const FHeavyAssets Heavy = CreateHeavyAssets();
		if (Heavy.IsValid())
		{
			for (int32 Index = 0; Index < 110; ++Index)
			{
				const FVector Location(Rng.FRandRange(-10000.f, -3500.f), Rng.FRandRange(-5500.f, 2500.f), Rng.FRandRange(150.f, 700.f));
				AStaticMeshActor* Actor = SpawnMesh(World, Heavy.DenseMesh, Location, FVector(Rng.FRandRange(2.f, 4.5f)), FString::Printf(TEXT("DenseMesh_NoLOD_%03d"), Index));
				Actor->GetStaticMeshComponent()->SetMaterial(0, Heavy.HeavyOpaque);
			}
			for (int32 Index = 0; Index < 30; ++Index)
			{
				const FVector Location(-8600.f + (Index % 10) * 500.f, 1600.f + (Index / 10) * 900.f, 150.f + (Index % 3) * 260.f);
				AStaticMeshActor* Actor = SpawnMesh(World, Cube, Location, FVector(4.f), FString::Printf(TEXT("HeavyMaterialBlock_%02d"), Index));
				Actor->GetStaticMeshComponent()->SetMaterial(0, Heavy.HeavyOpaque);
			}
			// A wall of glass layers across the view: every pixel behind it is shaded once per layer.
			for (int32 Index = 0; Index < 26; ++Index)
			{
				AStaticMeshActor* Actor = SpawnMesh(World, Plane, FVector(-9500.f + Index * 30.f, -2000.f, 700.f), FVector(1.f, 30.f, 18.f),
					FString::Printf(TEXT("GlassLayer_%02d"), Index));
				Actor->SetActorRotation(FRotator(90.f, 0.f, 0.f));
				Actor->GetStaticMeshComponent()->SetMaterial(0, Heavy.HeavyGlass);
			}
			if (UClass* PropClass = Heavy.HeavyProp->GeneratedClass)
			{
				for (int32 Index = 0; Index < 26; ++Index)
				{
					const FVector Location(Rng.FRandRange(-9500.f, -4000.f), Rng.FRandRange(-4500.f, 1500.f), 0.f);
					if (AActor* Prop = World->SpawnActor<AActor>(PropClass, Location, FRotator(0.f, Rng.FRandRange(0.f, 360.f), 0.f)))
					{
						Prop->SetActorLabel(FString::Printf(TEXT("BP_HeavyProp_%02d"), Index));
					}
				}
			}
		}
		else
		{
			UE_LOG(LogOptiCompanion, Warning, TEXT("Some heavy trap assets could not be created; the level has only the setting traps."));
		}

		// Look at the busy part of the level so naps measure the traps.
		if (GCurrentLevelEditingViewportClient)
		{
			GCurrentLevelEditingViewportClient->SetViewLocation(FVector(-11000.f, -2500.f, 900.f));
			GCurrentLevelEditingViewportClient->SetViewRotation(FRotator(-6.f, 12.f, 0.f));
			GCurrentLevelEditingViewportClient->Invalidate();
		}

		const bool bSaved = UEditorLoadingAndSavingUtils::SaveMap(World, LevelPath);
		UE_LOG(LogOptiCompanion, Display, TEXT("Trap level built%s: 124 shadow-casting lights (24 with 60 m radius), 2600 tiny props, dense volumetric fog, clouds, real-time sky capture, heavy post process, 110 dense meshes without LODs, 26 stacked translucent layers, heavy materials and textures, and 26 heavy Blueprint props."),
			bSaved ? *FString::Printf(TEXT(" and saved to %s"), LevelPath) : TEXT(" (not saved)"));
		return true;
	}

	FAutoConsoleCommand InventoryCommand(
		TEXT("Opti.Inventory"),
		TEXT("Lists what is in view (or the whole level with 'all'), with estimated memory and warnings, and exports it to Saved/OptiCompanion."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const bool bAll = Args.Contains(TEXT("all"));
			TSharedPtr<FOptiCompanion> Companion = FOptiCompanion::Get();
			const FOptiView View = (!bAll && Companion.IsValid()) ? Companion->CurrentView() : FOptiView();
			const FOptiInventory Inventory = OptiInventory::Collect(GEditor ? GEditor->GetEditorWorldContext().World() : nullptr, View);
			int32 Shown = 0;
			for (const FOptiInventoryItem& Item : Inventory.Items)
			{
				if (Item.Severity > 0 && Shown++ < 25)
				{
					UE_LOG(LogOptiCompanion, Display, TEXT("[%s] %-12s %-32s %7.2f MB  %s  | %s"), Item.Severity > 1 ? TEXT("!!") : TEXT("! "),
						OptiInventory::LexToString(Item.Type), *Item.Name, Item.MemoryMB, *Item.Details, *Item.Warning);
				}
			}
			const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OptiCompanion") /
				FString::Printf(TEXT("Inventory_%s.json"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S"))));
			Inventory.ExportJson(Path);
			UE_LOG(LogOptiCompanion, Display, TEXT("Inventory (%s): %d items, %d textures (%.1f MB), %d materials, %d warnings. Exported to %s"),
				View.bValid ? TEXT("in view") : TEXT("whole level"), Inventory.Items.Num(), Inventory.Count(FOptiInventoryItem::EType::Texture),
				Inventory.TotalMemoryMB(FOptiInventoryItem::EType::Texture), Inventory.Count(FOptiInventoryItem::EType::Material),
				Inventory.Items.FilterByPredicate([](const FOptiInventoryItem& I) { return I.Severity > 0; }).Num(), *Path);
		}));

	FAutoConsoleCommand BuildCommand(
		TEXT("Opti.BuildTrapLevel"),
		TEXT("Creates /Game/OptiTrap/TrapLevel, a small level with planted performance problems for testing the fly."),
		FConsoleCommandDelegate::CreateLambda([]() { Build(); }));
}
