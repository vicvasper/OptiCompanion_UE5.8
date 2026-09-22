#include "OptiReflex.h"
#include "DynamicRHI.h"
#include "RenderTimer.h"
#include "Misc/App.h"

namespace
{
	constexpr int32 RingSize = 600;
	constexpr int32 MinBaseline = 60;
	constexpr int32 AfterFrames = 120;
	constexpr int32 SettleAfterSave = 30;

	double Median(TArray<double> Values)
	{
		if (Values.IsEmpty())
		{
			return 0.0;
		}
		Values.Sort();
		const int32 Mid = Values.Num() / 2;
		return Values.Num() % 2 ? Values[Mid] : 0.5 * (Values[Mid - 1] + Values[Mid]);
	}

	/** Median absolute deviation scaled to a standard deviation. */
	double RobustNoise(const TArray<double>& Values)
	{
		const double M = Median(Values);
		TArray<double> Deviations;
		for (double V : Values)
		{
			Deviations.Add(FMath::Abs(V - M));
		}
		return 1.4826 * Median(Deviations);
	}
}

FOptiReflex::FOptiReflex()
{
	Ring.SetNum(RingSize);
}

void FOptiReflex::Sample(uint32 View)
{
	FSample Sample;
	Sample.GpuMs = static_cast<float>(FPlatformTime::ToMilliseconds(RHIGetGPUFrameCycles()));
	Sample.GameMs = static_cast<float>(FPlatformTime::ToMilliseconds(GGameThreadTime));
	Sample.RenderMs = static_cast<float>(FPlatformTime::ToMilliseconds(GRenderThreadTime));
	Sample.FrameMs = static_cast<float>(FApp::GetDeltaTime() * 1000.0);
	Sample.View = View;

	Ring[Head] = Sample;
	Head = (Head + 1) % RingSize;
	Count = FMath::Min(Count + 1, RingSize);

	if (bArmed && SettleFrames <= 0)
	{
		After.Add(Sample);
	}
}

const FOptiReflex::FSample& FOptiReflex::Last() const
{
	return Ring[(Head - 1 + RingSize) % RingSize];
}

TArray<FOptiReflex::FSample> FOptiReflex::StableTail(int32 MaxFrames) const
{
	TArray<FSample> Tail;
	if (Count == 0)
	{
		return Tail;
	}
	const uint32 View = Last().View;
	for (int32 Offset = 1; Offset <= FMath::Min(Count, MaxFrames); ++Offset)
	{
		const FSample& Sample = Ring[(Head - Offset + RingSize) % RingSize];
		if (Sample.View != View || View == 0)
		{
			break;
		}
		Tail.Add(Sample);
	}
	return Tail;
}

void FOptiReflex::OnChange(const FString& Asset)
{
	PendingAssets.AddUnique(Asset);
	if (bArmed)
	{
		// Several saves in a row (Save All) are judged together against the frames before the first one.
		After.Reset();
		SettleFrames = SettleAfterSave;
		return;
	}

	Before = StableTail(RingSize);
	if (Before.Num() < MinBaseline)
	{
		PendingAssets.Reset(); // the camera was moving; there is nothing reliable to compare against
		return;
	}
	bArmed = true;
	After.Reset();
	SettleFrames = SettleAfterSave;
}

TOptional<FOptiReflex::FResult> FOptiReflex::Poll(bool bBusy)
{
	if (!bArmed)
	{
		return {};
	}
	if (bBusy)
	{
		// Saving often triggers shader or asset compilation; wait for it and start counting again.
		After.Reset();
		SettleFrames = SettleAfterSave;
		return {};
	}
	if (SettleFrames > 0)
	{
		--SettleFrames;
		return {};
	}
	if (!After.IsEmpty() && After.Last().View != Before[0].View)
	{
		bArmed = false; // you moved the camera, so the comparison is no longer fair
		PendingAssets.Reset();
		return {};
	}
	if (After.Num() < AfterFrames)
	{
		return {};
	}

	auto Extract = [](const TArray<FSample>& Samples, bool bGpu)
	{
		TArray<double> Values;
		for (const FSample& Sample : Samples)
		{
			Values.Add(bGpu ? Sample.GpuMs : Sample.FrameMs);
		}
		return Values;
	};

	TArray<double> GpuBefore = Extract(Before, true);
	TArray<double> FrameBefore = Extract(Before, false);
	const bool bGpuBound = Median(GpuBefore) >= 0.9 * Median(FrameBefore);
	const TArray<double> ValuesBefore = bGpuBound ? GpuBefore : FrameBefore;
	const TArray<double> ValuesAfter = Extract(After, bGpuBound);

	FResult Result;
	Result.Assets = PendingAssets;
	Result.Metric = bGpuBound ? TEXT("GPUTime") : TEXT("FrameTime");
	Result.BeforeMs = Median(ValuesBefore);
	Result.AfterMs = Median(ValuesAfter);
	Result.NoiseMs = RobustNoise(ValuesBefore);
	Result.ThresholdMs = FMath::Max3(0.4, 0.05 * Result.BeforeMs, 3.0 * Result.NoiseMs);
	Result.bRegression = Result.DeltaMs() > Result.ThresholdMs;
	Result.bImprovement = -Result.DeltaMs() > Result.ThresholdMs;

	bArmed = false;
	PendingAssets.Reset();
	return Result;
}

double FOptiReflex::RecentFrameMs(int32 Frames) const
{
	TArray<double> Values;
	for (int32 Offset = 1; Offset <= FMath::Min(Count, Frames); ++Offset)
	{
		Values.Add(Ring[(Head - Offset + RingSize) % RingSize].FrameMs);
	}
	return Median(Values);
}
