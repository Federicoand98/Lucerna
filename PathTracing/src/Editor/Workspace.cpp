// L'area di lavoro del redesign (concept A): menu, barra di stato, il layout dei pannelli e i
// pannelli stessi. Outliner e materiali a sinistra, la viewport con la sua toolbar e l'HUD al
// centro, le proprieta' a destra con una colonna di icone che sceglie la sezione.
// I widget dei campi sono in Widgets.h, i colori in Theme.h.

#include "Editor/Application.h"
#include "Editor/Icons.h"
#include "Editor/Widgets.h"
#include "Core/Profiler.h"
#include "vendor/portable-file-dialogs.h"

#include "imgui_internal.h"
#include "imgui_impl_vulkan.h"

// Pubbliche in imgui_impl_vulkan.cpp ma assenti dall'header (ImGui 1.90): servono a RebuildUiForScale
bool ImGui_ImplVulkan_CreateDeviceObjects();
void ImGui_ImplVulkan_DestroyDeviceObjects();

namespace Lucerna {

	namespace {
		// I nomi delle finestre sono anche le chiavi del layout in imgui.ini
		constexpr const char* OUTLINER = "Outliner";
		constexpr const char* MATERIALS = "Materials";
		constexpr const char* VIEWPORT = "Viewport";
		constexpr const char* PROPERTIES = "Properties";
		constexpr const char* PROFILER = "Profiler";

		const char* const AOV_NAMES[] = { "Beauty", "Albedo", "Normal", "Depth", "History" };

		// Un riquadro dell'HUD sopra la render, in alto a sinistra (dir = 1) o a destra (dir = -1).
		// Ritorna il rettangolo, per chi lo rende cliccabile.
		ImRect HudLabel(ImDrawList* list, ImVec2 anchor, float dir, const char* text, ImU32 color, ImU32 background = 0) {
			ImGui::PushFont(UI::MonoFont());
			const float s = UI::Scale();
			const ImVec2 size = ImGui::CalcTextSize(text);
			const ImVec2 pad(8.0f * s, 4.0f * s);
			const ImVec2 min(dir > 0 ? anchor.x : anchor.x - size.x - 2 * pad.x, anchor.y);
			const ImVec2 max(min.x + size.x + 2 * pad.x, min.y + size.y + 2 * pad.y);
			list->AddRectFilled(min, max, background ? background : ImGui::GetColorU32(Theme::Hud), 4.0f * s);
			list->AddText(ImVec2(min.x + pad.x, min.y + pad.y), color, text);
			ImGui::PopFont();
			return ImRect(min, max);
		}
	}

	// ---------------------------------------------------------------- layout

	// Il layout del concept A: la prima volta che queste finestre compaiono, e da View > Reset layout.
	// Dopo, imgui.ini ricorda come l'utente le ha spostate.
	void Application::BuildDefaultLayout(ImGuiID dockspace) {
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const float s = UI::Scale();
		ImGui::DockBuilderRemoveNode(dockspace);
		ImGui::DockBuilderAddNode(dockspace, ImGuiDockNodeFlags_DockSpace);
		ImGui::DockBuilderSetNodeSize(dockspace, viewport->WorkSize);

		ImGuiID center = dockspace;
		const ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 280.0f * s / viewport->WorkSize.x, nullptr, &center);
		const ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 380.0f * s / (viewport->WorkSize.x - 280.0f * s), nullptr, &center);
		ImGuiID outliner = left;
		const ImGuiID materials = ImGui::DockBuilderSplitNode(outliner, ImGuiDir_Down, 0.28f, nullptr, &outliner);
		// il profiler sotto la viewport, a tutta larghezza; chiuso, il suo spazio torna alla viewport
		const ImGuiID profiler = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.40f, nullptr, &center);

		ImGui::DockBuilderDockWindow(OUTLINER, outliner);
		ImGui::DockBuilderDockWindow(MATERIALS, materials);
		ImGui::DockBuilderDockWindow(PROPERTIES, right);
		ImGui::DockBuilderDockWindow(VIEWPORT, center);
		ImGui::DockBuilderDockWindow(PROFILER, profiler);
		ImGui::DockBuilderFinish(dockspace);
		m_ShowOutliner = m_ShowMaterials = m_ShowProperties = true;
		m_ProfilerDockChecked = true;
	}

	// Va chiamata prima del dockspace: la barra riduce l'area di lavoro che il dockspace occupa
	void Application::DrawStatusBar() {
		const float height = ImGui::GetFrameHeight();
		const ImGuiWindowFlags flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_MenuBar;
		ImGui::PushStyleColor(ImGuiCol_MenuBarBg, Theme::Panel);
		if (ImGui::BeginViewportSideBar("##StatusBar", ImGui::GetMainViewport(), ImGuiDir_Down, height, flags)) {
			if (ImGui::BeginMenuBar()) {
				ImGui::PushStyleColor(ImGuiCol_Text, Theme::Rgb(0xb4b4bc));
				const int samples = m_Renderer.AccumulatedFrames() * m_Renderer.Settings.SamplesPerPixel;
				if (m_Renderer.View.PathTracing)
					ImGui::Text("Accumulating  %d samples", samples);
				else
					ImGui::TextUnformatted(ICON_PAUSE " Paused");
				ImGui::Spacing();
				ImGui::Text("Reprojection %s", m_Renderer.Settings.Reproject ? "on" : "off");
				ImGui::Spacing();
				if (m_Selection.IsValid())
					ImGui::Text("1 selected: %s", GetSelectionName().c_str());
				else
					ImGui::TextUnformatted("Nothing selected");
				ImGui::Spacing();
				DrawBlenderStatus();

				const std::string device = std::string("Vulkan 1.3  ray query  ") + m_Vulkan.DeviceName();
				ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(device.c_str()).x - ImGui::GetStyle().WindowPadding.x * 2);
				ImGui::TextUnformatted(device.c_str());
				ImGui::PopStyleColor();
				ImGui::EndMenuBar();
			}
		}
		ImGui::End();
		ImGui::PopStyleColor();
	}

	void Application::RenderUI(float deltaTime) {
		ValidateSelection();

		if (m_ShowOutliner) {
			PROFILE_SCOPE("Outliner");
			DrawOutlinerPanel();
		}
		if (m_ShowMaterials) {
			PROFILE_SCOPE("Materials");
			DrawMaterialsPanel();
		}
		if (m_ShowProperties) {
			PROFILE_SCOPE("Properties");
			DrawPropertiesPanel();
		}
		if (m_ShowPreferences)
			DrawPreferencesWindow();
		if (m_ShowProfiler) {
			PROFILE_SCOPE("Profiler");
			DrawProfilerPanel();
		}
		{
			PROFILE_SCOPE("Viewport");
			DrawViewportPanel(deltaTime);
		}
	}

	// ---------------------------------------------------------------- menu

	void Application::DrawEditMenu() {
		if (!ImGui::BeginMenu("Edit"))
			return;
		ImGui::TextDisabled("Undo is not available yet");
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_DELETE " Delete", "Del", false, CanDeleteSelection()))
			m_DeleteRequested = true;
		if (ImGui::MenuItem(ICON_CLOSE " Deselect", "Esc", false, m_Selection.IsValid()))
			m_Selection = {};
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_SETTINGS " Preferences..."))
			m_ShowPreferences = true;
		ImGui::EndMenu();
	}

	void Application::DrawViewMenu() {
		if (!ImGui::BeginMenu("View"))
			return;
		if (ImGui::BeginMenu(ICON_VIEW " Debug view")) {
			for (int i = 0; i < static_cast<int>(std::size(AOV_NAMES)); i++)
				if (ImGui::MenuItem(AOV_NAMES[i], nullptr, m_Renderer.View.AOVView == i))
					m_Renderer.View.AOVView = i;
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu(ICON_MOVE " Gizmo")) {
			if (ImGui::MenuItem("Move", "G", m_GizmoOperation == ImGuizmo::TRANSLATE)) m_GizmoOperation = ImGuizmo::TRANSLATE;
			if (ImGui::MenuItem("Rotate", "R", m_GizmoOperation == ImGuizmo::ROTATE)) m_GizmoOperation = ImGuizmo::ROTATE;
			if (ImGui::MenuItem("Scale", "S", m_GizmoOperation == ImGuizmo::SCALE)) m_GizmoOperation = ImGuizmo::SCALE;
			ImGui::EndMenu();
		}
		ImGui::Separator();
		ImGui::MenuItem(OUTLINER, nullptr, &m_ShowOutliner);
		ImGui::MenuItem(MATERIALS, nullptr, &m_ShowMaterials);
		ImGui::MenuItem(PROPERTIES, nullptr, &m_ShowProperties);
		ImGui::MenuItem(ICON_PROFILER " Profiler", nullptr, &m_ShowProfiler);
		ImGui::MenuItem("Viewport readouts", nullptr, &m_ShowHud);
		ImGui::MenuItem(ICON_DEBUG " Developer", nullptr, &m_ShowDeveloper);
		ImGui::Separator();
		if (ImGui::MenuItem(ICON_LAYOUT " Reset layout"))
			m_ResetLayout = true;
		ImGui::EndMenu();
	}

	void Application::DrawHelpMenu() {
		if (!ImGui::BeginMenu("Help"))
			return;
		ImGui::TextDisabled(ICON_KEYBOARD " Navigation");
		const std::pair<const char*, const char*> keys[] = {
			{ "Look and fly", "right mouse + WASD" }, { "Select", "left click" }, { "Move / rotate / scale", "G  R  S" },
			{ "Delete", "Del" }, { "Deselect", "Esc" }, { "Save", "Ctrl+S" }, { "Open", "Ctrl+O" } };
		if (ImGui::BeginTable("keys", 2, ImGuiTableFlags_SizingFixedFit)) {
			for (const auto& [action, key] : keys) {
				ImGui::TableNextColumn();
				ImGui::TextUnformatted(action);
				ImGui::TableNextColumn();
				ImGui::PushFont(UI::MonoFont());
				ImGui::TextDisabled("%s", key);
				ImGui::PopFont();
			}
			ImGui::EndTable();
		}
		ImGui::EndMenu();
	}

	void Application::DrawPreferencesWindow() {
		ImGui::SetNextWindowSize(ImVec2(360.0f * UI::Scale(), 0.0f), ImGuiCond_Appearing);
		if (ImGui::Begin(ICON_SETTINGS " Preferences###Preferences", &m_ShowPreferences, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse)) {
			if (UI::Checkbox("Vsync", &m_Vsync)) {
				m_Vulkan.SetVsync(m_Vsync);
				m_Preferences.Vsync = m_Vsync;
				m_Preferences.Save();
			}
			// la cornice si sceglie quando si crea la finestra: vale al prossimo avvio (ADR 0021)
			if (UI::Checkbox("System title bar", &m_Preferences.SystemTitleBar))
				m_Preferences.Save();
			if (m_Preferences.SystemTitleBar == m_CustomTitleBar) {
				UI::Label("");
				ImGui::TextDisabled("applies on the next start");
			}
			// font e stile si rifanno solo quando si lascia lo slider: rifarli a ogni passo lo
			// sposterebbe sotto il mouse
			UI::SliderFloat("UI scale", &m_Preferences.UiScale, Preferences::MIN_UI_SCALE, Preferences::MAX_UI_SCALE, "%.2f");
			if (ImGui::IsItemDeactivatedAfterEdit()) {
				m_Preferences.Save();
				m_RescaleRequested = true;
			}
			UI::Label("Device");
			ImGui::TextDisabled("%s", m_Vulkan.DeviceName());

			// File > Open in Blender (ADR 0024): vuoto = Lucerna lo cerca da se'
			ImGui::Spacing();
			char blender[1024];
			snprintf(blender, sizeof(blender), "%s", m_Preferences.BlenderPath.c_str());
			if (UI::InputText("Blender", blender, sizeof(blender))) {
				m_Preferences.BlenderPath = blender;
				m_Preferences.Save();
			}
			UI::Label("");
			if (UI::Pill(ICON_OPEN " Browse...")) {
				const auto chosen = pfd::open_file("Blender executable", m_Preferences.BlenderPath).result();
				if (!chosen.empty()) {
					m_Preferences.BlenderPath = chosen.front();
					m_Preferences.Save();
					m_BlenderError.clear();
				}
			}
			if (m_Preferences.BlenderPath.empty()) {
				const std::string found = BlenderLink::FindBlender("");
				UI::Label("");
				ImGui::TextDisabled(found.empty() ? "not found: set the executable" : "found: %s", found.c_str());
			}
			if (!m_BlenderError.empty()) {
				UI::Label("");
				ImGui::PushTextWrapPos(0.0f);
				ImGui::TextColored(Theme::Accent, "%s", m_BlenderError.c_str());
				ImGui::PopTextWrapPos();
			}
		}
		ImGui::End();
	}

	// ---------------------------------------------------------------- outliner

	// Una riga dell'outliner: icona, nome, e un'annotazione grigia (i triangoli di una mesh).
	// Cliccarla e' come cliccare l'oggetto nella viewport.
	bool Application::OutlinerRow(const char* icon, const ImVec4& iconColor, const std::string& name, bool selected, const char* note) {
		const float s = UI::Scale();
		ImGui::PushStyleColor(ImGuiCol_Header, Theme::Selected);
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, selected ? Theme::Selected : Theme::Header);
		ImGui::PushStyleColor(ImGuiCol_HeaderActive, Theme::Selected);
		const ImVec2 start = ImGui::GetCursorScreenPos();
		const bool clicked = ImGui::Selectable(("##row" + name).c_str(), selected, ImGuiSelectableFlags_SpanAvailWidth);
		ImGui::PopStyleColor(3);

		ImDrawList* list = ImGui::GetWindowDrawList();
		const float indent = 18.0f * s;
		const ImVec2 textPos(start.x + indent, start.y);
		list->AddText(textPos, ImGui::GetColorU32(selected ? Theme::SelectedText : iconColor), icon);
		const float iconWidth = ImGui::CalcTextSize(icon).x + 6.0f * s;
		list->AddText(ImVec2(textPos.x + iconWidth, textPos.y), ImGui::GetColorU32(selected ? Theme::SelectedText : Theme::Text), name.c_str());
		if (note != nullptr)
			list->AddText(ImVec2(textPos.x + iconWidth + ImGui::CalcTextSize(name.c_str()).x + 6.0f * s, textPos.y),
			              ImGui::GetColorU32(Theme::TextDim), note);
		return clicked;
	}

	bool Application::OutlinerGroup(const char* label, size_t count, bool defaultOpen) {
		ImGui::PushFont(UI::BoldFont());
		ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
		ImGui::PushStyleColor(ImGuiCol_HeaderHovered, Theme::Header);
		const std::string text = count > 0 ? std::string(label) + "  " + std::to_string(count) : std::string(label);
		const bool open = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoTreePushOnOpen
			| (defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0), "%s", text.c_str());
		ImGui::PopStyleColor(2);
		ImGui::PopFont();
		return open;
	}

	void Application::DrawOutlinerPanel() {
		if (!ImGui::Begin(OUTLINER, &m_ShowOutliner)) {
			ImGui::End();
			return;
		}
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 2.0f * UI::Scale()));

		auto select = [&](SelectionType type, int index) {
			m_Selection = Select(type, index);
			m_GizmoOperation = ImGuizmo::TRANSLATE;
			m_PropertiesTab = PropertiesTab::Object;
		};
		auto isSelected = [&](SelectionType type, int index) { return m_Selection.Type == type && SelectedIndex() == index; };

		// Il mondo non e' un oggetto selezionabile: le righe aprono la sua sezione delle proprieta'
		if (OutlinerGroup("World", 0, true)) {
			if (OutlinerRow(ICON_SUN, Theme::IconSun, "Sun", false, m_Scene.Sun().Enabled ? nullptr : "off"))
				m_PropertiesTab = PropertiesTab::World;
			if (OutlinerRow(ICON_SKY, Theme::IconSky, "Sky", false, m_Scene.EnvironmentMapping() ? "environment map" : "color"))
				m_PropertiesTab = PropertiesTab::World;
		}

		if (OutlinerGroup("Lights", m_Scene.PunctualLights().size(), true))
			for (int i = 0; i < static_cast<int>(m_Scene.PunctualLights().size()); i++) {
				const auto& light = m_Scene.PunctualLights()[i];
				if (OutlinerRow(light.Spot ? ICON_SPOT_LIGHT : ICON_POINT_LIGHT, Theme::Text, light.Name, isSelected(SelectionType::Light, i)))
					select(SelectionType::Light, i);
			}

		if (OutlinerGroup("Meshes", m_Scene.Meshes().size(), true))
			for (int i = 0; i < static_cast<int>(m_Scene.Meshes().size()); i++) {
				const auto& mesh = m_Scene.Meshes()[i];
				const MeshAsset* asset = m_Assets.FindMesh(mesh.Asset);
				char note[32] = "";
				if (asset != nullptr) {
					const size_t tris = asset->Triangles.size();
					tris >= 1000 ? snprintf(note, sizeof(note), "%zuk tris", tris / 1000) : snprintf(note, sizeof(note), "%zu tris", tris);
				}
				if (OutlinerRow(ICON_MESH, Theme::Text, mesh.Name, isSelected(SelectionType::Mesh, i), note))
					select(SelectionType::Mesh, i);
			}

		if (OutlinerGroup("Spheres", m_Scene.Spheres().size(), m_Scene.Spheres().size() <= 32))
			for (int i = 0; i < static_cast<int>(m_Scene.Spheres().size()); i++)
				if (OutlinerRow(ICON_SPHERE, Theme::Text, m_Scene.Spheres()[i].Name, isSelected(SelectionType::Sphere, i)))
					select(SelectionType::Sphere, i);

		// I 6 quad di un box non sono oggetti a se': si seleziona il box
		std::vector<bool> ownedByBox(m_Scene.Quads().size(), false);
		for (const Box& box : m_Scene.Boxes())
			for (int q = static_cast<int>(box.index); q < static_cast<int>(box.index) + 6 && q < static_cast<int>(ownedByBox.size()); q++)
				ownedByBox[q] = true;
		const size_t standaloneQuads = std::count(ownedByBox.begin(), ownedByBox.end(), false);
		if (OutlinerGroup("Quads and boxes", standaloneQuads + m_Scene.Boxes().size(), true)) {
			for (int i = 0; i < static_cast<int>(m_Scene.Boxes().size()); i++)
				if (OutlinerRow(ICON_BOX, Theme::Text, m_Scene.Boxes()[i].Name, isSelected(SelectionType::Box, i)))
					select(SelectionType::Box, i);
			for (int i = 0; i < static_cast<int>(m_Scene.Quads().size()); i++)
				if (!ownedByBox[i] && OutlinerRow(ICON_QUAD, Theme::Text, m_Scene.Quads()[i].Name, isSelected(SelectionType::Quad, i)))
					select(SelectionType::Quad, i);
		}

		if (OutlinerGroup("Camera", 0, true))
			if (OutlinerRow(ICON_CAMERA, Theme::Text, "Main camera", false, m_Renderer.View.LockViewToCamera ? "view locked" : nullptr))
				m_PropertiesTab = PropertiesTab::Camera;

		ImGui::PopStyleVar();
		ImGui::End();
	}

	// ---------------------------------------------------------------- materiali

	// I materiali come campioni del loro colore base (le miniature renderizzate verranno dopo).
	// Un click lo apre nella sezione Material e lo usa per i prossimi oggetti aggiunti.
	void Application::DrawMaterialsPanel() {
		if (!ImGui::Begin(MATERIALS, &m_ShowMaterials)) {
			ImGui::End();
			return;
		}
		const float s = UI::Scale();
		const int columns = 6;
		const float gap = 6.0f * s;
		const float size = std::max(12.0f * s, (ImGui::GetContentRegionAvail().x - gap * (columns - 1)) / columns);
		ImDrawList* list = ImGui::GetWindowDrawList();

		for (int i = 0; i <= static_cast<int>(m_Assets.Materials().size()); i++) {
			if (i % columns != 0)
				ImGui::SameLine(0.0f, gap);
			ImGui::PushID(i);
			const ImVec2 min = ImGui::GetCursorScreenPos();
			const ImVec2 center(min.x + size * 0.5f, min.y + size * 0.5f);
			const bool clicked = ImGui::InvisibleButton("swatch", ImVec2(size, size));
			const bool hovered = ImGui::IsItemHovered();

			if (i == static_cast<int>(m_Assets.Materials().size())) {
				// l'ultimo posto aggiunge un materiale
				list->AddCircle(center, size * 0.5f - 1.0f, ImGui::GetColorU32(hovered ? Theme::Accent : Theme::Border), 0, 1.5f * s);
				const ImVec2 plus = ImGui::CalcTextSize(ICON_ADD);
				list->AddText(ImVec2(center.x - plus.x * 0.5f, center.y - plus.y * 0.5f), ImGui::GetColorU32(Theme::TextDim), ICON_ADD);
				if (clicked) {
					m_EditedMaterial = m_Assets.AddMaterial(CreateDefaultPrincipled());
					m_PropertiesTab = PropertiesTab::Material;
				}
				if (hovered)
					ImGui::SetTooltip("Add a material");
			}
			else {
				const auto& material = m_Assets.Materials()[i];
				const glm::vec4 color = material.Color;
				list->AddCircleFilled(center, size * 0.5f - 1.0f, ImGui::GetColorU32(ImVec4(color.r, color.g, color.b, 1.0f)));
				if (material.Id == m_EditedMaterial)
					list->AddCircle(center, size * 0.5f + 1.0f * s, ImGui::GetColorU32(Theme::Accent), 0, 2.0f * s);
				else if (hovered)
					list->AddCircle(center, size * 0.5f, ImGui::GetColorU32(Theme::TextDim), 0, 1.0f * s);
				if (clicked) {
					m_EditedMaterial = material.Id;
					m_PropertiesTab = PropertiesTab::Material;
				}
				if (hovered)
					ImGui::SetTooltip("%s", material.Name.c_str());
			}
			ImGui::PopID();
		}
		ImGui::End();
	}

	// ---------------------------------------------------------------- viewport

	void Application::DrawViewportToolbar() {
		const float s = UI::Scale();
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 5.0f * s));
		ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Panel);
		ImGui::BeginChild("ViewportToolbar", ImVec2(0.0f, ImGui::GetFrameHeight() + 10.0f * s), false,
		                  ImGuiWindowFlags_AlwaysUseWindowPadding | ImGuiWindowFlags_NoScrollbar);

		const ImGuizmo::OPERATION effective = EffectiveGizmoOperation();
		if (UI::Pill(ICON_MOVE " Move", effective == ImGuizmo::TRANSLATE)) m_GizmoOperation = ImGuizmo::TRANSLATE;
		ImGui::SameLine();
		if (UI::Pill(ICON_ROTATE " Rotate", effective == ImGuizmo::ROTATE, !m_Selection.IsValid() || m_Selection.SupportsRotation()))
			m_GizmoOperation = ImGuizmo::ROTATE;
		ImGui::SameLine();
		if (UI::Pill(ICON_SCALE " Scale", effective == ImGuizmo::SCALE)) m_GizmoOperation = ImGuizmo::SCALE;

		ImGui::SameLine(0.0f, 14.0f * s);
		const std::string view = std::string(ICON_VIEW " View: ") + AOV_NAMES[std::clamp(m_Renderer.View.AOVView, 0, 4)];
		ImGui::SetNextItemWidth(ImGui::CalcTextSize(view.c_str()).x + ImGui::GetFrameHeight() + 16.0f * s);
		if (ImGui::BeginCombo("##aov", view.c_str())) {
			for (int i = 0; i < static_cast<int>(std::size(AOV_NAMES)); i++)
				if (ImGui::Selectable(AOV_NAMES[i], m_Renderer.View.AOVView == i))
					m_Renderer.View.AOVView = i;
			ImGui::EndCombo();
		}
		ImGui::SameLine();
		if (UI::Pill(ICON_DENOISE " Denoised", m_Renderer.Settings.Denoise))
			m_Renderer.Settings.Denoise = !m_Renderer.Settings.Denoise;
		ImGui::SameLine();
		if (UI::Pill(m_Renderer.View.PathTracing ? ICON_PAUSE "##pause" : ICON_PLAY "##pause", !m_Renderer.View.PathTracing))
			m_Renderer.View.PathTracing = !m_Renderer.View.PathTracing;
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip(m_Renderer.View.PathTracing ? "Pause accumulation" : "Resume accumulation");

		const char* hint = m_FocusPickArmed ? "click a point to focus" : "right mouse + WASD  G R S  Del";
		// a destra dei pulsanti; se non ci sta (finestra stretta, UI scale alta) l'aiuto si toglie,
		// l'avviso del focus no
		ImGui::SameLine();
		const float hintW = ImGui::CalcTextSize(hint).x;
		const float hintX = std::max(ImGui::GetCursorPosX() + 10.0f * s, ImGui::GetWindowWidth() - hintW - 12.0f * s);
		if (m_FocusPickArmed || hintX + hintW <= ImGui::GetWindowWidth()) {
			ImGui::SetCursorPosX(hintX);
			ImGui::AlignTextToFramePadding();
			if (m_FocusPickArmed)
				ImGui::TextColored(Theme::Accent, "%s", hint);
			else
				ImGui::TextDisabled("%s", hint);
		}
		else
			ImGui::NewLine();

		ImGui::EndChild();
		ImGui::PopStyleColor();
		ImGui::PopStyleVar();
	}

	// Le letture sopra la render: la selezione a sinistra, campioni e tempi GPU a destra
	void Application::DrawHud() {
		ImDrawList* list = ImGui::GetWindowDrawList();
		const float s = UI::Scale();
		const ImVec2 topLeft(m_ViewportPos.x + 12.0f * s, m_ViewportPos.y + 12.0f * s);
		const ImVec2 topRight(m_ViewportPos.x + m_ViewportSize.x - 12.0f * s, topLeft.y);
		const float lineStep = ImGui::GetFontSize() + 14.0f * s;

		if (m_Selection.IsValid()) {
			const char* mode = EffectiveGizmoOperation() == ImGuizmo::ROTATE ? "Rotate" : EffectiveGizmoOperation() == ImGuizmo::SCALE ? "Scale" : "Move";
			const std::string label = GetSelectionName() + "  " + mode;
			HudLabel(list, topLeft, 1.0f, label.c_str(), ImGui::GetColorU32(Theme::Accent));
		}
		// una scena che arriva da Blender si sostituisce al prossimo sync: lo si dice solo quando
		// c'e' qualcosa da perdere (ADR 0024)
		if (m_DocumentFromBlender && m_Document.IsModified(m_Scene, m_Assets, m_Renderer.Settings)) {
			const ImVec2 at(topLeft.x, topLeft.y + (m_Selection.IsValid() ? lineStep : 0.0f));
			HudLabel(list, at, 1.0f, ICON_LINK " Live from Blender: edits here are replaced on the next sync", ImGui::GetColorU32(Theme::TextDim));
		}
		if (!m_ShowHud)
			return;

		char line[128];
		const int samples = m_Renderer.AccumulatedFrames() * m_Renderer.Settings.SamplesPerPixel;
		snprintf(line, sizeof(line), "%d spp  %s", samples,
		         !m_Renderer.View.PathTracing ? "paused" : m_Renderer.AccumulatedFrames() < 64 ? "converging" : "accumulating");
		HudLabel(list, topRight, -1.0f, line, ImGui::GetColorU32(Theme::Text));
		const FrameRecord& latest = Profiler::Get().Latest();
		snprintf(line, sizeof(line), "PT %.2f ms  DN %.2f ms  %.0f fps", latest.GpuMs("Path tracer"), latest.GpuMs("Denoiser"), m_FPS);
		HudLabel(list, ImVec2(topRight.x, topRight.y + lineStep), -1.0f, line, ImGui::GetColorU32(Theme::Text));

		// la diagnosi del profiler: ambra quando il limite non e' la GPU; il clic apre il profiler
		bool warn = false;
		const std::string diagnosis = std::string(ICON_ZAP " ") + ProfilerHudLine(warn);
		const ImRect rect = HudLabel(list, ImVec2(topRight.x, topRight.y + 2 * lineStep), -1.0f, diagnosis.c_str(),
		                             ImGui::GetColorU32(warn ? Theme::AccentText : Theme::Text), warn ? ImGui::GetColorU32(Theme::Accent) : 0);
		if (ImGui::IsMouseHoveringRect(rect.Min, rect.Max) && ImGui::IsWindowHovered()) {
			ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
			ImGui::SetTooltip("Open the profiler");
			if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				m_ShowProfiler = true;
				m_FocusProfiler = true;
				m_HudClicked = true;
			}
		}
	}

	void Application::DrawViewportPanel(float deltaTime) {
		(void)deltaTime;
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, Theme::Rgb(0x0c0c0d));
		// la viewport non ha linguetta: la sua toolbar sta in cima, come nel mockup
		ImGuiWindowClass noTab;
		noTab.DockNodeFlagsOverrideSet = ImGuiDockNodeFlags_NoTabBar;
		ImGui::SetNextWindowClass(&noTab);
		ImGui::Begin(VIEWPORT, nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
		ImGui::PopStyleColor();

		DrawViewportToolbar();
		ImGui::SetCursorPosY(ImGui::GetCursorPosY() - ImGui::GetStyle().ItemSpacing.y); // l'immagine attaccata alla toolbar

		m_ViewportWidth = static_cast<uint32_t>(std::max(1.0f, ImGui::GetContentRegionAvail().x));
		m_ViewportHeight = static_cast<uint32_t>(std::max(1.0f, ImGui::GetContentRegionAvail().y));
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const ImTextureID image = DisplayTexture();
		if (image != 0)
			ImGui::GetWindowDrawList()->AddImage(image, pos, ImVec2(pos.x + m_ViewportWidth, pos.y + m_ViewportHeight),
			                                     ImVec2(0, 1), ImVec2(1, 0));

		// rettangolo dell'immagine: a questo si ancorano picking, gizmo e overlay
		m_ViewportPos = pos;
		m_ViewportSize = ImVec2(static_cast<float>(m_ViewportWidth), static_cast<float>(m_ViewportHeight));
		const ImVec2 mouse = ImGui::GetMousePos();
		m_ViewportHovered = ImGui::IsWindowHovered() && mouse.y >= pos.y;

		// sulla draw list della finestra: la background draw list starebbe sotto l'immagine
		DrawSelectionOverlay();
		DrawHud();
		DrawGizmo();

		ImGui::End();
		ImGui::PopStyleVar();
	}

	// ---------------------------------------------------------------- proprieta'

	void Application::DrawPropertiesPanel() {
		if (!ImGui::Begin(PROPERTIES, &m_ShowProperties, ImGuiWindowFlags_NoScrollbar)) {
			ImGui::End();
			return;
		}
		const float s = UI::Scale();

		struct Tab { PropertiesTab Id; const char* Icon; const char* Name; };
		const Tab tabs[] = {
			{ PropertiesTab::Object, ICON_OBJECT, "Object" }, { PropertiesTab::Material, ICON_MATERIAL, "Material" },
			{ PropertiesTab::World, ICON_WORLD, "World" }, { PropertiesTab::Camera, ICON_CAMERA, "Camera" },
			{ PropertiesTab::Render, ICON_RENDER, "Render" }, { PropertiesTab::Film, ICON_FILM, "Film" },
			{ PropertiesTab::Developer, ICON_DEBUG, "Developer" } };
		if (!m_ShowDeveloper && m_PropertiesTab == PropertiesTab::Developer)
			m_PropertiesTab = PropertiesTab::Render;

		// la colonna di icone: le sezioni della scena, poi quelle del render
		ImGui::PushStyleColor(ImGuiCol_ChildBg, Theme::Rail);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6.0f * s, 8.0f * s));
		ImGui::BeginChild("Rail", ImVec2(44.0f * s, 0.0f), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
		const float button = 32.0f * s;
		for (const Tab& tab : tabs) {
			if (tab.Id == PropertiesTab::Developer && !m_ShowDeveloper)
				continue;
			if (tab.Id == PropertiesTab::Render) {
				ImGui::Dummy(ImVec2(0, 2.0f * s));
				ImGui::Separator();
				ImGui::Dummy(ImVec2(0, 2.0f * s));
			}
			const bool active = m_PropertiesTab == tab.Id;
			ImGui::PushStyleColor(ImGuiCol_Button, active ? Theme::Rgb(0x3a3a42) : ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_Text, active ? Theme::Accent : Theme::Rgb(0xb4b4bc));
			if (ImGui::Button((std::string(tab.Icon) + "##" + tab.Name).c_str(), ImVec2(button, button)))
				m_PropertiesTab = tab.Id;
			ImGui::PopStyleColor(2);
			if (ImGui::IsItemHovered())
				ImGui::SetTooltip("%s", tab.Name);
		}
		ImGui::EndChild();
		ImGui::PopStyleVar();
		ImGui::PopStyleColor();

		ImGui::SameLine(0.0f, 0.0f);
		ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f * s, 10.0f * s));
		ImGui::BeginChild("Section", ImVec2(0.0f, 0.0f), false, ImGuiWindowFlags_AlwaysUseWindowPadding);
		ImGui::PopStyleVar();
		switch (m_PropertiesTab) {
		case PropertiesTab::Object:    DrawObjectSection(); break;
		case PropertiesTab::Material:  DrawMaterialSection(); break;
		case PropertiesTab::World:     DrawWorldSection(); break;
		case PropertiesTab::Camera:    DrawCameraSection(); break;
		case PropertiesTab::Render:    DrawRenderSection(); break;
		case PropertiesTab::Film:      DrawFilmSection(); break;
		case PropertiesTab::Developer: DrawDeveloperSection(); break;
		}
		ImGui::EndChild();
		ImGui::End();
	}

	void Application::DrawObjectSection() {
		if (!m_Selection.IsValid()) {
			ImGui::TextDisabled("No object selected.");
			ImGui::TextDisabled("Click in the viewport, or on an outliner row.");
			return;
		}
		ImGui::PushFont(UI::BoldFont());
		ImGui::TextColored(Theme::Accent, "%s", GetSelectionLabel().c_str());
		ImGui::PopFont();
		ImGui::Spacing();
		DrawSelectionProperties();
	}

	// Il materiale mostrato: quello cliccato nel pannello Materials, oppure, quando cambia la
	// selezione, quello dell'oggetto selezionato
	void Application::DrawMaterialSection() {
		if (m_Selection.Id != m_MaterialFollowsSelection) {
			m_MaterialFollowsSelection = m_Selection.Id;
			const ObjectId material = SelectedMaterial();
			if (!material.IsNil())
				m_EditedMaterial = material;
		}
		const int index = m_Assets.Materials().IndexOf(m_EditedMaterial);
		if (index < 0) {
			ImGui::TextDisabled("No material.");
			ImGui::TextDisabled("Pick one in the Materials panel, or select an object.");
		}
		else {
			Material material = m_Assets.Materials()[index];
			if (DrawMaterialEditor(material, index))
				m_Assets.Set(m_EditedMaterial, material);
			m_NewObjectMaterial = m_EditedMaterial; // i nuovi oggetti nascono col materiale aperto
		}
		ImGui::Spacing();
		if (UI::Section(ICON_TEXTURE " Textures", false))
			DrawTextureManager();
	}

	void Application::DrawWorldSection() {
		if (UI::Section("Sky")) {
			bool environment = m_Scene.EnvironmentMapping();
			glm::vec3 background = m_Scene.BackgroundColor();
			bool changed = UI::Checkbox("Environment map", &environment);
			ImGui::BeginDisabled(environment);
			changed |= UI::ColorEdit3("Background", glm::value_ptr(background));
			ImGui::EndDisabled();
			if (changed)
				m_Scene.SetBackground(background, environment);
		}
		if (UI::Section(ICON_SUN " Sun")) {
			SunParams sun = m_Scene.Sun();
			bool changed = UI::Checkbox("Enabled", &sun.Enabled);
			ImGui::BeginDisabled(!sun.Enabled);
			changed |= UI::SliderFloat("Elevation", &sun.Elevation, -90.0f, 90.0f, "%.1f deg");
			changed |= UI::SliderFloat("Azimuth", &sun.Azimuth, 0.0f, 360.0f, "%.1f deg");
			changed |= UI::SliderFloat("Angular radius", &sun.AngularRadius, 0.01f, 5.0f, "%.2f deg", ImGuiSliderFlags_Logarithmic);
			changed |= UI::ColorEdit3("Color", glm::value_ptr(sun.Color));
			changed |= UI::DragFloat("Irradiance", &sun.Irradiance, 0.05f, 0.0f, 1000.0f, "%.2f");
			ImGui::EndDisabled();
			if (changed)
				m_Scene.SetSun(sun);
		}
	}

	// L'inquadratura (ADR 0011): la vista la segue o naviga libera. L'obiettivo cambia i raggi
	// primari, quindi si riconsegna con SetCamera e l'accumulo riparte.
	void Application::DrawCameraSection() {
		if (UI::Section("View")) {
			if (UI::Checkbox("Lock to camera", &m_Renderer.View.LockViewToCamera) && m_Renderer.View.LockViewToCamera)
				m_Camera.SetView(m_Scene.Camera().Position, m_Scene.Camera().Direction); // torna all'inquadratura
			ImGui::TextDisabled(m_Renderer.View.LockViewToCamera ? "navigating moves the scene camera"
			                                                      : "free view: the scene camera stays put");
			ImGui::BeginDisabled(m_Renderer.View.LockViewToCamera);
			if (UI::Pill("Align camera to view")) { // cambia l'inquadratura salvata: e' una modifica
				SceneCamera camera = m_Scene.Camera();
				camera.Position = m_Camera.GetPosition();
				camera.Direction = m_Camera.GetDirection();
				m_Scene.SetCamera(camera);
			}
			ImGui::EndDisabled();
		}

		SceneCamera camera = m_Scene.Camera();
		bool changed = false;
		if (UI::Section("Lens")) {
			changed |= UI::SliderFloat("Vertical FOV", &camera.VerticalFOV, 30.0f, 140.0f, "%.1f deg");
			if (UI::Pill(ICON_RESET " Reset camera")) {
				const SceneCamera defaults;
				camera.Position = defaults.Position;
				camera.Direction = defaults.Direction;
				changed = true;
			}
		}
		if (UI::Section("Depth of field")) {
			if (UI::SliderFloat("Aperture", &camera.Aperture, 0.0f, 0.5f, "%.3f")) {
				camera.Aperture = std::max(camera.Aperture, 0.0f);
				changed = true;
			}
			const bool dofOff = camera.Aperture <= 0.0f;
			ImGui::BeginDisabled(dofOff);
			if (UI::DragFloat("Focus distance", &camera.FocusDistance, 0.05f, 0.01f, 1000.0f, "%.2f")) {
				camera.FocusDistance = std::max(camera.FocusDistance, 0.01f);
				changed = true;
			}
			// il prossimo click nella viewport mette a fuoco esattamente li'
			if (UI::Pill(m_FocusPickArmed ? "Cancel pick" : "Pick focus in the viewport", m_FocusPickArmed))
				m_FocusPickArmed = !m_FocusPickArmed;
			ImGui::EndDisabled();
			if (dofOff)
				ImGui::TextDisabled("aperture 0: no depth of field (pinhole)");
		}
		if (changed)
			m_Scene.SetCamera(camera);
	}

	// Come si calcola l'immagine (ADR 0010): si salva con la scena
	void Application::DrawRenderSection() {
		RenderSettings& settings = m_Renderer.Settings;
		const std::pair<const char*, RenderSettings (*)()> presets[] = {
			{ "Draft", &RenderSettings::Draft }, { "Preview", &RenderSettings::Preview }, { "Final", &RenderSettings::Final } };
		UI::Label("Preset");
		for (size_t i = 0; i < std::size(presets); i++) {
			if (i > 0) ImGui::SameLine();
			if (UI::Pill(presets[i].first, settings == presets[i].second()))
				settings = presets[i].second();
		}

		if (UI::Section("Integrator")) {
			UI::SliderInt("Samples / pixel", &settings.SamplesPerPixel, 1, 16);
			UI::SliderInt("Ray depth", &settings.RayDepth, 1, 50);
			UI::Checkbox("Light sampling", &settings.NEE);
			ImGui::SameLine();
			ImGui::TextDisabled("next event estimation");
			if (UI::DragFloat("Firefly clamp", &settings.FireflyClamp, 0.1f, 0.0f, 50.0f, settings.FireflyClamp > 0.0f ? "%.1f" : "off"))
				settings.FireflyClamp = std::max(settings.FireflyClamp, 0.0f);
		}

		if (UI::Section("Accumulation")) {
			UI::Checkbox("Reprojection", &settings.Reproject);
			ImGui::SameLine();
			ImGui::TextDisabled("keep history while moving");
			ImGui::BeginDisabled(!settings.Reproject);
			UI::SliderInt("History cap", &settings.HistoryCap, 1, 128, "%d frames");
			ImGui::EndDisabled();
			// risoluzione ridotta SOLO mentre la camera si muove; agisce al prossimo movimento
			const char* labels[] = { "100%", "75%", "50%", "25%" };
			const float values[] = { 1.0f, 0.75f, 0.5f, 0.25f };
			int scale = 0;
			for (int i = 0; i < 4; i++) if (settings.RenderScale == values[i]) scale = i;
			if (UI::Combo("Scale while moving", &scale, labels, 4))
				settings.RenderScale = values[scale];
			UI::Label("");
			if (UI::Pill(m_Renderer.View.PathTracing ? ICON_PAUSE " Pause" : ICON_PLAY " Resume", !m_Renderer.View.PathTracing))
				m_Renderer.View.PathTracing = !m_Renderer.View.PathTracing;
			ImGui::SameLine();
			if (UI::Pill(ICON_RESET " Reset"))
				m_Renderer.RestartAccumulation();
		}

		// Il denoiser filtra la vista, non tocca l'accumulo (ADR 0001)
		if (UI::Section("Denoiser")) {
			UI::Checkbox("A-trous filter", &settings.Denoise);
			ImGui::BeginDisabled(!settings.Denoise);
			UI::Checkbox("Fade", &settings.DenoiseAutoFade);
			ImGui::SameLine();
			ImGui::TextDisabled("as the image converges");
			UI::SliderFloat("Strength", &settings.DenoiseStrength, 0.0f, 1.0f, "%.2f");
			ImGui::EndDisabled();
			if (settings.Denoise && m_Renderer.View.AOVView != 0)
				ImGui::TextDisabled("active in the Beauty view only");
		}
		if (UI::Section("Edge stopping", false)) {
			ImGui::BeginDisabled(!settings.Denoise);
			UI::SliderFloat("Luminance", &settings.DenoiseCPhi, 0.05f, 10.0f, "%.2f");
			UI::SliderFloat("Normal", &settings.DenoiseNPhi, 1.0f, 128.0f, "%.0f");
			UI::SliderFloat("Depth", &settings.DenoisePPhi, 0.05f, 10.0f, "%.2f");
			UI::SliderFloat("Albedo", &settings.DenoiseAPhi, 0.02f, 1.0f, "%.2f");
			ImGui::EndDisabled();
		}
	}

	// Esposizione e tonemap agiscono dopo l'accumulo: non lo fanno ripartire
	void Application::DrawFilmSection() {
		if (UI::Section("Film")) {
			static constexpr const char* labels[] = { "None", "ACES", "ACES (fitted)", "AgX", "Khronos PBR Neutral", "Reinhard", "Hable" };
			static_assert(std::size(labels) == std::size(ToneMapperKeys));
			int mapper = static_cast<int>(m_Renderer.Settings.ToneMap);
			if (UI::Combo("Tone mapping", &mapper, labels, static_cast<int>(std::size(labels))))
				m_Renderer.Settings.ToneMap = static_cast<ToneMapper>(mapper);
			UI::SliderFloat("Exposure", &m_Renderer.Settings.Exposure, 0.0f, 10.0f, "%.2f");
		}
	}

	void Application::DrawDeveloperSection() {
		if (UI::Section("Picking")) {
			ImGui::PushFont(UI::MonoFont());
			ImGui::Text("viewport hover : %s", m_ViewportHovered ? "yes" : "no");
			ImGui::Text("viewport rect  : (%.0f,%.0f) %.0f x %.0f", m_ViewportPos.x, m_ViewportPos.y, m_ViewportSize.x, m_ViewportSize.y);
			ImGui::Text("pick requests  : %d", m_PickRequestCount);
			ImGui::Text("last pixel     : (%d, %d)", m_LastPickPixel.x, m_LastPickPixel.y);
			ImGui::Text("last result    : type=%d index=%d", m_LastPickType, m_LastPickIndex);
			ImGui::PopFont();
			ImGui::TextDisabled("-2 = not written by the shader, -1 = ray missed");
			// Bypassa il gate del mouse: se questo seleziona ma il click no, il problema e' nel gate
			if (UI::Pill("Pick at viewport center")) {
				m_LastPickPixel = { static_cast<int>(m_ViewportSize.x) / 2, static_cast<int>(m_ViewportSize.y) / 2 };
				m_PickRequestCount++;
				m_Renderer.RequestPick(m_LastPickPixel.x, m_LastPickPixel.y);
			}
		}
	}

	// ---------------------------------------------------------------- finestra (ADR 0021)

	// Due scale diverse. Su X11 e Windows la finestra e' in pixel: la UI si ingrandisce col
	// monitor. Su Wayland la finestra e' in unita' logiche e il framebuffer e' gia' scalato: le
	// misure restano quelle, e solo i font si rasterizzano a risoluzione piena per restare nitidi.
	void Application::ApplyUiScale() {
		float contentX = 1.0f, contentY = 1.0f;
		glfwGetWindowContentScale(m_Window, &contentX, &contentY);
		int windowW = 0, windowH = 0, framebufferW = 0, framebufferH = 0;
		glfwGetWindowSize(m_Window, &windowW, &windowH);
		glfwGetFramebufferSize(m_Window, &framebufferW, &framebufferH);
		m_FramebufferScale = windowW > 0 && framebufferW > 0 ? static_cast<float>(framebufferW) / windowW : 1.0f;
		const float content = std::max(1.0f, contentX);
		const float user = std::clamp(m_Preferences.UiScale, Preferences::MIN_UI_SCALE, Preferences::MAX_UI_SCALE);
		m_UiScale = std::max(1.0f, content / m_FramebufferScale) * user;
		const int platform = glfwGetPlatform();
		std::cout << "Window: " << (platform == GLFW_PLATFORM_WAYLAND ? "wayland" : platform == GLFW_PLATFORM_X11 ? "x11" : platform == GLFW_PLATFORM_WIN32 ? "win32" : "other")
		          << (m_CustomTitleBar ? ", app title bar" : ", system title bar")
		          << ", content scale " << content << ", framebuffer scale " << m_FramebufferScale << ", ui scale " << user
		          << ", cursor theme " << (getenv("XCURSOR_THEME") ? getenv("XCURSOR_THEME") : "default")
		          << " " << (getenv("XCURSOR_SIZE") ? getenv("XCURSOR_SIZE") : "") << std::endl;

		ImGuiIO& io = ImGui::GetIO();
		io.Fonts->Clear();
		const Theme::Fonts fonts = Theme::LoadFonts(content * user);
		io.FontGlobalScale = 1.0f / m_FramebufferScale;
		Theme::Apply(m_UiScale);
		UI::Configure(fonts, m_UiScale);
	}

	void Application::OnContentScale(GLFWwindow*, float, float) {
		Get().m_RescaleRequested = true;
	}

	// La texture dei font va rifatta (cambio di monitor o di UI scale). Si rifanno solo gli oggetti
	// Vulkan del backend: ImGui_ImplVulkan_Shutdown distruggerebbe anche le finestre della
	// piattaforma, compreso l'handle della finestra principale, e il backend GLFW al frame dopo lo
	// userebbe nullo. I due descrittori vecchi (font e render) si liberano a mano: il backend non lo fa.
	void Application::RebuildUiForScale() {
		m_RescaleRequested = false;
		vkDeviceWaitIdle(m_Vulkan.Device());
		const VkDescriptorSet oldFont = reinterpret_cast<VkDescriptorSet>(ImGui::GetIO().Fonts->TexID);
		ApplyUiScale();
		if (oldFont != VK_NULL_HANDLE)
			ImGui_ImplVulkan_RemoveTexture(oldFont);
		if (m_DisplaySet != VK_NULL_HANDLE)
			ImGui_ImplVulkan_RemoveTexture(m_DisplaySet);
		m_DisplaySet = VK_NULL_HANDLE; // si rifa' col layout nuovo al prossimo disegno
		ImGui_ImplVulkan_DestroyDeviceObjects();
		ImGui_ImplVulkan_CreateDeviceObjects();
		m_Vulkan.Immediate([](VkCommandBuffer cmd) { ImGui_ImplVulkan_CreateFontsTexture(cmd); });
		ImGui_ImplVulkan_DestroyFontUploadObjects();
	}

	// A destra nella barra dei menu: il file aperto e, senza cornice, i tre pulsanti della finestra.
	// Lo spazio vuoto della barra la trascina, e il doppio clic la ingrandisce.
	void Application::DrawWindowControls() {
		const float s = UI::Scale();
		const std::string name = m_Document.DisplayName() + (m_Document.IsModified(m_Scene, m_Assets, m_Renderer.Settings) ? "*" : "");
		const float button = ImGui::GetFrameHeight() * 1.4f;
		const float controls = m_CustomTitleBar ? button * 3.0f + ImGui::GetStyle().WindowPadding.x : 0.0f;
		ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(name.c_str()).x - controls - 16.0f * s);
		ImGui::TextDisabled("%s", name.c_str());
		if (!m_CustomTitleBar)
			return;

		// lo spazio vuoto della barra e' la barra del titolo
		if (ImGui::IsWindowHovered() && !ImGui::IsAnyItemHovered() && m_WindowEdges == 0) {
			if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
				if (glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED)) glfwRestoreWindow(m_Window);
				else glfwMaximizeWindow(m_Window);
			}
			else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
				glfwStartInteractiveMove(m_Window);
				// il rilascio va al sistema, che sta spostando la finestra: ImGui non lo vedrebbe e
				// terrebbe il tasto premuto, perdendo il clic dopo (e il doppio clic)
				ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
			}
		}

		ImGui::SameLine(ImGui::GetWindowWidth() - controls);
		ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
		ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
		ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
		const ImVec2 size(button, ImGui::GetFrameHeight());
		const bool maximized = glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED);
		if (ImGui::Button(ICON_MINIMIZE "##minimize", size))
			glfwIconifyWindow(m_Window);
		ImGui::SameLine();
		if (ImGui::Button(maximized ? ICON_RESTORE "##maximize" : ICON_MAXIMIZE "##maximize", size)) {
			if (maximized) glfwRestoreWindow(m_Window);
			else glfwMaximizeWindow(m_Window);
		}
		ImGui::SameLine();
		ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Rgb(0xc42b1c));
		ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Rgb(0xa8251a));
		if (ImGui::Button(ICON_CLOSE "##close", size))
			WhenSaved([this] { m_IsRunning = false; });
		ImGui::PopStyleColor(3);
		ImGui::PopStyleVar(2);
	}

	// Senza cornice i bordi della finestra sono nostri: una fascia di pochi pixel chiede al
	// sistema di ridimensionarla. Va prima dei pannelli: il click sul bordo non e' per loro.
	void Application::HandleWindowEdges() {
		m_WindowEdges = 0;
		if (!m_CustomTitleBar || glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED))
			return;
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		const ImVec2 mouse = ImGui::GetMousePos();
		const float border = 5.0f * UI::Scale(), corner = 14.0f * UI::Scale();
		const float left = mouse.x - viewport->Pos.x, top = mouse.y - viewport->Pos.y;
		const float right = viewport->Pos.x + viewport->Size.x - mouse.x, bottom = viewport->Pos.y + viewport->Size.y - mouse.y;
		if (left < 0 || top < 0 || right < 0 || bottom < 0)
			return; // fuori dalla finestra

		if (top < border) m_WindowEdges |= GLFW_WINDOW_EDGE_TOP;
		if (bottom < border) m_WindowEdges |= GLFW_WINDOW_EDGE_BOTTOM;
		if (left < border) m_WindowEdges |= GLFW_WINDOW_EDGE_LEFT;
		if (right < border) m_WindowEdges |= GLFW_WINDOW_EDGE_RIGHT;
		// vicino a un angolo, l'angolo: piu' facile da prendere della fascia sottile
		if (m_WindowEdges != 0) {
			if (top < corner) m_WindowEdges |= GLFW_WINDOW_EDGE_TOP;
			if (bottom < corner) m_WindowEdges |= GLFW_WINDOW_EDGE_BOTTOM;
			if (left < corner) m_WindowEdges |= GLFW_WINDOW_EDGE_LEFT;
			if (right < corner) m_WindowEdges |= GLFW_WINDOW_EDGE_RIGHT;
			if ((m_WindowEdges & (GLFW_WINDOW_EDGE_TOP | GLFW_WINDOW_EDGE_BOTTOM)) == (GLFW_WINDOW_EDGE_TOP | GLFW_WINDOW_EDGE_BOTTOM))
				m_WindowEdges &= ~(top < bottom ? GLFW_WINDOW_EDGE_BOTTOM : GLFW_WINDOW_EDGE_TOP);
			if ((m_WindowEdges & (GLFW_WINDOW_EDGE_LEFT | GLFW_WINDOW_EDGE_RIGHT)) == (GLFW_WINDOW_EDGE_LEFT | GLFW_WINDOW_EDGE_RIGHT))
				m_WindowEdges &= ~(left < right ? GLFW_WINDOW_EDGE_RIGHT : GLFW_WINDOW_EDGE_LEFT);
		}
		if (m_WindowEdges == 0)
			return;

		const bool vertical = m_WindowEdges & (GLFW_WINDOW_EDGE_TOP | GLFW_WINDOW_EDGE_BOTTOM);
		const bool horizontal = m_WindowEdges & (GLFW_WINDOW_EDGE_LEFT | GLFW_WINDOW_EDGE_RIGHT);
		const bool mainDiagonal = m_WindowEdges == (GLFW_WINDOW_EDGE_TOP | GLFW_WINDOW_EDGE_LEFT)
			|| m_WindowEdges == (GLFW_WINDOW_EDGE_BOTTOM | GLFW_WINDOW_EDGE_RIGHT);
		ImGui::SetMouseCursor(vertical && horizontal ? (mainDiagonal ? ImGuiMouseCursor_ResizeNWSE : ImGuiMouseCursor_ResizeNESW)
			: vertical ? ImGuiMouseCursor_ResizeNS : ImGuiMouseCursor_ResizeEW);
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
			glfwStartInteractiveResize(m_Window, m_WindowEdges);
			ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false); // come per lo spostamento
		}
	}
}
