#include "SOptiFly.h"
#include "OptiCompanion.h"
#include "OptiCompanionSettings.h"
#include "OptiPhrases.h"
#include "OptiStyle.h"
#include "SOptiNotebook.h"

#include "Framework/Application/SlateApplication.h"
#include "Framework/MultiBox/MultiBoxBuilder.h"
#include "IAssetViewport.h"
#include "ISettingsModule.h"
#include "LevelEditor.h"
#include "Misc/PackageName.h"
#include "Rendering/DrawElements.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SComboButton.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SCanvas.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "OptiCompanionFly"

namespace
{
	constexpr float FlySize = 54.f;
	constexpr float BubbleWidth = 320.f;
	constexpr double BubbleLifetime = 15.0;
	constexpr float WhisperMaxWidth = 280.f;
	// Pop travel for wingless mascots: shrink, stay gone a moment, grow at the destination with a small overshoot.
	constexpr double PopShrinkSeconds = 0.22;
	constexpr double PopGoneSeconds = 0.18;
	constexpr double PopGrowSeconds = 0.32;

	UOptiCompanionSettings& MutableSettings() { return *GetMutableDefault<UOptiCompanionSettings>(); }

	FText Ms(double Value)
	{
		FNumberFormattingOptions Options;
		Options.MinimumFractionalDigits = Value >= 10.0 ? 1 : 2;
		Options.MaximumFractionalDigits = Options.MinimumFractionalDigits;
		return FText::AsNumber(Value, &Options);
	}
}

// ---------------------------------------------------------------------------------------------- fly

void SOptiFlyWidget::Construct(const FArguments& InArgs)
{
	Companion = InArgs._Companion;
	Layer = InArgs._Layer;
	StartTime = FPlatformTime::Seconds();

	SetToolTipText(MakeAttributeLambda([this]()
	{
		TSharedPtr<FOptiCompanion> C = Companion.Pin();
		return C.IsValid() ? C->GetStatusText() : FText::GetEmpty();
	}));

	// Keep repainting while visible: the animations are cheap vector draws.
	RegisterActiveTimer(0.f, FWidgetActiveTimerDelegate::CreateLambda([this](double, float)
	{
		Invalidate(EInvalidateWidgetReason::Paint);
		return EActiveTimerReturnType::Continue;
	}));
}

int32 SOptiFlyWidget::OnPaint(const FPaintArgs& Args, const FGeometry& Geometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	TSharedPtr<FOptiCompanion> C = Companion.Pin();
	const EOptiFlyMood Mood = C.IsValid() ? C->GetMood() : EOptiFlyMood::Grooming;
	const bool bQuiet = GetDefault<UOptiCompanionSettings>()->bQuietMode;
	const float T = static_cast<float>(FPlatformTime::Seconds() - StartTime) * (bQuiet ? 0.25f : 1.f);

	// Mascots are heads: they lean into a flight a little instead of pointing the way like a whole insect.
	float Angle = FMath::Clamp(FRotator::NormalizeAxis(Heading), -25.f, 25.f);
	FVector2f Jitter(0.f, 0.f);
	float BodyScale = 1.f;
	float LegsOffset = 0.f;
	float WingScale = 1.f;
	float WingAlpha = 1.f;

	switch (Mood)
	{
	case EOptiFlyMood::Grooming:
		LegsOffset = -2.5f * FMath::Max(0.f, FMath::Sin(T * 7.5f));
		break;
	case EOptiFlyMood::Rubbing:
		LegsOffset = -2.5f * FMath::Abs(FMath::Sin(T * 20.f));
		break;
	case EOptiFlyMood::Sniffing:
		Angle += 7.f * FMath::Sin(T * 4.5f);
		break;
	case EOptiFlyMood::Sleeping:
		BodyScale = 1.f + 0.035f * (0.5f + 0.5f * FMath::Sin(T * 2.4f));
		break;
	case EOptiFlyMood::Buzzing:
		if (!bQuiet)
		{
			Jitter = FVector2f(FMath::Sin(T * 57.f), FMath::Cos(T * 43.f)) * 1.2f;
		}
		break;
	}
	if (bFlying || Mood == EOptiFlyMood::Buzzing)
	{
		const float Flap = FMath::Sin(T * 140.f);
		WingScale = 0.8f + 0.32f * (0.5f + 0.5f * Flap);
		WingAlpha = 0.45f + 0.5f * (0.5f - 0.5f * Flap);
	}

	if (PopScale <= 0.01f)
	{
		return LayerId;
	}
	BodyScale *= PopScale;
	WingScale *= PopScale;

	const FVector2f Size(FlySize, FlySize);
	const FSlateLayoutTransform Offset(Jitter);
	const float Radians = FMath::DegreesToRadians(Angle);
	auto Paint = [&](const TCHAR* Part, float ScaleX, float ScaleY, float OffsetY, const FLinearColor& Tint, int32 Layer)
	{
		const FSlateRenderTransform Transform(::Concatenate(FScale2f(ScaleX, ScaleY), FQuat2f(Radians)), FVector2f(0.f, OffsetY));
		FSlateDrawElement::MakeBox(OutDrawElements, Layer, Geometry.ToPaintGeometry(Size, Offset, Transform, FVector2f(0.5f, 0.5f)),
			FOptiStyle::MascotBrush(Part), ESlateDrawEffect::None, Tint * InWidgetStyle.GetColorAndOpacityTint());
	};

	// The chosen mascot theme is looked up every paint, so changing it in the settings takes effect at once.
	Paint(TEXT("Wings"), WingScale, PopScale, 0.f, FLinearColor(1.f, 1.f, 1.f, WingAlpha), LayerId);
	Paint(TEXT("LegsFront"), PopScale, PopScale, LegsOffset * PopScale, FLinearColor::White, LayerId + 1);
	Paint(TEXT("Body"), BodyScale, BodyScale, 0.f, FLinearColor::White, LayerId + 1);

	if (PopScale < 1.f)
	{
		return LayerId + 2; // mid-pop: no z's or badge floating around a half-sized head
	}
	if (Mood == EOptiFlyMood::Sleeping)
	{
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 9);
		for (int32 Index = 0; Index < 2; ++Index)
		{
			const float Phase = FMath::Fmod(T / 2.4f + Index * 0.33f, 1.f);
			const FVector2f Position(40.f + Phase * 8.f + Index * 5.f, 8.f - Phase * 12.f - Index * 4.f);
			const float Alpha = FMath::Sin(Phase * PI);
			FSlateDrawElement::MakeText(OutDrawElements, LayerId + 2, Geometry.ToPaintGeometry(FVector2f(12.f, 14.f), FSlateLayoutTransform(Position)),
				FText::FromString(TEXT("z")), Font, ESlateDrawEffect::None, FLinearColor(0.62f, 0.75f, 0.92f, Alpha));
		}
	}

	// Ambient presence: a small counter instead of a message.
	const int32 NewFindings = C.IsValid() ? C->GetNotebook().CountNew() : 0;
	if (NewFindings > 0)
	{
		const FVector2f BadgeSize(16.f, 16.f);
		const FVector2f BadgePosition(FlySize - 14.f, 2.f);
		FSlateDrawElement::MakeBox(OutDrawElements, LayerId + 3, Geometry.ToPaintGeometry(BadgeSize, FSlateLayoutTransform(BadgePosition)),
			FOptiStyle::Get().GetBrush("Opti.Badge"));
		const FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle("Bold", 8);
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 4, Geometry.ToPaintGeometry(BadgeSize, FSlateLayoutTransform(BadgePosition + FVector2f(NewFindings > 9 ? 2.f : 5.f, 1.f))),
			FText::AsNumber(FMath::Min(NewFindings, 99)), Font, ESlateDrawEffect::None, FLinearColor::White);
	}
	return LayerId + 4;
}

FReply SOptiFlyWidget::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bPressed = true;
		bDragging = false;
		PressPosition = MouseEvent.GetScreenSpacePosition();
		return FReply::Handled().CaptureMouse(SharedThis(this));
	}
	return FReply::Handled();
}

FReply SOptiFlyWidget::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (bPressed && HasMouseCapture())
	{
		if (!bDragging && FVector2D::Distance(MouseEvent.GetScreenSpacePosition(), PressPosition) > 4.0)
		{
			bDragging = true;
		}
		if (bDragging)
		{
			if (TSharedPtr<SOptiFlyLayer> L = Layer.Pin())
			{
				L->DragTo(MouseEvent.GetScreenSpacePosition());
			}
		}
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

FReply SOptiFlyWidget::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() == EKeys::RightMouseButton)
	{
		OpenContextMenu(MouseEvent);
		return FReply::Handled();
	}
	if (bPressed && MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton)
	{
		bPressed = false;
		if (bDragging)
		{
			if (TSharedPtr<SOptiFlyLayer> L = Layer.Pin())
			{
				L->EndDrag();
			}
		}
		else if (TSharedPtr<FOptiCompanion> C = Companion.Pin())
		{
			C->OnFlyClicked();
		}
		bDragging = false;
		return FReply::Handled().ReleaseMouseCapture();
	}
	return FReply::Unhandled();
}

FReply SOptiFlyWidget::OnMouseButtonDoubleClick(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (TSharedPtr<FOptiCompanion> C = Companion.Pin())
	{
		C->OpenNotebook();
	}
	return FReply::Handled();
}

FCursorReply SOptiFlyWidget::OnCursorQuery(const FGeometry& MyGeometry, const FPointerEvent& CursorEvent) const
{
	return FCursorReply::Cursor(bDragging ? EMouseCursor::GrabHandClosed : EMouseCursor::GrabHand);
}

void SOptiFlyWidget::OpenContextMenu(const FPointerEvent& MouseEvent)
{
	TSharedPtr<FOptiCompanion> C = Companion.Pin();
	if (!C.IsValid())
	{
		return;
	}
	TWeakPtr<FOptiCompanion> Weak = C;
	auto Toggle = [Weak](TFunction<void(UOptiCompanionSettings&)> Change)
	{
		return FUIAction(FExecuteAction::CreateLambda([Weak, Change]()
		{
			Change(MutableSettings());
			if (TSharedPtr<FOptiCompanion> Pinned = Weak.Pin()) { Pinned->ApplySettingsChange(); }
		}));
	};
	auto Checked = [](TFunction<bool(const UOptiCompanionSettings&)> Test)
	{
		return FIsActionChecked::CreateLambda([Test]() { return Test(*GetDefault<UOptiCompanionSettings>()); });
	};

	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(OptiText(TEXT("ui.notebook.tab")), FText::GetEmpty(), FSlateIcon(FOptiStyle::GetStyleSetName(), "Opti.Fly.Icon"),
		FUIAction(FExecuteAction::CreateSP(C.ToSharedRef(), &FOptiCompanion::OpenNotebook)));
	Menu.AddMenuEntry(OptiText(TEXT("ui.nap_now")), FText::GetEmpty(), FSlateIcon(),
		FUIAction(FExecuteAction::CreateSP(C.ToSharedRef(), &FOptiCompanion::NapNow)));
	Menu.AddSeparator();

	FUIAction Sober = Toggle([](UOptiCompanionSettings& S) { S.Tone = EOptiTone::Sober; });
	Sober.GetActionCheckState = FGetActionCheckState::CreateLambda([]() { return GetDefault<UOptiCompanionSettings>()->Tone == EOptiTone::Sober ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; });
	Menu.AddMenuEntry(OptiText(TEXT("ui.tone.sober")), FText::GetEmpty(), FSlateIcon(), Sober, NAME_None, EUserInterfaceActionType::RadioButton);
	FUIAction Friendly = Toggle([](UOptiCompanionSettings& S) { S.Tone = EOptiTone::Friendly; });
	Friendly.GetActionCheckState = FGetActionCheckState::CreateLambda([]() { return GetDefault<UOptiCompanionSettings>()->Tone == EOptiTone::Friendly ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; });
	Menu.AddMenuEntry(OptiText(TEXT("ui.tone.friendly")), FText::GetEmpty(), FSlateIcon(), Friendly, NAME_None, EUserInterfaceActionType::RadioButton);

	Menu.AddSubMenu(OptiText(TEXT("ui.mascot")), FText::GetEmpty(), FNewMenuDelegate::CreateLambda([Toggle](FMenuBuilder& Sub)
	{
		for (const FString& Theme : FOptiStyle::GetMascots())
		{
			FUIAction Pick = Toggle([Theme](UOptiCompanionSettings& S) { S.Mascot = Theme; });
			Pick.GetActionCheckState = FGetActionCheckState::CreateLambda([Theme]() { return FOptiStyle::CurrentMascot() == Theme ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; });
			Sub.AddMenuEntry(FOptiPhrases::Get().MascotName(Theme, true), FText::GetEmpty(),
				FSlateIcon(FOptiStyle::GetStyleSetName(), *FString::Printf(TEXT("Opti.Mascot.%s.Icon"), *Theme)), Pick, NAME_None, EUserInterfaceActionType::RadioButton);
		}
	}));

	FUIAction Quiet = Toggle([](UOptiCompanionSettings& S) { S.bQuietMode = !S.bQuietMode; });
	Quiet.GetActionCheckState = FGetActionCheckState::CreateLambda([]() { return GetDefault<UOptiCompanionSettings>()->bQuietMode ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; });
	Menu.AddMenuEntry(OptiText(TEXT("ui.quiet")), FText::GetEmpty(), FSlateIcon(), Quiet, NAME_None, EUserInterfaceActionType::ToggleButton);
	FUIAction Dnd = Toggle([](UOptiCompanionSettings& S) { S.bDoNotDisturb = !S.bDoNotDisturb; });
	Dnd.GetActionCheckState = FGetActionCheckState::CreateLambda([]() { return GetDefault<UOptiCompanionSettings>()->bDoNotDisturb ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; });
	Menu.AddMenuEntry(OptiText(TEXT("ui.dnd")), FText::GetEmpty(), FSlateIcon(), Dnd, NAME_None, EUserInterfaceActionType::ToggleButton);
	Menu.AddMenuEntry(OptiText(TEXT("ui.hide_fly")), FText::GetEmpty(), FSlateIcon(), Toggle([](UOptiCompanionSettings& S) { S.Presence = EOptiPresence::Professional; }));
	Menu.AddSeparator();

	Menu.AddMenuEntry(OptiText(TEXT("ui.export_brain")), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(C.ToSharedRef(), &FOptiCompanion::ExportBrain)));
	Menu.AddMenuEntry(OptiText(TEXT("ui.import_brain")), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateSP(C.ToSharedRef(), &FOptiCompanion::ImportBrain)));
	Menu.AddMenuEntry(OptiText(TEXT("ui.settings")), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([]()
	{
		FModuleManager::LoadModuleChecked<ISettingsModule>(TEXT("Settings")).ShowViewer(TEXT("Editor"), TEXT("Plugins"), TEXT("OptiCompanion"));
	})));

	FSlateApplication::Get().PushMenu(SharedThis(this), FWidgetPath(), Menu.MakeWidget(), MouseEvent.GetScreenSpacePosition(), FPopupTransitionEffect::ContextMenu);
}

// ---------------------------------------------------------------------------------------------- layer

void SOptiFlyLayer::Construct(const FArguments& InArgs)
{
	Companion = InArgs._Companion;
	SetVisibility(EVisibility::SelfHitTestInvisible);

	ChildSlot
	[
		SNew(SCanvas)
		.Visibility(EVisibility::SelfHitTestInvisible)
		+ SCanvas::Slot()
		.Position(MakeAttributeLambda([this]() { return FlyPosition - FVector2D(FlySize * 0.5f); }))
		.Size(FVector2D(FlySize))
		[
			SAssignNew(Fly, SOptiFlyWidget)
			.Companion(Companion)
			.Layer(SharedThis(this))
			.Visibility(MakeAttributeLambda([this]() { return IsFlyVisible() ? EVisibility::Visible : EVisibility::Collapsed; }))
		]
		+ SCanvas::Slot()
		.Position(MakeAttributeLambda([this]() { return BubblePosition(); }))
		.Size(MakeAttributeLambda([this]() { return BubbleSize(); }))
		[
			SAssignNew(BubbleHost, SBox)
			.WidthOverride(BubbleWidth)
			.Visibility(MakeAttributeLambda([this]() { return bBubbleVisible && IsFlyVisible() ? EVisibility::Visible : EVisibility::Collapsed; }))
		]
		// What it is doing, in one line under the head: never clickable, never in the way, gone after a few seconds.
		+ SCanvas::Slot()
		.Position(MakeAttributeLambda([this]() { return WhisperPosition(); }))
		.Size(MakeAttributeLambda([this]() { return WhisperSize(); }))
		[
			SAssignNew(Whisper, SBorder)
			.BorderImage(FOptiStyle::Get().GetBrush("Opti.Bubble"))
			.Padding(FMargin(8.f, 3.f))
			.Visibility(MakeAttributeLambda([this]() { return IsWhisperVisible() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; }))
			[
				SNew(STextBlock)
				.TextStyle(FOptiStyle::Get(), "Opti.Bubble.Small")
				.WrapTextAt(WhisperMaxWidth - 16.f)
				.Text_Lambda([this]()
				{
					TSharedPtr<FOptiCompanion> C = Companion.Pin();
					return C.IsValid() ? C->GetWhisper() : FText::GetEmpty();
				})
			]
		]
	];
}

bool SOptiFlyLayer::IsFlyVisible() const
{
	const UOptiCompanionSettings* S = GetDefault<UOptiCompanionSettings>();
	return S->bEnabled && S->Presence == EOptiPresence::Mascot;
}

void SOptiFlyLayer::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
	LastGeometry = AllottedGeometry;
	const FVector2D Size = AllottedGeometry.GetLocalSize();
	if (Size.X < 100.0 || Size.Y < 100.0)
	{
		return;
	}
	if (!bPlaced)
	{
		FlyPosition = GetDefault<UOptiCompanionSettings>()->FlyPosition * Size;
		bPlaced = true;
		UE_LOG(LogOptiCompanion, Verbose, TEXT("Fly placed at %s in a %s layer."), *FlyPosition.ToString(), *Size.ToString());
	}
	FlyPosition = FVector2D(FMath::Clamp(FlyPosition.X, 30.0, Size.X - 30.0), FMath::Clamp(FlyPosition.Y, 30.0, Size.Y - 30.0));

	if (bPopping)
	{
		const double T = FPlatformTime::Seconds() - FlightStart;
		if (T < PopShrinkSeconds)
		{
			const double K = T / PopShrinkSeconds;
			Fly->SetPopScale(static_cast<float>(1.0 - K * K));
		}
		else if (T < PopShrinkSeconds + PopGoneSeconds)
		{
			Fly->SetPopScale(0.f);
			FlyPosition = FlightTo;
		}
		else
		{
			// Ease-out-back: grows a little past its size and settles.
			const double K = FMath::Clamp((T - PopShrinkSeconds - PopGoneSeconds) / PopGrowSeconds, 0.0, 1.0);
			const double C1 = 1.70158, C3 = C1 + 1.0;
			Fly->SetPopScale(static_cast<float>(1.0 + C3 * FMath::Pow(K - 1.0, 3.0) + C1 * FMath::Pow(K - 1.0, 2.0)));
			if (K >= 1.0)
			{
				bPopping = false;
				Fly->SetPopScale(1.f);
			}
		}
	}
	else if (bFlying)
	{
		const double K = FMath::Clamp((FPlatformTime::Seconds() - FlightStart) / FlightDuration, 0.0, 1.0);
		const double E = K < 0.5 ? 2.0 * K * K : 1.0 - FMath::Pow(-2.0 * K + 2.0, 2.0) / 2.0;
		FlyPosition = (1 - E) * (1 - E) * FlightFrom + 2 * (1 - E) * E * FlightControl + E * E * FlightTo;
		const FVector2D Tangent = 2 * (1 - E) * (FlightControl - FlightFrom) + 2 * E * (FlightTo - FlightControl);
		Fly->SetHeading(FMath::RadiansToDegrees(FMath::Atan2(Tangent.Y, Tangent.X)) + 90.f);
		if (K >= 1.0)
		{
			bFlying = false;
			Fly->SetFlying(false);
			Fly->SetHeading(0.f);
		}
	}

	if (TSharedPtr<FOptiCompanion> C = Companion.Pin())
	{
		if (C->GetPointAtViewportRequest() != SeenPointRequest)
		{
			SeenPointRequest = C->GetPointAtViewportRequest();
			FlyToViewport();
		}
	}

	if (bBubbleVisible && !BubbleHost->IsHovered() && FPlatformTime::Seconds() - BubbleShownAt > BubbleLifetime)
	{
		HideBubble(true);
	}
}

void SOptiFlyLayer::FlyTo(const FVector2D& Target)
{
	const UOptiCompanionSettings* S = GetDefault<UOptiCompanionSettings>();
	const double Distance = FVector2D::Distance(FlyPosition, Target);
	if (S->bQuietMode || Distance < 4.0)
	{
		FlyPosition = Target; // quiet mode: no flight across the screen, it simply appears there
		return;
	}
	FlightFrom = FlyPosition;
	FlightTo = Target;
	FlightStart = FPlatformTime::Seconds();
	if (!FOptiStyle::CurrentMascotFlies())
	{
		bPopping = true;
		return;
	}
	FlightControl = (FlightFrom + FlightTo) * 0.5 + FVector2D(0.0, -60.0 - Distance * 0.08);
	FlightStart = FPlatformTime::Seconds();
	FlightDuration = FMath::Min(1.3, 0.42 + Distance * 0.0011);
	bFlying = true;
	Fly->SetFlying(true);
}

void SOptiFlyLayer::FlyToViewport()
{
	FLevelEditorModule* LevelEditor = FModuleManager::GetModulePtr<FLevelEditorModule>(TEXT("LevelEditor"));
	TSharedPtr<IAssetViewport> Viewport = LevelEditor ? LevelEditor->GetFirstActiveViewport() : nullptr;
	if (!Viewport.IsValid())
	{
		return;
	}
	const FGeometry& ViewportGeometry = Viewport->AsWidget()->GetTickSpaceGeometry();
	const FVector2D Absolute = ViewportGeometry.LocalToAbsolute(ViewportGeometry.GetLocalSize() * FVector2D(0.5, 0.3));
	FlyTo(LastGeometry.AbsoluteToLocal(Absolute));
}

void SOptiFlyLayer::DragTo(const FVector2D& ScreenPosition)
{
	bFlying = false;
	bPopping = false;
	Fly->SetFlying(false);
	Fly->SetPopScale(1.f);
	FlyPosition = LastGeometry.AbsoluteToLocal(ScreenPosition);
}

void SOptiFlyLayer::EndDrag()
{
	const FVector2D Size = LastGeometry.GetLocalSize();
	if (Size.X > 0.0 && Size.Y > 0.0)
	{
		UOptiCompanionSettings& S = MutableSettings();
		S.FlyPosition = FlyPosition / Size;
		S.SaveConfig();
	}
}

FVector2D SOptiFlyLayer::BubbleSize() const
{
	return FVector2D(BubbleWidth, BubbleHost.IsValid() ? BubbleHost->GetDesiredSize().Y : 0.0);
}

bool SOptiFlyLayer::IsWhisperVisible() const
{
	if (!IsFlyVisible() || bBubbleVisible || GetDefault<UOptiCompanionSettings>()->bDoNotDisturb)
	{
		return false;
	}
	TSharedPtr<FOptiCompanion> C = Companion.Pin();
	return C.IsValid() && !C->GetWhisper().IsEmpty();
}

FVector2D SOptiFlyLayer::WhisperSize() const
{
	if (!Whisper.IsValid())
	{
		return FVector2D::ZeroVector;
	}
	const FVector2D Desired = Whisper->GetDesiredSize();
	return FVector2D(FMath::Min<double>(Desired.X, WhisperMaxWidth), Desired.Y);
}

FVector2D SOptiFlyLayer::WhisperPosition() const
{
	const FVector2D Size = LastGeometry.GetLocalSize();
	const FVector2D Line = WhisperSize();
	// Under the head, or above it when the head is near the bottom edge.
	const bool bAbove = FlyPosition.Y + FlySize * 0.5 + 6.0 + Line.Y > Size.Y - 8.0;
	const double Y = bAbove ? FlyPosition.Y - FlySize * 0.5 - 6.0 - Line.Y : FlyPosition.Y + FlySize * 0.5 + 4.0;
	const double X = FlyPosition.X - Line.X * 0.5;
	return FVector2D(FMath::Clamp(X, 8.0, FMath::Max(8.0, Size.X - Line.X - 8.0)), Y);
}

FVector2D SOptiFlyLayer::BubblePosition() const
{
	const FVector2D Size = LastGeometry.GetLocalSize();
	const FVector2D Bubble = BubbleSize();
	const bool bLeft = FlyPosition.X > Size.X * 0.5;
	const double X = bLeft ? FlyPosition.X - FlySize * 0.5 - 10.0 - Bubble.X : FlyPosition.X + FlySize * 0.5 + 10.0;
	const double Y = FMath::Clamp(FlyPosition.Y - 40.0, 8.0, FMath::Max(8.0, Size.Y - Bubble.Y - 8.0));
	return FVector2D(FMath::Clamp(X, 8.0, FMath::Max(8.0, Size.X - Bubble.X - 8.0)), Y);
}

void SOptiFlyLayer::ShowBubbleContent(TSharedRef<SWidget> Content)
{
	BubbleHost->SetContent(
		SNew(SBorder)
		.BorderImage(FOptiStyle::Get().GetBrush("Opti.Bubble"))
		.Padding(FMargin(14.f, 12.f))
		[
			Content
		]);
	bBubbleVisible = true;
	BubbleShownAt = FPlatformTime::Seconds();
}

void SOptiFlyLayer::HideBubble(bool bIgnored)
{
	if (!bBubbleVisible)
	{
		return;
	}
	bBubbleVisible = false;
	BubbleHost->SetContent(SNullWidget::NullWidget);
	if (bIgnored && bBubbleIsFinding)
	{
		if (TSharedPtr<FOptiCompanion> C = Companion.Pin())
		{
			C->OnNoticeIgnored();
		}
	}
}

void SOptiFlyLayer::ShowMessage(const FText& Message)
{
	bBubbleIsFinding = false;
	ShowBubbleContent(
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.f)
		[
			SNew(STextBlock)
			.Text(Message)
			.AutoWrapText(true)
			.TextStyle(FOptiStyle::Get(), "Opti.Bubble.Text")
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Top).Padding(6.f, 0.f, 0.f, 0.f)
		[
			SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.OnClicked_Lambda([this]() { HideBubble(false); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(FText::FromString(TEXT("x"))).ColorAndOpacity(FOptiStyle::InkDim)
			]
		]);
}

TSharedRef<SWidget> SOptiFlyLayer::MakeLaterMenu(FGuid Id)
{
	TWeakPtr<FOptiCompanion> Weak = Companion;
	auto Postpone = [this, Weak, Id](EOptiReminder Reminder)
	{
		return FUIAction(FExecuteAction::CreateLambda([this, Weak, Id, Reminder]()
		{
			if (TSharedPtr<FOptiCompanion> C = Weak.Pin()) { C->PostponeFinding(Id, Reminder); }
			HideBubble(false);
		}));
	};
	FMenuBuilder Menu(true, nullptr);
	Menu.AddMenuEntry(OptiText(TEXT("later.twohours")), FText::GetEmpty(), FSlateIcon(), Postpone(EOptiReminder::InTwoHours));
	Menu.AddMenuEntry(OptiText(TEXT("later.nextsession")), FText::GetEmpty(), FSlateIcon(), Postpone(EOptiReminder::NextSession));
	Menu.AddMenuEntry(OptiText(TEXT("later.slowframe")), FText::GetEmpty(), FSlateIcon(), Postpone(EOptiReminder::WhenFrameIsSlow));
	Menu.AddSeparator();
	Menu.AddMenuEntry(OptiText(TEXT("later.never")), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([this, Weak, Id]()
	{
		if (TSharedPtr<FOptiCompanion> C = Weak.Pin()) { C->DismissFinding(Id, false); }
		HideBubble(false);
	})));
	return Menu.MakeWidget();
}

TSharedRef<SWidget> SOptiFlyLayer::MakeFindingBubble(TSharedRef<FOptiFinding> Finding)
{
	TSharedPtr<FOptiCompanion> C = Companion.Pin();
	const FGuid Id = Finding->Id;
	TWeakPtr<FOptiCompanion> Weak = Companion;
	const bool bRegression = Finding->Kind == EOptiFindingKind::Regression;
	const bool bResolved = Finding->State == EOptiFindingState::ResolvedByUser;

	FText Eyebrow;
	if (bRegression)
	{
		Eyebrow = OptiText(TEXT("kind.Regression"));
	}
	else if (const FOptiAction* Action = OptiActions::Find(Finding->ActionId))
	{
		Eyebrow = FText::Format(LOCTEXT("Eyebrow", "{0} · {1}"), FOptiPhrases::Get().Text(FString(TEXT("state.")) + LexToString(Finding->State)),
			FOptiPhrases::Get().Sector(Action->Sector));
	}

	const FText Numbers = bRegression
		? FText::Format(LOCTEXT("RegressionNumbers", "{0}: {1} -> {2} ms"), FText::FromString(Finding->Metric), Ms(Finding->BaselineMs), Ms(Finding->BaselineMs + Finding->GainMs))
		: FText::Format(LOCTEXT("FindingNumbers", "{0} -{1} ms  ·  95%: {2} to {3}  ·  FLIP {4}"), FText::FromString(Finding->Metric),
			Ms(Finding->GainMs), Ms(Finding->CILowMs), Ms(Finding->CIHighMs), FText::AsNumber(Finding->VisualMean));

	auto Button = [](const FText& Label, bool bPrimary, TFunction<void()> OnClick) -> TSharedRef<SWidget>
	{
		return SNew(SButton)
			.ButtonStyle(FAppStyle::Get(), bPrimary ? "PrimaryButton" : "Button")
			.OnClicked_Lambda([OnClick]() { OnClick(); return FReply::Handled(); })
			[
				SNew(STextBlock).Text(Label)
			];
	};

	TSharedRef<SHorizontalBox> Buttons = SNew(SHorizontalBox);
	if (bRegression)
	{
		Buttons->AddSlot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)[Button(OptiText(TEXT("ui.openasset")), true, [this, Weak, Id]() { if (auto P = Weak.Pin()) { P->OpenAsset(Id); } HideBubble(false); })];
		Buttons->AddSlot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)[Button(OptiText(TEXT("ui.intentional")), false, [this, Weak, Id]() { if (auto P = Weak.Pin()) { P->DismissFinding(Id, false, TEXT("Intentional")); } HideBubble(false); })];
	}
	else if (!bResolved)
	{
		Buttons->AddSlot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)[Button(OptiText(TEXT("ui.apply")), true, [this, Weak, Id]() { if (auto P = Weak.Pin()) { P->ApplyFinding(Id); } HideBubble(false); })];
		Buttons->AddSlot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)
		[
			SNew(SComboButton)
			.ButtonContent()[SNew(STextBlock).Text(OptiText(TEXT("ui.later")))]
			.OnGetMenuContent_Lambda([this, Id]() { return MakeLaterMenu(Id); })
		];
		Buttons->AddSlot().AutoWidth().Padding(0.f, 0.f, 6.f, 0.f)[Button(OptiText(TEXT("ui.details")), false, [this, Finding]() { OptiOpenFindingDetails(Finding); HideBubble(false); })];
		Buttons->AddSlot().FillWidth(1.f);
		Buttons->AddSlot().AutoWidth()
		[
			SNew(SComboButton)
			.HasDownArrow(false)
			.ButtonStyle(FAppStyle::Get(), "SimpleButton")
			.ButtonContent()[SNew(STextBlock).Text(FText::FromString(TEXT("..."))).ColorAndOpacity(FOptiStyle::InkDim)]
			.OnGetMenuContent_Lambda([this, Weak, Id]()
			{
				FMenuBuilder Menu(true, nullptr);
				Menu.AddMenuEntry(OptiText(TEXT("ui.dismiss")), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([this, Weak, Id]()
				{
					if (auto P = Weak.Pin()) { P->DismissFinding(Id, false); }
					HideBubble(false);
				})));
				Menu.AddMenuEntry(OptiText(TEXT("ui.dontsuggest")), FText::GetEmpty(), FSlateIcon(), FUIAction(FExecuteAction::CreateLambda([this, Weak, Id]()
				{
					if (auto P = Weak.Pin()) { P->DismissFinding(Id, true); }
					HideBubble(false);
				})));
				return Menu.MakeWidget();
			})
		];
	}

	return SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight()
		[
			SNew(SHorizontalBox)
			+ SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(Eyebrow).TextStyle(FOptiStyle::Get(), "Opti.Bubble.Small")
			]
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SButton)
				.ButtonStyle(FAppStyle::Get(), "SimpleButton")
				.OnClicked_Lambda([this]() { HideBubble(false); if (auto P = Companion.Pin()) { P->OnNoticeAnswered(); } return FReply::Handled(); })
				[
					SNew(STextBlock).Text(FText::FromString(TEXT("x"))).ColorAndOpacity(FOptiStyle::InkDim)
				]
			]
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 6.f)
		[
			SNew(STextBlock)
			.Text(C.IsValid() ? C->DescribeFinding(*Finding) : FText::GetEmpty())
			.AutoWrapText(true)
			.TextStyle(FOptiStyle::Get(), "Opti.Bubble.Text")
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.f, 0.f, 0.f, 8.f)
		[
			SNew(STextBlock).Text(Numbers).TextStyle(FOptiStyle::Get(), "Opti.Bubble.Small").AutoWrapText(true)
		]
		+ SVerticalBox::Slot().AutoHeight()
		[
			Buttons
		];
}

void SOptiFlyLayer::ShowBubble(TSharedRef<FOptiFinding> Finding)
{
	bBubbleIsFinding = true;
	ShowBubbleContent(MakeFindingBubble(Finding));
}

#undef LOCTEXT_NAMESPACE
