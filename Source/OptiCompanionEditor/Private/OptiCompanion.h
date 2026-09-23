#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "OptiFlyBrain.h"
#include "HAL/IConsoleManager.h"
#include "OptiInventory.h"
#include "OptiNotebook.h"
#include "OptiPlayProfiler.h"
#include "OptiProbe.h"
#include "OptiReflex.h"
#include "OptiSmell.h"
#include "UObject/ObjectSaveContext.h"

class FOptiProbe;
class SOptiFlyLayer;
class SWindow;
class UPackage;

/** What the fly is doing; every animation maps to a real state of the system. */
enum class EOptiFlyMood : uint8
{
	Grooming, // observing, all good
	Sniffing, // capturing the frame profile
	Sleeping, // nap: an experiment is running
	Rubbing,  // has a confirmed finding waiting
	Buzzing,  // regression detected
};

/**
 * The companion: owns the brain, the notebook and the reflex, decides when to nap and when to talk.
 *
 * Rules against being Clippy: it only talks at natural pauses (after a save, when leaving PIE, when you
 * come back from being idle), never steals focus, only reports findings with statistical confidence,
 * learns from ignored notices and can be silenced per finding, per category or entirely.
 */
class FOptiCompanion : public TSharedFromThis<FOptiCompanion>
{
public:
	static TSharedPtr<FOptiCompanion> Get();
	static void Create();
	static void Destroy();

	void Startup();
	void Shutdown();

	// State for the UI.
	EOptiFlyMood GetMood() const { return Mood; }
	FText GetStatusText() const;
	/** One short line for next to the mascot: what it is testing now, or how the last test went (for a few seconds). */
	FText GetWhisper() const;
	/** True once every change worth testing in this level has been tried without finding anything. */
	bool IsSceneClean() const { return !CleanContext.IsEmpty(); }
	/** Describes a finished experiment in one line ("Fog grid: only 0.04 ms, not worth it"). */
	static FText DescribeTrial(const FOptiTrial& Trial);
	bool IsNapping() const { return Nap != ENap::None; }
	FOptiNotebook& GetNotebook() { return Notebook; }
	const FOptiFlyBrain& GetBrain() const { return Brain; }
	/** Blueprint costs measured in the current or last Play session. */
	const TArray<FOptiBlueprintCost>& GetPlayCosts() const { return PlayProfiler.GetCosts(); }
	FText DescribeFinding(const FOptiFinding& Finding) const;
	/** Incremented every time the fly should fly to the viewport (regressions). */
	int32 GetPointAtViewportRequest() const { return PointAtViewportRequest; }

	// Actions from the bubble, the notebook and menus.
	void ApplyFinding(const FGuid& Id);
	void UndoFinding(const FGuid& Id);
	void PostponeFinding(const FGuid& Id, EOptiReminder Reminder);
	void DismissFinding(const FGuid& Id, bool bForever, const FString& Note = FString());
	void RestoreFinding(const FGuid& Id);
	void RecheckFinding(const FGuid& Id);
	void OpenAsset(const FGuid& Id);
	void OnNoticeIgnored();
	void OnNoticeAnswered();
	void OnFlyClicked();
	void NapNow();
	void OpenNotebook();
	void ExportBrain();
	void ImportBrain();
	void ApplySettingsChange();

	FSimpleMulticastDelegate OnChanged;

private:
	enum class ENap : uint8 { None, Sniffing, Experimenting, VisualCheck };
	enum class EPause : uint8 { Saved, EndedPlay, BackFromIdle, Clicked };

	bool Tick(float DeltaTime);
	/** Returns whether an experiment may start; sets bOutAway when you have been away long enough for riskier ones. */
	bool CanNap(double IdleSeconds, bool& bOutAway);
	/** Phrase key of the reason the fly cannot experiment right now, or empty. */
	FString NapBlocker() const;
	bool IsCameraStill() const;
	bool IsBusy() const;
	uint32 ViewHash() const;
public:
	/** The level viewport's camera, for smelling and listing only what is on screen. */
	struct FOptiView CurrentView() const;
private:
	UWorld* EditorWorld() const;
	class FViewport* EditorViewport() const;

	void StartNap(bool bAway);
	void TickNap(double IdleSeconds);
	void OnSniffed(const struct FOptiProbeResult& Result);
	/** Picks an action for the current smell and starts the A/B experiment. */
	void DecideAndExperiment();
	void OnExperimented(const struct FOptiProbeResult& Result);
	void TickVisualCheck(double IdleSeconds);
	void SetVariantNow(bool bVariantB);
	void EndNap(bool bLearned);
	void ReleaseProbe();
	void AbortNap();
	TSet<FName> BlockedActions() const;
	/** Returns whether it became a finding. */
	bool RecordFinding(const struct FOptiProbeResult& Result, const struct FOptiImageDiff& Diff);
	/** Keeps the experiment in the notebook's "Tested" page, says the result briefly and notices a clean scene. */
	void RecordTrial(EOptiTrialOutcome Outcome, const struct FOptiProbeResult* Result, float Visual);
	/** Nothing left worth testing here: say it once, then test less often until something changes. */
	void DeclareClean(int32 TrialsWithoutFinding);

	// Natural experiments: your own changes, measured by the reflex.
	void OnPropertyChanged(UObject* Object, struct FPropertyChangedEvent& Event);
	void OnSelectionChanged(UObject* Selection);
	/** If Object (an actor or component) is being changed by the running experiment, restore it and stop. */
	bool ReleaseSceneEditsOf(const UObject* Object);
	void OnCVarsChanged();
	void SnapshotCVars();
	void OnNaturalChange(int32 ActionIndex, const FString& Label);
	void HandleVerdict(const FOptiReflex::FResult& Verdict);
	void LogDecision(const FOptiFlyBrain::FDecision& Decision, float MeasuredGain, float Visual) const;

	void CheckFindings();
	void OnNaturalPause(EPause Pause);
	void Queue(const FGuid& Id, bool bFront = false);
	void ShowNextNotice(bool bForce);
	void ShowToast(const FOptiFinding& Finding);
	int32 NoticeBudget() const;

	void OnPackageSaved(const FString& Filename, UPackage* Package, FObjectPostSaveContext Context);
	void OnEndPIE(bool bIsSimulating);
	void OnBeginPIE(bool bIsSimulating);
	void OnTickIntervalResult(const FOptiTickIntervalResult& Result);
	void ShowPlaySummary();
	void AttachLayer(TSharedPtr<SWindow> Window);
	void DetachLayer();
	void SetMood(EOptiFlyMood NewMood);

	FOptiFlyBrain Brain;
	FOptiNotebook Notebook;
	FOptiReflex Reflex;
	TSharedPtr<FOptiProbe> Probe;
	/** Finished probes are released on the next tick: they call back into us from inside their own tick. */
	TArray<TSharedPtr<FOptiProbe>> FinishedProbes;
	TSharedPtr<SOptiFlyLayer> Layer;
	TWeakPtr<SWindow> LayerWindow;

	ENap Nap = ENap::None;
	double LastInteractionAtNapStart = 0.0;
	double NextNapTime = 0.0;
	bool bForceNap = false;
	bool bWasIdle = false;
	bool bJustWoke = false;
	FOptiSmell NapSmell;
	FOptiFlyBrain::FDecision NapDecision;
	TSharedPtr<FOptiFinding> NapRecheck;
	TSharedPtr<TArray<struct FOptiSceneEdit>> NapSceneEdits;
	uint32 NapView = 0;
	bool bNapAway = false;
	bool bHasSmell = false;
	/** When the current smell was taken, and for which camera: a fresh one is reused instead of sniffing again. */
	double SmellTime = -1000.0;
	uint32 SmellView = 0;

	// Visual check, done in the first short pause after a promising timing result.
	TSharedPtr<FOptiProbeResult> PendingResult;
	int32 VisualStep = 0;
	int32 VisualFrames = 0;
	double VisualDeadline = 0.0;
	FOptiCapture CaptureA, CaptureB, CaptureA2;

	struct FNaturalChange { int32 ActionIndex = INDEX_NONE; FString Label; };
	FNaturalChange PendingNatural;
	TMap<FString, FString> CVarSnapshot;
	bool bOwnChange = false;
	double LastConsolidation = 0.0;
	FDelegateHandle PropertyChangedHandle;
	FDelegateHandle SelectionHandle;
	FConsoleVariableSinkHandle CVarSinkHandle;
	FText LastStatus;
	FText LastResultText;
	double LastResultAt = -1000.0;
	FString DryContext;      // level + scalability the dry streak belongs to
	int32 DryStreak = 0;     // experiments in a row without a finding
	FString CleanContext;    // set when the scene was declared clean
	FString LastBlocker;
	TSet<FString> LoggedBlockers;

	TMap<FString, double> RecentlyTested; // "action|context" -> time
	TArray<FGuid> NoticeQueue;
	TArray<double> NoticeTimes;
	int32 IgnoredStreak = 0;
	double NextFindingCheck = 0.0;
	int32 PointAtViewportRequest = 0;
	EOptiFlyMood Mood = EOptiFlyMood::Grooming;

	FTSTicker::FDelegateHandle TickerHandle;
	FDelegateHandle PackageSavedHandle;
	FDelegateHandle EndPIEHandle;
	FDelegateHandle BeginPIEHandle;
	FOptiPlayProfiler PlayProfiler;
	/** The brain's decision for the Blueprint being tested in Play, kept to learn from the result. */
	FOptiFlyBrain::FDecision PlayDecision;
	FString PlayDecisionBlueprint;
	int32 ChoosePlayCandidate(const TArray<FOptiBlueprintCost>& Candidates, UWorld* World);
	bool bPlaySummaryPending = false;
	FDelegateHandle MainFrameHandle;
};
