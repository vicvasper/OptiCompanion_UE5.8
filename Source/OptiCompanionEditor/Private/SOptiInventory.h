#pragma once

#include "CoreMinimal.h"
#include "OptiInventory.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/Views/SListView.h"

class SHeaderRow;

/**
 * Inventory of what is in view, in the spirit of OptiLogger: every mesh, texture, material, light, sound
 * and post-process volume with its estimated memory and what usually makes it expensive. These are
 * observations, not measurements; the fly's findings are the measured part.
 */
class SOptiInventory : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOptiInventory) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

private:
	using FItemPtr = TSharedPtr<FOptiInventoryItem>;

	void Analyze();
	void Export();
	void Refresh();
	void OnSort(EColumnSortPriority::Type Priority, const FName& Column, EColumnSortMode::Type Mode);
	void Open(FItemPtr Item) const;
	TSharedRef<ITableRow> MakeRow(FItemPtr Item, const TSharedRef<STableViewBase>& Owner);
	FText Summary() const;

	FOptiInventory Inventory;
	TArray<FItemPtr> Rows;
	TSharedPtr<SListView<FItemPtr>> List;
	bool bVisibleOnly = true;
	bool bWarningsOnly = false;
	FName SortColumn = TEXT("Warning");
	EColumnSortMode::Type SortMode = EColumnSortMode::Descending;
	FString LastExport;
};
