#include "OptiCompanionModule.h"
#include "Modules/ModuleManager.h"
#include "HAL/IConsoleManager.h"
#include "OptiActions.h"
#include "Engine/World.h"

IMPLEMENT_MODULE(FOptiCompanionModule, OptiCompanion)

FOptiCompanionModule& FOptiCompanionModule::Get()
{
	return FModuleManager::LoadModuleChecked<FOptiCompanionModule>(TEXT("OptiCompanion"));
}

void FOptiCompanionModule::ShutdownModule()
{
	CancelProbe();
	ActiveProbe.Reset();
}

bool FOptiCompanionModule::StartProbe(const FOptiProbeSettings& Settings, FString& OutError, FOnOptiProbeFinished OnFinished)
{
	if (IsProbeRunning())
	{
		OutError = TEXT("Another probe is already running (Opti.Cancel stops it)");
		return false;
	}

	TSharedPtr<FOptiProbe> Probe = MakeShared<FOptiProbe>(Settings);
	Probe->OnFinished = OnFinished;
	if (!Probe->Start(OutError))
	{
		return false;
	}
	ActiveProbe = Probe;
	return true;
}

void FOptiCompanionModule::CancelProbe()
{
	if (ActiveProbe.IsValid())
	{
		ActiveProbe->Cancel();
	}
}

namespace
{
	int32 ArgAsInt(const TArray<FString>& Args, int32 Index, int32 Default)
	{
		return Args.IsValidIndex(Index) ? FCString::Atoi(*Args[Index]) : Default;
	}

	void StartFromConsole(const FOptiProbeSettings& Settings)
	{
		// -OptiExitAfterProbe lets an automated run (-game -ExecCmds="Opti.Probe ...") close itself when done.
		const bool bExitWhenDone = FParse::Param(FCommandLine::Get(), TEXT("OptiExitAfterProbe"));
		FOnOptiProbeFinished OnFinished = FOnOptiProbeFinished::CreateLambda([bExitWhenDone](const FOptiProbeResult&)
		{
			if (bExitWhenDone)
			{
				FPlatformMisc::RequestExit(false, TEXT("OptiExitAfterProbe"));
			}
		});

		FString Error;
		if (!FOptiCompanionModule::Get().StartProbe(Settings, Error, OnFinished))
		{
			UE_LOG(LogOptiCompanion, Warning, TEXT("%s"), *Error);
			OnFinished.Execute(FOptiProbeResult());
		}
	}

	FAutoConsoleCommand ProbeCommand(
		TEXT("Opti.Probe"),
		TEXT("A/B test a console variable with interleaved blocks and per-pass GPU timings.\n")
		TEXT("Usage: Opti.Probe <CVar> <ValueA> <ValueB> [BlocksPerVariant=8] [FramesPerBlock=60]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			if (Args.Num() < 3)
			{
				UE_LOG(LogOptiCompanion, Warning, TEXT("Usage: Opti.Probe <CVar> <ValueA> <ValueB> [BlocksPerVariant=8] [FramesPerBlock=60]"));
				return;
			}
			FOptiProbeSettings Settings = FOptiProbeSettings::ForCVar(Args[0], Args[1], Args[2]);
			Settings.BlocksPerVariant = ArgAsInt(Args, 3, Settings.BlocksPerVariant);
			Settings.FramesPerBlock = ArgAsInt(Args, 4, Settings.FramesPerBlock);
			StartFromConsole(Settings);
		}));

	FAutoConsoleCommand NoiseCommand(
		TEXT("Opti.Noise"),
		TEXT("A/A test: runs the probe without changing anything to measure how noisy this machine is.\n")
		TEXT("Usage: Opti.Noise [BlocksPerVariant=8] [FramesPerBlock=60]"),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			FOptiProbeSettings Settings;
			Settings.BlocksPerVariant = ArgAsInt(Args, 0, Settings.BlocksPerVariant);
			Settings.FramesPerBlock = ArgAsInt(Args, 1, Settings.FramesPerBlock);
			StartFromConsole(Settings);
		}));

	FAutoConsoleCommand CatalogCommand(
		TEXT("Opti.Catalog"),
		TEXT("Lists every action the fly knows and whether it can run on this engine and level."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			OptiActions::LogCatalog(GWorld);
		}));

	FAutoConsoleCommand CancelCommand(
		TEXT("Opti.Cancel"),
		TEXT("Cancels the running probe and restores every console variable it changed."),
		FConsoleCommandDelegate::CreateLambda([]()
		{
			FOptiCompanionModule::Get().CancelProbe();
		}));
}
