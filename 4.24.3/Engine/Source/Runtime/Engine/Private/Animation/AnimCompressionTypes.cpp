// Copyright 1998-2019 Epic Games, Inc. All Rights Reserved.

#include "Animation/AnimCompressionTypes.h"
#include "AnimationUtils.h"
#include "AnimEncoding.h"
#include "Misc/SecureHash.h"
#include "Interfaces/ITargetPlatform.h"
#include "Animation/AnimCurveCompressionCodec.h"
#include "Animation/AnimCurveCompressionSettings.h"
#include "AnimationRuntime.h"
#include "UObject/FortniteMainBranchObjectVersion.h"

#define ACL_ENABLE_UE4_DECOMP_STAT_LOGGING		0		// @third party code - ACLPlugin

CSV_DEFINE_CATEGORY(Animation, false);

DECLARE_CYCLE_STAT(TEXT("Build Anim Track Pairs"), STAT_BuildAnimTrackPairs, STATGROUP_Anim);
DECLARE_CYCLE_STAT(TEXT("Extract Pose From Anim Data"), STAT_ExtractPoseFromAnimData, STATGROUP_Anim);

template <typename ArrayType>
void UpdateSHAWithArray(FSHA1& Sha, const TArray<ArrayType>& Array)
{
	Sha.Update((uint8*)Array.GetData(), Array.Num() * Array.GetTypeSize());
}

void UpdateSHAWithRawTrack(FSHA1& Sha, const FRawAnimSequenceTrack& RawTrack)
{
	UpdateSHAWithArray(Sha, RawTrack.PosKeys);
	UpdateSHAWithArray(Sha, RawTrack.RotKeys);
	UpdateSHAWithArray(Sha, RawTrack.ScaleKeys);
}

template<class DataType>
void UpdateWithData(FSHA1& Sha, const DataType& Data)
{
	Sha.Update((uint8*)(&Data), sizeof(DataType));
}

void UpdateSHAWithCurves(FSHA1& Sha, const FRawCurveTracks& InRawCurveData) 
{
	for (const FFloatCurve& Curve : InRawCurveData.FloatCurves)
	{
		UpdateWithData(Sha, Curve.Name.UID);
		UpdateWithData(Sha, Curve.FloatCurve.DefaultValue);
		UpdateSHAWithArray(Sha, Curve.FloatCurve.GetConstRefOfKeys());
		UpdateWithData(Sha, Curve.FloatCurve.PreInfinityExtrap);
		UpdateWithData(Sha, Curve.FloatCurve.PostInfinityExtrap);
	}
}

FGuid GenerateGuidFromRawAnimData(const TArray<FRawAnimSequenceTrack>& RawAnimationData, const FRawCurveTracks& RawCurveData)
{
	FSHA1 Sha;

	for (const FRawAnimSequenceTrack& Track : RawAnimationData)
	{
		UpdateSHAWithRawTrack(Sha, Track);
	}

	UpdateSHAWithCurves(Sha, RawCurveData);

	Sha.Final();

	uint32 Hash[5];
	Sha.GetHash((uint8*)Hash);
	FGuid Guid(Hash[0] ^ Hash[4], Hash[1], Hash[2], Hash[3]);
	return Guid;
}

template<typename ArrayValue>
void StripFramesEven(TArray<ArrayValue>& Keys, const int32 NumFrames)
{
	if (Keys.Num() > 1)
	{
		check(Keys.Num() == NumFrames);

		for (int32 DstKey = 1, SrcKey = 2; SrcKey < NumFrames; ++DstKey, SrcKey += 2)
		{
			Keys[DstKey] = Keys[SrcKey];
		}

		const int32 HalfSize = (NumFrames - 1) / 2;
		const int32 StartRemoval = HalfSize + 1;

		Keys.RemoveAt(StartRemoval, NumFrames - StartRemoval);
	}
}

template<typename ArrayValue>
void StripFramesOdd(TArray<ArrayValue>& Keys, const int32 NumFrames)
{
	if (Keys.Num() > 1)
	{
		const int32 NewNumFrames = NumFrames / 2;

		TArray<ArrayValue> NewKeys;
		NewKeys.Reserve(NewNumFrames);

		check(Keys.Num() == NumFrames);

		NewKeys.Add(Keys[0]); //Always keep first 

		//Always keep first and last
		const int32 NumFramesToCalculate = NewNumFrames - 2;

		// Frame increment is ratio of old frame spaces vs new frame spaces 
		const double FrameIncrement = (double)(NumFrames - 1) / (double)(NewNumFrames - 1);

		for (int32 Frame = 0; Frame < NumFramesToCalculate; ++Frame)
		{
			const double NextFramePosition = FrameIncrement * (Frame + 1);
			const int32 Frame1 = (int32)NextFramePosition;
			const float Alpha = (NextFramePosition - (double)Frame1);

			NewKeys.Add(AnimationCompressionUtils::Interpolate(Keys[Frame1], Keys[Frame1 + 1], Alpha));

		}

		NewKeys.Add(Keys.Last()); // Always Keep Last

		const int32 HalfSize = (NumFrames - 1) / 2;
		const int32 StartRemoval = HalfSize + 1;

		Keys = MoveTemp(NewKeys);
	}
}

FCompressibleAnimData::FCompressibleAnimData(class UAnimSequence* InSeq, const bool bPerformStripping, const float InAltCompressionErrorThreshold) :
#if WITH_EDITOR
	RequestedCompressionScheme(InSeq->CompressionScheme) ,
#endif
	CurveCompressionSettings(InSeq->CurveCompressionSettings)
	, Skeleton(InSeq->GetSkeleton())
	, TrackToSkeletonMapTable(InSeq->GetRawTrackToSkeletonMapTable())
	, Interpolation(InSeq->Interpolation)
	, SequenceLength(InSeq->SequenceLength)
	, NumFrames(InSeq->GetRawNumberOfFrames())
	, bIsValidAdditive(InSeq->IsValidAdditive())
	, AltCompressionErrorThreshold(InAltCompressionErrorThreshold)
	, Name(InSeq->GetName())
	, FullName(InSeq->GetFullName())
	, AnimFName(InSeq->GetFName())
{
#if WITH_EDITOR
	FAnimationUtils::BuildSkeletonMetaData(Skeleton, BoneData);

	const bool bHasVirtualBones = InSeq->GetSkeleton()->GetVirtualBones().Num() > 0;

	if (InSeq->CanBakeAdditive())
	{
		TArray<FName> TempTrackNames;
		InSeq->BakeOutAdditiveIntoRawData(RawAnimationData, TempTrackNames, TrackToSkeletonMapTable, RawCurveData, AdditiveBaseAnimationData);
	}
	else if (bHasVirtualBones)// If we aren't additive we must bake virtual bones
	{
		TArray<FName> TempTrackNames;
		InSeq->BakeOutVirtualBoneTracks(RawAnimationData, TempTrackNames, TrackToSkeletonMapTable);
		RawCurveData = InSeq->RawCurveData;
	}
	else
	{
		RawAnimationData = InSeq->GetRawAnimationData();
		TrackToSkeletonMapTable = InSeq->GetRawTrackToSkeletonMapTable();
		RawCurveData = InSeq->RawCurveData;
	}

	if (bPerformStripping)
	{
		const int32 NumTracks = RawAnimationData.Num();
		
		// End frame does not count towards "Even framed" calculation
		const bool bIsEvenFramed = ((NumFrames - 1) % 2) == 0;

		//Strip every other frame from tracks
		if (bIsEvenFramed)
		{
			for (FRawAnimSequenceTrack& Track : RawAnimationData)
			{
				StripFramesEven(Track.PosKeys, NumFrames);
				StripFramesEven(Track.RotKeys, NumFrames);
				StripFramesEven(Track.ScaleKeys, NumFrames);
			}

			const int32 ActualFrames = NumFrames - 1; // strip bookmark end frame
			NumFrames = (ActualFrames / 2) + 1;
		}
		else
		{
			for (FRawAnimSequenceTrack& Track : RawAnimationData)
			{
				StripFramesOdd(Track.PosKeys, NumFrames);
				StripFramesOdd(Track.RotKeys, NumFrames);
				StripFramesOdd(Track.ScaleKeys, NumFrames);
			}

			const int32 ActualFrames = NumFrames;
			NumFrames = (ActualFrames / 2);
		}
	}
#endif
}

FCompressibleAnimData::FCompressibleAnimData(UAnimCompress* InRequestedCompressionScheme, UAnimCurveCompressionSettings* InCurveCompressionSettings, USkeleton* InSkeleton, EAnimInterpolationType InInterpolation, float InSequenceLength, int32 InNumFrames, const float InAltCompressionErrorThreshold) :
#if WITH_EDITOR
	RequestedCompressionScheme(InRequestedCompressionScheme) ,
#endif
	CurveCompressionSettings(InCurveCompressionSettings)
	, Skeleton(InSkeleton)
	, Interpolation(InInterpolation)
	, SequenceLength(InSequenceLength)
	, NumFrames(InNumFrames)
	, bIsValidAdditive(false)
	, AltCompressionErrorThreshold(InAltCompressionErrorThreshold)
{
#if WITH_EDITOR
	FAnimationUtils::BuildSkeletonMetaData(Skeleton, BoneData);
#endif
}

FCompressibleAnimData::FCompressibleAnimData() : RequestedCompressionScheme(nullptr)
, CurveCompressionSettings(nullptr)
, Skeleton(nullptr)
, Interpolation((EAnimInterpolationType)0)
, SequenceLength(0.f)
, NumFrames(0)
, bIsValidAdditive(false)
, AltCompressionErrorThreshold(FAnimationUtils::GetAlternativeCompressionThreshold())
{

}

void FCompressibleAnimData::Update(FCompressedAnimSequence& InOutCompressedData) const
{
	InOutCompressedData.CompressedTrackToSkeletonMapTable = TrackToSkeletonMapTable;
	InOutCompressedData.CompressedRawDataSize = GetApproxRawSize();

	const int32 NumCurves = RawCurveData.FloatCurves.Num();
	InOutCompressedData.CompressedCurveNames.Reset(NumCurves);
	InOutCompressedData.CompressedCurveNames.AddUninitialized(NumCurves);
	for (int32 CurveIndex = 0; CurveIndex < NumCurves; ++CurveIndex)
	{
		const FFloatCurve& Curve = RawCurveData.FloatCurves[CurveIndex];
		InOutCompressedData.CompressedCurveNames[CurveIndex] = Curve.Name;
	}
}

template<typename T>
void WriteArray(FMemoryWriter& MemoryWriter, TArray<T>& Array)
{
	const int64 NumBytes = (Array.GetTypeSize() * Array.Num());
	MemoryWriter.Serialize(Array.GetData(), NumBytes);
}

void FCompressibleAnimDataResult::BuildFinalBuffer(TArray<uint8>& OutBuffer)
{
	OutBuffer.Reset();
	FMemoryWriter MemoryWriter(OutBuffer);

	WriteArray(MemoryWriter, CompressedTrackOffsets);
	WriteArray(MemoryWriter, CompressedScaleOffsets.OffsetData);
	WriteArray(MemoryWriter, CompressedByteStream);
}

template<typename T>
void InitArrayView(TArrayView<T>& View, uint8*& DataPtr)
{
	View = TArrayView<T>((T*)DataPtr, View.Num());
	DataPtr += (View.Num() * View.GetTypeSize());
}

template<typename T>
void ResetArrayView(TArrayView<T>& ArrayView)
{
	ArrayView = TArrayView<T>();
}

void FUECompressedAnimData::Reset()
{
	ResetArrayView(CompressedTrackOffsets);
	ResetArrayView(CompressedScaleOffsets.OffsetData);
	ResetArrayView(CompressedByteStream);

	TranslationCompressionFormat = RotationCompressionFormat = ScaleCompressionFormat = ACF_None;
	TranslationCodec = RotationCodec = ScaleCodec = nullptr;
}

void FUECompressedAnimData::InitViewsFromBuffer(const TArrayView<uint8> BulkData)
{
	check(BulkData.Num() > 0);

	uint8* BulkDataPtr = BulkData.GetData();
	
	InitArrayView(CompressedTrackOffsets, BulkDataPtr);
	InitArrayView(CompressedScaleOffsets.OffsetData, BulkDataPtr);
	InitArrayView(CompressedByteStream, BulkDataPtr);

	check((BulkDataPtr - BulkData.GetData()) == BulkData.Num());
}

template<typename T>
void InitArrayViewSize(TArrayView<T>& Dest, const TArray<T>& Src)
{
	Dest = TArrayView<T>((T*)nullptr, Src.Num());
}

#if WITH_EDITOR
void FUECompressedAnimData::CopyFrom(const FCompressibleAnimDataResult& Other)
{
	InitArrayViewSize(CompressedTrackOffsets, Other.CompressedTrackOffsets);
	InitArrayViewSize(CompressedScaleOffsets.OffsetData, Other.CompressedScaleOffsets.OffsetData);
	InitArrayViewSize(CompressedByteStream, Other.CompressedByteStream);

	CompressedScaleOffsets.StripSize = Other.CompressedScaleOffsets.StripSize;

	CopyFromSettings(Other);
}
#endif

template<typename T>
void SerializeView(class FArchive& Ar, TArrayView<T>& View)
{
	int32 Size = View.Num();
	if (Ar.IsLoading())
	{
		Ar << Size;
		View = TArrayView<T>((T*)nullptr, Size);
	}
	else
	{
		Ar << Size;
	}
}

template<typename EnumType>
void SerializeEnum(FArchive& Ar, EnumType& Val)
{
	uint8 Temp = (uint8)Val;
	if (Ar.IsLoading())
	{
		Ar << Temp;
		Val = (EnumType)Temp;
	}
	else
	{
		Ar << Temp;
	}
}

FArchive& operator<<(FArchive& Ar, AnimationCompressionFormat& Fmt)
{
	SerializeEnum(Ar, Fmt);
	return Ar;
}

FArchive& operator<<(FArchive& Ar, AnimationKeyFormat& Fmt)
{
	SerializeEnum(Ar, Fmt);
	return Ar;
}

void FUECompressedAnimData::SerializeCompressedData(class FArchive& Ar)
{
	Ar << KeyEncodingFormat;
	Ar << TranslationCompressionFormat;
	Ar << RotationCompressionFormat;
	Ar << ScaleCompressionFormat;

	Ar << CompressedNumberOfFrames;

	SerializeView(Ar, CompressedTrackOffsets);
	SerializeView(Ar, CompressedScaleOffsets.OffsetData);
	Ar << CompressedScaleOffsets.StripSize;
	SerializeView(Ar, CompressedByteStream);

	AnimationFormat_SetInterfaceLinks(*this);
}

template<typename TArchive, typename T>
void ByteSwapArray(TArchive& MemoryStream, uint8*& StartOfArray, TArrayView<T>& ArrayView)
{
	for (int32 ItemIndex = 0; ItemIndex < ArrayView.Num(); ++ItemIndex)
	{
		AC_UnalignedSwap(MemoryStream, StartOfArray, ArrayView.GetTypeSize());
	}
}

template<typename TArchive>
void ByteSwapCodecData(class AnimEncoding& Codec, TArchive& MemoryStream, FUECompressedAnimData& CompressedData)
{
	check(false);
}

template<>
void ByteSwapCodecData(class AnimEncoding& Codec, FMemoryWriter& MemoryStream, FUECompressedAnimData& CompressedData)
{
	Codec.ByteSwapOut(CompressedData, MemoryStream);
}

template<>
void ByteSwapCodecData(class AnimEncoding& Codec, FMemoryReader& MemoryStream, FUECompressedAnimData& CompressedData)
{
	Codec.ByteSwapIn(CompressedData, MemoryStream);
}

template<typename TArchive>
void FUECompressedAnimData::ByteSwapData(TArrayView<uint8> CompressedData, TArchive& MemoryStream)
{
	//Handle Array Header
	uint8* MovingCompressedDataPtr = CompressedData.GetData();

	ByteSwapArray(MemoryStream, MovingCompressedDataPtr, CompressedTrackOffsets);
	ByteSwapArray(MemoryStream, MovingCompressedDataPtr, CompressedScaleOffsets.OffsetData);
	
	AnimationFormat_SetInterfaceLinks(*this);
	check(RotationCodec);

	ByteSwapCodecData(*RotationCodec, MemoryStream, *this);
}

template void FUECompressedAnimData::ByteSwapData(TArrayView<uint8> CompressedData, FMemoryReader& MemoryStream);
template void FUECompressedAnimData::ByteSwapData(TArrayView<uint8> CompressedData, FMemoryWriter& MemoryStream);

void ValidateUObjectLoaded(UObject* Obj, UObject* Source)
{
#if WITH_EDITOR
	if (FLinkerLoad* ObjLinker = Obj->GetLinker())
	{
		ObjLinker->Preload(Obj);
	}
#endif
	checkf(!Obj->HasAnyFlags(RF_NeedLoad), TEXT("Failed to load %s in %s"), *Obj->GetFullName(), *Source->GetFullName()); // in non editor should have been preloaded by GetPreloadDependencies
}

void FCompressedAnimSequence::SerializeCompressedData(FArchive& Ar, bool bDDCData, UObject* DataOwner, USkeleton* Skeleton, UAnimCurveCompressionSettings* CurveCompressionSettings, bool bCanUseBulkData)
{
	Ar << CompressedRawDataSize;
	Ar << CompressedTrackToSkeletonMapTable;
	Ar << CompressedCurveNames;

	CompressedDataStructure.SerializeCompressedData(Ar);

	// Serialize the compressed byte stream from the archive to the buffer.
	int32 NumBytes = CompressedByteStream.Num();
	Ar << NumBytes;

	if (Ar.IsLoading())
	{
		bool bUseBulkDataForLoad = false;
		if (!bDDCData && Ar.CustomVer(FFortniteMainBranchObjectVersion::GUID) >= FFortniteMainBranchObjectVersion::FortMappedCookedAnimation)
		{
			Ar << bUseBulkDataForLoad;
		}
		if (bUseBulkDataForLoad)
		{
#if !WITH_EDITOR
			FByteBulkData OptionalBulk;
#endif
			bool bUseMapping = FPlatformProperties::SupportsMemoryMappedFiles() && FPlatformProperties::SupportsMemoryMappedAnimation();
			OptionalBulk.Serialize(Ar, DataOwner, -1, bUseMapping);

			if (!bUseMapping)
			{
				OptionalBulk.ForceBulkDataResident();
			}

			size_t Size = OptionalBulk.GetBulkDataSize();

			FOwnedBulkDataPtr* OwnedPtr = OptionalBulk.StealFileMapping();

#if WITH_EDITOR
			check(!bUseMapping && !OwnedPtr->GetMappedHandle());
			CompressedByteStream.Empty(Size);
			CompressedByteStream.AddUninitialized(Size);
			if (Size)
			{
				FMemory::Memcpy(&CompressedByteStream[0], OwnedPtr->GetPointer(), Size);
			}
#else
			CompressedByteStream.AcceptOwnedBulkDataPtr(OwnedPtr, Size);
#endif
			delete OwnedPtr;

			CompressedDataStructure.InitViewsFromBuffer(CompressedByteStream);
		}
		else
		{
			CompressedByteStream.Empty(NumBytes);
			CompressedByteStream.AddUninitialized(NumBytes);

			if (CompressedByteStream.Num() > 0)
			{
				CompressedDataStructure.InitViewsFromBuffer(CompressedByteStream);
			}

			if (FPlatformProperties::RequiresCookedData())
			{
				Ar.Serialize(CompressedByteStream.GetData(), NumBytes);
			}
			else
			{
				TArray<uint8> SerializedData;
				SerializedData.Empty(NumBytes);
				SerializedData.AddUninitialized(NumBytes);
				Ar.Serialize(SerializedData.GetData(), NumBytes);

				// Swap the buffer into the byte stream.
				FMemoryReader MemoryReader(SerializedData, true);
				MemoryReader.SetByteSwapping(Ar.ForceByteSwapping());

				CompressedDataStructure.ByteSwapIn(CompressedByteStream, MemoryReader);
			}
		}

		FString CurveCodecPath;
		Ar << CurveCodecPath;

		ValidateUObjectLoaded(CurveCompressionSettings, DataOwner);
		CurveCompressionCodec = CurveCompressionSettings->GetCodec(CurveCodecPath);

		int32 NumCurveBytes;
		Ar << NumCurveBytes;

		CompressedCurveByteStream.Empty(NumCurveBytes);
		CompressedCurveByteStream.AddUninitialized(NumCurveBytes);
		Ar.Serialize(CompressedCurveByteStream.GetData(), NumCurveBytes);
	}
	else if (Ar.IsSaving() || Ar.IsCountingMemory())
	{
		// Swap the byte stream into a buffer.
		TArray<uint8> SerializedData;

		const bool bIsCooking = !bDDCData && Ar.IsCooking();

		// and then use the codecs to byte swap
		FMemoryWriter MemoryWriter(SerializedData, true);
		MemoryWriter.SetByteSwapping(Ar.ForceByteSwapping());
		CompressedDataStructure.ByteSwapOut(CompressedByteStream, MemoryWriter);

		// Make sure the entire byte stream was serialized.
		check(NumBytes == SerializedData.Num());

		bool bUseBulkDataForSave = bCanUseBulkData && NumBytes && bIsCooking && Ar.CookingTarget()->SupportsFeature(ETargetPlatformFeatures::MemoryMappedFiles) && Ar.CookingTarget()->SupportsFeature(ETargetPlatformFeatures::MemoryMappedAnimation);

		bool bSavebUseBulkDataForSave = false;
		if (!bDDCData)
		{
			Ar.UsingCustomVersion(FFortniteMainBranchObjectVersion::GUID);
			if (Ar.CustomVer(FFortniteMainBranchObjectVersion::GUID) < FFortniteMainBranchObjectVersion::FortMappedCookedAnimation)
			{
				bUseBulkDataForSave = false;
			}
			else
			{
				bSavebUseBulkDataForSave = true;
			}
		}

		// Count compressed data.
		Ar.CountBytes(SerializedData.Num(), SerializedData.Num());

		if (bSavebUseBulkDataForSave)
		{
			Ar << bUseBulkDataForSave;
		}
		else
		{
			check(!bUseBulkDataForSave);
		}

#define TEST_IS_CORRECTLY_FORMATTED_FOR_MEMORY_MAPPING 0 //Need to fix this
#if TEST_IS_CORRECTLY_FORMATTED_FOR_MEMORY_MAPPING
		if (!IsTemplate() && bIsCooking)
		{
			TArray<uint8> TempSerialized;
			FMemoryWriter MemoryWriter(TempSerialized, true);
			MemoryWriter.SetByteSwapping(Ar.ForceByteSwapping());

			check(RotationCodec != NULL);

			FMemoryReader MemoryReader(TempSerialized, true);
			MemoryReader.SetByteSwapping(Ar.ForceByteSwapping());

			TArray<uint8> SavedCompressedByteStream = CompressedByteStream;
			CompressedByteStream.Empty();

			check(CompressedByteStream.Num() == Num);

			check(FMemory::Memcmp(SerializedData.GetData(), CompressedByteStream.GetData(), Num) == 0);

			CompressedByteStream = SavedCompressedByteStream;
		}
#endif

		if (bUseBulkDataForSave)
		{
#if WITH_EDITOR
			OptionalBulk.Lock(LOCK_READ_WRITE);
			void* Dest = OptionalBulk.Realloc(NumBytes);
			FMemory::Memcpy(Dest, &(SerializedData[0]), NumBytes);
			OptionalBulk.Unlock();
			OptionalBulk.SetBulkDataFlags(BULKDATA_PayloadAtEndOfFile | BULKDATA_PayloadInSeperateFile | BULKDATA_Force_NOT_InlinePayload | BULKDATA_MemoryMappedPayload);
			OptionalBulk.ClearBulkDataFlags(BULKDATA_ForceInlinePayload);
			OptionalBulk.Serialize(Ar, DataOwner);
#else
			UE_LOG(LogAnimation, Fatal, TEXT("Can't save animation as bulk data in non-editor builds!"));
#endif
		}
		else
		{
			Ar.Serialize(SerializedData.GetData(), SerializedData.Num());
		}

		FString CurveCodecPath = CurveCompressionCodec->GetPathName();
		Ar << CurveCodecPath;

		int32 NumCurveBytes = CompressedCurveByteStream.Num();
		Ar << NumCurveBytes;
		Ar.Serialize(CompressedCurveByteStream.GetData(), NumCurveBytes);
	}

#if WITH_EDITOR
	if (bDDCData)
	{
		if (Ar.IsLoading() && Skeleton)
		{
			// Refresh the compressed curve names since the IDs might have changed since
			for (FSmartName& CurveName : CompressedCurveNames)
			{
				Skeleton->VerifySmartName(USkeleton::AnimCurveMappingName, CurveName);
			}
		}
	}
#endif
}

SIZE_T FCompressedAnimSequence::GetMemorySize() const
{
	return	  CompressedTrackToSkeletonMapTable.GetAllocatedSize()
			+ CompressedCurveNames.GetAllocatedSize()
			+ CompressedCurveByteStream.GetAllocatedSize()
			+ CompressedDataStructure.GetApproxBoneCompressedSize()
			+ sizeof(FCompressedAnimSequence);
}

struct FGetBonePoseScratchArea : public TThreadSingleton<FGetBonePoseScratchArea>
{
	BoneTrackArray RotationScalePairs;
	BoneTrackArray TranslationPairs;
	BoneTrackArray AnimScaleRetargetingPairs;
	BoneTrackArray AnimRelativeRetargetingPairs;
	BoneTrackArray OrientAndScaleRetargetingPairs;
};

// @third party code - BEGIN ACLPlugin
#if ACL_ENABLE_UE4_DECOMP_STAT_LOGGING
static FCriticalSection s_lock;
#endif
// @third party code - END ACLPlugin

void DecompressPose(FCompactPose& OutPose, const FCompressedAnimSequence& CompressedData, const FAnimExtractContext& ExtractionContext, USkeleton* Skeleton, float SequenceLength, EAnimInterpolationType Interpolation, bool bIsBakedAdditive, FName RetargetSource, FName SourceName, const FRootMotionReset& RootMotionReset)
{
	const FBoneContainer& RequiredBones = OutPose.GetBoneContainer();
	const int32 NumTracks = CompressedData.CompressedTrackToSkeletonMapTable.Num();

	TArray<int32> const& SkeletonToPoseBoneIndexArray = RequiredBones.GetSkeletonToPoseBoneIndexArray();

	BoneTrackArray& RotationScalePairs = FGetBonePoseScratchArea::Get().RotationScalePairs;
	BoneTrackArray& TranslationPairs = FGetBonePoseScratchArea::Get().TranslationPairs;
	BoneTrackArray& AnimScaleRetargetingPairs = FGetBonePoseScratchArea::Get().AnimScaleRetargetingPairs;
	BoneTrackArray& AnimRelativeRetargetingPairs = FGetBonePoseScratchArea::Get().AnimRelativeRetargetingPairs;
	BoneTrackArray& OrientAndScaleRetargetingPairs = FGetBonePoseScratchArea::Get().OrientAndScaleRetargetingPairs;

	// build a list of desired bones
	RotationScalePairs.Reset();
	TranslationPairs.Reset();
	AnimScaleRetargetingPairs.Reset();
	AnimRelativeRetargetingPairs.Reset();
	OrientAndScaleRetargetingPairs.Reset();

	// Optimization: assuming first index is root bone. That should always be the case in Skeletons.
	checkSlow((SkeletonToPoseBoneIndexArray[0] == 0));
	// this is not guaranteed for AnimSequences though... If Root is not animated, Track will not exist.
	const bool bFirstTrackIsRootBone = (CompressedData.GetSkeletonIndexFromTrackIndex(0) == 0);

	{
		SCOPE_CYCLE_COUNTER(STAT_BuildAnimTrackPairs);

		// Handle root bone separately if it is track 0. so we start w/ Index 1.
		for (int32 TrackIndex = (bFirstTrackIsRootBone ? 1 : 0); TrackIndex < NumTracks; TrackIndex++)
		{
			const int32 SkeletonBoneIndex = CompressedData.GetSkeletonIndexFromTrackIndex(TrackIndex);
			// not sure it's safe to assume that SkeletonBoneIndex can never be INDEX_NONE
			if (SkeletonBoneIndex != INDEX_NONE)
			{
				const FCompactPoseBoneIndex BoneIndex = RequiredBones.GetCompactPoseIndexFromSkeletonIndex(SkeletonBoneIndex);
				//Nasty, we break our type safety, code in the lower levels should be adjusted for this
				const int32 CompactPoseBoneIndex = BoneIndex.GetInt();
				if (CompactPoseBoneIndex != INDEX_NONE)
				{
					RotationScalePairs.Add(BoneTrackPair(CompactPoseBoneIndex, TrackIndex));

					// Skip extracting translation component for EBoneTranslationRetargetingMode::Skeleton.
					switch (Skeleton->GetBoneTranslationRetargetingMode(SkeletonBoneIndex))
					{
					case EBoneTranslationRetargetingMode::Animation:
						TranslationPairs.Add(BoneTrackPair(CompactPoseBoneIndex, TrackIndex));
						break;
					case EBoneTranslationRetargetingMode::AnimationScaled:
						TranslationPairs.Add(BoneTrackPair(CompactPoseBoneIndex, TrackIndex));
						AnimScaleRetargetingPairs.Add(BoneTrackPair(CompactPoseBoneIndex, SkeletonBoneIndex));
						break;
					case EBoneTranslationRetargetingMode::AnimationRelative:
						TranslationPairs.Add(BoneTrackPair(CompactPoseBoneIndex, TrackIndex));

						// With baked additives, we can skip 'AnimationRelative' tracks, as the relative transform gets canceled out.
						// (A1 + Rel) - (A2 + Rel) = A1 - A2.
						if (!bIsBakedAdditive)
						{
							AnimRelativeRetargetingPairs.Add(BoneTrackPair(CompactPoseBoneIndex, SkeletonBoneIndex));
						}
						break;
					case EBoneTranslationRetargetingMode::OrientAndScale:
						TranslationPairs.Add(BoneTrackPair(CompactPoseBoneIndex, TrackIndex));

						// Additives remain additives, they're not retargeted.
						if (!bIsBakedAdditive)
						{
							OrientAndScaleRetargetingPairs.Add(BoneTrackPair(CompactPoseBoneIndex, SkeletonBoneIndex));
						}
						break;
					}
				}
			}
		}
	}

	{
		SCOPE_CYCLE_COUNTER(STAT_ExtractPoseFromAnimData);
		CSV_SCOPED_TIMING_STAT(Animation, ExtractPoseFromAnimData);
		CSV_CUSTOM_STAT(Animation, NumberOfExtractedAnimations, 1, ECsvCustomStatOp::Accumulate);

		FAnimSequenceDecompressionContext EvalDecompContext(SequenceLength, Interpolation, SourceName, CompressedData.CompressedDataStructure);
		EvalDecompContext.Seek(ExtractionContext.CurrentTime);

		// @third party code - BEGIN ACLPlugin
#if ACL_ENABLE_UE4_DECOMP_STAT_LOGGING
		const uint64 DecompressionCyclesStart = FPlatformTime::Cycles64();
#endif
		// @third party code - END ACLPlugin

		// Handle Root Bone separately
		if (bFirstTrackIsRootBone)
		{
			const int32 TrackIndex = 0;
			FCompactPoseBoneIndex RootBone(0);
			FTransform& RootAtom = OutPose[RootBone];

			AnimationFormat_GetBoneAtom(
				RootAtom,
				EvalDecompContext,
				TrackIndex);

			// @laurent - we should look into splitting rotation and translation tracks, so we don't have to process translation twice.
			FAnimationRuntime::RetargetBoneTransform(Skeleton, RetargetSource, RootAtom, 0, RootBone, RequiredBones, bIsBakedAdditive);
		}

		if (RotationScalePairs.Num() > 0)
		{
			// get the remaining bone atoms
			FTransformArray LocalBones;
			OutPose.MoveBonesTo(LocalBones);

			AnimationFormat_GetAnimationPose(
				LocalBones,
				RotationScalePairs,
				TranslationPairs,
				RotationScalePairs,
				EvalDecompContext);

			OutPose.MoveBonesFrom(MoveTemp(LocalBones));
		}

		// @third party code - BEGIN ACLPlugin
#if ACL_ENABLE_UE4_DECOMP_STAT_LOGGING
		const uint64 DecompressionCyclesEnd = FPlatformTime::Cycles64();

		const uint64 DecompressionElapsedCycles = DecompressionCyclesEnd - DecompressionCyclesStart;
		const double DecompressionElapsedUS = FPlatformTime::ToMilliseconds64(DecompressionElapsedCycles) * 1000.0;
		const double DecompressionElapsedSecs = FPlatformTime::ToSeconds64(DecompressionElapsedCycles);
		const double PoseSize = OutPose.GetNumBones() * ((4 + 3 + 3) * sizeof(float));	// Rot/trans/scale per bone
		const double DecompressionSpeedMBSec = (PoseSize / (1024.0 * 1024.0)) / DecompressionElapsedSecs;
		const FString CodecName = FAnimationUtils::GetAnimationKeyFormatString(CompressedData.CompressedDataStructure.KeyEncodingFormat);
		const FString SeqName = SourceName.ToString();

		// A lock is used to keep the log clean when multi-threaded decompression is used
		s_lock.Lock();
		UE_LOG(LogAnimation, Warning, TEXT("Decompression for [%s] at [%.3f sec] took [%.2f us @ %.2f MB/sec] with codec [%s]"), *SeqName, ExtractionContext.CurrentTime, DecompressionElapsedUS, DecompressionSpeedMBSec, *CodecName);
		UE_LOG(LogAnimation, Warning, TEXT("[[%s,%.3f,%.6f,%.6f,%s]]"), *SeqName, ExtractionContext.CurrentTime, DecompressionElapsedUS, DecompressionSpeedMBSec, *CodecName);
		s_lock.Unlock();
#endif
		// @third party code - END ACLPlugin
	}

	// Once pose has been extracted, snap root bone back to first frame if we are extracting root motion.
	if ((ExtractionContext.bExtractRootMotion && RootMotionReset.bEnableRootMotion) || RootMotionReset.bForceRootLock)
	{
		RootMotionReset.ResetRootBoneForRootMotion(OutPose[FCompactPoseBoneIndex(0)], RequiredBones);
	}

	// Anim Scale Retargeting
	int32 const NumBonesToScaleRetarget = AnimScaleRetargetingPairs.Num();
	if (NumBonesToScaleRetarget > 0)
	{
		TArray<FTransform> const& AuthoredOnRefSkeleton = Skeleton->GetRefLocalPoses(RetargetSource);

		for (const BoneTrackPair& BonePair : AnimScaleRetargetingPairs)
		{
			const FCompactPoseBoneIndex BoneIndex(BonePair.AtomIndex); //Nasty, we break our type safety, code in the lower levels should be adjusted for this
			int32 const& SkeletonBoneIndex = BonePair.TrackIndex;

			// @todo - precache that in FBoneContainer when we have SkeletonIndex->TrackIndex mapping. So we can just apply scale right away.
			float const SourceTranslationLength = AuthoredOnRefSkeleton[SkeletonBoneIndex].GetTranslation().Size();
			if (SourceTranslationLength > KINDA_SMALL_NUMBER)
			{
				float const TargetTranslationLength = RequiredBones.GetRefPoseTransform(BoneIndex).GetTranslation().Size();
				OutPose[BoneIndex].ScaleTranslation(TargetTranslationLength / SourceTranslationLength);
			}
		}
	}

	// Anim Relative Retargeting
	int32 const NumBonesToRelativeRetarget = AnimRelativeRetargetingPairs.Num();
	if (NumBonesToRelativeRetarget > 0)
	{
		TArray<FTransform> const& AuthoredOnRefSkeleton = Skeleton->GetRefLocalPoses(RetargetSource);

		for (const BoneTrackPair& BonePair : AnimRelativeRetargetingPairs)
		{
			const FCompactPoseBoneIndex BoneIndex(BonePair.AtomIndex); //Nasty, we break our type safety, code in the lower levels should be adjusted for this
			int32 const& SkeletonBoneIndex = BonePair.TrackIndex;

			const FTransform& RefPose = RequiredBones.GetRefPoseTransform(BoneIndex);

			// Apply the retargeting as if it were an additive difference between the current skeleton and the retarget skeleton. 
			OutPose[BoneIndex].SetRotation(OutPose[BoneIndex].GetRotation() * AuthoredOnRefSkeleton[SkeletonBoneIndex].GetRotation().Inverse() * RefPose.GetRotation());
			OutPose[BoneIndex].SetTranslation(OutPose[BoneIndex].GetTranslation() + (RefPose.GetTranslation() - AuthoredOnRefSkeleton[SkeletonBoneIndex].GetTranslation()));
			OutPose[BoneIndex].SetScale3D(OutPose[BoneIndex].GetScale3D() * (RefPose.GetScale3D() * AuthoredOnRefSkeleton[SkeletonBoneIndex].GetSafeScaleReciprocal(AuthoredOnRefSkeleton[SkeletonBoneIndex].GetScale3D())));
			OutPose[BoneIndex].NormalizeRotation();
		}
	}

	// Translation 'Orient and Scale' Translation Retargeting
	const int32 NumBonesToOrientAndScaleRetarget = OrientAndScaleRetargetingPairs.Num();
	if (NumBonesToOrientAndScaleRetarget > 0)
	{
		const FRetargetSourceCachedData& RetargetSourceCachedData = RequiredBones.GetRetargetSourceCachedData(RetargetSource);
		const TArray<FOrientAndScaleRetargetingCachedData>& OrientAndScaleDataArray = RetargetSourceCachedData.OrientAndScaleData;
		const TArray<int32>& CompactPoseIndexToOrientAndScaleIndex = RetargetSourceCachedData.CompactPoseIndexToOrientAndScaleIndex;

		// If we have any cached retargeting data.
		if ((OrientAndScaleDataArray.Num() > 0) && (CompactPoseIndexToOrientAndScaleIndex.Num() == RequiredBones.GetCompactPoseNumBones()))
		{
			for (int32 Index = 0; Index < NumBonesToOrientAndScaleRetarget; Index++)
			{
				const BoneTrackPair& BonePair = OrientAndScaleRetargetingPairs[Index];
				const FCompactPoseBoneIndex CompactPoseBoneIndex(BonePair.AtomIndex);
				const int32 OrientAndScaleIndex = CompactPoseIndexToOrientAndScaleIndex[CompactPoseBoneIndex.GetInt()];
				if (OrientAndScaleIndex != INDEX_NONE)
				{
					const FOrientAndScaleRetargetingCachedData& OrientAndScaleData = OrientAndScaleDataArray[OrientAndScaleIndex];
					FTransform& BoneTransform = OutPose[CompactPoseBoneIndex];
					const FVector AnimatedTranslation = BoneTransform.GetTranslation();

					// If Translation is not animated, we can just copy the TargetTranslation. No retargeting needs to be done.
					const FVector NewTranslation = (AnimatedTranslation - OrientAndScaleData.SourceTranslation).IsNearlyZero(BONE_TRANS_RT_ORIENT_AND_SCALE_PRECISION) ?
						OrientAndScaleData.TargetTranslation :
						OrientAndScaleData.TranslationDeltaOrient.RotateVector(AnimatedTranslation) * OrientAndScaleData.TranslationScale;

					BoneTransform.SetTranslation(NewTranslation);
				}
			}
		}
	}
}

