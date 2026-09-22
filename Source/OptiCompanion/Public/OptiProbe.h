#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Async/Future.h"

OPTICOMPANION_API DECLARE_LOG_CATEGORY_EXTERN(LogOptiCompanion, Log, All);

class FViewport;

/** Console variable values applied for one variant of a probe. */
using FOptiCVarSet = TArray<TPair<FString, FString>>;

/** What to compare. With no changes on either side it is an A/A test that measures machine noise. */
struct OPTICOMPANION_API FOptiProbeSettings
{
	FOptiCVarSet ChangesA;
	FOptiCVarSet ChangesB;
	int32 BlocksPerVariant = 8;
	int32 FramesPerBlock = 60;
	int32 WarmupFrames = 15;
	FString Label;

	/** Grab one frame of each variant to measure how visible the change is. */
	bool bCaptureImages = false;
	/** Viewport to read back when capturing. Defaults to the game viewport. */
	TFunction<FViewport*()> ViewportProvider;
	/** Called with true when B is applied and false when A is (and when everything is restored). Used for scene changes. */
	TFunction<void(bool bVariantB)> OnVariant;
	/** Keep the CSV and JSON report in Saved/OptiCompanion. Short background probes delete them. */
	bool bKeepFiles = true;

	bool IsNoiseTest() const { return ChangesA.IsEmpty() && ChangesB.IsEmpty() && !OnVariant; }

	/** Convenience for the common "one CVar, two values" case. */
	static FOptiProbeSettings ForCVar(const FString& Name, const FString& ValueA, const FString& ValueB);
};

/** Result for one CSV column (a GPU pass, frame time, game thread...). */
struct OPTICOMPANION_API FOptiStatResult
{
	FString Name;
	double MeanA = 0.0;
	double MeanB = 0.0;
	double Delta = 0.0;        // B - A
	double CILow = 0.0;        // 95% bootstrap confidence interval of Delta
	double CIHigh = 0.0;
	double NoiseStdDev = 0.0;  // std dev of the A block means
	bool bSignificant = false; // the interval does not contain zero

	double RelativeDelta() const { return MeanA > UE_KINDA_SMALL_NUMBER ? Delta / MeanA : 0.0; }
};

struct OPTICOMPANION_API FOptiCapture
{
	int32 Width = 0;
	int32 Height = 0;
	TArray<FColor> Pixels;

	bool IsValid() const { return Width > 0 && Height > 0 && Pixels.Num() == Width * Height; }
};

struct OPTICOMPANION_API FOptiProbeResult
{
	FOptiProbeSettings Settings;
	FString CsvPath;
	FString ReportPath;
	int32 Seed = 0;
	int32 ValidBlocksA = 0;
	int32 ValidBlocksB = 0;
	TArray<FOptiStatResult> Stats;
	FOptiCapture CaptureA;
	FOptiCapture CaptureB;
	/** A second capture of A: the A/A difference is the noise floor (TAA/TSR jitter, animated sky...). */
	FOptiCapture CaptureA2;
	FString Error;
	bool bCancelled = false;

	bool IsValid() const { return Error.IsEmpty() && !bCancelled; }
	const FOptiStatResult* FindStat(const FString& Name) const;
};

DECLARE_DELEGATE_OneParam(FOnOptiProbeFinished, const FOptiProbeResult&);

/** Reads the viewport back and box-filters it to a small image (about 320 px wide). Stalls the GPU once. */
OPTICOMPANION_API bool OptiCaptureViewport(FViewport* Viewport, FOptiCapture& Out);

/**
 * The fly's A/B test bench.
 *
 * Alternates A and B in randomly interleaved blocks (AB or BA per pair) so thermal drift and
 * background processes hit both variants equally, records a CSV with per-pass GPU timings and
 * compares block means with a bootstrap interval. It never writes to the project: it only
 * changes console variables in memory and restores them when it finishes.
 */
class OPTICOMPANION_API FOptiProbe : public TSharedFromThis<FOptiProbe>
{
public:
	explicit FOptiProbe(const FOptiProbeSettings& InSettings);
	~FOptiProbe();

	bool Start(FString& OutError);
	void Cancel();
	bool IsRunning() const { return State != EState::Idle && State != EState::Done; }

	/**
	 * Interrupts the probe (camera moved, compile started...). The block in progress is thrown away, A is
	 * restored and the capture keeps running; Resume() repeats that block. This lets one experiment be
	 * spread over many short windows.
	 */
	void Pause();
	void Resume();
	bool IsPaused() const { return State == EState::Paused; }
	/** Blocks completed so far and in total. */
	int32 CompletedBlocks() const { return BlockIndex; }
	int32 TotalBlocks() const { return Schedule.Num(); }

	FOnOptiProbeFinished OnFinished;

private:
	enum class EState : uint8 { Idle, WaitingForCapture, Warmup, Measuring, Paused, WaitingForFile, Done };

	bool Tick(float DeltaTime);
	void ApplyVariant(bool bVariantB);
	void OverrideCVar(const FString& Name, const FString& Value);
	void RestoreCVars();
	void CaptureViewport(FOptiCapture& Out);
	void Finish(FOptiProbeResult&& Result);

	FOptiProbeSettings Settings;
	TArray<bool> Schedule; // true = B
	int32 Seed = 0;
	int32 BlockIndex = 0;
	int32 FramesLeft = 0;
	int32 FramesWaited = 0;
	EState State = EState::Idle;
	FTSTicker::FDelegateHandle TickerHandle;
	TSharedFuture<FString> CsvFuture;
	FOptiCapture CaptureA;
	FOptiCapture CaptureB;
	FOptiCapture CaptureA2;

	struct FSavedCVar { FString Name; FString Value; };
	TArray<FSavedCVar> SavedCVars;
};
