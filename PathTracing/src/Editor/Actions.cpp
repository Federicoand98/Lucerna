// Le azioni dell'interfaccia e le loro scorciatoie (ADR 0027): il registro, il dispatch dei tasti,
// le voci di menu che lo leggono, Duplicate e la sezione Keymap delle Preferences.

#include "Editor/Application.h"
#include "Editor/Icons.h"
#include "Editor/Theme.h"
#include "Editor/Widgets.h"
#include "Import/SceneFiles.h"
#include "vendor/portable-file-dialogs.h"

#include "imgui_internal.h"
#include <glm/gtc/matrix_transform.hpp>

namespace Lucerna {

	namespace {
		// Un tasto usato da una scorciatoia non arriva ai widget (ADR 0027): niente attivazione
		// della navigazione da tastiera in questo frame, e il tasto resta nostro finche' non si
		// rilascia, cosi' non si ripete.
		void ConsumeKey(ImGuiKey key) {
			ImGuiContext& g = *ImGui::GetCurrentContext();
			g.NavActivateId = g.NavActivateDownId = g.NavActivatePressedId = 0;
			g.NavDisableHighlight = true; // Spazio e Invio accenderebbero il riquadro della navigazione da tastiera
			ImGui::SetKeyOwner(key, ImGui::GetID("##shortcuts"), ImGuiInputFlags_LockUntilRelease);
		}

		bool IsModifier(ImGuiKey key) {
			return key == ImGuiKey_LeftCtrl || key == ImGuiKey_RightCtrl || key == ImGuiKey_LeftShift || key == ImGuiKey_RightShift
				|| key == ImGuiKey_LeftAlt || key == ImGuiKey_RightAlt || key == ImGuiKey_LeftSuper || key == ImGuiKey_RightSuper
				|| key == ImGuiKey_ReservedForModCtrl || key == ImGuiKey_ReservedForModShift
				|| key == ImGuiKey_ReservedForModAlt || key == ImGuiKey_ReservedForModSuper;
		}
	}

	void Application::RegisterActions() {
		Keymap& k = m_Keymap;
		using C = KeyContext;
		auto add = [&](const char* id, const char* group, const char* name, const char* icon, ImGuiKeyChord key,
		               std::function<void()> run, std::function<bool()> enabled = {}, std::function<bool()> checked = {}, C context = C::Global) {
			k.Add({ id, group, name, icon, context, key, std::move(run), std::move(enabled), std::move(checked) });
		};
		auto toggle = [](bool& flag) { return [&flag] { flag = !flag; }; };
		auto is = [](const bool& flag) { return [&flag] { return flag; }; };

		// ---- File
		add("file.new", "File", "New", ICON_NEW, ImGuiMod_Ctrl | ImGuiKey_N, [this] { WhenSaved([this] { NewDocument(); }); });
		add("file.open", "File", "Open...", ICON_OPEN, ImGuiMod_Ctrl | ImGuiKey_O, [this] { ShowOpenDialog(); });
		add("file.open_blender", "File", "Open in Blender...", ICON_EXTERNAL, ImGuiKey_None, [this] { OpenInBlender(); });
		add("file.blender_sync", "File", "Sync from Blender", ICON_REFRESH, ImGuiMod_Ctrl | ImGuiKey_R,
			[this] { m_Blender.RequestSync(); }, [this] { return m_Blender.CanSync(); });
		add("file.next_scene", "File", "Next built-in scene", nullptr, ImGuiKey_None, [this] {
			const int n = static_cast<int>(m_BuiltinScenes.size());
			WhenSaved([this, n] { LoadBuiltinScene(m_CurrentScene < 0 ? 0 : (m_CurrentScene + 1) % n); });
		}, [this] { return !m_BuiltinScenes.empty(); });
		add("file.previous_scene", "File", "Previous built-in scene", nullptr, ImGuiKey_None, [this] {
			const int n = static_cast<int>(m_BuiltinScenes.size());
			WhenSaved([this, n] { LoadBuiltinScene(m_CurrentScene < 0 ? n - 1 : (m_CurrentScene + n - 1) % n); });
		}, [this] { return !m_BuiltinScenes.empty(); });
		add("file.save", "File", "Save", ICON_SAVE, ImGuiMod_Ctrl | ImGuiKey_S, [this] { SaveDocument(); });
		add("file.save_as", "File", "Save As...", nullptr, ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_S, [this] { SaveDocumentAs(); });
		// un file importato si riapre quando cambia sul disco (un export di un altro tool)
		add("file.live_link", "File", "Reload when the file changes", nullptr, ImGuiKey_None, toggle(m_LiveLink),
			[this] { return !m_WatchPath.empty(); }, is(m_LiveLink));
		add("file.screenshot", "File", "Save screenshot", ICON_SCREENSHOT, ImGuiKey_F12, [this] { m_ScreenshotRequested = true; });
		add("file.quit", "File", "Quit", nullptr, ImGuiMod_Ctrl | ImGuiKey_Q, [this] { WhenSaved([this] { m_IsRunning = false; }); });

		// ---- Edit
		add("edit.duplicate", "Edit", "Duplicate", ICON_RESTORE, ImGuiMod_Shift | ImGuiKey_D, [this] { DuplicateSelection(); },
			[this] { return m_Selection.IsValid() && SelectedIndex() >= 0; });
		add("edit.delete", "Edit", "Delete", ICON_DELETE, ImGuiKey_Delete, [this] { m_DeleteRequested = true; },
			[this] { return CanDeleteSelection(); });
		add("edit.deselect", "Edit", "Deselect", ICON_CLOSE, ImGuiKey_Escape, [this] { m_Selection = {}; },
			[this] { return m_Selection.IsValid(); });
		add("edit.preferences", "Edit", "Preferences...", ICON_SETTINGS, ImGuiMod_Ctrl | ImGuiKey_Comma, [this] { OpenPreferences(m_PreferencesSection); });

		// ---- Add: gli oggetti nascono davanti alla camera, col materiale aperto nelle proprieta'
		add("add.menu", "Add", "Add menu at the mouse", ICON_ADD, ImGuiMod_Shift | ImGuiKey_A, [this] { m_OpenAddPopup = true; });
		add("add.sphere", "Add", "Sphere", ICON_SPHERE, ImGuiKey_None, [this] {
			Sphere sphere;
			sphere.Position = glm::vec4(SpawnPosition(), 0.5f); // la w e' il raggio
			sphere.Material = EnsureMaterialExists();
			m_Selection = Select(SelectionType::Sphere, m_Scene.AddSphere(sphere));
			m_GizmoOperation = ImGuizmo::TRANSLATE;
		});
		add("add.box", "Add", "Box", ICON_BOX, ImGuiKey_None, [this] {
			// CreateBox appende i suoi 6 quad in coda: gli indici dei box gia' esistenti restano validi
			const glm::vec3 spawn = SpawnPosition();
			m_Scene.CreateBox(spawn - glm::vec3(0.5f), spawn + glm::vec3(0.5f), EnsureMaterialExists());
			m_Selection = Select(SelectionType::Box, static_cast<int>(m_Scene.Boxes().size()) - 1);
			m_GizmoOperation = ImGuizmo::TRANSLATE;
		});
		add("add.quad", "Add", "Quad", ICON_QUAD, ImGuiKey_None, [this] {
			Quad quad;
			quad.PositionLLC = glm::vec4(SpawnPosition() - glm::vec3(0.5f, 0.5f, 0.0f), 0.0f);
			quad.U = { 1.0f, 0.0f, 0.0f, 0.0f };
			quad.V = { 0.0f, 1.0f, 0.0f, 0.0f };
			quad.Material = EnsureMaterialExists();
			m_Selection = Select(SelectionType::Quad, m_Scene.AddQuad(quad));
			m_GizmoOperation = ImGuizmo::TRANSLATE;
		});
		// luci punto e spot (ADR 0009): lo spot nasce rivolto in basso
		for (const bool spot : { false, true })
			add(spot ? "add.spot_light" : "add.point_light", "Add", spot ? "Spot light" : "Point light",
				spot ? ICON_SPOT_LIGHT : ICON_POINT_LIGHT, ImGuiKey_None, [this, spot] {
				PunctualLight light;
				light.Spot = spot;
				light.Position = SpawnPosition();
				m_Selection = Select(SelectionType::Light, m_Scene.AddLight(light, spot ? "Spot light" : "Point light"));
				m_GizmoOperation = ImGuizmo::TRANSLATE;
			});
		// le mesh e le luci di un file (issue #27): lo stesso file aggiunto due volte si carica una
		// volta sola, le istanze dividono la geometria
		add("add.model", "Add", "Model...", ICON_MESH, ImGuiKey_None, [this] {
			const auto selection = pfd::open_file("Add model", "models", FileFilters()).result();
			if (selection.empty())
				return;
			const int mesh = AddFile(selection[0], m_Scene, m_Assets, glm::translate(glm::mat4(1.0f), SpawnPosition()), EnsureMaterialExists());
			if (mesh >= 0) {
				m_Selection = Select(SelectionType::Mesh, mesh);
				m_GizmoOperation = ImGuizmo::TRANSLATE;
			}
		});

		// ---- View
		add("view.outliner", "View", "Outliner", nullptr, ImGuiKey_None, toggle(m_ShowOutliner), {}, is(m_ShowOutliner));
		add("view.materials", "View", "Materials", nullptr, ImGuiKey_None, toggle(m_ShowMaterials), {}, is(m_ShowMaterials));
		add("view.properties", "View", "Properties", nullptr, ImGuiKey_None, toggle(m_ShowProperties), {}, is(m_ShowProperties));
		add("view.profiler", "View", "Profiler", ICON_PROFILER, ImGuiKey_F3, [this] {
			m_ShowProfiler = !m_ShowProfiler;
			m_FocusProfiler = m_ShowProfiler;
		}, {}, is(m_ShowProfiler));
		add("view.hud", "View", "Viewport readouts", nullptr, ImGuiKey_H, toggle(m_ShowHud), {}, is(m_ShowHud));
		add("view.developer", "View", "Developer", ICON_DEBUG, ImGuiKey_None, toggle(m_ShowDeveloper), {}, is(m_ShowDeveloper));
		add("view.reset_layout", "View", "Reset layout", ICON_LAYOUT, ImGuiKey_None, [this] { m_ResetLayout = true; });
		static const char* const aovIds[] = { "view.aov_beauty", "view.aov_albedo", "view.aov_normal", "view.aov_depth", "view.aov_history" };
		for (int i = 0; i < 5; i++)
			add(aovIds[i], "View", AOV_NAMES[i], nullptr, ImGuiMod_Alt | (ImGuiKey_1 + i),
				[this, i] { m_Renderer.View.AOVView = i; }, {}, [this, i] { return m_Renderer.View.AOVView == i; });
		const std::pair<ImGuizmo::OPERATION, ImGuiKey> gizmo[] = { { ImGuizmo::TRANSLATE, ImGuiKey_G }, { ImGuizmo::ROTATE, ImGuiKey_R }, { ImGuizmo::SCALE, ImGuiKey_S } };
		const char* gizmoIds[] = { "view.move", "view.rotate", "view.scale" };
		const char* gizmoNames[] = { "Move", "Rotate", "Scale" };
		const char* gizmoIcons[] = { ICON_MOVE, ICON_ROTATE, ICON_SCALE };
		for (int i = 0; i < 3; i++)
			add(gizmoIds[i], "View", gizmoNames[i], gizmoIcons[i], gizmo[i].second, [this, op = gizmo[i].first] { m_GizmoOperation = op; },
				{}, [this, op = gizmo[i].first] { return m_GizmoOperation == op; });

		// ---- Viewport
		add("viewport.pause", "Viewport", "Pause accumulation", ICON_PAUSE, ImGuiKey_Space,
			[this] { m_Renderer.View.PathTracing = !m_Renderer.View.PathTracing; }, {}, [this] { return !m_Renderer.View.PathTracing; });
		add("viewport.reset", "Viewport", "Restart accumulation", ICON_RESET, ImGuiMod_Shift | ImGuiKey_R, [this] { m_Renderer.RestartAccumulation(); });
		add("viewport.denoise", "Viewport", "Denoiser", ICON_DENOISE, ImGuiMod_Alt | ImGuiKey_D,
			[this] { m_Renderer.Settings.Denoise = !m_Renderer.Settings.Denoise; }, {}, [this] { return m_Renderer.Settings.Denoise; });

		// ---- Camera (ADR 0011)
		add("camera.lock", "Camera", "Lock view to camera", nullptr, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiKey_0, [this] {
			m_Renderer.View.LockViewToCamera = !m_Renderer.View.LockViewToCamera;
			if (m_Renderer.View.LockViewToCamera)
				m_Camera.SetView(m_Scene.Camera().Position, m_Scene.Camera().Direction); // torna all'inquadratura
		}, {}, [this] { return m_Renderer.View.LockViewToCamera; });
		add("camera.align", "Camera", "Align camera to view", nullptr, ImGuiMod_Ctrl | ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_0, [this] {
			SceneCamera camera = m_Scene.Camera(); // cambia l'inquadratura salvata: e' una modifica
			camera.Position = m_Camera.GetPosition();
			camera.Direction = m_Camera.GetDirection();
			m_Scene.SetCamera(camera);
		}, [this] { return !m_Renderer.View.LockViewToCamera; });
		add("camera.reset", "Camera", "Reset camera", ICON_RESET, ImGuiKey_None, [this] {
			SceneCamera camera = m_Scene.Camera();
			const SceneCamera defaults;
			camera.Position = defaults.Position;
			camera.Direction = defaults.Direction;
			m_Scene.SetCamera(camera);
		});
		// il prossimo clic nella viewport mette a fuoco esattamente li'
		add("camera.pick_focus", "Camera", "Pick focus in the viewport", nullptr, ImGuiKey_F, [this] { m_FocusPickArmed = !m_FocusPickArmed; },
			[this] { return m_Scene.Camera().Aperture > 0.0f; }, is(m_FocusPickArmed));

		// ---- Render (ADR 0010)
		const std::pair<const char*, RenderSettings (*)()> presets[] = {
			{ "Draft", &RenderSettings::Draft }, { "Preview", &RenderSettings::Preview }, { "Final", &RenderSettings::Final } };
		const char* presetIds[] = { "render.draft", "render.preview", "render.final" };
		for (int i = 0; i < 3; i++)
			add(presetIds[i], "Render", presets[i].first, nullptr, ImGuiMod_Ctrl | (ImGuiKey_1 + i),
				[this, preset = presets[i].second] { m_Renderer.Settings = preset(); }, {},
				[this, preset = presets[i].second] { return m_Renderer.Settings == preset(); });
		add("render.nee", "Render", "Light sampling", nullptr, ImGuiKey_None, toggle(m_Renderer.Settings.NEE), {}, is(m_Renderer.Settings.NEE));
		add("render.reproject", "Render", "Reprojection", nullptr, ImGuiKey_None, toggle(m_Renderer.Settings.Reproject), {}, is(m_Renderer.Settings.Reproject));
		add("render.next_tone_mapper", "Render", "Next tone mapper", nullptr, ImGuiKey_None, [this] {
			const int n = static_cast<int>(std::size(ToneMapperKeys));
			m_Renderer.Settings.ToneMap = static_cast<ToneMapper>((static_cast<int>(m_Renderer.Settings.ToneMap) + 1) % n);
		});

		// ---- Properties: le sezioni della colonna di icone
		const std::tuple<const char*, const char*, const char*, PropertiesTab> tabs[] = {
			{ "properties.object", "Object", ICON_OBJECT, PropertiesTab::Object }, { "properties.material", "Material", ICON_MATERIAL, PropertiesTab::Material },
			{ "properties.world", "World", ICON_WORLD, PropertiesTab::World }, { "properties.camera", "Camera", ICON_CAMERA, PropertiesTab::Camera },
			{ "properties.render", "Render", ICON_RENDER, PropertiesTab::Render }, { "properties.film", "Film", ICON_FILM, PropertiesTab::Film },
			{ "properties.developer", "Developer", ICON_DEBUG, PropertiesTab::Developer } };
		for (const auto& [id, name, icon, tab] : tabs)
			add(id, "Properties", name, icon, ImGuiKey_None, [this, tab = tab] { m_ShowProperties = true; m_PropertiesTab = tab; },
				[this, tab = tab] { return tab != PropertiesTab::Developer || m_ShowDeveloper; }, [this, tab = tab] { return m_PropertiesTab == tab; });

		// ---- Profiler: valgono col pannello a fuoco, e li' vincono sul globale. Pausa e zoom
		// hanno bisogno della vista, quindi li esegue il pannello (m_ProfilerRequest).
		auto profiler = [&](const char* id, const char* name, ImGuiKeyChord key, std::function<void()> run, std::function<bool()> enabled = {}) {
			add(id, "Profiler", name, nullptr, key, std::move(run), std::move(enabled), {}, C::Profiler);
		};
		profiler("profiler.live", "Live / Pause", ImGuiKey_Space, [this] { m_ProfilerRequest = ProfilerRequest::ToggleLive; });
		profiler("profiler.back_to_live", "Back to live", ImGuiKey_Escape, [this] { m_ProfilerRequest = ProfilerRequest::GoLive; },
			[this] { return !m_ProfilerLive; });
		const std::pair<const char*, const char*> profilerTabs[] = { { "profiler.tab_frame", "Frame tab" }, { "profiler.tab_loading", "Loading tab" }, { "profiler.tab_memory", "Memory tab" } };
		for (int i = 0; i < 3; i++)
			profiler(profilerTabs[i].first, profilerTabs[i].second, ImGuiKey_None, [this, i] { m_ProfilerTab = i; });
		profiler("profiler.copy", "Copy summary", ImGuiKey_None, [this] { ImGui::SetClipboardText(ProfilerSummaryText().c_str()); });
		profiler("profiler.reference", "Save / clear reference", ImGuiKey_None, [this] { SaveProfilerReference(m_ProfilerReferenceFrameMs <= 0.0f); });
		const std::pair<const char*, const char*> zooms[] = { { "profiler.zoom_2s", "Zoom: 2 s" }, { "profiler.zoom_10s", "Zoom: 10 s" },
			{ "profiler.zoom_1min", "Zoom: 1 min" }, { "profiler.zoom_all", "Zoom: all history" } };
		for (int i = 0; i < 4; i++)
			profiler(zooms[i].first, zooms[i].second, ImGuiKey_None, [this, i] { m_ProfilerRequest = static_cast<ProfilerRequest>(static_cast<int>(ProfilerRequest::Zoom2s) + i); });

		// ---- Window (ADR 0021)
		add("window.maximize", "Window", "Maximize / restore", ICON_MAXIMIZE, ImGuiKey_F11, [this] {
			if (glfwGetWindowAttrib(m_Window, GLFW_MAXIMIZED)) glfwRestoreWindow(m_Window);
			else glfwMaximizeWindow(m_Window);
		});
		add("window.minimize", "Window", "Minimize", ICON_MINIMIZE, ImGuiKey_None, [this] { glfwIconifyWindow(m_Window); });

		// ---- Navigation: tasti tenuti col tasto destro premuto; la Camera li legge da NavigationKeys
		const std::pair<const char*, const char*> nav[] = { { "nav.forward", "Forward" }, { "nav.back", "Back" }, { "nav.left", "Left" },
			{ "nav.right", "Right" }, { "nav.up", "Up" }, { "nav.down", "Down" } };
		const ImGuiKey navKeys[] = { ImGuiKey_W, ImGuiKey_S, ImGuiKey_A, ImGuiKey_D, ImGuiKey_Q, ImGuiKey_E };
		for (int i = 0; i < 6; i++)
			add(nav[i].first, "Navigation", nav[i].second, nullptr, navKeys[i], [] {}, {}, {}, C::Navigation);

		m_Keymap.Apply(m_Preferences.Keymap);
	}

	CameraKeys Application::NavigationKeys() const {
		return { m_Keymap.Down("nav.forward"), m_Keymap.Down("nav.back"), m_Keymap.Down("nav.left"),
		         m_Keymap.Down("nav.right"), m_Keymap.Down("nav.up"), m_Keymap.Down("nav.down") };
	}

	// Le scorciatoie, una volta a frame dopo la barra dei menu. Sospese mentre si scrive in un
	// campo, con una modale aperta, durante la navigazione col tasto destro e mentre le
	// Preferences aspettano un tasto da assegnare. Il pannello a fuoco vince sul globale.
	void Application::HandleShortcuts() {
		const ImGuiIO& io = ImGui::GetIO();
		const bool suspended = io.WantTextInput || !m_CapturingAction.empty() || ImGui::GetTopMostPopupModal() != nullptr
			|| ImGui::IsMouseDown(ImGuiMouseButton_Right);
		if (!suspended) {
			const Action* action = m_ProfilerFocused && m_ShowProfiler ? m_Keymap.Pressed(KeyContext::Profiler) : nullptr;
			if (!action)
				action = m_Keymap.Pressed(KeyContext::Global);
			if (action) {
				ConsumeKey(static_cast<ImGuiKey>(m_Keymap.Key(action->Id) & ~ImGuiMod_Mask_));
				action->Run();
			}
		}

		// il menu Add sotto il mouse: si apre qui, a livello radice, dove lo cerca BeginPopup
		if (m_OpenAddPopup) {
			m_OpenAddPopup = false;
			ImGui::OpenPopup("##AddAtMouse");
		}
		if (ImGui::BeginPopup("##AddAtMouse")) {
			ImGui::TextDisabled(ICON_ADD " Add");
			ImGui::Separator();
			DrawAddItems();
			ImGui::EndPopup();
		}
	}

	bool Application::MenuAction(const char* id) {
		const Action* action = m_Keymap.Find(id);
		if (!action)
			return false;
		const std::string label = action->Icon ? std::string(action->Icon) + " " + action->Name : std::string(action->Name);
		const std::string key = Keymap::Format(m_Keymap.Key(id));
		const bool enabled = !action->Enabled || action->Enabled();
		if (!ImGui::MenuItem(label.c_str(), key.empty() ? nullptr : key.c_str(), action->Checked && action->Checked(), enabled))
			return false;
		action->Run();
		return true;
	}

	void Application::ActionTooltip(const char* id, const char* text) {
		if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			return;
		const Action* action = m_Keymap.Find(id);
		if (!action)
			return;
		const std::string key = Keymap::Format(m_Keymap.Key(id));
		const char* what = text ? text : action->Name;
		if (key.empty())
			ImGui::SetTooltip("%s", what);
		else
			ImGui::SetTooltip("%s  (%s)", what, key.c_str());
	}

	void Application::DrawAddItems() {
		for (const char* id : { "add.sphere", "add.box", "add.quad", "add.point_light", "add.spot_light", "add.model" })
			MenuAction(id);
	}

	// Edit > Duplicate (ADR 0027): la copia ha un nome unico, lo stesso materiale e, per una
	// mesh, lo stesso asset; si sposta lungo X di meta' della sua larghezza (una luce di 0,5)
	// e diventa la selezione.
	void Application::DuplicateSelection() {
		const int index = SelectedIndex();
		if (!m_Selection.IsValid() || index < 0)
			return;
		glm::vec3 boundsMin, boundsMax;
		GetSelectionBounds(boundsMin, boundsMax);
		const float width = boundsMax.x - boundsMin.x;
		const glm::vec3 offset(m_Selection.Type == SelectionType::Light || width <= 0.0f ? 0.5f : 0.5f * width, 0.0f, 0.0f);
		const std::string name = ObjectName(m_Selection);

		Selection copy;
		switch (m_Selection.Type) {
		case SelectionType::Sphere: {
			Sphere sphere = m_Scene.Spheres()[index];
			sphere.Position += glm::vec4(offset, 0.0f);
			copy = Select(SelectionType::Sphere, m_Scene.AddSphere(sphere));
			break;
		}
		case SelectionType::Quad: {
			Quad quad = m_Scene.Quads()[index];
			quad.PositionLLC += glm::vec4(offset, 0.0f);
			copy = Select(SelectionType::Quad, m_Scene.AddQuad(quad));
			break;
		}
		case SelectionType::Box: {
			const Box& box = m_Scene.Boxes()[index];
			m_Scene.CreateBox(glm::vec3(box.Min) + offset, glm::vec3(box.Max) + offset, box.Material);
			copy = Select(SelectionType::Box, static_cast<int>(m_Scene.Boxes().size()) - 1);
			break;
		}
		case SelectionType::Mesh: {
			Mesh mesh = m_Scene.Meshes()[index];
			mesh.Transform = glm::translate(glm::mat4(1.0f), offset) * mesh.Transform;
			copy = Select(SelectionType::Mesh, m_Scene.AddMesh(mesh, name));
			break;
		}
		case SelectionType::Light: {
			PunctualLight light = m_Scene.PunctualLights()[index];
			light.Position += offset;
			copy = Select(SelectionType::Light, m_Scene.AddLight(light, name));
			break;
		}
		default:
			return;
		}
		// la lista rende unico il nome: "Lamp" diventa "Lamp.001"
		if (m_Selection.Type == SelectionType::Sphere || m_Selection.Type == SelectionType::Quad || m_Selection.Type == SelectionType::Box)
			m_Scene.Rename(copy.Id, name);
		m_Selection = copy;
		m_GizmoOperation = ImGuizmo::TRANSLATE;
	}

	// Preferences > Keymap: la tabella delle azioni, raggruppata e filtrabile. Un clic sul tasto
	// aspetta il prossimo: Esc annulla, Backspace toglie la scorciatoia.
	void Application::DrawKeymapSection() {
		const float s = UI::Scale();

		// il tasto atteso, prima dei pulsanti: cosi' Spazio o Invio non ripremono quello appena cliccato
		if (!m_CapturingAction.empty()) {
			const Action* capturing = m_Keymap.Find(m_CapturingAction);
			for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END && capturing; key++) {
				const ImGuiKey k = static_cast<ImGuiKey>(key);
				if (IsModifier(k) || ImGui::IsMouseKey(k) || ImGui::IsGamepadKey(k) || !ImGui::IsKeyPressed(k, false))
					continue;
				if (k != ImGuiKey_Escape) {
					ImGuiKeyChord chord = ImGuiKey_None;
					if (k != ImGuiKey_Backspace) {
						// la navigazione e' fatta di tasti tenuti: niente modificatori
						const ImGuiKeyChord mods = ImGui::GetIO().KeyMods & (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt);
						chord = (capturing->Context == KeyContext::Navigation ? 0 : mods) | k;
					}
					const std::string lost = m_Keymap.Assign(m_CapturingAction, chord);
					if (!lost.empty())
						m_KeymapLost[lost] = Keymap::Format(chord);
					m_KeymapLost.erase(m_CapturingAction);
					m_Preferences.Keymap = m_Keymap.Overrides();
					m_Preferences.Save();
				}
				ConsumeKey(k);
				m_CapturingAction.clear();
				break;
			}
		}

		ImGui::SetNextItemWidth(-(ImGui::CalcTextSize(ICON_RESET " Reset all").x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x));
		ImGui::InputTextWithHint("##filter", "Filter by action or key", m_KeymapFilter, sizeof(m_KeymapFilter));
		ImGui::SameLine();
		if (UI::Pill(ICON_RESET " Reset all")) {
			m_Keymap.ResetAll();
			m_KeymapLost.clear();
			m_Preferences.Keymap.clear();
			m_Preferences.Save();
		}
		ImGui::TextDisabled("Click a key, then press the new one: Esc cancels, Backspace clears it.");

		auto contains = [](const std::string& text, const char* filter) {
			if (!filter[0])
				return true;
			std::string a = text, b = filter;
			for (char& c : a) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			for (char& c : b) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
			return a.find(b) != std::string::npos;
		};

		const ImGuiTableFlags flags = ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp;
		if (!ImGui::BeginTable("##keymap", 3, flags))
			return;
		ImGui::TableSetupScrollFreeze(0, 0);
		ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
		ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthFixed, 150.0f * s);
		ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
		const char* group = nullptr;
		for (const Action& action : m_Keymap.Actions()) {
			const ImGuiKeyChord key = m_Keymap.Key(action.Id);
			const std::string keyText = Keymap::Format(key);
			if (!contains(std::string(action.Group) + " " + action.Name, m_KeymapFilter) && !(m_KeymapFilter[0] && contains(keyText, m_KeymapFilter)))
				continue;
			if (!group || strcmp(group, action.Group) != 0) {
				group = action.Group;
				ImGui::TableNextRow();
				ImGui::TableNextColumn();
				ImGui::PushFont(UI::BoldFont());
				ImGui::TextUnformatted(action.Group);
				ImGui::PopFont();
				if (action.Context != KeyContext::Global) {
					ImGui::SameLine();
					ImGui::TextDisabled(action.Context == KeyContext::Profiler ? "with the Profiler focused" : "holding the right mouse button");
				}
			}
			ImGui::PushID(action.Id.c_str());
			ImGui::TableNextRow();
			ImGui::TableNextColumn();
			ImGui::AlignTextToFramePadding();
			ImGui::Indent(8.0f * s);
			ImGui::TextUnformatted(action.Name);
			ImGui::Unindent(8.0f * s);

			ImGui::TableNextColumn();
			const bool capturing = m_CapturingAction == action.Id;
			const auto lost = m_KeymapLost.find(action.Id);
			std::string label = capturing ? "Press a key..." : keyText.empty() ? (lost != m_KeymapLost.end() ? "none (was " + lost->second + ")" : "-") : keyText;
			ImGui::PushFont(UI::MonoFont());
			ImGui::PushStyleColor(ImGuiCol_Text, capturing ? Theme::Accent : lost != m_KeymapLost.end() ? Theme::Accent : keyText.empty() ? Theme::TextDim : ImGui::GetStyleColorVec4(ImGuiCol_Text));
			if (ImGui::Button((label + "##key").c_str(), ImVec2(-FLT_MIN, 0.0f)))
				m_CapturingAction = capturing ? std::string() : action.Id;
			ImGui::PopStyleColor();
			ImGui::PopFont();

			ImGui::TableNextColumn();
			if (key != action.Default) {
				if (ImGui::Button(ICON_RESET "##default", ImVec2(ImGui::GetFrameHeight(), 0.0f))) {
					const std::string taken = m_Keymap.Assign(action.Id, action.Default);
					if (!taken.empty())
						m_KeymapLost[taken] = Keymap::Format(action.Default);
					m_KeymapLost.erase(action.Id);
					m_Preferences.Keymap = m_Keymap.Overrides();
					m_Preferences.Save();
				}
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Back to %s", action.Default == ImGuiKey_None ? "no key" : Keymap::Format(action.Default).c_str());
			}
			ImGui::PopID();
		}
		ImGui::EndTable();
	}
}
