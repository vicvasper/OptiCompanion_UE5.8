#include "OptiStyle.h"
#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Brushes/SlateColorBrush.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Interfaces/IPluginManager.h"
#include "Styling/SlateStyleMacros.h"
#include "Styling/SlateStyleRegistry.h"
#include "Styling/CoreStyle.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "OptiCompanionSettings.h"

TSharedPtr<FSlateStyleSet> FOptiStyle::StyleSet;

const FLinearColor FOptiStyle::Accent = FLinearColor::FromSRGBColor(FColor(0xE2, 0x60, 0x3F));
const FLinearColor FOptiStyle::Good = FLinearColor::FromSRGBColor(FColor(0x65, 0xB4, 0x7E));
const FLinearColor FOptiStyle::Warn = FLinearColor::FromSRGBColor(FColor(0xD7, 0xA5, 0x48));
const FLinearColor FOptiStyle::Info = FLinearColor::FromSRGBColor(FColor(0x74, 0xA1, 0xD7));
const FLinearColor FOptiStyle::Bad = FLinearColor::FromSRGBColor(FColor(0xD0, 0x61, 0x5A));
const FLinearColor FOptiStyle::Paper = FLinearColor::FromSRGBColor(FColor(0xEE, 0xF0, 0xEA));
const FLinearColor FOptiStyle::Ink = FLinearColor::FromSRGBColor(FColor(0x1B, 0x1D, 0x1F));
const FLinearColor FOptiStyle::InkDim = FLinearColor::FromSRGBColor(FColor(0x5B, 0x60, 0x66));

FName FOptiStyle::GetStyleSetName()
{
	static const FName Name(TEXT("OptiCompanionStyle"));
	return Name;
}

const ISlateStyle& FOptiStyle::Get()
{
	return *StyleSet;
}

void FOptiStyle::Initialize()
{
	if (StyleSet.IsValid())
	{
		return;
	}
	StyleSet = MakeShared<FSlateStyleSet>(GetStyleSetName());
	StyleSet->SetContentRoot(IPluginManager::Get().FindPlugin(TEXT("OptiCompanion"))->GetBaseDir() / TEXT("Resources"));

	// Every folder in Resources/Mascots with a body.svg is a mascot theme: body, wings, legs_front and icon,
	// all drawn on a 64x64 canvas with the head up. Anyone can add one without touching the code.
	auto Svg = [](const FString& Path, const FVector2D& Size)
	{
		return new FSlateVectorImageBrush(Path, Size);
	};
	const FString MascotRoot = StyleSet->GetContentRootDir() / TEXT("Mascots");
	TArray<FString> Folders;
	IFileManager::Get().FindFiles(Folders, *(MascotRoot / TEXT("*")), false, true);
	Folders.Sort();
	for (const FString& Folder : Folders)
	{
		const FString Dir = MascotRoot / Folder;
		if (!FPaths::FileExists(Dir / TEXT("body.svg")))
		{
			continue;
		}
		Mascots.Add(Folder);
		// Optional theme.json: { "flies": false } for mascots without wings.
		FString ThemeText;
		TSharedPtr<FJsonObject> Theme;
		bool bFlies = true;
		if (FFileHelper::LoadFileToString(ThemeText, *(Dir / TEXT("theme.json")))
			&& FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(ThemeText), Theme) && Theme.IsValid()
			&& Theme->TryGetBoolField(TEXT("flies"), bFlies) && !bFlies)
		{
			GroundedMascots.Add(Folder);
		}
		const FString Prefix = TEXT("Opti.Mascot.") + Folder;
		auto Part = [&Dir](const TCHAR* File) { const FString Path = Dir / File; return FPaths::FileExists(Path) ? Path : Dir / TEXT("body.svg"); };
		StyleSet->Set(*(Prefix + TEXT(".Body")), Svg(Dir / TEXT("body.svg"), FVector2D(64.0, 64.0)));
		StyleSet->Set(*(Prefix + TEXT(".Wings")), Svg(Part(TEXT("wings.svg")), FVector2D(64.0, 64.0)));
		StyleSet->Set(*(Prefix + TEXT(".LegsFront")), Svg(Part(TEXT("legs_front.svg")), FVector2D(64.0, 64.0)));
		StyleSet->Set(*(Prefix + TEXT(".Icon")), Svg(Part(TEXT("icon.svg")), FVector2D(16.0, 16.0)));
		StyleSet->Set(*(Prefix + TEXT(".Icon.Large")), Svg(Part(TEXT("icon.svg")), FVector2D(40.0, 40.0)));
	}

	// Menus, tabs and the status bar keep the icon of the mascot chosen when the editor started.
	const FString Startup = CurrentMascot();
	StyleSet->Set("Opti.Fly.Icon", Svg(MascotRoot / Startup / TEXT("icon.svg"), FVector2D(16.0, 16.0)));
	StyleSet->Set("Opti.Fly.Icon.Large", Svg(MascotRoot / Startup / TEXT("icon.svg"), FVector2D(40.0, 40.0)));

	StyleSet->Set("Opti.Bubble", new FSlateRoundedBoxBrush(Paper, 10.f, FLinearColor(0.f, 0.f, 0.f, 0.35f), 1.f));
	StyleSet->Set("Opti.Badge", new FSlateRoundedBoxBrush(Accent, 8.f));
	StyleSet->Set("Opti.Pill", new FSlateRoundedBoxBrush(FLinearColor::White, 3.f));
	StyleSet->Set("Opti.Card", new FSlateRoundedBoxBrush(FLinearColor(0.02f, 0.02f, 0.025f, 1.f), 4.f));
	StyleSet->Set("Opti.Strip", new FSlateRoundedBoxBrush(FLinearColor::White, 1.f));
	FSlateColorBrush* Strike = new FSlateColorBrush(Good.CopyWithNewOpacity(0.8f));
	Strike->ImageSize = FVector2D(1.0, 1.5);
	StyleSet->Set("Opti.Strike", Strike);

	StyleSet->Set("Opti.Bubble.Text", FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
		.SetFont(FCoreStyle::GetDefaultFontStyle("Bold", 12))
		.SetColorAndOpacity(Ink));
	StyleSet->Set("Opti.Bubble.Small", FTextBlockStyle(FCoreStyle::Get().GetWidgetStyle<FTextBlockStyle>("NormalText"))
		.SetFont(FCoreStyle::GetDefaultFontStyle("Regular", 9))
		.SetColorAndOpacity(InkDim));

	FSlateStyleRegistry::RegisterSlateStyle(*StyleSet);
}

TArray<FString> FOptiStyle::Mascots;
TSet<FString> FOptiStyle::GroundedMascots;

bool FOptiStyle::CurrentMascotFlies()
{
	return !GroundedMascots.Contains(CurrentMascot());
}

const TArray<FString>& FOptiStyle::GetMascots()
{
	return Mascots;
}

FString FOptiStyle::CurrentMascot()
{
	const FString& Chosen = GetDefault<UOptiCompanionSettings>()->Mascot;
	if (Mascots.Contains(Chosen))
	{
		return Chosen;
	}
	return Mascots.Contains(TEXT("oc")) ? FString(TEXT("oc")) : (Mascots.IsEmpty() ? FString() : Mascots[0]);
}

const FSlateBrush* FOptiStyle::MascotBrush(const TCHAR* Part)
{
	return StyleSet->GetBrush(*FString::Printf(TEXT("Opti.Mascot.%s.%s"), *CurrentMascot(), Part));
}

void FOptiStyle::Shutdown()
{
	if (StyleSet.IsValid())
	{
		FSlateStyleRegistry::UnRegisterSlateStyle(*StyleSet);
		StyleSet.Reset();
	}
}
