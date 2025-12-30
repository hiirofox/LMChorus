#pragma once

#define _USE_MATH_DEFINES
#include <math.h>

template<int MaxDelayLen>
class DelayLine
{
private:
	float dat[MaxDelayLen] = { 0 };
	float out = 0;
	int time1 = 0, time2 = 0, updateTime = 0;
	float gradient = 0;
	float dt = 1.0 / 200.0;//更新一次时间，其间会平滑过渡。对于chorus来说已经足够了
	int pos = 0;
public:
	DelayLine()
	{
		memset(dat, 0, sizeof(float) * MaxDelayLen);
	}
	void SetGradientRate(float rate)
	{
		dt = rate;
	}
	inline void SetDelayTime(float t)
	{
		updateTime = t;
	}
	int GetMaxDelay()
	{
		return MaxDelayLen;
	}
	inline float ReadSample()
	{
		return out;
	}
	inline void WriteSample(float val, int delay)
	{
		dat[pos] = val;

		int index1 = (MaxDelayLen + pos - time1) % MaxDelayLen;
		int index2 = (MaxDelayLen + pos - time2) % MaxDelayLen;
		out = dat[index1] + (dat[index2] - dat[index1]) * gradient;
		gradient += dt;
		if (gradient >= 1.0)
		{
			gradient -= 1.0;
			time1 = time2;
			time2 = updateTime;
			updateTime = delay;
		}

		pos = (pos + 1) % MaxDelayLen;
	}
};

class LMChorus
{
private:
	constexpr static int MaxDelaySamples = 4800; //~100ms
	constexpr static int MaxTapN = 8;
	constexpr static float sampleRate = 48000;
	DelayLine<MaxDelaySamples> dlyl[MaxTapN];
	DelayLine<MaxDelaySamples> dlyr[MaxTapN];
	int numTaps = 2;
	float delayTime = 0.1, depth = 0.5, rate = 0.5, spread = 1.0, mix = 1.0;

	float phase = 0.0f;
	int delayIntl[MaxTapN] = { 0 };
	int delayIntr[MaxTapN] = { 0 };
	int updateCounter = 0;
	const int updateInterval = 200.0; // 与 DelayLine 的 1.0/25.0 对应

	template<typename T>
	static T clamp(T val, T low, T high)
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
		// 频率映射：0.1Hz - 5.0Hz
		float freqHz = 0.0f + rate * 2.0f;
		float phaseInc = freqHz / sampleRate;

		// 延迟映射基础：中心延迟约 10ms - 50ms
		float baseDelaySamples = delayTime * (4800 - 100) + 100.0;
		// 调制深度映射：0 - 20ms 的波动范围
		float modDepthSamples = depth * 10.0f * (sampleRate / 1000.0f);

		float gainNormalizer = 1.0f / sqrtf((float)numTaps); // 能量补偿
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
					float lfol = sinf((phase + (float)t / numTaps) * 2.0f * (float)M_PI);
					float lfor = sinf((phase + (float)t / numTaps + spread * 0.5) * 2.0f * (float)M_PI);

					// 计算目标延迟采样数
					float targetDelayl = baseDelaySamples / (t + 1) + lfol * modDepthSamples;
					float targetDelayr = baseDelaySamples / (t + 1) + lfor * modDepthSamples;
					delayIntl[t] = clamp((int)targetDelayl, 1, MaxDelaySamples - 1);
					delayIntr[t] = clamp((int)targetDelayr, 1, MaxDelaySamples - 1);

				}

				// 3. 写入并读取（DelayLine 内部会处理平滑过渡）
				// 左右声道交替相位或处理以增加宽度
				dlyl[t].WriteSample(dryL, delayIntl[t]); // 注意：DelayLine的WriteSample第二个参数在你的实现中是 updateTime
				dlyr[t].WriteSample(dryR, delayIntr[t]);

				float tapOutL = dlyl[t].ReadSample();
				float tapOutR = dlyr[t].ReadSample();

				// 简单的立体声分布：偶数 Tap 偏左，奇数 Tap 偏右
				if (t % 2 == 0) {
					wetL += tapOutL;
					wetR += tapOutR * 0.2f; // 少量交叉
				}
				else {
					wetL += tapOutL * 0.2f;
					wetR += tapOutR;
				}
			}

			// 计数器逻辑
			if (++updateCounter >= updateInterval)
				updateCounter = 0;

			// 混合输出
			outl[i] = dryL * (1.0f - mix) + (wetL * gainNormalizer) * mix;
			outr[i] = dryR * (1.0f - mix) + (wetR * gainNormalizer) * mix;
		}
	}
};