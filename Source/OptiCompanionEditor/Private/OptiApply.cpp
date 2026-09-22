#include "OptiApply.h"
#include "OptiNotebook.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformFileManager.h"
#include "ISourceControlModule.h"
#include "ISourceControlProvider.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "SourceControlHelpers.h"
#include "OptiActions.h"
#include "Components/ActorComponent.h"
#include "ScopedTransaction.h"
#include "Editor.h"
#include "Engine/Blueprint.h"
#include "EngineUtils.h"
#include "GameFramework/Actor.h"

namespace
{
	const TCHAR* Section = TEXT("[SystemSettings]");

	bool ValuesMatch(const FString& A, const FString& B)
	{
		if (A == B)
		{
			return true;
		}
		// "0.5" vs "0.500000": compare numerically when both parse.
		if (FCString::IsNumeric(*A) && FCString::IsNumeric(*B))
		{
			return FMath::IsNearlyEqual(FCString::Atod(*A), FCString::Atod(*B), 1e-4);
		}
		return false;
	}

	/** Returns the current value of Key inside [SystemSettings], or "" when absent. */
	FString ReadKey(const TArray<FString>& Lines, const FString& Key)
	{
		bool bInSection = false;
		for (const FString& Line : Lines)
		{
			const FString Trimmed = Line.TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("[")))
			{
				bInSection = Trimmed.Equals(Section, ESearchCase::IgnoreCase);
				continue;
			}
			FString Left, Right;
			if (bInSection && Trimmed.Split(TEXT("="), &Left, &Right) && Left.TrimEnd().Equals(Key, ESearchCase::IgnoreCase))
			{
				return Right.TrimStart();
			}
		}
		return FString();
	}

	/** Sets Key=Value in [SystemSettings] (Value "" removes the key), adding the section if needed. */
	void WriteKey(TArray<FString>& Lines, const FString& Key, const FString& Value)
	{
		int32 SectionLine = INDEX_NONE;
		int32 SectionEnd = Lines.Num();
		for (int32 Index = 0; Index < Lines.Num(); ++Index)
		{
			const FString Trimmed = Lines[Index].TrimStartAndEnd();
			if (Trimmed.StartsWith(TEXT("[")))
			{
				if (SectionLine != INDEX_NONE)
				{
					SectionEnd = Index;
					break;
				}
				if (Trimmed.Equals(Section, ESearchCase::IgnoreCase))
				{
					SectionLine = Index;
				}
			}
		}

		if (SectionLine != INDEX_NONE)
		{
			for (int32 Index = SectionLine + 1; Index < SectionEnd; ++Index)
			{
				FString Left, Right;
				if (Lines[Index].Split(TEXT("="), &Left, &Right) && Left.TrimStartAndEnd().Equals(Key, ESearchCase::IgnoreCase))
				{
					if (Value.IsEmpty())
					{
						Lines.RemoveAt(Index);
					}
					else
					{
						Lines[Index] = Key + TEXT("=") + Value;
					}
					return;
				}
			}
			if (!Value.IsEmpty())
			{
				// Insert before trailing blank lines of the section.
				int32 Insert = SectionEnd;
				while (Insert > SectionLine + 1 && Lines[Insert - 1].TrimStartAndEnd().IsEmpty())
				{
					--Insert;
				}
				Lines.Insert(Key + TEXT("=") + Value, Insert);
			}
			return;
		}

		if (!Value.IsEmpty())
		{
			if (!Lines.IsEmpty() && !Lines.Last().TrimStartAndEnd().IsEmpty())
			{
				Lines.Add(FString());
			}
			Lines.Add(Section);
			Lines.Add(Key + TEXT("=") + Value);
		}
	}

	bool PrepareFile(const FString& Path, FText& OutError)
	{
		ISourceControlModule& SourceControl = ISourceControlModule::Get();
		if (SourceControl.IsEnabled() && SourceControl.GetProvider().IsAvailable())
		{
			SourceControlHelpers::CheckOutOrAddFile(Path, true);
		}
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		if (PlatformFile.FileExists(*Path) && PlatformFile.IsReadOnly(*Path))
		{
			OutError = FText::FromString(FString::Printf(TEXT("%s is read-only. Check it out and try again."), *Path));
			return false;
		}
		return true;
	}

	void SetLive(const FOptiCVarSet& Set)
	{
		for (const TPair<FString, FString>& Pair : Set)
		{
			if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Pair.Key))
			{
				CVar->Set(*Pair.Value, ECVF_SetByConsole);
			}
		}
	}
}

namespace OptiApply
{
	FString ConfigPath()
	{
		return FPaths::ConvertRelativePathToFull(FPaths::ProjectConfigDir() / TEXT("DefaultEngine.ini"));
	}

	bool CurrentMatches(const FOptiCVarSet& Set)
	{
		if (Set.IsEmpty())
		{
			return false;
		}
		for (const TPair<FString, FString>& Pair : Set)
		{
			IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(*Pair.Key);
			if (!CVar || !ValuesMatch(CVar->GetString(), Pair.Value))
			{
				return false;
			}
		}
		return true;
	}

	bool SceneMatches(const FString& SceneEdits, bool bAfter)
	{
		const TArray<FOptiSceneEdit> Edits = OptiScene::Deserialize(SceneEdits);
		if (Edits.IsEmpty())
		{
			return false;
		}
		for (const FOptiSceneEdit& Edit : Edits)
		{
			const UActorComponent* Component = Edit.Component.Get();
			if (!Component || !ValuesMatch(OptiScene::GetValue(*Component, Edit.Property), bAfter ? Edit.After : Edit.Before))
			{
				return false;
			}
		}
		return true;
	}

	/** Sets every edited property to Before or After inside one undoable editor transaction. */
	static bool CommitScene(const FOptiFinding& Finding, bool bToAfter, FText& OutError)
	{
		const TArray<FOptiSceneEdit> Edits = OptiScene::Deserialize(Finding.SceneEdits);
		FScopedTransaction Transaction(bToAfter
			? NSLOCTEXT("OptiCompanion", "ApplySceneFinding", "Apply OptiCompanion finding")
			: NSLOCTEXT("OptiCompanion", "UndoSceneFinding", "Undo OptiCompanion finding"));
		int32 Changed = 0;
		for (const FOptiSceneEdit& Edit : Edits)
		{
			UActorComponent* Component = Edit.Component.Get();
			if (!Component)
			{
				continue;
			}
			Component->Modify();
			if (OptiScene::SetValue(*Component, Edit.Property, bToAfter ? Edit.After : Edit.Before))
			{
				if (FProperty* Property = FindFProperty<FProperty>(Component->GetClass(), Edit.Property))
				{
					FPropertyChangedEvent Event(Property);
					Component->PostEditChangeProperty(Event);
				}
				++Changed;
			}
		}
		if (Changed == 0)
		{
			Transaction.Cancel();
			OutError = NSLOCTEXT("OptiCompanion", "SceneGone", "The components in this finding are no longer in the level.");
			return false;
		}
		return true;
	}

	static AActor* BlueprintDefaults(const FOptiFinding& Finding)
	{
		const UBlueprint* Blueprint = LoadObject<UBlueprint>(nullptr, *Finding.Blueprint);
		return Blueprint && Blueprint->GeneratedClass ? Blueprint->GeneratedClass->GetDefaultObject<AActor>() : nullptr;
	}

	static float BlueprintValue(const FOptiCVarSet& Set)
	{
		return Set.IsEmpty() ? 0.f : FCString::Atof(*Set[0].Value);
	}

	bool BlueprintMatches(const FOptiFinding& Finding, bool bAfter)
	{
		const AActor* Defaults = BlueprintDefaults(Finding);
		return Defaults && FMath::IsNearlyEqual(Defaults->PrimaryActorTick.TickInterval, BlueprintValue(bAfter ? Finding.To : Finding.From), 0.001f);
	}

	/** Blueprint findings change the class defaults (and the placed copies that still used the old default). */
	static bool CommitBlueprint(const FOptiFinding& Finding, bool bToAfter, FText& OutError)
	{
		AActor* Defaults = BlueprintDefaults(Finding);
		if (!Defaults)
		{
			OutError = NSLOCTEXT("OptiCompanion", "BlueprintGone", "The Blueprint in this finding no longer exists.");
			return false;
		}
		const float Value = BlueprintValue(bToAfter ? Finding.To : Finding.From);
		FScopedTransaction Transaction(bToAfter
			? NSLOCTEXT("OptiCompanion", "ApplyBlueprintFinding", "Apply OptiCompanion finding")
			: NSLOCTEXT("OptiCompanion", "UndoBlueprintFinding", "Undo OptiCompanion finding"));
		const float Old = Defaults->PrimaryActorTick.TickInterval;
		Defaults->Modify();
		Defaults->PrimaryActorTick.TickInterval = Value;
		if (UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr)
		{
			for (TActorIterator<AActor> It(World, Defaults->GetClass()); It; ++It)
			{
				if (FMath::IsNearlyEqual(It->PrimaryActorTick.TickInterval, Old, 0.001f))
				{
					It->Modify();
					It->PrimaryActorTick.TickInterval = Value;
				}
			}
		}
		return true;
	}

	bool Apply(FOptiFinding& Finding, FText& OutError)
	{
		if (Finding.IsBlueprintFinding())
		{
			return CommitBlueprint(Finding, true, OutError);
		}
		if (Finding.IsSceneFinding() && !CommitScene(Finding, true, OutError))
		{
			return false;
		}
		if (Finding.To.IsEmpty())
		{
			return true;
		}
		const FString Path = ConfigPath();
		if (!PrepareFile(Path, OutError))
		{
			return false;
		}
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *Path);

		Finding.IniBefore.Reset();
		for (const TPair<FString, FString>& Pair : Finding.To)
		{
			Finding.IniBefore.Add({ Pair.Key, ReadKey(Lines, Pair.Key) });
			WriteKey(Lines, Pair.Key, Pair.Value);
		}
		if (!FFileHelper::SaveStringArrayToFile(Lines, *Path))
		{
			OutError = FText::FromString(FString::Printf(TEXT("Could not write %s"), *Path));
			return false;
		}
		SetLive(Finding.To);
		return true;
	}

	bool Undo(FOptiFinding& Finding, FText& OutError)
	{
		if (Finding.IsBlueprintFinding())
		{
			return CommitBlueprint(Finding, false, OutError);
		}
		if (Finding.IsSceneFinding() && !CommitScene(Finding, false, OutError))
		{
			return false;
		}
		if (Finding.To.IsEmpty())
		{
			return true;
		}
		const FString Path = ConfigPath();
		if (!PrepareFile(Path, OutError))
		{
			return false;
		}
		TArray<FString> Lines;
		FFileHelper::LoadFileToStringArray(Lines, *Path);
		for (const TPair<FString, FString>& Pair : Finding.To)
		{
			const TPair<FString, FString>* Before = Finding.IniBefore.FindByPredicate([&Pair](const TPair<FString, FString>& B) { return B.Key == Pair.Key; });
			WriteKey(Lines, Pair.Key, Before ? Before->Value : FString());
		}
		if (!FFileHelper::SaveStringArrayToFile(Lines, *Path))
		{
			OutError = FText::FromString(FString::Printf(TEXT("Could not write %s"), *Path));
			return false;
		}
		SetLive(Finding.From);
		return true;
	}
}
