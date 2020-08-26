// Copyright 2018 Nicholas Frechette

#include "Animation/AnimEncodingRegistry.h"

FAnimEncodingRegistry::FAnimEncodingRegistry()
{
	for (int32 i = 0; i < AKF_MAX; ++i)
	{
		EncodingInterfaceMap[i] = nullptr;
	}
}

FAnimEncodingRegistry& FAnimEncodingRegistry::Get()
{
	static FAnimEncodingRegistry Instance;
	return Instance;
}
