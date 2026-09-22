#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"
#include "Framework/Docking/TabManager.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"
#include "OptiCompanion.h"
#include "OptiCompanionModule.h"
#include "OptiCompanionSettings.h"
#include "OptiPhrases.h"
#include "OptiStyle.h"
#include "OptiTrapLevel.h"
#include "OptiInventory.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInterface.h"
#include "SOptiNotebook.h"

#define LOCTEXT_NAMESPACE "OptiCompanionEditor"

class FOptiCompanionEditorModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		if (IsRunningCommandlet() || !FSlateApplication::IsInitialized())
		{
			return; // no editor UI to live in (cooking, -game, automation)
		}
		FOptiStyle::Initialize();

		// Shader instruction counts, as OptiLogger reads them: the material editor's own statistics. Cached,
		// because the first query of a material can be slow and the fly sniffs often.
		OptiInventory::SetMaterialInstructionCounter([](UMaterialInterface* Material) -> int32
		{
			static TMap<TWeakObjectPtr<UMaterialInterface>, int32> Cache;
			if (const int32* Found = Cache.Find(Material))
			{
				return *Found;
			}
			const FMaterialStatistics Statistics = UMaterialEditingLibrary::GetStatistics(Material);
			return Cache.Add(Material, Statistics.NumPixelShaderInstructions);
		});

		FGlobalTabmanager::Get()->RegisterNomadTabSpawner(SOptiNotebook::TabName, FOnSpawnTab::CreateLambda([](const FSpawnTabArgs&)
		{
			return SNew(SDockTab).TabRole(ETabRole::NomadTab)[SNew(SOptiNotebook)];
		}))
		.SetDisplayName(OptiText(TEXT("ui.notebook.tab")))
		.SetIcon(FSlateIcon(FOptiStyle::GetStyleSetName(), "Opti.Fly.Icon"))
		.SetMenuType(ETabSpawnerMenuType::Hidden);

		FOptiCompanion::Create();
		UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FOptiCompanionEditorModule::RegisterMenus));
		bStarted = true;
	}

	virtual void ShutdownModule() override
	{
		if (!bStarted)
		{
			return;
		}
		UToolMenus::UnRegisterStartupCallback(this);
		UToolMenus::UnregisterOwner(this);
		if (FSlateApplication::IsInitialized())
		{
			FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(SOptiNotebook::TabName);
		}
		FOptiCompanion::Destroy();
		OptiInventory::SetMaterialInstructionCounter(nullptr);
		FOptiStyle::Shutdown();
	}

private:
	void RegisterMenus()
	{
		FToolMenuOwnerScoped OwnerScoped(this);

		UToolMenu* Tools = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.MainMenu.Tools"));
		FToolMenuSection& Section = Tools->AddSection(TEXT("OptiCompanion"), LOCTEXT("SectionLabel", "OptiCompanion"));

		Section.AddMenuEntry(TEXT("OptiCompanionNotebook"), OptiText(TEXT("ui.notebook.tab")), FText::GetEmpty(),
			FSlateIcon(FOptiStyle::GetStyleSetName(), "Opti.Fly.Icon"),
			FUIAction(FExecuteAction::CreateLambda([]() { if (auto C = FOptiCompanion::Get()) { C->OpenNotebook(); } })));

		Section.AddMenuEntry(TEXT("OptiCompanionNap"), OptiText(TEXT("ui.nap_now")), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { if (auto C = FOptiCompanion::Get()) { C->NapNow(); } })));

		Section.AddMenuEntry(TEXT("OptiCompanionShowFly"), OptiText(TEXT("ui.show_fly")), FText::GetEmpty(), FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([]()
				{
					UOptiCompanionSettings* S = GetMutableDefault<UOptiCompanionSettings>();
					S->Presence = S->Presence == EOptiPresence::Mascot ? EOptiPresence::Professional : EOptiPresence::Mascot;
					if (auto C = FOptiCompanion::Get()) { C->ApplySettingsChange(); }
				}),
				FCanExecuteAction(),
				FIsActionChecked::CreateLambda([]() { return GetDefault<UOptiCompanionSettings>()->Presence == EOptiPresence::Mascot; })),
			EUserInterfaceActionType::ToggleButton);

		Section.AddMenuEntry(TEXT("OptiCompanionNoise"),
			LOCTEXT("NoiseLabel", "Measure Machine Noise (A/A)"),
			LOCTEXT("NoiseTooltip", "Runs the probe without changing anything to see how much this machine's timings wander. Keep the viewport visible and don't touch the editor for about a minute."),
			FSlateIcon(),
			FUIAction(
				FExecuteAction::CreateLambda([]()
				{
					FString Error;
					if (!FOptiCompanionModule::Get().StartProbe(FOptiProbeSettings(), Error))
					{
						UE_LOG(LogOptiCompanion, Warning, TEXT("%s"), *Error);
					}
				}),
				FCanExecuteAction::CreateLambda([]() { return !FOptiCompanionModule::Get().IsProbeRunning(); })));

		Section.AddMenuEntry(TEXT("OptiCompanionTrap"),
			LOCTEXT("TrapLabel", "Build Trap Level"),
			LOCTEXT("TrapTooltip", "Creates /Game/OptiTrap/TrapLevel, a small level with planted performance problems, to check what the fly finds."),
			FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { OptiTrap::Build(); })));

		Section.AddMenuEntry(TEXT("OptiCompanionExport"), OptiText(TEXT("ui.export_brain")), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { if (auto C = FOptiCompanion::Get()) { C->ExportBrain(); } })));
		Section.AddMenuEntry(TEXT("OptiCompanionImport"), OptiText(TEXT("ui.import_brain")), FText::GetEmpty(), FSlateIcon(),
			FUIAction(FExecuteAction::CreateLambda([]() { if (auto C = FOptiCompanion::Get()) { C->ImportBrain(); } })));

		// Status bar: the professional-mode face of the fly, useful in mascot mode too.
		UToolMenu* StatusBar = UToolMenus::Get()->ExtendMenu(TEXT("LevelEditor.StatusBar.ToolBar"));
		FToolMenuSection& StatusSection = StatusBar->AddSection(TEXT("OptiCompanion"), FText::GetEmpty(), FToolMenuInsert(NAME_None, EToolMenuInsertType::First));
		FToolMenuEntry Entry = FToolMenuEntry::InitToolBarButton(
			TEXT("OptiCompanionStatus"),
			FUIAction(FExecuteAction::CreateLambda([]() { if (auto C = FOptiCompanion::Get()) { C->OpenNotebook(); } })),
			MakeAttributeLambda([]()
			{
				TSharedPtr<FOptiCompanion> C = FOptiCompanion::Get();
				if (!C.IsValid())
				{
					return FText::GetEmpty();
				}
				const int32 New = C->GetNotebook().CountNew();
				return New > 0 ? FText::Format(LOCTEXT("StatusWithCount", "{0} ({1})"), C->GetStatusText(), New) : C->GetStatusText();
			}),
			OptiText(TEXT("ui.notebook.tab")),
			FSlateIcon(FOptiStyle::GetStyleSetName(), "Opti.Fly.Icon"));
		Entry.StyleNameOverride = "CalloutToolbar";
		StatusSection.AddEntry(Entry);
	}

	bool bStarted = false;
};

IMPLEMENT_MODULE(FOptiCompanionEditorModule, OptiCompanionEditor)

#undef LOCTEXT_NAMESPACE
