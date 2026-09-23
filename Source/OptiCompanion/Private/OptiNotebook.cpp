#include "OptiNotebook.h"
#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

const TCHAR* LexToString(EOptiFindingState State)
{
	switch (State)
	{
	case EOptiFindingState::New: return TEXT("New");
	case EOptiFindingState::Postponed: return TEXT("Postponed");
	case EOptiFindingState::Applied: return TEXT("Applied");
	case EOptiFindingState::ResolvedByUser: return TEXT("ResolvedByUser");
	case EOptiFindingState::Stale: return TEXT("Stale");
	case EOptiFindingState::Dismissed: return TEXT("Dismissed");
	default: return TEXT("New");
	}
}

const TCHAR* LexToString(EOptiTrialOutcome Outcome)
{
	switch (Outcome)
	{
	case EOptiTrialOutcome::NoGain: return TEXT("NoGain");
	case EOptiTrialOutcome::TooSmall: return TEXT("TooSmall");
	case EOptiTrialOutcome::Visible: return TEXT("Visible");
	case EOptiTrialOutcome::Finding: return TEXT("Finding");
	case EOptiTrialOutcome::Dropped: return TEXT("Dropped");
	default: return TEXT("NoGain");
	}
}

namespace
{
	constexpr int32 MaxTrials = 300;

	EOptiTrialOutcome OutcomeFromString(const FString& Text)
	{
		for (int32 Value = 0; Value <= static_cast<int32>(EOptiTrialOutcome::Dropped); ++Value)
		{
			if (Text == LexToString(static_cast<EOptiTrialOutcome>(Value)))
			{
				return static_cast<EOptiTrialOutcome>(Value);
			}
		}
		return EOptiTrialOutcome::NoGain;
	}

	EOptiFindingState StateFromString(const FString& Text)
	{
		for (int32 Value = 0; Value <= static_cast<int32>(EOptiFindingState::Dismissed); ++Value)
		{
			if (Text == LexToString(static_cast<EOptiFindingState>(Value)))
			{
				return static_cast<EOptiFindingState>(Value);
			}
		}
		return EOptiFindingState::New;
	}

	TArray<TSharedPtr<FJsonValue>> CVarsToJson(const FOptiCVarSet& Set)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const TPair<FString, FString>& Pair : Set)
		{
			TSharedRef<FJsonObject> Item = MakeShared<FJsonObject>();
			Item->SetStringField(TEXT("name"), Pair.Key);
			Item->SetStringField(TEXT("value"), Pair.Value);
			Out.Add(MakeShared<FJsonValueObject>(Item));
		}
		return Out;
	}

	FOptiCVarSet CVarsFromJson(const TSharedPtr<FJsonObject>& Object, const TCHAR* Field)
	{
		FOptiCVarSet Out;
		const TArray<TSharedPtr<FJsonValue>>* Items;
		if (Object->TryGetArrayField(Field, Items))
		{
			for (const TSharedPtr<FJsonValue>& Item : *Items)
			{
				const TSharedPtr<FJsonObject>& Pair = Item->AsObject();
				Out.Add({ Pair->GetStringField(TEXT("name")), Pair->GetStringField(TEXT("value")) });
			}
		}
		return Out;
	}

	TSharedRef<FJsonObject> ToJson(const FOptiFinding& F)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("id"), F.Id.ToString());
		J->SetStringField(TEXT("kind"), F.Kind == EOptiFindingKind::Regression ? TEXT("Regression") : TEXT("Optimization"));
		J->SetStringField(TEXT("state"), LexToString(F.State));
		J->SetStringField(TEXT("action"), F.ActionId.ToString());
		J->SetStringField(TEXT("asset"), F.Asset);
		J->SetArrayField(TEXT("from"), CVarsToJson(F.From));
		J->SetArrayField(TEXT("to"), CVarsToJson(F.To));
		J->SetArrayField(TEXT("iniBefore"), CVarsToJson(F.IniBefore));
		J->SetStringField(TEXT("sceneEdits"), F.SceneEdits);
		J->SetStringField(TEXT("blueprint"), F.Blueprint);
		J->SetStringField(TEXT("metric"), F.Metric);
		J->SetNumberField(TEXT("gainMs"), F.GainMs);
		J->SetNumberField(TEXT("ciLowMs"), F.CILowMs);
		J->SetNumberField(TEXT("ciHighMs"), F.CIHighMs);
		J->SetNumberField(TEXT("baselineMs"), F.BaselineMs);
		TArray<TSharedPtr<FJsonValue>> Passes;
		for (const FOptiPassDelta& Pass : F.TopPasses)
		{
			TSharedRef<FJsonObject> P = MakeShared<FJsonObject>();
			P->SetStringField(TEXT("pass"), Pass.Pass);
			P->SetNumberField(TEXT("deltaMs"), Pass.DeltaMs);
			Passes.Add(MakeShared<FJsonValueObject>(P));
		}
		J->SetArrayField(TEXT("passes"), Passes);
		J->SetNumberField(TEXT("visualMean"), F.VisualMean);
		J->SetNumberField(TEXT("visualP95"), F.VisualP95);
		J->SetStringField(TEXT("captureA"), F.CaptureA);
		J->SetStringField(TEXT("captureB"), F.CaptureB);
		J->SetStringField(TEXT("captureDiff"), F.CaptureDiff);
		J->SetStringField(TEXT("context"), F.ContextKey);
		TArray<TSharedPtr<FJsonValue>> Cells;
		for (int32 Cell : F.ActiveKenyonCells)
		{
			Cells.Add(MakeShared<FJsonValueNumber>(Cell));
		}
		J->SetArrayField(TEXT("kenyonCells"), Cells);
		J->SetStringField(TEXT("created"), F.Created.ToIso8601());
		J->SetStringField(TEXT("changed"), F.Changed.ToIso8601());
		J->SetNumberField(TEXT("reminder"), static_cast<int32>(F.Reminder));
		J->SetStringField(TEXT("remindAt"), F.RemindAt.ToIso8601());
		J->SetStringField(TEXT("note"), F.Note);
		J->SetBoolField(TEXT("dontSuggestAgain"), F.bDontSuggestAgain);
		return J;
	}

	FOptiFinding FromJson(const TSharedPtr<FJsonObject>& J)
	{
		FOptiFinding F;
		FGuid::Parse(J->GetStringField(TEXT("id")), F.Id);
		F.Kind = J->GetStringField(TEXT("kind")) == TEXT("Regression") ? EOptiFindingKind::Regression : EOptiFindingKind::Optimization;
		F.State = StateFromString(J->GetStringField(TEXT("state")));
		F.ActionId = FName(*J->GetStringField(TEXT("action")));
		F.Asset = J->GetStringField(TEXT("asset"));
		F.From = CVarsFromJson(J, TEXT("from"));
		F.To = CVarsFromJson(J, TEXT("to"));
		F.IniBefore = CVarsFromJson(J, TEXT("iniBefore"));
		J->TryGetStringField(TEXT("sceneEdits"), F.SceneEdits);
		J->TryGetStringField(TEXT("blueprint"), F.Blueprint);
		F.Metric = J->GetStringField(TEXT("metric"));
		F.GainMs = J->GetNumberField(TEXT("gainMs"));
		F.CILowMs = J->GetNumberField(TEXT("ciLowMs"));
		F.CIHighMs = J->GetNumberField(TEXT("ciHighMs"));
		F.BaselineMs = J->GetNumberField(TEXT("baselineMs"));
		const TArray<TSharedPtr<FJsonValue>>* Passes;
		if (J->TryGetArrayField(TEXT("passes"), Passes))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Passes)
			{
				F.TopPasses.Add({ Value->AsObject()->GetStringField(TEXT("pass")), Value->AsObject()->GetNumberField(TEXT("deltaMs")) });
			}
		}
		F.VisualMean = J->GetNumberField(TEXT("visualMean"));
		F.VisualP95 = J->GetNumberField(TEXT("visualP95"));
		F.CaptureA = J->GetStringField(TEXT("captureA"));
		F.CaptureB = J->GetStringField(TEXT("captureB"));
		F.CaptureDiff = J->GetStringField(TEXT("captureDiff"));
		F.ContextKey = J->GetStringField(TEXT("context"));
		const TArray<TSharedPtr<FJsonValue>>* Cells;
		if (J->TryGetArrayField(TEXT("kenyonCells"), Cells))
		{
			for (const TSharedPtr<FJsonValue>& Value : *Cells)
			{
				F.ActiveKenyonCells.Add(static_cast<int32>(Value->AsNumber()));
			}
		}
		FDateTime::ParseIso8601(*J->GetStringField(TEXT("created")), F.Created);
		FDateTime::ParseIso8601(*J->GetStringField(TEXT("changed")), F.Changed);
		F.Reminder = static_cast<EOptiReminder>(J->GetIntegerField(TEXT("reminder")));
		FDateTime::ParseIso8601(*J->GetStringField(TEXT("remindAt")), F.RemindAt);
		F.Note = J->GetStringField(TEXT("note"));
		F.bDontSuggestAgain = J->GetBoolField(TEXT("dontSuggestAgain"));
		return F;
	}
}

FString FOptiNotebook::Path() const
{
	const FString Base = bSharedWithTeam ? FPaths::ProjectConfigDir() : FPaths::ProjectSavedDir();
	return FPaths::ConvertRelativePathToFull(Base / TEXT("OptiCompanion") / TEXT("Notebook.json"));
}

FString FOptiNotebook::CaptureDirectory() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OptiCompanion") / TEXT("Captures"));
}

FString FOptiNotebook::TrialsPath() const
{
	return FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OptiCompanion") / TEXT("Trials.json"));
}

void FOptiNotebook::LoadTrials()
{
	Trials.Reset();
	FString Text;
	TSharedPtr<FJsonObject> Root;
	if (!FFileHelper::LoadFileToString(Text, *TrialsPath()) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items;
	if (Root->TryGetArrayField(TEXT("trials"), Items))
	{
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			const TSharedPtr<FJsonObject>& J = Item->AsObject();
			FOptiTrial& T = Trials.AddDefaulted_GetRef();
			FDateTime::ParseIso8601(*J->GetStringField(TEXT("time")), T.Time);
			T.ActionId = FName(*J->GetStringField(TEXT("action")));
			T.Metric = J->GetStringField(TEXT("metric"));
			T.BaselineMs = J->GetNumberField(TEXT("baselineMs"));
			T.GainMs = J->GetNumberField(TEXT("gainMs"));
			T.bSignificant = J->GetBoolField(TEXT("significant"));
			T.Visual = static_cast<float>(J->GetNumberField(TEXT("visual")));
			T.Outcome = OutcomeFromString(J->GetStringField(TEXT("outcome")));
			T.bWhileWorking = J->GetBoolField(TEXT("whileWorking"));
			T.ContextKey = J->GetStringField(TEXT("context"));
		}
	}
}

void FOptiNotebook::SaveTrials() const
{
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FOptiTrial& T : Trials)
	{
		TSharedRef<FJsonObject> J = MakeShared<FJsonObject>();
		J->SetStringField(TEXT("time"), T.Time.ToIso8601());
		J->SetStringField(TEXT("action"), T.ActionId.ToString());
		J->SetStringField(TEXT("metric"), T.Metric);
		J->SetNumberField(TEXT("baselineMs"), T.BaselineMs);
		J->SetNumberField(TEXT("gainMs"), T.GainMs);
		J->SetBoolField(TEXT("significant"), T.bSignificant);
		J->SetNumberField(TEXT("visual"), T.Visual);
		J->SetStringField(TEXT("outcome"), LexToString(T.Outcome));
		J->SetBoolField(TEXT("whileWorking"), T.bWhileWorking);
		J->SetStringField(TEXT("context"), T.ContextKey);
		Items.Add(MakeShared<FJsonValueObject>(J));
	}
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetArrayField(TEXT("trials"), Items);
	FString Text;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(TrialsPath()), true);
	FFileHelper::SaveStringToFile(Text, *TrialsPath(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

void FOptiNotebook::AddTrial(const FOptiTrial& Trial)
{
	Trials.Add(Trial);
	if (Trials.Num() > MaxTrials)
	{
		Trials.RemoveAt(0, Trials.Num() - MaxTrials);
	}
	SaveTrials();
	OnChanged.Broadcast();
}

void FOptiNotebook::ClearTrials()
{
	Trials.Reset();
	SaveTrials();
	OnChanged.Broadcast();
}

void FOptiNotebook::Clear()
{
	Findings.Reset();
	Trials.Reset();
	Save();
	SaveTrials();
	OnChanged.Broadcast();
}

void FOptiNotebook::Load(bool bShared)
{
	bSharedWithTeam = bShared;
	Findings.Reset();
	LoadTrials();

	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *Path()))
	{
		return;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid())
	{
		return;
	}
	const TArray<TSharedPtr<FJsonValue>>* Items;
	if (Root->TryGetArrayField(TEXT("findings"), Items))
	{
		for (const TSharedPtr<FJsonValue>& Item : *Items)
		{
			Findings.Add(MakeShared<FOptiFinding>(FromJson(Item->AsObject())));
		}
	}
}

void FOptiNotebook::Save() const
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const TSharedRef<FOptiFinding>& Finding : Findings)
	{
		Items.Add(MakeShared<FJsonValueObject>(ToJson(*Finding)));
	}
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetArrayField(TEXT("findings"), Items);

	FString Text;
	FJsonSerializer::Serialize(Root, TJsonWriterFactory<>::Create(&Text));
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path()), true);
	FFileHelper::SaveStringToFile(Text, *Path(), FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

TSharedRef<FOptiFinding> FOptiNotebook::Add(const FOptiFinding& Finding)
{
	TSharedRef<FOptiFinding> Entry = MakeShared<FOptiFinding>(Finding);
	if (!Entry->Id.IsValid())
	{
		Entry->Id = FGuid::NewGuid();
	}
	Entry->Created = Entry->Changed = FDateTime::Now();
	Findings.Add(Entry);
	Save();
	OnChanged.Broadcast();
	return Entry;
}

TSharedPtr<FOptiFinding> FOptiNotebook::Find(const FGuid& Id) const
{
	const TSharedRef<FOptiFinding>* Found = Findings.FindByPredicate([&Id](const TSharedRef<FOptiFinding>& F) { return F->Id == Id; });
	return Found ? TSharedPtr<FOptiFinding>(*Found) : nullptr;
}

TSharedPtr<FOptiFinding> FOptiNotebook::FindOpenForAction(FName ActionId) const
{
	const TSharedRef<FOptiFinding>* Found = Findings.FindByPredicate([ActionId](const TSharedRef<FOptiFinding>& F)
	{
		return F->Kind == EOptiFindingKind::Optimization && F->ActionId == ActionId && F->IsOpen();
	});
	return Found ? TSharedPtr<FOptiFinding>(*Found) : nullptr;
}

void FOptiNotebook::SetState(FOptiFinding& Finding, EOptiFindingState State)
{
	Finding.State = State;
	Finding.Changed = FDateTime::Now();
	Save();
	OnChanged.Broadcast();
}

int32 FOptiNotebook::CountNew() const
{
	return Findings.FilterByPredicate([](const TSharedRef<FOptiFinding>& F) { return F->State == EOptiFindingState::New; }).Num();
}

double FOptiNotebook::TotalGainMs() const
{
	double Total = 0.0;
	for (const TSharedRef<FOptiFinding>& Finding : Findings)
	{
		if (Finding->Kind == EOptiFindingKind::Optimization && Finding->IsDone())
		{
			Total += Finding->GainMs;
		}
	}
	return Total;
}
