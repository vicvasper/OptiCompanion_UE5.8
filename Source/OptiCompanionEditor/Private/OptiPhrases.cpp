#include "OptiPhrases.h"
#include "OptiCompanionSettings.h"
#include "OptiStyle.h"
#include "Dom/JsonObject.h"
#include "Interfaces/IPluginManager.h"
#include "Internationalization/Culture.h"
#include "Internationalization/Internationalization.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
	TSharedPtr<FJsonObject> LoadTable(const FString& Language)
	{
		TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("OptiCompanion"));
		if (!Plugin.IsValid())
		{
			return nullptr;
		}
		FString Text;
		const FString Path = Plugin->GetBaseDir() / TEXT("Resources") / TEXT("Phrases") / (Language + TEXT(".json"));
		if (!FFileHelper::LoadFileToString(Text, *Path))
		{
			return nullptr;
		}
		TSharedPtr<FJsonObject> Table;
		FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Table);
		return Table;
	}

	FString ResolveLanguage()
	{
		switch (GetDefault<UOptiCompanionSettings>()->Language)
		{
		case EOptiLanguage::English: return TEXT("en");
		case EOptiLanguage::Spanish: return TEXT("es");
		default:
		{
			const FString Editor = FInternationalization::Get().GetCurrentLanguage()->GetTwoLetterISOLanguageName();
			return Editor == TEXT("es") ? TEXT("es") : TEXT("en");
		}
		}
	}
}

FOptiPhrases& FOptiPhrases::Get()
{
	static FOptiPhrases Instance;
	if (!Instance.English.IsValid())
	{
		Instance.Reload();
	}
	return Instance;
}

void FOptiPhrases::Reload()
{
	English = LoadTable(TEXT("en"));
	const FString Language = ResolveLanguage();
	Current = Language == TEXT("en") ? English : LoadTable(Language);
	if (!Current.IsValid())
	{
		Current = English;
	}
}

FString FOptiPhrases::Lookup(const TSharedPtr<FJsonObject>& Table, const FString& Key, bool bNotice) const
{
	if (!Table.IsValid())
	{
		return FString();
	}
	const TSharedPtr<FJsonValue> Value = Table->TryGetField(Key);
	if (!Value.IsValid())
	{
		return FString();
	}
	if (Value->Type == EJson::String)
	{
		return Value->AsString();
	}

	// Notices: { "sober": [...], "friendly": [...] } or a plain array of variants.
	TArray<TSharedPtr<FJsonValue>> Variants;
	if (Value->Type == EJson::Object)
	{
		const bool bFriendly = GetDefault<UOptiCompanionSettings>()->Tone == EOptiTone::Friendly;
		const TSharedPtr<FJsonObject> Tones = Value->AsObject();
		const TArray<TSharedPtr<FJsonValue>>* Found = nullptr;
		if (!Tones->TryGetArrayField(bFriendly ? TEXT("friendly") : TEXT("sober"), Found))
		{
			Tones->TryGetArrayField(TEXT("sober"), Found);
		}
		if (Found)
		{
			Variants = *Found;
		}
	}
	else if (Value->Type == EJson::Array)
	{
		Variants = Value->AsArray();
	}
	if (Variants.IsEmpty())
	{
		return FString();
	}
	const int32 Pick = bNotice ? FMath::RandHelper(Variants.Num()) : 0;
	return Variants[Pick]->AsString();
}

FText FOptiPhrases::Text(const FString& Key, const FFormatNamedArguments& Args) const
{
	FString Pattern = Lookup(Current, Key, false);
	if (Pattern.IsEmpty())
	{
		Pattern = Lookup(English, Key, false);
	}
	if (Pattern.IsEmpty())
	{
		return FText::FromString(Key);
	}
	return FText::Format(FTextFormat::FromString(Pattern), WithMascot(Args));
}

FFormatNamedArguments FOptiPhrases::WithMascot(const FFormatNamedArguments& Args) const
{
	FFormatNamedArguments Out = Args;
	const FString Theme = FOptiStyle::CurrentMascot();
	if (!Out.Contains(TEXT("mascot")))
	{
		Out.Add(TEXT("mascot"), MascotName(Theme, false));
	}
	if (!Out.Contains(TEXT("Mascot")))
	{
		Out.Add(TEXT("Mascot"), MascotName(Theme, true));
	}
	return Out;
}

FText FOptiPhrases::MascotName(const FString& Theme, bool bTitle) const
{
	const FString Key = FString::Printf(TEXT("mascot.%s%s"), *Theme, bTitle ? TEXT(".title") : TEXT(""));
	FString Name = Lookup(Current, Key, false);
	if (Name.IsEmpty())
	{
		Name = Lookup(English, Key, false);
	}
	return FText::FromString(Name.IsEmpty() ? Theme : Name);
}

FText FOptiPhrases::Notice(const FString& Key, const FFormatNamedArguments& Args) const
{
	FString Pattern = Lookup(Current, Key, true);
	if (Pattern.IsEmpty())
	{
		Pattern = Lookup(English, Key, true);
	}
	return Pattern.IsEmpty() ? FText::FromString(Key) : FText::Format(FTextFormat::FromString(Pattern), WithMascot(Args));
}

FText FOptiPhrases::Action(FName ActionId) const
{
	return Text(TEXT("action.") + ActionId.ToString());
}

FText FOptiPhrases::Sector(EOptiSector Sector) const
{
	return Text(FString(TEXT("sector.")) + LexToString(Sector));
}
