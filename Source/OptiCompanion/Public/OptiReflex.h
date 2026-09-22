#pragma once

#include "CoreMinimal.h"

/**
 * Giant fiber: the fly's escape reflex. No learning and no experiments, just a cheap per-frame monitor
 * that notices when saving an asset made the frame slower. It only judges when the camera did not move,
 * because otherwise the numbers change for reasons that have nothing to do with the save.
 */
class OPTICOMPANION_API FOptiReflex
{
public:
	struct FSample
	{
		float GpuMs = 0.f;
		float GameMs = 0.f;
		float RenderMs = 0.f;
		float FrameMs = 0.f;
		uint32 View = 0;
	};

	struct FResult
	{
		TArray<FString> Assets;
		FString Metric;
		double BeforeMs = 0.0;
		double AfterMs = 0.0;
		double NoiseMs = 0.0;
		double ThresholdMs = 0.0;
		bool bRegression = false;
		bool bImprovement = false;

		double DeltaMs() const { return AfterMs - BeforeMs; }
	};

	FOptiReflex();

	/** Call once per frame. View is a hash of the camera; 0 means unknown. */
	void Sample(uint32 View);

	/** A package was saved: remember the frames before it and start watching. */
	void OnAssetSaved(const FString& Asset) { OnChange(Asset); }

	/** Anything that may change the frame cost (a save, a property you edited, a setting): same before/after watch. */
	void OnChange(const FString& Label);

	/** Frames the camera has stayed exactly where it is now. */
	int32 StableFrames() const { return StableTail(600).Num(); }
	bool IsWatching() const { return bArmed; }

	/** Returns a verdict once enough frames were seen after the save. Busy = shaders or assets compiling. */
	TOptional<FResult> Poll(bool bBusy);

	/** Median frame time over the last N frames, for "remind me when the frame is slow". */
	double RecentFrameMs(int32 Frames = 120) const;
	const FSample& Last() const;

private:
	TArray<FSample> StableTail(int32 MaxFrames) const;

	TArray<FSample> Ring;
	int32 Head = 0;
	int32 Count = 0;

	bool bArmed = false;
	TArray<FString> PendingAssets;
	TArray<FSample> Before;
	TArray<FSample> After;
	int32 SettleFrames = 0;
};
