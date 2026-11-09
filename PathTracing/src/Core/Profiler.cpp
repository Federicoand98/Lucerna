#include "Core/Profiler.h"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace Lucerna {

	float FrameRecord::CpuWorkMs() const {
		return std::max(0.0f, FrameMs - WaitMs());
	}

	float FrameRecord::GpuWaitMs() const {
		return std::min(WaitMs(), GpuBusyMs());
	}

	float FrameRecord::GpuBusyMs() const {
		float sum = 0.0f;
		for (const GpuTiming& timing : Gpu)
			if (timing.Depth == 0)
				sum += timing.Ms;
		return sum;
	}

	float FrameRecord::GpuMs(std::string_view name) const {
		float sum = 0.0f;
		for (const GpuTiming& timing : Gpu)
			if (timing.Name == name)
				sum += timing.Ms;
		return sum;
	}

	// p99 col nearest-rank: il valore sotto cui sta il 99% dei frame
	Summary Summarize(std::vector<float> values) {
		Summary s;
		if (values.empty())
			return s;
		std::sort(values.begin(), values.end());
		double sum = 0.0;
		for (float v : values)
			sum += v;
		const size_t rank = static_cast<size_t>(std::ceil(0.99 * values.size()));
		s.Min = values.front();
		s.Max = values.back();
		s.Avg = static_cast<float>(sum / values.size());
		s.P99 = values[std::max<size_t>(rank, 1) - 1];
		return s;
	}

	Diagnosis Diagnose(const std::vector<const FrameRecord*>& frames) {
		Diagnosis d;
		if (frames.empty())
			return d;

		std::vector<std::pair<std::string, float>> passes; // somma per passata di primo livello
		for (const FrameRecord* f : frames) {
			d.FrameMs += f->FrameMs;
			d.CpuMs += f->CpuWorkMs();
			d.GpuWaitMs += f->GpuWaitMs();
			d.PresentWaitMs += f->PresentWaitMs();
			d.GpuBusyMs += f->GpuBusyMs();
			for (const GpuTiming& timing : f->Gpu) {
				if (timing.Depth != 0)
					continue;
				auto found = std::find_if(passes.begin(), passes.end(), [&](const auto& p) { return p.first == timing.Name; });
				if (found == passes.end())
					passes.emplace_back(timing.Name, timing.Ms);
				else
					found->second += timing.Ms;
			}
		}
		const float n = static_cast<float>(frames.size());
		d.FrameMs /= n;
		d.CpuMs /= n;
		d.GpuWaitMs /= n;
		d.PresentWaitMs /= n;
		d.GpuBusyMs /= n;
		for (const auto& [name, total] : passes)
			if (total / n > d.TopPassMs) {
				d.TopPass = name;
				d.TopPassMs = total / n;
			}

		if (d.PresentWaitMs >= 0.25f * d.FrameMs)
			d.Limit = Bottleneck::Present;
		else if (d.GpuWaitMs >= d.CpuMs)
			d.Limit = Bottleneck::Gpu;
		else
			d.Limit = Bottleneck::Cpu;
		return d;
	}

	Profiler& Profiler::Get() {
		static Profiler profiler;
		return profiler;
	}

	double Profiler::NowMs() {
		using namespace std::chrono;
		return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
	}

	FrameRecord& Profiler::Current() {
		if (!m_HasCurrent) {
			m_Current = {};
			m_CurrentStart = NowMs();
			m_HasCurrent = true;
		}
		return m_Current;
	}

	void Profiler::BeginFrame() {
		const double now = NowMs();
		if (m_HasCurrent) {
			m_Current.FrameMs = static_cast<float>(now - m_CurrentStart);
			// un giro che non ha inviato nulla (finestra ridotta a icona) non avra' mai i tempi GPU
			if (m_HasPending)
				Complete(std::move(m_Pending));
			m_Pending = std::move(m_Current);
			m_HasPending = true;
		}
		m_Current = {};
		m_CurrentStart = now;
		m_HasCurrent = true;
	}

	void Profiler::AttachFrameGpu(std::vector<GpuTiming> timings) {
		if (!m_HasPending)
			return;
		// prima le passate del frame, poi le build in Immediate che il giro aveva gia' raccolto
		timings.insert(timings.end(), std::make_move_iterator(m_Pending.Gpu.begin()), std::make_move_iterator(m_Pending.Gpu.end()));
		m_Pending.Gpu = std::move(timings);
		Complete(std::move(m_Pending));
		m_HasPending = false;
	}

	void Profiler::Complete(FrameRecord record) {
		m_Latest = record;
		if (m_Paused)
			return;
		m_History.push_back(std::move(record));
		if (m_History.size() > Capacity)
			m_History.pop_front();
	}

	void Profiler::Reset() {
		*this = Profiler();
	}
}
