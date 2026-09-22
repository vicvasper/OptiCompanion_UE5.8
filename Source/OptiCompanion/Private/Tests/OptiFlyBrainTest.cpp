#include "Misc/AutomationTest.h"
#include "OptiFlyBrain.h"
#include "OptiProbe.h"
#include "OptiSmell.h"
#include "Math/RandomStream.h"
#include "HAL/IConsoleManager.h"

#if WITH_DEV_AUTOMATION_TESTS

/**
 * Does the fly earn its place? A synthetic benchmark in the spirit of FlyDoom: the same stream of
 * situations is given to the fly brain, to context-free Thompson sampling and to random choice.
 *
 * In the synthetic world an action's true saving depends on the scene: cutting shadows pays off where
 * there are many shadow-casting lights, effects where there is a lot of Niagara, geometry in dense
 * levels. The innate guesses in the catalog are deliberately not the truth, so everyone has to learn.
 * Only a policy that uses context can exploit that, which is what the mushroom body is for.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOptiFlyBrainBenchmark, "OptiCompanion.Brain.FlyVersusBaselines",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace
{
	/**
	 * The benchmark must not depend on the project it runs in: accepted findings change CVars in the project's
	 * ini, which changes which actions are available. Every catalog CVar is set to its engine default for the
	 * duration of the test and restored afterwards.
	 */
	struct FScopedEngineDefaults
	{
		TArray<TPair<IConsoleVariable*, FString>> Saved;

		FScopedEngineDefaults()
		{
			for (const FOptiAction& Action : OptiActions::Catalog())
			{
				TArray<FString> Names;
				for (const FOptiCVarChange& Change : Action.Changes) { Names.Add(Change.Name); }
				for (const TPair<FString, FString>& Requirement : Action.Requires) { Names.Add(Requirement.Key); }
				for (const FString& Name : Names)
				{
					IConsoleVariable* CVar = OptiFindCVar(Name);
					if (CVar && !Saved.ContainsByPredicate([CVar](const TPair<IConsoleVariable*, FString>& S) { return S.Key == CVar; }))
					{
						Saved.Add({ CVar, CVar->GetString() });
						CVar->Set(*CVar->GetDefaultValue(), ECVF_SetByConsole);
					}
				}
			}
		}

		~FScopedEngineDefaults()
		{
			for (const TPair<IConsoleVariable*, FString>& Entry : Saved)
			{
				Entry.Key->Set(*Entry.Value, ECVF_SetByConsole);
			}
		}
	};

	struct FScene
	{
		FOptiSmell Smell;
		float Boost[static_cast<int32>(EOptiSector::Count)];
	};

	struct FWorld
	{
		TArray<FScene> Scenes;
		TArray<float> TrueGain;   // per action, before the scene boost
		TArray<float> TrueVisual; // per action, 1 = visible
	};

	FWorld MakeWorld(int32 Seed)
	{
		FRandomStream Rng(Seed);
		FWorld World;
		for (const FOptiAction& Action : OptiActions::Catalog())
		{
			const bool bHarmful = Rng.GetFraction() < 0.2f;
			World.TrueGain.Add(bHarmful ? -0.1f : Action.TypicalSaving * Rng.FRandRange(0.2f, 1.6f));
			World.TrueVisual.Add(Action.VisualRisk * Rng.FRandRange(0.2f, 2.f));
		}

		for (int32 Index = 0; Index < 16; ++Index)
		{
			FOptiProfile Profile;
			for (double& Ms : Profile.PassMs)
			{
				Ms = Rng.GetFraction() < 0.25f ? 0.0 : Rng.FRandRange(0.1f, 2.f);
				Profile.GpuMs += Ms;
			}
			Profile.FrameMs = Profile.GpuMs + 1.0;
			Profile.GameThreadMs = 3.0;
			Profile.RenderThreadMs = 4.0;
			Profile.DrawCalls = Rng.FRandRange(500.f, 5000.f);

			FOptiSceneScent Scent;
			Scent.ShadowLights = Rng.GetFraction() < 0.5f ? 2 : 150;
			Scent.Niagara = Rng.GetFraction() < 0.5f ? 0 : 300;
			Scent.StaticMeshes = Rng.GetFraction() < 0.5f ? 100 : 20000;
			Scent.Lights = Scent.ShadowLights + 10;

			FScene Scene;
			Scene.Smell = FOptiSmell::FromParts(Profile, Scent, FString::Printf(TEXT("scene%d"), Index));
			for (float& Boost : Scene.Boost) { Boost = 1.f; }
			Scene.Boost[static_cast<int32>(EOptiSector::Shadows)] = Scent.ShadowLights > 50 ? 2.f : 0.3f;
			Scene.Boost[static_cast<int32>(EOptiSector::Effects)] = Scent.Niagara > 100 ? 2.f : 0.3f;
			Scene.Boost[static_cast<int32>(EOptiSector::Geometry)] = Scent.StaticMeshes > 5000 ? 2.f : 0.3f;
			World.Scenes.Add(MoveTemp(Scene));
		}
		return World;
	}

	float Gain(const FWorld& World, const FScene& Scene, int32 Action)
	{
		const EOptiSector Sector = OptiActions::Catalog()[Action].Sector;
		return FMath::Clamp(World.TrueGain[Action] * Scene.Boost[static_cast<int32>(Sector)], -0.3f, 0.9f);
	}

	/** Milliseconds actually won by picking this action here; visible changes are rejected, so they win nothing. */
	float Utility(const FWorld& World, const FScene& Scene, int32 Action)
	{
		if (Action == INDEX_NONE || World.TrueVisual[Action] > 1.f)
		{
			return 0.f;
		}
		return Gain(World, Scene, Action) * static_cast<float>(OptiActions::Catalog()[Action].TargetMs(Scene.Smell.Profile));
	}

	float Oracle(const FWorld& World, const FScene& Scene)
	{
		float Best = 0.f;
		for (int32 Action = 0; Action < OptiActions::Catalog().Num(); ++Action)
		{
			FOptiCVarSet A, B;
			if (OptiActions::Catalog()[Action].BuildVariants(A, B))
			{
				Best = FMath::Max(Best, Utility(World, Scene, Action));
			}
		}
		return Best;
	}

	/** Share of the oracle's milliseconds won over the second half of the run. */
	float Run(const FWorld& World, EOptiSelector Selector, int32 Seed)
	{
		FOptiFlyBrain Brain;
		Brain.Initialize(TEXT("Benchmark"), false);
		FRandomStream Rng(Seed);
		constexpr int32 Trials = 400;
		double Won = 0.0, Possible = 0.0;
		for (int32 Trial = 0; Trial < Trials; ++Trial)
		{
			const FScene& Scene = World.Scenes[Rng.RandHelper(World.Scenes.Num())];
			const FOptiFlyBrain::FDecision Decision = Brain.Decide(Scene.Smell, Selector, {});
			if (Decision.IsValid())
			{
				const float Measured = Gain(World, Scene, Decision.ActionIndex) + 0.05f * Rng.FRandRange(-1.f, 1.f);
				Brain.LearnFromExperiment(Decision, Measured, World.TrueVisual[Decision.ActionIndex]);
			}
			if (Trial >= Trials / 2)
			{
				Won += Utility(World, Scene, Decision.ActionIndex);
				Possible += Oracle(World, Scene);
			}
		}
		return Possible > 0.0 ? static_cast<float>(Won / Possible) : 0.f;
	}
}

bool FOptiFlyBrainBenchmark::RunTest(const FString& Parameters)
{
	const FScopedEngineDefaults Defaults;
	struct FEntry { EOptiSelector Selector; const TCHAR* Name; float Score = 0.f; };
	FEntry Entries[] = { { EOptiSelector::Fly, TEXT("Fly brain") }, { EOptiSelector::Thompson, TEXT("Thompson sampling") }, { EOptiSelector::Random, TEXT("Random") } };

	constexpr int32 Worlds = 4;
	for (int32 WorldSeed = 0; WorldSeed < Worlds; ++WorldSeed)
	{
		const FWorld World = MakeWorld(1000 + WorldSeed);
		for (FEntry& Entry : Entries)
		{
			Entry.Score += Run(World, Entry.Selector, 77 + WorldSeed) / Worlds;
		}
	}

	for (const FEntry& Entry : Entries)
	{
		AddInfo(FString::Printf(TEXT("%-18s %5.1f%% of the best possible saving (second half of 400 naps, 4 worlds)"), Entry.Name, Entry.Score * 100.f));
		UE_LOG(LogOptiCompanion, Display, TEXT("Benchmark: %-18s %5.1f%% of oracle"), Entry.Name, Entry.Score * 100.f);
	}
	TestTrue(TEXT("The fly beats random choice"), Entries[0].Score > Entries[2].Score);
	return true;
}

/** Sweeps the brain's choice weights on the same benchmark. Not part of the default run (stress filter). */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FOptiFlyBrainTuning, "OptiCompanion.Brain.Tuning",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::StressFilter)

bool FOptiFlyBrainTuning::RunTest(const FString& Parameters)
{
	const FScopedEngineDefaults Defaults;
	IConsoleVariable* Focus = IConsoleManager::Get().FindConsoleVariable(TEXT("opti.Brain.FocusWeight"));
	IConsoleVariable* Curiosity = IConsoleManager::Get().FindConsoleVariable(TEXT("opti.Brain.Curiosity"));
	if (!TestNotNull(TEXT("focus cvar"), Focus) || !TestNotNull(TEXT("curiosity cvar"), Curiosity))
	{
		return false;
	}
	const float OldFocus = Focus->GetFloat(), OldCuriosity = Curiosity->GetFloat();

	constexpr int32 Worlds = 4;
	float Thompson = 0.f;
	for (int32 WorldSeed = 0; WorldSeed < Worlds; ++WorldSeed)
	{
		Thompson += Run(MakeWorld(1000 + WorldSeed), EOptiSelector::Thompson, 77 + WorldSeed) / Worlds;
	}
	UE_LOG(LogOptiCompanion, Display, TEXT("Tuning: Thompson sampling %5.1f%%"), Thompson * 100.f);

	for (const float F : { 1.f, 0.5f, 0.f })
	{
		for (const float C : { 0.5f, 0.25f, 0.1f, 0.03f })
		{
			Focus->Set(F, ECVF_SetByCode);
			Curiosity->Set(C, ECVF_SetByCode);
			float Fly = 0.f;
			for (int32 WorldSeed = 0; WorldSeed < Worlds; ++WorldSeed)
			{
				Fly += Run(MakeWorld(1000 + WorldSeed), EOptiSelector::Fly, 77 + WorldSeed) / Worlds;
			}
			UE_LOG(LogOptiCompanion, Display, TEXT("Tuning: focus %.2f curiosity %.2f -> fly %5.1f%%"), F, C, Fly * 100.f);
		}
	}
	Focus->Set(OldFocus, ECVF_SetByCode);
	Curiosity->Set(OldCuriosity, ECVF_SetByCode);
	return true;
}

#endif
