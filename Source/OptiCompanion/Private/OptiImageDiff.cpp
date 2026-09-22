#include "OptiImageDiff.h"
#include "OptiProbe.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Modules/ModuleManager.h"
#include "Misc/FileHelper.h"

namespace OptiImage
{
	namespace
	{
		struct FPlane3
		{
			int32 W = 0, H = 0;
			TArray<float> C[3];

			void Init(int32 InW, int32 InH)
			{
				W = InW; H = InH;
				for (TArray<float>& Channel : C) { Channel.SetNumZeroed(W * H); }
			}
		};

		float SrgbToLinear(float V)
		{
			return V <= 0.04045f ? V / 12.92f : FMath::Pow((V + 0.055f) / 1.055f, 2.4f);
		}

		// D65 reference white.
		constexpr float Xn = 0.950456f, Yn = 1.f, Zn = 1.088754f;

		void LinearToXyz(float R, float G, float B, float& X, float& Y, float& Z)
		{
			X = 0.4124564f * R + 0.3575761f * G + 0.1804375f * B;
			Y = 0.2126729f * R + 0.7151522f * G + 0.0721750f * B;
			Z = 0.0193339f * R + 0.1191920f * G + 0.9503041f * B;
		}

		float LabF(float T)
		{
			constexpr float Delta = 6.f / 29.f;
			return T > Delta * Delta * Delta ? FMath::Pow(T, 1.f / 3.f) : T / (3.f * Delta * Delta) + 4.f / 29.f;
		}

		/** sRGB capture -> YyCxCz, the linear opponent space FLIP filters in. */
		FPlane3 ToOpponent(const FOptiCapture& Image)
		{
			FPlane3 Out;
			Out.Init(Image.Width, Image.Height);
			for (int32 Index = 0; Index < Image.Pixels.Num(); ++Index)
			{
				const FColor& P = Image.Pixels[Index];
				float X, Y, Z;
				LinearToXyz(SrgbToLinear(P.R / 255.f), SrgbToLinear(P.G / 255.f), SrgbToLinear(P.B / 255.f), X, Y, Z);
				Out.C[0][Index] = 116.f * (Y / Yn) - 16.f;
				Out.C[1][Index] = 500.f * (X / Xn - Y / Yn);
				Out.C[2][Index] = 200.f * (Y / Yn - Z / Zn);
			}
			return Out;
		}

		/** Separable Gaussian: a cheap stand-in for FLIP's contrast sensitivity functions. */
		void Blur(TArray<float>& Channel, int32 W, int32 H, float Sigma)
		{
			const int32 Radius = FMath::CeilToInt32(Sigma * 3.f);
			TArray<float> Kernel;
			float Sum = 0.f;
			for (int32 K = -Radius; K <= Radius; ++K)
			{
				Kernel.Add(FMath::Exp(-(K * K) / (2.f * Sigma * Sigma)));
				Sum += Kernel.Last();
			}
			for (float& K : Kernel) { K /= Sum; }

			TArray<float> Temp;
			Temp.SetNumZeroed(Channel.Num());
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					float Acc = 0.f;
					for (int32 K = -Radius; K <= Radius; ++K)
					{
						Acc += Kernel[K + Radius] * Channel[Y * W + FMath::Clamp(X + K, 0, W - 1)];
					}
					Temp[Y * W + X] = Acc;
				}
			}
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					float Acc = 0.f;
					for (int32 K = -Radius; K <= Radius; ++K)
					{
						Acc += Kernel[K + Radius] * Temp[FMath::Clamp(Y + K, 0, H - 1) * W + X];
					}
					Channel[Y * W + X] = Acc;
				}
			}
		}

		/** Filtered YyCxCz -> CIELAB, clamping back into gamut like FLIP does. */
		void OpponentToLab(const FPlane3& In, int32 Index, float& L, float& A, float& B)
		{
			const float Y = (In.C[0][Index] + 16.f) / 116.f * Yn;
			const float X = (In.C[1][Index] / 500.f + Y / Yn) * Xn;
			const float Z = (Y / Yn - In.C[2][Index] / 200.f) * Zn;

			// Round-trip through clamped linear RGB.
			float R = 3.2404542f * X - 1.5371385f * Y - 0.4985314f * Z;
			float G = -0.9692660f * X + 1.8760108f * Y + 0.0415560f * Z;
			float Bl = 0.0556434f * X - 0.2040259f * Y + 1.0572252f * Z;
			R = FMath::Clamp(R, 0.f, 1.f); G = FMath::Clamp(G, 0.f, 1.f); Bl = FMath::Clamp(Bl, 0.f, 1.f);

			float X2, Y2, Z2;
			LinearToXyz(R, G, Bl, X2, Y2, Z2);
			const float Fx = LabF(X2 / Xn), Fy = LabF(Y2 / Yn), Fz = LabF(Z2 / Zn);
			L = 116.f * Fy - 16.f;
			A = 500.f * (Fx - Fy);
			B = 200.f * (Fy - Fz);
		}

		/** Sobel edge magnitude of the unfiltered luminance, normalised to 0..1. */
		TArray<float> Edges(const FOptiCapture& Image)
		{
			const int32 W = Image.Width, H = Image.Height;
			TArray<float> Lum;
			Lum.SetNumUninitialized(W * H);
			for (int32 Index = 0; Index < Lum.Num(); ++Index)
			{
				const FColor& P = Image.Pixels[Index];
				Lum[Index] = (0.2126f * P.R + 0.7152f * P.G + 0.0722f * P.B) / 255.f;
			}
			TArray<float> Out;
			Out.SetNumZeroed(W * H);
			auto At = [&](int32 X, int32 Y) { return Lum[FMath::Clamp(Y, 0, H - 1) * W + FMath::Clamp(X, 0, W - 1)]; };
			for (int32 Y = 0; Y < H; ++Y)
			{
				for (int32 X = 0; X < W; ++X)
				{
					const float Gx = -At(X - 1, Y - 1) - 2.f * At(X - 1, Y) - At(X - 1, Y + 1) + At(X + 1, Y - 1) + 2.f * At(X + 1, Y) + At(X + 1, Y + 1);
					const float Gy = -At(X - 1, Y - 1) - 2.f * At(X, Y - 1) - At(X + 1, Y - 1) + At(X - 1, Y + 1) + 2.f * At(X, Y + 1) + At(X + 1, Y + 1);
					Out[Y * W + X] = FMath::Min(1.f, FMath::Sqrt(Gx * Gx + Gy * Gy) / 4.f);
				}
			}
			return Out;
		}
	}

	FOptiImageDiff Compare(const FOptiCapture& A, const FOptiCapture& B)
	{
		FOptiImageDiff Diff;
		if (!A.IsValid() || !B.IsValid() || A.Width != B.Width || A.Height != B.Height)
		{
			return Diff;
		}
		const int32 W = A.Width, H = A.Height;

		// Sigmas in capture pixels (about 320 wide): achromatic detail survives, chroma is blurred more.
		FPlane3 OA = ToOpponent(A), OB = ToOpponent(B);
		for (FPlane3* Plane : { &OA, &OB })
		{
			Blur(Plane->C[0], W, H, 0.6f);
			Blur(Plane->C[1], W, H, 1.2f);
			Blur(Plane->C[2], W, H, 1.6f);
		}

		const TArray<float> EdgesA = Edges(A);
		const TArray<float> EdgesB = Edges(B);

		// HyAB distance between green and blue is roughly the largest one in gamut; FLIP normalises by it.
		constexpr float MaxHyAB = 308.f;
		constexpr float Compress = 0.7f;

		Diff.Width = W;
		Diff.Height = H;
		Diff.ErrorMap.SetNumUninitialized(W * H);
		double Sum = 0.0;
		for (int32 Index = 0; Index < W * H; ++Index)
		{
			float L1, A1, B1, L2, A2, B2;
			OpponentToLab(OA, Index, L1, A1, B1);
			OpponentToLab(OB, Index, L2, A2, B2);
			const float HyAB = FMath::Abs(L1 - L2) + FMath::Sqrt((A1 - A2) * (A1 - A2) + (B1 - B2) * (B1 - B2));
			const float ColorError = FMath::Pow(FMath::Clamp(HyAB / MaxHyAB, 0.f, 1.f), Compress);
			const float EdgeError = FMath::Clamp(FMath::Abs(EdgesA[Index] - EdgesB[Index]), 0.f, 1.f);
			const float Error = FMath::Pow(ColorError, 1.f - EdgeError);
			Diff.ErrorMap[Index] = Error;
			Sum += Error;
		}
		Diff.Mean = static_cast<float>(Sum / (W * H));

		TArray<float> Sorted = Diff.ErrorMap;
		Sorted.Sort();
		Diff.P95 = Sorted[FMath::Clamp(FMath::FloorToInt32(0.95f * (Sorted.Num() - 1)), 0, Sorted.Num() - 1)];
		return Diff;
	}

	TArray<FColor> Heatmap(const FOptiImageDiff& Diff)
	{
		static const FLinearColor Ramp[] = {
			FLinearColor(0.f, 0.f, 0.f), FLinearColor(0.23f, 0.10f, 0.39f), FLinearColor(0.70f, 0.20f, 0.35f),
			FLinearColor(0.94f, 0.54f, 0.24f), FLinearColor(0.98f, 0.89f, 0.48f) };
		constexpr int32 Stops = UE_ARRAY_COUNT(Ramp) - 1;

		TArray<FColor> Out;
		Out.Reserve(Diff.ErrorMap.Num());
		for (float Error : Diff.ErrorMap)
		{
			// Errors are small in practice, so the ramp is stretched with a square root to stay readable.
			const float T = FMath::Clamp(FMath::Sqrt(Error), 0.f, 1.f) * Stops;
			const int32 I = FMath::Min(FMath::FloorToInt32(T), Stops - 1);
			Out.Add(FMath::Lerp(Ramp[I], Ramp[I + 1], T - I).ToFColor(false));
		}
		return Out;
	}

	bool SavePng(const FString& Path, int32 Width, int32 Height, const TArray<FColor>& Pixels)
	{
		if (Width <= 0 || Height <= 0 || Pixels.Num() != Width * Height)
		{
			return false;
		}
		IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
		TSharedPtr<IImageWrapper> Wrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
		if (!Wrapper.IsValid() || !Wrapper->SetRaw(Pixels.GetData(), Pixels.Num() * sizeof(FColor), Width, Height, ERGBFormat::BGRA, 8))
		{
			return false;
		}
		const TArray64<uint8>& Compressed = Wrapper->GetCompressed();
		return FFileHelper::SaveArrayToFile(Compressed, *Path);
	}
}
