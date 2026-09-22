#pragma once

#include "CoreMinimal.h"
#include "OptiActions.h"
#include "OptiConnectome.h"

struct FOptiSmell;

/** Which policy chooses the next experiment. The baselines exist to prove the fly earns its place. */
enum class EOptiSelector : uint8
{
	Fly,
	Thompson,
	Random,
};

/**
 * The fly's brain.
 *
 *  - Antennal lobe (FOptiSmell): 51 glomeruli with divisive normalisation.
 *  - Mushroom body: PN -> Kenyon cell expansion through the connectome, APL feedback inhibition that keeps
 *    only ~5% of KCs active (a sparse "tag" of the situation), and three MBON outputs per action: expected
 *    saving, expected visual cost and how much you like it. Dopamine carries the prediction error
 *    (Bennett, Philippides & Nowotny 2021), so learning stops by itself once the prediction is right.
 *  - Three memories at different speeds (incentive circuit, Gkanias et al. 2022): session (fast, fades),
 *    project (saved in Saved/) and long-term (in your user folder, shared by every project, filled during
 *    sleep consolidation and weakened when the GPU or engine changes).
 *  - MBON-a'3 novelty: KCs lose novelty each time they fire (Dasgupta et al. 2018); new situations make
 *    the fly curious.
 *  - Lateral horn: innate knowledge in the action catalog, which learning can correct.
 *  - Central complex: a ring attractor over sectors keeps focus on one area until it is exhausted.
 */
class OPTICOMPANION_API FOptiFlyBrain
{
public:
	enum EOutput : int32 { Gain, Visual, Acceptance, NumOutputs };
	enum EMemory : int32 { Session, Project, LongTerm, NumMemories };

	struct FPrediction
	{
		float GainFraction = 0.f;   // of the targeted time
		float InnateGain = 0.f;     // lateral horn part
		float Visual = 0.f;         // 1 = at your visibility threshold
		float Acceptance = 0.f;     // -1 you dismiss these, +1 you accept them
		float Uncertainty = 1.f;
		double TargetMs = 0.0;
		double ExpectedMs = 0.0;
	};

	struct FDecision
	{
		int32 ActionIndex = INDEX_NONE;
		FOptiCVarSet VariantA;
		FOptiCVarSet VariantB;
		FPrediction Prediction;
		TArray<int32> ActiveKenyonCells;
		float Novelty = 0.f;
		float Score = 0.f;
		EOptiSector Focus = EOptiSector::Shadows;
		EOptiSelector Selector = EOptiSelector::Fly;

		bool IsValid() const { return ActionIndex != INDEX_NONE; }
		const FOptiAction& Action() const { return OptiActions::Catalog()[ActionIndex]; }
	};

	struct FStats
	{
		int32 Experiments = 0;
		int32 Naps = 0;
		int32 Projects = 0;
		int32 LongTermExperiments = 0;
	};

	FOptiFlyBrain();

	/** Loads the wiring and the project and long-term memories. Non-persistent brains (tests) never touch disk. */
	void Initialize(const FString& ProjectName, bool bPersistent = true);
	void Save() const;

	/** Kenyon cells that survive APL inhibition for this smell. */
	TArray<int32> Encode(const FOptiSmell& Smell) const;
	float Novelty(const TArray<int32>& ActiveKenyonCells) const;
	FPrediction Predict(int32 ActionIndex, const TArray<int32>& ActiveKenyonCells, const FOptiSmell& Smell) const;

	/**
	 * Picks the next experiment for this smell. Actions in Blocked (tested recently, or dismissed for good)
	 * are skipped. Returns an invalid decision when nothing is worth a nap.
	 */
	FDecision Decide(const FOptiSmell& Smell, EOptiSelector Selector, const TSet<FName>& Blocked, UWorld* World = nullptr,
		float MaxPredictedVisual = FLT_MAX);

	/** A decision for one specific action, used to re-measure stale findings. Invalid if it cannot apply. */
	FDecision DecideFor(const FOptiSmell& Smell, int32 ActionIndex, UWorld* World = nullptr) const;

	/**
	 * Play-only actions (Blueprint Tick Interval): the variants are built by the caller, the brain only smells the
	 * situation and scores it with the same utility + curiosity + focus as editor experiments.
	 */
	FDecision DecidePlay(const FOptiSmell& Smell, int32 ActionIndex) const;

	/**
	 * Dopamine after an experiment: measured saving on the targeted passes and visual difference (1 = threshold).
	 * A negative visual score means it was not measured (yet); only the saving is learned then.
	 */
	void LearnFromExperiment(const FDecision& Decision, float MeasuredGainFraction, float VisualScore);

	/** The visual check of an experiment arrived later than its timing. */
	void LearnVisual(const FDecision& Decision, float VisualScore);

	/**
	 * Natural experiment: you made a change that matches an action and the reflex measured the frame before
	 * and after. Noisier than a nap (whole frame, one before/after), so it counts for half.
	 */
	void LearnNatural(int32 ActionIndex, const FOptiSmell& Smell, float GainFraction);

	/** Dopamine from you: accepting a finding is reward, dismissing it is punishment for that action in that context. */
	void LearnFromUser(int32 ActionIndex, const TArray<int32>& ActiveKenyonCells, bool bAccepted);

	/** Sleep: part of the project memory moves to long-term memory. */
	void Consolidate();

	void NoteNap() { ++Stats.Naps; }

	const FStats& GetStats() const { return Stats; }
	int32 LearnedActions() const;
	const FOptiConnectome& GetConnectome() const { return Connectome; }
	EOptiSector Focus() const;
	const TArray<float>& RingActivity() const { return Ring; }

	FString LongTermPath() const;
	FString ProjectPath() const;
	bool ExportLongTerm(const FString& Path) const;
	bool ImportLongTerm(const FString& Path);

private:
	/**
	 * Besides the sparse, situation-specific Kenyon cells, each action has one "broad" input that is active in
	 * every situation, like the alpha'/beta' KCs that respond to almost any odour. It learns what an action is
	 * worth in general; the sparse cells learn how the current situation differs from that.
	 */
	int32 Stride() const { return NumCells + 1; }
	int32 BroadCell() const { return NumCells; }

	float& Weight(EMemory Memory, EOutput Output, int32 Action, int32 Cell);
	float WeightSum(EOutput Output, int32 Action, int32 Cell) const;
	float LearnedOutput(EOutput Output, int32 Action, const TArray<int32>& Active) const;
	void Teach(EOutput Output, int32 Action, const TArray<int32>& Active, float Target, float Current, float Rate);
	void UpdateRing(const FOptiSmell& Smell);
	/** Ring activity of a sector relative to the bump's peak, 0..1. */
	float FocusOf(EOptiSector Sector) const;
	/** Updates the projection neurons' adapted mean and variance with a new smell. */
	void Adapt(const FOptiSmell& Smell);

	bool SaveMemory(const FString& Path, bool bLongTerm) const;
	bool LoadMemory(const FString& Path, bool bLongTerm);

	FOptiConnectome Connectome;
	int32 NumActions = 0;
	int32 NumCells = 0;

	/** [Memory][Output][Action][Cell], flattened. */
	TArray<float> Weights;
	/** Evidence per action and cell: project, long-term, and project evidence not yet consolidated. */
	TArray<float> EvidenceProject;
	TArray<float> EvidenceLongTerm;
	TArray<float> EvidencePending;
	/** MBON-a'3 novelty weight per KC, 1 = never seen. */
	TArray<float> NoveltyWeights;

	/** Thompson-sampling baseline: per action running mean and count of the measured gain fraction. */
	TArray<float> BaselineMean;
	TArray<float> BaselineCount;

	/** Antennal-lobe adaptation: running mean and variance of every projection neuron. */
	TArray<float> PnMean;
	TArray<float> PnVar;
	int32 PnSamples = 0;

	/** Central complex: ring activity per sector, and per-sector reward and exhaustion traces. */
	TArray<float> Ring;
	TArray<float> SectorReward;
	TArray<float> SectorExhaustion;

	FStats Stats;
	bool bPersistent = true;
	FString ProjectName;
	FString Fingerprint;       // empty when there is no real GPU (-nullrhi, automation)
	FString StoredFingerprint; // what the long-term file says, kept as is when Fingerprint is empty
	TArray<FString> KnownProjects;
};
