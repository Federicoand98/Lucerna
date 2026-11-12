// Il pannello Profiler (issue #31, ADR 0023): la diagnosi del frame, il grafico dei tempi e le
// passate GPU. Legge il Profiler globale; l'unica cosa che ci scrive e' la pausa.

#include "Editor/Application.h"
#include "Editor/Icons.h"
#include "Editor/Widgets.h"
#include "Core/Profiler.h"
#include <nlohmann/json.hpp>

#include "imgui_internal.h"

#include <fstream>
#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Lucerna {

	namespace {
		constexpr const char* PROFILER = "Profiler";
		constexpr const char* VIEWPORT = "Viewport";
		constexpr double MIN_SPAN = 20.0; // lo zoom orizzontale non va sotto questi frame

		ImU32 Col(const ImVec4& color) { return ImGui::GetColorU32(color); }

		// I colori delle passate nel grafico e nella tabella, come nel mockup
		ImU32 PassColor(const std::string& name) {
			if (name == "Path tracer") return Col(Theme::Accent);
			if (name == "Denoiser") return Col(Theme::IconSky);
			if (name == "Tonemap") return Col(Theme::Rgb(0xc3a6ff));
			if (name == "ImGui") return Col(Theme::Rgb(0x6fd6b8));
			if (name.find("build") != std::string::npos) return Col(Theme::Rgb(0xff7f6b));
			return Col(Theme::Rgb(0xd9d9e0));
		}
		const ImVec4 CPU_WORK = Theme::Rgb(0x6fd6b8);
		const ImVec4 WAIT_GPU = Theme::Accent;
		const ImVec4 WAIT_PRESENT = Theme::IconSky;
		const ImVec4 GHOST = Theme::Rgb(0x34343b);

		// Gli ultimi `count` frame dello storico, dal piu' vecchio
		std::vector<const FrameRecord*> LastFrames(size_t count) {
			const std::deque<FrameRecord>& history = Profiler::Get().History();
			std::vector<const FrameRecord*> frames;
			for (size_t i = history.size() > count ? history.size() - count : 0; i < history.size(); i++)
				frames.push_back(&history[i]);
			return frames;
		}

		// Una passata GPU nella tabella: per nome e profondita' (le parti del denoiser sono figlie)
		struct PassKey {
			std::string Name;
			int Depth = 0;
			bool operator==(const PassKey& other) const { return Name == other.Name && Depth == other.Depth; }
		};

		float PassMs(const FrameRecord& frame, const PassKey& key) {
			float sum = 0.0f;
			for (const GpuTiming& timing : frame.Gpu)
				if (timing.Depth == key.Depth && timing.Name == key.Name)
					sum += timing.Ms;
			return sum;
		}

		// L'ordine del frame piu' recente, poi le passate viste solo prima (una build dei BLAS)
		std::vector<PassKey> PassKeys(const std::vector<const FrameRecord*>& frames) {
			std::vector<PassKey> keys;
			for (auto it = frames.rbegin(); it != frames.rend(); ++it)
				for (const GpuTiming& timing : (*it)->Gpu) {
					PassKey key{ timing.Name, timing.Depth };
					if (std::find(keys.begin(), keys.end(), key) == keys.end())
						keys.push_back(key);
				}
			return keys;
		}

		float DisplayRefreshHz() {
			GLFWmonitor* monitor = glfwGetPrimaryMonitor();
			const GLFWvidmode* mode = monitor ? glfwGetVideoMode(monitor) : nullptr;
			return mode && mode->refreshRate > 0 ? static_cast<float>(mode->refreshRate) : 60.0f;
		}

		// 1, 2 o 5 per una potenza di dieci, cosi' che ci stiano circa quattro righe
		float NiceStep(float range) {
			const float raw = range / 4.0f;
			const float power = std::pow(10.0f, std::floor(std::log10(raw)));
			for (float step : { 1.0f, 2.0f, 5.0f, 10.0f })
				if (raw <= step * power)
					return step * power;
			return 10.0f * power;
		}

		// Una riga della tabella CPU: il frame e le sue attese (Id < 0), o uno scope col suo id
		struct CpuRow {
			std::string Name;
			int Depth = 0;
			int Id = -1;
			std::string Key; // per ricordare le righe chiuse: il percorso, o il nome di una riga fissa
		};
		enum FixedRow { RowFrame = -2, RowFence = -3, RowAcquire = -4, RowPresent = -5, RowUntracked = -6 };

		// Il frame, le attese, gli scope nell'ordine del frame piu' recente (poi quelli visti solo
		// prima, dopo i fratelli), il present e il tempo che nessuno scope copre
		std::vector<CpuRow> CpuRows(const std::vector<const FrameRecord*>& frames) {
			const Profiler& profiler = Profiler::Get();
			auto descends = [&](int id, int ancestor) {
				for (; id >= 0; id = profiler.ScopeParent(id))
					if (id == ancestor)
						return true;
				return false;
			};
			std::vector<CpuRow> scopes;
			std::vector<bool> known(profiler.ScopeCount(), false);
			for (auto it = frames.rbegin(); it != frames.rend(); ++it)
				for (const CpuTiming& scope : (*it)->Cpu) {
					if (known[scope.Id])
						continue;
					known[scope.Id] = true;
					const int parent = profiler.ScopeParent(scope.Id);
					auto at = scopes.end();
					if (parent >= 0)
						for (auto r = scopes.begin(); r != scopes.end(); ++r)
							if (descends(r->Id, parent))
								at = r + 1;
					scopes.insert(at, { profiler.ScopeName(scope.Id), scope.Depth + 1, static_cast<int>(scope.Id), profiler.ScopePath(scope.Id) });
				}
			std::vector<CpuRow> rows = { { "Frame", 0, RowFrame, "#frame" }, { "Fence wait", 1, RowFence, "#fence" }, { "Acquire image", 1, RowAcquire, "#acquire" } };
			rows.insert(rows.end(), scopes.begin(), scopes.end());
			rows.push_back({ "Present", 1, RowPresent, "#present" });
			rows.push_back({ "Untracked", 1, RowUntracked, "#untracked" });
			return rows;
		}

		// I millisecondi di ogni riga in ogni frame: series[riga][frame]
		std::vector<std::vector<float>> CpuSeries(const std::vector<CpuRow>& rows, const std::vector<const FrameRecord*>& frames) {
			std::vector<int> rowOfScope(Profiler::Get().ScopeCount(), -1);
			int fixed[7] = {};
			for (size_t r = 0; r < rows.size(); r++) {
				if (rows[r].Id >= 0)
					rowOfScope[rows[r].Id] = static_cast<int>(r);
				else
					fixed[-rows[r].Id] = static_cast<int>(r);
			}
			std::vector<std::vector<float>> series(rows.size(), std::vector<float>(frames.size(), 0.0f));
			for (size_t f = 0; f < frames.size(); f++) {
				const FrameRecord& frame = *frames[f];
				series[fixed[-RowFrame]][f] = frame.FrameMs;
				series[fixed[-RowFence]][f] = frame.FenceMs;
				series[fixed[-RowAcquire]][f] = frame.AcquireMs;
				series[fixed[-RowPresent]][f] = frame.PresentMs;
				float tracked = frame.WaitMs();
				for (const CpuTiming& scope : frame.Cpu) {
					// un frame fissato fuori dal range puo' avere scope che le righe non conoscono
					if (rowOfScope[scope.Id] >= 0)
						series[rowOfScope[scope.Id]][f] += scope.Ms;
					if (scope.Depth == 0)
						tracked += scope.Ms;
				}
				series[fixed[-RowUntracked]][f] = std::max(0.0f, frame.FrameMs - tracked);
			}
			return series;
		}

		std::string Percent(float part, float whole) {
			return std::to_string(static_cast<int>(std::lround(whole > 0.0f ? 100.0f * part / whole : 0.0f))) + "%";
		}

		// L'etichetta e il dettaglio della diagnosi; warn quando il limite non e' la GPU
		std::pair<std::string, std::string> DiagnosisText(const Diagnosis& d, bool& warn) {
			warn = d.Limit == Bottleneck::Cpu || d.Limit == Bottleneck::Present;
			switch (d.Limit) {
			case Bottleneck::Gpu: {
				std::string pass = d.TopPass;
				if (!pass.empty())
					pass[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(pass[0])));
				return { "GPU-bound", pass + " " + Percent(d.TopPassMs, d.FrameMs) + " of the frame" };
			}
			case Bottleneck::Present:
				return { "Present-bound", "vsync at " + std::to_string(static_cast<int>(std::lround(1000.0f / d.FrameMs))) +
				         " Hz \xc2\xb7 GPU busy " + Percent(d.GpuBusyMs, d.FrameMs) };
			case Bottleneck::Cpu:
				return { "CPU-bound", "CPU work " + Percent(d.CpuMs, d.FrameMs) + " of the frame" };
			default:
				return { "Measuring", "collecting frames" };
			}
		}

		// Un quadratino colorato in linea col testo, come nella legenda del mockup
		void Swatch(ImU32 color) {
			const float size = ImGui::GetFontSize() * 0.62f;
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float y = p.y + (ImGui::GetTextLineHeight() - size) * 0.5f;
			ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(p.x, y), ImVec2(p.x + size, y + size), color, 2.0f * UI::Scale());
			ImGui::Dummy(ImVec2(size, ImGui::GetTextLineHeight()));
			ImGui::SameLine(0.0f, 6.0f * UI::Scale());
		}
	}

	namespace {
		// Le chiavi del riferimento A/B: una passata GPU per nome e profondita', una riga CPU
		// per il suo percorso
		std::string GpuKey(const PassKey& key) { return "gpu/" + key.Name + "@" + std::to_string(key.Depth); }
		std::string CpuKey(const CpuRow& row) { return "cpu/" + row.Key; }

		struct StatRow {
			std::string Key, Name;
			int Depth = 0;
			Summary Sum;
		};

		// Tutto quello che il pannello mostra di una finestra di frame, in una forma che si puo'
		// salvare come riferimento, copiare o scrivere in JSON (issue #37)
		struct ProfileStats {
			size_t Frames = 0;
			Diagnosis Diag;
			std::vector<StatRow> Gpu, Cpu;
			std::vector<PassKey> Keys; // le passate di Gpu, nello stesso ordine
			std::vector<CpuRow> Rows;  // le righe di Cpu, nello stesso ordine
			double Rays = 0.0, Pixels = 0.0, PathTracerMs = 0.0;
			size_t CountedFrames = 0;
			bool HasThroughput() const { return CountedFrames > 0 && PathTracerMs > 0.0; }
			double MraysPerSecond() const { return Rays / PathTracerMs / 1000.0; }
			double RaysPerFrame() const { return Rays / CountedFrames; }
			double RaysPerPixel() const { return Rays / Pixels; }
		};

		ProfileStats ComputeStats(const std::vector<const FrameRecord*>& frames) {
			ProfileStats stats;
			stats.Frames = frames.size();
			stats.Diag = Diagnose(frames);
			stats.Keys = PassKeys(frames);
			for (const PassKey& key : stats.Keys) {
				std::vector<float> values;
				for (const FrameRecord* f : frames)
					values.push_back(PassMs(*f, key));
				stats.Gpu.push_back({ GpuKey(key), key.Name, key.Depth, Summarize(std::move(values)) });
			}
			stats.Rows = CpuRows(frames);
			std::vector<std::vector<float>> series = CpuSeries(stats.Rows, frames);
			for (size_t r = 0; r < stats.Rows.size(); r++)
				stats.Cpu.push_back({ CpuKey(stats.Rows[r]), stats.Rows[r].Name, stats.Rows[r].Depth, Summarize(std::move(series[r])) });
			for (const FrameRecord* f : frames)
				if (f->Rays > 0) {
					stats.Rays += f->Rays;
					stats.Pixels += f->Pixels;
					stats.PathTracerMs += f->GpuMs("Path tracer");
					stats.CountedFrames++;
				}
			return stats;
		}

		// La differenza col riferimento: arancio se piu' lento, blu se piu' veloce
		void DeltaCell(const std::unordered_map<std::string, float>& reference, const std::string& key, float current) {
			const auto found = reference.find(key);
			const float delta = current - (found != reference.end() ? found->second : 0.0f);
			char text[24];
			if (std::abs(delta) < 0.005f)
				snprintf(text, sizeof(text), "0.00");
			else
				snprintf(text, sizeof(text), "%s%.2f", delta > 0.0f ? "+" : "-", std::abs(delta));
			const ImVec4 color = std::abs(delta) < 0.005f ? Theme::TextDim : delta > 0.0f ? Theme::Rgb(0xff9f80) : Theme::Rgb(0x8fc8ff);
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(text).x);
			ImGui::TextColored(color, "%s", text);
		}

		std::string FormatDuration(double ms) {
			char text[32];
			if (ms >= 60000.0) snprintf(text, sizeof(text), "%.1f min", ms / 60000.0);
			else if (ms >= 1000.0) snprintf(text, sizeof(text), "%.1f s", ms / 1000.0);
			else snprintf(text, sizeof(text), "%.0f ms", ms);
			return text;
		}

		const char* LimitName(Bottleneck limit) {
			switch (limit) {
			case Bottleneck::Gpu: return "gpu";
			case Bottleneck::Cpu: return "cpu";
			case Bottleneck::Present: return "present";
			default: return "none";
			}
		}
	}

	// Il range visibile del grafico in numeri di giro: lo storico e' [First, Last), la vista
	// [Begin, End). Pinned e' il frame fissato, anche fuori dalla vista.
	struct ProfilerRange {
		uint64_t First = 0, Last = 0, Begin = 0, End = 0;
		size_t Slots = 0; // i posti del grafico: piu' dei frame quando lo storico e' ancora corto
		std::vector<const FrameRecord*> Frames;
		const FrameRecord* Pinned = nullptr;
		double DurationMs = 0.0, AgoMs = 0.0; // quanto dura la vista, e quanto prima di adesso finisce
	};

	// Le statistiche del range visibile. Calcolarle su decine di migliaia di frame costa
	// millisecondi: si rifanno subito dopo uno zoom o un preset, e quando la vista scorre (il live,
	// un trascinamento) al massimo 4 volte al secondo e mai piu' spesso di 50 volte il loro costo,
	// cosi' non prendono piu' del 2% del frame (ADR 0023).
	struct ProfilerViewStats {
		uint64_t Begin = 0, End = 0;
		double ComputedAt = -1e9, CostMs = 0.0;
		ProfileStats Stats;
	};

	void Application::SaveProfilerReference(bool save) {
		m_ProfilerReference.clear();
		m_ProfilerReferenceFrameMs = 0.0f;
		if (!save)
			return;
		const ProfileStats stats = m_ProfilerStats ? m_ProfilerStats->Stats : ComputeStats(LastFrames(600));
		for (const StatRow& row : stats.Gpu)
			m_ProfilerReference[row.Key] = row.Sum.Avg;
		for (const StatRow& row : stats.Cpu)
			m_ProfilerReference[row.Key] = row.Sum.Avg;
		m_ProfilerReferenceFrameMs = stats.Diag.FrameMs;
	}

	// Il riepilogo da incollare in una issue o in chat: diagnosi, tempi, memoria, ultimo caricamento
	std::string Application::ProfilerSummaryText() const {
		const ProfileStats stats = m_ProfilerStats ? m_ProfilerStats->Stats : ComputeStats(LastFrames(600));
		std::string out;
		char line[256];
		auto add = [&](const char* format, auto... args) {
			snprintf(line, sizeof(line), format, args...);
			out += line;
			out += '\n';
		};
		const LoadCapture& load = Profiler::Get().LastLoad();
		add("Lucerna profile: %s on %s, %zu frames", load.Label.empty() ? "(no scene file)" : load.Label.c_str(), m_Vulkan.DeviceName(), stats.Frames);
		bool warn = false;
		const auto [label, detail] = DiagnosisText(stats.Diag, warn);
		add("%s: %s", label.c_str(), detail.c_str());
		add("Frame %.2f ms (%.0f fps): CPU work %.2f, waiting for GPU %.2f, waiting for present %.2f",
		    stats.Diag.FrameMs, stats.Diag.FrameMs > 0.0f ? 1000.0f / stats.Diag.FrameMs : 0.0f, stats.Diag.CpuMs, stats.Diag.GpuWaitMs, stats.Diag.PresentWaitMs);
		if (stats.HasThroughput())
			add("Throughput: %.0f Mrays/s, %.2f M rays/frame, %.2f rays/pixel", stats.MraysPerSecond(), stats.RaysPerFrame() / 1e6, stats.RaysPerPixel());
		add("");
		add("%-32s %8s %8s %8s %8s", "GPU pass (ms)", "min", "avg", "p99", "max");
		for (const StatRow& row : stats.Gpu)
			add("%-32s %8.2f %8.2f %8.2f %8.2f", (std::string(row.Depth * 2, ' ') + row.Name).c_str(), row.Sum.Min, row.Sum.Avg, row.Sum.P99, row.Sum.Max);
		add("");
		add("%-32s %8s %8s", "CPU scope (ms)", "avg", "p99");
		for (const StatRow& row : stats.Cpu)
			add("%-32s %8.2f %8.2f", (std::string(row.Depth * 2, ' ') + row.Name).c_str(), row.Sum.Avg, row.Sum.P99);
		const MemoryReport memory = m_Vulkan.Memory();
		add("");
		if (memory.HasBudget)
			add("VRAM: %.0f MB used, %.0f MB budget, %.0f MB heap", memory.HeapUsage / 1e6, memory.HeapBudget / 1e6, memory.HeapSize / 1e6);
		else
			add("VRAM: %.0f MB tracked, %.0f MB heap", memory.DeviceTotal() / 1e6, memory.HeapSize / 1e6);
		for (size_t c = 0; c < static_cast<size_t>(MemoryCategory::Count); c++)
			if (memory.Device[c] > 0)
				add("  %-30s %8.1f MB", MemoryCategoryName(static_cast<MemoryCategory>(c)), memory.Device[c] / 1e6);
		const ProcessMemoryInfo ram = ProcessMemory();
		add("RAM: %.0f MB resident, %.0f MB peak", ram.Resident / 1e6, ram.Peak / 1e6);
		if (!load.Label.empty()) {
			add("");
			add("Last load: %s in %.2f s", load.Label.c_str(), load.TotalMs / 1000.0f);
			for (const LoadPhase& phase : load.Phases)
				add("  %-40s %8.0f ms", (std::string(phase.Depth * 2, ' ') + phase.Name).c_str(), phase.Ms);
		}
		return out;
	}

	// PT_PROFILE: lo stesso riepilogo in JSON, alla fine di una run scriptata. I primi frame si
	// saltano: sono il caricamento e il riscaldamento, e falserebbero max e p99.
	void Application::WriteProfile(const std::string& path) const {
		const std::deque<FrameRecord>& history = Profiler::Get().History();
		const size_t skip = history.size() > 40 ? 10 : 0;
		const ProfileStats stats = ComputeStats(LastFrames(history.size() - skip));
		auto summary = [](const Summary& s) { return nlohmann::json{ { "min", s.Min }, { "avg", s.Avg }, { "p99", s.P99 }, { "max", s.Max } }; };

		nlohmann::json j;
		const LoadCapture& load = Profiler::Get().LastLoad();
		j["scene"] = load.Label;
		j["device"] = m_Vulkan.DeviceName();
		j["frames"] = stats.Frames;
		j["diagnosis"] = { { "limit", LimitName(stats.Diag.Limit) }, { "frameMs", stats.Diag.FrameMs }, { "cpuWorkMs", stats.Diag.CpuMs },
		                   { "gpuWaitMs", stats.Diag.GpuWaitMs }, { "presentWaitMs", stats.Diag.PresentWaitMs },
		                   { "gpuBusyMs", stats.Diag.GpuBusyMs }, { "topPass", stats.Diag.TopPass }, { "topPassMs", stats.Diag.TopPassMs } };
		for (const StatRow& row : stats.Gpu)
			j["gpu"].push_back({ { "name", row.Name }, { "depth", row.Depth }, { "ms", summary(row.Sum) } });
		for (const StatRow& row : stats.Cpu)
			j["cpu"].push_back({ { "name", row.Name }, { "path", row.Key.substr(4) }, { "depth", row.Depth }, { "ms", summary(row.Sum) } });
		if (stats.HasThroughput())
			j["throughput"] = { { "mraysPerSecond", stats.MraysPerSecond() }, { "raysPerFrame", stats.RaysPerFrame() }, { "raysPerPixel", stats.RaysPerPixel() } };
		const MemoryReport memory = m_Vulkan.Memory();
		nlohmann::json categories = nlohmann::json::object();
		for (size_t c = 0; c < static_cast<size_t>(MemoryCategory::Count); c++)
			if (memory.Device[c] > 0)
				categories[MemoryCategoryName(static_cast<MemoryCategory>(c))] = memory.Device[c];
		j["memory"] = { { "heapBytes", memory.HeapSize }, { "categoryBytes", categories } };
		if (memory.HasBudget)
			j["memory"]["budget"] = { { "budgetBytes", memory.HeapBudget }, { "usageBytes", memory.HeapUsage } };
		const ProcessMemoryInfo ram = ProcessMemory();
		j["ram"] = { { "residentBytes", ram.Resident }, { "peakBytes", ram.Peak } };
		if (!load.Label.empty()) {
			j["load"] = { { "label", load.Label }, { "totalMs", load.TotalMs } };
			for (const LoadPhase& phase : load.Phases)
				j["load"]["phases"].push_back({ { "name", phase.Name }, { "depth", phase.Depth }, { "startMs", phase.StartMs }, { "ms", phase.Ms } });
		}

		std::ofstream file(path);
		file << j.dump(2) << std::endl;
		std::cout << (file ? "Profile: " : "Profile: could not write ") << path << std::endl;
	}

	std::string Application::ProfilerHudLine(bool& warn) const {
		const auto [label, detail] = DiagnosisText(Diagnose(LastFrames(120)), warn);
		return label + " \xc2\xb7 " + detail;
	}

	// Un imgui.ini di prima del profiler non sa dove metterlo: la prima volta che si apre lo si
	// aggancia sotto la viewport, come nel layout di default. Al primo frame la viewport non
	// esiste ancora: si riprova, e intanto il pannello aspetta (DrawProfilerPanel).
	void Application::DockProfilerUnderViewport() {
		if (ImGui::FindWindowSettingsByID(ImHashStr(PROFILER)) != nullptr) {
			m_ProfilerDockChecked = true;
			return;
		}
		ImGuiWindow* viewport = ImGui::FindWindowByName(VIEWPORT);
		if (viewport == nullptr || viewport->DockNode == nullptr)
			return;
		m_ProfilerDockChecked = true;
		ImGuiID up = viewport->DockNode->ID;
		const ImGuiID down = ImGui::DockBuilderSplitNode(up, ImGuiDir_Down, 0.40f, nullptr, &up);
		ImGui::DockBuilderDockWindow(PROFILER, down);
		ImGui::DockBuilderFinish(ImGui::DockNodeGetRootNode(viewport->DockNode)->ID);
	}

	namespace {
		std::string FormatBytes(double bytes) {
			char text[32];
			if (bytes >= 1e9) snprintf(text, sizeof(text), "%.2f GB", bytes / 1e9);
			else if (bytes >= 1e6) snprintf(text, sizeof(text), "%.0f MB", bytes / 1e6);
			else if (bytes >= 1e3) snprintf(text, sizeof(text), "%.0f KB", bytes / 1e3);
			else snprintf(text, sizeof(text), "%.0f B", bytes);
			return text;
		}

		ImU32 CategoryColor(MemoryCategory category) {
			switch (category) {
			case MemoryCategory::TriangleBuffers: return Col(Theme::Rgb(0x6fd6b8));
			case MemoryCategory::ColorTextures: return Col(Theme::Accent);
			case MemoryCategory::DataTextures: return Col(Theme::Rgb(0xc3a6ff));
			case MemoryCategory::Blas: return Col(Theme::Rgb(0xff7f6b));
			case MemoryCategory::Tlas: return Col(Theme::SelectedText);
			case MemoryCategory::AccelScratch: return Col(Theme::Rgb(0xb05a4e));
			case MemoryCategory::RenderTargets: return Col(Theme::IconSky);
			case MemoryCategory::Environment: return Col(Theme::Rgb(0x4f8fd1));
			case MemoryCategory::SceneBuffers: return Col(Theme::Rgb(0xd9d9e0));
			case MemoryCategory::Staging: return Col(Theme::Rgb(0x8a8a93));
			default: return Col(Theme::Rgb(0x6a6a72));
			}
		}

		// Un numero allineato a destra nella cella della tabella
		void RightAligned(const std::string& text, bool dim) {
			ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(text.c_str()).x);
			if (dim)
				ImGui::TextColored(Theme::TextDim, "%s", text.c_str());
			else
				ImGui::TextUnformatted(text.c_str());
		}
	}

	// La tab Memory: la VRAM per categoria contro il budget del driver, le strutture di
	// accelerazione per asset, e la RAM del processo
	void Application::DrawMemoryTab() {
		PROFILE_SCOPE("Memory tab");
		const float s = UI::Scale();
		const MemoryReport memory = m_Vulkan.Memory();
		constexpr size_t categories = static_cast<size_t>(MemoryCategory::Count);
		const double tracked = static_cast<double>(memory.DeviceTotal());
		// quello che il driver conta e noi no: swapchain, ImGui, il driver stesso
		const double untracked = memory.HasBudget && memory.HeapUsage > memory.DeviceTotal() ? double(memory.HeapUsage - memory.DeviceTotal()) : 0.0;
		const double used = tracked + untracked;
		const double heap = static_cast<double>(std::max<VkDeviceSize>(memory.HeapSize, 1));
		const double budget = memory.HasBudget ? static_cast<double>(memory.HeapBudget) : heap;
		ImDrawList* list = ImGui::GetWindowDrawList();

		// ---- la barra della VRAM
		{
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x;
			const float h = 116.0f * s;
			list->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(Theme::Rail), 4.0f * s);
			list->AddRect(p, ImVec2(p.x + w, p.y + h), Col(Theme::Border), 4.0f * s);
			ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f * s, p.y + 10.0f * s));
			ImGui::PushFont(UI::BoldFont());
			ImGui::TextUnformatted("VRAM");
			ImGui::PopFont();
			ImGui::SameLine();
			ImGui::TextColored(Theme::TextDim, "%s \xc2\xb7 device-local heap%s", m_Vulkan.DeviceName(),
			                   memory.HasBudget ? " \xc2\xb7 VK_EXT_memory_budget" : " \xc2\xb7 no budget extension: only tracked allocations");
			std::string summary = FormatBytes(used) + " Lucerna";
			if (memory.HasBudget)
				summary += " \xc2\xb7 " + FormatBytes(budget) + " budget";
			summary += " \xc2\xb7 " + FormatBytes(heap) + " heap";
			ImGui::PushFont(UI::MonoFont());
			list->AddText(ImVec2(p.x + w - 14.0f * s - ImGui::CalcTextSize(summary.c_str()).x, p.y + 12.0f * s), Col(Theme::Text), summary.c_str());
			ImGui::PopFont();

			// le categorie, poi il non tracciato, il libero nel budget, e oltre il budget (gli altri processi)
			const float x0 = p.x + 14.0f * s, x1 = p.x + w - 14.0f * s, y0 = p.y + 36.0f * s, y1 = y0 + 16.0f * s;
			list->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), Col(Theme::Field), 3.0f * s);
			float x = x0;
			auto segment = [&](double bytes, ImU32 color) {
				const float x2 = std::min(x1, x + static_cast<float>((x1 - x0) * bytes / heap));
				if (x2 > x)
					list->AddRectFilled(ImVec2(x, y0), ImVec2(x2, y1), color);
				x = x2;
			};
			for (size_t c = 0; c < categories; c++)
				segment(static_cast<double>(memory.Device[c]), CategoryColor(static_cast<MemoryCategory>(c)));
			segment(untracked, Col(Theme::Rgb(0x55555d)));
			const float freeEnd = x0 + static_cast<float>((x1 - x0) * std::min(budget, heap) / heap);
			for (float hx = freeEnd; hx < x1; hx += 6.0f * s) // oltre il budget: tratteggio
				list->AddLine(ImVec2(hx, y1), ImVec2(std::min(hx + 6.0f * s, x1), y0), Col(Theme::Rgb(0x4a4a52)), 1.0f);

			ImGui::SetCursorScreenPos(ImVec2(x0, y1 + 10.0f * s));
			bool first = true;
			for (size_t c = 0; c < categories; c++) {
				if (memory.Device[c] == 0)
					continue;
				if (!first)
					ImGui::SameLine(0.0f, 14.0f * s);
				first = false;
				Swatch(CategoryColor(static_cast<MemoryCategory>(c)));
				ImGui::TextColored(Theme::TextDim, "%s", MemoryCategoryName(static_cast<MemoryCategory>(c)));
			}
			if (untracked > 0.0) {
				ImGui::SameLine(0.0f, 14.0f * s);
				Swatch(Col(Theme::Rgb(0x55555d)));
				ImGui::TextColored(Theme::TextDim, "Untracked");
			}

			// la memoria fuori dall'heap della GPU e la RAM del processo
			VkDeviceSize host = 0;
			for (VkDeviceSize bytes : memory.Host)
				host += bytes;
			const ProcessMemoryInfo ram = ProcessMemory();
			ImGui::SetCursorScreenPos(ImVec2(x0, ImGui::GetCursorScreenPos().y + 2.0f * s));
			auto pair = [&](const char* label, double bytes, bool first) {
				if (!first)
					ImGui::SameLine(0.0f, 6.0f * s);
				ImGui::TextColored(Theme::TextDim, "%s", label);
				ImGui::SameLine();
				ImGui::PushFont(UI::MonoFont());
				ImGui::TextUnformatted(FormatBytes(bytes).c_str());
				ImGui::PopFont();
			};
			pair("Process RAM", static_cast<double>(ram.Resident), true);
			pair("\xc2\xb7 peak", static_cast<double>(ram.Peak), false);
			pair("\xc2\xb7 host-visible buffers", static_cast<double>(host), false);
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 10.0f * s));
		}

		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Theme::Header);
		ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, Theme::Rgb(0x26262b));
		if (ImGui::BeginTable("##memory", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0.0f, ImGui::GetContentRegionAvail().y))) {
			ImGui::TableSetupColumn("categories", ImGuiTableColumnFlags_WidthStretch, 5.0f);
			ImGui::TableSetupColumn("structures", ImGuiTableColumnFlags_WidthStretch, 7.0f);
			ImGui::TableNextRow();

			// ---- le categorie
			ImGui::TableSetColumnIndex(0);
			if (ImGui::BeginTable("##categories", 3, flags)) {
				ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("VRAM", ImGuiTableColumnFlags_WidthFixed, 80.0f * s);
				ImGui::TableSetupColumn("% used", ImGuiTableColumnFlags_WidthFixed, 60.0f * s);
				ImGui::TableHeadersRow();
				auto row = [&](ImU32 color, const char* name, double bytes, bool dim) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					Swatch(color);
					ImGui::TextColored(dim ? Theme::TextDim : Theme::Text, "%s", name);
					ImGui::PushFont(UI::MonoFont());
					ImGui::TableSetColumnIndex(1);
					RightAligned(FormatBytes(bytes), false);
					ImGui::TableSetColumnIndex(2);
					RightAligned(Percent(static_cast<float>(bytes), static_cast<float>(used)), true);
					ImGui::PopFont();
				};
				std::vector<size_t> order;
				for (size_t c = 0; c < categories; c++)
					if (memory.Device[c] > 0)
						order.push_back(c);
				std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return memory.Device[a] > memory.Device[b]; });
				for (size_t c : order)
					row(CategoryColor(static_cast<MemoryCategory>(c)), MemoryCategoryName(static_cast<MemoryCategory>(c)), static_cast<double>(memory.Device[c]), false);
				if (untracked > 0.0)
					row(Col(Theme::Rgb(0x55555d)), "Untracked (driver, swapchain, ImGui)", untracked, true);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 0.62f, 0.0f));
				ImGui::SameLine(0.0f, 6.0f * s);
				ImGui::TextColored(Theme::SelectedText, "Total");
				ImGui::PushFont(UI::MonoFont());
				ImGui::TableSetColumnIndex(1);
				RightAligned(FormatBytes(used), false);
				ImGui::PopFont();
				ImGui::EndTable();
			}


			// ---- le strutture di accelerazione
			ImGui::TableSetColumnIndex(1);
			const RenderScene& gpu = m_Renderer.GpuScene();
			const AccelStats& accel = gpu.Accel();
			std::vector<MeshAccelRow> meshes = gpu.MeshAccel();
			std::sort(meshes.begin(), meshes.end(), [](const MeshAccelRow& a, const MeshAccelRow& b) { return a.Size > b.Size; });
			if (ImGui::BeginTable("##structures", 6, flags)) {
				ImGui::TableSetupColumn("Acceleration structure", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("Instances", ImGuiTableColumnFlags_WidthFixed, 64.0f * s);
				ImGui::TableSetupColumn("Triangles", ImGuiTableColumnFlags_WidthFixed, 84.0f * s);
				ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
				ImGui::TableSetupColumn("Scratch", ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
				ImGui::TableSetupColumn("Build", ImGuiTableColumnFlags_WidthFixed, 70.0f * s);
				ImGui::TableHeadersRow();
				auto row = [&](const std::string& name, int depth, const std::string& instances, const std::string& triangles,
				               const std::string& size, const std::string& scratch, const std::string& build) {
					ImGui::TableNextRow();
					ImGui::TableSetColumnIndex(0);
					ImGui::Indent(16.0f * s * depth);
					ImGui::TextColored(depth == 0 ? Theme::Text : Theme::TextDim, "%s", name.c_str());
					ImGui::Unindent(16.0f * s * depth);
					ImGui::PushFont(UI::MonoFont());
					const std::string* cells[5] = { &instances, &triangles, &size, &scratch, &build };
					for (int c = 0; c < 5; c++) {
						ImGui::TableSetColumnIndex(c + 1);
						RightAligned(*cells[c], c != 1 && c != 2);
					}
					ImGui::PopFont();
				};
				auto ms = [](float v) { char t[24]; snprintf(t, sizeof(t), v >= 1.0f ? "%.0f ms" : "%.2f ms", v); return std::string(t); };
				auto count = [](uint32_t v) { return std::to_string(v); };
				for (const MeshAccelRow& mesh : meshes) {
					row(mesh.Name, 0, std::to_string(mesh.Instances), count(mesh.Opaque + mesh.Masked),
					    FormatBytes(double(mesh.Size)), FormatBytes(double(mesh.Scratch)), "");
					if (mesh.Masked > 0) {
						row("Opaque", 1, "", count(mesh.Opaque), "", "", "");
						row("Masked (alpha)", 1, "", count(mesh.Masked), "", "", "");
					}
				}
				if (!meshes.empty())
					row("Mesh BLAS build", 0, "", "", "", "", ms(accel.MeshBuildMs));
				if (accel.Primitives > 0)
					row("Primitives (AABB)", 0, std::to_string(accel.Primitives), "-",
					    FormatBytes(double(accel.ProceduralSize)), FormatBytes(double(accel.ProceduralScratch)), ms(accel.ProceduralBuildMs));
				row("TLAS", 0, std::to_string(accel.Instances), "-", FormatBytes(double(accel.TlasSize)),
				    FormatBytes(double(accel.TlasScratch)), ms(accel.TlasBuildMs));
				ImGui::EndTable();
			}
			ImGui::Spacing();
			ImGui::TextWrapped("Sorted by size. The mesh BLAS share one buffer and build in one batch; masked geometry is split as in ADR 0008.");
			ImGui::EndTable();
		}
		ImGui::PopStyleColor(2);
	}

	// La tab Loading: l'ultimo caricamento come albero di fasi, con quando comincia ognuna, quanto
	// dura e una barra sulla linea del tempo del caricamento
	void Application::DrawLoadingTab() {
		const float s = UI::Scale();
		const Profiler& profiler = Profiler::Get();
		const LoadCapture& load = profiler.LastLoad();
		if (load.Label.empty()) {
			ImGui::TextDisabled(profiler.Loading() ? "Loading..." : "No load captured yet. Open a scene or a file to see where its time goes.");
			return;
		}

		PROFILE_SCOPE("Loading tab");
		ImGui::AlignTextToFramePadding();
		ImGui::PushFont(UI::BoldFont());
		ImGui::TextUnformatted("Last load");
		ImGui::PopFont();
		ImGui::SameLine();
		ImGui::PushFont(UI::MonoFont());
		ImGui::TextColored(Theme::TextDim, "%s", std::filesystem::path(load.Label).lexically_normal().string().c_str());
		char total[32];
		snprintf(total, sizeof(total), "%.2f s", load.TotalMs / 1000.0f);
		const char* suffix = "to the first frame";
		ImGui::PopFont();
		const float rightW = ImGui::CalcTextSize(suffix).x + ImGui::GetStyle().ItemSpacing.x;
		ImGui::PushFont(UI::MonoFont());
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - rightW - ImGui::CalcTextSize(total).x);
		ImGui::TextUnformatted(total);
		ImGui::PopFont();
		ImGui::SameLine();
		ImGui::TextColored(Theme::TextDim, "%s", suffix);
		if (profiler.Loading()) {
			ImGui::SameLine();
			ImGui::TextColored(Theme::Accent, "  loading another...");
		}

		// i colori della linea del tempo seguono la fase di primo livello
		const ImU32 palette[] = { Col(Theme::Rgb(0x6fd6b8)), Col(Theme::IconSky), Col(Theme::Accent), Col(Theme::Rgb(0xc3a6ff)),
		                          Col(Theme::Rgb(0xff7f6b)), Col(Theme::Rgb(0xd9d9e0)) };
		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY;
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Theme::Header);
		ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, Theme::Rgb(0x26262b));
		if (ImGui::BeginTable("##load", 4, flags, ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y, 80.0f * s)))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("Phase", ImGuiTableColumnFlags_WidthFixed, 380.0f * s);
			ImGui::TableSetupColumn("Start", ImGuiTableColumnFlags_WidthFixed, 64.0f * s);
			ImGui::TableSetupColumn("Time", ImGuiTableColumnFlags_WidthFixed, 72.0f * s);
			ImGui::TableSetupColumn("Timeline", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableHeadersRow();

			std::vector<std::string> stack; // i percorsi degli antenati: la chiave per chiudere una riga
			int collapsedDepth = -1, topLevel = -1;
			for (size_t i = 0; i < load.Phases.size(); i++) {
				const LoadPhase& phase = load.Phases[i];
				stack.resize(phase.Depth);
				const std::string key = "load/" + (stack.empty() ? phase.Name : stack.back() + "/" + phase.Name);
				stack.push_back(key);
				if (phase.Depth == 0)
					topLevel++;
				if (collapsedDepth >= 0 && phase.Depth > collapsedDepth)
					continue;
				collapsedDepth = -1;
				const bool parent = i + 1 < load.Phases.size() && load.Phases[i + 1].Depth > phase.Depth;
				const bool collapsed = parent && m_ProfilerCollapsed.count(key) > 0;
				if (collapsed)
					collapsedDepth = phase.Depth;

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(key.c_str());
				ImGui::Indent(16.0f * s * phase.Depth);
				const ImVec4 tone = phase.Depth == 0 ? Theme::Text : Theme::TextDim;
				if (parent) {
					const std::string label = std::string(collapsed ? ICON_CHEVRON_RIGHT " " : ICON_CHEVRON_DOWN " ") + phase.Name;
					ImGui::PushStyleColor(ImGuiCol_Text, tone);
					if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
						if (collapsed)
							m_ProfilerCollapsed.erase(key);
						else
							m_ProfilerCollapsed.insert(key);
					}
					ImGui::PopStyleColor();
				}
				else {
					ImGui::Dummy(ImVec2(ImGui::CalcTextSize(ICON_CHEVRON_DOWN " ").x, 0.0f));
					ImGui::SameLine(0.0f, 0.0f);
					ImGui::TextColored(tone, "%s", phase.Name.c_str());
				}
				ImGui::Unindent(16.0f * s * phase.Depth);
				ImGui::PopID();

				ImGui::PushFont(UI::MonoFont());
				char start[24], time[24];
				snprintf(start, sizeof(start), "%.2f s", phase.StartMs / 1000.0f);
				if (phase.Ms >= 1000.0f)
					snprintf(time, sizeof(time), "%.2f s", phase.Ms / 1000.0f);
				else
					snprintf(time, sizeof(time), "%.0f ms", phase.Ms);
				ImGui::TableSetColumnIndex(1);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(start).x);
				ImGui::TextColored(Theme::TextDim, "%s", start);
				ImGui::TableSetColumnIndex(2);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(time).x);
				ImGui::TextUnformatted(time);
				ImGui::PopFont();

				ImGui::TableSetColumnIndex(3);
				const ImVec2 p = ImGui::GetCursorScreenPos();
				const float w = ImGui::GetContentRegionAvail().x - 8.0f * s;
				const float h = ImGui::GetTextLineHeight();
				const float x0 = p.x + 8.0f * s + w * phase.StartMs / load.TotalMs;
				const float x1 = std::max(x0 + 2.0f, x0 + w * phase.Ms / load.TotalMs);
				const ImU32 color = palette[std::max(topLevel, 0) % std::size(palette)];
				ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(x0, p.y + h * 0.2f), ImVec2(x1, p.y + h * 0.8f), color, 2.0f * s);
				ImGui::Dummy(ImVec2(w, h));
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor(2);
	}


	void Application::DrawProfilerPanel() {
		const float s = UI::Scale();
		if (!m_ProfilerDockChecked)
			return; // non ancora agganciato: aperto adesso, comparirebbe flottante
		if (!ImGui::Begin(PROFILER, &m_ShowProfiler)) {
			m_ProfilerFocused = false;
			ImGui::End();
			return;
		}
		m_ProfilerFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
		if (m_FocusProfiler) {
			ImGui::SetWindowFocus();
			m_FocusProfiler = false;
		}

		// ---- il range visibile: live finisce all'ultimo giro, se no a m_ProfilerEnd
		const Profiler& profiler = Profiler::Get();
		const std::deque<FrameRecord>& history = profiler.History();
		ProfilerRange range;
		range.First = profiler.FirstFrame();
		range.Last = range.First + history.size();
		// lo span chiesto resta anche se lo storico e' ancora piu' corto: si riempie mentre cresce
		m_ProfilerSpan = std::clamp(m_ProfilerSpan, MIN_SPAN, static_cast<double>(Profiler::MaxFrames));
		const uint64_t span = std::min(static_cast<uint64_t>(std::llround(m_ProfilerSpan)), history.size());
		if (m_ProfilerLive)
			range.End = range.Last;
		else {
			// la vista resta sugli stessi giri mentre lo storico cresce; se escono, si ferma sul piu' vecchio
			m_ProfilerEnd = std::clamp(m_ProfilerEnd, static_cast<double>(range.First + span), static_cast<double>(range.Last));
			range.End = static_cast<uint64_t>(m_ProfilerEnd);
		}
		range.Begin = range.End - span;
		// finche' lo storico e' piu' corto dello span, i posti vuoti stanno a sinistra; "tutto lo
		// storico" (lo span al tetto) e' invece cio' che c'e'
		const bool all = m_ProfilerSpan >= static_cast<double>(Profiler::MaxFrames);
		range.Slots = all ? span : std::max<size_t>(span, static_cast<size_t>(std::llround(m_ProfilerSpan)));
		range.Frames.reserve(span);
		for (uint64_t i = range.Begin; i < range.End; i++) {
			range.Frames.push_back(&history[i - range.First]);
			range.DurationMs += history[i - range.First].FrameMs;
		}
		for (uint64_t i = range.End; i < range.Last; i++)
			range.AgoMs += history[i - range.First].FrameMs;
		if (m_ProfilerPinned >= 0 && static_cast<uint64_t>(m_ProfilerPinned) < range.First)
			m_ProfilerPinned = -1; // uscito dallo storico
		if (m_ProfilerPinned >= 0)
			range.Pinned = &history[m_ProfilerPinned - range.First];

		auto goLive = [&] {
			m_ProfilerLive = true;
			m_ProfilerPinned = -1;
			m_ProfilerStatsDirty = true;
		};
		if (!m_ProfilerLive && m_ProfilerFocused && ImGui::IsKeyPressed(ImGuiKey_Escape))
			goLive();

		// ---- toolbar: le tab, e per Frame la vista
		if (UI::Pill("Frame", m_ProfilerTab == 0))
			m_ProfilerTab = 0;
		ImGui::SameLine(0.0f, 2.0f * s);
		if (UI::Pill("Memory", m_ProfilerTab == 2))
			m_ProfilerTab = 2;
		ImGui::SameLine(0.0f, 2.0f * s);
		if (UI::Pill("Loading", m_ProfilerTab == 1))
			m_ProfilerTab = 1;
		if (m_ProfilerTab != 0) {
			ImGui::Separator();
			if (m_ProfilerTab == 1)
				DrawLoadingTab();
			else
				DrawMemoryTab();
			ImGui::End();
			return;
		}
		if (!m_ProfilerLive) {
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			const std::string ago = FormatDuration(range.AgoMs);
			if (range.Pinned)
				ImGui::TextColored(Theme::SelectedText, ICON_PAUSE " Paused \xc2\xb7 frame %lld pinned", static_cast<long long>(m_ProfilerPinned) - static_cast<long long>(range.Last) + 1);
			else
				ImGui::TextColored(Theme::SelectedText, ICON_PAUSE " Paused \xc2\xb7 %s ago", ago.c_str());
		}
		const bool hasReference = m_ProfilerReferenceFrameMs > 0.0f;
		const char* copyLabel = ICON_RESTORE " Copy summary";
		const char* referenceLabel = hasReference ? "Clear reference" : "Save reference";
		const char* liveLabel = m_ProfilerLive ? ICON_PAUSE " Pause" : ICON_PLAY " Live";
		const float comboW = 170.0f * s;
		auto pillW = [](const char* label) { return ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2; };
		const float spacing = ImGui::GetStyle().ItemSpacing.x;
		// i controlli a destra; se non ci stanno accanto alle tab, vanno sulla riga sotto
		const float rightX = ImGui::GetWindowContentRegionMax().x - comboW - pillW(liveLabel) - pillW(referenceLabel) - pillW(copyLabel) - 3 * spacing;
		ImGui::SameLine();
		if (ImGui::GetCursorPosX() > rightX)
			ImGui::NewLine();
		ImGui::SetCursorPosX(std::max(rightX, ImGui::GetCursorPosX()));
		if (UI::Pill(copyLabel))
			ImGui::SetClipboardText(ProfilerSummaryText().c_str());
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Copy diagnosis, times, memory and the last load as text");
		ImGui::SameLine();
		if (UI::Pill(referenceLabel, hasReference))
			SaveProfilerReference(!hasReference);
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(hasReference ? "Remove the comparison column" : "Keep the visible averages and compare everything against them");
		ImGui::SameLine();
		if (UI::Pill(liveLabel, !m_ProfilerLive)) {
			if (m_ProfilerLive) {
				m_ProfilerLive = false;
				m_ProfilerEnd = static_cast<double>(range.End);
			}
			else
				goLive();
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(m_ProfilerLive ? "Stop following the present: recording goes on" : "Follow the present again (Esc)");
		ImGui::SameLine();
		// i preset dello zoom, in tempo: contano all'indietro dalla fine della vista
		ImGui::SetNextItemWidth(comboW);
		char preview[64];
		snprintf(preview, sizeof(preview), "%s \xc2\xb7 %zu frames", FormatDuration(range.DurationMs).c_str(), range.Frames.size());
		if (ImGui::BeginCombo("##span", preview)) {
			const double presets[] = { 2000.0, 10000.0, 60000.0, 0.0 };
			const char* names[] = { "2 s", "10 s", "1 min", "All history" };
			for (int i = 0; i < 4; i++)
				if (ImGui::Selectable(names[i])) {
					double frames = static_cast<double>(Profiler::MaxFrames), ms = 0.0;
					if (presets[i] > 0.0)
						for (uint64_t f = range.End; f > range.First; f--) {
							ms += history[f - 1 - range.First].FrameMs;
							if (ms >= presets[i]) {
								frames = static_cast<double>(range.End - f + 1);
								break;
							}
						}
					m_ProfilerSpan = std::max(frames, MIN_SPAN);
					m_ProfilerStatsDirty = true;
				}
			ImGui::EndCombo();
		}
		ImGui::Separator();

		if (range.Frames.empty()) {
			ImGui::TextDisabled("Collecting frames...");
			ImGui::End();
			return;
		}

		// ---- le statistiche del range, dalla cache se non serve rifarle
		if (!m_ProfilerStats)
			m_ProfilerStats = std::make_shared<ProfilerViewStats>();
		{
			ProfilerViewStats& cache = *m_ProfilerStats;
			const double now = Profiler::NowMs();
			const bool changed = cache.Begin != range.Begin || cache.End != range.End;
			const bool due = now - cache.ComputedAt >= std::max(250.0, 50.0 * cache.CostMs);
			if (m_ProfilerStatsDirty || (changed && due)) {
				PROFILE_SCOPE("Profiler stats");
				cache.Stats = ComputeStats(range.Frames);
				cache.Begin = range.Begin;
				cache.End = range.End;
				cache.ComputedAt = Profiler::NowMs();
				cache.CostMs = cache.ComputedAt - now;
				m_ProfilerStatsDirty = false;
			}
		}
		const ProfileStats& stats = m_ProfilerStats->Stats;
		const Diagnosis& diagnosis = stats.Diag;
		ImDrawList* list = ImGui::GetWindowDrawList();
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(ImGui::GetStyle().ItemSpacing.x, 4.0f * s));

		// ---- la banda della diagnosi: cosa limita il frame, e come si divide il suo tempo
		{
			bool warn = false;
			const auto [label, detail] = DiagnosisText(diagnosis, warn);
			const ImVec2 p = ImGui::GetCursorScreenPos();
			const float w = ImGui::GetContentRegionAvail().x, h = 52.0f * s;
			list->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Col(Theme::Rail), 4.0f * s);
			list->AddRect(p, ImVec2(p.x + w, p.y + h), Col(warn ? Theme::AccentFill : Theme::Border), 4.0f * s);
			const ImVec4 tone = warn ? Theme::Accent : Theme::Text;

			ImGui::SetCursorScreenPos(ImVec2(p.x + 14.0f * s, p.y + (h - ImGui::GetTextLineHeight()) * 0.5f));
			ImGui::TextColored(tone, ICON_ZAP);
			ImGui::SameLine();
			ImGui::PushFont(UI::BoldFont());
			ImGui::TextColored(tone, "%s", label.c_str());
			ImGui::PopFont();
			ImGui::SameLine();
			ImGui::TextColored(Theme::TextDim, "%s", detail.c_str());
			const float labelEnd = ImGui::GetItemRectMax().x;

			// a destra: la durata del frame
			char frameText[48], fpsText[48];
			snprintf(frameText, sizeof(frameText), "%.2f ms", diagnosis.FrameMs);
			snprintf(fpsText, sizeof(fpsText), "%.0f fps \xc2\xb7 avg of %zu", 1000.0f / diagnosis.FrameMs, stats.Frames);
			ImGui::PushFont(UI::MonoFont());
			const float rightW = std::max(ImGui::CalcTextSize(frameText).x, ImGui::CalcTextSize(fpsText).x);
			const float rightX = p.x + w - 14.0f * s - rightW;
			list->AddText(ImVec2(p.x + w - 14.0f * s - ImGui::CalcTextSize(frameText).x, p.y + 9.0f * s), Col(Theme::Text), frameText);
			list->AddText(ImVec2(p.x + w - 14.0f * s - ImGui::CalcTextSize(fpsText).x, p.y + 28.0f * s), Col(Theme::TextDim), fpsText);
			ImGui::PopFont();

			// in mezzo: il frame diviso in lavoro della CPU, attesa della GPU e attesa del display
			const float barX0 = labelEnd + 28.0f * s, barX1 = rightX - 24.0f * s;
			if (barX1 - barX0 > 120.0f * s) {
				const float sum = std::max(diagnosis.CpuMs + diagnosis.GpuWaitMs + diagnosis.PresentWaitMs, 1e-3f);
				const float parts[3] = { diagnosis.CpuMs, diagnosis.GpuWaitMs, diagnosis.PresentWaitMs };
				const ImVec4 colors[3] = { CPU_WORK, WAIT_GPU, WAIT_PRESENT };
				const char* longNames[3] = { "CPU work", "Waiting for GPU", "Waiting for present" };
				const char* shortNames[3] = { "CPU", "GPU", "Present" };
				// la legenda con i nomi lunghi se ci sta, poi con quelli corti, se no niente
				auto legendWidth = [&](const char* const* names) {
					float width = 0.0f;
					for (int i = 0; i < 3; i++) {
						char value[32];
						snprintf(value, sizeof(value), "%.2f ms", parts[i]);
						ImGui::PushFont(UI::MonoFont());
						width += ImGui::CalcTextSize(value).x;
						ImGui::PopFont();
						width += ImGui::GetFontSize() * 0.62f + 6.0f * s + ImGui::CalcTextSize(names[i]).x + ImGui::GetStyle().ItemSpacing.x + (i > 0 ? 18.0f * s : 0.0f);
					}
					return width;
				};
				const char* const* names = legendWidth(longNames) <= barX1 - barX0 ? longNames
				                         : legendWidth(shortNames) <= barX1 - barX0 ? shortNames : nullptr;
				const float barY = p.y + (names ? 11.0f : 21.0f) * s;
				list->AddRectFilled(ImVec2(barX0, barY), ImVec2(barX1, barY + 10.0f * s), Col(Theme::Field), 2.0f * s);
				float x = barX0;
				for (int i = 0; i < 3; i++) {
					const float x1 = x + (barX1 - barX0) * parts[i] / sum;
					list->AddRectFilled(ImVec2(x, barY), ImVec2(x1, barY + 10.0f * s), Col(colors[i]));
					x = x1;
				}
				ImGui::SetCursorScreenPos(ImVec2(barX0, barY + 15.0f * s));
				for (int i = 0; names && i < 3; i++) {
					if (i > 0)
						ImGui::SameLine(0.0f, 18.0f * s);
					Swatch(Col(colors[i]));
					ImGui::TextColored(Theme::TextDim, "%s", names[i]);
					ImGui::SameLine();
					ImGui::PushFont(UI::MonoFont());
					ImGui::Text("%.2f ms", parts[i]);
					ImGui::PopFont();
				}
			}
			ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + h + 10.0f * s));
		}

		// ---- grafico e tabelle: affiancati, o uno sopra l'altro se il pannello e' stretto
		const float tablesW = 470.0f * s;
		const float lines = ImGui::GetTextLineHeightWithSpacing();
		if (ImGui::GetContentRegionAvail().x >= tablesW + 360.0f * s) {
			const float plotH = std::clamp(ImGui::GetContentRegionAvail().y - 3.0f * lines - 30.0f * s, 60.0f * s, 280.0f * s);
			if (ImGui::BeginTable("##profiler", 2, ImGuiTableFlags_SizingStretchProp)) {
				ImGui::TableSetupColumn("graph", ImGuiTableColumnFlags_WidthStretch);
				ImGui::TableSetupColumn("passes", ImGuiTableColumnFlags_WidthFixed, tablesW);
				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				DrawProfilerGraph(range, plotH);
				ImGui::TableSetColumnIndex(1);
				DrawProfilerTables(range);
				ImGui::EndTable();
			}
		}
		else {
			DrawProfilerGraph(range, std::clamp(ImGui::GetContentRegionAvail().y * 0.35f, 80.0f * s, 220.0f * s));
			ImGui::Spacing();
			DrawProfilerTables(range);
		}
		ImGui::PopStyleVar();
		ImGui::End();
	}

	// Il grafico: una barra per frame, le passate GPU impilate dentro il frame della CPU. Con piu'
	// frame che pixel ogni colonna mostra il suo frame piu' lungo, cosi' uno spike resta visibile.
	// Rotella: zoom orizzontale; Ctrl: verticale; Shift o trascinamento destro/centrale: tempo.
	void Application::DrawProfilerGraph(const ProfilerRange& range, float plotH) {
		PROFILE_SCOPE("Graph");
		const float s = UI::Scale();
		const ProfileStats& stats = m_ProfilerStats->Stats;
		const std::vector<const FrameRecord*>& frames = range.Frames;
		ImDrawList* list = ImGui::GetWindowDrawList();
		ImGuiIO& io = ImGui::GetIO();

		ImGui::PushFont(UI::BoldFont());
		ImGui::TextUnformatted("Frame time");
		ImGui::PopFont();
		ImGui::SameLine();
		// con l'altezza fissata a mano, al posto dell'aiuto c'e' il pulsante che la rimette automatica
		if (m_ProfilerYMax > 0.0f) {
			if (UI::Pill("Auto height"))
				m_ProfilerYMax = 0.0f;
		}
		else
			ImGui::TextDisabled("wheel zoom \xc2\xb7 ctrl+wheel height \xc2\xb7 right-drag pan \xc2\xb7 click to pin");

		const float avail = ImGui::GetContentRegionAvail().x;
		const float axisW = 34.0f * s;
		const float labelH = ImGui::GetTextLineHeight();
		const ImVec2 origin = ImGui::GetCursorScreenPos();
		const ImVec2 plotMin(origin.x + axisW, origin.y + 8.0f * s);
		const ImVec2 plotMax(origin.x + avail - 2.0f * s, plotMin.y + plotH);
		const float W = std::max(plotMax.x - plotMin.x, 1.0f);
		ImGui::InvisibleButton("##graph", ImVec2(avail, plotH + 8.0f * s + labelH + 6.0f * s),
		                       ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_MouseButtonMiddle);
		const bool hovered = ImGui::IsItemHovered();
		const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
		const bool dragging = ImGui::IsItemActive() && (ImGui::IsMouseDown(ImGuiMouseButton_Right) || ImGui::IsMouseDown(ImGuiMouseButton_Middle));
		ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY); // la rotella zooma, non scorre la finestra

		const float autoMax = std::max(2.0f, (stats.Cpu.empty() ? 0.0f : stats.Cpu[0].Sum.P99) * 1.25f); // riga 0: Frame
		const float yMax = m_ProfilerYMax > 0.0f ? m_ProfilerYMax : autoMax;
		auto yOf = [&](float ms) { return plotMax.y - std::min(ms, yMax) / yMax * plotH; };
		const double slots = static_cast<double>(range.Slots);
		const double firstSlot = slots - static_cast<double>(frames.size());
		auto xOf = [&](double index) { return plotMin.x + static_cast<float>((firstSlot + index) * W / slots); };

		// ---- navigazione: agisce sul frame dopo, quando il range si ricalcola
		auto moveEnd = [&](double end) {
			if (end >= static_cast<double>(range.Last)) {
				m_ProfilerLive = true;
				return;
			}
			m_ProfilerLive = false;
			m_ProfilerEnd = end;
		};
		const double shown = static_cast<double>(frames.size());
		if (hovered && io.MouseWheel != 0.0f) {
			const float wheel = io.MouseWheel;
			if (io.KeyCtrl)
				m_ProfilerYMax = std::clamp(yMax * std::pow(0.8f, wheel), 0.05f, 100000.0f);
			else if (io.KeyShift)
				moveEnd(static_cast<double>(range.End) - wheel * 0.1 * shown);
			else {
				// zoom attorno al mouse; dal vivo il bordo destro resta il presente
				const double t = std::clamp((io.MousePos.x - plotMin.x) / W, 0.0f, 1.0f);
				const double newSpan = std::clamp(std::min(m_ProfilerSpan, shown) * std::pow(0.8, wheel), MIN_SPAN, std::max(static_cast<double>(range.Last - range.First), MIN_SPAN));
				if (!m_ProfilerLive) {
					const double anchor = static_cast<double>(range.End) - (1.0 - t) * shown;
					moveEnd(anchor + (1.0 - t) * newSpan);
				}
				m_ProfilerSpan = newSpan;
				m_ProfilerStatsDirty = true;
			}
		}
		if (dragging && io.MouseDelta.x != 0.0f)
			moveEnd((m_ProfilerLive ? static_cast<double>(range.End) : m_ProfilerEnd) - io.MouseDelta.x / W * shown);

		// ---- le barre: una per frame, o una per colonna di pixel col frame piu' lungo
		struct Bar { size_t Index; float X0, X1; };
		std::vector<Bar> bars;
		const float barW = static_cast<float>(W / slots);
		if (barW >= 1.0f) {
			const float gap = barW >= 3.0f ? 1.0f : 0.0f;
			for (size_t i = 0; i < frames.size(); i++) {
				const float x0 = xOf(static_cast<double>(i));
				bars.push_back({ i, x0, std::max(x0 + 1.0f, x0 + barW - gap) });
			}
		}
		else {
			const int columns = static_cast<int>(W);
			for (int c = 0; c < columns; c++) {
				const double a = c * slots / W - firstSlot, b = (c + 1) * slots / W - firstSlot;
				const size_t i0 = static_cast<size_t>(std::max(0.0, std::ceil(a)));
				const size_t i1 = static_cast<size_t>(std::clamp(std::ceil(b), 0.0, shown));
				if (i0 >= i1)
					continue;
				size_t worst = i0;
				for (size_t i = i0 + 1; i < i1; i++)
					if (frames[i]->FrameMs > frames[worst]->FrameMs)
						worst = i;
				const float x0 = plotMin.x + static_cast<float>(c);
				bars.push_back({ worst, x0, x0 + 1.0f });
			}
		}
		int hoveredBar = -1;
		if (hovered && !dragging)
			for (size_t b = 0; b < bars.size(); b++)
				if (io.MousePos.x >= bars[b].X0 && io.MousePos.x < std::max(bars[b].X1, bars[b].X0 + barW)) {
					hoveredBar = static_cast<int>(b);
					break;
				}

		ImGui::PushFont(UI::MonoFont());
		const float step = NiceStep(yMax);
		for (float v = 0.0f; v <= yMax + 1e-3f; v += step) {
			const float y = yOf(v);
			list->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), Col(Theme::Header));
			char tick[16];
			snprintf(tick, sizeof(tick), "%g", v);
			const ImVec2 size = ImGui::CalcTextSize(tick);
			list->AddText(ImVec2(plotMin.x - 6.0f * s - size.x, y - size.y * 0.5f), Col(Theme::TextDim), tick);
		}
		const int64_t pinnedIndex = m_ProfilerPinned >= 0 && static_cast<uint64_t>(m_ProfilerPinned) >= range.Begin && static_cast<uint64_t>(m_ProfilerPinned) < range.End
		                          ? m_ProfilerPinned - static_cast<int64_t>(range.Begin) : -1;
		list->PushClipRect(ImVec2(plotMin.x, plotMin.y - 1.0f), ImVec2(plotMax.x, plotMax.y + 1.0f), true);
		for (size_t b = 0; b < bars.size(); b++) {
			const FrameRecord& f = *frames[bars[b].Index];
			const float x0 = bars[b].X0, x1 = bars[b].X1;
			list->AddRectFilled(ImVec2(x0, yOf(f.FrameMs)), ImVec2(x1, plotMax.y), Col(GHOST));
			float bottom = plotMax.y, stacked = 0.0f;
			for (const GpuTiming& timing : f.Gpu) {
				if (timing.Depth != 0)
					continue;
				stacked += timing.Ms;
				const float top = yOf(stacked);
				list->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom), PassColor(timing.Name));
				bottom = top;
			}
			if (static_cast<int>(b) == hoveredBar)
				list->AddRectFilled(ImVec2(x0, plotMin.y), ImVec2(x1, plotMax.y), Col(Theme::Rgb(0xffffff, 0.10f)));
		}
		if (pinnedIndex >= 0) {
			const float x0 = std::floor(xOf(static_cast<double>(pinnedIndex)));
			const float x1 = std::max(x0 + 1.0f, xOf(static_cast<double>(pinnedIndex) + 1.0) - (barW >= 3.0f ? 1.0f : 0.0f));
			list->AddRect(ImVec2(x0 - 1.0f, plotMin.y), ImVec2(x1 + 1.0f, plotMax.y), Col(Theme::SelectedText));
		}
		list->PopClipRect();
		list->AddLine(ImVec2(plotMin.x, plotMax.y), ImVec2(plotMax.x, plotMax.y), Col(Theme::Border));

		// ---- l'asse del tempo: quanto prima di adesso, contando le durate dei frame da destra
		{
			const float stepMs = NiceStep(static_cast<float>(std::max(range.DurationMs, 1.0)) * 0.66f);
			double ago = range.AgoMs;
			double next = std::ceil(ago / stepMs) * stepMs;
			float lastLabelX = plotMax.x + 1000.0f;
			for (size_t i = frames.size(); i-- > 0 && next <= range.AgoMs + range.DurationMs + 1e-6;) {
				const double frameEnd = ago + frames[i]->FrameMs;
				for (; next <= frameEnd; next += stepMs) {
					const float x = xOf(static_cast<double>(i) + 1.0 - (next - ago) / std::max(static_cast<double>(frames[i]->FrameMs), 1e-6));
					char text[32];
					if (next < 0.5)
						snprintf(text, sizeof(text), "now");
					else if (next >= 1000.0)
						snprintf(text, sizeof(text), "-%g s", next / 1000.0);
					else
						snprintf(text, sizeof(text), "-%g ms", next);
					const float tw = ImGui::CalcTextSize(text).x;
					if (x + tw * 0.5f < lastLabelX - 10.0f * s && x - tw * 0.5f >= plotMin.x - axisW) {
						list->AddLine(ImVec2(x, plotMax.y), ImVec2(x, plotMax.y + 4.0f * s), Col(Theme::Border));
						list->AddText(ImVec2(std::min(x - tw * 0.5f, plotMax.x - tw), plotMax.y + 5.0f * s), Col(Theme::TextDim), text);
						lastLabelX = x - tw * 0.5f;
					}
				}
				ago = frameEnd;
			}
		}

		// i riferimenti: 60 Hz e il refresh del display, se e' diverso
		const float refresh = DisplayRefreshHz();
		auto refLine = [&](float ms, const char* text, ImU32 color) {
			if (ms > yMax)
				return;
			const float y = yOf(ms);
			list->AddLine(ImVec2(plotMin.x, y), ImVec2(plotMax.x, y), color);
			const ImVec2 size = ImGui::CalcTextSize(text);
			const ImVec2 at(plotMax.x - size.x - 8.0f * s, y - size.y - 2.0f * s);
			list->AddRectFilled(ImVec2(at.x - 4.0f * s, at.y), ImVec2(at.x + size.x + 4.0f * s, at.y + size.y), Col(Theme::Panel));
			list->AddText(at, color, text);
		};
		char refText[64];
		if (std::abs(refresh - 60.0f) > 0.5f) {
			refLine(1000.0f / 60.0f, "60 Hz \xc2\xb7 16.7 ms", Col(Theme::TextDim));
			snprintf(refText, sizeof(refText), "display %.0f Hz \xc2\xb7 %.1f ms", refresh, 1000.0f / refresh);
		}
		else
			snprintf(refText, sizeof(refText), "display 60 Hz \xc2\xb7 16.7 ms");
		refLine(1000.0f / refresh, refText, Col(Theme::Accent));
		if (m_ProfilerReferenceFrameMs > 0.0f && m_ProfilerReferenceFrameMs <= yMax) {
			const float y = yOf(m_ProfilerReferenceFrameMs);
			for (float x = plotMin.x; x < plotMax.x; x += 8.0f * s)
				list->AddLine(ImVec2(x, y), ImVec2(std::min(x + 4.0f * s, plotMax.x), y), Col(Theme::IconSky), 1.5f);
			char text[48];
			snprintf(text, sizeof(text), "reference \xc2\xb7 %.2f ms", m_ProfilerReferenceFrameMs);
			list->AddText(ImVec2(plotMin.x + 6.0f * s, y - ImGui::GetTextLineHeight() - 2.0f * s), Col(Theme::IconSky), text);
		}
		ImGui::PopFont();

		if (hoveredBar >= 0) {
			const size_t index = bars[hoveredBar].Index;
			const FrameRecord& f = *frames[index];
			const uint64_t number = range.Begin + index;
			double ago = range.AgoMs;
			for (size_t i = index + 1; i < frames.size(); i++)
				ago += frames[i]->FrameMs;
			ImGui::BeginTooltip();
			ImGui::PushFont(UI::BoldFont());
			ImGui::Text("Frame %lld", static_cast<long long>(number) - static_cast<long long>(range.Last) + 1);
			ImGui::PopFont();
			ImGui::SameLine(160.0f * s);
			ImGui::PushFont(UI::MonoFont());
			ImGui::Text("%.2f ms", f.FrameMs);
			ImGui::PopFont();
			ImGui::TextColored(Theme::TextDim, "%s ago%s", FormatDuration(ago).c_str(), barW < 1.0f ? " \xc2\xb7 longest in this column" : "");
			auto row = [&](ImU32 color, const char* name, float ms) {
				Swatch(color);
				ImGui::TextColored(Theme::TextDim, "%s", name);
				ImGui::SameLine(160.0f * s);
				ImGui::PushFont(UI::MonoFont());
				ImGui::Text("%.2f", ms);
				ImGui::PopFont();
			};
			for (const GpuTiming& timing : f.Gpu)
				if (timing.Depth == 0)
					row(PassColor(timing.Name), timing.Name.c_str(), timing.Ms);
			ImGui::Separator();
			row(Col(CPU_WORK), "CPU work", f.CpuWorkMs());
			row(Col(WAIT_GPU), "Waiting for GPU", f.GpuWaitMs());
			row(Col(WAIT_PRESENT), "Waiting for present", f.PresentWaitMs());
			ImGui::EndTooltip();
			if (clicked) {
				// fissare un frame stacca la vista dal presente: non scorre via mentre lo si legge
				m_ProfilerPinned = static_cast<int64_t>(number);
				if (m_ProfilerLive) {
					m_ProfilerLive = false;
					m_ProfilerEnd = static_cast<double>(range.End);
				}
			}
		}

		// la legenda
		ImGui::SetCursorScreenPos(ImVec2(plotMin.x, plotMax.y + labelH + 14.0f * s));
		bool first = true;
		for (const PassKey& key : stats.Keys) {
			if (key.Depth != 0)
				continue;
			if (!first)
				ImGui::SameLine(0.0f, 16.0f * s);
			first = false;
			Swatch(PassColor(key.Name));
			ImGui::TextColored(Theme::TextDim, "%s", key.Name.c_str());
		}
		ImGui::SameLine(0.0f, 16.0f * s);
		Swatch(Col(GHOST));
		ImGui::TextColored(Theme::TextDim, "CPU frame");
	}

	// Le tabelle delle passate GPU e degli scope CPU sul range visibile; col frame fissato la
	// colonna centrale mostra i suoi valori
	void Application::DrawProfilerTables(const ProfilerRange& range) {
		std::optional<CpuScope> tableScope(std::in_place, "GPU table");
		const float s = UI::Scale();
		const ProfileStats& stats = m_ProfilerStats->Stats;
		const FrameRecord* pinnedFrame = range.Pinned;
		const bool pinned = pinnedFrame != nullptr;
		const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Theme::Header);
		ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, Theme::Rgb(0x26262b));
		const bool compare = m_ProfilerReferenceFrameMs > 0.0f;
		if (ImGui::BeginTable("##gpu", compare ? 7 : 6, flags)) {
			const float numW = 50.0f * s;
			ImGui::TableSetupColumn("GPU pass", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("min", ImGuiTableColumnFlags_WidthFixed, numW);
			ImGui::TableSetupColumn(pinned ? "frame" : "avg", ImGuiTableColumnFlags_WidthFixed, numW + 4.0f * s);
			ImGui::TableSetupColumn("p99", ImGuiTableColumnFlags_WidthFixed, numW);
			ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthFixed, numW);
			ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 40.0f * s);
			if (compare)
				ImGui::TableSetupColumn("vs ref", ImGuiTableColumnFlags_WidthFixed, 54.0f * s);
			ImGui::TableHeadersRow();

			const float frameRef = pinned ? pinnedFrame->FrameMs : stats.Diag.FrameMs;
			for (size_t k = 0; k < stats.Keys.size(); k++) {
				const PassKey& key = stats.Keys[k];
				const Summary& sum = stats.Gpu[k].Sum;
				const float shown = pinned ? PassMs(*pinnedFrame, key) : sum.Avg;

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				if (key.Depth == 0) {
					Swatch(PassColor(key.Name));
					ImGui::TextUnformatted(key.Name.c_str());
				}
				else {
					ImGui::Indent(18.0f * s * key.Depth);
					ImGui::TextColored(Theme::TextDim, "%s", key.Name.c_str());
					ImGui::Unindent(18.0f * s * key.Depth);
				}
				ImGui::PushFont(UI::MonoFont());
				const float cells[4] = { sum.Min, shown, sum.P99, sum.Max };
				for (int c = 0; c < 4; c++) {
					ImGui::TableSetColumnIndex(c + 1);
					char text[16];
					snprintf(text, sizeof(text), "%.2f", cells[c]);
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(text).x);
					if (c == 1)
						ImGui::TextUnformatted(text);
					else
						ImGui::TextColored(Theme::TextDim, "%s", text);
				}
				ImGui::TableSetColumnIndex(5);
				const std::string pct = Percent(shown, frameRef);
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(pct.c_str()).x);
				ImGui::TextColored(Theme::TextDim, "%s", pct.c_str());
				if (compare) {
					ImGui::TableSetColumnIndex(6);
					DeltaCell(m_ProfilerReference, GpuKey(key), shown);
				}
				ImGui::PopFont();
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor(2);
		ImGui::Spacing();
		if (pinned)
			ImGui::TextWrapped("Values are the pinned frame; min, p99 and max stay over the %zu visible frames. Live or Esc go back to the present.", stats.Frames);
		else
			ImGui::TextDisabled(compare ? "Averages over the %zu visible frames; vs ref: orange slower, blue faster." : "Averages over the %zu visible frames. Click a bar to pin a frame.", stats.Frames);

		// ---- il throughput: le ray query contate dallo shader sul tempo del path tracer
		{
			double rays = 0.0, pixels = 0.0, ptMs = 0.0;
			size_t counted = 0;
			if (pinned) {
				if (pinnedFrame->Rays > 0) {
					rays = pinnedFrame->Rays;
					pixels = pinnedFrame->Pixels;
					ptMs = pinnedFrame->GpuMs("Path tracer");
					counted = 1;
				}
			}
			else {
				rays = stats.Rays;
				pixels = stats.Pixels;
				ptMs = stats.PathTracerMs;
				counted = stats.CountedFrames;
			}
			ImGui::Spacing();
			ImGui::PushFont(UI::BoldFont());
			ImGui::TextUnformatted("Throughput");
			ImGui::PopFont();
			ImGui::SameLine();
			if (counted == 0 || ptMs <= 0.0)
				ImGui::TextDisabled("counting rays...");
			else {
				char text[96];
				snprintf(text, sizeof(text), "%.0f", rays / ptMs / 1000.0);
				ImGui::PushFont(UI::MonoFont());
				ImGui::TextUnformatted(text);
				ImGui::PopFont();
				ImGui::SameLine(0.0f, 4.0f * s);
				ImGui::TextColored(Theme::TextDim, "Mrays/s \xc2\xb7");
				ImGui::SameLine();
				snprintf(text, sizeof(text), "%.2f M", rays / counted / 1e6);
				ImGui::PushFont(UI::MonoFont());
				ImGui::TextUnformatted(text);
				ImGui::PopFont();
				ImGui::SameLine(0.0f, 4.0f * s);
				ImGui::TextColored(Theme::TextDim, "rays/frame \xc2\xb7");
				ImGui::SameLine();
				snprintf(text, sizeof(text), "%.2f", rays / pixels);
				ImGui::PushFont(UI::MonoFont());
				ImGui::TextUnformatted(text);
				ImGui::PopFont();
				ImGui::SameLine(0.0f, 4.0f * s);
				ImGui::TextColored(Theme::TextDim, "rays/pixel");
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Every ray query of the path tracer: camera rays, bounces and NEE shadow rays");
			}
		}

		// ---- la tabella degli scope CPU, ad albero
		tableScope.emplace("CPU table");
		ImGui::Spacing();
		const std::vector<CpuRow>& rows = stats.Rows;
		std::vector<std::vector<float>> pinnedSeries;
		if (pinned)
			pinnedSeries = CpuSeries(rows, { pinnedFrame });
		ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Theme::Header);
		ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, Theme::Rgb(0x26262b));
		if (ImGui::BeginTable("##cpu", compare ? 5 : 4, flags | ImGuiTableFlags_ScrollY, ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y, 80.0f * s)))) {
			ImGui::TableSetupScrollFreeze(0, 1);
			ImGui::TableSetupColumn("CPU scope", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn(pinned ? "frame" : "avg", ImGuiTableColumnFlags_WidthFixed, 64.0f * s);
			ImGui::TableSetupColumn("p99", ImGuiTableColumnFlags_WidthFixed, 64.0f * s);
			ImGui::TableSetupColumn("% frame", ImGuiTableColumnFlags_WidthFixed, 64.0f * s);
			if (compare)
				ImGui::TableSetupColumn("vs ref", ImGuiTableColumnFlags_WidthFixed, 54.0f * s);
			ImGui::TableHeadersRow();

			int collapsedDepth = -1; // i discendenti di una riga chiusa non si mostrano
			for (size_t r = 0; r < rows.size(); r++) {
				const CpuRow& row = rows[r];
				if (collapsedDepth >= 0 && row.Depth > collapsedDepth)
					continue;
				collapsedDepth = -1;
				const bool parent = r + 1 < rows.size() && rows[r + 1].Depth > row.Depth;
				const bool collapsed = parent && m_ProfilerCollapsed.count(row.Key) > 0;
				if (collapsed)
					collapsedDepth = row.Depth;

				const Summary& sum = stats.Cpu[r].Sum;
				const float shown = pinned ? pinnedSeries[r][0] : sum.Avg;
				const float frameRef = pinned ? pinnedSeries[0][0] : stats.Diag.FrameMs;

				ImGui::TableNextRow();
				ImGui::TableSetColumnIndex(0);
				ImGui::PushID(row.Key.c_str());
				ImGui::Indent(16.0f * s * row.Depth);
				const ImVec4 tone = row.Depth <= 1 ? Theme::Text : Theme::TextDim;
				if (parent) {
					const std::string label = std::string(collapsed ? ICON_CHEVRON_RIGHT " " : ICON_CHEVRON_DOWN " ") + row.Name;
					ImGui::PushStyleColor(ImGuiCol_Text, tone);
					if (ImGui::Selectable(label.c_str(), false, ImGuiSelectableFlags_SpanAllColumns)) {
						if (collapsed)
							m_ProfilerCollapsed.erase(row.Key);
						else
							m_ProfilerCollapsed.insert(row.Key);
					}
					ImGui::PopStyleColor();
				}
				else {
					ImGui::Dummy(ImVec2(ImGui::CalcTextSize(ICON_CHEVRON_DOWN " ").x, 0.0f));
					ImGui::SameLine(0.0f, 0.0f);
					ImGui::TextColored(row.Id == RowUntracked ? Theme::TextDim : tone, "%s", row.Name.c_str());
				}
				ImGui::Unindent(16.0f * s * row.Depth);
				ImGui::PopID();

				ImGui::PushFont(UI::MonoFont());
				char text[3][24];
				snprintf(text[0], sizeof(text[0]), "%.2f", shown);
				snprintf(text[1], sizeof(text[1]), "%.2f", sum.P99);
				snprintf(text[2], sizeof(text[2]), "%s", Percent(shown, frameRef).c_str());
				for (int c = 0; c < 3; c++) {
					ImGui::TableSetColumnIndex(c + 1);
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetColumnWidth() - ImGui::CalcTextSize(text[c]).x);
					if (c == 0)
						ImGui::TextUnformatted(text[c]);
					else
						ImGui::TextColored(Theme::TextDim, "%s", text[c]);
				}
				if (compare) {
					ImGui::TableSetColumnIndex(4);
					DeltaCell(m_ProfilerReference, CpuKey(row), shown);
				}
				ImGui::PopFont();
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleColor(2);
	}
}
