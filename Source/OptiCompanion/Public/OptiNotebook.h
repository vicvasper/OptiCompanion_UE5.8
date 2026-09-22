#pragma once

#include "CoreMinimal.h"
#include "OptiProbe.h"

enum class EOptiFindingKind : uint8
{
	Optimization, // the fly measured a change that saves time
	Regression,   // the save reflex saw a saved asset make the frame slower
};

enum class EOptiFindingState : uint8
{
	New,
	Postponed,
	Applied,
	ResolvedByUser, // you made the change yourself
	Stale,          // the project changed since it was measured; it is re-measured before you can apply it
	Dismissed,
};

enum class EOptiReminder : uint8
{
	None,
	InTwoHours,
	NextSession,
	WhenFrameIsSlow, // frame time above 16.6 ms for a few seconds
};

struct OPTICOMPANION_API FOptiPassDelta
{
	FString Pass;
	double DeltaMs = 0.0;
};

struct OPTICOMPANION_API FOptiFinding
{
	FGuid Id;
	EOptiFindingKind Kind = EOptiFindingKind::Optimization;
	EOptiFindingState State = EOptiFindingState::New;

	FName ActionId;      // optimizations
	FString Asset;       // regressions: the package that was saved
	FOptiCVarSet From;
	FOptiCVarSet To;
	/** What DefaultEngine.ini had for each key before applying ("" = no entry), so Undo can restore it. */
	FOptiCVarSet IniBefore;
	/** Scene actions: every component property touched, serialized with OptiScene::Serialize. */
	FString SceneEdits;
	/** Blueprint findings (measured in Play): the Blueprint asset whose class defaults From/To describe. */
	FString Blueprint;

	bool IsSceneFinding() const { return !SceneEdits.IsEmpty(); }
	bool IsBlueprintFinding() const { return !Blueprint.IsEmpty(); }

	FString Metric;      // GPUTime or FrameTime
	double GainMs = 0.0; // positive = faster (for regressions, positive = how much slower it got)
	double CILowMs = 0.0;
	double CIHighMs = 0.0;
	double BaselineMs = 0.0;
	TArray<FOptiPassDelta> TopPasses;
	float VisualMean = 0.f;
	float VisualP95 = 0.f;
	FString CaptureA;
	FString CaptureB;
	FString CaptureDiff;

	FString ContextKey;
	TArray<int32> ActiveKenyonCells; // the situation's tag, used to learn from your decision
	FDateTime Created;
	FDateTime Changed;
	EOptiReminder Reminder = EOptiReminder::None;
	FDateTime RemindAt;
	FString Note;
	bool bDontSuggestAgain = false;

	bool IsOpen() const { return State == EOptiFindingState::New || State == EOptiFindingState::Postponed || State == EOptiFindingState::Stale; }
	bool IsDone() const { return State == EOptiFindingState::Applied || State == EOptiFindingState::ResolvedByUser; }
	double GainPercent() const { return BaselineMs > 0.0 ? 100.0 * GainMs / BaselineMs : 0.0; }
};

/** How one experiment ended. Every experiment is kept, so you can see what was tried and why it was not suggested. */
enum class EOptiTrialOutcome : uint8
{
	NoGain,   // no measurable difference
	TooSmall, // faster, but below the minimum gain worth your time
	Visible,  // faster, but the image changes
	Finding,  // faster and invisible: it went to the notebook
	Dropped,  // interrupted (camera moved, you selected what it was testing)
};

struct OPTICOMPANION_API FOptiTrial
{
	FDateTime Time;
	FName ActionId;
	FString Metric;
	double BaselineMs = 0.0;
	double GainMs = 0.0; // positive = faster
	bool bSignificant = false;
	float Visual = -1.f; // 1 = at the visibility threshold, -1 = not checked
	EOptiTrialOutcome Outcome = EOptiTrialOutcome::NoGain;
	bool bWhileWorking = false;
	FString ContextKey;
};

/**
 * The fly's notebook: every finding and what you decided about it. Lives in Saved/ (personal) or, when
 * shared with the team, in Config/ so it goes through version control with the project.
 */
class OPTICOMPANION_API FOptiNotebook
{
public:
	void Load(bool bShared);
	void Save() const;

	TSharedRef<FOptiFinding> Add(const FOptiFinding& Finding);
	TSharedPtr<FOptiFinding> Find(const FGuid& Id) const;
	TSharedPtr<FOptiFinding> FindOpenForAction(FName ActionId) const;
	void SetState(FOptiFinding& Finding, EOptiFindingState State);

	const TArray<TSharedRef<FOptiFinding>>& GetFindings() const { return Findings; }
	int32 CountNew() const;
	double TotalGainMs() const;
	FString CaptureDirectory() const;

	/** Experiments, newest last; personal (Saved/) even when the findings are shared, because timings are per machine. */
	void AddTrial(const FOptiTrial& Trial);
	const TArray<FOptiTrial>& GetTrials() const { return Trials; }
	void ClearTrials();

	FSimpleMulticastDelegate OnChanged;

private:
	FString Path() const;
	FString TrialsPath() const;
	void LoadTrials();
	void SaveTrials() const;

	TArray<FOptiTrial> Trials;

	TArray<TSharedRef<FOptiFinding>> Findings;
	bool bSharedWithTeam = false;
};

OPTICOMPANION_API const TCHAR* LexToString(EOptiFindingState State);
OPTICOMPANION_API const TCHAR* LexToString(EOptiTrialOutcome Outcome);
