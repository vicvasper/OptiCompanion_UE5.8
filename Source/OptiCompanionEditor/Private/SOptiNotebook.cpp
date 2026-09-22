#include "SOptiNotebook.h"
#include "OptiActions.h"
#include "OptiCompanion.h"
#include "OptiCompanionSettings.h"
#include "OptiPhrases.h"
#include "OptiStyle.h"

#include "Brushes/SlateImageBrush.h"
#include "Engine/Texture2D.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Docking/TabManager.h"
#include "HAL/IConsoleManager.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "ImageUtils.h"
#include "ISettingsModule.h"
#include "Misc/PackageName.h"
#include "Styling/AppStyle.h"
#include "UObject/StrongObjectPtr.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Input/SSlider.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "SOptiInventory.h"
#include "Widgets/SCanvas.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SWindow.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "OptiCompanionNotebook"

const FName SOptiNotebook::TabName(TEXT("OptiCompanionNotebook"));

namespace
{
	TWeakPtr<SOptiNotebook> GOpenNotebook;
	int32 GRequestedPage = INDEX_NONE;

	FAutoConsoleCommand NotebookCommand(
		TEXT("Opti.Notebook"),
		TEXT("Opens the OptiCompanion notebook. Optional page: findings, tested or inventory."),
		FConsoleCommandWithArgsDelegate::CreateLambda([](const TArray<FString>& Args)
		{
			const FString Page = Args.IsEmpty() ? FString() : Args[0].ToLower();
			SOptiNotebook::Open(Page == TEXT("tested") ? SOptiNotebook::TestedPage
				: Page == TEXT("inventory") ? SOptiNotebook::InventoryPage : SOptiNotebook::FindingsPage);
		}));
}

void SOptiNotebook::Open(int32 NewPage)
{
	GRequestedPage = NewPage;
	FGlobalTabmanager::Get()->TryInvokeTab(FTabId(TabName));
	if (TSharedPtr<SOptiNotebook> Notebook = GOpenNotebook.Pin())
	{
		Notebook->Page = NewPage;
		GRequestedPage = INDEX_NONE;
	}
}

namespace
{
	FText Ms(double Value)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = FMath::Abs(Value) >= 10.0 ? 1 : 2;
		Options.MaximumFractionalDigits = Options.MinimumFractionalDigits;
		return FText::AsNumber(Value, &Options);
	}

	FLinearColor StateColor(const FOptiFinding& Finding)
	{
		if (Finding.Kind == EOptiFindingKind::Regression && Finding.State == EOptiFindingState::New)
		{
			return FOptiStyle::Bad;
		}
		switch (Finding.State)
		{
		case EOptiFindingState::New: return FOptiStyle::Accent;
		case EOptiFindingState::Postponed: return FOptiStyle::Warn;
		case EOptiFindingState::Applied:
		case EOptiFindingState::ResolvedByUser: return FOptiStyle::Good;
		case EOptiFindingState::Stale: return FOptiStyle::Info;
		default: return FLinearColor(0.35f, 0.35f, 0.38f);
		}
	}

	FText Title(const FOptiFinding& Finding)
	{
		if (Finding.Kind == EOptiFindingKind::Regression)
		{
			return FText::Format(LOCTEXT("RegressionTitle", "{0}: {1}"), OptiText(TEXT("kind.Regression")), FText::FromString(FPackageName::GetShortName(Finding.Asset)));
		}
		if (Finding.IsBlueprintFinding())
		{
			return FText::Format(LOCTEXT("BlueprintTitle", "{0}: {1}"), FText::FromString(FSoftObjectPath(Finding.Blueprint).GetAssetName()),
				FOptiPhrases::Get().Action(Finding.ActionId));
		}
		return FOptiPhrases::Get().Action(Finding.ActionId);
	}

	TSharedRef<SWidget> Pill(const FText& Text, const FLinearColor& Color)
	{
		return SNew(SBorder)
			.BorderImage(FOptiStyle::Get().GetBrush("Opti.Pill"))
			.BorderBackgroundColor(Color.CopyWithNewOpacity(0.18f))
			.Padding(FMargin(6.f, 1.f))
			[
				SNew(STextBlock).Text(Text).ColorAndOpacity(Color).Font(FAppStyle::GetFontStyle("SmallFontBold"))
			];
	}

	TSharedRef<SWidget> TextButton(const FText& Label, bool bPrimary, TFunction<void()> OnClick)
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), bPrimary ? "PrimaryButton" : "Button")
			.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(Label)
			];
	}

	TSharedRef<SWidget> LaterMenu(FGuid Id)
	{
		auto Postpone = [Id](EOptiReminder Reminder)
		{
			return FUIAction(FExecuteAction::CreateLambda([Id, Reminder]()
			{
				if (TSharedPtr<FOptiCompanion> C = FOptiCompanion::Get()) { C->PostponeFinding(Id, Reminder); }
			}));
		};
		FMenuBuilder Menu(true, nullptr);
		Menu.AddMenuEntry(OptiText(TEXT("later.twohours")), FText::GetEmpty(), FSlateIcon(), Postpone(EOptiReminder::InTwoHours));
		Menu.AddMenuEntry(OptiText(TEXT("later.nextsession")), FText::GetEmpty(), FSlateIcon(), Postpone(EOptiReminder::NextSession));
		Menu.AddMenuEntry(OptiText(TEXT("later.slowframe")), FText::GetEmpty(), FSlateIcon(), Postpone(EOptiReminder::WhenFrameIsSlow));
		return Menu.MakeWidget();
	}

	FText ReminderText(const FOptiFinding& Finding)
	{
		switch (Finding.Reminder)
		{
		case EOptiReminder::InTwoHours: return FText::AsTime(Finding.RemindAt, EDateTimeStyle::Short, FText::GetInvariantTimeZone());
		case EOptiReminder::NextSession: return OptiText(TEXT("later.nextsession"));
		case EOptiReminder::WhenFrameIsSlow: return OptiText(TEXT("later.slowframe"));
		default: return FText::GetEmpty();
		}
	}

	/** Loaded PNG plus the brush that shows it; lives as long as the details window. */
	struct FLoadedImage
	{
		TStrongObjectPtr<UTexture2D> Texture;
		FSlateBrush Brush;
		FVector2D Size = FVector2D::ZeroVector;

		bool Load(const FString& Path)
		{
			if (Path.IsEmpty() || !FPaths::FileExists(Path))
			{
				return false;
			}
			UTexture2D* Loaded = FImageUtils::ImportFileAsTexture2D(Path);
			if (!Loaded)
			{
				return false;
			}
			Texture.Reset(Loaded);
			Size = FVector2D(Loaded->GetSizeX(), Loaded->GetSizeY());
			Brush.SetResourceObject(Loaded);
			Brush.ImageSize = Size;
			Brush.DrawAs = ESlateBrushDrawType::Image;
			return true;
		}
	};
}

void OptiOpenFindingDetails(TSharedRef<FOptiFinding> Finding)
{
	struct FState
	{
		FLoadedImage A, B, Diff;
		float Split = 0.5f;
		bool bShowDiff = false;
	};
	TSharedRef<FState> State = MakeShared<FState>();
	const bool bHasImages = State->A.Load(Finding->CaptureA) && State->B.Load(Finding->CaptureB);
	State->Diff.Load(Finding->CaptureDiff);

	constexpr float Width = 600.f;
	const float Height = bHasImages && State->A.Size.X > 0 ? Width * State->A.Size.Y / State->A.Size.X : 0.f;

	TSharedRef<SWidget> Comparison = SNullWidget::NullWidget;
	if (bHasImages)
	{
		Comparison = SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()
			[
				SNew(SBox).WidthOverride(Width).HeightOverride(Height)
				[
					SNew(SOverlay)
					+ SOverlay::Slot()
					[
						SNew(SImage).Image_Lambda([State]() { return State->bShowDiff && State->Diff.Texture.IsValid() ? &State->Diff.Brush : &State->B.Brush; })
					]
					+ SOverlay::Slot().HAlign(HAlign_Left)
					[
						SNew(SBox)
						.Visibility_Lambda([State]() { return State->bShowDiff ? EVisibility::Collapsed : EVisibility::HitTestInvisible; })
						.WidthOverride_Lambda([State, Width]() { return FOptionalSize(Width * State->Split); })
						.Clipping(EWidgetClipping::ClipToBounds)
						[
							SNew(SCanvas)
							+ SCanvas::Slot().Position(FVector2D::ZeroVector).Size(FVector2D(Width, Height))
							[
								SNew(SImage).Image(&State->A.Brush)
							]
						]
					]
					+ SOverlay::Slot().HAlign(HAlign_Left).VAlign(VAlign_Top).Padding(8.f)
					[
						SNew(STextBlock).Text(OptiText(TEXT("details.before"))).ShadowOffset(FVector2D(1.0, 1.0))
					]
					+ SOverlay::Slot().HAlign(HAlign_Right).VAlign(VAlign_Top).Padding(8.f)
					[
						SNew(STextBlock).Text(OptiText(TEXT("details.after"))).ShadowOffset(FVector2D(1.0, 1.0))
					]
				]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SSlider)
					.Value_Lambda([State]() { return State->Split; })
					.OnValueChanged_Lambda([State](float Value) { State->Split = Value; })
					.ToolTipText(OptiText(TEXT("details.slider")))
				]
				+ SHorizontalBox::Slot().AutoWidth().Padding(10.f, 0.f, 0.f, 0.f)
				[
					SNew(SCheckBox)
					.IsChecked_Lambda([State]() { return State->bShowDiff ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
					.OnCheckStateChanged_Lambda([State](ECheckBoxState NewState) { State->bShowDiff = NewState == ECheckBoxState::Checked; })
					[
						SNew(STextBlock).Text(OptiText(TEXT("details.diff")))
					]
				]
			];
	}
	else
	{
		Comparison = SNew(STextBlock).Text(OptiText(TEXT("details.no_capture"))).ColorAndOpacity(FSlateColor::UseSubduedForeground());
	}

	auto Fact = [](const TCHAR* Key, const FText& Value)
	{
		return SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(OptiText(Key)).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Font(FAppStyle::GetFontStyle("SmallFont"))]
			+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(Value).Font(FAppStyle::GetFontStyle("NormalFontBold"))];
	};

	TSharedRef<SVerticalBox> Passes = SNew(SVerticalBox);
	for (const FOptiPassDelta& Pass : Finding->TopPasses)
	{
		Passes->AddSlot().AutoHeight()
		[
			SNew(STextBlock).Text(FText::Format(LOCTEXT("PassLine", "{0}: {1} ms"), FText::FromString(Pass.Pass), Ms(Pass.DeltaMs)))
		];
	}
	TSharedRef<SVerticalBox> CVars = SNew(SVerticalBox);
	if (Finding->IsSceneFinding())
	{
		// Scene findings: list the components, shortest path form, a few at a time.
		const TArray<FOptiSceneEdit> Edits = OptiScene::Deserialize(Finding->SceneEdits);
		constexpr int32 Shown = 12;
		for (int32 Index = 0; Index < FMath::Min(Edits.Num(), Shown); ++Index)
		{
			const FOptiSceneEdit& Edit = Edits[Index];
			FString Owner = Edit.ComponentPath;
			int32 Dot;
			if (Owner.FindLastChar(TEXT(':'), Dot)) { Owner.RightChopInline(Dot + 1); }
			CVars->AddSlot().AutoHeight()
			[
				SNew(STextBlock)
				.Font(FAppStyle::GetFontStyle("MonoFont"))
				.Text(FText::FromString(FString::Printf(TEXT("%s  %s: %s -> %s"), *Owner, *Edit.Property.ToString(), *Edit.Before, *Edit.After)))
			];
		}
		if (Edits.Num() > Shown)
		{
			CVars->AddSlot().AutoHeight()
			[
				SNew(STextBlock).Text(FText::Format(LOCTEXT("MoreEdits", "... and {0} more"), Edits.Num() - Shown)).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			];
		}
	}
	for (int32 Index = 0; Index < Finding->To.Num(); ++Index)
	{
		const FString From = Finding->From.IsValidIndex(Index) ? Finding->From[Index].Value : TEXT("?");
		CVars->AddSlot().AutoHeight()
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("MonoFont"))
			.Text(FText::FromString(FString::Printf(TEXT("%s: %s -> %s"), *Finding->To[Index].Key, *From, *Finding->To[Index].Value)))
		];
	}

	TSharedRef<SWindow> Window = SNew(SWindow)
		.Title(FText::Format(LOCTEXT("DetailsTitle", "{0}: {1}"), OptiText(TEXT("details.title")), Title(*Finding)))
		.SizingRule(ESizingRule::Autosized)
		.SupportsMaximize(false)
		.SupportsMinimize(false);

	TWeakPtr<SWindow> WeakWindow = Window;
	const FGuid Id = Finding->Id;
	const bool bCanApply = Finding->Kind == EOptiFindingKind::Optimization && (Finding->State == EOptiFindingState::New || Finding->State == EOptiFindingState::Postponed);

	Window->SetContent(
		SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Panel"))
		.Padding(16.f)
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot().AutoHeight()[Comparison]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f)
			[
				SNew(SUniformGridPanel).SlotPadding(FMargin(0.f, 0.f, 16.f, 0.f))
				+ SUniformGridPanel::Slot(0, 0)[Fact(TEXT("details.gain"), FText::Format(LOCTEXT("GainValue", "-{0} ms ({1}%) {2}"), Ms(Finding->GainMs), FText::AsNumber(FMath::RoundToInt32(Finding->GainPercent())), FText::FromString(Finding->Metric)))]
				+ SUniformGridPanel::Slot(1, 0)[Fact(TEXT("details.ci"), FText::Format(LOCTEXT("CIValue", "{0} to {1} ms"), Ms(Finding->CILowMs), Ms(Finding->CIHighMs)))]
				+ SUniformGridPanel::Slot(2, 0)[Fact(TEXT("details.visual"), FText::Format(LOCTEXT("VisualValue", "mean {0}, p95 {1}"), FText::AsNumber(Finding->VisualMean), FText::AsNumber(Finding->VisualP95)))]
			]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f)[SNew(STextBlock).Text(OptiText(TEXT("details.passes"))).Font(FAppStyle::GetFontStyle("NormalFontBold"))]
			+ SVerticalBox::Slot().AutoHeight()[Passes]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f, 0.f, 4.f)[SNew(STextBlock).Text(OptiText(TEXT("details.changes"))).Font(FAppStyle::GetFontStyle("NormalFontBold"))]
			+ SVerticalBox::Slot().AutoHeight()[CVars]
			+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 10.f)
			[
				SNew(STextBlock).Text(OptiText(Finding->IsSceneFinding() ? TEXT("details.note_scene") : TEXT("details.note"))).AutoWrapText(true).ColorAndOpacity(FSlateColor::UseSubduedForeground())
			]
			+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
				[
					SNew(SBox).Visibility(bCanApply ? EVisibility::Visible : EVisibility::Collapsed)
					[
						TextButton(OptiText(TEXT("ui.apply")), true, [Id, WeakWindow]()
						{
							if (TSharedPtr<FOptiCompanion> C = FOptiCompanion::Get()) { C->ApplyFinding(Id); }
							if (TSharedPtr<SWindow> W = WeakWindow.Pin()) { W->RequestDestroyWindow(); }
						})
					]
				]
				+ SHorizontalBox::Slot().AutoWidth()
				[
					TextButton(OptiText(TEXT("ui.close")), false, [WeakWindow]()
					{
						if (TSharedPtr<SWindow> W = WeakWindow.Pin()) { W->RequestDestroyWindow(); }
					})
				]
			]
		]);

	// The window owns the loaded textures through the lambdas above; keep State alive with it.
	Window->SetOnWindowClosed(FOnWindowClosed::CreateLambda([State](const TSharedRef<SWindow>&) {}));
	FSlateApplication::Get().AddWindow(Window);
}

// ---------------------------------------------------------------------------------------------- notebook

void SOptiNotebook::Construct(const FArguments& InArgs)
{
	TSharedPtr<FOptiCompanion> C = FOptiCompanion::Get();

	auto BrainLine = [](int32 Line)
	{
		return MakeAttributeLambda([Line]()
		{
			TSharedPtr<FOptiCompanion> Companion = FOptiCompanion::Get();
			if (!Companion.IsValid())
			{
				return FText::GetEmpty();
			}
			const FOptiFlyBrain& Brain = Companion->GetBrain();
			const FOptiFlyBrain::FStats& Stats = Brain.GetStats();
			FOptiPhrases& Phrases = FOptiPhrases::Get();
			switch (Line)
			{
			case 0: return Phrases.Text(TEXT("ui.brain"), { { TEXT("projects"), FText::AsNumber(Stats.Projects) },
				{ TEXT("experiments"), FText::AsNumber(Stats.LongTermExperiments) }, { TEXT("learned"), FText::AsNumber(Brain.LearnedActions()) } });
			case 1: return Phrases.Text(TEXT("ui.brain.project"), { { TEXT("naps"), FText::AsNumber(Stats.Naps) }, { TEXT("focus"), Phrases.Sector(Brain.Focus()) } });
			default: return Phrases.Text(TEXT("ui.brain.wiring"), { { TEXT("wiring"), FText::FromString(Brain.GetConnectome().Source) } });
			}
		});
	};

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 12.f, 12.f, 6.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 8.f, 0.f)
			[
				SNew(SImage).Image_Lambda([]() { return FOptiStyle::MascotBrush(TEXT("Icon")); })
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text_Lambda([]() { return OptiText(TEXT("ui.notebook.title")); }).Font(FAppStyle::GetFontStyle("HeadingExtraSmall"))
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f)
			[
				// Only while the mascot is hidden: the easy way back from professional mode.
				SNew(SBox)
				.Visibility_Lambda([]() { return GetDefault<UOptiCompanionSettings>()->Presence == EOptiPresence::Professional ? EVisibility::Visible : EVisibility::Collapsed; })
				[
					TextButton(OptiText(TEXT("ui.show_fly")), true, []()
					{
						GetMutableDefault<UOptiCompanionSettings>()->Presence = EOptiPresence::Mascot;
						if (auto P = FOptiCompanion::Get()) { P->ApplySettingsChange(); }
					})
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(4.f, 0.f)
			[
				TextButton(OptiText(TEXT("ui.nap_now")), false, []() { if (auto P = FOptiCompanion::Get()) { P->NapNow(); } })
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				TextButton(OptiText(TEXT("ui.settings")), false, []()
				{
					FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings")).ShowViewer(TEXT("Editor"), TEXT("Plugins"), TEXT("OptiCompanion"));
				})
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 0.f)
		[
			SNew(STextBlock)
			.Font(FAppStyle::GetFontStyle("NormalFontBold"))
			.ColorAndOpacity(FOptiStyle::Good)
			.Text_Lambda([]()
			{
				TSharedPtr<FOptiCompanion> P = FOptiCompanion::Get();
				if (!P.IsValid()) { return FText::GetEmpty(); }
				return FText::Format(LOCTEXT("Score", "{0}   ·   {1}"),
					FOptiPhrases::Get().Text(TEXT("ui.score.ms"), { { TEXT("ms"), Ms(P->GetNotebook().TotalGainMs()) }, { TEXT("project"), FText::FromString(FApp::GetProjectName()) } }),
					FOptiPhrases::Get().Text(TEXT("ui.score.open"), { { TEXT("count"), FText::AsNumber(P->GetNotebook().CountNew()) } }));
			})
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 6.f, 12.f, 0.f)
		[
			SNew(STextBlock).Text(BrainLine(0)).ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 0.f)
		[
			SNew(STextBlock).Text(BrainLine(1)).ColorAndOpacity(FSlateColor::UseSubduedForeground())
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 0.f)
		[
			SNew(STextBlock).Text(BrainLine(2)).ColorAndOpacity(FSlateColor::UseSubduedForeground()).AutoWrapText(true)
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 8.f)
		[
			SNew(STextBlock)
			.Text_Lambda([]() { TSharedPtr<FOptiCompanion> P = FOptiCompanion::Get(); return P.IsValid() ? P->GetStatusText() : FText::GetEmpty(); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 0.f, 12.f, 8.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()[MakePageButton(0, TEXT("ui.page.findings"))]
			+ SHorizontalBox::Slot().AutoWidth()[MakePageButton(2, TEXT("ui.page.tested"))]
			+ SHorizontalBox::Slot().AutoWidth()[MakePageButton(1, TEXT("ui.page.inventory"))]
		]
		+ SVerticalBox::Slot().FillHeight(1.f)
		[
			SNew(SWidgetSwitcher)
			.WidgetIndex_Lambda([this]() { return Page; })
			+ SWidgetSwitcher::Slot()
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 0.f, 12.f, 8.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth()[MakeFilterButton(EFilter::All, TEXT("ui.filter.all"))]
					+ SHorizontalBox::Slot().AutoWidth()[MakeFilterButton(EFilter::New, TEXT("ui.filter.new"))]
					+ SHorizontalBox::Slot().AutoWidth()[MakeFilterButton(EFilter::Postponed, TEXT("ui.filter.postponed"))]
					+ SHorizontalBox::Slot().AutoWidth()[MakeFilterButton(EFilter::Done, TEXT("ui.filter.done"))]
					+ SHorizontalBox::Slot().AutoWidth()[MakeFilterButton(EFilter::Dismissed, TEXT("ui.filter.dismissed"))]
				]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(List, SVerticalBox)
					]
				]
			]
			+ SWidgetSwitcher::Slot()
			[
				SNew(SOptiInventory)
			]
			+ SWidgetSwitcher::Slot()
			[
				// Every experiment, including the ones that were not worth telling you about.
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 0.f, 12.f, 8.f)
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
					[
						SNew(STextBlock)
						.ColorAndOpacity(FSlateColor::UseSubduedForeground())
						.AutoWrapText(true)
						.Text_Lambda([]()
						{
							TSharedPtr<FOptiCompanion> P = FOptiCompanion::Get();
							if (!P.IsValid()) { return FText::GetEmpty(); }
							int32 Found = 0, Dry = 0;
							for (const FOptiTrial& Trial : P->GetNotebook().GetTrials())
							{
								Found += Trial.Outcome == EOptiTrialOutcome::Finding;
								Dry += Trial.Outcome == EOptiTrialOutcome::NoGain || Trial.Outcome == EOptiTrialOutcome::TooSmall || Trial.Outcome == EOptiTrialOutcome::Visible;
							}
							const FText Summary = FOptiPhrases::Get().Text(TEXT("ui.tested.summary"), { { TEXT("n"), FText::AsNumber(P->GetNotebook().GetTrials().Num()) },
								{ TEXT("found"), FText::AsNumber(Found) }, { TEXT("dry"), FText::AsNumber(Dry) } });
							return P->IsSceneClean() ? FText::Format(LOCTEXT("TestedClean", "{0}   ·   {1}"), Summary, OptiText(TEXT("status.clean"))) : Summary;
						})
					]
					+ SHorizontalBox::Slot().AutoWidth()
					[
						TextButton(OptiText(TEXT("ui.tested.clear")), false, []() { if (auto P = FOptiCompanion::Get()) { P->GetNotebook().ClearTrials(); } })
					]
				]
				+ SVerticalBox::Slot().FillHeight(1.f)
				[
					SNew(SScrollBox)
					+ SScrollBox::Slot()
					[
						SAssignNew(TestedList, SVerticalBox)
					]
				]
			]
		]
	];

	if (C.IsValid())
	{
		ChangedHandle = C->OnChanged.AddSP(this, &SOptiNotebook::Rebuild);
	}
	GOpenNotebook = SharedThis(this);
	if (GRequestedPage != INDEX_NONE)
	{
		Page = GRequestedPage;
		GRequestedPage = INDEX_NONE;
	}
	Rebuild();
}

SOptiNotebook::~SOptiNotebook()
{
	if (TSharedPtr<FOptiCompanion> C = FOptiCompanion::Get())
	{
		C->OnChanged.Remove(ChangedHandle);
	}
}

TSharedRef<SWidget> SOptiNotebook::MakePageButton(int32 Value, const TCHAR* Key)
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.Padding(FMargin(14.f, 3.f))
		.IsChecked_Lambda([this, Value]() { return Page == Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this, Value](ECheckBoxState) { Page = Value; })
		[
			SNew(STextBlock).Text(OptiText(Key)).Font(FAppStyle::GetFontStyle("NormalFontBold"))
		];
}

TSharedRef<SWidget> SOptiNotebook::MakeFilterButton(EFilter Value, const TCHAR* Key)
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.Padding(FMargin(10.f, 2.f))
		.IsChecked_Lambda([this, Value]() { return Filter == Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
		.OnCheckStateChanged_Lambda([this, Value](ECheckBoxState) { Filter = Value; Rebuild(); })
		[
			SNew(STextBlock).Text(OptiText(Key))
		];
}

bool SOptiNotebook::PassesFilter(const FOptiFinding& Finding) const
{
	switch (Filter)
	{
	case EFilter::New: return Finding.State == EOptiFindingState::New || Finding.State == EOptiFindingState::Stale;
	case EFilter::Postponed: return Finding.State == EOptiFindingState::Postponed;
	case EFilter::Done: return Finding.IsDone();
	case EFilter::Dismissed: return Finding.State == EOptiFindingState::Dismissed;
	default: return true;
	}
}

void SOptiNotebook::RebuildTested()
{
	TSharedPtr<FOptiCompanion> C = FOptiCompanion::Get();
	if (!TestedList.IsValid() || !C.IsValid())
	{
		return;
	}
	const TArray<FOptiTrial>& Trials = C->GetNotebook().GetTrials();
	const FDateTime Last = Trials.IsEmpty() ? FDateTime() : Trials.Last().Time;
	if (Trials.Num() == TestedShownCount && Last == TestedShownLast)
	{
		return; // the companion changes often (status text); the list only when an experiment ends
	}
	TestedShownCount = Trials.Num();
	TestedShownLast = Last;
	TestedList->ClearChildren();

	if (Trials.IsEmpty())
	{
		TestedList->AddSlot().AutoHeight().Padding(16.f, 24.f).HAlign(HAlign_Center)
		[
			SNew(STextBlock).Text(OptiText(TEXT("ui.tested.empty"))).ColorAndOpacity(FSlateColor::UseSubduedForeground()).AutoWrapText(true)
		];
		return;
	}

	constexpr int32 MaxRows = 150;
	for (int32 Index = Trials.Num() - 1; Index >= FMath::Max(0, Trials.Num() - MaxRows); --Index)
	{
		const FOptiTrial& Trial = Trials[Index];
		FLinearColor Color(0.45f, 0.45f, 0.45f);
		switch (Trial.Outcome)
		{
		case EOptiTrialOutcome::Finding: Color = FOptiStyle::Good; break;
		case EOptiTrialOutcome::TooSmall: Color = FLinearColor(0.85f, 0.65f, 0.2f); break;
		case EOptiTrialOutcome::Visible: Color = FLinearColor(0.6f, 0.45f, 0.9f); break;
		default: break;
		}
		const FText Numbers = Trial.BaselineMs > 0.0
			? FOptiPhrases::Get().Text(TEXT("ui.tested.numbers"), { { TEXT("gain"), FText::FromString(FString::Printf(TEXT("%+.2f"), -Trial.GainMs)) },
				{ TEXT("base"), Ms(Trial.BaselineMs) }, { TEXT("metric"), FText::FromString(Trial.Metric == TEXT("GPUTime") ? TEXT("GPU") : TEXT("frame")) } })
			: FText::GetEmpty();
		const FText When = FText::Format(LOCTEXT("TestedWhen", "{0}{1}"), FText::AsTime(Trial.Time, EDateTimeStyle::Short, FText::GetInvariantTimeZone()),
			Trial.bWhileWorking ? FText::Format(LOCTEXT("TestedWorking", " · {0}"), OptiText(TEXT("ui.tested.working"))) : FText::GetEmpty());

		TestedList->AddSlot().AutoHeight().Padding(8.f, 1.f)
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
			.Padding(FMargin(10.f, 5.f))
			[
				SNew(SHorizontalBox)
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
				[
					SNew(SBox).WidthOverride(130.f)[Pill(FOptiPhrases::Get().Text(FString(TEXT("outcome.")) + LexToString(Trial.Outcome)), Color)]
				]
				+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
				[
					SNew(SVerticalBox)
					+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(FOptiPhrases::Get().Action(Trial.ActionId)).AutoWrapText(true)]
					+ SVerticalBox::Slot().AutoHeight()[SNew(STextBlock).Text(When).ColorAndOpacity(FSlateColor::UseSubduedForeground())]
				]
				+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(10.f, 0.f, 0.f, 0.f)
				[
					SNew(STextBlock).Text(Numbers).ColorAndOpacity(Trial.Outcome == EOptiTrialOutcome::Finding ? FSlateColor(FOptiStyle::Good) : FSlateColor::UseSubduedForeground())
				]
			]
		];
	}
}

void SOptiNotebook::Rebuild()
{
	RebuildTested();
	if (!List.IsValid())
	{
		return;
	}
	List->ClearChildren();
	TSharedPtr<FOptiCompanion> C = FOptiCompanion::Get();
	if (!C.IsValid())
	{
		return;
	}

	TArray<TSharedRef<FOptiFinding>> Findings = C->GetNotebook().GetFindings().FilterByPredicate([this](const TSharedRef<FOptiFinding>& F) { return PassesFilter(*F); });
	static const int32 Order[] = { 0, 2, 3, 4, 1, 5 }; // New, Postponed, Applied, Resolved, Stale, Dismissed
	Findings.Sort([](const TSharedRef<FOptiFinding>& L, const TSharedRef<FOptiFinding>& R)
	{
		const int32 OL = Order[static_cast<int32>(L->State)], OR = Order[static_cast<int32>(R->State)];
		return OL != OR ? OL < OR : L->Changed > R->Changed;
	});

	if (Findings.IsEmpty())
	{
		List->AddSlot().AutoHeight().Padding(16.f, 24.f).HAlign(HAlign_Center)
		[
			SNew(STextBlock).Text(OptiText(TEXT("ui.empty"))).ColorAndOpacity(FSlateColor::UseSubduedForeground())
		];
		return;
	}
	for (const TSharedRef<FOptiFinding>& Finding : Findings)
	{
		List->AddSlot().AutoHeight().Padding(8.f, 2.f)[MakeRow(Finding)];
	}
}

TSharedRef<SWidget> SOptiNotebook::MakeRow(TSharedRef<FOptiFinding> Finding)
{
	const FGuid Id = Finding->Id;
	const bool bRegression = Finding->Kind == EOptiFindingKind::Regression;
	const bool bDone = Finding->IsDone();
	const FLinearColor Color = StateColor(*Finding);

	TSharedRef<SHorizontalBox> Meta = SNew(SHorizontalBox);
	Meta->AddSlot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)[Pill(FOptiPhrases::Get().Text(FString(TEXT("state.")) + LexToString(Finding->State)), Color)];
	if (const FOptiAction* Action = OptiActions::Find(Finding->ActionId))
	{
		Meta->AddSlot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f).VAlign(VAlign_Center)[SNew(STextBlock).Text(FOptiPhrases::Get().Sector(Action->Sector)).ColorAndOpacity(FSlateColor::UseSubduedForeground())];
	}
	FText Extra;
	if (Finding->State == EOptiFindingState::Postponed)
	{
		Extra = FOptiPhrases::Get().Text(TEXT("ui.remind"), { { TEXT("when"), ReminderText(*Finding) } });
	}
	else if (Finding->State == EOptiFindingState::Stale)
	{
		Extra = OptiText(TEXT("ui.recheck_pending"));
	}
	else if (Finding->State == EOptiFindingState::Dismissed && !Finding->Note.IsEmpty())
	{
		Extra = FOptiPhrases::Get().Text(TEXT("ui.dismissed_note"), { { TEXT("note"), FText::FromString(Finding->Note) } });
	}
	else
	{
		Extra = FText::AsDateTime(Finding->Changed, EDateTimeStyle::Short, EDateTimeStyle::Short, FText::GetInvariantTimeZone());
	}
	Meta->AddSlot().AutoWidth().VAlign(VAlign_Center)[SNew(STextBlock).Text(Extra).ColorAndOpacity(FSlateColor::UseSubduedForeground())];

	TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
	auto Add = [&Buttons](TSharedRef<SWidget> Widget) { Buttons->AddSlot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)[Widget]; };
	auto Do = [Id](void (FOptiCompanion::*Method)(const FGuid&)) { return [Id, Method]() { if (TSharedPtr<FOptiCompanion> P = FOptiCompanion::Get()) { (P.Get()->*Method)(Id); } }; };

	if (bRegression && Finding->State == EOptiFindingState::New)
	{
		Add(TextButton(OptiText(TEXT("ui.openasset")), true, Do(&FOptiCompanion::OpenAsset)));
		Add(TextButton(OptiText(TEXT("ui.intentional")), false, [Id]() { if (auto P = FOptiCompanion::Get()) { P->DismissFinding(Id, false, TEXT("Intentional")); } }));
	}
	else if (!bRegression)
	{
		switch (Finding->State)
		{
		case EOptiFindingState::New:
			Add(TextButton(OptiText(TEXT("ui.apply")), true, Do(&FOptiCompanion::ApplyFinding)));
			Add(SNew(SComboButton).ButtonContent()[SNew(STextBlock).Text(OptiText(TEXT("ui.later")))].OnGetMenuContent_Lambda([Id]() { return LaterMenu(Id); }));
			Add(TextButton(OptiText(TEXT("ui.details")), false, [Finding]() { OptiOpenFindingDetails(Finding); }));
			Add(TextButton(OptiText(TEXT("ui.dismiss")), false, [Id]() { if (auto P = FOptiCompanion::Get()) { P->DismissFinding(Id, false); } }));
			break;
		case EOptiFindingState::Postponed:
			Add(TextButton(OptiText(TEXT("ui.apply")), true, Do(&FOptiCompanion::ApplyFinding)));
			Add(TextButton(OptiText(TEXT("ui.details")), false, [Finding]() { OptiOpenFindingDetails(Finding); }));
			Add(TextButton(OptiText(TEXT("ui.dismiss")), false, [Id]() { if (auto P = FOptiCompanion::Get()) { P->DismissFinding(Id, false); } }));
			break;
		case EOptiFindingState::Stale:
			Add(TextButton(OptiText(TEXT("ui.recheck")), false, Do(&FOptiCompanion::RecheckFinding)));
			break;
		case EOptiFindingState::Applied:
			Add(TextButton(OptiText(TEXT("ui.undo")), false, Do(&FOptiCompanion::UndoFinding)));
			Add(TextButton(OptiText(TEXT("ui.details")), false, [Finding]() { OptiOpenFindingDetails(Finding); }));
			break;
		case EOptiFindingState::Dismissed:
			Add(TextButton(OptiText(TEXT("ui.restore")), false, Do(&FOptiCompanion::RestoreFinding)));
			break;
		default:
			break;
		}
	}
	else if (Finding->State == EOptiFindingState::Dismissed)
	{
		Add(TextButton(OptiText(TEXT("ui.restore")), false, Do(&FOptiCompanion::RestoreFinding)));
	}

	const FText Gain = bRegression
		? FText::Format(LOCTEXT("RegressionGain", "+{0} ms"), Ms(Finding->GainMs))
		: FText::Format(LOCTEXT("FindingGain", "-{0} ms"), Ms(Finding->GainMs));

	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("Brushes.Recessed"))
		.Padding(0.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(3.f)
				[
					SNew(SBorder).BorderImage(FOptiStyle::Get().GetBrush("Opti.Strip")).BorderBackgroundColor(Color)
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.f).Padding(10.f, 8.f)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(1.f)
					[
						SNew(STextBlock)
						.Text(Title(*Finding))
						.AutoWrapText(true)
						.ColorAndOpacity(bDone || Finding->State == EOptiFindingState::Dismissed ? FSlateColor::UseSubduedForeground() : FSlateColor::UseForeground())
						.StrikeBrush(bDone ? FOptiStyle::Get().GetBrush("Opti.Strike") : nullptr)
					]
					+ SHorizontalBox::Slot().AutoWidth().Padding(10.f, 0.f, 0.f, 0.f)
					[
						SNew(STextBlock).Text(Gain).Font(FAppStyle::GetFontStyle("NormalFontBold"))
						.ColorAndOpacity(bRegression ? FOptiStyle::Bad : (bDone ? FLinearColor(0.5f, 0.5f, 0.5f) : FOptiStyle::Good))
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)[Meta]
				+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)[Buttons]
			]
		];
}

#undef LOCTEXT_NAMESPACE
