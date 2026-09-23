#include "OptiFlyBrain.h"
#include "OptiProbe.h"
#include "OptiSmell.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Math/RandomStream.h"
#include "Misc/EngineVersion.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "RHIGlobals.h"
#include "HAL/IConsoleManager.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

namespace
{
	constexpr float ActiveFraction = 0.05f;     // APL keeps about 5% of Kenyon cells active
	constexpr float SessionRate = 0.5f;         // fast, "susceptible" memory
	constexpr float ProjectRate = 0.15f;        // slower, "restrained" memory
	constexpr float UserRate = 2.f;             // your decisions weigh more than a single measurement
	constexpr float SessionDecay = 0.97f;       // session memory fades a little after every experiment
	constexpr float NoveltyDepression = 0.6f;   // MBON-a'3 synapses depress each time a KC fires
	constexpr float ConsolidationShare = 0.3f;  // share of project memory moved to long-term per sleep
	constexpr float ForgettingOnNewHardware = 0.5f;
	// Defaults from the OptiCompanion.Brain.Tuning sweep (focus 0.5 / curiosity 0.25 scored best among balanced settings).
	TAutoConsoleVariable<float> CVarFocusWeight(TEXT("opti.Brain.FocusWeight"), 0.5f,
		TEXT("How strongly the central-complex focus biases which action the fly tries (0 = no bias)."));
	TAutoConsoleVariable<float> CVarCuriosity(TEXT("opti.Brain.Curiosity"), 0.25f,
		TEXT("Weight of the curiosity (uncertainty and novelty) bonus when choosing an experiment."));

	constexpr uint32 FileMagic = 0x4F505442;    // "OPTB"
	constexpr int32 FileVersion = 4; // 2: broad KC input per action, 3: PN adaptation, 4: view-based scent inputs

	const TArray<int32>& SectorPasses(EOptiSector Sector)
	{
		using namespace EOptiGlom;
		static const TArray<int32> Map[] = {
			{ PassShadowDepths, PassShadowProjection },
			{ PassDirectLighting, PassLumenScene, PassLumenGI, PassRayTracing },
			{ PassLumenReflections, PassReflections },
			{ PassTranslucency, PassFX },
			{ PassPostProcess, PassUpscale, PassMotionBlur, PassDepthOfField },
			{ PassClouds, PassFog, PassSky },
			{ PassBasePass, PassPrepass, PassNanite, PassOcclusion },
			{},
		};
		return Map[static_cast<int32>(Sector)];
	}

	FString HardwareFingerprint()
	{
		// No adapter name means no real GPU (-nullrhi, automation): unknown hardware, not different hardware.
		const FString Adapter = GRHIGlobals.GpuInfo.AdapterName.TrimStartAndEnd();
		return Adapter.IsEmpty() ? FString() : FString::Printf(TEXT("%s|%s"), *Adapter, *FEngineVersion::Current().ToString(EVersionComponent::Minor));
	}
}

FOptiFlyBrain::FOptiFlyBrain() = default;

void FOptiFlyBrain::Initialize(const FString& InProjectName, bool bInPersistent)
{
	ProjectName = InProjectName;
	bPersistent = bInPersistent;
	Connectome = FOptiConnectome::Load();
	NumActions = OptiActions::Catalog().Num();
	NumCells = Connectome.NumKenyonCells();

	Weights.Init(0.f, NumMemories * NumOutputs * NumActions * Stride());
	EvidenceProject.Init(0.f, NumActions * Stride());
	EvidenceLongTerm.Init(0.f, NumActions * Stride());
	EvidencePending.Init(0.f, NumActions * Stride());
	NoveltyWeights.Init(1.f, NumCells);
	PnMean.Init(0.f, EOptiGlom::Count);
	PnVar.Init(0.f, EOptiGlom::Count);
	PnSamples = 0;
	BaselineMean.Init(0.f, NumActions);
	BaselineCount.Init(0.f, NumActions);
	Ring.Init(1.f / static_cast<float>(EOptiSector::Count), static_cast<int32>(EOptiSector::Count));
	SectorReward.Init(0.f, Ring.Num());
	SectorExhaustion.Init(0.f, Ring.Num());
	Fingerprint = HardwareFingerprint();

	if (bPersistent)
	{
		LoadMemory(LongTermPath(), true);
		LoadMemory(ProjectPath(), false);
	}
	KnownProjects.AddUnique(ProjectName);
	Stats.Projects = KnownProjects.Num();

	UE_CLOG(bPersistent, LogOptiCompanion, Display, TEXT("Fly brain ready: %d Kenyon cells from %s, %d actions, %d experiments in long-term memory across %d projects."),
		NumCells, *Connectome.Source, NumActions, Stats.LongTermExperiments, Stats.Projects);
}

float& FOptiFlyBrain::Weight(EMemory Memory, EOutput Output, int32 Action, int32 Cell)
{
	return Weights[((Memory * NumOutputs + Output) * NumActions + Action) * Stride() + Cell];
}

float FOptiFlyBrain::WeightSum(EOutput Output, int32 Action, int32 Cell) const
{
	float Sum = 0.f;
	for (int32 Memory = 0; Memory < NumMemories; ++Memory)
	{
		Sum += Weights[((Memory * NumOutputs + Output) * NumActions + Action) * Stride() + Cell];
	}
	return Sum;
}

TArray<int32> FOptiFlyBrain::Encode(const FOptiSmell& Smell) const
{
	// Each PN passes part of its absolute rate plus how far it is above its own adapted mean, so inputs that
	// vary between situations stand out even if they are few (sensory adaptation).
	float Pn[EOptiGlom::Count];
	for (int32 Index = 0; Index < EOptiGlom::Count; ++Index)
	{
		const float Deviation = PnSamples > 1 ? (Smell.Projection[Index] - PnMean[Index]) / FMath::Sqrt(PnVar[Index] + 0.0025f) : 0.f;
		Pn[Index] = 0.3f * Smell.Projection[Index] + 0.25f * FMath::Clamp(Deviation, 0.f, 3.f);
	}

	TArray<TPair<float, int32>> Drive;
	Drive.Reserve(NumCells);
	for (int32 Cell = 0; Cell < NumCells; ++Cell)
	{
		float Input = 0.f;
		for (const FOptiConnectome::FClaw& Claw : Connectome.KenyonCells[Cell])
		{
			Input += Claw.Weight * Pn[Claw.Glomerulus];
		}
		if (Input > 0.f)
		{
			Drive.Add({ Input, Cell });
		}
	}

	// APL: one inhibitory neuron sums all KC output and feeds it back, so only the most driven ~5% fire.
	const int32 Keep = FMath::Clamp(FMath::RoundToInt32(ActiveFraction * NumCells), 1, Drive.Num());
	Drive.Sort([](const TPair<float, int32>& L, const TPair<float, int32>& R) { return L.Key > R.Key; });

	TArray<int32> Active;
	Active.Reserve(Keep);
	for (int32 Index = 0; Index < Keep; ++Index)
	{
		Active.Add(Drive[Index].Value);
	}
	Active.Sort();
	return Active;
}

float FOptiFlyBrain::Novelty(const TArray<int32>& Active) const
{
	if (Active.IsEmpty())
	{
		return 1.f;
	}
	float Sum = 0.f;
	for (int32 Cell : Active)
	{
		Sum += NoveltyWeights[Cell];
	}
	return Sum / Active.Num();
}

float FOptiFlyBrain::LearnedOutput(EOutput Output, int32 Action, const TArray<int32>& Active) const
{
	if (Active.IsEmpty())
	{
		return 0.f;
	}
	// Half the vote comes from the situation-specific cells, half from the broad one.
	float Sum = 0.f;
	for (int32 Cell : Active)
	{
		Sum += WeightSum(Output, Action, Cell);
	}
	Sum += Active.Num() * WeightSum(Output, Action, BroadCell());
	return Sum / (2 * Active.Num());
}

FOptiFlyBrain::FPrediction FOptiFlyBrain::Predict(int32 ActionIndex, const TArray<int32>& Active, const FOptiSmell& Smell) const
{
	const FOptiAction& Action = OptiActions::Catalog()[ActionIndex];
	FPrediction Prediction;
	Prediction.InnateGain = Action.TypicalSaving;
	Prediction.GainFraction = FMath::Clamp(Action.TypicalSaving + LearnedOutput(Gain, ActionIndex, Active), -1.f, 1.f);
	Prediction.Visual = FMath::Clamp(Action.VisualRisk + LearnedOutput(Visual, ActionIndex, Active), 0.f, 3.f);
	Prediction.Acceptance = FMath::Clamp(LearnedOutput(Acceptance, ActionIndex, Active), -1.f, 1.f);

	float Evidence = 0.f;
	for (int32 Cell : Active)
	{
		const int32 Index = ActionIndex * Stride() + Cell;
		Evidence += EvidenceProject[Index] + EvidenceLongTerm[Index];
	}
	const int32 Broad = ActionIndex * Stride() + BroadCell();
	Evidence += Active.Num() * (EvidenceProject[Broad] + EvidenceLongTerm[Broad]);
	Evidence = Active.IsEmpty() ? 0.f : Evidence / (2 * Active.Num());
	Prediction.Uncertainty = 1.f / FMath::Sqrt(1.f + Evidence);

	Prediction.TargetMs = Action.TargetMs(Smell.Profile);
	Prediction.ExpectedMs = Prediction.GainFraction * Prediction.TargetMs;
	return Prediction;
}

void FOptiFlyBrain::UpdateRing(const FOptiSmell& Smell)
{
	const int32 Sectors = Ring.Num();
	const double Gpu = FMath::Max(Smell.Profile.GpuMs, 0.001);

	TArray<float> Input;
	Input.SetNumZeroed(Sectors);
	for (int32 Sector = 0; Sector < Sectors; ++Sector)
	{
		float Share = 0.f;
		if (static_cast<EOptiSector>(Sector) == EOptiSector::CPU)
		{
			// The editor is often CPU-bound for reasons a game build does not share, so this pull is kept modest.
			Share = Smell.Profile.IsGpuBound() ? 0.05f : 0.35f;
		}
		else
		{
			for (int32 Pass : SectorPasses(static_cast<EOptiSector>(Sector)))
			{
				Share += static_cast<float>(Smell.Profile.PassMs[Pass] / Gpu);
			}
		}
		Input[Sector] = Share + 0.5f * SectorReward[Sector] - 0.6f * SectorExhaustion[Sector];
	}

	// Ring attractor: neighbours excite each other (cosine kernel) and everyone inhibits everyone (uniform
	// term), so one bump of activity forms around the strongest input. The bump starts from where it was
	// last time, so focus only moves when another sector clearly wins. Rates saturate at 1.
	TArray<float> Next;
	Next.SetNumZeroed(Sectors);
	const float Scale = 2.f / Sectors;
	for (int32 Step = 0; Step < 40; ++Step)
	{
		for (int32 I = 0; I < Sectors; ++I)
		{
			float Recurrent = 0.f;
			for (int32 J = 0; J < Sectors; ++J)
			{
				const float Angle = 2.f * PI * (I - J) / Sectors;
				Recurrent += Scale * (1.f * FMath::Cos(Angle) - 0.25f) * Ring[J];
			}
			Next[I] = Ring[I] + 0.3f * (-Ring[I] + FMath::Clamp(Recurrent + Input[I], 0.f, 1.f));
		}
		Ring = Next;
	}
}

float FOptiFlyBrain::FocusOf(EOptiSector Sector) const
{
	const float Peak = FMath::Max(Ring);
	return Peak > 1e-4f ? Ring[static_cast<int32>(Sector)] / Peak : 0.5f;
}

void FOptiFlyBrain::Adapt(const FOptiSmell& Smell)
{
	// Projection neurons adapt to sustained input: each keeps a running mean and variance of its own activity.
	constexpr float Rate = 0.05f;
	for (int32 Index = 0; Index < EOptiGlom::Count; ++Index)
	{
		const float Value = Smell.Projection[Index];
		if (PnSamples == 0)
		{
			PnMean[Index] = Value;
			PnVar[Index] = 0.f;
			continue;
		}
		const float Delta = Value - PnMean[Index];
		PnMean[Index] += Rate * Delta;
		PnVar[Index] = (1.f - Rate) * (PnVar[Index] + Rate * Delta * Delta);
	}
	++PnSamples;
}

EOptiSector FOptiFlyBrain::Focus() const
{
	int32 Best = 0;
	for (int32 Sector = 1; Sector < Ring.Num(); ++Sector)
	{
		if (Ring[Sector] > Ring[Best])
		{
			Best = Sector;
		}
	}
	return static_cast<EOptiSector>(Best);
}

FOptiFlyBrain::FDecision FOptiFlyBrain::Decide(const FOptiSmell& Smell, EOptiSelector Selector, const TSet<FName>& Blocked, UWorld* World,
	float MaxPredictedVisual)
{
	FDecision Best;
	Best.Selector = Selector;
	Adapt(Smell);
	Best.ActiveKenyonCells = Encode(Smell);
	Best.Novelty = Novelty(Best.ActiveKenyonCells);
	UpdateRing(Smell);
	Best.Focus = Focus();

	FRandomStream Rng(static_cast<int32>(FDateTime::Now().GetTicks() & 0x7fffffff));
	const TArray<FOptiAction>& Catalog = OptiActions::Catalog();
	float BestScore = -FLT_MAX;

	for (int32 Index = 0; Index < Catalog.Num(); ++Index)
	{
		const FOptiAction& Action = Catalog[Index];
		if (Blocked.Contains(Action.Id))
		{
			continue;
		}
		FOptiCVarSet A, B;
		if (!Action.BuildVariants(A, B, World))
		{
			continue;
		}
		const FPrediction Prediction = Predict(Index, Best.ActiveKenyonCells, Smell);
		const bool bCpu = Action.Passes.IsEmpty();
		if ((!bCpu && Prediction.TargetMs < 0.05) || (bCpu && Smell.Profile.IsGpuBound()))
		{
			continue; // nothing to gain here
		}
		if (Prediction.Visual > MaxPredictedVisual)
		{
			continue; // likely visible: only tried when you are not looking
		}

		float Score = 0.f;
		switch (Selector)
		{
		case EOptiSelector::Fly:
		{
			const float Useful = static_cast<float>(Prediction.ExpectedMs) * (1.f - FMath::Min(1.f, Prediction.Visual))
				* (1.f + 0.3f * Prediction.Acceptance);
			// Curiosity: what could be learned here, from uncertain actions or never-seen situations.
			const float Potential = static_cast<float>(Prediction.TargetMs) * Action.TypicalSaving;
			const float Curiosity = CVarCuriosity.GetValueOnGameThread() * (Prediction.Uncertainty + Best.Novelty) * Potential;
			const float FocusGain = 1.f + CVarFocusWeight.GetValueOnGameThread() * (FocusOf(Action.Sector) - 0.5f);
			Score = (Useful + Curiosity) * FocusGain;
			break;
		}
		case EOptiSelector::Thompson:
		{
			const float Count = BaselineCount[Index];
			const float Mean = Count > 0.f ? BaselineMean[Index] : Action.TypicalSaving;
			const float U1 = FMath::Max(Rng.GetFraction(), 1e-6f), U2 = Rng.GetFraction();
			const float Normal = FMath::Sqrt(-2.f * FMath::Loge(U1)) * FMath::Cos(2.f * PI * U2);
			Score = (Mean + Normal * 0.3f / FMath::Sqrt(1.f + Count)) * static_cast<float>(Prediction.TargetMs);
			break;
		}
		case EOptiSelector::Random:
			Score = Rng.GetFraction();
			break;
		}

		if (Score > BestScore)
		{
			BestScore = Score;
			Best.ActionIndex = Index;
			Best.VariantA = MoveTemp(A);
			Best.VariantB = MoveTemp(B);
			Best.Prediction = Prediction;
			Best.Score = Score;
		}
	}

	// The fly only naps on something worth at least a few hundredths of a millisecond.
	if (Selector == EOptiSelector::Fly && BestScore < 0.02f)
	{
		Best.ActionIndex = INDEX_NONE;
	}
	return Best;
}

FOptiFlyBrain::FDecision FOptiFlyBrain::DecideFor(const FOptiSmell& Smell, int32 ActionIndex, UWorld* World) const
{
	FDecision Decision;
	if (!OptiActions::Catalog().IsValidIndex(ActionIndex) || !OptiActions::Catalog()[ActionIndex].BuildVariants(Decision.VariantA, Decision.VariantB, World))
	{
		return Decision;
	}
	Decision.ActionIndex = ActionIndex;
	Decision.ActiveKenyonCells = Encode(Smell);
	Decision.Novelty = Novelty(Decision.ActiveKenyonCells);
	Decision.Prediction = Predict(ActionIndex, Decision.ActiveKenyonCells, Smell);
	Decision.Focus = Focus();
	return Decision;
}

FOptiFlyBrain::FDecision FOptiFlyBrain::DecidePlay(const FOptiSmell& Smell, int32 ActionIndex) const
{
	FDecision Decision;
	if (!OptiActions::Catalog().IsValidIndex(ActionIndex) || !OptiActions::Catalog()[ActionIndex].bPlayOnly)
	{
		return Decision;
	}
	const FOptiAction& Action = OptiActions::Catalog()[ActionIndex];
	Decision.ActionIndex = ActionIndex;
	Decision.Selector = EOptiSelector::Fly;
	Decision.ActiveKenyonCells = Encode(Smell);
	Decision.Novelty = Novelty(Decision.ActiveKenyonCells);
	Decision.Prediction = Predict(ActionIndex, Decision.ActiveKenyonCells, Smell);
	Decision.Focus = Focus();
	const FPrediction& Prediction = Decision.Prediction;
	const float Useful = static_cast<float>(Prediction.ExpectedMs) * (1.f - FMath::Min(1.f, Prediction.Visual)) * (1.f + 0.3f * Prediction.Acceptance);
	const float Potential = static_cast<float>(Prediction.TargetMs) * Action.TypicalSaving;
	const float Curiosity = CVarCuriosity.GetValueOnGameThread() * (Prediction.Uncertainty + Decision.Novelty) * Potential;
	const float FocusGain = 1.f + CVarFocusWeight.GetValueOnGameThread() * (FocusOf(Action.Sector) - 0.5f);
	Decision.Score = (Useful + Curiosity) * FocusGain;
	return Decision;
}

void FOptiFlyBrain::Teach(EOutput Output, int32 Action, const TArray<int32>& Active, float Target, float Current, float Rate)
{
	// Dopamine = prediction error. When the prediction is already right, nothing changes.
	const float Error = Target - Current;
	for (int32 Cell : Active)
	{
		Weight(Session, Output, Action, Cell) += SessionRate * Rate * Error;
		Weight(Project, Output, Action, Cell) += ProjectRate * Rate * Error;
	}
	Weight(Session, Output, Action, BroadCell()) += SessionRate * Rate * Error;
	Weight(Project, Output, Action, BroadCell()) += ProjectRate * Rate * Error;
}

void FOptiFlyBrain::LearnFromExperiment(const FDecision& Decision, float MeasuredGainFraction, float VisualScore)
{
	if (!Decision.IsValid())
	{
		return;
	}
	const int32 Action = Decision.ActionIndex;
	const FOptiAction& Info = Decision.Action();
	const TArray<int32>& Active = Decision.ActiveKenyonCells;

	// Session memory fades a little with every new experience.
	for (int32 Output = 0; Output < NumOutputs; ++Output)
	{
		for (int32 Cell = 0; Cell < Stride(); ++Cell)
		{
			Weight(Session, static_cast<EOutput>(Output), Action, Cell) *= SessionDecay;
		}
	}

	const float Gain = FMath::Clamp(MeasuredGainFraction, -1.f, 1.f);
	Teach(EOutput::Gain, Action, Active, Gain, Info.TypicalSaving + LearnedOutput(EOutput::Gain, Action, Active), 1.f);
	if (VisualScore >= 0.f)
	{
		LearnVisual(Decision, VisualScore);
	}

	for (int32 Cell : Active)
	{
		EvidenceProject[Action * Stride() + Cell] += 1.f;
		EvidencePending[Action * Stride() + Cell] += 1.f;
		NoveltyWeights[Cell] *= NoveltyDepression;
	}
	EvidenceProject[Action * Stride() + BroadCell()] += 1.f;
	EvidencePending[Action * Stride() + BroadCell()] += 1.f;

	BaselineCount[Action] += 1.f;
	BaselineMean[Action] += (Gain - BaselineMean[Action]) / BaselineCount[Action];

	const int32 Sector = static_cast<int32>(Info.Sector);
	SectorReward[Sector] = 0.7f * SectorReward[Sector] + 0.3f * FMath::Max(0.f, Gain);
	SectorExhaustion[Sector] = 0.7f * SectorExhaustion[Sector] + 0.3f * (Gain < 0.05f ? 1.f : 0.f);

	++Stats.Experiments;
	++Stats.LongTermExperiments;
}

void FOptiFlyBrain::LearnVisual(const FDecision& Decision, float VisualScore)
{
	if (!Decision.IsValid())
	{
		return;
	}
	const int32 Action = Decision.ActionIndex;
	const float Visual = FMath::Clamp(VisualScore, 0.f, 3.f);
	Teach(EOutput::Visual, Action, Decision.ActiveKenyonCells, Visual, Decision.Action().VisualRisk + LearnedOutput(EOutput::Visual, Action, Decision.ActiveKenyonCells), 1.f);
}

void FOptiFlyBrain::LearnNatural(int32 ActionIndex, const FOptiSmell& Smell, float GainFraction)
{
	if (!OptiActions::Catalog().IsValidIndex(ActionIndex) || Smell.Projection.Num() != EOptiGlom::Count)
	{
		return;
	}
	const TArray<int32> Active = Encode(Smell);
	const FOptiAction& Info = OptiActions::Catalog()[ActionIndex];
	const float Gain = FMath::Clamp(GainFraction, -1.f, 1.f);
	Teach(EOutput::Gain, ActionIndex, Active, Gain, Info.TypicalSaving + LearnedOutput(EOutput::Gain, ActionIndex, Active), 0.5f);
	for (int32 Cell : Active)
	{
		EvidenceProject[ActionIndex * Stride() + Cell] += 0.5f;
		EvidencePending[ActionIndex * Stride() + Cell] += 0.5f;
	}
	EvidenceProject[ActionIndex * Stride() + BroadCell()] += 0.5f;
	EvidencePending[ActionIndex * Stride() + BroadCell()] += 0.5f;
	++Stats.Experiments;
	++Stats.LongTermExperiments;
}

void FOptiFlyBrain::LearnFromUser(int32 ActionIndex, const TArray<int32>& Active, bool bAccepted)
{
	if (!OptiActions::Catalog().IsValidIndex(ActionIndex))
	{
		return;
	}
	Teach(EOutput::Acceptance, ActionIndex, Active, bAccepted ? 1.f : -1.f, LearnedOutput(EOutput::Acceptance, ActionIndex, Active), UserRate);
}

void FOptiFlyBrain::Consolidate()
{
	for (int32 Output = 0; Output < NumOutputs; ++Output)
	{
		for (int32 Action = 0; Action < NumActions; ++Action)
		{
			for (int32 Cell = 0; Cell < Stride(); ++Cell)
			{
				float& ProjectWeight = Weight(Project, static_cast<EOutput>(Output), Action, Cell);
				const float Moved = ConsolidationShare * ProjectWeight;
				Weight(LongTerm, static_cast<EOutput>(Output), Action, Cell) += Moved;
				ProjectWeight -= Moved;
			}
		}
	}
	for (int32 Index = 0; Index < EvidencePending.Num(); ++Index)
	{
		EvidenceLongTerm[Index] += EvidencePending[Index];
		EvidenceProject[Index] = FMath::Max(0.f, EvidenceProject[Index] - EvidencePending[Index]);
		EvidencePending[Index] = 0.f;
	}
	Save();
}

int32 FOptiFlyBrain::LearnedActions() const
{
	// An action counts as learned after two experiments; the broad cell sees every one of them.
	int32 Learned = 0;
	for (int32 Action = 0; Action < NumActions; ++Action)
	{
		const int32 Broad = Action * Stride() + BroadCell();
		Learned += EvidenceProject[Broad] + EvidenceLongTerm[Broad] >= 2.f ? 1 : 0;
	}
	return Learned;
}

void FOptiFlyBrain::Forget(bool bIncludeLongTerm)
{
	for (float& Value : Weights)
	{
		Value = 0.f;
	}
	for (float& Value : EvidenceProject)
	{
		Value = 0.f;
	}
	for (float& Value : EvidencePending)
	{
		Value = 0.f;
	}
	for (float& Value : NoveltyWeights)
	{
		Value = 1.f;
	}
	Stats.Experiments = 0;
	Stats.Naps = 0;
	IFileManager::Get().Delete(*ProjectPath(), false, true, true);

	if (bIncludeLongTerm)
	{
		for (float& Value : EvidenceLongTerm)
		{
			Value = 0.f;
		}
		Stats.LongTermExperiments = 0;
		KnownProjects.Reset();
		KnownProjects.Add(ProjectName);
		Stats.Projects = 1;
		IFileManager::Get().Delete(*LongTermPath(), false, true, true);
	}
	UE_LOG(LogOptiCompanion, Display, TEXT("The fly forgot %s. Only its innate knowledge is left."),
		bIncludeLongTerm ? TEXT("everything, in every project") : TEXT("what it learned in this project"));
}

FString FOptiFlyBrain::LongTermPath() const
{
	return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("OptiCompanion"), TEXT("LongTermBrain.opti"));
}

FString FOptiFlyBrain::ProjectPath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OptiCompanion") / TEXT("ProjectBrain.opti"));
}

void FOptiFlyBrain::Save() const
{
	if (!bPersistent)
	{
		return;
	}
	SaveMemory(ProjectPath(), false);
	SaveMemory(LongTermPath(), true);
}

bool FOptiFlyBrain::SaveMemory(const FString& Path, bool bLongTerm) const
{
	TArray<uint8> Bytes;
	FMemoryWriter Ar(Bytes);

	uint32 Magic = FileMagic;
	int32 Version = FileVersion;
	uint32 Hash = Connectome.Hash;
	int32 Cells = NumCells;
	bool bIsLongTerm = bLongTerm;
	Ar << Magic << Version << bIsLongTerm << Hash << Cells;

	TArray<FString> Ids;
	for (const FOptiAction& Action : OptiActions::Catalog())
	{
		Ids.Add(Action.Id.ToString());
	}
	Ar << Ids;

	const EMemory Memory = bLongTerm ? LongTerm : Project;
	TArray<float> Block;
	for (int32 Output = 0; Output < NumOutputs; ++Output)
	{
		const int32 Start = ((Memory * NumOutputs + Output) * NumActions) * Stride();
		Block.Append(&Weights[Start], NumActions * Stride());
	}
	Ar << Block;

	if (bLongTerm)
	{
		TArray<float> Evidence = EvidenceLongTerm;
		FString Print = Fingerprint.IsEmpty() ? StoredFingerprint : Fingerprint;
		TArray<FString> Projects = KnownProjects;
		int32 Experiments = Stats.LongTermExperiments;
		Ar << Evidence << Print << Projects << Experiments;
	}
	else
	{
		TArray<float> Evidence = EvidenceProject, Pending = EvidencePending, NoveltyCopy = NoveltyWeights;
		TArray<float> Mean = BaselineMean, Count = BaselineCount, RingCopy = Ring, Reward = SectorReward, Exhaustion = SectorExhaustion;
		int32 Experiments = Stats.Experiments, Naps = Stats.Naps;
		TArray<float> MeanCopy = PnMean, VarCopy = PnVar;
		int32 Samples = PnSamples;
		Ar << Evidence << Pending << NoveltyCopy << Mean << Count << RingCopy << Reward << Exhaustion << Experiments << Naps << MeanCopy << VarCopy << Samples;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	return FFileHelper::SaveArrayToFile(Bytes, *Path);
}

bool FOptiFlyBrain::LoadMemory(const FString& Path, bool bLongTerm)
{
	TArray<uint8> Bytes;
	if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
	{
		return false;
	}
	FMemoryReader Ar(Bytes);

	uint32 Magic = 0, Hash = 0;
	int32 Version = 0, Cells = 0;
	bool bIsLongTerm = false;
	Ar << Magic << Version << bIsLongTerm << Hash << Cells;
	if (Magic != FileMagic || Version != FileVersion || bIsLongTerm != bLongTerm)
	{
		UE_LOG(LogOptiCompanion, Warning, TEXT("Ignoring brain file %s: unknown format."), *Path);
		return false;
	}
	if (Hash != Connectome.Hash || Cells != NumCells)
	{
		UE_LOG(LogOptiCompanion, Warning, TEXT("Ignoring brain file %s: it was learned with a different connectome."), *Path);
		return false;
	}

	TArray<FString> Ids;
	TArray<float> Block;
	Ar << Ids << Block;
	if (Ar.IsError() || Block.Num() != NumOutputs * Ids.Num() * Stride())
	{
		return false;
	}

	// Actions are matched by id, so memories survive catalog changes between plugin versions.
	TArray<int32> Remap;
	for (const FString& Id : Ids)
	{
		Remap.Add(OptiActions::IndexOf(FName(*Id)));
	}

	const EMemory Memory = bLongTerm ? LongTerm : Project;
	for (int32 Output = 0; Output < NumOutputs; ++Output)
	{
		for (int32 Stored = 0; Stored < Ids.Num(); ++Stored)
		{
			if (Remap[Stored] == INDEX_NONE)
			{
				continue;
			}
			FMemory::Memcpy(&Weight(Memory, static_cast<EOutput>(Output), Remap[Stored], 0),
				&Block[(Output * Ids.Num() + Stored) * Stride()], Stride() * sizeof(float));
		}
	}

	auto CopyEvidence = [&](const TArray<float>& Source, TArray<float>& Target)
	{
		if (Source.Num() != Ids.Num() * Stride())
		{
			return;
		}
		for (int32 Stored = 0; Stored < Ids.Num(); ++Stored)
		{
			if (Remap[Stored] != INDEX_NONE)
			{
				FMemory::Memcpy(&Target[Remap[Stored] * Stride()], &Source[Stored * Stride()], Stride() * sizeof(float));
			}
		}
	};
	auto CopyPerAction = [&](const TArray<float>& Source, TArray<float>& Target)
	{
		for (int32 Stored = 0; Stored < FMath::Min(Ids.Num(), Source.Num()); ++Stored)
		{
			if (Remap[Stored] != INDEX_NONE)
			{
				Target[Remap[Stored]] = Source[Stored];
			}
		}
	};

	if (bLongTerm)
	{
		TArray<float> Evidence;
		FString StoredPrint;
		int32 Experiments = 0;
		Ar << Evidence << StoredPrint << KnownProjects << Experiments;
		CopyEvidence(Evidence, EvidenceLongTerm);
		Stats.LongTermExperiments = Experiments;
		StoredFingerprint = StoredPrint;

		// Forgetting: memories learned on another GPU or engine version are weakened instead of trusted.
		// A run without a GPU says nothing about the hardware, and a file that has none yet has nothing to compare.
		const bool bKnownPrints = !Fingerprint.IsEmpty() && !StoredPrint.IsEmpty() && !StoredPrint.StartsWith(TEXT("|"));
		if (bKnownPrints && StoredPrint != Fingerprint)
		{
			UE_LOG(LogOptiCompanion, Display, TEXT("Hardware or engine changed (%s -> %s): weakening long-term memory."), *StoredPrint, *Fingerprint);
			for (int32 Output = 0; Output < NumOutputs; ++Output)
			{
				for (int32 Action = 0; Action < NumActions; ++Action)
				{
					for (int32 Cell = 0; Cell < Stride(); ++Cell)
					{
						Weight(LongTerm, static_cast<EOutput>(Output), Action, Cell) *= ForgettingOnNewHardware;
					}
				}
			}
			for (float& Value : EvidenceLongTerm)
			{
				Value *= ForgettingOnNewHardware;
			}
		}
	}
	else
	{
		TArray<float> Evidence, Pending, NoveltyCopy, Mean, Count, RingCopy, Reward, Exhaustion;
		TArray<float> MeanCopy, VarCopy;
		int32 Samples = 0;
		Ar << Evidence << Pending << NoveltyCopy << Mean << Count << RingCopy << Reward << Exhaustion << Stats.Experiments << Stats.Naps << MeanCopy << VarCopy << Samples;
		if (MeanCopy.Num() == EOptiGlom::Count && VarCopy.Num() == EOptiGlom::Count)
		{
			PnMean = MeanCopy;
			PnVar = VarCopy;
			PnSamples = Samples;
		}
		CopyEvidence(Evidence, EvidenceProject);
		CopyEvidence(Pending, EvidencePending);
		if (NoveltyCopy.Num() == NumCells) { NoveltyWeights = NoveltyCopy; }
		CopyPerAction(Mean, BaselineMean);
		CopyPerAction(Count, BaselineCount);
		if (RingCopy.Num() == Ring.Num()) { Ring = RingCopy; }
		if (Reward.Num() == SectorReward.Num()) { SectorReward = Reward; }
		if (Exhaustion.Num() == SectorExhaustion.Num()) { SectorExhaustion = Exhaustion; }
	}
	return !Ar.IsError();
}

bool FOptiFlyBrain::ExportLongTerm(const FString& Path) const
{
	return SaveMemory(Path, true);
}

bool FOptiFlyBrain::ImportLongTerm(const FString& Path)
{
	if (!LoadMemory(Path, true))
	{
		return false;
	}
	Save();
	return true;
}
