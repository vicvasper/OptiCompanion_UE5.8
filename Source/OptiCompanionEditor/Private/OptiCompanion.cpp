#include "OptiCompanion.h"
#include "OptiApply.h"
#include "OptiCompanionModule.h"
#include "OptiCompanionSettings.h"
#include "OptiImageDiff.h"
#include "OptiPhrases.h"
#include "OptiProbe.h"
#include "OptiStyle.h"
#include "SOptiFly.h"
#include "SOptiNotebook.h"

#include "AssetCompilingManager.h"
#include "DesktopPlatformModule.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Interfaces/IMainFrameModule.h"
#include "LevelEditorViewport.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "ShaderCompiler.h"
#include "Selection.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "Widgets/Notifications/SNotificationList.h"

#if PLATFORM_WINDOWS
#include "Windows/AllowWindowsPlatformTypes.h"
#include <windows.h>
#include "Windows/HideWindowsPlatformTypes.h"
#endif

namespace
{
	TSharedPtr<FOptiCompanion> GInstance;

	/**
	 * FPlatformMisc::IsRunningOnBattery() reports true whenever a battery exists, even on AC power,
	 * so the power line is checked directly.
	 */
	bool IsOnBatteryPower()
	{
#if PLATFORM_WINDOWS
		SYSTEM_POWER_STATUS Status;
		return GetSystemPowerStatus(&Status) != 0 && Status.ACLineStatus == 0;
#else
		return FPlatformMisc::IsRunningOnBattery();
#endif
	}

	const UOptiCompanionSettings& Settings() { return *GetDefault<UOptiCompanionSettings>(); }

	constexpr double RecentlyTestedSeconds = 60.0 * 60.0;
	constexpr double SlowFrameMs = 1000.0 / 60.0;
	/** Experiments in a row without a finding before the scene is called clean. */
	constexpr int32 CleanAfterTrials = 12;
	/** Once clean, experiments continue this much less often (your own changes are still measured at once). */
	constexpr double CleanBackoffSeconds = 90.0;
	/** How long the result of the last experiment stays next to the mascot. */
	constexpr double ResultWhisperSeconds = 8.0;
	const FText RealtimeOverrideName = NSLOCTEXT("OptiCompanion", "RealtimeOverride", "OptiCompanion nap");

	FString FormatMs(double Ms)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = Options.MaximumFractionalDigits = Ms >= 10.0 ? 1 : 2;
		return FText::AsNumber(Ms, &Options).ToString();
	}

	/** Sums A or B means of the GPU passes an action targets (or the slowest CPU thread for CPU actions). */
	double TargetTime(const FOptiProbeResult& Result, const FOptiAction& Action, bool bB)
	{
		if (Action.Passes.IsEmpty())
		{
			double Worst = 0.0;
			for (const TCHAR* Name : { TEXT("GameThreadTime"), TEXT("RenderThreadTime") })
			{
				if (const FOptiStatResult* Stat = Result.FindStat(Name))
				{
					Worst = FMath::Max(Worst, bB ? Stat->MeanB : Stat->MeanA);
				}
			}
			return Worst;
		}
		double Sum = 0.0;
		for (const FOptiStatResult& Stat : Result.Stats)
		{
			if (Action.Passes.Contains(FOptiProfile::PassGroupOf(Stat.Name)))
			{
				Sum += bB ? Stat.MeanB : Stat.MeanA;
			}
		}
		return Sum;
	}
}

TSharedPtr<FOptiCompanion> FOptiCompanion::Get()
{
	return GInstance;
}

void FOptiCompanion::Create()
{
	if (!GInstance.IsValid())
	{
		GInstance = MakeShared<FOptiCompanion>();
		GInstance->Startup();
	}
}

void FOptiCompanion::Destroy()
{
	if (GInstance.IsValid())
	{
		GInstance->Shutdown();
		GInstance.Reset();
	}
}

void FOptiCompanion::Startup()
{
	Brain.Initialize(FApp::GetProjectName());
	Notebook.Load(Settings().bShareNotebookWithTeam);

	// "Remind me next session" findings come back now.
	for (const TSharedRef<FOptiFinding>& Finding : Notebook.GetFindings())
	{
		if (Finding->State == EOptiFindingState::Postponed && Finding->Reminder == EOptiReminder::NextSession)
		{
			Finding->Reminder = EOptiReminder::None;
			Notebook.SetState(*Finding, EOptiFindingState::New);
			Queue(Finding->Id);
		}
	}

	NextNapTime = FPlatformTime::Seconds() + Settings().IdleSecondsBeforeNap;
	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FOptiCompanion::Tick));
	PackageSavedHandle = UPackage::PackageSavedWithContextEvent.AddSP(this, &FOptiCompanion::OnPackageSaved);
	EndPIEHandle = FEditorDelegates::EndPIE.AddSP(this, &FOptiCompanion::OnEndPIE);
	BeginPIEHandle = FEditorDelegates::PostPIEStarted.AddSP(this, &FOptiCompanion::OnBeginPIE);
	PlayProfiler.IsAlreadySuggested = [this](const FString& Blueprint)
	{
		return Notebook.GetFindings().ContainsByPredicate([&Blueprint](const TSharedRef<FOptiFinding>& F)
		{
			return F->Blueprint == Blueprint && (F->IsOpen() || F->IsDone() || F->bDontSuggestAgain);
		});
	};
	PlayProfiler.OnTickIntervalResult = [this](const FOptiTickIntervalResult& Result) { OnTickIntervalResult(Result); };
	PlayProfiler.OnCostsUpdated = [this]() { OnChanged.Broadcast(); };
	PlayProfiler.ChooseCandidate = [this](const TArray<FOptiBlueprintCost>& Candidates, UWorld* World) { return ChoosePlayCandidate(Candidates, World); };
	PropertyChangedHandle = FCoreUObjectDelegates::OnObjectPropertyChanged.AddSP(this, &FOptiCompanion::OnPropertyChanged);
	SelectionHandle = USelection::SelectionChangedEvent.AddSP(this, &FOptiCompanion::OnSelectionChanged);
	CVarSinkHandle = IConsoleManager::Get().RegisterConsoleVariableSink_Handle(FConsoleCommandDelegate::CreateSP(this, &FOptiCompanion::OnCVarsChanged));
	SnapshotCVars();
	LastConsolidation = FPlatformTime::Seconds();
	Notebook.OnChanged.AddLambda([WeakThis = AsWeak()]() { if (TSharedPtr<FOptiCompanion> This = WeakThis.Pin()) { This->OnChanged.Broadcast(); } });

	IMainFrameModule& MainFrame = FModuleManager::LoadModuleChecked<IMainFrameModule>(TEXT("MainFrame"));
	if (MainFrame.IsWindowInitialized())
	{
		AttachLayer(MainFrame.GetParentWindow());
	}
	else
	{
		MainFrameHandle = MainFrame.OnMainFrameCreationFinished().AddLambda([WeakThis = AsWeak()](TSharedPtr<SWindow> Window, bool)
		{
			if (TSharedPtr<FOptiCompanion> This = WeakThis.Pin())
			{
				This->AttachLayer(Window);
			}
		});
	}
}

void FOptiCompanion::Shutdown()
{
	if (Probe.IsValid())
	{
		Probe->OnFinished.Unbind();
		Probe->Cancel();
		Probe.Reset();
	}
	Brain.Consolidate();
	Notebook.Save();

	FTSTicker::GetCoreTicker().RemoveTicker(TickerHandle);
	UPackage::PackageSavedWithContextEvent.Remove(PackageSavedHandle);
	FEditorDelegates::EndPIE.Remove(EndPIEHandle);
	FEditorDelegates::PostPIEStarted.Remove(BeginPIEHandle);
	FCoreUObjectDelegates::OnObjectPropertyChanged.Remove(PropertyChangedHandle);
	USelection::SelectionChangedEvent.Remove(SelectionHandle);
	IConsoleManager::Get().UnregisterConsoleVariableSink_Handle(CVarSinkHandle);
	if (FModuleManager::Get().IsModuleLoaded(TEXT("MainFrame")))
	{
		FModuleManager::GetModuleChecked<IMainFrameModule>(TEXT("MainFrame")).OnMainFrameCreationFinished().Remove(MainFrameHandle);
	}
	DetachLayer();
}

void FOptiCompanion::AttachLayer(TSharedPtr<SWindow> Window)
{
	if (!Window.IsValid() || Layer.IsValid())
	{
		return;
	}
	Window->AddOverlaySlot()
	[
		SAssignNew(Layer, SOptiFlyLayer).Companion(AsShared())
	];
	LayerWindow = Window;
	UE_LOG(LogOptiCompanion, Verbose, TEXT("Fly layer attached to '%s'."), *Window->GetTitle().ToString());

	if (!Settings().bIntroShown && Settings().bEnabled)
	{
		UOptiCompanionSettings* Mutable = GetMutableDefault<UOptiCompanionSettings>();
		Mutable->bIntroShown = true;
		Mutable->SaveConfig();
		Layer->ShowMessage(FOptiPhrases::Get().Notice(TEXT("notice.firstrun"), {}));
	}
}

void FOptiCompanion::DetachLayer()
{
	if (TSharedPtr<SWindow> Window = LayerWindow.Pin())
	{
		if (Layer.IsValid())
		{
			Window->RemoveOverlaySlot(Layer.ToSharedRef());
		}
	}
	Layer.Reset();
}

UWorld* FOptiCompanion::EditorWorld() const
{
	return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
}

FViewport* FOptiCompanion::EditorViewport() const
{
	return GCurrentLevelEditingViewportClient ? GCurrentLevelEditingViewportClient->Viewport : nullptr;
}

FOptiView FOptiCompanion::CurrentView() const
{
	FOptiView View;
	if (FLevelEditorViewportClient* Client = GCurrentLevelEditingViewportClient)
	{
		if (Client->IsPerspective() && Client->Viewport && Client->Viewport->GetSizeXY().Y > 0)
		{
			View.Location = Client->GetViewLocation();
			View.Rotation = Client->GetViewRotation();
			View.FOVDegrees = Client->ViewFOV;
			View.AspectRatio = static_cast<float>(Client->Viewport->GetSizeXY().X) / Client->Viewport->GetSizeXY().Y;
			View.bValid = true;
		}
	}
	return View;
}

uint32 FOptiCompanion::ViewHash() const
{
	if (!GCurrentLevelEditingViewportClient || (GEditor && GEditor->PlayWorld))
	{
		return 0;
	}
	const FVector Location = GCurrentLevelEditingViewportClient->GetViewLocation();
	const FRotator Rotation = GCurrentLevelEditingViewportClient->GetViewRotation();
	const FIntPoint Size = GCurrentLevelEditingViewportClient->Viewport ? GCurrentLevelEditingViewportClient->Viewport->GetSizeXY() : FIntPoint::ZeroValue;
	uint32 Hash = GetTypeHash(FIntVector(FMath::RoundToInt32(Location.X), FMath::RoundToInt32(Location.Y), FMath::RoundToInt32(Location.Z)));
	Hash = HashCombine(Hash, GetTypeHash(FIntVector(FMath::RoundToInt32(Rotation.Pitch * 10), FMath::RoundToInt32(Rotation.Yaw * 10), FMath::RoundToInt32(Rotation.Roll * 10))));
	return HashCombine(Hash, GetTypeHash(Size)) | 1u;
}

bool FOptiCompanion::IsBusy() const
{
	return (GShaderCompilingManager && GShaderCompilingManager->IsCompiling()) || FAssetCompilingManager::Get().GetNumRemainingAssets() > 0;
}

FString FOptiCompanion::NapBlocker() const
{
	// Conditions that would make a measurement unreliable. Empty = free to nap.
	const UOptiCompanionSettings& S = Settings();
	if (!GEditor || GEditor->PlayWorld) return TEXT("blocked.pie");
	if (!GCurrentLevelEditingViewportClient || !EditorViewport()) return TEXT("blocked.viewport");
	if (IsBusy()) return TEXT("blocked.compiling");
	if (S.bNapOnlyWhenForeground && !FPlatformApplicationMisc::IsThisApplicationForeground()) return TEXT("blocked.background");
	if (!S.bNapOnBattery && IsOnBatteryPower()) return TEXT("blocked.battery");
	// A manual probe, or someone else's CSV capture (our own experiment captures while napping).
	if (FOptiCompanionModule::Get().IsProbeRunning() || (!IsNapping() && FCsvProfiler::IsCapturing())) return TEXT("blocked.probe");
	return FString();
}

bool FOptiCompanion::IsCameraStill() const
{
	// About StillCameraSeconds at the frame rate the editor is running at right now.
	const double FrameMs = FMath::Max(1.0, Reflex.RecentFrameMs(30));
	const int32 Needed = FMath::Clamp(FMath::RoundToInt32(Settings().StillCameraSeconds * 1000.0 / FrameMs), 10, 590);
	return Reflex.Last().View != 0 && Reflex.StableFrames() >= Needed;
}

bool FOptiCompanion::CanNap(double IdleSeconds, bool& bOutAway)
{
	const UOptiCompanionSettings& S = Settings();
	bOutAway = false;
	if (!S.bEnabled || !(S.bEnableNaps || bForceNap))
	{
		return false;
	}
	const FString Blocker = NapBlocker();
	if (Blocker != LastBlocker)
	{
		LastBlocker = Blocker;
		if (!Blocker.IsEmpty() && !LoggedBlockers.Contains(Blocker))
		{
			LoggedBlockers.Add(Blocker); // once per reason per session; the status bar shows the live one
			UE_LOG(LogOptiCompanion, Log, TEXT("Not napping for now: %s"), *OptiText(*Blocker).ToString());
		}
		OnChanged.Broadcast();
	}
	if (!Blocker.IsEmpty())
	{
		return false;
	}
	bOutAway = IdleSeconds >= S.IdleSecondsBeforeNap;
	if (bForceNap)
	{
		return true; // "Try now": start at once; only moving the camera will stop it
	}
	return FPlatformTime::Seconds() >= NextNapTime && (bOutAway || (S.bExperimentWhileWorking && IsCameraStill()));
}

void FOptiCompanion::ReleaseProbe()
{
	if (Probe.IsValid())
	{
		FinishedProbes.Add(Probe);
		Probe.Reset();
	}
}

bool FOptiCompanion::Tick(float DeltaTime)
{
	FinishedProbes.Reset();
	const UOptiCompanionSettings& S = Settings();
	if (!S.bEnabled)
	{
		if (IsNapping())
		{
			AbortNap();
		}
		return true;
	}

	PlayProfiler.Tick();
	if (bPlaySummaryPending && !PlayProfiler.IsPlaying())
	{
		bPlaySummaryPending = false;
		ShowPlaySummary();
	}

	Reflex.Sample(ViewHash());
	if (S.bEnableSaveReflex)
	{
		if (TOptional<FOptiReflex::FResult> Verdict = Reflex.Poll(IsBusy()))
		{
			HandleVerdict(*Verdict);
		}
	}

	if (!FSlateApplication::IsInitialized())
	{
		return true;
	}
	const double Now = FPlatformTime::Seconds();
	const double LastInteraction = FSlateApplication::Get().GetLastUserInteractionTime();
	const double Idle = Now - LastInteraction;

	bool bAway = false;
	if (IsNapping())
	{
		TickNap(Idle);
	}
	else if (CanNap(Idle, bAway))
	{
		StartNap(bAway);
	}

	// Natural pauses: coming back from a break, or a few seconds of calm while notices are waiting.
	if (bWasIdle && Idle < 1.0)
	{
		OnNaturalPause(EPause::BackFromIdle);
	}
	bWasIdle = Idle >= FMath::Min<double>(S.IdleSecondsBeforeNap, 20.0);
	if (!NoticeQueue.IsEmpty() && Idle >= 4.0 && Idle < 4.0 + DeltaTime * 2.0)
	{
		OnNaturalPause(EPause::BackFromIdle);
	}

	if (Now >= NextFindingCheck)
	{
		NextFindingCheck = Now + 2.0;
		CheckFindings();
	}
	return true;
}

void FOptiCompanion::TickNap(double Idle)
{
	const UOptiCompanionSettings& S = Settings();
	const bool bTouched = FSlateApplication::Get().GetLastUserInteractionTime() > LastInteractionAtNapStart;

	if (Nap == ENap::VisualCheck)
	{
		TickVisualCheck(Idle);
		return;
	}
	// Working does not stop an experiment: only the old "only while I'm away" mode does. Components you
	// might edit are protected by OnSelectionChanged/OnPropertyChanged instead.
	if (bTouched && !S.bExperimentWhileWorking)
	{
		AbortNap();
		return;
	}
	// An experiment belongs to one camera view; moving it makes earlier blocks incomparable.
	if (ViewHash() != NapView)
	{
		AbortNap();
		return;
	}
	if (!Probe.IsValid())
	{
		return;
	}
	const bool bBlocked = !NapBlocker().IsEmpty();
	if (bBlocked && !Probe->IsPaused())
	{
		Probe->Pause();
		LastStatus = OptiText(TEXT("status.paused"));
		OnChanged.Broadcast();
	}
	else if (!bBlocked && Probe->IsPaused())
	{
		Probe->Resume();
		LastStatus = FText::GetEmpty();
		OnChanged.Broadcast();
	}
}

void FOptiCompanion::StartNap(bool bAway)
{
	bForceNap = false;
	bNapAway = bAway;
	Nap = ENap::Sniffing;
	NapView = ViewHash();
	LastInteractionAtNapStart = FSlateApplication::Get().GetLastUserInteractionTime();
	GCurrentLevelEditingViewportClient->AddRealtimeOverride(true, RealtimeOverrideName);
	SetMood(bAway ? EOptiFlyMood::Sleeping : EOptiFlyMood::Sniffing);
	LastStatus = OptiText(TEXT("status.sniffing"));

	FOptiProbeSettings Sniff;
	Sniff.Label = TEXT("Sniff");
	Sniff.BlocksPerVariant = 2;
	Sniff.FramesPerBlock = 30;
	Sniff.WarmupFrames = 10;
	Sniff.bKeepFiles = false;
	Probe = MakeShared<FOptiProbe>(Sniff);
	Probe->OnFinished.BindSP(this, &FOptiCompanion::OnSniffed);
	FString Error;
	if (!Probe->Start(Error))
	{
		UE_LOG(LogOptiCompanion, Verbose, TEXT("Nap skipped: %s"), *Error);
		Probe.Reset();
		EndNap(false);
	}
	OnChanged.Broadcast();
}

TSet<FName> FOptiCompanion::BlockedActions() const
{
	TSet<FName> Blocked;
	const FString Context = NapSmell.ContextKey;
	const double Now = FPlatformTime::Seconds();
	for (const TPair<FString, double>& Tested : RecentlyTested)
	{
		FString Action, TestedContext;
		if (Tested.Key.Split(TEXT("|"), &Action, &TestedContext) && TestedContext == Context && Now - Tested.Value < RecentlyTestedSeconds)
		{
			Blocked.Add(FName(*Action));
		}
	}
	for (const TSharedRef<FOptiFinding>& Finding : Notebook.GetFindings())
	{
		if (Finding->Kind != EOptiFindingKind::Optimization)
		{
			continue;
		}
		if (Finding->IsOpen() || Finding->bDontSuggestAgain)
		{
			Blocked.Add(Finding->ActionId);
		}
	}
	return Blocked;
}

void FOptiCompanion::OnSniffed(const FOptiProbeResult& Result)
{
	if (!Result.IsValid())
	{
		EndNap(false);
		return;
	}

	const FOptiProfile Profile = FOptiProfile::FromProbe(Result);
	// Smell what the camera sees: the same action is worth more or less depending on what is on screen.
	NapSmell = FOptiSmell::Sniff(Profile, EditorWorld(), CurrentView());
	bHasSmell = true;

	// While you work only changes the brain expects to be invisible are tried; the riskier ones wait for a break.
	UWorld* SceneWorld = EditorWorld();
	const float MaxVisual = bNapAway ? FLT_MAX : 0.5f;

	// Stale findings are re-measured first, so nothing is ever applied with old numbers.
	NapRecheck.Reset();
	NapDecision = FOptiFlyBrain::FDecision();
	for (const TSharedRef<FOptiFinding>& Finding : Notebook.GetFindings())
	{
		if (Finding->State == EOptiFindingState::Stale && Finding->Kind == EOptiFindingKind::Optimization)
		{
			NapDecision = Brain.DecideFor(NapSmell, OptiActions::IndexOf(Finding->ActionId), SceneWorld);
			if (NapDecision.IsValid())
			{
				NapRecheck = Finding;
				break;
			}
		}
	}
	if (!NapDecision.IsValid())
	{
		static const EOptiSelector Selectors[] = { EOptiSelector::Fly, EOptiSelector::Thompson, EOptiSelector::Random };
		NapDecision = Brain.Decide(NapSmell, Selectors[static_cast<int32>(Settings().Selector)], BlockedActions(), SceneWorld, MaxVisual);
	}
	if (!NapDecision.IsValid())
	{
		UE_LOG(LogOptiCompanion, Verbose, TEXT("Nap: nothing worth testing here right now."));
		EndNap(false);
		if (DryContext == NapSmell.ContextKey && DryStreak > 0)
		{
			DeclareClean(DryStreak);
		}
		return;
	}

	if (DryContext != NapSmell.ContextKey)
	{
		DryContext = NapSmell.ContextKey;
		DryStreak = 0;
		CleanContext.Reset();
	}

	const FOptiAction& Action = NapDecision.Action();
	UE_LOG(LogOptiCompanion, Log, TEXT("%s: testing %s (focus %s, expected %.2f ms of %.2f ms, uncertainty %.2f, novelty %.2f)"),
		bNapAway ? TEXT("Nap") : TEXT("While you work"), *Action.Id.ToString(), LexToString(NapDecision.Focus),
		NapDecision.Prediction.ExpectedMs, NapDecision.Prediction.TargetMs, NapDecision.Prediction.Uncertainty, NapDecision.Novelty);

	Nap = ENap::Experimenting;
	LastStatus = FOptiPhrases::Get().Text(TEXT("status.experimenting"), { { TEXT("action"), FOptiPhrases::Get().Action(Action.Id) } });

	FOptiProbeSettings Experiment;
	Experiment.Label = Action.Id.ToString();
	Experiment.ChangesA = NapDecision.VariantA;
	Experiment.ChangesB = NapDecision.VariantB;
	// Frames are noisier while you work in other panels, so the experiment takes a few more blocks.
	Experiment.BlocksPerVariant = bNapAway ? 4 : 6;
	Experiment.FramesPerBlock = 40;
	Experiment.WarmupFrames = 12;
	Experiment.bKeepFiles = false;
	// Scene actions: the components are switched in memory between blocks and always end back at A.
	NapSceneEdits.Reset();
	if (Action.IsSceneAction())
	{
		NapSceneEdits = MakeShared<TArray<FOptiSceneEdit>>(OptiScene::Collect(Action, EditorWorld()));
		Experiment.OnVariant = [Edits = NapSceneEdits](bool bVariantB) { OptiScene::SetVariant(*Edits, bVariantB); };
		UE_LOG(LogOptiCompanion, Log, TEXT("Nap: %s touches %d component properties."), *Action.Id.ToString(), NapSceneEdits->Num());
	}
	ReleaseProbe();
	Probe = MakeShared<FOptiProbe>(Experiment);
	Probe->OnFinished.BindSP(this, &FOptiCompanion::OnExperimented);
	FString Error;
	if (!Probe->Start(Error))
	{
		UE_LOG(LogOptiCompanion, Warning, TEXT("Nap experiment could not start: %s"), *Error);
		Probe.Reset();
		EndNap(false);
	}
	OnChanged.Broadcast();
}

void FOptiCompanion::OnExperimented(const FOptiProbeResult& Result)
{
	if (!Result.IsValid() || !NapDecision.IsValid())
	{
		EndNap(false);
		return;
	}

	const UOptiCompanionSettings& S = Settings();
	const FOptiAction& Action = NapDecision.Action();
	const double TargetA = TargetTime(Result, Action, false);
	const double TargetB = TargetTime(Result, Action, true);
	const float GainFraction = TargetA > 0.001 ? static_cast<float>((TargetA - TargetB) / TargetA) : 0.f;

	// The timing is learned now; how it looks is learned after the visual check (if it is worth one).
	Brain.LearnFromExperiment(NapDecision, GainFraction, -1.f);
	RecentlyTested.Add(Action.Id.ToString() + TEXT("|") + NapSmell.ContextKey, FPlatformTime::Seconds());

	const FString Metric = Action.Passes.IsEmpty() ? TEXT("FrameTime") : TEXT("GPUTime");
	const FOptiStatResult* Stat = Result.FindStat(Metric);
	const double GainMs = Stat ? -Stat->Delta : 0.0;
	const bool bPromising = Stat && Stat->bSignificant && GainMs >= FMath::Max<double>(S.MinGainMs, Stat->MeanA * S.MinGainPercent / 100.0);

	UE_LOG(LogOptiCompanion, Log, TEXT("Result %s: target %.3f -> %.3f ms (%.0f%% saved, predicted %.0f%%)%s"),
		*Action.Id.ToString(), TargetA, TargetB, GainFraction * 100.f, NapDecision.Prediction.GainFraction * 100.f,
		bPromising ? TEXT(", promising: visual check at the next short pause") : TEXT(""));

	if (!bPromising)
	{
		LogDecision(NapDecision, GainFraction, -1.f);
		if (NapRecheck.IsValid())
		{
			NapRecheck->Note = TEXT("No longer saves time after re-measuring");
			Notebook.SetState(*NapRecheck, EOptiFindingState::Dismissed);
		}
		RecordTrial(Stat && Stat->bSignificant && GainMs > 0.0 ? EOptiTrialOutcome::TooSmall : EOptiTrialOutcome::NoGain, &Result, -1.f);
		EndNap(true);
		return;
	}

	PendingResult = MakeShared<FOptiProbeResult>(Result);
	Nap = ENap::VisualCheck;
	VisualStep = 0;
	VisualDeadline = FPlatformTime::Seconds() + 600.0;
	LastStatus = OptiText(TEXT("status.visual_check"));
	OnChanged.Broadcast();
}

void FOptiCompanion::SetVariantNow(bool bVariantB)
{
	bOwnChange = true;
	for (const TPair<FString, FString>& Change : bVariantB ? NapDecision.VariantB : NapDecision.VariantA)
	{
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Change.Key))
		{
			CVar->Set(*Change.Value, ECVF_SetByConsole);
		}
	}
	if (NapSceneEdits.IsValid())
	{
		OptiScene::SetVariant(*NapSceneEdits, bVariantB);
	}
	bOwnChange = false;
}

void FOptiCompanion::TickVisualCheck(double Idle)
{
	// Three frame grabs (A, B, A again) take a fraction of a second and stall the GPU briefly, so they wait
	// for a short pause with the camera still. If you touch anything halfway, A is restored and it retries.
	constexpr double PauseNeeded = 0.5;
	constexpr int32 SettleFrames = 10;
	const bool bCalm = Idle >= PauseNeeded && Reflex.StableFrames() >= 20 && NapBlocker().IsEmpty();

	if (FPlatformTime::Seconds() > VisualDeadline)
	{
		LogDecision(NapDecision, -1.f, -1.f);
		RecordTrial(EOptiTrialOutcome::Dropped, PendingResult.Get(), -1.f);
		EndNap(true);
		return;
	}
	if (VisualStep > 0 && !bCalm)
	{
		SetVariantNow(false);
		VisualStep = 0;
		return;
	}

	switch (VisualStep)
	{
	case 0:
		if (bCalm)
		{
			SetVariantNow(false);
			OptiCaptureViewport(EditorViewport(), CaptureA);
			SetVariantNow(true);
			VisualFrames = SettleFrames;
			VisualStep = 1;
		}
		break;
	case 1:
		if (--VisualFrames <= 0)
		{
			OptiCaptureViewport(EditorViewport(), CaptureB);
			SetVariantNow(false);
			VisualFrames = SettleFrames;
			VisualStep = 2;
		}
		break;
	case 2:
		if (--VisualFrames <= 0)
		{
			OptiCaptureViewport(EditorViewport(), CaptureA2);

			// Only the difference beyond what two frames of A already differ by counts (TSR jitter, animated sky...).
			FOptiImageDiff Diff = OptiImage::Compare(CaptureA, CaptureB);
			const FOptiImageDiff Noise = OptiImage::Compare(CaptureA, CaptureA2);
			if (Diff.IsValid() && Noise.IsValid())
			{
				Diff.Mean = FMath::Max(0.f, Diff.Mean - Noise.Mean);
				Diff.P95 = FMath::Max(0.f, Diff.P95 - Noise.P95);
			}
			const UOptiCompanionSettings& S = Settings();
			const float Visual = Diff.IsValid() ? FMath::Max(Diff.Mean / S.VisualThresholdMean, Diff.P95 / S.VisualThresholdP95) : -1.f;
			if (Visual >= 0.f)
			{
				Brain.LearnVisual(NapDecision, Visual);
			}
			const FOptiAction& Action = NapDecision.Action();
			const double TargetA = TargetTime(*PendingResult, Action, false);
			const double TargetB = TargetTime(*PendingResult, Action, true);
			const float GainFraction = TargetA > 0.001 ? static_cast<float>((TargetA - TargetB) / TargetA) : 0.f;
			LogDecision(NapDecision, GainFraction, Visual);
			UE_LOG(LogOptiCompanion, Log, TEXT("Visual check %s: %.2f (mean %.4f, p95 %.3f)"), *Action.Id.ToString(), Visual, Diff.Mean, Diff.P95);

			PendingResult->CaptureA = CaptureA;
			PendingResult->CaptureB = CaptureB;
			const bool bFound = RecordFinding(*PendingResult, Diff);
			const FOptiStatResult* Stat = PendingResult->FindStat(Action.Passes.IsEmpty() ? TEXT("FrameTime") : TEXT("GPUTime"));
			const bool bVisible = !Diff.IsValid() || Diff.Mean > S.VisualThresholdMean || Diff.P95 > S.VisualThresholdP95;
			RecordTrial(bFound ? EOptiTrialOutcome::Finding : bVisible ? EOptiTrialOutcome::Visible
				: (Stat && Stat->bSignificant && Stat->Delta < 0.0 ? EOptiTrialOutcome::TooSmall : EOptiTrialOutcome::NoGain), PendingResult.Get(), Visual);
			EndNap(true);
		}
		break;
	}
}

bool FOptiCompanion::RecordFinding(const FOptiProbeResult& Result, const FOptiImageDiff& Diff)
{
	const UOptiCompanionSettings& S = Settings();
	const FOptiAction& Action = NapDecision.Action();
	// GPU actions are judged on GPU time: the editor is often CPU-bound, but the shipped game rarely is the same way.
	const FString Metric = Action.Passes.IsEmpty() ? TEXT("FrameTime") : TEXT("GPUTime");
	const FOptiStatResult* Stat = Result.FindStat(Metric);

	const bool bVisible = !Diff.IsValid() || Diff.Mean > S.VisualThresholdMean || Diff.P95 > S.VisualThresholdP95;
	const double GainMs = Stat ? -Stat->Delta : 0.0;
	const bool bWorthIt = Stat && Stat->bSignificant && GainMs >= FMath::Max<double>(S.MinGainMs, Stat->MeanA * S.MinGainPercent / 100.0) && !bVisible;

	if (!bWorthIt)
	{
		if (NapRecheck.IsValid())
		{
			NapRecheck->Note = TEXT("No longer saves time after re-measuring");
			Notebook.SetState(*NapRecheck, EOptiFindingState::Dismissed);
		}
		return false;
	}

	FOptiFinding Finding;
	if (NapRecheck.IsValid())
	{
		Finding = *NapRecheck;
	}
	Finding.Kind = EOptiFindingKind::Optimization;
	Finding.ActionId = Action.Id;
	Finding.From = NapDecision.VariantA;
	Finding.To = NapDecision.VariantB;
	Finding.SceneEdits = NapSceneEdits.IsValid() ? OptiScene::Serialize(*NapSceneEdits) : FString();
	Finding.Metric = Metric;
	Finding.GainMs = GainMs;
	Finding.CILowMs = -Stat->CIHigh;
	Finding.CIHighMs = -Stat->CILow;
	Finding.BaselineMs = Stat->MeanA;
	Finding.ContextKey = NapSmell.ContextKey;
	Finding.ActiveKenyonCells = NapDecision.ActiveKenyonCells;
	Finding.VisualMean = Diff.Mean;
	Finding.VisualP95 = Diff.P95;
	Finding.TopPasses.Reset();
	for (const FOptiStatResult& Pass : Result.Stats)
	{
		if (Pass.Name.StartsWith(TEXT("GPU/")) && Pass.Delta < 0.0 && Pass.bSignificant && Finding.TopPasses.Num() < 3)
		{
			Finding.TopPasses.Add({ Pass.Name.RightChop(4), Pass.Delta });
		}
	}
	Finding.TopPasses.Sort([](const FOptiPassDelta& L, const FOptiPassDelta& R) { return L.DeltaMs < R.DeltaMs; });

	if (!Finding.Id.IsValid())
	{
		Finding.Id = FGuid::NewGuid();
	}
	const FString Directory = Notebook.CaptureDirectory();
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Base = Directory / Finding.Id.ToString(EGuidFormats::Digits);
	if (Result.CaptureA.IsValid() && Result.CaptureB.IsValid())
	{
		Finding.CaptureA = Base + TEXT("_A.png");
		Finding.CaptureB = Base + TEXT("_B.png");
		OptiImage::SavePng(Finding.CaptureA, Result.CaptureA.Width, Result.CaptureA.Height, Result.CaptureA.Pixels);
		OptiImage::SavePng(Finding.CaptureB, Result.CaptureB.Width, Result.CaptureB.Height, Result.CaptureB.Pixels);
		if (Diff.IsValid())
		{
			Finding.CaptureDiff = Base + TEXT("_Diff.png");
			OptiImage::SavePng(Finding.CaptureDiff, Diff.Width, Diff.Height, OptiImage::Heatmap(Diff));
		}
	}

	if (NapRecheck.IsValid())
	{
		*NapRecheck = Finding;
		Notebook.SetState(*NapRecheck, EOptiFindingState::New);
		Queue(NapRecheck->Id);
	}
	else
	{
		Queue(Notebook.Add(Finding)->Id);
	}
	SetMood(EOptiFlyMood::Rubbing);
	return true;
}

void FOptiCompanion::RecordTrial(EOptiTrialOutcome Outcome, const FOptiProbeResult* Result, float Visual)
{
	if (!NapDecision.IsValid())
	{
		return;
	}
	const FOptiAction& Action = NapDecision.Action();
	FOptiTrial Trial;
	Trial.Time = FDateTime::Now();
	Trial.ActionId = Action.Id;
	Trial.Metric = Action.Passes.IsEmpty() ? TEXT("FrameTime") : TEXT("GPUTime");
	if (const FOptiStatResult* Stat = Result ? Result->FindStat(Trial.Metric) : nullptr)
	{
		Trial.BaselineMs = Stat->MeanA;
		Trial.GainMs = -Stat->Delta;
		Trial.bSignificant = Stat->bSignificant;
	}
	Trial.Visual = Visual;
	Trial.Outcome = Outcome;
	Trial.bWhileWorking = !bNapAway;
	Trial.ContextKey = NapSmell.ContextKey;
	Notebook.AddTrial(Trial);

	LastResultText = DescribeTrial(Trial);
	LastResultAt = FPlatformTime::Seconds();

	if (Outcome == EOptiTrialOutcome::Finding)
	{
		DryStreak = 0;
		CleanContext.Reset();
	}
	else if (Outcome != EOptiTrialOutcome::Dropped && ++DryStreak >= CleanAfterTrials)
	{
		DeclareClean(DryStreak);
	}
}

void FOptiCompanion::DeclareClean(int32 TrialsWithoutFinding)
{
	NextNapTime = FMath::Max(NextNapTime, FPlatformTime::Seconds() + CleanBackoffSeconds);
	if (CleanContext == DryContext)
	{
		return; // already said for this level
	}
	CleanContext = DryContext;
	UE_LOG(LogOptiCompanion, Log, TEXT("Scene looks clean: %d experiments in a row without a finding in %s."), TrialsWithoutFinding, *DryContext);

	FFormatNamedArguments Args;
	Args.Add(TEXT("n"), TrialsWithoutFinding);
	const FText Message = FOptiPhrases::Get().Notice(TEXT("notice.clean"), Args);
	if (!Settings().bDoNotDisturb)
	{
		if (Settings().Presence == EOptiPresence::Mascot && Layer.IsValid())
		{
			Layer->ShowMessage(Message);
		}
		else
		{
			FNotificationInfo Info(Message);
			Info.ExpireDuration = 8.f;
			Info.Image = FOptiStyle::Get().GetBrush("Opti.Fly.Icon.Large");
			FSlateNotificationManager::Get().AddNotification(Info);
		}
	}
	OnChanged.Broadcast();
}

FText FOptiCompanion::DescribeTrial(const FOptiTrial& Trial)
{
	FFormatNamedArguments Args;
	Args.Add(TEXT("action"), FOptiPhrases::Get().Action(Trial.ActionId));
	Args.Add(TEXT("ms"), FText::FromString(FormatMs(FMath::Abs(Trial.GainMs))));
	return FOptiPhrases::Get().Text(FString(TEXT("trial.")) + LexToString(Trial.Outcome), Args);
}

void FOptiCompanion::EndNap(bool bLearned)
{
	if (GCurrentLevelEditingViewportClient)
	{
		GCurrentLevelEditingViewportClient->RemoveRealtimeOverride(RealtimeOverrideName, false);
	}
	Nap = ENap::None;
	ReleaseProbe();
	NapRecheck.Reset();
	NapSceneEdits.Reset();
	PendingResult.Reset();
	CaptureA = CaptureB = CaptureA2 = FOptiCapture();
	NextNapTime = FPlatformTime::Seconds() + Settings().SecondsBetweenNaps;
	LastStatus = FText::GetEmpty();

	if (bLearned)
	{
		Brain.NoteNap();
		SnapshotCVars();
		// Sleep consolidation moves part of today's memory to the long-term brain and saves it; with experiments
		// running while you work that would mean a disk write every few seconds, so it happens every few minutes.
		const double Now = FPlatformTime::Seconds();
		if (Now - LastConsolidation > 300.0)
		{
			LastConsolidation = Now;
			Brain.Consolidate();
		}
	}
	if (Mood == EOptiFlyMood::Sleeping || Mood == EOptiFlyMood::Sniffing)
	{
		SetMood(Notebook.CountNew() > 0 ? EOptiFlyMood::Rubbing : EOptiFlyMood::Grooming);
	}
	OnChanged.Broadcast();
}

void FOptiCompanion::AbortNap()
{
	if (Nap == ENap::VisualCheck && VisualStep > 0)
	{
		SetVariantNow(false);
	}
	if (Probe.IsValid())
	{
		TSharedPtr<FOptiProbe> Running = Probe;
		Running->OnFinished.Unbind();
		Running->Cancel();
	}
	if (Nap == ENap::Experimenting || Nap == ENap::VisualCheck)
	{
		RecordTrial(EOptiTrialOutcome::Dropped, PendingResult.Get(), -1.f);
	}
	EndNap(false);
	SnapshotCVars();
	bJustWoke = true;
	LastStatus = OptiText(TEXT("status.woke"));
	OnChanged.Broadcast();
}

// ---------------------------------------------------------------------------------------------- natural experiments

void FOptiCompanion::SnapshotCVars()
{
	CVarSnapshot.Reset();
	for (const FOptiAction& Action : OptiActions::Catalog())
	{
		for (const FOptiCVarChange& Change : Action.Changes)
		{
			FString RealName;
			if (IConsoleVariable* CVar = OptiFindCVar(Change.Name, &RealName))
			{
				CVarSnapshot.Add(RealName, CVar->GetString());
			}
		}
	}
}

void FOptiCompanion::OnCVarsChanged()
{
	// Our own experiments change CVars all the time; only your changes are natural experiments.
	if (bOwnChange || FOptiCompanionModule::Get().IsProbeRunning())
	{
		return;
	}
	// While an experiment runs, the CVars it is switching are its own; any other one is you, and wins.
	TSet<FString> ExperimentKeys;
	if ((Nap == ENap::Experimenting || Nap == ENap::VisualCheck) && NapDecision.IsValid())
	{
		for (const TPair<FString, FString>& Change : NapDecision.VariantA) { ExperimentKeys.Add(Change.Key.ToLower()); }
		for (const TPair<FString, FString>& Change : NapDecision.VariantB) { ExperimentKeys.Add(Change.Key.ToLower()); }
	}
	for (TPair<FString, FString>& Entry : CVarSnapshot)
	{
		IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Entry.Key);
		if (!CVar || CVar->GetString() == Entry.Value || ExperimentKeys.Contains(Entry.Key.ToLower()))
		{
			continue;
		}
		const FString Label = FString::Printf(TEXT("%s %s -> %s"), *Entry.Key, *Entry.Value, *CVar->GetString());
		Entry.Value = CVar->GetString();
		const TArray<FOptiAction>& Catalog = OptiActions::Catalog();
		for (int32 Index = 0; Index < Catalog.Num(); ++Index)
		{
			if (Catalog[Index].Changes.ContainsByPredicate([&Entry](const FOptiCVarChange& Change) { return Change.Name.Equals(Entry.Key, ESearchCase::IgnoreCase); }))
			{
				OnNaturalChange(Index, Label);
				break;
			}
		}
	}
}

bool FOptiCompanion::ReleaseSceneEditsOf(const UObject* Object)
{
	if (!IsNapping() || !NapSceneEdits.IsValid() || !Object)
	{
		return false;
	}
	const AActor* Actor = Cast<AActor>(Object);
	const UActorComponent* Component = Cast<UActorComponent>(Object);
	bool bTouches = false;
	for (const FOptiSceneEdit& Edit : *NapSceneEdits)
	{
		const UActorComponent* Edited = Edit.Component.Get();
		bTouches |= Edited && (Edited == Component || (Actor && Edited->GetOwner() == Actor));
	}
	if (bTouches)
	{
		// Put everything back to how you left it before you edit it, then give up this experiment.
		UE_LOG(LogOptiCompanion, Log, TEXT("You selected something the experiment was changing: restored it and stopped the experiment."));
		AbortNap();
	}
	return bTouches;
}

void FOptiCompanion::OnSelectionChanged(UObject* Selection)
{
	if (!IsNapping() || !NapSceneEdits.IsValid() || !GEditor)
	{
		return;
	}
	for (FSelectionIterator It(GEditor->GetSelectedActorIterator()); It; ++It)
	{
		if (ReleaseSceneEditsOf(*It))
		{
			return;
		}
	}
	for (FSelectionIterator It(GEditor->GetSelectedComponentIterator()); It; ++It)
	{
		if (ReleaseSceneEditsOf(*It))
		{
			return;
		}
	}
}

void FOptiCompanion::OnPropertyChanged(UObject* Object, FPropertyChangedEvent& Event)
{
	if (ReleaseSceneEditsOf(Object))
	{
		return;
	}
	if (bOwnChange || Event.ChangeType == EPropertyChangeType::Interactive || !Object || !Settings().bEnabled)
	{
		return;
	}
	const UActorComponent* Component = Cast<UActorComponent>(Object);
	if (!Component || !Component->GetWorld() || Component->GetWorld() != EditorWorld())
	{
		return;
	}
	const FName Property = Event.GetPropertyName();
	const TArray<FOptiAction>& Catalog = OptiActions::Catalog();
	for (int32 Index = 0; Index < Catalog.Num(); ++Index)
	{
		for (const FOptiSceneRule& Rule : Catalog[Index].SceneRules)
		{
			if (Rule.Property == Property && Component->IsA(Rule.ComponentClass))
			{
				const AActor* Owner = Component->GetOwner();
				OnNaturalChange(Index, FString::Printf(TEXT("%s on %s"), *Property.ToString(), Owner ? *Owner->GetActorLabel() : *Component->GetName()));
				return;
			}
		}
	}
}

void FOptiCompanion::OnNaturalChange(int32 ActionIndex, const FString& Label)
{
	// You changed something yourself: there may be new things worth testing.
	DryStreak = 0;
	CleanContext.Reset();
	if (!Settings().bEnableSaveReflex)
	{
		return;
	}
	// If an experiment is running, your change would contaminate it: drop the experiment instead.
	if (IsNapping())
	{
		AbortNap();
	}
	PendingNatural = { ActionIndex, Label };
	Reflex.OnChange(Label);
	UE_LOG(LogOptiCompanion, Verbose, TEXT("Watching your change: %s"), *Label);
}

void FOptiCompanion::HandleVerdict(const FOptiReflex::FResult& Verdict)
{
	const FNaturalChange Natural = PendingNatural;
	PendingNatural = FNaturalChange();

	// Natural experiment: learn from what you just did, whatever the outcome.
	if (Natural.ActionIndex != INDEX_NONE && bHasSmell)
	{
		const FOptiAction& Action = OptiActions::Catalog()[Natural.ActionIndex];
		const double TargetMs = FMath::Max(Action.TargetMs(NapSmell.Profile), 0.05);
		const float GainFraction = static_cast<float>(-Verdict.DeltaMs() / TargetMs);
		Brain.LearnNatural(Natural.ActionIndex, NapSmell, GainFraction);
		UE_LOG(LogOptiCompanion, Log, TEXT("Natural experiment %s (%s): %s %.2f -> %.2f ms"),
			*Action.Id.ToString(), *Natural.Label, *Verdict.Metric, Verdict.BeforeMs, Verdict.AfterMs);
	}

	if (Verdict.bImprovement && Natural.ActionIndex != INDEX_NONE && -Verdict.DeltaMs() >= Settings().MinGainMs)
	{
		// You just made it faster: say so now, it is the moment it is useful.
		FFormatNamedArguments Args;
		Args.Add(TEXT("change"), FText::FromString(Natural.Label));
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = Options.MaximumFractionalDigits = 2;
		Args.Add(TEXT("ms"), FText::AsNumber(-Verdict.DeltaMs(), &Options));
		Args.Add(TEXT("metric"), FText::FromString(Verdict.Metric == TEXT("GPUTime") ? TEXT("GPU") : TEXT("frame")));
		const FText Message = FOptiPhrases::Get().Notice(TEXT("notice.natural_gain"), Args);
		if (!Settings().bDoNotDisturb)
		{
			if (Settings().Presence == EOptiPresence::Mascot && Layer.IsValid())
			{
				Layer->ShowMessage(Message);
			}
			else
			{
				FNotificationInfo Info(Message);
				Info.ExpireDuration = 6.f;
				Info.Image = FOptiStyle::Get().GetBrush("Opti.Fly.Icon.Large");
				FSlateNotificationManager::Get().AddNotification(Info);
			}
		}
		SetMood(EOptiFlyMood::Rubbing);
		return;
	}

	if (Verdict.bRegression)
	{
		FOptiFinding Finding;
		Finding.Kind = EOptiFindingKind::Regression;
		Finding.Asset = FString::Join(Verdict.Assets, TEXT(", "));
		Finding.Metric = Verdict.Metric;
		Finding.GainMs = Verdict.DeltaMs();
		Finding.BaselineMs = Verdict.BeforeMs;
		Finding.CILowMs = Verdict.DeltaMs() - Verdict.NoiseMs;
		Finding.CIHighMs = Verdict.DeltaMs() + Verdict.NoiseMs;
		Finding.ContextKey = FOptiSmell::ContextKeyFor(EditorWorld());
		const TSharedRef<FOptiFinding> Added = Notebook.Add(Finding);
		SetMood(EOptiFlyMood::Buzzing);
		++PointAtViewportRequest;
		// Direct presence: you just caused it, so this is the moment to say it.
		Queue(Added->Id, true);
		ShowNextNotice(true);
	}
}

void FOptiCompanion::LogDecision(const FOptiFlyBrain::FDecision& Decision, float MeasuredGain, float Visual) const
{
	// One row per experiment, to compare the fly with the baselines on real projects.
	const FString Path = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OptiCompanion") / TEXT("Decisions.csv"));
	const bool bNew = !FPaths::FileExists(Path);
	FString Line;
	if (bNew)
	{
		Line += TEXT("time,selector,action,focus,target_ms,predicted_gain,innate_gain,measured_gain,visual,uncertainty,novelty\n");
	}
	static const TCHAR* SelectorNames[] = { TEXT("fly"), TEXT("thompson"), TEXT("random") };
	Line += FString::Printf(TEXT("%s,%s,%s,%s,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n"),
		*FDateTime::Now().ToIso8601(), SelectorNames[static_cast<int32>(Decision.Selector)], *Decision.Action().Id.ToString(),
		LexToString(Decision.Focus), Decision.Prediction.TargetMs, Decision.Prediction.GainFraction, Decision.Prediction.InnateGain,
		MeasuredGain, Visual, Decision.Prediction.Uncertainty, Decision.Novelty);
	FFileHelper::SaveStringToFile(Line, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM, &IFileManager::Get(), FILEWRITE_Append);
}

void FOptiCompanion::CheckFindings()
{
	const FString Context = FOptiSmell::ContextKeyFor(EditorWorld());
	const FDateTime Now = FDateTime::Now();
	for (const TSharedRef<FOptiFinding>& Finding : Notebook.GetFindings())
	{
		if (Finding->Kind != EOptiFindingKind::Optimization || !Finding->IsOpen())
		{
			continue;
		}
		if (Finding->IsBlueprintFinding())
		{
			if (OptiApply::BlueprintMatches(*Finding, true))
			{
				Notebook.SetState(*Finding, EOptiFindingState::ResolvedByUser);
				Queue(Finding->Id);
			}
			continue;
		}
		const bool bScene = Finding->IsSceneFinding();
		const bool bDoneByUser = bScene ? OptiApply::SceneMatches(Finding->SceneEdits, true) : OptiApply::CurrentMatches(Finding->To);
		if (bDoneByUser)
		{
			// You made the change yourself: cross it off and learn that you like it.
			Brain.LearnFromUser(OptiActions::IndexOf(Finding->ActionId), Finding->ActiveKenyonCells, true);
			Notebook.SetState(*Finding, EOptiFindingState::ResolvedByUser);
			Queue(Finding->Id);
			continue;
		}
		const bool bStillAsMeasured = bScene ? OptiApply::SceneMatches(Finding->SceneEdits, false)
			: (Finding->From.IsEmpty() || OptiApply::CurrentMatches(Finding->From));
		const bool bChanged = !bStillAsMeasured || Finding->ContextKey != Context;
		if (bChanged && Finding->State != EOptiFindingState::Stale)
		{
			Notebook.SetState(*Finding, EOptiFindingState::Stale);
			continue;
		}
		if (Finding->State == EOptiFindingState::Postponed)
		{
			const bool bDue = (Finding->Reminder == EOptiReminder::InTwoHours && Now >= Finding->RemindAt)
				|| (Finding->Reminder == EOptiReminder::WhenFrameIsSlow && Reflex.RecentFrameMs() > SlowFrameMs);
			if (bDue)
			{
				Finding->Reminder = EOptiReminder::None;
				Notebook.SetState(*Finding, EOptiFindingState::New);
				Queue(Finding->Id);
			}
		}
	}
}

void FOptiCompanion::Queue(const FGuid& Id, bool bFront)
{
	NoticeQueue.Remove(Id);
	if (bFront)
	{
		NoticeQueue.Insert(Id, 0);
	}
	else
	{
		NoticeQueue.Add(Id);
	}
	OnChanged.Broadcast();
}

int32 FOptiCompanion::NoticeBudget() const
{
	// Every ignored notice lowers the hourly budget: the fly learns how often you want to hear from it.
	return FMath::Max(1, Settings().MaxNoticesPerHour - IgnoredStreak);
}

void FOptiCompanion::OnNaturalPause(EPause Pause)
{
	if (bJustWoke)
	{
		bJustWoke = false;
	}
	ShowNextNotice(Pause == EPause::Clicked);
}

void FOptiCompanion::ShowNextNotice(bool bForce)
{
	const UOptiCompanionSettings& S = Settings();
	if (NoticeQueue.IsEmpty() || (S.bDoNotDisturb && !bForce))
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();
	NoticeTimes.RemoveAll([Now](double Time) { return Now - Time > 3600.0; });
	if (!bForce && NoticeTimes.Num() >= NoticeBudget())
	{
		return;
	}

	TSharedPtr<FOptiFinding> Finding;
	while (!NoticeQueue.IsEmpty() && !Finding.IsValid())
	{
		Finding = Notebook.Find(NoticeQueue[0]);
		NoticeQueue.RemoveAt(0);
		if (Finding.IsValid() && Finding->State == EOptiFindingState::Dismissed)
		{
			Finding.Reset();
		}
	}
	if (!Finding.IsValid())
	{
		return;
	}
	NoticeTimes.Add(Now);

	if (S.Presence == EOptiPresence::Mascot && Layer.IsValid())
	{
		Layer->ShowBubble(Finding.ToSharedRef());
	}
	else
	{
		ShowToast(*Finding);
	}
}

void FOptiCompanion::ShowToast(const FOptiFinding& Finding)
{
	FNotificationInfo Info(DescribeFinding(Finding));
	Info.ExpireDuration = 8.f;
	Info.bFireAndForget = true;
	Info.Image = FOptiStyle::Get().GetBrush("Opti.Fly.Icon.Large");
	Info.Hyperlink = FSimpleDelegate::CreateSP(this, &FOptiCompanion::OpenNotebook);
	Info.HyperlinkText = OptiText(TEXT("ui.notebook.tab"));
	FSlateNotificationManager::Get().AddNotification(Info);
}

FText FOptiCompanion::DescribeFinding(const FOptiFinding& Finding) const
{
	FOptiPhrases& Phrases = FOptiPhrases::Get();
	FFormatNamedArguments Args;
	Args.Add(TEXT("ms"), FText::FromString(FormatMs(FMath::Abs(Finding.GainMs))));
	Args.Add(TEXT("pct"), FText::FromString(FString::Printf(TEXT("%.0f"), FMath::Abs(Finding.GainPercent()))));
	Args.Add(TEXT("metric"), FText::FromString(Finding.Metric == TEXT("GPUTime") ? TEXT("GPU") : Finding.Metric == TEXT("GameThread") ? TEXT("game thread") : TEXT("frame")));
	Args.Add(TEXT("asset"), FText::FromString(FPackageName::GetShortName(Finding.Asset)));
	Args.Add(TEXT("action"), Phrases.Action(Finding.ActionId));
	Args.Add(TEXT("pass"), FText::FromString(Finding.TopPasses.IsEmpty() ? Finding.Metric : Finding.TopPasses[0].Pass));

	if (Finding.Kind == EOptiFindingKind::Regression)
	{
		return Phrases.Notice(TEXT("notice.regression"), Args);
	}
	if (Finding.IsBlueprintFinding() && Finding.State != EOptiFindingState::ResolvedByUser)
	{
		Args.Add(TEXT("asset"), FText::FromString(FSoftObjectPath(Finding.Blueprint).GetAssetName()));
		return Phrases.Notice(TEXT("notice.blueprint_finding"), Args);
	}
	if (Finding.State == EOptiFindingState::ResolvedByUser)
	{
		return Phrases.Notice(TEXT("notice.resolved"), Args);
	}
	return Phrases.Notice(TEXT("notice.finding"), Args);
}

FText FOptiCompanion::GetStatusText() const
{
	if (!Settings().bEnabled)
	{
		return OptiText(TEXT("status.disabled"));
	}
	if (!LastStatus.IsEmpty())
	{
		return LastStatus;
	}
	const FText Play = PlayProfiler.GetStatus();
	if (!Play.IsEmpty())
	{
		return Play;
	}
	if (FPlatformTime::Seconds() - LastResultAt < ResultWhisperSeconds)
	{
		return LastResultText;
	}
	if (!LastBlocker.IsEmpty())
	{
		return FOptiPhrases::Get().Text(TEXT("status.waiting"), { { TEXT("reason"), OptiText(*LastBlocker) } });
	}
	return IsSceneClean() ? OptiText(TEXT("status.clean")) : OptiText(TEXT("status.observing"));
}

FText FOptiCompanion::GetWhisper() const
{
	if (!Settings().bEnabled)
	{
		return FText::GetEmpty();
	}
	if (IsNapping() && !LastStatus.IsEmpty())
	{
		return LastStatus;
	}
	const FText Play = PlayProfiler.GetStatus();
	if (!Play.IsEmpty())
	{
		return Play;
	}
	if (FPlatformTime::Seconds() - LastResultAt < ResultWhisperSeconds)
	{
		return LastResultText;
	}
	return FText::GetEmpty();
}

void FOptiCompanion::SetMood(EOptiFlyMood NewMood)
{
	if (Mood != NewMood)
	{
		Mood = NewMood;
		OnChanged.Broadcast();
	}
}

void FOptiCompanion::OnPackageSaved(const FString& Filename, UPackage* Package, FObjectPostSaveContext Context)
{
	if (!Package || Context.IsProceduralSave() || !Settings().bEnabled || (GEditor && GEditor->PlayWorld))
	{
		return;
	}
	if (Settings().bEnableSaveReflex)
	{
		Reflex.OnAssetSaved(Package->GetName());
	}
	OnNaturalPause(EPause::Saved);
}

void FOptiCompanion::OnBeginPIE(bool bIsSimulating)
{
	PlayProfiler.OnBeginPlay();
}

void FOptiCompanion::OnEndPIE(bool bIsSimulating)
{
	PlayProfiler.OnEndPlay();
	bPlaySummaryPending = PlayProfiler.GetCaptureCount() > 0 || PlayProfiler.IsPlaying();
	OnNaturalPause(EPause::EndedPlay);
}

int32 FOptiCompanion::ChoosePlayCandidate(const TArray<FOptiBlueprintCost>& Candidates, UWorld* World)
{
	const int32 Action = OptiActions::IndexOf(TEXT("blueprint.tick_interval"));
	if (Action == INDEX_NONE)
	{
		return INDEX_NONE;
	}
	// One smell per candidate: what the class costs, how long the frame and the world tick are, what is in the level.
	const FOptiSceneScent Scene = FOptiSceneScent::FromWorld(World, FOptiView());
	int32 Best = INDEX_NONE;
	float BestScore = -FLT_MAX;
	FOptiFlyBrain::FDecision BestDecision;
	for (int32 Index = 0; Index < Candidates.Num(); ++Index)
	{
		const FOptiBlueprintCost& Cost = Candidates[Index];
		FOptiProfile Profile;
		Profile.GameThreadMs = Cost.MsPerFrame; // what a Tick Interval acts on
		Profile.FrameMs = FMath::Max(PlayProfiler.GetFrameMs(), Cost.MsPerFrame);
		Profile.RenderThreadMs = 0.0;
		Profile.GpuMs = 0.0;
		const FOptiSmell Smell = FOptiSmell::FromParts(Profile, Scene, TEXT("play|") + Cost.BlueprintPath);
		const FOptiFlyBrain::FDecision Decision = Brain.DecidePlay(Smell, Action);
		UE_LOG(LogOptiCompanion, Log, TEXT("Play: the fly smells %s: expects %.3f of %.3f ms (uncertainty %.2f, novelty %.2f), score %.3f"),
			*Cost.ClassName, Decision.Prediction.ExpectedMs, Decision.Prediction.TargetMs, Decision.Prediction.Uncertainty, Decision.Novelty, Decision.Score);
		if (Decision.IsValid() && Decision.Score > BestScore)
		{
			BestScore = Decision.Score;
			Best = Index;
			BestDecision = Decision;
		}
	}
	// Same bar as editor experiments: not worth a test when the fly expects almost nothing.
	if (Best == INDEX_NONE || BestScore < 0.02f)
	{
		UE_LOG(LogOptiCompanion, Log, TEXT("Play: nothing the fly expects enough from."));
		return INDEX_NONE;
	}
	PlayDecision = BestDecision;
	PlayDecisionBlueprint = Candidates[Best].BlueprintPath;
	return Best;
}

void FOptiCompanion::OnTickIntervalResult(const FOptiTickIntervalResult& Result)
{
	const UOptiCompanionSettings& S = Settings();
	const double GainMs = -Result.Stat.Delta;
	const bool bWorthIt = Result.Stat.bSignificant && GainMs >= FMath::Max<double>(S.MinGainMs, Result.Stat.MeanA * S.MinGainPercent / 100.0);
	const FName ActionId(TEXT("blueprint.tick_interval"));

	FOptiTrial Trial;
	Trial.Time = FDateTime::Now();
	Trial.ActionId = ActionId;
	Trial.Metric = TEXT("GameThread");
	Trial.BaselineMs = Result.Stat.MeanA;
	Trial.GainMs = GainMs;
	Trial.bSignificant = Result.Stat.bSignificant;
	Trial.Outcome = bWorthIt ? EOptiTrialOutcome::Finding : (Result.Stat.bSignificant && GainMs > 0.0 ? EOptiTrialOutcome::TooSmall : EOptiTrialOutcome::NoGain);
	Trial.ContextKey = TEXT("play|") + Result.Cost.BlueprintPath;
	Notebook.AddTrial(Trial);

	// Dopamine: the saving as a fraction of what the class cost, against what the fly predicted.
	const bool bKnownDecision = PlayDecision.IsValid() && PlayDecisionBlueprint == Result.Cost.BlueprintPath;
	if (bKnownDecision)
	{
		const float GainFraction = FMath::Clamp(static_cast<float>(GainMs / FMath::Max(Result.Cost.MsPerFrame, 0.05)), -1.f, 1.5f);
		Brain.LearnFromExperiment(PlayDecision, GainFraction, -1.f);
		LogDecision(PlayDecision, GainFraction, -1.f);
		UE_LOG(LogOptiCompanion, Log, TEXT("Play: learned %s: %.0f%% saved, predicted %.0f%%"), *Result.Cost.ClassName,
			GainFraction * 100.f, PlayDecision.Prediction.GainFraction * 100.f);
	}
	LastResultText = FText::Format(NSLOCTEXT("OptiCompanion", "PlayTrial", "{0} · {1}"), FText::FromString(Result.Cost.ClassName.LeftChop(2)), DescribeTrial(Trial));
	LastResultAt = FPlatformTime::Seconds();

	if (!bWorthIt)
	{
		return;
	}
	FOptiFinding Finding;
	Finding.Kind = EOptiFindingKind::Optimization;
	Finding.ActionId = ActionId;
	Finding.Blueprint = Result.Cost.BlueprintPath;
	Finding.Asset = Result.Cost.BlueprintPath;
	Finding.From = { { TEXT("TickInterval"), FString::SanitizeFloat(Result.From) } };
	Finding.To = { { TEXT("TickInterval"), FString::SanitizeFloat(Result.To) } };
	Finding.Metric = TEXT("GameThread");
	Finding.GainMs = GainMs;
	Finding.CILowMs = -Result.Stat.CIHigh;
	Finding.CIHighMs = -Result.Stat.CILow;
	Finding.BaselineMs = Result.Stat.MeanA;
	Finding.ContextKey = TEXT("play");
	if (bKnownDecision)
	{
		Finding.ActiveKenyonCells = PlayDecision.ActiveKenyonCells; // so applying or dismissing it teaches the fly too
	}
	Queue(Notebook.Add(Finding)->Id);
	SetMood(EOptiFlyMood::Rubbing);
}

void FOptiCompanion::ShowPlaySummary()
{
	const TArray<FOptiBlueprintCost>& Costs = PlayProfiler.GetCosts();
	if (Costs.IsEmpty() || Settings().bDoNotDisturb)
	{
		return;
	}
	constexpr double WorthMentioningMs = 0.2;
	FNumberFormattingOptions Options;
	Options.MinimumFractionalDigits = Options.MaximumFractionalDigits = 2;
	if (Costs[0].MsPerFrame < WorthMentioningMs)
	{
		double Total = 0.0;
		for (const FOptiBlueprintCost& Cost : Costs)
		{
			Total += Cost.MsPerFrame;
		}
		LastResultText = FOptiPhrases::Get().Text(TEXT("play.fine"), { { TEXT("ms"), FText::AsNumber(Total, &Options) } });
		LastResultAt = FPlatformTime::Seconds();
		return;
	}
	FFormatNamedArguments Args;
	Args.Add(TEXT("blueprint"), FText::FromString(Costs[0].ClassName.LeftChop(2)));
	Args.Add(TEXT("ms"), FText::AsNumber(Costs[0].MsPerFrame, &Options));
	Args.Add(TEXT("n"), Costs[0].Instances);
	const FText Message = FOptiPhrases::Get().Notice(TEXT("notice.play_costs"), Args);
	if (Settings().Presence == EOptiPresence::Mascot && Layer.IsValid())
	{
		Layer->ShowMessage(Message);
	}
	else
	{
		FNotificationInfo Info(Message);
		Info.ExpireDuration = 8.f;
		Info.Image = FOptiStyle::Get().GetBrush("Opti.Fly.Icon.Large");
		FSlateNotificationManager::Get().AddNotification(Info);
	}
}

void FOptiCompanion::ApplyFinding(const FGuid& Id)
{
	TSharedPtr<FOptiFinding> Finding = Notebook.Find(Id);
	if (!Finding.IsValid() || Finding->Kind != EOptiFindingKind::Optimization)
	{
		return;
	}
	if (Finding->State == EOptiFindingState::Stale && !Finding->IsBlueprintFinding())
	{
		RecheckFinding(Id); // never apply with old numbers
		return;
	}
	FText Error;
	// Our own change, not a natural experiment. The CVar sink runs later in the frame, hence the fresh snapshot.
	bOwnChange = true;
	const bool bApplied = OptiApply::Apply(*Finding, Error);
	bOwnChange = false;
	SnapshotCVars();
	if (!bApplied)
	{
		FNotificationInfo Info(Error);
		Info.ExpireDuration = 6.f;
		FSlateNotificationManager::Get().AddNotification(Info);
		return;
	}
	Brain.LearnFromUser(OptiActions::IndexOf(Finding->ActionId), Finding->ActiveKenyonCells, true);
	Notebook.SetState(*Finding, EOptiFindingState::Applied);
	SetMood(Notebook.CountNew() > 0 ? EOptiFlyMood::Rubbing : EOptiFlyMood::Grooming);
	OnNoticeAnswered();
}

void FOptiCompanion::UndoFinding(const FGuid& Id)
{
	TSharedPtr<FOptiFinding> Finding = Notebook.Find(Id);
	if (!Finding.IsValid() || Finding->State != EOptiFindingState::Applied)
	{
		return;
	}
	FText Error;
	bOwnChange = true;
	const bool bUndone = OptiApply::Undo(*Finding, Error);
	bOwnChange = false;
	SnapshotCVars();
	if (bUndone)
	{
		Notebook.SetState(*Finding, EOptiFindingState::New);
	}
}

void FOptiCompanion::PostponeFinding(const FGuid& Id, EOptiReminder Reminder)
{
	TSharedPtr<FOptiFinding> Finding = Notebook.Find(Id);
	if (!Finding.IsValid())
	{
		return;
	}
	Finding->Reminder = Reminder;
	Finding->RemindAt = FDateTime::Now() + FTimespan::FromHours(2.0);
	Notebook.SetState(*Finding, EOptiFindingState::Postponed);
	OnNoticeAnswered();
}

void FOptiCompanion::DismissFinding(const FGuid& Id, bool bForever, const FString& Note)
{
	TSharedPtr<FOptiFinding> Finding = Notebook.Find(Id);
	if (!Finding.IsValid())
	{
		return;
	}
	if (Finding->Kind == EOptiFindingKind::Optimization)
	{
		Brain.LearnFromUser(OptiActions::IndexOf(Finding->ActionId), Finding->ActiveKenyonCells, false);
	}
	Finding->bDontSuggestAgain = bForever;
	Finding->Note = Note;
	Notebook.SetState(*Finding, EOptiFindingState::Dismissed);
	if (Mood == EOptiFlyMood::Buzzing || Mood == EOptiFlyMood::Rubbing)
	{
		SetMood(Notebook.CountNew() > 0 ? EOptiFlyMood::Rubbing : EOptiFlyMood::Grooming);
	}
	OnNoticeAnswered();
}

void FOptiCompanion::RestoreFinding(const FGuid& Id)
{
	if (TSharedPtr<FOptiFinding> Finding = Notebook.Find(Id))
	{
		Finding->bDontSuggestAgain = false;
		// Blueprint findings are re-measured in the next Play session, not by an editor experiment.
		Notebook.SetState(*Finding, Finding->IsBlueprintFinding() ? EOptiFindingState::New : EOptiFindingState::Stale);
	}
}

void FOptiCompanion::RecheckFinding(const FGuid& Id)
{
	if (TSharedPtr<FOptiFinding> Finding = Notebook.Find(Id))
	{
		if (Finding->IsBlueprintFinding())
		{
			return; // measured in Play
		}
		Notebook.SetState(*Finding, EOptiFindingState::Stale);
		NextNapTime = FPlatformTime::Seconds();
	}
}

void FOptiCompanion::OpenAsset(const FGuid& Id)
{
	TSharedPtr<FOptiFinding> Finding = Notebook.Find(Id);
	if (!Finding.IsValid() || Finding->Asset.IsEmpty() || !GEditor)
	{
		return;
	}
	FString First = Finding->Asset;
	First.Split(TEXT(","), &First, nullptr);
	const FString ObjectPath = First + TEXT(".") + FPackageName::GetShortName(First);
	if (UObject* Asset = StaticLoadObject(UObject::StaticClass(), nullptr, *ObjectPath))
	{
		GEditor->GetEditorSubsystem<UAssetEditorSubsystem>()->OpenEditorForAsset(Asset);
	}
	OnNoticeAnswered();
}

void FOptiCompanion::OnNoticeIgnored()
{
	++IgnoredStreak;
}

void FOptiCompanion::OnNoticeAnswered()
{
	IgnoredStreak = 0;
}

void FOptiCompanion::OnFlyClicked()
{
	if (NoticeQueue.IsEmpty())
	{
		// Nothing pending: show the most relevant open finding again, if any.
		for (const TSharedRef<FOptiFinding>& Finding : Notebook.GetFindings())
		{
			if (Finding->State == EOptiFindingState::New)
			{
				NoticeQueue.Add(Finding->Id);
				break;
			}
		}
	}
	if (NoticeQueue.IsEmpty() && Layer.IsValid())
	{
		const FOptiFlyBrain::FStats& Stats = Brain.GetStats();
		Layer->ShowMessage(FOptiPhrases::Get().Text(TEXT("ui.brain.project"), {
			{ TEXT("naps"), FText::AsNumber(Stats.Naps) },
			{ TEXT("focus"), FOptiPhrases::Get().Sector(Brain.Focus()) } }));
		return;
	}
	OnNaturalPause(EPause::Clicked);
}

void FOptiCompanion::NapNow()
{
	bForceNap = true;
	NextNapTime = 0.0;
}

void FOptiCompanion::OpenNotebook()
{
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(SOptiNotebook::TabName));
}

void FOptiCompanion::ExportBrain()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	TArray<FString> Files;
	if (Desktop && Desktop->SaveFileDialog(nullptr, OptiText(TEXT("ui.export_brain")).ToString(), FPaths::GetPath(Brain.LongTermPath()),
		TEXT("FlyBrain.opti"), TEXT("Fly brain (*.opti)|*.opti"), EFileDialogFlags::None, Files) && !Files.IsEmpty())
	{
		Brain.ExportLongTerm(Files[0]);
	}
}

void FOptiCompanion::ImportBrain()
{
	IDesktopPlatform* Desktop = FDesktopPlatformModule::Get();
	TArray<FString> Files;
	if (Desktop && Desktop->OpenFileDialog(nullptr, OptiText(TEXT("ui.import_brain")).ToString(), FPaths::GetPath(Brain.LongTermPath()),
		TEXT(""), TEXT("Fly brain (*.opti)|*.opti"), EFileDialogFlags::None, Files) && !Files.IsEmpty())
	{
		Brain.ImportLongTerm(Files[0]);
		OnChanged.Broadcast();
	}
}

void FOptiCompanion::ApplySettingsChange()
{
	GetMutableDefault<UOptiCompanionSettings>()->SaveConfig();
	FOptiPhrases::Get().Reload();
	OnChanged.Broadcast();
}
