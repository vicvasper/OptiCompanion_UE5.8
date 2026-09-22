#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"

class FOptiCompanion;
struct FOptiFinding;
class SVerticalBox;

/** Opens the before/after comparison window for a finding. */
void OptiOpenFindingDetails(TSharedRef<FOptiFinding> Finding);

/** The fly's notebook tab: score, brain summary and every finding with its state. */
class SOptiNotebook : public SCompoundWidget
{
public:
	static const FName TabName;

	SLATE_BEGIN_ARGS(SOptiNotebook) {}
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual ~SOptiNotebook() override;

	enum EPage : int32 { FindingsPage = 0, InventoryPage = 1, TestedPage = 2 };
	/** Opens the notebook tab on a page (also the Opti.Notebook console command). */
	static void Open(int32 Page);

private:
	enum class EFilter : uint8 { All, New, Postponed, Done, Dismissed };

	void Rebuild();
	void RebuildTested();
	bool PassesFilter(const FOptiFinding& Finding) const;
	TSharedRef<SWidget> MakeRow(TSharedRef<FOptiFinding> Finding);
	TSharedRef<SWidget> MakeFilterButton(EFilter Value, const TCHAR* Key);
	TSharedRef<SWidget> MakePageButton(int32 Value, const TCHAR* Key);

	TSharedPtr<SVerticalBox> List;
	TSharedPtr<SVerticalBox> TestedList;
	int32 TestedShownCount = -1;
	FDateTime TestedShownLast;
	EFilter Filter = EFilter::All;
	int32 Page = 0; // 0 findings, 1 inventory, 2 tested
	FDelegateHandle ChangedHandle;
};
