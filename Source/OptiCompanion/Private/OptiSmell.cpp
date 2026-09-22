#include "OptiSmell.h"
#include "OptiProbe.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Components/LightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/SkinnedMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInterface.h"
#include "Scalability.h"
#include "Components/LocalLightComponent.h"
#include "Engine/Texture2D.h"
#include "OptiInventory.h"
#include "RHI.h"

namespace EOptiGlom
{
	namespace
	{
		// The 51 uniglomerular PN types of the adult antennal lobe, in the order the inputs above are wired.
		const TCHAR* const RealNames[Count] = {
			TEXT("DA1"), TEXT("DA2"), TEXT("DA3"), TEXT("DA4l"), TEXT("DA4m"), TEXT("DC1"), TEXT("DC2"), TEXT("DC3"),
			TEXT("DC4"), TEXT("DL1"), TEXT("DL2d"), TEXT("DL2v"), TEXT("DL3"), TEXT("DL4"), TEXT("DL5"), TEXT("DM1"),
			TEXT("DM2"), TEXT("DM3"), TEXT("DM4"), TEXT("DM5"), TEXT("DM6"), TEXT("DP1l"), TEXT("DP1m"), TEXT("D"),
			TEXT("VA1d"), TEXT("VA1v"), TEXT("VA2"), TEXT("VA3"), TEXT("VA4"), TEXT("VA5"), TEXT("VA6"), TEXT("VA7l"),
			TEXT("VA7m"), TEXT("VC1"), TEXT("VC2"), TEXT("VC3"), TEXT("VC4"), TEXT("VC5"), TEXT("VL1"), TEXT("VL2a"),
			TEXT("VL2p"), TEXT("VM1"), TEXT("VM2"), TEXT("VM3"), TEXT("VM4"), TEXT("VM5d"), TEXT("VM5v"), TEXT("VM7d"),
			TEXT("VM7v"), TEXT("V"), TEXT("VP1d"),
		};

		const TCHAR* const InputNames[Count] = {
			TEXT("ShadowDepths"), TEXT("ShadowProjection"), TEXT("DirectLighting"), TEXT("LumenScene"), TEXT("LumenGI"),
			TEXT("LumenReflections"), TEXT("Reflections"), TEXT("Translucency"), TEXT("FX"), TEXT("PostProcess"),
			TEXT("Upscale"), TEXT("MotionBlur"), TEXT("DepthOfField"), TEXT("BasePass"), TEXT("Prepass"),
			TEXT("Nanite"), TEXT("Clouds"), TEXT("Fog"), TEXT("Sky"), TEXT("RayTracing"),
			TEXT("Occlusion"), TEXT("OtherPasses"),
			TEXT("GpuMs"), TEXT("FrameMs"), TEXT("GameThreadMs"), TEXT("RenderThreadMs"), TEXT("RhiThreadMs"), TEXT("DrawCalls"), TEXT("Primitives"),
			TEXT("GpuBound"), TEXT("GameThreadBound"), TEXT("RenderThreadBound"),
			TEXT("ShadowLights"), TEXT("TextureMemory"), TEXT("StaticMeshes"), TEXT("InstancedFraction"), TEXT("NaniteFraction"),
			TEXT("Translucent"), TEXT("Niagara"), TEXT("MaterialCost"), TEXT("TrianglesPerMesh"), TEXT("Foliage"),
			TEXT("sg.ViewDistance"), TEXT("sg.AntiAliasing"), TEXT("sg.Shadow"), TEXT("sg.GlobalIllumination"), TEXT("sg.Reflection"),
			TEXT("sg.PostProcess"), TEXT("sg.Texture"), TEXT("sg.Effects"), TEXT("sg.Foliage"),
		};
	}

	const TCHAR* RealName(int32 Glomerulus)
	{
		return Glomerulus >= 0 && Glomerulus < Count ? RealNames[Glomerulus] : TEXT("?");
	}

	int32 FromRealName(const FString& Name)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			if (Name.Equals(RealNames[Index], ESearchCase::IgnoreCase))
			{
				return Index;
			}
		}
		return INDEX_NONE;
	}

	const TCHAR* InputName(int32 Glomerulus)
	{
		return Glomerulus >= 0 && Glomerulus < Count ? InputNames[Glomerulus] : TEXT("?");
	}
}

int32 FOptiProfile::PassGroupOf(const FString& Column)
{
	if (!Column.StartsWith(TEXT("GPU/")))
	{
		return INDEX_NONE;
	}
	const FString Pass = Column.RightChop(4).ToLower();

	struct FRule { const TCHAR* Key; int32 Group; };
	// Order matters: the first match wins, so specific names come before generic ones.
	static const FRule Rules[] = {
		{ TEXT("shadowdepth"), EOptiGlom::PassShadowDepths }, { TEXT("virtualshadow"), EOptiGlom::PassShadowDepths },
		{ TEXT("shadowprojection"), EOptiGlom::PassShadowProjection }, { TEXT("shadow"), EOptiGlom::PassShadowProjection },
		{ TEXT("lumenreflection"), EOptiGlom::PassLumenReflections },
		{ TEXT("screenprobe"), EOptiGlom::PassLumenGI }, { TEXT("diffuseindirect"), EOptiGlom::PassLumenGI }, { TEXT("lumengi"), EOptiGlom::PassLumenGI },
		{ TEXT("lumen"), EOptiGlom::PassLumenScene },
		{ TEXT("raytracing"), EOptiGlom::PassRayTracing },
		{ TEXT("reflection"), EOptiGlom::PassReflections }, { TEXT("ssr"), EOptiGlom::PassReflections },
		{ TEXT("translucen"), EOptiGlom::PassTranslucency },
		{ TEXT("fxsystem"), EOptiGlom::PassFX }, { TEXT("niagara"), EOptiGlom::PassFX }, { TEXT("particle"), EOptiGlom::PassFX },
		{ TEXT("volcloud"), EOptiGlom::PassClouds }, { TEXT("volumetriccloud"), EOptiGlom::PassClouds },
		{ TEXT("fog"), EOptiGlom::PassFog },
		{ TEXT("skyatmosphere"), EOptiGlom::PassSky }, { TEXT("sky"), EOptiGlom::PassSky }, { TEXT("capture"), EOptiGlom::PassSky },
		{ TEXT("temporalsuperresolution"), EOptiGlom::PassUpscale }, { TEXT("tsr"), EOptiGlom::PassUpscale }, { TEXT("taa"), EOptiGlom::PassUpscale },
		{ TEXT("motionblur"), EOptiGlom::PassMotionBlur },
		{ TEXT("depthoffield"), EOptiGlom::PassDepthOfField }, { TEXT("dof"), EOptiGlom::PassDepthOfField }, { TEXT("bokeh"), EOptiGlom::PassDepthOfField },
		{ TEXT("postprocess"), EOptiGlom::PassPostProcess }, { TEXT("bloom"), EOptiGlom::PassPostProcess }, { TEXT("tonemap"), EOptiGlom::PassPostProcess },
		{ TEXT("nanite"), EOptiGlom::PassNanite },
		{ TEXT("basepass"), EOptiGlom::PassBasePass },
		{ TEXT("prepass"), EOptiGlom::PassPrepass }, { TEXT("depthpass"), EOptiGlom::PassPrepass },
		{ TEXT("deferredlighting"), EOptiGlom::PassDirectLighting }, { TEXT("lights"), EOptiGlom::PassDirectLighting }, { TEXT("lightgrid"), EOptiGlom::PassDirectLighting },
		{ TEXT("hzb"), EOptiGlom::PassOcclusion }, { TEXT("occlusion"), EOptiGlom::PassOcclusion },
	};
	for (const FRule& Rule : Rules)
	{
		if (Pass.Contains(Rule.Key))
		{
			return Rule.Group;
		}
	}
	return EOptiGlom::PassOther;
}

FOptiProfile FOptiProfile::FromProbe(const FOptiProbeResult& Probe)
{
	FOptiProfile Profile;
	for (const FOptiStatResult& Stat : Probe.Stats)
	{
		Profile.Columns.Add(Stat.Name, Stat.MeanA);
		const int32 Group = PassGroupOf(Stat.Name);
		if (Group != INDEX_NONE)
		{
			Profile.PassMs[Group] += Stat.MeanA;
		}
	}
	auto Get = [&Profile](const TCHAR* Name) { const double* Value = Profile.Columns.Find(Name); return Value ? *Value : 0.0; };
	Profile.GpuMs = Get(TEXT("GPUTime"));
	Profile.FrameMs = Get(TEXT("FrameTime"));
	Profile.GameThreadMs = Get(TEXT("GameThreadTime"));
	Profile.RenderThreadMs = Get(TEXT("RenderThreadTime"));
	Profile.RhiThreadMs = Get(TEXT("RHIThreadTime"));
	Profile.DrawCalls = Get(TEXT("RHI/DrawCalls"));
	Profile.Primitives = Get(TEXT("RHI/PrimitivesDrawn"));
	return Profile;
}

FOptiSceneScent FOptiSceneScent::FromWorld(UWorld* World, const FOptiView& View)
{
	FOptiSceneScent Scent;
	if (!World)
	{
		return Scent;
	}

	// What each material brings, cached: its textures and shader cost do not change between sniffs.
	struct FMaterialScent { TArray<TWeakObjectPtr<UTexture2D>> Textures; int32 Instructions = 0; };
	static TMap<TWeakObjectPtr<UMaterialInterface>, FMaterialScent> MaterialCache;

	// Big levels can have hundreds of thousands of components; a sample is enough to smell the scene.
	constexpr int32 MaxComponents = 50000;
	int32 Visited = 0;
	double Triangles = 0.0;
	int32 MeshesWithTriangles = 0;
	double InstructionSum = 0.0;
	int32 InstructionUses = 0;
	TSet<UTexture2D*> Textures;

	for (TActorIterator<AActor> It(World); It && Visited < MaxComponents; ++It)
	{
		It->ForEachComponent<UPrimitiveComponent>(false, [&](UPrimitiveComponent* Primitive)
		{
			if (!View.SeesSphere(Primitive->Bounds.Origin, Primitive->Bounds.SphereRadius))
			{
				return;
			}
			++Visited;
			if (UInstancedStaticMeshComponent* Instanced = Cast<UInstancedStaticMeshComponent>(Primitive))
			{
				const int32 Count = Instanced->GetInstanceCount();
				if (Instanced->GetClass()->GetName().Contains(TEXT("Foliage")))
				{
					Scent.FoliageInstances += Count;
				}
				else
				{
					Scent.Instances += Count;
				}
			}
			else if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(Primitive))
			{
				++Scent.StaticMeshes;
				if (UStaticMesh* Asset = Mesh->GetStaticMesh())
				{
					if (Asset->IsNaniteEnabled())
					{
						++Scent.NaniteMeshes;
					}
					else if (Asset->GetNumLODs() > 0)
					{
						Triangles += Asset->GetNumTriangles(0);
						++MeshesWithTriangles;
					}
				}
			}
			else if (Primitive->IsA<USkinnedMeshComponent>())
			{
				++Scent.Skeletal;
			}
			else if (Primitive->GetClass()->GetName().Contains(TEXT("Niagara")))
			{
				++Scent.Niagara;
			}

			bool bTranslucent = false;
			for (int32 Slot = 0; Slot < Primitive->GetNumMaterials(); ++Slot)
			{
				UMaterialInterface* Material = Primitive->GetMaterial(Slot);
				if (!Material)
				{
					continue;
				}
				bTranslucent |= IsTranslucentBlendMode(Material->GetBlendMode());
				FMaterialScent* Cached = MaterialCache.Find(Material);
				if (!Cached)
				{
					Cached = &MaterialCache.Add(Material);
					TArray<UTexture*> Used;
					Material->GetUsedTextures(Used); // all quality levels and platforms (the 5-argument overload is an empty stub since 5.7)
					for (UTexture* Texture : Used)
					{
						if (UTexture2D* Texture2D = Cast<UTexture2D>(Texture))
						{
							Cached->Textures.Add(Texture2D);
						}
					}
					Cached->Instructions = OptiInventory::PixelInstructions(Material);
				}
				for (const TWeakObjectPtr<UTexture2D>& Texture : Cached->Textures)
				{
					if (Texture.IsValid())
					{
						Textures.Add(Texture.Get());
					}
				}
				if (Cached->Instructions > 0)
				{
					InstructionSum += Cached->Instructions;
					++InstructionUses;
				}
			}
			Scent.Translucent += bTranslucent ? 1 : 0;
		});

		It->ForEachComponent<ULightComponent>(false, [&Scent, &View](ULightComponent* Light)
		{
			const ULocalLightComponent* Local = Cast<ULocalLightComponent>(Light);
			if (Local && !View.SeesSphere(Light->GetComponentLocation(), Local->AttenuationRadius))
			{
				return;
			}
			++Scent.Lights;
			if (Light->CastShadows && Light->Mobility != EComponentMobility::Static)
			{
				++Scent.ShadowLights;
			}
		});
	}

	for (UTexture2D* Texture : Textures)
	{
		Scent.TextureMB += OptiInventory::EstimateTextureMB(Texture->GetSizeX(), Texture->GetSizeY(), Texture->CompressionSettings, Texture->GetNumMips());
	}
	Scent.MaterialInstructions = InstructionUses > 0 ? InstructionSum / InstructionUses : 0.0;
	Scent.TrianglesPerMesh = MeshesWithTriangles > 0 ? Triangles / MeshesWithTriangles : 0.0;
	return Scent;
}

namespace
{
	float LogScale(double Value, double Max)
	{
		return FMath::Clamp(static_cast<float>(FMath::Loge(1.0 + FMath::Max(0.0, Value)) / FMath::Loge(1.0 + Max)), 0.f, 1.f);
	}
}

FOptiSmell FOptiSmell::Sniff(const FOptiProfile& Profile, UWorld* World, const FOptiView& View)
{
	return FromParts(Profile, FOptiSceneScent::FromWorld(World, View), ContextKeyFor(World));
}

FOptiSmell FOptiSmell::FromParts(const FOptiProfile& Profile, const FOptiSceneScent& Scene, const FString& ContextKey)
{
	FOptiSmell Smell;
	Smell.Profile = Profile;
	Smell.Scene = Scene;

	const Scalability::FQualityLevels Levels = Scalability::GetQualityLevels();
	const int32 Sg[9] = { Levels.ViewDistanceQuality, Levels.AntiAliasingQuality, Levels.ShadowQuality, Levels.GlobalIlluminationQuality,
		Levels.ReflectionQuality, Levels.PostProcessQuality, Levels.TextureQuality, Levels.EffectsQuality, Levels.FoliageQuality };
	FMemory::Memcpy(Smell.Scalability, Sg, sizeof(Sg));

	TArray<float>& R = Smell.Receptors;
	R.Init(0.f, EOptiGlom::Count);

	const double Gpu = FMath::Max(Profile.GpuMs, 0.001);
	for (int32 Pass = 0; Pass < EOptiGlom::NumPasses; ++Pass)
	{
		R[EOptiGlom::FirstPass + Pass] = FMath::Clamp(static_cast<float>(Profile.PassMs[Pass] / Gpu), 0.f, 1.f);
	}
	R[EOptiGlom::GpuMs] = LogScale(Profile.GpuMs, 50.0);
	R[EOptiGlom::FrameMs] = LogScale(Profile.FrameMs, 50.0);
	R[EOptiGlom::GameThreadMs] = LogScale(Profile.GameThreadMs, 50.0);
	R[EOptiGlom::RenderThreadMs] = LogScale(Profile.RenderThreadMs, 50.0);
	R[EOptiGlom::RhiThreadMs] = LogScale(Profile.RhiThreadMs, 50.0);
	R[EOptiGlom::DrawCalls] = LogScale(Profile.DrawCalls, 20000.0);
	R[EOptiGlom::Primitives] = LogScale(Profile.Primitives, 50e6);
	const double Slowest = FMath::Max3(Profile.GpuMs, Profile.GameThreadMs, Profile.RenderThreadMs);
	R[EOptiGlom::GpuBound] = Slowest > 0.0 && Profile.GpuMs >= Slowest ? 1.f : 0.f;
	R[EOptiGlom::GameThreadBound] = Slowest > 0.0 && Profile.GameThreadMs >= Slowest ? 1.f : 0.f;
	R[EOptiGlom::RenderThreadBound] = Slowest > 0.0 && Profile.RenderThreadMs >= Slowest ? 1.f : 0.f;

	const FOptiSceneScent& S = Smell.Scene;
	const double Meshes = FMath::Max(1, S.StaticMeshes + S.Instances);
	R[EOptiGlom::SceneShadowLights] = LogScale(S.ShadowLights, 200.0);
	R[EOptiGlom::SceneTextureMemory] = LogScale(S.TextureMB, 4096.0);
	R[EOptiGlom::SceneStaticMeshes] = LogScale(S.StaticMeshes, 100000.0);
	R[EOptiGlom::SceneInstancedFraction] = static_cast<float>(S.Instances / Meshes);
	R[EOptiGlom::SceneNaniteFraction] = S.StaticMeshes > 0 ? static_cast<float>(S.NaniteMeshes) / S.StaticMeshes : 0.f;
	R[EOptiGlom::SceneTranslucent] = LogScale(S.Translucent, 5000.0);
	R[EOptiGlom::SceneNiagara] = LogScale(S.Niagara, 1000.0);
	R[EOptiGlom::SceneMaterialCost] = LogScale(S.MaterialInstructions, 1000.0);
	R[EOptiGlom::SceneTrianglesPerMesh] = LogScale(S.TrianglesPerMesh, 1e6);
	R[EOptiGlom::SceneFoliage] = LogScale(S.FoliageInstances, 1e6);

	for (int32 Group = 0; Group < 9; ++Group)
	{
		R[EOptiGlom::SgViewDistance + Group] = FMath::Clamp(Sg[Group] / 3.f, 0.f, 1.f);
	}

	// Lateral inhibition from local interneurons: every PN is divided by the summed input of the whole lobe,
	// so a loud frame and a quiet frame with the same shape produce similar PN patterns.
	constexpr float RMax = 1.f, Sigma = 0.12f, M = 0.02f;
	float Total = 0.f;
	for (float Value : R) { Total += Value; }
	const float Inhibition = FMath::Pow(M * Total, 1.5f);
	Smell.Projection.SetNumUninitialized(EOptiGlom::Count);
	for (int32 Index = 0; Index < EOptiGlom::Count; ++Index)
	{
		const float Drive = FMath::Pow(R[Index], 1.5f);
		Smell.Projection[Index] = RMax * Drive / (FMath::Pow(Sigma, 1.5f) + Drive + Inhibition);
	}

	Smell.ContextKey = ContextKey;
	return Smell;
}

FString FOptiSmell::ContextKeyFor(UWorld* World)
{
	const Scalability::FQualityLevels L = Scalability::GetQualityLevels();
	return FString::Printf(TEXT("%s|sg%d%d%d%d%d%d%d%d%d|res%.0f"),
		World ? *World->GetMapName() : TEXT("none"), L.ViewDistanceQuality, L.AntiAliasingQuality, L.ShadowQuality, L.GlobalIlluminationQuality,
		L.ReflectionQuality, L.PostProcessQuality, L.TextureQuality, L.EffectsQuality, L.FoliageQuality, L.ResolutionQuality);
}
