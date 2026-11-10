#include "Core/Profiler.h"
#include "Core/Parallel.h"

#include <algorithm>
#include <fstream>
#include <sstream>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <psapi.h> // GetProcessMemoryInfo e' in kernel32 (K32GetProcessMemoryInfo) da Windows 7
#endif
#include <chrono>
#include <cstdint>
#include <cmath>

namespace Lucerna {

	ProcessMemoryInfo ProcessMemory() {
		ProcessMemoryInfo info;
#ifdef _WIN32
		PROCESS_MEMORY_COUNTERS counters{};
		if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters))) {
			info.Resident = counters.WorkingSetSize;
			info.Peak = counters.PeakWorkingSetSize;
		}
#else
		// VmRSS e VmHWM (il picco) in kB
		std::ifstream status("/proc/self/status");
		std::string line;
		while (std::getline(status, line)) {
			std::istringstream fields(line);
			std::string key;
			size_t kb = 0;
			fields >> key >> kb;
			if (key == "VmRSS:")
				info.Resident = kb * 1024;
			else if (key == "VmHWM:")
				info.Peak = kb * 1024;
		}
#endif
		return info;
	}

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
		// Anche il main thread lavora dentro un ParallelFor: lo scope registrerebbe solo gli indici
		// che gli capitano, cioe' un numero che dipende da come si dividono. Dentro non conta nessuno.
		if (std::this_thread::get_id() != m_MainThread || InsideParallelFor())
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
		const double now = NowMs();
		size_t loadIndex = SIZE_MAX;
		if (m_Loading) {
			// la profondita' conta solo gli scope aperti che fanno parte della cattura: uno scope
			// aperto prima (la UI da cui si e' scelto il file) non e' una fase
			loadIndex = m_Load.Phases.size();
			m_Load.Phases.push_back({ std::string(name), m_LoadOpenPhases++, static_cast<float>(now - m_LoadStart), -1.0f });
		}
		m_OpenScopes.push_back({ current.Cpu.size() - 1, now, loadIndex });
		return true;
	}

	void Profiler::EndScope() {
		if (m_OpenScopes.empty())
			return;
		const OpenScope open = m_OpenScopes.back();
		m_OpenScopes.pop_back();
		const float ms = static_cast<float>(NowMs() - open.Start);
		if (open.Index < m_Current.Cpu.size())
			m_Current.Cpu[open.Index].Ms = ms;
		if (m_Loading && open.LoadIndex < m_Load.Phases.size()) {
			m_Load.Phases[open.LoadIndex].Ms = ms;
			m_LoadOpenPhases--;
		}
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

	void Profiler::AttachRays(uint32_t rays, uint32_t pixels) {
		m_Latest.Rays = rays;
		m_Latest.Pixels = pixels;
		if (!m_Paused && !m_History.empty()) {
			m_History.back().Rays = rays;
			m_History.back().Pixels = pixels;
		}
	}

	void Profiler::Complete(FrameRecord record) {
		m_Latest = record;
		if (m_Paused)
			return;
		m_History.push_back(std::move(record));
		if (m_History.size() > Capacity)
			m_History.pop_front();
	}

	void Profiler::BeginLoad(std::string label) {
		m_Load = {};
		m_Load.Label = std::move(label);
		m_LoadStart = NowMs();
		m_LoadOpenPhases = 0;
		// gli scope gia' aperti di una cattura sostituita non sono fasi di questa
		for (OpenScope& open : m_OpenScopes)
			open.LoadIndex = SIZE_MAX;
		m_Loading = true;
		m_LoadSynced = false;
	}

	bool Profiler::FramePresented() {
		if (!m_Loading || !m_LoadSynced)
			return false;
		m_Loading = false;
		m_Load.TotalMs = static_cast<float>(NowMs() - m_LoadStart);

		// Restano le fasi chiuse e lunghe: quelle brevi sono il resto dei frame in mezzo (la UI,
		// gli eventi). Una figlia non dura piu' della madre, quindi l'albero resta intero.
		std::vector<LoadPhase> kept;
		float lastEnd = 0.0f;
		for (LoadPhase& phase : m_Load.Phases)
			if (phase.Ms >= MinPhaseMs) {
				if (phase.Depth == 0)
					lastEnd = std::max(lastEnd, phase.StartMs + phase.Ms);
				kept.push_back(std::move(phase));
			}
		// quello che resta fino al present: il primo frame con la scena nuova
		if (m_Load.TotalMs - lastEnd >= MinPhaseMs)
			kept.push_back({ "First frame", 0, lastEnd, m_Load.TotalMs - lastEnd });
		m_Load.Phases = std::move(kept);
		m_LastLoad = std::move(m_Load);
		m_Load = {};
		return true;
	}

	void Profiler::Reset() {
		const std::thread::id main = m_MainThread;
		*this = Profiler();
		m_MainThread = main;
	}
}
