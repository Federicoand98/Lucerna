#pragma once

#ifndef CORE_PROFILER_H
#define CORE_PROFILER_H

#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <deque>
#include <string>
#include <string_view>
#include <thread>
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

	// Uno scope CPU (PROFILE_SCOPE), nell'ordine in cui si apre: la profondita' dice chi e' dentro chi.
	struct CpuTiming {
		std::string Name;
		int Depth = 0;
		float Ms = 0.0f;
		uint32_t Id = 0; // il percorso dalla radice ("Renderer/RenderScene.Sync"), come numero: Profiler::ScopePath
	};

	// Un giro del ciclo: quanto e' durato e dove e' andato il tempo.
	struct FrameRecord {
		float FrameMs = 0.0f;   // dall'inizio di questo giro all'inizio del prossimo
		float FenceMs = 0.0f;   // le tre chiamate in cui la CPU puo' fermarsi ad aspettare
		float AcquireMs = 0.0f;
		float PresentMs = 0.0f;
		std::vector<GpuTiming> Gpu; // le passate del frame, poi le build fatte in Immediate
		std::vector<CpuTiming> Cpu; // gli scope del main thread, in preordine

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

	// Una fase di un caricamento: uno scope CPU aperto mentre la cattura era aperta. StartMs e'
	// dall'inizio del caricamento; Depth e' relativa a dove il caricamento e' cominciato.
	struct LoadPhase {
		std::string Name;
		int Depth = 0;
		float StartMs = 0.0f;
		float Ms = 0.0f;
	};

	// Un caricamento, da quando si apre il file al primo frame presentato con la scena nuova: in
	// mezzo ci sono frame interi, perche' texture e strutture di accelerazione si fanno nel Sync.
	struct LoadCapture {
		std::string Label;  // il file aperto
		float TotalMs = 0.0f;
		std::vector<LoadPhase> Phases; // in preordine; solo quelle da almeno MinPhaseMs
	};

	// La RAM del processo: residente adesso, e il massimo da quando e' partito. Zero dove non si sa.
	struct ProcessMemoryInfo {
		size_t Resident = 0, Peak = 0;
	};
	ProcessMemoryInfo ProcessMemory();

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

		// Gli scope CPU, attraverso PROFILE_SCOPE. Solo il thread che ha creato il profiler (il
		// main thread) registra: Begin ritorna false altrove, o oltre MaxScopes in un giro, e
		// allora End non va chiamata.
		// I caricamenti (ADR 0023): l'Application apre la cattura dove comincia a caricare, il
		// Renderer dice quando ha applicato la scena nuova, e il primo present dopo la chiude.
		// Una cattura nuova sostituisce quella aperta; un caricamento fallito la annulla.
		static constexpr float MinPhaseMs = 1.0f; // le fasi piu' brevi sono il resto dei frame
		void BeginLoad(std::string label);
		void CancelLoad() { m_Loading = false; }
		void LoadSynced() { m_LoadSynced = m_Loading; }
		bool FramePresented(); // true quando ha appena chiuso una cattura
		bool Loading() const { return m_Loading; }
		const LoadCapture& LastLoad() const { return m_LastLoad; }

		static constexpr size_t MaxScopes = 512;
		bool BeginScope(std::string_view name);
		void EndScope();
		// Ogni percorso di scope distinto ha un id, assegnato la prima volta che compare: chi legge
		// lo storico indicizza per id invece di confrontare stringhe a ogni frame.
		size_t ScopeCount() const { return m_ScopePaths.size(); }
		const std::string& ScopePath(uint32_t id) const { return m_ScopePaths[id]; }
		int ScopeParent(uint32_t id) const { return m_ScopeParents[id]; } // -1 = scope di primo livello

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

		struct OpenScope {
			size_t Index;
			double Start;
			size_t LoadIndex; // la sua fase nella cattura aperta, o SIZE_MAX
		};

		std::thread::id m_MainThread = std::this_thread::get_id();
		std::vector<OpenScope> m_OpenScopes;
		std::vector<std::string> m_ScopePaths;
		std::vector<int> m_ScopeParents;
		std::unordered_map<std::string, uint32_t> m_ScopeIds;
		std::deque<FrameRecord> m_History;
		FrameRecord m_Latest;
		FrameRecord m_Current, m_Pending;
		double m_CurrentStart = 0.0;
		bool m_HasCurrent = false, m_HasPending = false;
		bool m_Paused = false;

		LoadCapture m_Load, m_LastLoad;
		double m_LoadStart = 0.0;
		int m_LoadOpenPhases = 0; // le fasi aperte adesso: la profondita' della prossima
		bool m_Loading = false, m_LoadSynced = false;
	};

	// Misura il blocco in cui sta: PROFILE_SCOPE("RenderScene.Sync");
	class CpuScope {
	public:
		explicit CpuScope(std::string_view name) : m_Active(Profiler::Get().BeginScope(name)) {}
		~CpuScope() { if (m_Active) Profiler::Get().EndScope(); }
		CpuScope(const CpuScope&) = delete;
		CpuScope& operator=(const CpuScope&) = delete;
	private:
		bool m_Active;
	};
}

#define LUCERNA_PROFILE_CONCAT2(a, b) a##b
#define LUCERNA_PROFILE_CONCAT(a, b) LUCERNA_PROFILE_CONCAT2(a, b)
#define PROFILE_SCOPE(name) ::Lucerna::CpuScope LUCERNA_PROFILE_CONCAT(profileScope, __LINE__)(name)

#endif // CORE_PROFILER_H
