#pragma once

#define _USE_MATH_DEFINES
#include <math.h>

template<int MaxDelayLen>
class DelayLine
{
private:
	float dat[MaxDelayLen] = { 0 };
	float out = 0;

	// 平滑处理
	float currentDelay = 0;
	float targetDelay = 0;
	float delayVelocity = 0; // 用于线性平滑延迟时间本身

	int pos = 0;

	// 4点三阶 Hermite 插值：比线性插值显著减少高频混叠
	// 
	inline float ReadSampleHermite(float delay)
	{
		float readPos = (float)pos - delay;
		// 确保 readPos 在 [0, MaxDelayLen) 之间
		while (readPos < 0) readPos += MaxDelayLen;
		while (readPos >= MaxDelayLen) readPos -= MaxDelayLen;

		int i1 = (int)readPos;
		float f = readPos - i1;

		// 获取相邻的四个点进行计算
		int i0 = (i1 - 1 + MaxDelayLen) % MaxDelayLen;
		int i2 = (i1 + 1) % MaxDelayLen;
		int i3 = (i1 + 2) % MaxDelayLen;

		float y0 = dat[i0];
		float y1 = dat[i1];
		float y2 = dat[i2];
		float y3 = dat[i3];

		// Hermite 插值公式，兼顾计算速度与质量
		float c0 = y1;
		float c1 = 0.5f * (y2 - y0);
		float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
		float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);

		return ((c3 * f + c2) * f + c1) * f + c0;
	}
	inline float ReadSampleLinear(float delay)
	{
		float readPos = (float)pos - delay;
		// 确保 readPos 在 [0, MaxDelayLen) 之间
		while (readPos < 0) readPos += MaxDelayLen;
		while (readPos >= MaxDelayLen) readPos -= MaxDelayLen;
		int i1 = (int)readPos;
		int i2 = (i1 + 1) % MaxDelayLen;
		float f = readPos - i1;
		return dat[i1] * (1.0f - f) + dat[i2] * f;
	}
public:
	// 为了兼容你原本的 Chorus 逻辑，保留这个常数，但逻辑改为平滑步进
	constexpr static int GradientSamples = 10000;

	DelayLine()
	{
		std::fill(dat, dat + MaxDelayLen, 0.0f);
	}

	// 设置目标延迟并计算步进值（在 GradientSamples 周期内完成过渡）
	inline void SetDelayTime(float t)
	{
		targetDelay = t;
		delayVelocity = (targetDelay - currentDelay) / (float)GradientSamples;
	}

	inline float ReadSample()
	{
		return out;
	}

	inline void WriteSample(float val)
	{
		// 1. 写入当前采样
		dat[pos] = val;

		// 2. 更新平滑的延迟时间（代替之前的 crossfade 逻辑）
		// 这样可以保证读取指针的运动更加平滑，减少多普勒产生的畸变
		if (fabsf(targetDelay - currentDelay) > 0.0001f)
		{
			currentDelay += delayVelocity;
		}
		else
		{
			currentDelay = targetDelay;
		}

		// 3. 使用高阶插值读取
		//out = ReadSampleHermite(currentDelay);
		out = ReadSampleLinear(currentDelay);

		// 4. 更新指针
		if (++pos >= MaxDelayLen) pos = 0;
	}
};

class LMChorus
{
private:
	constexpr static int MaxDelaySamples = 4800; //~100ms
	constexpr static int MaxTapN = 6;
	constexpr static float sampleRate = 48000;
	DelayLine<MaxDelaySamples> dlyl[MaxTapN];
	DelayLine<MaxDelaySamples> dlyr[MaxTapN];
	int numTaps = 2;
	float delayTime = 0.1, depth = 0.5, rate = 0.5, spread = 1.0, mix = 1.0;

	float phase = 0.0f;
	float delaytl[MaxTapN] = { 0 };
	float delaytr[MaxTapN] = { 0 };
	int updateCounter = 0;
	constexpr static int updateInterval = DelayLine<MaxDelaySamples>::GradientSamples / 200; // 与 DelayLine 的 1.0/25.0 对应

	static float clamp(float val, float low, float high)
	{
		if (val < low) return low;
		if (val > high) return high;
		return val;
	}
public:
	void SetParameters(int numTaps, float delayTime, float depth, float rate, float spread, float mix)
	{
		this->numTaps = numTaps;
		this->delayTime = delayTime;
		this->depth = depth;
		this->rate = rate;
		this->spread = spread;
		this->mix = mix;
	}
	void ProcessBlock(const float* inl, const float* inr, float* outl, float* outr, int numSamples)
	{
		float freqHz = 0.0f + rate * 2.0f;
		float phaseInc = freqHz / sampleRate;

		float baseDelaySamples = delayTime * (4800 - 2400);
		float modDepthSamples = depth * 2400;

		float gainNormalizer = 1.0f / sqrtf((float)numTaps); // 能量补偿
		float spreadfix = spread * spread * spread * spread;

		float mixdry = cosf(mix * (float)M_PI * 0.5f);
		float mixwet = sinf(mix * (float)M_PI * 0.5f) * gainNormalizer;

		for (int i = 0; i < numSamples; ++i)
		{
			float dryL = inl[i];
			float dryR = inr[i];
			float wetL = 0;
			float wetR = 0;

			// 检查是否到了 DelayLine 更新 target 的时刻
			bool shouldUpdateMod = (updateCounter == 0);

			// 1. 更新相位
			phase += phaseInc;
			phase -= (int)phase;

			for (int t = 0; t < numTaps; ++t)
			{

				// 2. 性能优化：仅在更新窗口计算 sinf
				if (shouldUpdateMod)
				{
					float t1 = phase + (float)t / numTaps * (numTaps % 2 == 0 ? 2.0 / 3.0 : 1.0);//偶数 tap 时相位错开一些，避免齐拍
					float lfol = sinf((t1) * 2.0f * (float)M_PI) + 1.0;
					float lfor = sinf((t1 + spreadfix * 0.5) * 2.0f * (float)M_PI) + 1.0;

					// 计算目标延迟采样数
					float targetDelayl = baseDelaySamples * t / numTaps + lfol * modDepthSamples * 0.5;
					float targetDelayr = baseDelaySamples * t / numTaps + lfor * modDepthSamples * 0.5;
					delaytl[t] = clamp(targetDelayl, 1.0, (float)MaxDelaySamples - 1.0);
					delaytr[t] = clamp(targetDelayr, 1.0, (float)MaxDelaySamples - 1.0);
					dlyl[t].SetDelayTime(delaytl[t]);
					dlyr[t].SetDelayTime(delaytr[t]);
				}

				// 3. 写入并读取（DelayLine 内部会处理平滑过渡）
				// 左右声道交替相位或处理以增加宽度
				dlyl[t].WriteSample(dryL); // 注意：DelayLine的WriteSample第二个参数在你的实现中是 updateTime
				dlyr[t].WriteSample(dryR);

				float tapOutL = dlyl[t].ReadSample();
				float tapOutR = dlyr[t].ReadSample();

				wetL += tapOutL;
				wetR += tapOutR;
			}

			// 计数器逻辑
			if (++updateCounter >= updateInterval)
				updateCounter = 0;

			// 混合输出
			//outl[i] = dryL * mixdry + wetL * mixwet;
			//outr[i] = dryR * mixdry + wetR * mixwet;
			outl[i] = dryL + wetL * mixwet;
			outr[i] = dryR + wetR * mixwet;
		}
	}
};