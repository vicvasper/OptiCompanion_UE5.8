#pragma once

#include "CoreMinimal.h"
#include "OptiActions.h"

class FJsonObject;

/**
 * Everything the fly says, loaded from Resources/Phrases/<language>.json. Keeping the text out of the code
 * lets anyone translate the plugin or change the fly's personality without recompiling.
 */
class FOptiPhrases
{
public:
	static FOptiPhrases& Get();

	void Reload();

	/** Plain UI text. Unknown keys fall back to English, then to the key itself. */
	FText Text(const FString& Key, const FFormatNamedArguments& Args = FFormatNamedArguments()) const;

	/** A notice line in the configured tone, picking a random variant so the fly does not repeat itself. */
	FText Notice(const FString& Key, const FFormatNamedArguments& Args) const;

	FText Action(FName ActionId) const;
	FText Sector(EOptiSector Sector) const;

	/** "the bee" / "la abeja", or "Bee" / "Abeja" as a title. Unknown themes use their folder name. */
	FText MascotName(const FString& Theme, bool bTitle) const;

private:
	/** Every text can say {mascot} or {Mascot}; they are filled in from the chosen theme. */
	FFormatNamedArguments WithMascot(const FFormatNamedArguments& Args) const;

	FString Lookup(const TSharedPtr<FJsonObject>& Table, const FString& Key, bool bNotice) const;

	TSharedPtr<FJsonObject> Current;
	TSharedPtr<FJsonObject> English;
};

/** Shorthand used across the editor UI. */
inline FText OptiText(const TCHAR* Key) { return FOptiPhrases::Get().Text(Key); }
