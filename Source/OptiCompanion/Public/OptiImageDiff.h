#pragma once

#include "CoreMinimal.h"

struct FOptiCapture;

/** Per-image perceptual difference between two captures of the same view. */
struct OPTICOMPANION_API FOptiImageDiff
{
	float Mean = 0.f;  // average per-pixel error, 0..1
	float P95 = 0.f;   // 95th percentile, catches small but visible regions
	int32 Width = 0;
	int32 Height = 0;
	TArray<float> ErrorMap;

	bool IsValid() const { return Width > 0 && ErrorMap.Num() == Width * Height; }
};

namespace OptiImage
{
	/**
	 * FLIP-inspired difference (Andersson et al. 2020), simplified to run on the CPU over small captures:
	 * opponent colour space, a contrast-sensitivity blur, HyAB colour distance and an edge term, combined
	 * per pixel as colour^(1 - edge). It is a screening metric, not a certified FLIP implementation.
	 */
	OPTICOMPANION_API FOptiImageDiff Compare(const FOptiCapture& A, const FOptiCapture& B);

	/** Turns an error map into a black-purple-orange-yellow heatmap. */
	OPTICOMPANION_API TArray<FColor> Heatmap(const FOptiImageDiff& Diff);

	/** PNG encoding for captures shown in the notebook. */
	OPTICOMPANION_API bool SavePng(const FString& Path, int32 Width, int32 Height, const TArray<FColor>& Pixels);
}
