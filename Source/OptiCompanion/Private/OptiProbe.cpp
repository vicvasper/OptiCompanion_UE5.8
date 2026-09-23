#include "OptiProbe.h"
#include "OptiAnalysis.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "UnrealClient.h"
#include "HAL/IConsoleManager.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Math/RandomStream.h"
#include "Async/Async.h"
#include "Async/ParallelFor.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Dom/JsonObject.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

DEFINE_LOG_CATEGORY(LogOptiCompanion);

namespace
{
	constexpr int32 MaxFramesWaitingForCapture = 300;
	constexpr int32 CaptureMaxWidth = 320;

	FString ProbeDirectory()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OptiCompanion") / TEXT("Probes"));
	}

	FString DescribeChanges(const FOptiCVarSet& Changes)
	{
		TArray<FString> Parts;
		for (const TPair<FString, FString>& Change : Changes)
		{
			Parts.Add(FString::Printf(TEXT("%s=%s"), *Change.Key, *Change.Value));
		}
		return Parts.IsEmpty() ? TEXT("(current)") : FString::Join(Parts, TEXT(" "));
	}

	TSharedRef<FJsonObject> StatToJson(const FOptiStatResult& Stat)
	{
		TSharedRef<FJsonObject> Json = MakeShared<FJsonObject>();
		Json->SetStringField(TEXT("name"), Stat.Name);
		Json->SetNumberField(TEXT("meanA"), Stat.MeanA);
		Json->SetNumberField(TEXT("meanB"), Stat.MeanB);
		Json->SetNumberField(TEXT("delta"), Stat.Delta);
		Json->SetNumberField(TEXT("ciLow"), Stat.CILow);
		Json->SetNumberField(TEXT("ciHigh"), Stat.CIHigh);
		Json->SetNumberField(TEXT("noiseStdDev"), Stat.NoiseStdDev);
		Json->SetBoolField(TEXT("significant"), Stat.bSignificant);
		return Json;
	}

	FString WriteReport(const FOptiProbeResult& Result)
	{
		TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
		Root->SetStringField(TEXT("label"), Result.Settings.Label);
		Root->SetStringField(TEXT("variantA"), DescribeChanges(Result.Settings.ChangesA));
		Root->SetStringField(TEXT("variantB"), DescribeChanges(Result.Settings.ChangesB));
		Root->SetNumberField(TEXT("blocksPerVariant"), Result.Settings.BlocksPerVariant);
		Root->SetNumberField(TEXT("framesPerBlock"), Result.Settings.FramesPerBlock);
		Root->SetNumberField(TEXT("warmupFrames"), Result.Settings.WarmupFrames);
		Root->SetNumberField(TEXT("seed"), Result.Seed);
		Root->SetNumberField(TEXT("validBlocksA"), Result.ValidBlocksA);
		Root->SetNumberField(TEXT("validBlocksB"), Result.ValidBlocksB);
		Root->SetStringField(TEXT("csv"), Result.CsvPath);

		TArray<TSharedPtr<FJsonValue>> Stats;
		for (const FOptiStatResult& Stat : Result.Stats)
		{
			Stats.Add(MakeShared<FJsonValueObject>(StatToJson(Stat)));
		}
		Root->SetArrayField(TEXT("stats"), Stats);

		FString Text;
		const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
		FJsonSerializer::Serialize(Root, Writer);

		const FString Path = FPaths::GetBaseFilename(Result.CsvPath, false) + TEXT(".json");
		FFileHelper::SaveStringToFile(Text, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		return Path;
	}

	void LogResult(const FOptiProbeResult& Result)
	{
		const FOptiProbeSettings& S = Result.Settings;
		if (S.IsNoiseTest())
		{
			UE_LOG(LogOptiCompanion, Display, TEXT("Noise test (A/A), %d+%d blocks of %d frames"), Result.ValidBlocksA, Result.ValidBlocksB, S.FramesPerBlock);
		}
		else
		{
			UE_LOG(LogOptiCompanion, Display, TEXT("Probe '%s': A=[%s]  B=[%s], %d+%d blocks of %d frames"),
				*S.Label, *DescribeChanges(S.ChangesA), *DescribeChanges(S.ChangesB), Result.ValidBlocksA, Result.ValidBlocksB, S.FramesPerBlock);
		}

		// Timings first, counters after; passes that cost next to nothing only go to the JSON report.
		constexpr double MinVisibleMs = 0.01;
		int32 Hidden = 0;
		for (const bool bCounters : { false, true })
		{
			UE_LOG(LogOptiCompanion, Display, TEXT("  %-34s %11s %11s %11s %26s %9s"),
				bCounters ? TEXT("Counter") : TEXT("Timing (ms)"), TEXT("A"), TEXT("B"), TEXT("B-A"), TEXT("95% CI"), TEXT("noise"));
			for (const FOptiStatResult& Stat : Result.Stats)
			{
				if (OptiAnalysis::IsCounterColumn(Stat.Name) != bCounters)
				{
					continue;
				}
				if (!bCounters && FMath::Max(Stat.MeanA, Stat.MeanB) < MinVisibleMs)
				{
					++Hidden;
					continue;
				}
				UE_LOG(LogOptiCompanion, Display, TEXT("%s %-34s %11.3f %11.3f %+11.3f [%+11.3f, %+11.3f] %9.3f"),
					Stat.bSignificant ? TEXT("*") : TEXT(" "),
					*Stat.Name, Stat.MeanA, Stat.MeanB, Stat.Delta, Stat.CILow, Stat.CIHigh, Stat.NoiseStdDev);
			}
		}
		UE_LOG(LogOptiCompanion, Display, TEXT("  * = the 95%% interval excludes zero. %d passes under %.2f ms hidden. Report: %s"), Hidden, MinVisibleMs, *Result.ReportPath);

		if (S.IsNoiseTest())
		{
			const int32 FalsePositives = Result.Stats.FilterByPredicate([](const FOptiStatResult& Stat) { return Stat.bSignificant; }).Num();
			UE_LOG(LogOptiCompanion, Display, TEXT("  A/A test: %d of %d stats flagged by chance (about 5%% is expected)."), FalsePositives, Result.Stats.Num());
		}
	}
}

FOptiProbeSettings FOptiProbeSettings::ForCVar(const FString& Name, const FString& ValueA, const FString& ValueB)
{
	FOptiProbeSettings Settings;
	Settings.ChangesA.Add({ Name, ValueA });
	Settings.ChangesB.Add({ Name, ValueB });
	Settings.Label = Name;
	return Settings;
}

const FOptiStatResult* FOptiProbeResult::FindStat(const FString& Name) const
{
	return Stats.FindByPredicate([&Name](const FOptiStatResult& Stat) { return Stat.Name == Name; });
}

FOptiProbe::FOptiProbe(const FOptiProbeSettings& InSettings)
	: Settings(InSettings)
{
	Settings.BlocksPerVariant = FMath::Max(Settings.BlocksPerVariant, 2);
	Settings.FramesPerBlock = FMath::Max(Settings.FramesPerBlock, 10);
	Settings.WarmupFrames = FMath::Max(Settings.WarmupFrames, 2);
	if (Settings.Label.IsEmpty())
	{
		Settings.Label = Settings.IsNoiseTest() ? TEXT("Noise") : TEXT("Probe");
	}
}

FOptiProbe::~FOptiProbe()
{
	if (IsRunning())
	{
		Cancel();
	}
}

bool FOptiProbe::Start(FString& OutError)
{
#if CSV_PROFILER_STATS
	if (IsRunning())
	{
		OutError = TEXT("This probe is already running");
		return false;
	}
	if (FCsvProfiler::IsCapturing())
	{
		OutError = TEXT("A CSV capture is already running. Stop it first (CsvProfile Stop)");
		return false;
	}
	for (const FOptiCVarSet* Changes : { &Settings.ChangesA, &Settings.ChangesB })
	{
		for (const TPair<FString, FString>& Change : *Changes)
		{
			if (!IConsoleManager::Get().FindConsoleVariable(*Change.Key))
			{
				OutError = FString::Printf(TEXT("Unknown console variable '%s'"), *Change.Key);
				return false;
			}
		}
	}

	SavedCVars.Reset();
	OverrideCVar(TEXT("r.GPUCsvStatsEnabled"), TEXT("1"));
	OverrideCVar(TEXT("csv.CompressionMode"), TEXT("0"));

	// Every pair of blocks runs AB or BA at random, so slow drift hits both variants equally.
	Seed = static_cast<int32>(FDateTime::Now().GetTicks() & 0x7fffffff);
	FRandomStream Rng(Seed);
	Schedule.Reset();
	for (int32 Pair = 0; Pair < Settings.BlocksPerVariant; ++Pair)
	{
		const bool bFirstIsB = Rng.RandHelper(2) == 1;
		Schedule.Add(bFirstIsB);
		Schedule.Add(!bFirstIsB);
	}

	const FString Directory = ProbeDirectory();
	IFileManager::Get().MakeDirectory(*Directory, true);
	// Dots would be taken as an extension by the CSV profiler (r.ScreenPercentage -> "Probe_r.csv").
	const FString SafeLabel = FPaths::MakeValidFileName(Settings.Label, TEXT('_')).Replace(TEXT("."), TEXT("_"));
	const FString FileName = FString::Printf(TEXT("Probe_%s_%s"), *SafeLabel, *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	FCsvProfiler::Get()->BeginCapture(-1, Directory, FileName);

	BlockIndex = 0;
	FramesWaited = 0;
	CaptureA = FOptiCapture();
	CaptureB = FOptiCapture();
	CaptureA2 = FOptiCapture();
	State = EState::WaitingForCapture;
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FOptiProbe::Tick));

	UE_LOG(LogOptiCompanion, Verbose, TEXT("Probe '%s' started: %d blocks x (%d warmup + %d measured) frames."),
		*Settings.Label, Schedule.Num(), Settings.WarmupFrames, Settings.FramesPerBlock);
	return true;
#else
	OutError = TEXT("The CSV profiler is not available in this build configuration");
	return false;
#endif
}

void FOptiProbe::Pause()
{
#if CSV_PROFILER_STATS
	if (State == EState::Measuring)
	{
		FCsvProfiler::RecordEventf(CSV_CATEGORY_INDEX_GLOBAL, TEXT("%s%d"), OptiAnalysis::AbortEventPrefix, BlockIndex);
	}
#endif
	if (State == EState::Warmup || State == EState::Measuring)
	{
		ApplyVariant(false);
		State = EState::Paused;
	}
}

void FOptiProbe::Resume()
{
	if (State == EState::Paused)
	{
		ApplyVariant(Schedule[BlockIndex]);
		FramesLeft = Settings.WarmupFrames;
		State = EState::Warmup;
	}
}

void FOptiProbe::Cancel()
{
	if (!IsRunning())
	{
		return;
	}
	const bool bWasCapturing = State != EState::WaitingForFile;
#if CSV_PROFILER_STATS
	if (bWasCapturing && FCsvProfiler::IsCapturing())
	{
		FCsvProfiler::Get()->EndCapture();
	}
#endif
	RestoreCVars();
	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	TickerHandle.Reset();
	State = EState::Done;
	UE_LOG(LogOptiCompanion, Verbose, TEXT("Probe '%s' cancelled; console variables restored."), *Settings.Label);

	FOptiProbeResult Result;
	Result.Settings = Settings;
	Result.bCancelled = true;
	OnFinished.ExecuteIfBound(Result);
}

bool FOptiProbe::Tick(float DeltaTime)
{
#if CSV_PROFILER_STATS
	switch (State)
	{
	case EState::WaitingForCapture:
		if (FCsvProfiler::IsCapturing())
		{
			ApplyVariant(Schedule[0]);
			FramesLeft = Settings.WarmupFrames;
			State = EState::Warmup;
		}
		else if (++FramesWaited > MaxFramesWaitingForCapture)
		{
			FOptiProbeResult Result;
			Result.Settings = Settings;
			Result.Error = TEXT("The CSV capture never started");
			RestoreCVars();
			Finish(MoveTemp(Result));
			return false;
		}
		break;

	case EState::Warmup:
		if (--FramesLeft <= 0)
		{
			FCsvProfiler::RecordEventf(CSV_CATEGORY_INDEX_GLOBAL, TEXT("%s%d_%s"), OptiAnalysis::BeginEventPrefix, BlockIndex, Schedule[BlockIndex] ? TEXT("B") : TEXT("A"));
			FramesLeft = Settings.FramesPerBlock;
			State = EState::Measuring;
		}
		break;

	case EState::Measuring:
		if (--FramesLeft <= 0)
		{
			FCsvProfiler::RecordEventf(CSV_CATEGORY_INDEX_GLOBAL, TEXT("%s%d"), OptiAnalysis::EndEventPrefix, BlockIndex);

			// The read-back stalls the GPU, so it happens after the block ends; the next warmup absorbs the hitch.
			if (Settings.bCaptureImages)
			{
				FOptiCapture* Capture = Schedule[BlockIndex] ? &CaptureB : (CaptureA.IsValid() ? &CaptureA2 : &CaptureA);
				if (!Capture->IsValid())
				{
					CaptureViewport(*Capture);
				}
			}

			if (++BlockIndex < Schedule.Num())
			{
				ApplyVariant(Schedule[BlockIndex]);
				FramesLeft = Settings.WarmupFrames;
				State = EState::Warmup;
			}
			else
			{
				CsvFuture = FCsvProfiler::Get()->EndCapture();
				RestoreCVars();
				State = EState::WaitingForFile;
			}
		}
		break;

	case EState::WaitingForFile:
		if (CsvFuture.IsValid() && CsvFuture.IsReady())
		{
			FOptiProbeResult Pending;
			Pending.Settings = Settings;
			Pending.Seed = Seed;
			Pending.CsvPath = CsvFuture.Get();
			Pending.CaptureA = MoveTemp(CaptureA);
			Pending.CaptureB = MoveTemp(CaptureB);
			Pending.CaptureA2 = MoveTemp(CaptureA2);

			// Reading the CSV and bootstrapping every column takes long enough to be felt as a stutter: do it
			// on a worker thread, one task per column, and pick the result up on a later tick.
			AnalysisFuture = Async(EAsyncExecution::ThreadPool, [Result = MoveTemp(Pending)]() mutable
			{
				OptiAnalysis::FBlockMeans Blocks;
				if (OptiAnalysis::ParseProbeCsv(Result.CsvPath, Blocks, Result.Error))
				{
					Result.ValidBlocksA = Blocks.A.Num();
					Result.ValidBlocksB = Blocks.B.Num();
					TArray<int32> Columns;
					for (int32 Column = 1; Column < Blocks.Columns.Num(); ++Column)
					{
						if (OptiAnalysis::IsReportedColumn(Blocks.Columns[Column]))
						{
							Columns.Add(Column);
						}
					}
					TArray<FOptiStatResult> Stats;
					Stats.SetNum(Columns.Num());
					ParallelFor(Columns.Num(), [&Columns, &Blocks, &Stats, Seed = Result.Seed](int32 Index)
					{
						const int32 Column = Columns[Index];
						TArray<double> ColumnA, ColumnB;
						ColumnA.Reserve(Blocks.A.Num());
						ColumnB.Reserve(Blocks.B.Num());
						for (const TArray<double>& Block : Blocks.A) { ColumnA.Add(Block[Column]); }
						for (const TArray<double>& Block : Blocks.B) { ColumnB.Add(Block[Column]); }
						Stats[Index] = OptiAnalysis::CompareColumn(Blocks.Columns[Column], ColumnA, ColumnB, Seed);
					});
					Result.Stats = MoveTemp(Stats);
					Result.Stats.Sort([](const FOptiStatResult& L, const FOptiStatResult& R)
					{
						const bool bLeftCounter = OptiAnalysis::IsCounterColumn(L.Name);
						if (bLeftCounter != OptiAnalysis::IsCounterColumn(R.Name))
						{
							return !bLeftCounter;
						}
						return FMath::Abs(L.Delta) > FMath::Abs(R.Delta);
					});
				}
				return MoveTemp(Result);
			});
			State = EState::Analyzing;
		}
		break;

	case EState::Analyzing:
		if (AnalysisFuture.IsValid() && AnalysisFuture.IsReady())
		{
			FOptiProbeResult Analysed = AnalysisFuture.Get();
			Finish(MoveTemp(Analysed));
			return false;
		}
		break;

	default:
		break;
	}
#endif
	return true;
}

void FOptiProbe::ApplyVariant(bool bVariantB)
{
	for (const TPair<FString, FString>& Change : bVariantB ? Settings.ChangesB : Settings.ChangesA)
	{
		OverrideCVar(Change.Key, Change.Value);
	}
	if (Settings.OnVariant)
	{
		Settings.OnVariant(bVariantB);
	}
}

void FOptiProbe::OverrideCVar(const FString& Name, const FString& Value)
{
	IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Name);
	if (!CVar)
	{
		return;
	}
	if (!SavedCVars.ContainsByPredicate([&Name](const FSavedCVar& Saved) { return Saved.Name == Name; }))
	{
		SavedCVars.Add({ Name, CVar->GetString() });
	}
	CVar->Set(*Value, ECVF_SetByConsole);
}

void FOptiProbe::RestoreCVars()
{
	if (Settings.OnVariant)
	{
		Settings.OnVariant(false); // scene changes always end back at A, even when cancelled
	}
	for (const FSavedCVar& Saved : SavedCVars)
	{
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Saved.Name))
		{
			CVar->Set(*Saved.Value, ECVF_SetByConsole);
		}
	}
	SavedCVars.Reset();
}

void FOptiProbe::CaptureViewport(FOptiCapture& Out)
{
	FViewport* Viewport = nullptr;
	if (Settings.ViewportProvider)
	{
		Viewport = Settings.ViewportProvider();
	}
	else if (GEngine && GEngine->GameViewport)
	{
		Viewport = GEngine->GameViewport->Viewport;
	}
	OptiCaptureViewport(Viewport, Out);
}

bool OptiCaptureViewport(FViewport* Viewport, FOptiCapture& Out)
{
	if (!Viewport || Viewport->GetSizeXY().X <= 0)
	{
		return false;
	}

	TArray<FColor> Full;
	const FIntPoint Size = Viewport->GetSizeXY();
	if (!Viewport->ReadPixels(Full) || Full.Num() < Size.X * Size.Y)
	{
		return false;
	}

	// Box-filter down to a small image: the perceptual diff does not need more and it keeps memory low.
	const int32 Step = FMath::Max(1, FMath::DivideAndRoundUp(Size.X, CaptureMaxWidth));
	Out.Width = Size.X / Step;
	Out.Height = Size.Y / Step;
	Out.Pixels.SetNumUninitialized(Out.Width * Out.Height);
	for (int32 Y = 0; Y < Out.Height; ++Y)
	{
		for (int32 X = 0; X < Out.Width; ++X)
		{
			uint32 R = 0, G = 0, B = 0, N = 0;
			for (int32 SY = 0; SY < Step; ++SY)
			{
				const FColor* Row = &Full[(Y * Step + SY) * Size.X + X * Step];
				for (int32 SX = 0; SX < Step; ++SX)
				{
					R += Row[SX].R; G += Row[SX].G; B += Row[SX].B; ++N;
				}
			}
			Out.Pixels[Y * Out.Width + X] = FColor(R / N, G / N, B / N, 255);
		}
	}
	return true;
}

void FOptiProbe::Finish(FOptiProbeResult&& Result)
{
	TickerHandle.Reset();
	State = EState::Done;

	if (Result.IsValid())
	{
		if (Settings.bKeepFiles)
		{
			Result.ReportPath = WriteReport(Result);
			LogResult(Result);
		}
		else
		{
			IFileManager::Get().Delete(*Result.CsvPath, false, false, true);
		}
	}
	else
	{
		UE_LOG(LogOptiCompanion, Warning, TEXT("Probe '%s' failed: %s"), *Settings.Label, *Result.Error);
	}
	OnFinished.ExecuteIfBound(Result);
}
