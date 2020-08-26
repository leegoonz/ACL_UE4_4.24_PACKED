// Copyright 2018 Nicholas Frechette

#pragma once

#include "CoreMinimal.h"
#include "Containers/SortedMap.h"
#include "Animation/AnimSequence.h"
#include "AnimEncoding.h"

/*
* This simple registry singleton manages the mapping between the animation key format
* and the runtime decompression code. This allows plugins to register codecs with minimal engine changes.
*
* Note: This singleton is not thread-safe!
*/
class ENGINE_API FAnimEncodingRegistry
{
public:
	typedef void(*SetInterfaceLinksFun)(AnimationKeyFormat KeyFormat, AnimEncoding*& RotationCodec, AnimEncoding*& TranslationCodec, AnimEncoding*& ScaleCodec);

	static FAnimEncodingRegistry& Get();

	void RegisterEncoding(AnimationKeyFormat KeyFormat, SetInterfaceLinksFun SetInterfaceLinks)
	{
		check(SetInterfaceLinks != nullptr);
		check(EncodingInterfaceMap[KeyFormat] == nullptr || EncodingInterfaceMap[KeyFormat] == SetInterfaceLinks);
		EncodingInterfaceMap[KeyFormat] = SetInterfaceLinks;
	}

	void UnregisterEncoding(AnimationKeyFormat KeyFormat)
	{
		EncodingInterfaceMap[KeyFormat] = nullptr;
	}

	bool SetInterfaceLinks(AnimationKeyFormat KeyFormat, AnimEncoding*& RotationCodec, AnimEncoding*& TranslationCodec, AnimEncoding*& ScaleCodec) const
	{
		SetInterfaceLinksFun SetInterfaceLinks = EncodingInterfaceMap[KeyFormat];
		if (SetInterfaceLinks == nullptr)
		{
			return false;	// KeyFormat not associated with a codec
		}

		(*SetInterfaceLinks)(KeyFormat, RotationCodec, TranslationCodec, ScaleCodec);
		return true;
	}

private:
	FAnimEncodingRegistry();

	SetInterfaceLinksFun EncodingInterfaceMap[AKF_MAX];
};
