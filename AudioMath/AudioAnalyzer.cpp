#include "pch.h"
#include "AudioAnalyzer.h"
#include <DirectXMath.h>
#include "XDSP.h"
#include <exception>

using namespace DirectX;
using namespace XDSP;

namespace AudioMath
{
	CAudioAnalyzer::CAudioAnalyzer(size_t inputBufferSize) :
		_inputSampleRate(48000),
		_inputChannels(2),
		_fftLength(0),
		_stepFrames(0),
		_overlapFrames(0),
		_pWindow(nullptr),
		_pFftReal(nullptr),
		_pFftUnityTable(nullptr),
		_pFftBuffers(nullptr)
	{
		_spInputBuffer = std::make_shared<CAudioBuffer>(inputBufferSize);
	}


	CAudioAnalyzer::~CAudioAnalyzer()
	{
		FreeBuffers();
	}

	void CAudioAnalyzer::AllocateBuffers()
	{
		FreeBuffers();	// First free any existing memory allocations

		_pWindow = static_cast<XMVECTOR *>(_aligned_malloc(_stepFrames+1 * sizeof(float),16));
		_pFftReal = static_cast<XMVECTOR *>(_aligned_malloc(_fftLength * sizeof(XMVECTOR) * _inputChannels, 16));	// For real data allocate space for all channels
		_pFftUnityTable = static_cast<XMVECTOR *> (_aligned_malloc(2 * _fftLength * sizeof(XMVECTOR), 16));	// Complex values 
		_pFftBuffers = static_cast<XMVECTOR *>(_aligned_malloc(2 * _fftLength * sizeof(XMVECTOR), 16));	// Preallocate buffers for FFT calculation 2*length of Fft

		if (_pFftReal == nullptr || _pWindow == nullptr || _pFftUnityTable == nullptr || _pFftBuffers == nullptr)
			throw std::exception("Out of memory", E_OUTOFMEMORY);
	}

	void CAudioAnalyzer::FreeBuffers()
	{
		if (_pFftReal != nullptr)
			_aligned_free(_pFftReal);
		_pFftReal = nullptr;
		if (_pFftUnityTable != nullptr)
			_aligned_free(_pFftUnityTable);
		_pFftUnityTable = nullptr;
		if (_pFftBuffers != nullptr)
			_aligned_free(_pFftBuffers);
		_pFftBuffers = nullptr;
		if (_pWindow != nullptr)
			_aligned_free(_pWindow);
		_pWindow = nullptr;
	}
	void CAudioAnalyzer::ConfigureAnalyzer(UINT32 fftLength, UINT32 stepFrames, UINT32 overlapFrames)
	{
		using namespace DirectX;
		_fftLength = fftLength;
		_fFftScale = 2.0f / _fftLength;
		_stepFrames = stepFrames;
		_overlapFrames = overlapFrames;
		AllocateBuffers();

		_fftLengthLog2 = 1;
		while (1U << _fftLengthLog2 != _fftLength)
			_fftLengthLog2++;

		/*
		float pi = 3.14159f;
		for (size_t index = 0; index < _stepFrames; index++)
		{
			_pWindow[index] = a0 - a1 * cosf(2 * pi * index / (_stepFrames - 1)) +
				a2 * cosf(4 * pi * index / (_stepFrames - 1)) -
				a3 * cosf(6 * pi * index / (_stepFrames - 1));
		}*/

		// Initialize window, use Blackman-Nuttall window for low sidelobes
		float a0 = 0.3635819f, a1 = 0.4891775f, a2 = 0.1365995f, a3 = 0.0106411f;
		XMVECTOR vElementIndex = XMVectorSet(0, 1, 2, 3),vElementStep = XMVectorReplicate(4.0f);
		float fIndexScaler = 1.0f /(_stepFrames - 1.0f);
		for (size_t vIndex = 0; vIndex < _stepFrames >> 2; vIndex++,vElementIndex+=vElementStep)
		{
			XMVECTOR vCosArg = XMVectorScale(DirectX::g_XMTwoPi * vElementIndex, fIndexScaler);
			_pWindow[vIndex] = 
				XMVectorReplicate(a0) -
				XMVectorScale(XMVectorCos(vCosArg), a1) +
				XMVectorScale(XMVectorCos(XMVectorScale(vCosArg, 2)), a2) -
				XMVectorScale(XMVectorCos(XMVectorScale(vCosArg, 3)), a3);
		}
		FFTInitializeUnityTable(_pFftUnityTable, _fftLength);
		_spInputBuffer->Configure(_stepFrames, _overlapFrames);
	}

	bool CAudioAnalyzer::IsOutputAvailable()
	{
		return _spInputBuffer->IsDataAvailable();
	}

	void CAudioAnalyzer::AddInput(float * pData, size_t frameCount, long frameIndex)
	{
		if (frameIndex != -1 && _spInputBuffer->GetPosition() == -1)
			_spInputBuffer->SetPosition(frameIndex);
		_spInputBuffer->Add(pData, frameCount);
	}

	bool CAudioAnalyzer::Step(long *pPosition, float *pRMS, float *pPeak, DirectX::XMVECTOR *pSpectrum)
	{
		if (!IsOutputAvailable())
			return false;

		if (pPosition != nullptr)
			*pPosition = _spInputBuffer->GetPosition();

		_spInputBuffer->Step((float*)_pFftReal, _fftLength);

		size_t vStep = _fftLength >> 2;

		if (pRMS != nullptr || pPeak != nullptr)
		{
			size_t vFromFrame = _overlapFrames >> 2, vToFrame = (_stepFrames - _overlapFrames) >> 2;
			float rmsScaler = 1.0f / ((vToFrame - vFromFrame) << 2);
			
				for (size_t channelIndex = 0,vOffset = 0; channelIndex < _inputChannels; channelIndex++,vOffset+=vStep)
			{
				XMVECTOR vRMSSum = XMVectorZero();
				XMVECTOR vPeak = XMVectorZero();
				for (size_t vIndex = vFromFrame + vOffset; vIndex < vToFrame + vOffset; vIndex++)
				{
					XMVECTOR vValue = _pFftReal[vIndex];
					vRMSSum +=  vValue * vValue;
					vPeak = XMVectorMax(vPeak, XMVectorAbs(vValue));
				}
				if (pRMS != nullptr)
				{
					XMVECTOR vRMS = XMVectorSqrt(XMVectorScale(XMVectorSum(vRMSSum), rmsScaler));
					pRMS[channelIndex] = vRMS.m128_f32[0];
				}
				if (pPeak != nullptr)
				{
					float peakValue = max(max(max(vPeak.m128_f32[0], vPeak.m128_f32[1]), vPeak.m128_f32[2]), vPeak.m128_f32[3]);
					pPeak[channelIndex] = peakValue;
				}
			}
		}
		if (pSpectrum != nullptr)
		{
			// First window the data
			for (size_t vIndex = 0; vIndex < _stepFrames << 2; vIndex++)
			{
				for (size_t channelIndex = 0,vElementIndex = vIndex; channelIndex < _inputChannels; channelIndex++, vElementIndex+=vStep)
				{
					_pFftReal[vElementIndex] *= _pWindow[vIndex];
				}
			}
			for (size_t channelIndex = 0; channelIndex < _inputChannels; channelIndex++)
			{
				XMVECTOR *pData = _pFftReal + channelIndex * vStep;
				XMVECTOR *pOutput = pSpectrum + channelIndex * (vStep >> 1);
				for (size_t vIndex = 0; vIndex < _stepFrames << 2; vIndex++)
				{
					pData[vIndex] *= _pWindow[vIndex];
				}

				XMVECTOR *pImag = _pFftBuffers;
				XMVECTOR *pRealUnswizzled = _pFftBuffers + vStep;

				memset(pImag, 0, sizeof(float)*_fftLength);	// Imaginary values are 0 for input

				XDSP::FFT(pData, pImag, _pFftUnityTable, _fftLength);
				XDSP::FFTUnswizzle(pRealUnswizzled, pData, _fftLengthLog2);
				XDSP::FFTUnswizzle(pData, pImag, _fftLengthLog2); // Use input data for temporary buffer for reordered imaginary data

				// Calculate abs value first half of FFT output and copy to output
				for (size_t vIndex = 0; vIndex < vStep>>1; vIndex++)	// vector length is 4 times shorter, copy only positive frequency values
				{
					XMVECTOR vRR = XMVectorMultiply(pRealUnswizzled[vIndex], pRealUnswizzled[vIndex]);
					XMVECTOR vII = XMVectorMultiply(pData[vIndex], pData[vIndex]);	// pData is used as buffer for reordered imaginary values
					XMVECTOR vRRplusvII = XMVectorAdd(vRR, vII);
					XMVECTOR vAbs = XMVectorSqrtEst(vRRplusvII);
					XMVECTOR vScaledAbs = XMVectorScale(vAbs, _fFftScale);
					pOutput[vIndex] = vScaledAbs;
				}
			}
		}
		return true;
	}

	void CAudioAnalyzer::Flush()
	{
		_spInputBuffer->Flush();
	}

}

