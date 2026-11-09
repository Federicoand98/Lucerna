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

	// p99 col nearest-rank: il valore sotto cui sta il 99% dei frame. nth_element e non sort:
	// il pannello ne calcola una trentina a ogni frame.
	Summary Summarize(std::vector<float> values) {
		Summary s;
		if (values.empty())
			return s;
		double sum = 0.0;
		s.Min = s.Max = values.front();
		for (float v : values) {
			sum += v;
			s.Min = std::min(s.Min, v);
			s.Max = std::max(s.Max, v);
		}
		s.Avg = static_cast<float>(sum / values.size());
		const size_t rank = std::max<size_t>(static_cast<size_t>(std::ceil(0.99 * values.size())), 1) - 1;
		std::nth_element(values.begin(), values.begin() + rank, values.end());
		s.P99 = values[rank];
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

	bool Profiler::BeginScope(std::string_view name) {
		if (std::this_thread::get_id() != m_MainThread)
			return false;
		FrameRecord& current = Current();
		if (current.Cpu.size() >= MaxScopes)
			return false;
		const int parent = m_OpenScopes.empty() ? -1 : static_cast<int>(current.Cpu[m_OpenScopes.back().Index].Id);
		std::string path = parent < 0 ? std::string(name) : m_ScopePaths[parent] + "/" + std::string(name);
		auto [found, added] = m_ScopeIds.try_emplace(path, static_cast<uint32_t>(m_ScopePaths.size()));
		if (added) {
			m_ScopePaths.push_back(std::move(path));
			m_ScopeParents.push_back(parent);
		}
		current.Cpu.push_back({ std::string(name), static_cast<int>(m_OpenScopes.size()), 0.0f, found->second });
		m_OpenScopes.push_back({ current.Cpu.size() - 1, NowMs() });
		return true;
	}

	void Profiler::EndScope() {
		if (m_OpenScopes.empty())
			return;
		const OpenScope open = m_OpenScopes.back();
		m_OpenScopes.pop_back();
		if (open.Index < m_Current.Cpu.size())
			m_Current.Cpu[open.Index].Ms = static_cast<float>(NowMs() - open.Start);
	}

	void Profiler::BeginFrame() {
		// uno scope non attraversa il confine di un giro: quelli rimasti aperti si chiudono qui
		while (!m_OpenScopes.empty())
			EndScope();
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
		const std::thread::id main = m_MainThread;
		*this = Profiler();
		m_MainThread = main;
	}
}
