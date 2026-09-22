#pragma once

#include "CoreMinimal.h"

struct FOptiProbeResult;
class UWorld;

/**
 * Antennal lobe. Each input the fly "smells" is one glomerulus, and there are exactly as many as the
 * real Drosophila antennal lobe has uniglomerular projection-neuron types (51), each named after its
 * real glomerulus. That lets the plugin wire the mushroom body with the real PN -> Kenyon cell
 * connectome when the data file is present.
 */
namespace EOptiGlom
{
	enum Type : int32
	{
		// Share of GPU time per pass group.
		PassShadowDepths, PassShadowProjection, PassDirectLighting, PassLumenScene, PassLumenGI,
		PassLumenReflections, PassReflections, PassTranslucency, PassFX, PassPostProcess,
		PassUpscale, PassMotionBlur, PassDepthOfField, PassBasePass, PassPrepass,
		PassNanite, PassClouds, PassFog, PassSky, PassRayTracing,
		PassOcclusion, PassOther,
		// Frame budget.
		GpuMs, FrameMs, GameThreadMs, RenderThreadMs, RhiThreadMs, DrawCalls, Primitives,
		GpuBound, GameThreadBound, RenderThreadBound,
		// What is in the level.
		SceneShadowLights, SceneTextureMemory, SceneStaticMeshes, SceneInstancedFraction, SceneNaniteFraction,
		SceneTranslucent, SceneNiagara, SceneMaterialCost, SceneTrianglesPerMesh, SceneFoliage,
		// Scalability groups.
		SgViewDistance, SgAntiAliasing, SgShadow, SgGlobalIllumination, SgReflection,
		SgPostProcess, SgTexture, SgEffects, SgFoliage,

		Count
	};

	constexpr int32 FirstPass = PassShadowDepths;
	constexpr int32 NumPasses = PassOther + 1;

	/** Real glomerulus each input is wired as. */
	OPTICOMPANION_API const TCHAR* RealName(int32 Glomerulus);
	OPTICOMPANION_API int32 FromRealName(const FString& Name);

	/** Human-readable name of an input, for logs and the notebook. */
	OPTICOMPANION_API const TCHAR* InputName(int32 Glomerulus);
}

/** Measured frame: per-column mean milliseconds from a short A/A capture. */
struct OPTICOMPANION_API FOptiProfile
{
	TMap<FString, double> Columns;
	double PassMs[EOptiGlom::NumPasses] = {};
	double GpuMs = 0.0;
	double FrameMs = 0.0;
	double GameThreadMs = 0.0;
	double RenderThreadMs = 0.0;
	double RhiThreadMs = 0.0;
	double DrawCalls = 0.0;
	double Primitives = 0.0;

	bool IsValid() const { return FrameMs > 0.0; }
	bool IsGpuBound() const { return GpuMs >= FMath::Max(GameThreadMs, RenderThreadMs) * 0.9; }

	/** Builds a profile from the A side of a probe. */
	static FOptiProfile FromProbe(const FOptiProbeResult& Probe);

	/** Maps a GPU/<pass> CSV column to one of the pass glomeruli. */
	static int32 PassGroupOf(const FString& Column);
};

struct OPTICOMPANION_API FOptiSceneScent
{
	int32 ShadowLights = 0;
	int32 Lights = 0;
	int32 StaticMeshes = 0;
	int32 Instances = 0;
	int32 NaniteMeshes = 0;
	int32 Translucent = 0;
	int32 Niagara = 0;
	int32 Skeletal = 0;
	/** Estimated memory of the textures used by what is in view (OptiLogger's model). */
	double TextureMB = 0.0;
	/** Average pixel-shader instructions of the materials in view, weighted by use. 0 when unknown. */
	double MaterialInstructions = 0.0;
	double TrianglesPerMesh = 0.0;
	int32 FoliageInstances = 0;

	/** What is in the level, or only what the camera sees when View is valid. */
	static FOptiSceneScent FromWorld(UWorld* World, const struct FOptiView& View);
};

struct OPTICOMPANION_API FOptiSmell
{
	FOptiProfile Profile;
	FOptiSceneScent Scene;
	int32 Scalability[9] = {};

	/** Receptor activity (ORNs), each 0..1. */
	TArray<float> Receptors;
	/** Projection-neuron output after divisive normalisation by lateral inhibition (Olsen et al. 2010). */
	TArray<float> Projection;

	/** Identifies the situation a measurement belongs to, so old findings can be detected as stale. */
	FString ContextKey;

	static FOptiSmell Sniff(const FOptiProfile& Profile, UWorld* World, const struct FOptiView& View);

	/** Same as Sniff with an already gathered scene scent (tests, or scents computed elsewhere). */
	static FOptiSmell FromParts(const FOptiProfile& Profile, const FOptiSceneScent& Scene, const FString& ContextKey);

	/** Map and scalability settings; cheap enough to check every few seconds. */
	static FString ContextKeyFor(UWorld* World);
};
