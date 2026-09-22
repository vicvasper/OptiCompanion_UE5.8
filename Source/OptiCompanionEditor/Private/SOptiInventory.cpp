#include "SOptiInventory.h"
#include "OptiCompanion.h"
#include "OptiPhrases.h"
#include "OptiStyle.h"

#include "Editor.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "UObject/SoftObjectPath.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SHeaderRow.h"

#define LOCTEXT_NAMESPACE "OptiCompanionInventory"

namespace
{
	const FName ColumnName(TEXT("Name"));
	const FName ColumnType(TEXT("Type"));
	const FName ColumnMemory(TEXT("Memory"));
	const FName ColumnDetails(TEXT("Details"));
	const FName ColumnWarning(TEXT("Warning"));

	/** OptiLogger's memory colour coding. */
	FLinearColor MemoryColor(float MB)
	{
		if (MB < 1.f) { return FOptiStyle::Good; }
		if (MB < 10.f) { return FLinearColor(0.9f, 0.9f, 0.9f); }
		if (MB < 50.f) { return FOptiStyle::Warn; }
		return FOptiStyle::Bad;
	}

	FLinearColor SeverityColor(int32 Severity)
	{
		return Severity >= 2 ? FOptiStyle::Bad : (Severity == 1 ? FOptiStyle::Warn : FLinearColor(0.55f, 0.55f, 0.58f));
	}

	FText TypeText(FOptiInventoryItem::EType Type)
	{
		return OptiText(*(FString(TEXT("inventory.type.")) + OptiInventory::LexToString(Type)));
	}

	class SInventoryRow : public SMultiColumnTableRow<TSharedPtr<FOptiInventoryItem>>
	{
	public:
		SLATE_BEGIN_ARGS(SInventoryRow) {}
			SLATE_ARGUMENT(TSharedPtr<FOptiInventoryItem>, Item)
		SLATE_END_ARGS()

		void Construct(const FArguments& InArgs, const TSharedRef<STableViewBase>& Owner)
		{
			Item = InArgs._Item;
			SMultiColumnTableRow::Construct(FSuperRowType::FArguments().Padding(FMargin(2.f, 1.f)), Owner);
		}

		virtual TSharedRef<SWidget> GenerateWidgetForColumn(const FName& Column) override
		{
			FText Text;
			FSlateColor Color = FSlateColor::UseForeground();
			if (Column == ColumnName) { Text = FText::FromString(Item->Name); }
			else if (Column == ColumnType) { Text = TypeText(Item->Type); Color = FSlateColor::UseSubduedForeground(); }
			else if (Column == ColumnMemory)
			{
				FNumberFormattingOptions Options;
				Options.MinimumFractionalDigits = Options.MaximumFractionalDigits = 2;
				if (Item->Type == FOptiInventoryItem::EType::Blueprint)
				{
					// Blueprints have no memory figure here: their cost is game-thread time, measured in Play.
					Text = FText::Format(LOCTEXT("BlueprintMs", "{0} ms"), FText::AsNumber(Item->CostMs, &Options));
					Color = SeverityColor(Item->Severity);
				}
				else
				{
					Text = Item->MemoryMB > 0.f ? FText::AsNumber(Item->MemoryMB, &Options) : FText::FromString(TEXT("-"));
					Color = MemoryColor(Item->MemoryMB);
				}
			}
			else if (Column == ColumnDetails) { Text = FText::FromString(Item->Details); Color = FSlateColor::UseSubduedForeground(); }
			else if (Column == ColumnWarning) { Text = FText::FromString(Item->Warning); Color = SeverityColor(Item->Severity); }
			return SNew(STextBlock).Text(Text).ColorAndOpacity(Color).ToolTipText(Text);
		}

	private:
		TSharedPtr<FOptiInventoryItem> Item;
	};
}

void SOptiInventory::Construct(const FArguments& InArgs)
{
	auto Button = [](const FText& Label, TFunction<void()> OnClick)
	{
		return SNew(SButton).OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })[SNew(STextBlock).Text(Label)];
	};
	auto Check = [](const FText& Label, bool* Value, TFunction<void()> OnChange)
	{
		return SNew(SCheckBox)
			.IsChecked_Lambda([Value]() { return *Value ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
			.OnCheckStateChanged_Lambda([Value, OnChange](ECheckBoxState State) { *Value = State == ECheckBoxState::Checked; OnChange(); })
			[SNew(STextBlock).Text(Label)];
	};

	ChildSlot
	[
		SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 8.f, 12.f, 4.f)
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)[Button(OptiText(TEXT("inventory.analyze")), [this]() { Analyze(); })]
			+ SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 12.f, 0.f)[Button(OptiText(TEXT("inventory.export")), [this]() { Export(); })]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 12.f, 0.f)[Check(OptiText(TEXT("inventory.visible_only")), &bVisibleOnly, [this]() { Analyze(); })]
			+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)[Check(OptiText(TEXT("inventory.warnings_only")), &bWarningsOnly, [this]() { Refresh(); })]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(12.f, 2.f, 12.f, 6.f)
		[
			SNew(STextBlock).Text_Lambda([this]() { return Summary(); }).ColorAndOpacity(FSlateColor::UseSubduedForeground()).AutoWrapText(true)
		]
		+ SVerticalBox::Slot().FillHeight(1.f).Padding(8.f, 0.f)
		[
			SAssignNew(List, SListView<FItemPtr>)
			.ListItemsSource(&Rows)
			.OnGenerateRow(this, &SOptiInventory::MakeRow)
			.OnMouseButtonDoubleClick_Lambda([this](FItemPtr Item) { Open(Item); })
			.HeaderRow
			(
				SNew(SHeaderRow)
				+ SHeaderRow::Column(ColumnName).DefaultLabel(OptiText(TEXT("inventory.col.name"))).FillWidth(0.22f)
					.SortMode_Lambda([this]() { return SortColumn == ColumnName ? SortMode : EColumnSortMode::None; }).OnSort(this, &SOptiInventory::OnSort)
				+ SHeaderRow::Column(ColumnType).DefaultLabel(OptiText(TEXT("inventory.col.type"))).FillWidth(0.1f)
					.SortMode_Lambda([this]() { return SortColumn == ColumnType ? SortMode : EColumnSortMode::None; }).OnSort(this, &SOptiInventory::OnSort)
				+ SHeaderRow::Column(ColumnMemory).DefaultLabel(OptiText(TEXT("inventory.col.memory"))).FillWidth(0.08f)
					.SortMode_Lambda([this]() { return SortColumn == ColumnMemory ? SortMode : EColumnSortMode::None; }).OnSort(this, &SOptiInventory::OnSort)
				+ SHeaderRow::Column(ColumnDetails).DefaultLabel(OptiText(TEXT("inventory.col.details"))).FillWidth(0.3f)
				+ SHeaderRow::Column(ColumnWarning).DefaultLabel(OptiText(TEXT("inventory.col.warning"))).FillWidth(0.3f)
					.SortMode_Lambda([this]() { return SortColumn == ColumnWarning ? SortMode : EColumnSortMode::None; }).OnSort(this, &SOptiInventory::OnSort)
			)
		]
	];
	// Deliberately not analysed on open: the pass is synchronous and blocks the editor (a lesson from OptiLogger).
}

void SOptiInventory::Analyze()
{
	TSharedPtr<FOptiCompanion> Companion = FOptiCompanion::Get();
	FOptiView View;
	if (bVisibleOnly && Companion.IsValid())
	{
		View = Companion->CurrentView();
	}
	UWorld* World = GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	Inventory = OptiInventory::Collect(World, View);
	if (Companion.IsValid())
	{
		// What each Blueprint cost in the last Play session: the only place that cost exists.
		FNumberFormattingOptions Two;
		Two.MinimumFractionalDigits = Two.MaximumFractionalDigits = 2;
		for (const FOptiBlueprintCost& Cost : Companion->GetPlayCosts())
		{
			FOptiInventoryItem& Item = Inventory.Items.AddDefaulted_GetRef();
			Item.Type = FOptiInventoryItem::EType::Blueprint;
			Item.Name = Cost.ClassName.LeftChop(2);
			Item.Path = Cost.BlueprintPath;
			Item.CostMs = static_cast<float>(Cost.MsPerFrame);
			Item.Uses = Cost.Instances;
			Item.Details = FOptiPhrases::Get().Text(TEXT("inventory.blueprint.details"), { { TEXT("n"), FText::AsNumber(Cost.Instances) },
				{ TEXT("interval"), FText::AsNumber(Cost.TickInterval, &Two) }, { TEXT("peak"), FText::AsNumber(Cost.PeakMsPerFrame, &Two) } }).ToString();
			Item.Severity = Cost.MsPerFrame >= 1.0 ? 2 : Cost.MsPerFrame >= 0.2 ? 1 : 0;
			if (Item.Severity > 0)
			{
				Item.Warning = OptiText(TEXT("inventory.blueprint.warning")).ToString();
			}
		}
	}
	Refresh();
}

void SOptiInventory::Export()
{
	if (Inventory.Items.IsEmpty())
	{
		Analyze();
	}
	const FString Directory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("OptiCompanion"));
	IFileManager::Get().MakeDirectory(*Directory, true);
	const FString Path = Directory / FString::Printf(TEXT("Inventory_%s.json"), *FDateTime::Now().ToString(TEXT("%Y%m%d_%H%M%S")));
	LastExport = Inventory.ExportJson(Path) ? Path : FString();
}

void SOptiInventory::OnSort(EColumnSortPriority::Type Priority, const FName& Column, EColumnSortMode::Type Mode)
{
	SortColumn = Column;
	SortMode = Mode;
	Refresh();
}

void SOptiInventory::Refresh()
{
	Rows.Reset();
	for (const FOptiInventoryItem& Item : Inventory.Items)
	{
		if (!bWarningsOnly || Item.Severity > 0)
		{
			Rows.Add(MakeShared<FOptiInventoryItem>(Item));
		}
	}
	const bool bAscending = SortMode == EColumnSortMode::Ascending;
	const FName Column = SortColumn;
	Rows.Sort([Column, bAscending](const FItemPtr& L, const FItemPtr& R)
	{
		bool bLess;
		if (Column == ColumnName) { bLess = L->Name < R->Name; }
		else if (Column == ColumnType) { bLess = L->Type < R->Type; }
		else if (Column == ColumnMemory) { bLess = L->MemoryMB + L->CostMs < R->MemoryMB + R->CostMs; }
		else { bLess = L->Severity != R->Severity ? L->Severity < R->Severity : L->MemoryMB < R->MemoryMB; }
		return bAscending ? bLess : !bLess;
	});
	if (List.IsValid())
	{
		List->RequestListRefresh();
	}
}

void SOptiInventory::Open(FItemPtr Item) const
{
	if (!Item.IsValid() || !GEditor)
	{
		return;
	}
	UObject* Object = FSoftObjectPath(Item->Path).ResolveObject();
	if (AActor* Actor = Cast<AActor>(Object))
	{
		GEditor->SelectNone(false, true);
		GEditor->SelectActor(Actor, true, true);
		GEditor->MoveViewportCamerasToActor(*Actor, false);
	}
	else if (Object)
	{
		TArray<UObject*> Objects = { Object };
		GEditor->SyncBrowserToObjects(Objects);
	}
}

TSharedRef<ITableRow> SOptiInventory::MakeRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner)
{
	return SNew(SInventoryRow, Owner).Item(Item);
}

FText SOptiInventory::Summary() const
{
	if (Inventory.Items.IsEmpty())
	{
		return OptiText(TEXT("inventory.empty"));
	}
	using EType = FOptiInventoryItem::EType;
	FNumberFormattingOptions Options;
	Options.MaximumFractionalDigits = 1;
	const int32 Warnings = Inventory.Items.FilterByPredicate([](const FOptiInventoryItem& Item) { return Item.Severity > 0; }).Num();
	FText Summary = FOptiPhrases::Get().Text(TEXT("inventory.summary"), {
		{ TEXT("meshes"), FText::AsNumber(Inventory.Count(EType::StaticMesh) + Inventory.Count(EType::SkeletalMesh)) },
		{ TEXT("textures"), FText::AsNumber(Inventory.Count(EType::Texture)) },
		{ TEXT("texmb"), FText::AsNumber(Inventory.TotalMemoryMB(EType::Texture), &Options) },
		{ TEXT("materials"), FText::AsNumber(Inventory.Count(EType::Material)) },
		{ TEXT("lights"), FText::AsNumber(Inventory.Count(EType::Light)) },
		{ TEXT("warnings"), FText::AsNumber(Warnings) },
		{ TEXT("scope"), OptiText(Inventory.bVisibleOnly ? TEXT("inventory.scope.visible") : TEXT("inventory.scope.level")) } });
	if (!LastExport.IsEmpty())
	{
		Summary = FText::Format(LOCTEXT("SummaryExported", "{0}   ·   {1}"), Summary, FText::FromString(LastExport));
	}
	return Summary;
}

#undef LOCTEXT_NAMESPACE
