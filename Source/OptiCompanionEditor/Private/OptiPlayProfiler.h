#pragma once

#include "CoreMinimal.h"
#include "OptiProbe.h"
#include "Async/Future.h"

class AActor;
class UWorld;

/** What one Blueprint class cost on the game thread while you played. */
struct FOptiBlueprintCost
{
	FString ClassName;     // BP_Foo_C
	FString BlueprintPath; // /Game/Foo/BP_Foo.BP_Foo
	int32 Instances = 0;   // instances that ticked
	double MsPerFrame = 0.0;     // average over the captures
	double PeakMsPerFrame = 0.0; // worst capture
	float TickInterval = 0.f;    // what the instances used (0 = every frame)
	int32 Captures = 0;
};

/** Result of trying a longer Tick Interval on one Blueprint class, in Play. */
struct FOptiTickIntervalResult
{
	FOptiBlueprintCost Cost;
	float From = 0.f;
	float To = 0.f;
	FOptiStatResult Stat; // game-thread milliseconds, A = From, B = To
};

/**
 * Blueprint costs only exist while the game runs, where the camera never stays still, so the editor-side
 * A/B experiments cannot see them. In Play (and Simulate) this records a few seconds of the engine's own
 * CPU trace (the one Unreal Insights reads), where every actor and component Tick is a named scope,
 * analyses it in the background and adds it up per Blueprint class. For the most expensive class it then
 * tries a longer Tick Interval, alternating blocks of frames, and measures the game thread only: moving
 * the camera barely changes that.
 */
class FOptiPlayProfiler
{
public:
	~FOptiPlayProfiler();

	void OnBeginPlay();
	void OnEndPlay();
	void Tick();

	bool IsPlaying() const { return State != EState::Off; }
	/** Short line for the mascot while it measures, empty otherwise. */
	FText GetStatus() const;
	/** Costs from the current or last Play session, most expensive first. */
	const TArray<FOptiBlueprintCost>& GetCosts() const { return Costs; }
	int32 GetCaptureCount() const { return CaptureCount; }

	/** Classes with an open finding are not tested again. */
	TFunction<bool(const FString& BlueprintPath)> IsAlreadySuggested;
	TFunction<void(const FOptiTickIntervalResult&)> OnTickIntervalResult;
	TFunction<void()> OnCostsUpdated;
	/** Picks which candidate to test (index into Candidates) or INDEX_NONE to test none. Default: the most expensive. */
	TFunction<int32(const TArray<FOptiBlueprintCost>& Candidates, UWorld* World)> ChooseCandidate;

	/** Game-thread frame time and world tick time per frame in the last capture, for the brain's smell. */
	double GetFrameMs() const { return LastFrameMs; }
	double GetWorldTickMs() const { return LastWorldTickMs; }

private:
	enum class EState : uint8 { Off, Waiting, Capturing, Analyzing, Experimenting };

	struct FActorInfo { FString ClassName; FString BlueprintPath; float TickInterval = 0.f; };
	struct FTimerTotal { FString Name; double TotalSeconds = 0.0; uint64 Count = 0; };
	struct FAnalysis { bool bOk = false; FString Error; uint64 Frames = 0; TArray<FTimerTotal> Timers; };
	double LastFrameMs = 0.0;
	double LastWorldTickMs = 0.0;

	UWorld* PlayWorld() const;
	void StartCapture();
	void StopCapture();
	void SnapshotNames();
	void FinishAnalysis(const FAnalysis& Analysis);
	void StartExperiment();
	void TickExperiment();
	void SetVariant(bool bVariantB);
	void EndExperiment(bool bCompleted);

	EState State = EState::Off;
	double NextActionTime = 0.0;
	double CaptureEnd = 0.0;
	double CaptureStart = 0.0;
	/** Analysis modules turned off while we analyse (memory, networking...), turned back on afterwards. */
	TArray<FName> ModulesTurnedOff;
	FString TracePath;
	TFuture<FAnalysis> PendingAnalysis;
	/** Scope name (actor or component FName) -> the Blueprint class it belongs to. */
	TMap<FString, FActorInfo> NameToClass;
	TSet<FString> AmbiguousNames;
	TArray<FOptiBlueprintCost> Costs;
	int32 CaptureCount = 0;
	TSet<FString> Tested;

	// Tick Interval experiment.
	FOptiBlueprintCost Candidate;
	TArray<TWeakObjectPtr<AActor>> Subjects;
	TArray<float> OriginalIntervals;
	TArray<double> BlockMeansA, BlockMeansB;
	TArray<double> BlockFrames;
	int32 Block = 0;
	int32 FrameInBlock = 0;
	// Actor tick phase of the play world, timed from its own tick delegates: exactly what a Tick Interval changes.
	FDelegateHandle WorldTickStartHandle;
	FDelegateHandle WorldPostActorTickHandle;
	double WorldTickStartedAt = 0.0;
	double LastActorTickMs = -1.0;
	uint64 LastActorTickFrame = 0;
	uint64 LastReadFrame = 0;
	static constexpr float TriedInterval = 0.1f;
};
