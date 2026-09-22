#include "OptiCompanionSettings.h"
#include "OptiStyle.h"

UOptiCompanionSettings::UOptiCompanionSettings()
{
	SectionName = TEXT("OptiCompanion");
}

TArray<FString> UOptiCompanionSettings::GetMascotOptions()
{
	return FOptiStyle::GetMascots();
}
