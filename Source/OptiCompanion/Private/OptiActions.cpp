#include "OptiActions.h"
#include "OptiSmell.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Components/LightComponent.h"
#include "Components/LocalLightComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "UObject/SoftObjectPath.h"
#include "UObject/UnrealType.h"

const TCHAR* LexToString(EOptiSector Sector)
{
	switch (Sector)
	{
	case EOptiSector::Shadows: return TEXT("Shadows");
	case EOptiSector::Lighting: return TEXT("Lighting");
	case EOptiSector::Reflections: return TEXT("Reflections");
	case EOptiSector::Effects: return TEXT("Effects");
	case EOptiSector::PostProcess: return TEXT("PostProcess");
	case EOptiSector::Atmosphere: return TEXT("Atmosphere");
	case EOptiSector::Geometry: return TEXT("Geometry");
	case EOptiSector::CPU: return TEXT("CPU");
	default: return TEXT("?");
	}
}

namespace
{
	FString FormatValue(double Value, bool bInteger)
	{
		if (bInteger)
		{
			return FString::FromInt(FMath::RoundToInt32(Value));
		}
		return FString::SanitizeFloat(Value, 0);
	}

	bool SameValue(const FString& A, const FString& B)
	{
		if (A.Equals(B, ESearchCase::IgnoreCase))
		{
			return true;
		}
		return FCString::IsNumeric(*A) && FCString::IsNumeric(*B) && FMath::IsNearlyEqual(FCString::Atod(*A), FCString::Atod(*B), 1e-3);
	}
}

IConsoleVariable* OptiFindCVar(const FString& Name, FString* OutRealName)
{
	// Renamed CVars leave a "shadow" behind that ensures when used; follow it to the real one instead.
	IConsoleManager& Manager = IConsoleManager::Get();
	IConsoleObject* Object = Manager.FindConsoleObject(*Name, false);
	if (Object && Object->IsShadowObject())
	{
		Object = Object->GetShadowedObject();
	}
	if (!Object || Object->IsDeprecated())
	{
		return nullptr;
	}
	if (OutRealName)
	{
		*OutRealName = Manager.FindConsoleObjectName(Object);
		if (OutRealName->IsEmpty())
		{
			*OutRealName = Name;
		}
	}
	return Object->AsVariable();
}

bool FOptiAction::BuildVariants(FOptiCVarSet& OutA, FOptiCVarSet& OutB, UWorld* World) const
{
	OutA.Reset();
	OutB.Reset();
	if (bPlayOnly)
	{
		return false; // Play experiments build their own variants (see FOptiPlayProfiler)
	}
	if (!bMeasurableInEditor && GIsEditor)
	{
		return false;
	}

	for (const TPair<FString, FString>& Requirement : Requires)
	{
		IConsoleVariable* CVar = OptiFindCVar(Requirement.Key);
		if (!CVar || !SameValue(CVar->GetString(), Requirement.Value))
		{
			return false;
		}
	}

	for (const FOptiCVarChange& Change : Changes)
	{
		FString RealName;
		IConsoleVariable* CVar = OptiFindCVar(Change.Name, &RealName);
		if (!CVar)
		{
			return false;
		}
		const bool bInteger = CVar->IsVariableInt() || CVar->IsVariableBool();
		const double Current = CVar->GetFloat();

		double Target = Current;
		switch (Change.Op)
		{
		case FOptiCVarChange::EOp::Set:
			if (Change.OnlyIfAbove.IsSet() && Current <= Change.OnlyIfAbove.GetValue())
			{
				return false;
			}
			Target = Change.Value;
			break;
		case FOptiCVarChange::EOp::Multiply:
			Target = Current * Change.Value;
			break;
		case FOptiCVarChange::EOp::Add:
			Target = Current + Change.Value;
			break;
		}
		if (Change.WhenDefault.IsSet() && Current <= 0.0)
		{
			Target = Change.WhenDefault.GetValue();
		}
		Target = FMath::Clamp(Target, Change.Min, Change.Max);
		if (bInteger)
		{
			Target = FMath::RoundToDouble(Target);
		}
		if (FMath::IsNearlyEqual(Target, Current, 1e-4))
		{
			return false; // already at the limit, nothing to test
		}

		OutA.Add({ RealName, CVar->GetString() });
		OutB.Add({ RealName, FormatValue(Target, bInteger) });
	}

	if (IsSceneAction())
	{
		return World && !OptiScene::Collect(*this, World, 1).IsEmpty();
	}
	return !OutB.IsEmpty();
}

double FOptiAction::TargetMs(const FOptiProfile& Profile) const
{
	if (Passes.IsEmpty())
	{
		return FMath::Max(Profile.GameThreadMs, Profile.RenderThreadMs);
	}
	double Ms = 0.0;
	for (int32 Pass : Passes)
	{
		if (Pass >= 0 && Pass < EOptiGlom::NumPasses)
		{
			Ms += Profile.PassMs[Pass];
		}
	}
	return Ms;
}

// ---------------------------------------------------------------------------------------------- scene edits

namespace OptiScene
{
	FString GetValue(const UActorComponent& Component, FName Property)
	{
		const FProperty* Prop = FindFProperty<FProperty>(Component.GetClass(), Property);
		if (!Prop)
		{
			return FString();
		}
		FString Text;
		Prop->ExportText_InContainer(0, Text, &Component, nullptr, nullptr, PPF_None);
		return Text;
	}

	bool SetValue(UActorComponent& Component, FName Property, const FString& Value)
	{
		// A few properties are cached elsewhere and need their setter to take effect.
		if (Property == GET_MEMBER_NAME_CHECKED(UPrimitiveComponent, LDMaxDrawDistance))
		{
			if (UPrimitiveComponent* Primitive = Cast<UPrimitiveComponent>(&Component))
			{
				Primitive->SetCullDistance(FCString::Atof(*Value));
				return true;
			}
		}
		if (Property == GET_MEMBER_NAME_CHECKED(UStaticMeshComponent, ForcedLodModel))
		{
			if (UStaticMeshComponent* Mesh = Cast<UStaticMeshComponent>(&Component))
			{
				Mesh->SetForcedLodModel(FCString::Atoi(*Value));
				return true;
			}
		}

		const FProperty* Prop = FindFProperty<FProperty>(Component.GetClass(), Property);
		if (!Prop || !Prop->ImportText_InContainer(*Value, &Component, &Component, PPF_None))
		{
			return false;
		}
		Component.MarkRenderStateDirty();
		return true;
	}

	TArray<FOptiSceneEdit> Collect(const FOptiAction& Action, UWorld* World, int32 MaxEdits)
	{
		TArray<FOptiSceneEdit> Edits;
		if (!World)
		{
			return Edits;
		}
		for (TActorIterator<AActor> It(World); It && Edits.Num() < MaxEdits; ++It)
		{
			if (It->IsTemplate() || It->HasAnyFlags(RF_Transient))
			{
				continue;
			}
			It->ForEachComponent<UActorComponent>(false, [&](UActorComponent* Component)
			{
				if (Edits.Num() >= MaxEdits || Component->IsEditorOnly() || Component->IsVisualizationComponent())
				{
					return;
				}
				for (const FOptiSceneRule& Rule : Action.SceneRules)
				{
					if (!Component->IsA(Rule.ComponentClass) || (Rule.Filter && !Rule.Filter(*Component)))
					{
						continue;
					}
					const FString Before = GetValue(*Component, Rule.Property);
					const FString After = Rule.NewValue(*Component);
					if (!Before.IsEmpty() && !SameValue(Before, After))
					{
						Edits.Add({ Component, Component->GetPathName(), Rule.Property, Before, After });
					}
				}
			});
		}
		return Edits;
	}

	void SetVariant(const TArray<FOptiSceneEdit>& Edits, bool bVariantB)
	{
		for (const FOptiSceneEdit& Edit : Edits)
		{
			if (UActorComponent* Component = Edit.Component.Get())
			{
				SetValue(*Component, Edit.Property, bVariantB ? Edit.After : Edit.Before);
			}
		}
	}

	FString Serialize(const TArray<FOptiSceneEdit>& Edits)
	{
		TArray<FString> Lines;
		for (const FOptiSceneEdit& Edit : Edits)
		{
			Lines.Add(FString::Printf(TEXT("%s\t%s\t%s\t%s"), *Edit.ComponentPath, *Edit.Property.ToString(), *Edit.Before, *Edit.After));
		}
		return FString::Join(Lines, TEXT("\n"));
	}

	TArray<FOptiSceneEdit> Deserialize(const FString& Text)
	{
		TArray<FOptiSceneEdit> Edits;
		TArray<FString> Lines, Fields;
		Text.ParseIntoArrayLines(Lines);
		for (const FString& Line : Lines)
		{
			Line.ParseIntoArray(Fields, TEXT("\t"), false);
			if (Fields.Num() != 4)
			{
				continue;
			}
			FOptiSceneEdit Edit;
			Edit.ComponentPath = Fields[0];
			Edit.Property = FName(*Fields[1]);
			Edit.Before = Fields[2];
			Edit.After = Fields[3];
			Edit.Component = Cast<UActorComponent>(FSoftObjectPath(Edit.ComponentPath).ResolveObject());
			Edits.Add(MoveTemp(Edit));
		}
		return Edits;
	}
}

// ---------------------------------------------------------------------------------------------- catalog

namespace OptiActions
{
	namespace
	{
		using EOp = FOptiCVarChange::EOp;

		FOptiCVarChange Mul(const TCHAR* Name, double Factor, double Min = -DBL_MAX, double Max = DBL_MAX)
		{
			FOptiCVarChange Change;
			Change.Name = Name; Change.Op = EOp::Multiply; Change.Value = Factor; Change.Min = Min; Change.Max = Max;
			return Change;
		}

		FOptiCVarChange Add(const TCHAR* Name, double Delta, double Min = -DBL_MAX, double Max = DBL_MAX)
		{
			FOptiCVarChange Change;
			Change.Name = Name; Change.Op = EOp::Add; Change.Value = Delta; Change.Min = Min; Change.Max = Max;
			return Change;
		}

		FOptiCVarChange SetIfAbove(const TCHAR* Name, double Value, double Above)
		{
			FOptiCVarChange Change;
			Change.Name = Name; Change.Op = EOp::Set; Change.Value = Value; Change.OnlyIfAbove = Above;
			return Change;
		}

		FOptiCVarChange WithDefault(FOptiCVarChange Change, double WhenDefault)
		{
			Change.WhenDefault = WhenDefault;
			return Change;
		}

		/** Switches a feature off when it is on. */
		FOptiCVarChange Off(const TCHAR* Name)
		{
			return SetIfAbove(Name, 0, 0);
		}

		FOptiAction Make(const TCHAR* Id, EOptiSector Sector, FOptiCVarChange Change, TArray<int32> Passes, float Saving, float Risk,
			TArray<TPair<FString, FString>> Requires = {})
		{
			FOptiAction Action;
			Action.Id = Id;
			Action.Sector = Sector;
			Action.Changes.Add(MoveTemp(Change));
			Action.Passes = MoveTemp(Passes);
			Action.TypicalSaving = Saving;
			Action.VisualRisk = Risk;
			Action.Requires = MoveTemp(Requires);
			return Action;
		}

		FOptiAction MakeScene(const TCHAR* Id, EOptiSector Sector, TArray<FOptiSceneRule> Rules, TArray<int32> Passes, float Saving, float Risk)
		{
			FOptiAction Action;
			Action.Id = Id;
			Action.Sector = Sector;
			Action.SceneRules = MoveTemp(Rules);
			Action.Passes = MoveTemp(Passes);
			Action.TypicalSaving = Saving;
			Action.VisualRisk = Risk;
			return Action;
		}

		template <typename TComponent>
		FOptiSceneRule Rule(FName Property, TFunction<bool(const TComponent&)> Filter, FString Value)
		{
			FOptiSceneRule SceneRule;
			SceneRule.ComponentClass = TComponent::StaticClass();
			SceneRule.Property = Property;
			SceneRule.Filter = [Filter](const UActorComponent& Component) { return Filter(*CastChecked<const TComponent>(&Component)); };
			SceneRule.NewValue = [Value](const UActorComponent&) { return Value; };
			return SceneRule;
		}

		bool IsMovable(const USceneComponent& Component)
		{
			return Component.Mobility != EComponentMobility::Static;
		}

		/** Single-mesh components only: instanced components cover many objects with one bound. */
		bool IsSingleMesh(const UPrimitiveComponent& Primitive)
		{
			return !Primitive.IsA<UInstancedStaticMeshComponent>();
		}

		void AddCVarActions(TArray<FOptiAction>& Catalog)
		{
			using namespace EOptiGlom;
			const TPair<FString, FString> VsmOn(TEXT("r.Shadow.Virtual.Enable"), TEXT("1"));
			const TPair<FString, FString> VsmOff(TEXT("r.Shadow.Virtual.Enable"), TEXT("0"));
			const TArray<int32> Pixels = { PassBasePass, PassDirectLighting, PassLumenGI, PassLumenReflections, PassReflections,
				PassTranslucency, PassPostProcess, PassUpscale, PassShadowProjection, PassClouds, PassFog };
			const TArray<int32> Meshes = { PassBasePass, PassPrepass, PassShadowDepths };
			const TArray<int32> Lights = { PassDirectLighting, PassShadowDepths, PassShadowProjection };

			// Shadows
			Catalog.Add(Make(TEXT("shadow.csm_resolution"), EOptiSector::Shadows, Mul(TEXT("r.Shadow.MaxCSMResolution"), 0.5, 512), { PassShadowDepths, PassShadowProjection }, 0.3f, 0.35f, { VsmOff }));
			Catalog.Add(Make(TEXT("shadow.csm_cascades"), EOptiSector::Shadows, Add(TEXT("r.Shadow.CSM.MaxCascades"), -1, 1), { PassShadowDepths }, 0.25f, 0.3f, { VsmOff }));
			Catalog.Add(Make(TEXT("shadow.distance"), EOptiSector::Shadows, Mul(TEXT("r.Shadow.DistanceScale"), 0.75, 0.3), { PassShadowDepths, PassShadowProjection }, 0.2f, 0.3f));
			Catalog.Add(Make(TEXT("shadow.local_resolution"), EOptiSector::Shadows, Mul(TEXT("r.Shadow.MaxResolution"), 0.5, 256), { PassShadowDepths, PassShadowProjection }, 0.2f, 0.3f, { VsmOff }));
			Catalog.Add(Make(TEXT("shadow.radius_threshold"), EOptiSector::Shadows, Mul(TEXT("r.Shadow.RadiusThreshold"), 2.0, 0.0, 0.1), { PassShadowDepths }, 0.15f, 0.15f));
			Catalog.Add(Make(TEXT("shadow.vsm_directional_bias"), EOptiSector::Shadows, Add(TEXT("r.Shadow.Virtual.ResolutionLodBiasDirectional"), 1, -DBL_MAX, 3), { PassShadowDepths, PassShadowProjection }, 0.25f, 0.3f, { VsmOn }));
			Catalog.Add(Make(TEXT("shadow.vsm_local_bias"), EOptiSector::Shadows, Add(TEXT("r.Shadow.Virtual.ResolutionLodBiasLocal"), 1, -DBL_MAX, 3), { PassShadowDepths, PassShadowProjection }, 0.2f, 0.25f, { VsmOn }));
			Catalog.Add(Make(TEXT("shadow.vsm_rays_directional"), EOptiSector::Shadows, Mul(TEXT("r.Shadow.Virtual.SMRT.RayCountDirectional"), 0.5, 1), { PassShadowProjection }, 0.3f, 0.25f, { VsmOn }));
			Catalog.Add(Make(TEXT("shadow.vsm_samples_directional"), EOptiSector::Shadows, Mul(TEXT("r.Shadow.Virtual.SMRT.SamplesPerRayDirectional"), 0.5, 1), { PassShadowProjection }, 0.2f, 0.2f, { VsmOn }));
			Catalog.Add(Make(TEXT("shadow.vsm_rays_local"), EOptiSector::Shadows, Mul(TEXT("r.Shadow.Virtual.SMRT.RayCountLocal"), 0.5, 1), { PassShadowProjection }, 0.3f, 0.25f, { VsmOn }));
			Catalog.Add(Make(TEXT("shadow.distance_field"), EOptiSector::Shadows, Off(TEXT("r.DistanceFieldShadowing")), { PassShadowProjection }, 0.4f, 0.35f));
			Catalog.Add(Make(TEXT("shadow.contact"), EOptiSector::Shadows, Off(TEXT("r.ContactShadows")), { PassShadowProjection, PassDirectLighting }, 0.15f, 0.15f));
			Catalog.Add(Make(TEXT("shadow.capsule"), EOptiSector::Shadows, Off(TEXT("r.CapsuleShadows")), { PassShadowProjection }, 0.3f, 0.3f));
			// Lighting
			Catalog.Add(Make(TEXT("lighting.lumen_probe_downsample"), EOptiSector::Lighting, Mul(TEXT("r.Lumen.ScreenProbeGather.DownsampleFactor"), 2.0, 1, 64), { PassLumenGI }, 0.35f, 0.3f));
			Catalog.Add(Make(TEXT("lighting.lumen_octahedron"), EOptiSector::Lighting, SetIfAbove(TEXT("r.Lumen.ScreenProbeGather.TracingOctahedronResolution"), 6, 6), { PassLumenGI }, 0.2f, 0.25f));
			Catalog.Add(Make(TEXT("lighting.lumen_radiosity_spacing"), EOptiSector::Lighting, Mul(TEXT("r.LumenScene.Radiosity.ProbeSpacing"), 2.0, 1, 16), { PassLumenScene }, 0.3f, 0.2f));
			Catalog.Add(Make(TEXT("lighting.lumen_software_tracing"), EOptiSector::Lighting, Off(TEXT("r.Lumen.HardwareRayTracing")), { PassRayTracing, PassLumenGI, PassLumenReflections, PassLumenScene }, 0.3f, 0.35f));
			Catalog.Add(Make(TEXT("lighting.lumen_short_range_ao"), EOptiSector::Lighting, Off(TEXT("r.Lumen.ScreenProbeGather.ShortRangeAO")), { PassLumenGI }, 0.15f, 0.25f));
			Catalog.Add(Make(TEXT("lighting.lumen_mesh_sdf"), EOptiSector::Lighting, Off(TEXT("r.Lumen.TraceMeshSDFs")), { PassLumenScene, PassLumenGI, PassLumenReflections }, 0.25f, 0.35f));
			Catalog.Add(Make(TEXT("lighting.lumen_screen_traces"), EOptiSector::Lighting, Off(TEXT("r.Lumen.ScreenProbeGather.ScreenTraces")), { PassLumenGI }, 0.15f, 0.3f));
			Catalog.Add(Make(TEXT("lighting.distance_field_ao"), EOptiSector::Lighting, Off(TEXT("r.DistanceFieldAO")), { PassOther, PassLumenGI }, 0.2f, 0.3f));
			Catalog.Add(Make(TEXT("lighting.ssao_quality"), EOptiSector::Lighting, Mul(TEXT("r.AmbientOcclusionMaxQuality"), 0.5, 0), { PassOther }, 0.2f, 0.25f));
			Catalog.Add(Make(TEXT("lighting.light_functions"), EOptiSector::Lighting, Add(TEXT("r.LightFunctionQuality"), -1, 0), { PassDirectLighting }, 0.2f, 0.2f));
			Catalog.Add(Make(TEXT("lighting.particle_lights"), EOptiSector::Lighting, Add(TEXT("r.ParticleLightQuality"), -1, 0), { PassFX, PassDirectLighting }, 0.2f, 0.2f));
			Catalog.Add(Make(TEXT("lighting.local_light_distance"), EOptiSector::Lighting, Mul(TEXT("r.LightMaxDrawDistanceScale"), 0.7, 0.3), Lights, 0.2f, 0.3f));
			// Reflections
			Catalog.Add(Make(TEXT("reflections.lumen_downsample"), EOptiSector::Reflections, Mul(TEXT("r.Lumen.Reflections.DownsampleFactor"), 2.0, 1, 4), { PassLumenReflections }, 0.4f, 0.35f));
			Catalog.Add(Make(TEXT("reflections.lumen_max_roughness"), EOptiSector::Reflections, WithDefault(Mul(TEXT("r.Lumen.Reflections.MaxRoughnessToTrace"), 0.75, 0.2), 0.3), { PassLumenReflections }, 0.25f, 0.3f));
			Catalog.Add(Make(TEXT("reflections.lumen_screen_traces"), EOptiSector::Reflections, Off(TEXT("r.Lumen.Reflections.ScreenTraces")), { PassLumenReflections }, 0.15f, 0.3f));
			Catalog.Add(Make(TEXT("reflections.ssr_quality"), EOptiSector::Reflections, Add(TEXT("r.SSR.Quality"), -1, 1), { PassReflections }, 0.3f, 0.3f));
			Catalog.Add(Make(TEXT("reflections.ssr_roughness"), EOptiSector::Reflections, WithDefault(Mul(TEXT("r.SSR.MaxRoughness"), 0.7, 0.1), 0.4), { PassReflections }, 0.2f, 0.25f));
			Catalog.Add(Make(TEXT("reflections.ssgi_quality"), EOptiSector::Reflections, Add(TEXT("r.SSGI.Quality"), -1, 1), { PassOther, PassLumenGI }, 0.3f, 0.3f));
			// Effects
			Catalog.Add(Make(TEXT("effects.translucency_volume"), EOptiSector::Effects, Off(TEXT("r.TranslucencyLightingVolume")), { PassTranslucency }, 0.3f, 0.3f));
			Catalog.Add(Make(TEXT("effects.translucency_resolution"), EOptiSector::Effects, SetIfAbove(TEXT("r.SeparateTranslucencyScreenPercentage"), 50, 50), { PassTranslucency, PassFX }, 0.35f, 0.3f));
			Catalog.Add(Make(TEXT("effects.niagara_quality"), EOptiSector::Effects, Add(TEXT("fx.Niagara.QualityLevel"), -1, 0), { PassFX, PassTranslucency }, 0.3f, 0.4f));
			Catalog.Add(Make(TEXT("effects.refraction"), EOptiSector::Effects, Add(TEXT("r.RefractionQuality"), -1, 0), { PassTranslucency }, 0.2f, 0.25f));
			// Post process
			Catalog.Add(Make(TEXT("post.bloom"), EOptiSector::PostProcess, Add(TEXT("r.BloomQuality"), -2, 1), { PassPostProcess }, 0.2f, 0.15f));
			Catalog.Add(Make(TEXT("post.motion_blur"), EOptiSector::PostProcess, Add(TEXT("r.MotionBlurQuality"), -2, 1), { PassMotionBlur }, 0.4f, 0.15f));
			Catalog.Add(Make(TEXT("post.depth_of_field"), EOptiSector::PostProcess, Add(TEXT("r.DepthOfFieldQuality"), -1, 1), { PassDepthOfField }, 0.35f, 0.2f));
			Catalog.Add(Make(TEXT("post.dof_rings"), EOptiSector::PostProcess, Add(TEXT("r.DOF.Gather.RingCount"), -1, 3), { PassDepthOfField }, 0.2f, 0.2f));
			Catalog.Add(Make(TEXT("post.tonemapper"), EOptiSector::PostProcess, SetIfAbove(TEXT("r.Tonemapper.Quality"), 2, 2), { PassPostProcess }, 0.1f, 0.1f));
			Catalog.Add(Make(TEXT("post.lens_flare"), EOptiSector::PostProcess, Off(TEXT("r.LensFlareQuality")), { PassPostProcess }, 0.1f, 0.1f));
			Catalog.Add(Make(TEXT("post.color_fringe"), EOptiSector::PostProcess, Off(TEXT("r.SceneColorFringeQuality")), { PassPostProcess }, 0.05f, 0.05f));
			Catalog.Add(Make(TEXT("post.eye_adaptation"), EOptiSector::PostProcess, Add(TEXT("r.EyeAdaptationQuality"), -1, 1), { PassPostProcess }, 0.1f, 0.2f));
			Catalog.Add(Make(TEXT("post.tsr_history"), EOptiSector::PostProcess, SetIfAbove(TEXT("r.TSR.History.ScreenPercentage"), 100, 100), { PassUpscale }, 0.3f, 0.3f));
			Catalog.Add(Make(TEXT("post.tsr_rejection"), EOptiSector::PostProcess, Add(TEXT("r.TSR.RejectionAntiAliasingQuality"), -1, 0), { PassUpscale }, 0.15f, 0.2f));
			Catalog.Add(Make(TEXT("post.screen_percentage"), EOptiSector::PostProcess, WithDefault(Mul(TEXT("r.ScreenPercentage"), 0.85, 50), 85), Pixels, 0.2f, 0.3f));
			Catalog.Last().bMeasurableInEditor = false; // editor viewports use their own screen percentage
			// Atmosphere
			Catalog.Add(Make(TEXT("atmosphere.cloud_samples"), EOptiSector::Atmosphere, Mul(TEXT("r.VolumetricCloud.ViewRaySampleMaxCount"), 0.5, 64), { PassClouds }, 0.3f, 0.3f));
			Catalog.Add(Make(TEXT("atmosphere.cloud_reflection_samples"), EOptiSector::Atmosphere, Mul(TEXT("r.VolumetricCloud.ReflectionRaySampleMaxCount"), 0.5, 16), { PassClouds, PassSky }, 0.2f, 0.1f));
			Catalog.Add(Make(TEXT("atmosphere.cloud_shadow_samples"), EOptiSector::Atmosphere, Mul(TEXT("r.VolumetricCloud.ShadowMap.RaySampleMaxCount"), 0.5, 4), { PassClouds }, 0.15f, 0.15f));
			Catalog.Add(Make(TEXT("atmosphere.cloud_sky_ao"), EOptiSector::Atmosphere, Off(TEXT("r.VolumetricCloud.SkyAO")), { PassClouds }, 0.15f, 0.2f));
			Catalog.Add(Make(TEXT("atmosphere.fog_grid"), EOptiSector::Atmosphere, Mul(TEXT("r.VolumetricFog.GridPixelSize"), 2.0, 1, 32), { PassFog }, 0.4f, 0.25f));
			Catalog.Add(Make(TEXT("atmosphere.fog_depth_slices"), EOptiSector::Atmosphere, Mul(TEXT("r.VolumetricFog.GridSizeZ"), 0.5, 16), { PassFog }, 0.35f, 0.25f));
			Catalog.Add(Make(TEXT("atmosphere.fog_history_samples"), EOptiSector::Atmosphere, Mul(TEXT("r.VolumetricFog.HistoryMissSupersampleCount"), 0.5, 1), { PassFog }, 0.1f, 0.15f));
			Catalog.Add(Make(TEXT("atmosphere.sky_samples"), EOptiSector::Atmosphere, Mul(TEXT("r.SkyAtmosphere.SampleCountMax"), 0.5, 2), { PassSky }, 0.2f, 0.2f));
			// Geometry
			Catalog.Add(Make(TEXT("geometry.lod_distance"), EOptiSector::Geometry, Mul(TEXT("r.StaticMeshLODDistanceScale"), 1.25, 0, 3), Meshes, 0.1f, 0.2f));
			Catalog.Add(Make(TEXT("geometry.view_distance"), EOptiSector::Geometry, Mul(TEXT("r.ViewDistanceScale"), 0.8, 0.4), Meshes, 0.1f, 0.3f));
			Catalog.Add(Make(TEXT("geometry.nanite_pixels_per_edge"), EOptiSector::Geometry, Mul(TEXT("r.Nanite.MaxPixelsPerEdge"), 2.0, 0, 4), { PassNanite, PassShadowDepths }, 0.25f, 0.2f));
			Catalog.Add(Make(TEXT("geometry.foliage_lod"), EOptiSector::Geometry, Mul(TEXT("foliage.LODDistanceScale"), 0.8, 0.4), Meshes, 0.1f, 0.25f));
			Catalog.Add(Make(TEXT("geometry.foliage_density"), EOptiSector::Geometry, Mul(TEXT("foliage.DensityScale"), 0.8, 0.3), Meshes, 0.1f, 0.35f));
			Catalog.Add(Make(TEXT("geometry.grass_density"), EOptiSector::Geometry, Mul(TEXT("grass.DensityScale"), 0.75, 0.3), Meshes, 0.15f, 0.3f));
			Catalog.Add(Make(TEXT("geometry.grass_distance"), EOptiSector::Geometry, Mul(TEXT("grass.CullDistanceScale"), 0.75, 0.3), Meshes, 0.15f, 0.3f));
			Catalog.Add(Make(TEXT("geometry.anisotropy"), EOptiSector::Geometry, SetIfAbove(TEXT("r.MaxAnisotropy"), 4, 4), { PassBasePass }, 0.1f, 0.2f));
			Catalog.Add(Make(TEXT("geometry.texture_mip_bias"), EOptiSector::Geometry, Add(TEXT("r.Streaming.MipBias"), 1, -DBL_MAX, 2), { PassBasePass }, 0.1f, 0.35f));
			// CPU
			Catalog.Add(Make(TEXT("cpu.skeletal_lod_bias"), EOptiSector::CPU, Add(TEXT("r.SkeletalMeshLODBias"), 1, -DBL_MAX, 2), {}, 0.05f, 0.3f));
			Catalog.Add(Make(TEXT("cpu.occlusion_queries"), EOptiSector::CPU, Off(TEXT("r.AllowOcclusionQueries")), {}, 0.1f, 0.05f));
			// Blueprints, in Play: tick a costly class 10 times per second instead of every frame. The risk is behaviour
			// (smoothness, timing), not image, so it is only ever suggested, never applied behind your back.
			{
				FOptiAction Tick;
				Tick.Id = TEXT("blueprint.tick_interval");
				Tick.Sector = EOptiSector::CPU;
				Tick.TypicalSaving = 0.45f;
				Tick.VisualRisk = 0.25f;
				Tick.bPlayOnly = true;
				Tick.bMeasurableInEditor = false;
				Catalog.Add(MoveTemp(Tick));
			}
		}

		void AddSceneActions(TArray<FOptiAction>& Catalog)
		{
			using namespace EOptiGlom;
			const FString False = TEXT("False");
			const TArray<int32> Lights = { PassDirectLighting, PassShadowDepths, PassShadowProjection };
			const TArray<int32> Meshes = { PassBasePass, PassPrepass, PassShadowDepths, PassNanite };

			// Tiny objects rarely need a shadow, but every caster costs a draw in every shadow view.
			Catalog.Add(MakeScene(TEXT("scene.tiny_objects_no_shadow"), EOptiSector::Shadows,
				{ Rule<UPrimitiveComponent>(TEXT("CastShadow"), [](const UPrimitiveComponent& C) { return C.CastShadow && IsSingleMesh(C) && C.Bounds.SphereRadius < 40.f; }, False) },
				{ PassShadowDepths }, 0.25f, 0.15f));
			Catalog.Add(MakeScene(TEXT("scene.far_shadow_off"), EOptiSector::Shadows,
				{ Rule<UPrimitiveComponent>(TEXT("bCastFarShadow"), [](const UPrimitiveComponent& C) { return C.bCastFarShadow != 0; }, False) },
				{ PassShadowDepths }, 0.15f, 0.15f));
			// Small local lights with dynamic shadows are a classic hidden cost.
			Catalog.Add(MakeScene(TEXT("scene.small_lights_no_shadow"), EOptiSector::Shadows,
				{ Rule<ULocalLightComponent>(TEXT("CastShadows"), [](const ULocalLightComponent& C) { return C.CastShadows && IsMovable(C) && C.AttenuationRadius < 600.f; }, False) },
				{ PassShadowDepths, PassShadowProjection }, 0.35f, 0.25f));
			Catalog.Add(MakeScene(TEXT("scene.lights_translucent_shadows"), EOptiSector::Shadows,
				{ Rule<ULightComponent>(TEXT("CastTranslucentShadows"), [](const ULightComponent& C) { return C.CastTranslucentShadows != 0; }, False) },
				{ PassShadowDepths, PassTranslucency }, 0.2f, 0.2f));
			Catalog.Add(MakeScene(TEXT("scene.lights_contact_shadows"), EOptiSector::Shadows,
				{ Rule<ULightComponent>(TEXT("ContactShadowLength"), [](const ULightComponent& C) { return C.ContactShadowLength > 0.f; }, TEXT("0.0")) },
				{ PassShadowProjection, PassDirectLighting }, 0.2f, 0.15f));
			// Huge light radii light (and shadow) far more pixels and meshes than they visibly affect.
			{
				FOptiSceneRule Radius = Rule<ULocalLightComponent>(TEXT("AttenuationRadius"), [](const ULocalLightComponent& C) { return IsMovable(C) && C.AttenuationRadius > 2500.f; }, FString());
				Radius.NewValue = [](const UActorComponent& C) { return FString::SanitizeFloat(CastChecked<ULocalLightComponent>(&C)->AttenuationRadius * 0.6f); };
				Catalog.Add(MakeScene(TEXT("scene.light_radius"), EOptiSector::Lighting, { Radius }, Lights, 0.25f, 0.3f));
			}
			Catalog.Add(MakeScene(TEXT("scene.light_draw_distance"), EOptiSector::Lighting,
				{ Rule<ULocalLightComponent>(TEXT("MaxDrawDistance"), [](const ULocalLightComponent& C) { return IsMovable(C) && C.MaxDrawDistance <= 0.f; }, TEXT("6000.0")),
				  Rule<ULocalLightComponent>(TEXT("MaxDistanceFadeRange"), [](const ULocalLightComponent& C) { return IsMovable(C) && C.MaxDrawDistance <= 0.f; }, TEXT("1000.0")) },
				Lights, 0.3f, 0.2f));
			Catalog.Add(MakeScene(TEXT("scene.lights_volumetric_shadow"), EOptiSector::Atmosphere,
				{ Rule<ULocalLightComponent>(TEXT("bCastVolumetricShadow"), [](const ULocalLightComponent& C) { return C.bCastVolumetricShadow && C.VolumetricScatteringIntensity > 0.f; }, False) },
				{ PassFog }, 0.35f, 0.2f));
			Catalog.Add(MakeScene(TEXT("scene.lights_translucent_lighting"), EOptiSector::Effects,
				{ Rule<ULocalLightComponent>(TEXT("bAffectTranslucentLighting"), [](const ULocalLightComponent& C) { return C.bAffectTranslucentLighting != 0; }, False) },
				{ PassTranslucency }, 0.3f, 0.2f));
			Catalog.Add(MakeScene(TEXT("scene.skylight_realtime_capture"), EOptiSector::Atmosphere,
				{ Rule<USkyLightComponent>(TEXT("bRealTimeCapture"), [](const USkyLightComponent& C) { return C.bRealTimeCapture; }, False) },
				{ PassSky }, 0.4f, 0.3f));
			// Small props drawn all the way to the horizon.
			Catalog.Add(MakeScene(TEXT("scene.cull_small_objects"), EOptiSector::Geometry,
				{ Rule<UStaticMeshComponent>(TEXT("LDMaxDrawDistance"), [](const UStaticMeshComponent& C) { return IsSingleMesh(C) && C.Bounds.SphereRadius < 150.f && C.LDMaxDrawDistance <= 0.f; }, TEXT("8000.0")) },
				Meshes, 0.15f, 0.2f));
			// Dense meshes that already have LODs but are forced to the full one.
			Catalog.Add(MakeScene(TEXT("scene.dense_meshes_lod1"), EOptiSector::Geometry,
				{ Rule<UStaticMeshComponent>(TEXT("ForcedLodModel"), [](const UStaticMeshComponent& C)
				{
					const UStaticMesh* Mesh = C.GetStaticMesh();
					return IsSingleMesh(C) && C.ForcedLodModel == 0 && Mesh && !Mesh->IsNaniteEnabled() && Mesh->GetNumLODs() >= 2 && Mesh->GetNumTriangles(0) > 50000;
				}, TEXT("2")) },
				Meshes, 0.3f, 0.35f));
		}

		TArray<FOptiAction> BuildCatalog()
		{
			TArray<FOptiAction> Catalog;
			AddCVarActions(Catalog);
			AddSceneActions(Catalog);
			return Catalog;
		}
	}

	const TArray<FOptiAction>& Catalog()
	{
		static const TArray<FOptiAction> Actions = BuildCatalog();
		return Actions;
	}

	const FOptiAction* Find(FName Id)
	{
		return Catalog().FindByPredicate([Id](const FOptiAction& Action) { return Action.Id == Id; });
	}

	int32 IndexOf(FName Id)
	{
		return Catalog().IndexOfByPredicate([Id](const FOptiAction& Action) { return Action.Id == Id; });
	}

	void LogCatalog(UWorld* World)
	{
		int32 Ready = 0, Missing = 0;
		for (const FOptiAction& Action : Catalog())
		{
			FString Status;
			for (const FOptiCVarChange& Change : Action.Changes)
			{
				FString RealName;
				IConsoleVariable* CVar = OptiFindCVar(Change.Name, &RealName);
				Status += CVar ? FString::Printf(TEXT("%s=%s "), *RealName, *CVar->GetString()) : FString::Printf(TEXT("MISSING %s "), *Change.Name);
				Missing += CVar ? 0 : 1;
			}
			for (const FOptiSceneRule& SceneRule : Action.SceneRules)
			{
				const bool bHasProperty = FindFProperty<FProperty>(SceneRule.ComponentClass, SceneRule.Property) != nullptr;
				Status += FString::Printf(TEXT("%s.%s%s "), *SceneRule.ComponentClass->GetName(), *SceneRule.Property.ToString(), bHasProperty ? TEXT("") : TEXT(" MISSING"));
				Missing += bHasProperty ? 0 : 1;
			}
			if (Action.IsSceneAction() && World)
			{
				Status += FString::Printf(TEXT("(%d components here) "), OptiScene::Collect(Action, World).Num());
			}
			FOptiCVarSet A, B;
			const bool bCanRun = Action.BuildVariants(A, B, World);
			Ready += bCanRun ? 1 : 0;
			UE_LOG(LogOptiCompanion, Display, TEXT("%s %-40s %s"), bCanRun ? TEXT("[ready]") : TEXT("[ -- ]"), *Action.Id.ToString(), *Status);
		}
		UE_LOG(LogOptiCompanion, Display, TEXT("Catalog: %d actions, %d can run right now, %d missing CVars or properties."), Catalog().Num(), Ready, Missing);
	}
}
