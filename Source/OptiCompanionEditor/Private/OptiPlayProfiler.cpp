#include "OptiPlayProfiler.h"
#include "OptiAnalysis.h"
#include "OptiCompanionSettings.h"
#include "OptiPhrases.h"

#include "Async/Async.h"
#include "Components/ActorComponent.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ProfilingDebugging/MiscTrace.h"
#include "ProfilingDebugging/TraceAuxiliary.h"
#include "TraceServices/AnalysisService.h"
#include "TraceServices/ITraceServicesModule.h"
#include "TraceServices/ModuleService.h"
#include "TraceServices/Model/AnalysisSession.h"
#include "TraceServices/Model/Frames.h"
#include "TraceServices/Model/Threads.h"
#include "TraceServices/Model/TimingProfiler.h"

namespace
{
	constexpr double WarmupSeconds = 4.0;       // let the level load and settle before measuring
	constexpr double CaptureSeconds = 8.0;
	constexpr double SecondsBetweenCaptures = 45.0;
	constexpr double MinTestMsPerFrame = 0.15;  // below this a class is not worth an experiment
	constexpr int32 BlocksPerVariant = 8;
	constexpr int32 SettleFrames = 6;
	constexpr int32 FramesPerBlock = 24;

	const UOptiCompanionSettings& Settings() { return *GetDefault<UOptiCompanionSettings>(); }

	TAutoConsoleVariable<int32> CVarPlayDryRun(
		TEXT("opti.Play.DryRun"), 0,
		TEXT("1: in Play the fly smells every expensive Blueprint and picks one, but runs no experiment and writes no finding (tests)."));
}

FOptiPlayProfiler::~FOptiPlayProfiler()
{
	FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
	FWorldDelegates::OnWorldPostActorTick.Remove(WorldPostActorTickHandle);
	if (State == EState::Capturing)
	{
		FTraceAuxiliary::Stop();
	}
	if (PendingAnalysis.IsValid())
	{
		PendingAnalysis.Wait();
	}
}

UWorld* FOptiPlayProfiler::PlayWorld() const
{
	return GEditor ? GEditor->PlayWorld.Get() : nullptr;
}

void FOptiPlayProfiler::OnBeginPlay()
{
	if (!Settings().bEnabled || !Settings().bMeasureBlueprintsInPlay)
	{
		return;
	}
	State = EState::Waiting;
	NextActionTime = FPlatformTime::Seconds() + WarmupSeconds;
	Costs.Reset();
	CaptureCount = 0;
	Tested.Reset();
}

void FOptiPlayProfiler::OnEndPlay()
{
	if (State == EState::Capturing)
	{
		SnapshotNames(); // the play world is still alive here
		StopCapture();   // analysed in the background; the result still counts for this session
	}
	else if (State == EState::Experimenting)
	{
		EndExperiment(false);
	}
	if (State != EState::Analyzing)
	{
		State = EState::Off;
	}
}

FText FOptiPlayProfiler::GetStatus() const
{
	switch (State)
	{
	case EState::Capturing:
	case EState::Analyzing:
		return OptiText(TEXT("play.measuring"));
	case EState::Experimenting:
		return FOptiPhrases::Get().Text(TEXT("play.testing"), { { TEXT("blueprint"), FText::FromString(Candidate.ClassName.LeftChop(2)) } });
	default:
		return FText::GetEmpty();
	}
}

void FOptiPlayProfiler::Tick()
{
	const double Now = FPlatformTime::Seconds();
	switch (State)
	{
	case EState::Waiting:
		if (Now >= NextActionTime && PlayWorld())
		{
			StartCapture();
		}
		break;
	case EState::Capturing:
		if (Now >= CaptureEnd)
		{
			SnapshotNames();
			StopCapture();
		}
		break;
	case EState::Analyzing:
		if (PendingAnalysis.IsValid() && PendingAnalysis.IsReady())
		{
			const FAnalysis Analysis = PendingAnalysis.Get();
			PendingAnalysis.Reset();
			if (ITraceServicesModule* Module = FModuleManager::GetModulePtr<ITraceServicesModule>(TEXT("TraceServices")))
			{
				if (TSharedPtr<TraceServices::IModuleService> Modules = Module->GetModuleService())
				{
					for (const FName& Name : ModulesTurnedOff)
					{
						Modules->SetModuleEnabled(Name, true);
					}
				}
			}
			ModulesTurnedOff.Reset();
			FinishAnalysis(Analysis);
		}
		break;
	case EState::Experimenting:
		TickExperiment();
		break;
	default:
		break;
	}
}

// ---------------------------------------------------------------------------------------------- capture

void FOptiPlayProfiler::StartCapture()
{
	// Never take over a trace you started yourself (Unreal Insights, -trace on the command line).
	if (FTraceAuxiliary::IsConnected())
	{
		UE_LOG(LogOptiCompanion, Log, TEXT("Play: a trace is already running (%s); Blueprint costs are not measured this session."), *FTraceAuxiliary::GetTraceDestinationString());
		State = EState::Off;
		return;
	}
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OptiCompanion") / TEXT("Play"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	TracePath = Directory / FString::Printf(TEXT("Play_%s_%d.utrace"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")), CaptureCount);
	if (!FTraceAuxiliary::Start(FTraceAuxiliary::EConnectionType::File, *TracePath, TEXT("cpu,frame")))
	{
		UE_LOG(LogOptiCompanion, Warning, TEXT("Play: could not start a CPU trace to %s."), *TracePath);
		State = EState::Off;
		return;
	}
	State = EState::Capturing;
	CaptureStart = FPlatformTime::Seconds();
	CaptureEnd = CaptureStart + CaptureSeconds;
	UE_LOG(LogOptiCompanion, Log, TEXT("Play: measuring Blueprint ticks for %.0f s."), CaptureSeconds);
}

void FOptiPlayProfiler::SnapshotNames()
{
	// Each Tick scope in the trace is named after the object's class (or, in some engine setups, the object's own
	// FName). Map both to Blueprint classes.
	NameToClass.Reset();
	AmbiguousNames.Reset();
	UWorld* World = PlayWorld();
	if (!World)
	{
		return;
	}
	auto Add = [this](const FString& Name, const FActorInfo& Info)
	{
		if (AmbiguousNames.Contains(Name))
		{
			return;
		}
		if (const FActorInfo* Existing = NameToClass.Find(Name))
		{
			if (Existing->BlueprintPath != Info.BlueprintPath)
			{
				NameToClass.Remove(Name); // "CharacterMovement0" in two different classes: cannot tell them apart
				AmbiguousNames.Add(Name);
			}
			return;
		}
		NameToClass.Add(Name, Info);
	};
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		const UBlueprint* Blueprint = Cast<UBlueprint>(Actor->GetClass()->ClassGeneratedBy);
		if (!Blueprint)
		{
			continue;
		}
		FActorInfo Info;
		Info.ClassName = Actor->GetClass()->GetName();
		Info.BlueprintPath = Blueprint->GetPathName();
		Info.TickInterval = Actor->GetActorTickInterval();
		Add(Actor->GetFName().ToString(), Info);
		Add(Info.ClassName, Info);
		for (UActorComponent* Component : Actor->GetComponents())
		{
			if (Component && Component->IsComponentTickEnabled())
			{
				Add(Component->GetFName().ToString(), Info);
			}
		}
	}
}

void FOptiPlayProfiler::StopCapture()
{
	FTraceAuxiliary::Stop();
	State = EState::Analyzing;
	++CaptureCount;

	// Analysis reads the whole file and can take a second or two: never on the game thread.
	ITraceServicesModule& Module = FModuleManager::LoadModuleChecked<ITraceServicesModule>(TEXT("TraceServices"));
	TSharedPtr<TraceServices::IAnalysisService> Service = Module.GetAnalysisService();
	// Only the timing analysis is needed. The others (memory above all) would also chew through the history the
	// engine flushes into every new trace, and complain about it in the log.
	ModulesTurnedOff.Reset();
	if (TSharedPtr<TraceServices::IModuleService> Modules = Module.GetModuleService())
	{
		TArray<TraceServices::FModuleInfo> Enabled;
		Modules->GetEnabledModules(Enabled);
		for (const TraceServices::FModuleInfo& Info : Enabled)
		{
			if (Info.Name != FName(TEXT("TraceModule_TimingProfiler")))
			{
				Modules->SetModuleEnabled(Info.Name, false);
				ModulesTurnedOff.Add(Info.Name);
			}
		}
	}
	const FString Path = TracePath;
	// A new trace starts with everything the engine kept since startup: only our own window is measured.
	const double Window = FPlatformTime::Seconds() - CaptureStart;
	PendingAnalysis = Async(EAsyncExecution::Thread, [Service, Path, Window]()
	{
		FAnalysis Out;
		TSharedPtr<const TraceServices::IAnalysisSession> Session = Service.IsValid() ? Service->Analyze(*Path) : nullptr;
		if (!Session.IsValid())
		{
			Out.Error = TEXT("the trace could not be analysed");
		}
		else
		{
			TraceServices::FAnalysisSessionReadScope Scope(*Session);
			uint32 GameThread = 0;
			TraceServices::ReadThreadProvider(*Session).EnumerateThreads([&GameThread](const TraceServices::FThreadInfo& Thread)
			{
				if (Thread.Name && FCString::Strcmp(Thread.Name, TEXT("GameThread")) == 0)
				{
					GameThread = Thread.Id;
				}
			});
			const double End = Session->GetDurationSeconds();
			const double Start = FMath::Max(0.0, End - Window);
			TraceServices::ReadFrameProvider(*Session).EnumerateFrames(TraceFrameType_Game, Start, End, [&Out](const TraceServices::FFrame&) { ++Out.Frames; });
			const TraceServices::ITimingProfilerProvider* Timing = TraceServices::ReadTimingProfilerProvider(*Session);
			if (Timing && GameThread != 0 && Out.Frames > 0)
			{
				TraceServices::FCreateAggregationParams Params;
				Params.IntervalStart = Start;
				Params.IntervalEnd = End;
				Params.CpuThreadFilter = [GameThread](uint32 Thread) { return Thread == GameThread; };
				if (TraceServices::ITable<TraceServices::FTimingProfilerAggregatedStats>* Table = Timing->CreateAggregation(Params))
				{
					TraceServices::ITableReader<TraceServices::FTimingProfilerAggregatedStats>* Reader = Table->CreateReader();
					while (Reader && Reader->IsValid())
					{
						const TraceServices::FTimingProfilerAggregatedStats* Row = Reader->GetCurrentRow();
						if (Row && Row->Timer && Row->Timer->Name)
						{
							Out.Timers.Add({ Row->Timer->Name, Row->TotalInclusiveTime, Row->InstanceCount });
						}
						Reader->NextRow();
					}
					delete Reader;
					delete Table;
					Out.bOk = true;
				}
			}
			if (!Out.bOk && Out.Error.IsEmpty())
			{
				Out.Error = TEXT("no game-thread timing in the trace");
			}
		}
		IFileManager::Get().Delete(*Path, false, true, true); // it can be tens of MB; only the numbers are kept
		return Out;
	});
}

void FOptiPlayProfiler::FinishAnalysis(const FAnalysis& Analysis)
{
	const bool bStillPlaying = PlayWorld() != nullptr;
	State = bStillPlaying ? EState::Waiting : EState::Off;
	NextActionTime = FPlatformTime::Seconds() + SecondsBetweenCaptures;
	if (!Analysis.bOk)
	{
		UE_LOG(LogOptiCompanion, Log, TEXT("Play: no Blueprint costs this time (%s)."), *Analysis.Error);
		return;
	}

	// Add the Tick scopes up per Blueprint class. Nested scopes (a component ticked from its actor) are rare
	// in Blueprints; the inclusive time of each scope is what that object cost.
	TMap<FString, FOptiBlueprintCost> ThisCapture;
	TMap<FString, TSet<FString>> InstancesSeen;
	for (const FTimerTotal& Timer : Analysis.Timers)
	{
		if (Timer.Name == TEXT("FEngineLoop::Tick"))
		{
			LastFrameMs = Timer.TotalSeconds * 1000.0 / double(Analysis.Frames);
		}
		else if (Timer.Name == TEXT("UWorld_Tick"))
		{
			LastWorldTickMs = Timer.TotalSeconds * 1000.0 / double(Analysis.Frames);
		}
		const FActorInfo* Info = NameToClass.Find(Timer.Name);
		if (!Info)
		{
			continue;
		}
		FOptiBlueprintCost& Cost = ThisCapture.FindOrAdd(Info->BlueprintPath);
		Cost.ClassName = Info->ClassName;
		Cost.BlueprintPath = Info->BlueprintPath;
		Cost.TickInterval = Info->TickInterval;
		Cost.MsPerFrame += Timer.TotalSeconds * 1000.0 / double(Analysis.Frames);
		if (Timer.Name == Info->ClassName)
		{
			// One scope for the whole class: its call count per frame is the number of instances ticking.
			Cost.Instances = FMath::Max(Cost.Instances, FMath::Max(1, FMath::RoundToInt32(double(Timer.Count) / double(Analysis.Frames))));
		}
		else
		{
			InstancesSeen.FindOrAdd(Info->BlueprintPath).Add(Timer.Name);
		}
	}

	for (TPair<FString, FOptiBlueprintCost>& Pair : ThisCapture)
	{
		if (const TSet<FString>* Seen = InstancesSeen.Find(Pair.Key))
		{
			Pair.Value.Instances = FMath::Max(Pair.Value.Instances, Seen->Num());
		}
		FOptiBlueprintCost* Existing = Costs.FindByPredicate([&Pair](const FOptiBlueprintCost& C) { return C.BlueprintPath == Pair.Key; });
		if (!Existing)
		{
			Existing = &Costs.Add_GetRef(Pair.Value);
			Existing->MsPerFrame = 0.0;
		}
		// Running average over captures, so one hitchy capture does not dominate.
		Existing->MsPerFrame = (Existing->MsPerFrame * Existing->Captures + Pair.Value.MsPerFrame) / (Existing->Captures + 1);
		Existing->PeakMsPerFrame = FMath::Max(Existing->PeakMsPerFrame, Pair.Value.MsPerFrame);
		Existing->Instances = FMath::Max(Existing->Instances, Pair.Value.Instances);
		Existing->TickInterval = Pair.Value.TickInterval;
		++Existing->Captures;
	}
	Costs.Sort([](const FOptiBlueprintCost& L, const FOptiBlueprintCost& R) { return L.MsPerFrame > R.MsPerFrame; });

	UE_LOG(LogOptiCompanion, Log, TEXT("Play: %llu frames analysed, %d Blueprint classes ticked."), Analysis.Frames, ThisCapture.Num());
	for (int32 Index = 0; Index < FMath::Min(5, Costs.Num()); ++Index)
	{
		UE_LOG(LogOptiCompanion, Log, TEXT("Play:   %-32s %6.3f ms/frame  (%d instances, tick interval %.2f s)"),
			*Costs[Index].ClassName, Costs[Index].MsPerFrame, Costs[Index].Instances, Costs[Index].TickInterval);
	}
	if (OnCostsUpdated)
	{
		OnCostsUpdated();
	}
	if (bStillPlaying)
	{
		StartExperiment();
	}
}

// ---------------------------------------------------------------------------------------------- experiment

void FOptiPlayProfiler::StartExperiment()
{
	UWorld* World = PlayWorld();
	const bool bDryRun = CVarPlayDryRun.GetValueOnGameThread() != 0;
	TArray<FOptiBlueprintCost> Candidates = Costs.FilterByPredicate([this, bDryRun](const FOptiBlueprintCost& Cost)
	{
		return bDryRun || (Cost.MsPerFrame >= MinTestMsPerFrame && Cost.TickInterval < TriedInterval * 0.5f && !Tested.Contains(Cost.BlueprintPath)
			&& !(IsAlreadySuggested && IsAlreadySuggested(Cost.BlueprintPath)));
	});
	if (Candidates.IsEmpty() || !World)
	{
		return;
	}
	// The fly brain picks (and may decline): it predicts what each class would save from how the situation smells.
	const int32 Choice = ChooseCandidate ? ChooseCandidate(Candidates, World) : 0;
	if (!Candidates.IsValidIndex(Choice))
	{
		return;
	}
	Candidate = Candidates[Choice];
	if (bDryRun)
	{
		UE_LOG(LogOptiCompanion, Log, TEXT("Play: dry run, the fly would test %s."), *Candidate.ClassName);
		return;
	}
	Tested.Add(Candidate.BlueprintPath);
	Subjects.Reset();
	OriginalIntervals.Reset();
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		const UBlueprint* Blueprint = Cast<UBlueprint>(It->GetClass()->ClassGeneratedBy);
		if (Blueprint && Blueprint->GetPathName() == Candidate.BlueprintPath && It->IsActorTickEnabled())
		{
			Subjects.Add(*It);
			OriginalIntervals.Add(It->GetActorTickInterval());
		}
	}
	if (Subjects.IsEmpty())
	{
		return;
	}
	BlockMeansA.Reset();
	BlockMeansB.Reset();
	BlockFrames.Reset();
	Block = 0;
	FrameInBlock = 0;
	LastActorTickMs = -1.0;
	LastReadFrame = 0;
	TWeakObjectPtr<UWorld> WeakWorld = World;
	WorldTickStartHandle = FWorldDelegates::OnWorldTickStart.AddLambda([this, WeakWorld](UWorld* Ticking, ELevelTick, float)
	{
		if (Ticking == WeakWorld.Get())
		{
			WorldTickStartedAt = FPlatformTime::Seconds();
		}
	});
	WorldPostActorTickHandle = FWorldDelegates::OnWorldPostActorTick.AddLambda([this, WeakWorld](UWorld* Ticking, ELevelTick, float)
	{
		if (Ticking == WeakWorld.Get() && WorldTickStartedAt > 0.0)
		{
			LastActorTickMs = (FPlatformTime::Seconds() - WorldTickStartedAt) * 1000.0;
			LastActorTickFrame = GFrameCounter;
		}
	});
	SetVariant(false);
	State = EState::Experimenting;
	UE_LOG(LogOptiCompanion, Log, TEXT("Play: testing Tick Interval %.1f s on %s (%d instances, %.3f ms/frame)."),
		TriedInterval, *Candidate.ClassName, Subjects.Num(), Candidate.MsPerFrame);
}

void FOptiPlayProfiler::SetVariant(bool bVariantB)
{
	for (int32 Index = 0; Index < Subjects.Num(); ++Index)
	{
		if (AActor* Actor = Subjects[Index].Get())
		{
			Actor->SetActorTickInterval(bVariantB ? TriedInterval : OriginalIntervals[Index]);
		}
	}
}

void FOptiPlayProfiler::TickExperiment()
{
	UWorld* World = PlayWorld();
	if (!World)
	{
		EndExperiment(false);
		return;
	}
	if (World->IsPaused())
	{
		return; // paused game: nothing ticks, nothing to measure
	}
	// Blocks alternate A B A B ...: slow drifts (streaming, the editor doing something) hit both variants alike.
	const bool bVariantB = (Block % 2) == 1;
	if (LastActorTickMs < 0.0 || LastActorTickFrame == LastReadFrame)
	{
		return; // no new world tick since the last sample
	}
	LastReadFrame = LastActorTickFrame;
	++FrameInBlock;
	if (FrameInBlock > SettleFrames)
	{
		BlockFrames.Add(LastActorTickMs);
	}
	if (FrameInBlock < SettleFrames + FramesPerBlock)
	{
		return;
	}
	double Sum = 0.0;
	for (double Value : BlockFrames)
	{
		Sum += Value;
	}
	(bVariantB ? BlockMeansB : BlockMeansA).Add(BlockFrames.IsEmpty() ? 0.0 : Sum / BlockFrames.Num());
	BlockFrames.Reset();
	FrameInBlock = 0;
	++Block;
	if (Block >= BlocksPerVariant * 2)
	{
		EndExperiment(true);
		return;
	}
	SetVariant((Block % 2) == 1);
}

void FOptiPlayProfiler::EndExperiment(bool bCompleted)
{
	FWorldDelegates::OnWorldTickStart.Remove(WorldTickStartHandle);
	FWorldDelegates::OnWorldPostActorTick.Remove(WorldPostActorTickHandle);
	WorldTickStartHandle.Reset();
	WorldPostActorTickHandle.Reset();
	SetVariant(false);
	Subjects.Reset();
	State = PlayWorld() ? EState::Waiting : EState::Off;
	if (!bCompleted || BlockMeansA.Num() < 3 || BlockMeansB.Num() < 3)
	{
		UE_LOG(LogOptiCompanion, Log, TEXT("Play: Tick Interval test on %s interrupted."), *Candidate.ClassName);
		return;
	}
	FOptiTickIntervalResult Result;
	Result.Cost = Candidate;
	Result.From = Candidate.TickInterval;
	Result.To = TriedInterval;
	Result.Stat = OptiAnalysis::CompareColumn(TEXT("GameThread"), BlockMeansA, BlockMeansB, 1709);
	UE_LOG(LogOptiCompanion, Log, TEXT("Play: Tick Interval %.1f s on %s: actor ticks %.3f -> %.3f ms (95%% CI of change %.3f .. %.3f)%s"),
		TriedInterval, *Candidate.ClassName, Result.Stat.MeanA, Result.Stat.MeanB, Result.Stat.CILow, Result.Stat.CIHigh,
		Result.Stat.bSignificant ? TEXT(", significant") : TEXT(""));
	if (OnTickIntervalResult)
	{
		OnTickIntervalResult(Result);
	}
}
