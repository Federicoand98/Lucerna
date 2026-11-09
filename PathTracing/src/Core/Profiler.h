#pragma once

#ifndef CORE_PROFILER_H
#define CORE_PROFILER_H

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace Lucerna {

	// Il profiler (ADR 0023): uno storico dei giri del ciclo dell'app, con le attese della CPU e i
	// tempi delle passate GPU. Globale, perche' lo alimentano moduli che non si conoscono (Vulkan
	// misura le attese e le passate, l'Editor apre i frame); il pannello lo legge e basta.

	// Il tempo di una passata GPU, dal GpuProfiler. Depth 0 e' una passata, 1 una sua parte.
	struct GpuTiming {
		std::string Name;
		int Depth = 0;
		float Ms = 0.0f;
	};

	// Un giro del ciclo: quanto e' durato e dove e' andato il tempo.
	struct FrameRecord {
		float FrameMs = 0.0f;   // dall'inizio di questo giro all'inizio del prossimo
		float FenceMs = 0.0f;   // le tre chiamate in cui la CPU puo' fermarsi ad aspettare
		float AcquireMs = 0.0f;
		float PresentMs = 0.0f;
		std::vector<GpuTiming> Gpu; // le passate del frame, poi le build fatte in Immediate

		// Dove la CPU si ferma dipende dal driver: NVIDIA aspetta la GPU dentro il present, non
		// nella fence. Quindi l'attesa si divide per causa e non per chiamata: va alla GPU fino al
		// tempo in cui la GPU ha lavorato, e il resto e' attesa del display (vsync).
		float WaitMs() const { return FenceMs + AcquireMs + PresentMs; }
		float GpuWaitMs() const;
		float PresentWaitMs() const { return WaitMs() - GpuWaitMs(); }
		float CpuWorkMs() const;  // il resto del frame: il lavoro vero della CPU
		float GpuBusyMs() const;  // la somma delle passate di primo livello
		float GpuMs(std::string_view name) const; // 0 se la passata in questo frame non c'e'
	};

	struct Summary {
		float Min = 0.0f, Avg = 0.0f, P99 = 0.0f, Max = 0.0f;
	};
	Summary Summarize(std::vector<float> values);

	// Cosa limita il frame, dalle medie di una finestra di frame.
	enum class Bottleneck { None, Gpu, Cpu, Present };
	struct Diagnosis {
		Bottleneck Limit = Bottleneck::None;
		float FrameMs = 0.0f, CpuMs = 0.0f, GpuWaitMs = 0.0f, PresentWaitMs = 0.0f, GpuBusyMs = 0.0f;
		std::string TopPass; // la passata GPU piu' pesante, e quanto pesa
		float TopPassMs = 0.0f;
	};
	// Un quarto del frame passato ad aspettare il display e' vsync: il limite e' il display.
	// Altrimenti, se la CPU aspetta la GPU piu' di quanto lavora, il limite e' la GPU; se no la CPU.
	Diagnosis Diagnose(const std::vector<const FrameRecord*>& frames);

	class Profiler {
	public:
		static constexpr size_t Capacity = 1000;

		static Profiler& Get();

		// All'inizio di ogni giro del ciclo: chiude il giro prima, che aspetta ancora i tempi GPU
		void BeginFrame();
		void AddFenceWait(float ms) { Current().FenceMs += ms; }
		void AddAcquire(float ms) { Current().AcquireMs += ms; }
		void AddPresent(float ms) { Current().PresentMs += ms; }
		// Le passate del frame inviato nel giro prima: arrivano dopo la sua fence, e lo completano
		void AttachFrameGpu(std::vector<GpuTiming> timings);
		// Una build in Immediate: e' sincrona, quindi appartiene al giro in corso
		void AddImmediateGpu(GpuTiming timing) { Current().Gpu.push_back(std::move(timing)); }

		// I giri completi, dal piu' vecchio. In pausa non cresce, ma Latest si aggiorna.
		const std::deque<FrameRecord>& History() const { return m_History; }
		const FrameRecord& Latest() const { return m_Latest; }
		void SetPaused(bool paused) { m_Paused = paused; }
		bool Paused() const { return m_Paused; }
		void Reset(); // per i controlli: si riparte da zero

		static double NowMs();

	private:
		FrameRecord& Current();
		void Complete(FrameRecord record);

		std::deque<FrameRecord> m_History;
		FrameRecord m_Latest;
		FrameRecord m_Current, m_Pending;
		double m_CurrentStart = 0.0;
		bool m_HasCurrent = false, m_HasPending = false;
		bool m_Paused = false;
	};
}

#endif // CORE_PROFILER_H
