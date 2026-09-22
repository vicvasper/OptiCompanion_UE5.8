#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "Widgets/SLeafWidget.h"

class FOptiCompanion;
struct FOptiFinding;
class SCanvas;
class SBox;
class SOptiFlyLayer;

/** The fly itself: layered SVG parts animated according to what the companion is doing. */
class SOptiFlyWidget : public SLeafWidget
{
public:
	SLATE_BEGIN_ARGS(SOptiFlyWidget) {}
		SLATE_ARGUMENT(TWeakPtr<FOptiCompanion>, Companion)
		SLATE_ARGUMENT(TWeakPtr<SOptiFlyLayer>, Layer)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	void SetHeading(float Degrees) { Heading = Degrees; }
	void SetFlying(bool bInFlying) { bFlying = bInFlying; }
	/** Overall size while popping from one place to another (1 = normal, 0 = gone). */
	void SetPopScale(float Scale) { PopScale = Scale; }

	virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D(54.0, 54.0); }
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;

	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FCursorReply OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const override;

private:
	void OpenContextMenu(const FPointerEvent& MouseEvent);

	TWeakPtr<FOptiCompanion> Companion;
	TWeakPtr<SOptiFlyLayer> Layer;
	float Heading = 0.f;
	float PopScale = 1.f;
	bool bFlying = false;
	bool bPressed = false;
	bool bDragging = false;
	FVector2D PressPosition;
	double StartTime = 0.0;
};

/**
 * Full-window overlay that hosts the fly and its speech bubble. It lets every click through except the
 * ones on the fly or the bubble, so the editor underneath works as usual.
 */
class SOptiFlyLayer : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SOptiFlyLayer) {}
		SLATE_ARGUMENT(TSharedPtr<FOptiCompanion>, Companion)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);
	virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

	void ShowBubble(TSharedRef<FOptiFinding> Finding);
	void ShowMessage(const FText& Message);
	void HideBubble(bool bIgnored);

	void DragTo(const FVector2D& ScreenPosition);
	void EndDrag();

private:
	void ShowBubbleContent(TSharedRef<SWidget> Content);
	TSharedRef<SWidget> MakeFindingBubble(TSharedRef<FOptiFinding> Finding);
	TSharedRef<SWidget> MakeLaterMenu(FGuid Id);
	void FlyTo(const FVector2D& Target);
	void FlyToViewport();
	FVector2D BubblePosition() const;
	FVector2D BubbleSize() const;
	FVector2D WhisperPosition() const;
	FVector2D WhisperSize() const;
	bool IsWhisperVisible() const;
	bool IsFlyVisible() const;

	TWeakPtr<FOptiCompanion> Companion;
	TSharedPtr<SOptiFlyWidget> Fly;
	TSharedPtr<SBox> BubbleHost;
	TSharedPtr<SWidget> Whisper;
	FGeometry LastGeometry;

	FVector2D FlyPosition = FVector2D::ZeroVector; // centre, in layer space
	bool bPlaced = false;
	bool bFlying = false;
	FVector2D FlightFrom, FlightControl, FlightTo;
	bool bPopping = false; // wingless mascots: shrink away, then grow at the destination
	double FlightStart = 0.0;
	double FlightDuration = 1.0;

	bool bBubbleVisible = false;
	double BubbleShownAt = 0.0;
	bool bBubbleIsFinding = false;
	int32 SeenPointRequest = 0;
};
