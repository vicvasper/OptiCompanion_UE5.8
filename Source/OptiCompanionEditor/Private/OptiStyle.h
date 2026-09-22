#pragma once

#include "CoreMinimal.h"
#include "Styling/SlateStyle.h"

/** Brushes and colours for the fly, its bubble and the notebook. */
class FOptiStyle
{
public:
	static void Initialize();
	static void Shutdown();
	static const ISlateStyle& Get();
	static FName GetStyleSetName();

	/** Mascot themes found in Resources/Mascots (folder names). */
	static const TArray<FString>& GetMascots();
	/** The chosen mascot, or the bee (or the first theme) when the setting names a missing one. */
	static FString CurrentMascot();
	/** Whether the current mascot travels by flying; wingless ones shrink away and pop up at the destination. */
	static bool CurrentMascotFlies();
	/** "Body", "Wings", "LegsFront", "Icon" or "Icon.Large" of the current mascot; switches live with the setting. */
	static const FSlateBrush* MascotBrush(const TCHAR* Part);

	// Palette shared with the mockup.
	static const FLinearColor Accent;
	static const FLinearColor Good;
	static const FLinearColor Warn;
	static const FLinearColor Info;
	static const FLinearColor Bad;
	static const FLinearColor Paper;
	static const FLinearColor Ink;
	static const FLinearColor InkDim;

private:
	static TSharedPtr<FSlateStyleSet> StyleSet;
	static TArray<FString> Mascots;
	static TSet<FString> GroundedMascots;
};
