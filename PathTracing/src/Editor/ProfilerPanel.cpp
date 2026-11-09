// Il pannello Profiler (issue #31, ADR 0023): la diagnosi del frame, il grafico dei tempi e le
// passate GPU. Legge il Profiler globale; l'unica cosa che ci scrive e' la pausa.

#include "Editor/Application.h"
#include "Editor/Icons.h"
#include "Editor/Widgets.h"
#include "Core/Profiler.h"

#include "imgui_internal.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>

namespace Lucerna {

	namespace {
		constexpr const char* PROFILER = "Profiler";
		constexpr const char* VIEWPORT = "Viewport";
		constexpr int WINDOWS[] = { 120, 300, 1000 }; // le lunghezze dello storico tra cui scegliere

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
					scopes.insert(at, { scope.Name, scope.Depth + 1, static_cast<int>(scope.Id), profiler.ScopePath(scope.Id) });
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

		Profiler& profiler = Profiler::Get();
		const int window = WINDOWS[m_ProfilerWindow];
		const std::vector<const FrameRecord*> frames = LastFrames(window);
		if (m_ProfilerPinned >= static_cast<int>(frames.size()))
			m_ProfilerPinned = -1;
		auto resume = [&] { profiler.SetPaused(false); m_ProfilerPinned = -1; };
		if (profiler.Paused() && m_ProfilerFocused && ImGui::IsKeyPressed(ImGuiKey_Escape))
			resume();

		// ---- toolbar: le tab (Memory e Loading arrivano con #34 e #35), pausa, finestra
		UI::Pill("Frame", true);
		if (profiler.Paused()) {
			ImGui::SameLine();
			ImGui::AlignTextToFramePadding();
			if (m_ProfilerPinned >= 0)
				ImGui::TextColored(Theme::SelectedText, ICON_PAUSE " Paused \xc2\xb7 frame %d pinned", m_ProfilerPinned - static_cast<int>(frames.size()) + 1);
			else
				ImGui::TextColored(Theme::SelectedText, ICON_PAUSE " Paused");
		}
		const char* pauseLabel = profiler.Paused() ? ICON_PLAY " Resume" : ICON_PAUSE " Pause";
		const float comboW = 120.0f * s;
		const float pauseW = ImGui::CalcTextSize(pauseLabel).x + ImGui::GetStyle().FramePadding.x * 2;
		ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - comboW - pauseW - ImGui::GetStyle().ItemSpacing.x);
		if (UI::Pill(pauseLabel, profiler.Paused())) {
			if (profiler.Paused())
				resume();
			else
				profiler.SetPaused(true);
		}
		ImGui::SameLine();
		ImGui::SetNextItemWidth(comboW);
		ImGui::Combo("##window", &m_ProfilerWindow, "120 frames\0" "300 frames\0" "1000 frames\0");
		ImGui::Separator();

		if (frames.empty()) {
			ImGui::TextDisabled("Collecting frames...");
			ImGui::End();
			return;
		}

		std::optional<CpuScope> diagnosisScope(std::in_place, "Diagnosis");
		const Diagnosis diagnosis = Diagnose(frames);
		diagnosisScope.reset();
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
			snprintf(fpsText, sizeof(fpsText), "%.0f fps \xc2\xb7 avg of %zu", 1000.0f / diagnosis.FrameMs, frames.size());
			ImGui::PushFont(UI::MonoFont());
			const float rightW = std::max(ImGui::CalcTextSize(frameText).x, ImGui::CalcTextSize(fpsText).x);
			const float rightX = p.x + w - 14.0f * s - rightW;
			list->AddText(ImVec2(p.x + w - 14.0f * s - ImGui::CalcTextSize(frameText).x, p.y + 9.0f * s), Col(Theme::Text), frameText);
			list->AddText(ImVec2(p.x + w - 14.0f * s - ImGui::CalcTextSize(fpsText).x, p.y + 28.0f * s), Col(Theme::TextDim), fpsText);
			ImGui::PopFont();

			// in mezzo: il frame diviso in lavoro della CPU, attesa della GPU e attesa del display
			const float barX0 = labelEnd + 28.0f * s, barX1 = rightX - 24.0f * s;
			if (barX1 - barX0 > 120.0f * s) {
				const float total = std::max(diagnosis.CpuMs + diagnosis.GpuWaitMs + diagnosis.PresentWaitMs, 1e-3f);
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
					const float x1 = x + (barX1 - barX0) * parts[i] / total;
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

		// Dopo la toolbar: Resume ed Esc tolgono il frame fissato in questo stesso frame
		const bool pinned = m_ProfilerPinned >= 0;
		const std::vector<PassKey> keys = PassKeys(frames);
		const FrameRecord* pinnedFrame = pinned ? frames[m_ProfilerPinned] : nullptr;

		if (ImGui::BeginTable("##profiler", 2, ImGuiTableFlags_SizingStretchProp)) {
			ImGui::TableSetupColumn("graph", ImGuiTableColumnFlags_WidthStretch);
			ImGui::TableSetupColumn("passes", ImGuiTableColumnFlags_WidthFixed, 470.0f * s);
			ImGui::TableNextRow();

			// ---- il grafico: una barra per frame, le passate GPU impilate dentro il frame della CPU
			ImGui::TableSetColumnIndex(0);
			{
				PROFILE_SCOPE("Graph");
				ImGui::PushFont(UI::BoldFont());
				ImGui::TextUnformatted("Frame time");
				ImGui::PopFont();
				ImGui::SameLine();
				ImGui::TextDisabled("hover to inspect, click to pin");

				// il grafico prende l'altezza che c'e', tolta la legenda sotto
				const float avail = ImGui::GetContentRegionAvail().x;
				// il grafico prende l'altezza che c'e', tolta la legenda sotto
				const float legendH = ImGui::GetTextLineHeightWithSpacing() + 12.0f * s;
				const float axisW = 34.0f * s;
				const float plotH = std::clamp(ImGui::GetContentRegionAvail().y - legendH - 16.0f * s, 60.0f * s, 280.0f * s);
				const ImVec2 origin = ImGui::GetCursorScreenPos();
				const ImVec2 plotMin(origin.x + axisW, origin.y + 8.0f * s);
				const ImVec2 plotMax(origin.x + avail - 2.0f * s, plotMin.y + plotH);
				ImGui::InvisibleButton("##graph", ImVec2(avail, plotH + 16.0f * s));
				const bool hovered = ImGui::IsItemHovered();
				const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

				std::vector<float> frameMs;
				for (const FrameRecord* f : frames)
					frameMs.push_back(f->FrameMs);
				const float refresh = DisplayRefreshHz();
				// il p99 e non il massimo: il primo frame dopo un caricamento dura secondi. Le linee
				// di 60 Hz e del display compaiono quando i frame ci arrivano vicino.
				const float yMax = std::max(2.0f, Summarize(frameMs).P99 * 1.25f);
				auto yOf = [&](float ms) { return plotMax.y - std::min(ms, yMax) / yMax * plotH; };

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

				const float barW = (plotMax.x - plotMin.x) / static_cast<float>(window);
				const int firstSlot = window - static_cast<int>(frames.size());
				const float gap = barW >= 3.0f ? 1.0f : 0.0f;
				int hoveredIndex = -1;
				if (hovered) {
					const int slot = static_cast<int>((ImGui::GetIO().MousePos.x - plotMin.x) / barW);
					if (slot >= firstSlot && slot < window)
						hoveredIndex = slot - firstSlot;
				}
				for (size_t i = 0; i < frames.size(); i++) {
					const FrameRecord& f = *frames[i];
					const float x0 = plotMin.x + (firstSlot + static_cast<float>(i)) * barW;
					const float x1 = std::max(x0 + 1.0f, x0 + barW - gap);
					list->AddRectFilled(ImVec2(x0, yOf(f.FrameMs)), ImVec2(x1, plotMax.y), Col(GHOST));
					float bottom = plotMax.y;
					float stacked = 0.0f;
					for (const GpuTiming& timing : f.Gpu) {
						if (timing.Depth != 0)
							continue;
						stacked += timing.Ms;
						const float top = yOf(stacked);
						list->AddRectFilled(ImVec2(x0, top), ImVec2(x1, bottom), PassColor(timing.Name));
						bottom = top;
					}
					if (static_cast<int>(i) == hoveredIndex)
						list->AddRectFilled(ImVec2(x0, plotMin.y), ImVec2(x1, plotMax.y), Col(Theme::Rgb(0xffffff, 0.10f)));
					if (static_cast<int>(i) == m_ProfilerPinned)
						list->AddRect(ImVec2(x0 - 1.0f, plotMin.y), ImVec2(x1 + 1.0f, plotMax.y), Col(Theme::SelectedText));
				}
				list->AddLine(ImVec2(plotMin.x, plotMax.y), ImVec2(plotMax.x, plotMax.y), Col(Theme::Border));

				// i riferimenti: 60 Hz e il refresh del display, se e' diverso
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
				ImGui::PopFont();

				if (hoveredIndex >= 0) {
					const FrameRecord& f = *frames[hoveredIndex];
					ImGui::BeginTooltip();
					ImGui::PushFont(UI::BoldFont());
					ImGui::Text("Frame %d", hoveredIndex - static_cast<int>(frames.size()) + 1);
					ImGui::PopFont();
					ImGui::SameLine(160.0f * s);
					ImGui::PushFont(UI::MonoFont());
					ImGui::Text("%.2f ms", f.FrameMs);
					ImGui::PopFont();
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
						m_ProfilerPinned = hoveredIndex;
						profiler.SetPaused(true);
					}
				}

				// la legenda
				ImGui::SetCursorScreenPos(ImVec2(plotMin.x, plotMax.y + 10.0f * s));
				bool first = true;
				for (const PassKey& key : keys) {
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

			// ---- la tabella delle passate GPU
			ImGui::TableSetColumnIndex(1);
			{
				std::optional<CpuScope> tableScope(std::in_place, "GPU table");
				const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit;
				ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Theme::Header);
				ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, Theme::Rgb(0x26262b));
				if (ImGui::BeginTable("##gpu", 6, flags)) {
					const float numW = 50.0f * s;
					ImGui::TableSetupColumn("GPU pass", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn("min", ImGuiTableColumnFlags_WidthFixed, numW);
					ImGui::TableSetupColumn(pinned ? "frame" : "avg", ImGuiTableColumnFlags_WidthFixed, numW + 4.0f * s);
					ImGui::TableSetupColumn("p99", ImGuiTableColumnFlags_WidthFixed, numW);
					ImGui::TableSetupColumn("max", ImGuiTableColumnFlags_WidthFixed, numW);
					ImGui::TableSetupColumn("%", ImGuiTableColumnFlags_WidthFixed, 40.0f * s);
					ImGui::TableHeadersRow();

					const float frameRef = pinned ? pinnedFrame->FrameMs : diagnosis.FrameMs;
					for (const PassKey& key : keys) {
						std::vector<float> values;
						for (const FrameRecord* f : frames)
							values.push_back(PassMs(*f, key));
						const Summary sum = Summarize(values);
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
						ImGui::PopFont();
					}
					ImGui::EndTable();
				}
				ImGui::PopStyleColor(2);
				ImGui::Spacing();
				if (pinned)
					ImGui::TextWrapped("Values are the pinned frame; min, p99 and max stay over the %zu-frame window. Resume or press Esc to go back to averages.", frames.size());
				else
					ImGui::TextDisabled("Averages over the last %zu frames. Click a bar to pin a frame.", frames.size());

				// ---- la tabella degli scope CPU, ad albero
				tableScope.emplace("CPU table");
				ImGui::Spacing();
				const std::vector<CpuRow> rows = CpuRows(frames);
				const std::vector<std::vector<float>> series = CpuSeries(rows, frames);
				ImGui::PushStyleColor(ImGuiCol_TableHeaderBg, Theme::Header);
				ImGui::PushStyleColor(ImGuiCol_TableRowBgAlt, Theme::Rgb(0x26262b));
				if (ImGui::BeginTable("##cpu", 4, flags | ImGuiTableFlags_ScrollY, ImVec2(0.0f, std::max(ImGui::GetContentRegionAvail().y, 80.0f * s)))) {
					ImGui::TableSetupScrollFreeze(0, 1);
					ImGui::TableSetupColumn("CPU scope", ImGuiTableColumnFlags_WidthStretch);
					ImGui::TableSetupColumn(pinned ? "frame" : "avg", ImGuiTableColumnFlags_WidthFixed, 64.0f * s);
					ImGui::TableSetupColumn("p99", ImGuiTableColumnFlags_WidthFixed, 64.0f * s);
					ImGui::TableSetupColumn("% frame", ImGuiTableColumnFlags_WidthFixed, 64.0f * s);
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

						const Summary sum = Summarize(series[r]);
						const float shown = pinned ? series[r][m_ProfilerPinned] : sum.Avg;
						const float frameRef = pinned ? series[0][m_ProfilerPinned] : diagnosis.FrameMs;

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
						ImGui::PopFont();
					}
					ImGui::EndTable();
				}
				ImGui::PopStyleColor(2);
			}
			ImGui::EndTable();
		}
		ImGui::PopStyleVar();
		ImGui::End();
	}
}
