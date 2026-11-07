#include "stb/stb_image_write.h"
#include "Editor/Application.h"
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include "imgui_internal.h"
#include "Editor/Input.h"
#include "Editor/Icons.h"
#include "Editor/Theme.h"
#include "Editor/Widgets.h"
#include "Core/Random.h"
#include "Serialization/SceneSerializer.h"
#include <functional>
#include "Import/SceneFiles.h"
#include "Serialization/BuiltinScenes.h"
#include "Import/Importer.h"
#include "vendor/portable-file-dialogs.h"

namespace Lucerna {

	// il testo prima di ### e' il titolo, il resto e' l'ID stabile
	static const char* kDeletePopupId = "Delete object?###DeleteObject";
	static const char* kSavePromptId = "Save changes?###SaveChanges";

	static Application* s_Instance = nullptr;

	namespace {
		// Su Wayland il cursore lo disegna l'app, e GLFW sceglie il tema solo da XCURSOR_THEME e
		// XCURSOR_SIZE. GNOME non le imposta (tiene il tema nelle sue impostazioni): senza, si
		// vedrebbe il tema di default invece di quello scelto dall'utente. Le si ricava da
		// gsettings prima di glfwInit; dove il desktop le imposta gia' (KDE) non si tocca niente.
		void UseDesktopCursorTheme() {
#ifdef __linux__
			auto gsetting = [](const char* key) {
				const std::string command = std::string("gsettings get org.gnome.desktop.interface ") + key + " 2>/dev/null";
				std::string value;
				if (FILE* pipe = popen(command.c_str(), "r")) {
					char buffer[256];
					while (fgets(buffer, sizeof(buffer), pipe))
						value += buffer;
					pclose(pipe);
				}
				// "'Bibata-Modern-Ice'\n" -> "Bibata-Modern-Ice"; un numero resta com'e'
				value.erase(std::remove_if(value.begin(), value.end(), [](char c) { return c == '\'' || c == '\n'; }), value.end());
				return value;
			};
			if (getenv("XCURSOR_THEME") == nullptr)
				if (const std::string theme = gsetting("cursor-theme"); !theme.empty())
					setenv("XCURSOR_THEME", theme.c_str(), 0);
			if (getenv("XCURSOR_SIZE") == nullptr)
				if (const std::string size = gsetting("cursor-size"); !size.empty() && std::isdigit(static_cast<unsigned char>(size[0])))
					setenv("XCURSOR_SIZE", size.c_str(), 0);
#endif
		}

		// Proietta un punto world sullo schermo. Ritorna false se sta dietro la camera:
		// dividere per una w negativa produrrebbe un punto ribaltato davanti all'occhio.
		bool WorldToScreen(const glm::mat4& viewProjection, const glm::vec3& point,
						   const ImVec2& viewportPos, const ImVec2& viewportSize, ImVec2& outScreen) {
			glm::vec4 clip = viewProjection * glm::vec4(point, 1.0f);
			if (clip.w <= 1e-6f)
				return false;

			glm::vec3 ndc = glm::vec3(clip) / clip.w;
			outScreen = ImVec2(viewportPos.x + (ndc.x * 0.5f + 0.5f) * viewportSize.x,
							   viewportPos.y + (1.0f - (ndc.y * 0.5f + 0.5f)) * viewportSize.y);
			return true;
		}

	}

	Application::Application() : m_Window(nullptr), m_Height(900), m_Width(1600), m_IsRunning(true),
								 m_Camera(45.0f, 0.1f, 100.0f) {
		s_Instance = this;
		// Prima delle scene: aprire un file aggiorna i file recenti, che stanno nelle preferenze
		m_Preferences = Preferences::Load();

		// Le scene integrate sono file (issue #28). PT_SCENE=<numero o nome> sceglie quella di
		// partenza; senza, Setup 2.
		m_BuiltinScenes = ListBuiltinScenes();
		const char* startScene = getenv("PT_SCENE");
		if (const BuiltinScene* scene = FindBuiltinScene(m_BuiltinScenes, startScene ? startScene : "setup-2"))
			LoadBuiltinScene(static_cast<int>(scene - m_BuiltinScenes.data()));
		else {
			std::cerr << "PT_SCENE: nessuna scena '" << startScene << "'. Ci sono:";
			for (const BuiltinScene& b : m_BuiltinScenes)
				std::cerr << " " << b.Number << " (" << b.Key << ")";
			std::cerr << std::endl;
		}

		// Test/CI headless: PT_GLTF=<file> o PT_SCENE_FILE=<file.json> aprono un file al posto
		// della scena di default; il documento li apre entrambi, in qualunque formato
		if (const char* g = getenv("PT_GLTF"))
			OpenDocument(g);
		if (const char* f = getenv("PT_SCENE_FILE"))
			OpenDocument(f);

		// Test/CI headless: PT_NEE=0 forza la NEE spenta (A/B col light sampling).
		// PT_CAM=x,y,z,dx,dy,dz: posizione e direzione della camera, per catture ripetibili
		if (const char* c = getenv("PT_CAM")) {
			glm::vec3 pos, dir;
			if (sscanf(c, "%f,%f,%f,%f,%f,%f", &pos.x, &pos.y, &pos.z, &dir.x, &dir.y, &dir.z) == 6) {
				m_Scene.MoveCamera(pos, glm::normalize(dir)); // e' l'inquadratura: la vista la segue in SyncCameras
			}
		}

		if (const char* a = getenv("PT_AOV"))
			m_Renderer.View.AOVView = atoi(a);

		if (const char* n = getenv("PT_NEE"))
			m_Renderer.Settings.NEE = (atoi(n) != 0);
		// Test/CI headless: PT_DEPTH=<n> forza la profondita' dei rimbalzi (2 = solo diretta).
		if (const char* d = getenv("PT_DEPTH"))
			m_Renderer.Settings.RayDepth = atoi(d);
		// Test/CI headless: PT_REPROJ=0 spegne la riproiezione temporale (misura del suo costo).
		if (const char* r = getenv("PT_REPROJ"))
			m_Renderer.Settings.Reproject = (atoi(r) != 0);
		// Test/CI headless: PT_DENOISE=1 accende il denoiser (misura del suo costo).
		if (const char* dn = getenv("PT_DENOISE"))
			m_Renderer.Settings.Denoise = (atoi(dn) != 0);
	}

	Application::~Application() {
		//Shutdown();
		m_Scene.Clear();
		m_Assets.Clear();
		s_Instance = nullptr;
	}

	Application &Application::Get() {
		return *s_Instance;
	}

	bool Application::Initialize(int width, int height) {
		m_Width = width;
		m_Height = height;

		// PT_PLATFORM=x11|wayland sceglie la piattaforma di GLFW; senza, Wayland se c'e'. La
		// cattura dell'intera UI da script usa x11 (vedi CLAUDE.md).
		if (const char* platform = getenv("PT_PLATFORM")) {
			const std::string name = platform;
			glfwInitHint(GLFW_PLATFORM, name == "x11" ? GLFW_PLATFORM_X11 : name == "wayland" ? GLFW_PLATFORM_WAYLAND : GLFW_ANY_PLATFORM);
		}
		UseDesktopCursorTheme();
		if (!glfwInit())
			return false;

		// La barra del titolo e' quella dei menu (ADR 0021), dove GLFW sa chiedere al sistema di
		// spostare e ridimensionare la finestra; altrove, o se l'utente la preferisce, resta la
		// cornice del sistema
		const int platform = glfwGetPlatform();
		const bool canMoveItself = platform == GLFW_PLATFORM_WAYLAND || platform == GLFW_PLATFORM_X11 || platform == GLFW_PLATFORM_WIN32;
		m_CustomTitleBar = canMoveItself && !m_Preferences.SystemTitleBar;
		glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
		glfwWindowHint(GLFW_DECORATED, m_CustomTitleBar ? GLFW_FALSE : GLFW_TRUE);
		m_Window = glfwCreateWindow(width, height, "Lucerna", nullptr, nullptr);

		if (m_Window == nullptr)
			return false;

		// Test/CI headless: PT_NOVSYNC=1. Con la vsync il driver abbassa i clock per riempire il
		// frame, quindi i tempi GPU non misurano il costo reale delle passate.
		m_Vsync = m_Preferences.Vsync && getenv("PT_NOVSYNC") == nullptr;
		if (!m_Vulkan.Initialize(m_Window, m_Vsync))
			return false;

		// Setup Dear ImGui context
		IMGUI_CHECKVERSION();
		ImGui::CreateContext();
		ImGuiIO& io = ImGui::GetIO(); (void)io;
		io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
		io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
		// Le finestre staccate di ImGui vanno posizionate, e Wayland non lo permette alle app
		if (platform != GLFW_PLATFORM_WAYLAND)
			io.ConfigFlags |= ImGuiConfigFlags_ViewportsEnable;

		// L'aspetto del redesign (concept A): font, palette e spaziature alla scala del monitor
		ApplyUiScale();
		glfwSetWindowContentScaleCallback(m_Window, &Application::OnContentScale);

		ImGui_ImplGlfw_InitForVulkan(m_Window, true);

		// Le finestre staccate di ImGui hanno ognuna la sua swapchain e i suoi set: da qui la
		// dimensione generosa del pool.
		VkDescriptorPoolSize poolSize{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 };
		VkDescriptorPoolCreateInfo poolInfo{ VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
		poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		poolInfo.maxSets = 256;
		poolInfo.poolSizeCount = 1;
		poolInfo.pPoolSizes = &poolSize;
		vkCreateDescriptorPool(m_Vulkan.Device(), &poolInfo, nullptr, &m_ImGuiPool);

		VkFormat swapchainFormat = m_Vulkan.SwapchainFormat();
		ImGui_ImplVulkan_InitInfo initInfo{};
		initInfo.Instance = m_Vulkan.Instance();
		initInfo.PhysicalDevice = m_Vulkan.PhysicalDevice();
		initInfo.Device = m_Vulkan.Device();
		initInfo.QueueFamily = m_Vulkan.QueueFamily();
		initInfo.Queue = m_Vulkan.Queue();
		initInfo.DescriptorPool = m_ImGuiPool;
		initInfo.MinImageCount = m_Vulkan.SwapchainImageCount();
		initInfo.ImageCount = m_Vulkan.SwapchainImageCount();
		initInfo.UseDynamicRendering = true;
		initInfo.ColorAttachmentFormat = swapchainFormat;
		ImGui_ImplVulkan_Init(&initInfo, VK_NULL_HANDLE);
		m_ImGuiVulkanInfo = initInfo;

		m_Vulkan.Immediate([](VkCommandBuffer cmd) { ImGui_ImplVulkan_CreateFontsTexture(cmd); });
		ImGui_ImplVulkan_DestroyFontUploadObjects();

		return true;
	}

	void Application::RunLoop() {
		ImGuiIO& io = ImGui::GetIO();

		m_Camera.OnResize(m_Width, m_Height);
		m_Renderer.OnResize(m_Width, m_Height);
		m_Renderer.Initialize();

		while (m_IsRunning) {
			glfwPollEvents();

			// Chiudere la finestra chiede prima di salvare, se ci sono modifiche
			if (glfwWindowShouldClose(m_Window)) {
				glfwSetWindowShouldClose(m_Window, GLFW_FALSE);
				WhenSaved([this] { m_IsRunning = false; });
			}

			if (!m_Vulkan.BeginFrame())
				continue;

			// La camera che si muove NON azzera piu' l'accumulo: la riproiezione temporale
			// riusa il frame precedente invece di buttarlo (ADR 0002). Il reset resta per i
			// cambi di scena/materiale, dove la storia e' genuinamente invalida.
			// SetInteracting dice al renderer che la storia va cappata (ghosting) e che puo'
			// abbassare la risoluzione (render scale).
			bool cameraMoved = m_Camera.OnUpdate(m_DeltaTime);
			SyncCameras(cameraMoved);
			m_Renderer.SetInteracting(cameraMoved);

			// Live-link glTF: ricarica la scena se il file e' stato ri-esportato da Blender.
			PollLiveLink();

			if (m_RescaleRequested)
				RebuildUiForScale();

			// Start the Dear ImGui frame
			ImGui_ImplVulkan_NewFrame();
			ImGui_ImplGlfw_NewFrame();

			ImGui::NewFrame();
			ImGuizmo::BeginFrame();

			HandleWindowEdges();
			// Prima del dockspace: la barra di menu riduce l'area di lavoro che il dockspace occupa
			DrawMainMenu();
			DrawStatusBar();
			HandleShortcuts();

			{
				static ImGuiDockNodeFlags dockspace_flags = ImGuiDockNodeFlags_None;

				ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoDocking;

				const ImGuiViewport* viewport = ImGui::GetMainViewport();
				ImGui::SetNextWindowPos(viewport->WorkPos);
				ImGui::SetNextWindowSize(viewport->WorkSize);
				ImGui::SetNextWindowViewport(viewport->ID);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
				ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
				window_flags |= ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove;
				window_flags |= ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus;

				if (dockspace_flags & ImGuiDockNodeFlags_PassthruCentralNode)
					window_flags |= ImGuiWindowFlags_NoBackground;

				ImGui::Begin("DockSpace Demo", nullptr, window_flags);

				ImGui::PopStyleVar(2);

				// Submit the DockSpace
				ImGuiIO& io = ImGui::GetIO();
				if (io.ConfigFlags & ImGuiConfigFlags_DockingEnable) {
					ImGuiID dockspace_id = ImGui::GetID("OpenGL Dockspace"); // il nome resta: e' la chiave del layout salvato in imgui.ini
					// Il layout del concept A la prima volta che i suoi pannelli compaiono (un
					// imgui.ini di prima non li conosce), e quando lo si chiede dal menu View
					if (m_ResetLayout || ImGui::FindWindowSettingsByID(ImHashStr("Outliner")) == nullptr)
						BuildDefaultLayout(dockspace_id);
					m_ResetLayout = false;
					ImGui::DockSpace(dockspace_id, ImVec2(0.0f, 0.0f), dockspace_flags);
				}

				ImGui::End();
			}

			// I pannelli; la finestra "Viewport" disegna anche overlay, HUD e gizmo
			RenderUI(m_DeltaTime);
			UpdateSelection();
			// dopo UpdateSelection: e' li' che Canc chiama OpenPopup, cosi' la modale
			// compare nello stesso frame in cui viene richiesta
			DrawDeletePopup();
			DrawSavePrompt();
			UpdateWindowTitle();
			Render(m_DeltaTime);

			ImGui::Render();

			m_Vulkan.BeginUiPass();
			ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), m_Vulkan.Cmd());
			m_Vulkan.EndUiPass();
			m_Vulkan.SubmitFrame();

			if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable) {
				ImGui::UpdatePlatformWindows();
				ImGui::RenderPlatformWindowsDefault();
			}

			m_Vulkan.PresentFrame();

			CalculateTime();
			HandleCaptures();
		}
	}

	// Va dopo la submit: le immagini si leggono solo a frame eseguito.
	void Application::HandleCaptures() {
		if (m_ScreenshotRequested) {
			m_Renderer.SaveScreenshot();
			m_ScreenshotRequested = false;
		}

		// Cattura headless: PT_SHOT=<file> PT_SHOT_FRAME=<n> salva dopo n frame accumulati
		// e chiude. Serve a ispezionare il render senza guardare lo schermo (test, CI).
		if (const char* shot = getenv("PT_SHOT")) {
			static int shotFrame = getenv("PT_SHOT_FRAME") ? atoi(getenv("PT_SHOT_FRAME")) : 200;
			static int n = 0;
			if (++n >= shotFrame) {
				m_Renderer.SaveScreenshot(shot);
				// PT_HDR=<file>: dump della beauty LINEARE (rgba32f, pre-tonemap) come raw
				// float32. Serve a confronti non falsati da tonemap/clipping a 8 bit.
				if (const char* hdr = getenv("PT_HDR"))
					m_Renderer.SaveHdr(hdr);
				m_IsRunning = false;
			}
		}
	}

	// ---------------------------------------------------------------- documento

	// Apre un file come documento: un .json e' una scena, ogni altro formato passa dagli importer
	// (issue #27). Se il file non si legge scena e documento restano come erano. Un file importato
	// si sorveglia per il live-link: da qui in poi PollLiveLink lo riapre quando cambia.
	bool Application::OpenDocument(const std::string& path) {
		m_Selection = {};
		if (!m_Document.Open(path, m_Scene, m_Assets, m_Renderer.Settings))
			return false;

		m_CurrentScene = -1;
		m_WatchPath.clear();
		if (m_Document.Origin() == DocumentOrigin::Imported) {
			m_WatchPath = m_Document.Path();
			std::error_code ec;
			m_WatchMtime = std::filesystem::last_write_time(m_WatchPath, ec); // ec ignorato: se fallisce, 0 -> ricarica al primo poll
		}
		RememberRecent(m_Document.Path());
		return true;
	}

	// I file aperti o salvati finiscono in Open Recent. Le esecuzioni da script non lo toccano.
	void Application::RememberRecent(const std::string& path) {
		if (getenv("PT_SHOT"))
			return;
		m_Preferences.AddRecent(path);
		m_Preferences.Save();
	}

	// Una scena integrata e' un file di scena come gli altri, ma un modello: Save chiede dove
	// salvare. La selezione si azzera prima: la finestra "Viewport" viene disegnata piu' avanti
	// in questo stesso frame e userebbe gli indici della scena vecchia.
	bool Application::LoadBuiltinScene(int index) {
		m_Selection = {};
		if (!m_Document.OpenBuiltin(m_BuiltinScenes[index], m_Scene, m_Assets, m_Renderer.Settings))
			return false;
		m_CurrentScene = index;
		m_WatchPath.clear(); // il live-link e' dei file importati
		return true;
	}

	void Application::NewDocument() {
		m_Selection = {};
		m_Document.New(m_Scene, m_Assets, m_Renderer.Settings);
		m_CurrentScene = -1;
		m_WatchPath.clear();
	}

	// Save riscrive il file di scena aperto; una scena nuova, integrata o importata chiede dove
	// salvare. false = non salvato (dialog annullato o scrittura fallita).
	bool Application::SaveDocument() {
		if (m_Document.CanSaveInPlace())
			return m_Document.Save(m_Scene, m_Assets, m_Renderer.Settings);
		return SaveDocumentAs();
	}

	bool Application::SaveDocumentAs() {
		const std::filesystem::path current(m_Document.Path());
		const std::string suggested = m_Document.CanSaveInPlace() ? current.string()
			: (std::filesystem::path("scenes") / (current.empty() ? std::string("untitled") : current.stem().string())).string() + ".json";
		std::string path = pfd::save_file("Save scene", suggested, { "Scene", "*.json" }).result();
		if (path.empty())
			return false;
		if (std::filesystem::path(path).extension() != ".json")
			path += ".json";
		if (!m_Document.SaveAs(path, m_Scene, m_Assets, m_Renderer.Settings))
			return false;
		m_CurrentScene = -1;
		m_WatchPath.clear(); // ora il documento e' il .json, non il file importato
		RememberRecent(m_Document.Path());
		return true;
	}

	// Un'azione che butterebbe via le modifiche non salvate (New, Open, chiudere, il live-link)
	// passa da qui: senza modifiche parte subito, altrimenti prima si chiede. Le esecuzioni da
	// script (PT_SHOT) non chiedono mai.
	void Application::WhenSaved(std::function<void()> action) {
		if (!m_Document.IsModified(m_Scene, m_Assets, m_Renderer.Settings) || getenv("PT_SHOT")) {
			action();
			return;
		}
		m_PendingAction = std::move(action);
		m_SavePromptRequested = true;
	}

	// Modale: va aperta fuori dal Begin/End di qualsiasi finestra, come quella di Delete
	void Application::DrawSavePrompt() {
		if (m_SavePromptRequested) {
			m_SavePromptRequested = false;
			ImGui::OpenPopup(kSavePromptId);
		}
		ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
		if (!ImGui::BeginPopupModal(kSavePromptId, nullptr, ImGuiWindowFlags_AlwaysAutoResize))
			return;

		ImGui::Text("Save changes to %s?", m_Document.DisplayName().c_str());
		ImGui::TextDisabled("Your changes will be lost if you don't save them.");
		ImGui::Spacing();
		std::function<void()> run;
		if (ImGui::Button("Save", ImVec2(120, 0))) {
			if (SaveDocument())
				run = std::move(m_PendingAction);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SetItemDefaultFocus();
		ImGui::SameLine();
		if (ImGui::Button("Don't Save", ImVec2(120, 0))) {
			run = std::move(m_PendingAction);
			ImGui::CloseCurrentPopup();
		}
		ImGui::SameLine();
		if (ImGui::Button("Cancel", ImVec2(120, 0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
			m_PendingAction = {};
			ImGui::CloseCurrentPopup();
		}
		ImGui::EndPopup();
		if (run)
			run();
	}

	void Application::ShowOpenDialog() {
		std::vector<std::string> filters = FileFilters();
		filters[1] += " *.json";                                   // "Supported files" comprende le scene
		filters.insert(filters.begin() + 2, { "Scene", "*.json" });
		const auto selection = pfd::open_file("Open", "scenes", filters).result();
		if (!selection.empty())
			WhenSaved([this, path = selection[0]] { OpenDocument(path); });
	}

	// Il menu File: resta anche dopo il redesign della UI
	void Application::DrawMainMenu() {
		if (!ImGui::BeginMainMenuBar())
			return;
		ImGui::PushFont(UI::BoldFont());
		ImGui::TextColored(Theme::Accent, "Lucerna");
		ImGui::PopFont();
		if (ImGui::BeginMenu("File")) {
			if (ImGui::MenuItem(ICON_NEW " New", "Ctrl+N"))
				WhenSaved([this] { NewDocument(); });
			if (ImGui::MenuItem(ICON_OPEN " Open...", "Ctrl+O"))
				ShowOpenDialog();
			if (ImGui::BeginMenu("Open Recent")) {
				// un file che non c'e' piu' sparisce dall'elenco
				std::vector<std::string>& recent = m_Preferences.RecentFiles;
				const size_t before = recent.size();
				recent.erase(std::remove_if(recent.begin(), recent.end(),
					[](const std::string& f) { return !std::filesystem::exists(f); }), recent.end());
				if (recent.size() != before)
					m_Preferences.Save();
				if (recent.empty())
					ImGui::TextDisabled("no recent files");
				for (const std::string& file : recent)
					if (ImGui::MenuItem(file.c_str()))
						WhenSaved([this, path = file] { OpenDocument(path); });
				ImGui::EndMenu();
			}
			if (ImGui::BeginMenu("Built-in Scenes")) {
				for (int i = 0; i < static_cast<int>(m_BuiltinScenes.size()); i++)
					if (ImGui::MenuItem(m_BuiltinScenes[i].DisplayName.c_str(), nullptr, i == m_CurrentScene))
						WhenSaved([this, i] { LoadBuiltinScene(i); });
				ImGui::EndMenu();
			}
			ImGui::Separator();
			if (ImGui::MenuItem(ICON_SAVE " Save", "Ctrl+S"))
				SaveDocument();
			if (ImGui::MenuItem("Save As...", "Ctrl+Shift+S"))
				SaveDocumentAs();
			ImGui::Separator();
			// il live-link riapre il file importato quando Blender lo riesporta (ADR 0003)
			if (ImGui::MenuItem("Live-link", nullptr, &m_LiveLink, !m_WatchPath.empty())) {}
			if (ImGui::MenuItem(ICON_SCREENSHOT " Save screenshot"))
				m_ScreenshotRequested = true;
			ImGui::Separator();
			if (ImGui::MenuItem("Quit", "Ctrl+Q"))
				WhenSaved([this] { m_IsRunning = false; });
			ImGui::EndMenu();
		}
		DrawEditMenu();
		DrawAddMenu();
		DrawViewMenu();
		DrawHelpMenu();

		DrawWindowControls();
		ImGui::EndMainMenuBar();
	}

	// Le scorciatoie del menu File. Non mentre si scrive in un campo di testo.
	void Application::HandleShortcuts() {
		const ImGuiIO& io = ImGui::GetIO();
		if (!io.KeyCtrl || io.WantTextInput)
			return;
		if (ImGui::IsKeyPressed(ImGuiKey_N, false))
			WhenSaved([this] { NewDocument(); });
		else if (ImGui::IsKeyPressed(ImGuiKey_O, false))
			ShowOpenDialog();
		else if (ImGui::IsKeyPressed(ImGuiKey_S, false))
			io.KeyShift ? SaveDocumentAs() : SaveDocument();
		else if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
			WhenSaved([this] { m_IsRunning = false; });
	}

	// "Lucerna - sponza.json*": il file aperto, e un asterisco se ci sono modifiche non salvate
	void Application::UpdateWindowTitle() {
		const std::string title = "Lucerna - " + m_Document.DisplayName()
			+ (m_Document.IsModified(m_Scene, m_Assets, m_Renderer.Settings) ? "*" : "");
		if (title != m_WindowTitle) {
			glfwSetWindowTitle(m_Window, title.c_str());
			m_WindowTitle = title;
		}
	}

	// Il filtro del file dialog: le estensioni di tutti gli importer, poi un formato per riga
	std::vector<std::string> Application::FileFilters() {
		std::vector<std::string> filters;
		std::string all;
		for (const auto& importer : Importers()) {
			std::string patterns;
			for (const std::string& e : importer->Extensions())
				patterns += (patterns.empty() ? "*" : " *") + e;
			all += (all.empty() ? "" : " ") + patterns;
			filters.push_back(importer->Name());
			filters.push_back(patterns);
		}
		filters.insert(filters.begin(), { "Supported files", all });
		filters.push_back("All files");
		filters.push_back("*");
		return filters;
	}

	// Live-link: se il file aperto e' cambiato (Blender l'ha ri-esportato su save), lo riapre.
	// Chiamato ogni frame; il costo a regime e' una sola stat() del file.
	void Application::PollLiveLink() {
		if (!m_LiveLink || m_WatchPath.empty())
			return;

		std::error_code ec;
		auto mtime = std::filesystem::last_write_time(m_WatchPath, ec);
		if (ec || mtime == m_WatchMtime)
			return; // file assente/in scrittura, o invariato

		// Aggiorna il timestamp PRIMA di ricaricare: se il parse fallisce (export a meta',
		// file troncato) Open lascia la scena com'e' e non ri-tenta all'infinito sullo stesso
		// timestamp; ci riprovera' al prossimo cambio di mtime (Blender riscrive).
		// Con modifiche non salvate si chiede: Cancel salta questo export e resta in ascolto.
		m_WatchMtime = mtime;
		std::cout << "Live-link: rilevato cambio, riapro " << m_WatchPath << std::endl;
		WhenSaved([this, path = m_WatchPath] { OpenDocument(path); });
	}

	void Application::CalculateTime() {
		float time = glfwGetTime();
		m_Timer = time - m_ResetTimer;
		m_DeltaTime = time - m_LastFrameTime;
		m_NFrames++;

		if (m_Timer >= 1.0) {
			m_FPS = (double)m_NFrames / m_Timer;
			if (getenv("PT_LOG_FPS")) std::cout << "PTFPS " << m_FPS << " pt_ms " << m_Renderer.GetPathTraceMs()
			                                    << " dn_ms " << m_Renderer.GetDenoiseMs() << std::endl; // TEMP: misura headless
			m_NFrames = 0;
			m_ResetTimer = time;
		}
		
		m_LastFrameTime = time;
	}

	void Application::Shutdown() {
		if (m_Vulkan.Device() != VK_NULL_HANDLE) {
			vkDeviceWaitIdle(m_Vulkan.Device());
			if (m_DisplaySet != VK_NULL_HANDLE)
				ImGui_ImplVulkan_RemoveTexture(m_DisplaySet);
			m_Renderer.Release();
			ImGui_ImplVulkan_Shutdown();
			ImGui_ImplGlfw_Shutdown();
			ImGui::DestroyContext();
			vkDestroyDescriptorPool(m_Vulkan.Device(), m_ImGuiPool, nullptr);
		}
		m_Vulkan.Shutdown();

		glfwDestroyWindow(m_Window);
		glfwTerminate();

		m_IsRunning = false;
	}

	void Application::DrawSelectionProperties() {
		if (!m_Selection.IsValid()) {
			ImGui::TextDisabled("No object selected.");
			ImGui::TextDisabled("Click in the viewport, or on an outliner row.");
			return;
		}

		ImGui::PushItemWidth(-FLT_MIN);

		// il nome si rinomina qui; e' unico nella lista del suo tipo (issue #23)
		{
			NameField(m_Selection.Id, ObjectName(m_Selection));
		}

		switch (m_Selection.Type) {
		case SelectionType::Sphere: {
			Sphere sphere = m_Scene.Spheres().at(SelectedIndex());
			bool changed = UI::DragFloat3("Center", glm::value_ptr(sphere.Position), 0.1f);
			changed |= UI::DragFloat("Radius", &sphere.Position.w, 0.05f, 0.001f, 1000.0f);
			changed |= MaterialCombo("Material", sphere.Material);
			if (changed)
				m_Scene.Set(m_Selection.Id, sphere);
			break;
		}
		case SelectionType::Quad: {
			Quad quad = m_Scene.Quads().at(SelectedIndex());
			bool changed = UI::DragFloat3("Corner", glm::value_ptr(quad.PositionLLC), 0.1f);
			changed |= UI::DragFloat3("U axis", glm::value_ptr(quad.U), 0.05f);
			changed |= UI::DragFloat3("V axis", glm::value_ptr(quad.V), 0.05f);
			changed |= UI::DragFloat("Width", &quad.Width, 0.1f, 0.001f, 1000.0f);
			changed |= UI::DragFloat("Height", &quad.Height, 0.1f, 0.001f, 1000.0f);
			changed |= MaterialCombo("Material", quad.Material);
			if (changed)
				m_Scene.Set(m_Selection.Id, quad);
			ImGui::TextDisabled("pivot = lower-left corner");
			break;
		}
		case SelectionType::Box: {
			Box box = m_Scene.Boxes().at(SelectedIndex());

			bool changed = false;
			changed |= UI::DragFloat3("Min", glm::value_ptr(box.Min), 0.1f);
			changed |= UI::DragFloat3("Max", glm::value_ptr(box.Max), 0.1f);
			changed |= MaterialCombo("Material", box.Material);

			if (changed)
				m_Scene.Set(m_Selection.Id, box); // riscrive anche i suoi 6 quad
			ImGui::TextDisabled("axis-aligned: cannot be rotated");
			break;
		}
		case SelectionType::Mesh: {
			Mesh mesh = m_Scene.Meshes().at(SelectedIndex());
			const MeshAsset* asset = m_Assets.FindMesh(mesh.Asset);
			ImGui::Text("Triangles: %d", asset ? static_cast<int>(asset->Triangles.size()) : 0);
			// le istanze dello stesso asset dividono triangoli e BLAS (issue #26)
			ImGui::TextDisabled("mesh asset: %s", asset ? m_Assets.FindMesh(mesh.Asset)->Name.c_str() : "missing");

			glm::vec3 position = glm::vec3(mesh.Transform[3]); // la traslazione e' l'ultima colonna
			bool changed = UI::DragFloat3("Position", glm::value_ptr(position), 0.1f);
			if (changed)
				mesh.Transform[3] = glm::vec4(position, 1.0f);
			changed |= MaterialCombo("Material", mesh.Material);
			if (ImGui::Button("Reset transform")) {
				mesh.Transform = glm::mat4(1.0f);
				changed = true;
			}
			if (changed)
				m_Scene.Set(m_Selection.Id, mesh);
			break;
		}
		case SelectionType::Light: {
			PunctualLight light = m_Scene.PunctualLights().at(SelectedIndex());
			bool changed = false;
			int type = light.Spot ? 1 : 0;
			if (UI::Combo("Type", &type, "Point\0Spot\0\0")) {
				light.Spot = type == 1;
				changed = true;
			}
			changed |= UI::DragFloat3("Position", glm::value_ptr(light.Position), 0.05f);
			changed |= UI::ColorEdit3("Color", glm::value_ptr(light.Color));
			changed |= UI::DragFloat("Intensity", &light.Intensity, 0.1f, 0.0f, 1e6f, "%.2f W/sr", ImGuiSliderFlags_Logarithmic);
			ImGui::TextDisabled("power %.1f W, as a Blender lamp", light.Intensity * 4.0f * 3.14159265f);
			if (light.Spot) {
				if (UI::DragFloat3("Direction", glm::value_ptr(light.Direction), 0.01f)) {
					if (glm::length(light.Direction) < 1e-4f)
						light.Direction = { 0.0f, -1.0f, 0.0f };
					light.Direction = glm::normalize(light.Direction);
					changed = true;
				}
				changed |= UI::SliderFloat("Outer angle", &light.OuterAngle, 0.5f, 90.0f, "%.1f deg");
				changed |= UI::SliderFloat("Inner angle", &light.InnerAngle, 0.0f, light.OuterAngle, "%.1f deg");
				light.InnerAngle = glm::min(light.InnerAngle, light.OuterAngle);
			}
			if (changed)
				m_Scene.Set(m_Selection.Id, light);
			break;
		}
		default:
			break;
		}

		ImGui::PopItemWidth();

		ImGui::Spacing();
		if (ImGui::Button("Deselect (ESC)"))
			m_Selection = {};

		ImGui::SameLine();
		if (ImGui::Button("Delete (Del)"))
			m_DeleteRequested = true;
	}

	// Un DragFloat sull'indice del materiale non dice niente su cosa si sta scegliendo:
	// qui ogni voce mostra la pastiglia col colore del materiale.
	bool Application::MaterialCombo(const char* label, ObjectId& material) {
		if (m_Assets.Materials().empty())
			return false;

		const int count = static_cast<int>(m_Assets.Materials().size());
		const int current = m_Assets.Materials().IndexOf(material);

		const std::string preview = current >= 0 ? m_Assets.Materials()[current].Name : "(none)";
		bool changed = false;

		UI::Label(label);
		ImGui::SetNextItemWidth(-(ImGui::GetFrameHeight() + ImGui::GetStyle().ItemSpacing.x));
		if (ImGui::BeginCombo((std::string("##") + label).c_str(), preview.c_str())) {
			for (int i = 0; i < count; i++) {
				ImGui::PushID(i);

				const glm::vec4& color = m_Assets.Materials()[i].Color;
				ImGui::ColorButton("##swatch", ImVec4(color.r, color.g, color.b, 1.0f),
					ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop, ImVec2(14.0f, 14.0f));
				ImGui::SameLine();

				const std::string& item = m_Assets.Materials()[i].Name;
				if (ImGui::Selectable(item.c_str(), i == current)) {
					material = m_Assets.Materials()[i].Id;
					changed = true;
				}

				ImGui::PopID();
			}
			ImGui::EndCombo();
		}

		// Scorciatoia verso l'editor di QUESTO materiale: evita di cambiare tab,
		// ritrovare l'indice giusto e riaprire l'albero.
		ImGui::SameLine();
		ImGui::PushID(label);
		// apre questo materiale nella sezione Material delle proprieta'
		if (ImGui::Button(ICON_MATERIAL, ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())) && current >= 0) {
			m_EditedMaterial = m_Assets.Materials()[current].Id;
			m_PropertiesTab = PropertiesTab::Material;
		}
		if (ImGui::IsItemHovered())
			ImGui::SetTooltip("Edit this material");
		ImGui::PopID();

		return changed;
	}

	// Le texture di un tipo, piu' "None". Un materiale cita la texture per id (issue #26).
	bool Application::TextureCombo(const char* label, ObjectId& texture, TextureKind kind) {
		const int current = m_Assets.Textures().IndexOf(texture);
		bool changed = false;
		UI::Label(label);
		if (ImGui::BeginCombo((std::string("##") + label).c_str(), current >= 0 ? m_Assets.Textures()[current].Name.c_str() : "None")) {
			if (ImGui::Selectable("None", current < 0)) {
				texture = {};
				changed = true;
			}
			for (int i = 0; i < static_cast<int>(m_Assets.Textures().size()); i++) {
				const auto& t = m_Assets.Textures()[i];
				if (t.Kind != kind)
					continue;
				ImGui::PushID(i);
				if (ImGui::Selectable(t.Name.c_str(), i == current)) {
					texture = t.Id;
					changed = true;
				}
				ImGui::PopID();
			}
			ImGui::EndCombo();
		}
		return changed;
	}

	// Nuovi oggetti nascono davanti alla camera, non nell'origine: altrimenti finirebbero
	// fuori vista e sembrerebbe che il pulsante non abbia fatto niente.
	glm::vec3 Application::SpawnPosition() const {
		return m_Camera.GetPosition() + m_Camera.GetDirection() * 4.0f;
	}

	// Il materiale scelto per i nuovi oggetti, se esiste ancora; altrimenti il primo, creandolo
	// se la scena non ne ha.
	ObjectId Application::EnsureMaterialExists() {
		if (m_Assets.Materials().empty())
			m_Assets.AddMaterial(CreateDefaultPrincipled());
		if (m_Assets.Materials().IndexOf(m_NewObjectMaterial) < 0)
			m_NewObjectMaterial = m_Assets.Materials()[0].Id;
		return m_NewObjectMaterial;
	}

	// Il menu Add: gli oggetti nascono davanti alla camera, col materiale aperto nelle proprieta'
	void Application::DrawAddMenu() {
		if (!ImGui::BeginMenu("Add"))
			return;
		const glm::vec3 spawn = SpawnPosition();

		if (ImGui::MenuItem(ICON_SPHERE " Sphere")) {
			const ObjectId material = EnsureMaterialExists();

			Sphere sphere;
			sphere.Position = glm::vec4(spawn, 0.5f); // la w e' il raggio
			sphere.Material = material;
			m_Selection = Select(SelectionType::Sphere, m_Scene.AddSphere(sphere));
			m_GizmoOperation = ImGuizmo::TRANSLATE;
		}

		if (ImGui::MenuItem(ICON_BOX " Box")) {
			const ObjectId material = EnsureMaterialExists();

			// CreateBox appende i suoi 6 quad in coda, quindi i Box::index gia' esistenti
			// restano validi e in ordine crescente, come si aspetta il loop dei Cubes
			// nello shader (che li scorre in parallelo ai quad).
			m_Scene.CreateBox(spawn - glm::vec3(0.5f), spawn + glm::vec3(0.5f), material);

			m_Selection = Select(SelectionType::Box, (int)m_Scene.Boxes().size() - 1);
			m_GizmoOperation = ImGuizmo::TRANSLATE;
		}

		if (ImGui::MenuItem(ICON_QUAD " Quad")) {
			const ObjectId material = EnsureMaterialExists();

			Quad quad;
			quad.PositionLLC = glm::vec4(spawn - glm::vec3(0.5f, 0.5f, 0.0f), 0.0f);
			quad.U = { 1.0f, 0.0f, 0.0f, 0.0f };
			quad.V = { 0.0f, 1.0f, 0.0f, 0.0f };
			quad.Width = 1.0f;
			quad.Height = 1.0f;
			quad.Material = material;
			m_Selection = Select(SelectionType::Quad, m_Scene.AddQuad(quad));
			m_GizmoOperation = ImGuizmo::TRANSLATE;
		}

		// luci punto e spot (ADR 0009): lo spot nasce rivolto in basso
		for (int spot = 0; spot < 2; spot++) {
			if (ImGui::MenuItem(spot ? ICON_SPOT_LIGHT " Spot light" : ICON_POINT_LIGHT " Point light")) {
				PunctualLight light;
				light.Spot = spot == 1;
				light.Position = spawn;
				m_Selection = Select(SelectionType::Light, m_Scene.AddLight(light, spot ? "Spot light" : "Point light"));
				m_GizmoOperation = ImGuizmo::TRANSLATE;
			}
		}

		// Add (issue #27): le mesh e le luci di un file, davanti alla camera. Lo stesso file
		// aggiunto due volte si carica una volta sola: le istanze dividono la geometria.
		if (ImGui::MenuItem(ICON_MESH " Model...")) {
			const auto selection = pfd::open_file("Add model", "models", FileFilters()).result();
			if (!selection.empty()) {
				const ObjectId material = EnsureMaterialExists();
				const int mesh = AddFile(selection[0], m_Scene, m_Assets, glm::translate(glm::mat4(1.0f), spawn), material);
				if (mesh >= 0) {
					m_Selection = Select(SelectionType::Mesh, mesh);
					m_GizmoOperation = ImGuizmo::TRANSLATE;
				}
			}
		}

		ImGui::EndMenu();
	}

	// ------------------------------------------------------------ selezione e cancellazione

	// Quanti elementi ha la collezione a cui punta una selezione di questo tipo.
	int Application::SelectionCollectionSize(SelectionType type) const {
		switch (type) {
		case SelectionType::Sphere: return static_cast<int>(m_Scene.Spheres().size());
		case SelectionType::Quad:   return static_cast<int>(m_Scene.Quads().size());
		case SelectionType::Box:    return static_cast<int>(m_Scene.Boxes().size());
		case SelectionType::Mesh:   return static_cast<int>(m_Scene.Meshes().size());
		case SelectionType::Light:  return static_cast<int>(m_Scene.PunctualLights().size());
		default:                    return 0;
		}
	}

	bool Application::CanDeleteSelection() const {
		return m_Selection.IsValid() && SelectedIndex() < SelectionCollectionSize(m_Selection.Type);
	}

	// Ogni rimozione fa scalare gli indici successivi della sua collezione: la selezione
	// corrente diventa insensata comunque vada, quindi si azzera PRIMA di toccare la scena
	// (la finestra "Viewport" la userebbe piu' avanti in questo stesso frame).
	void Application::DeleteSelection() {
		if (!CanDeleteSelection()) return;

		const Selection sel = m_Selection;
		m_Selection = {};

		switch (sel.Type) {
		case SelectionType::Sphere: m_Scene.RemoveSphere(IndexOf(sel)); break;
		case SelectionType::Quad:   m_Scene.RemoveQuad(IndexOf(sel));   break;
		case SelectionType::Box:    m_Scene.RemoveBox(IndexOf(sel));    break;
		case SelectionType::Mesh:   m_Scene.RemoveMesh(IndexOf(sel));   break;
		case SelectionType::Light:  m_Scene.RemoveLight(IndexOf(sel));  break;
		default: break;
		}
	}

	// Modale: va aperta fuori dal Begin/End di qualsiasi finestra.
	// m_ShowDeletePopup e' solo il nostro specchio dello stato di ImGui: la OpenPopup
	// la fa chi preme Canc, e se ImGui chiude da solo (Escape, click fuori) BeginPopupModal
	// torna false e qui si risincronizza. Riaprirla in base al flag sarebbe un loop.
	void Application::DrawDeletePopup() {
		if (m_DeleteRequested) {
			m_DeleteRequested = false;
			m_ShowDeletePopup = true;
			ImGui::OpenPopup(kDeletePopupId);
		}

		if (!m_ShowDeletePopup)
			return;

		ImVec2 center = ImGui::GetMainViewport()->GetCenter();
		ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

		if (ImGui::BeginPopupModal(kDeletePopupId, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
			// La selezione puo' essere svanita mentre la modale era aperta (cambio scena):
			// stessa classe di bug che faceva crashare la selezione, stesso trattamento.
			if (!CanDeleteSelection()) {
				m_ShowDeletePopup = false;
				ImGui::CloseCurrentPopup();
				ImGui::EndPopup();
				return;
			}

			ImGui::Text("Delete %s?", GetSelectionName().c_str());

			switch (m_Selection.Type) {
			case SelectionType::Mesh:
				ImGui::TextDisabled("instance of %d triangles - the mesh asset stays loaded",
					m_Assets.FindMesh(m_Scene.Meshes()[SelectedIndex()].Asset) ? static_cast<int>(m_Assets.FindMesh(m_Scene.Meshes()[SelectedIndex()].Asset)->Triangles.size()) : 0);
				break;
			case SelectionType::Box:
				ImGui::TextDisabled("also removes its 6 faces");
				break;
			default:
				break;
			}

			ImGui::Spacing();
			ImGui::Separator();
			ImGui::Spacing();

			if (ImGui::Button("Delete", ImVec2(120, 0))) {
				DeleteSelection();
				m_ShowDeletePopup = false;
				ImGui::CloseCurrentPopup();
			}
			ImGui::SetItemDefaultFocus();
			ImGui::SameLine();
			if (ImGui::Button("Cancel", ImVec2(120, 0))) {
				m_ShowDeletePopup = false;
				ImGui::CloseCurrentPopup();
			}

			ImGui::EndPopup();
		}
		else {
			// ImGui l'ha chiusa per conto suo: Escape o click all'esterno
			m_ShowDeletePopup = false;
		}
	}

	// Corpo dell'editor, condiviso fra il tab Materials e la finestra flottante:
	// due copie divergerebbero al primo campo aggiunto.
	bool Application::DrawMaterialEditor(Material& material, int index) {
		// Editor in stile Principled (Blender): mostra concetti da artista e scrive nei campi
		// raw dello struct, che restano il contratto con lo shader. Le "probabilita'" di
		// importance sampling non compaiono: le derivo qui. Cosi' non possono piu' sommare
		// a un valore che sopprime il diffuso, che era il footgun della UI precedente.
		if (index >= 0 && index < (int)m_Assets.Materials().size())
			NameField(m_Assets.Materials()[index].Id, m_Assets.Materials()[index].Name);

		bool edited = false;
		bool changed = false;
		const bool isMetal = material.Metalness > 0.001f;

		// Base color. La tinta del vetro segue la base color (assorbimento Beer-Lambert).
		glm::vec3 baseColor = glm::vec3(material.Color);
		if (UI::ColorEdit3("Base Color", glm::value_ptr(baseColor))) {
			material.Color = glm::vec4(baseColor, 1.0f);
			changed = true;
		}

		ImGui::SeparatorText("Surface");

		if (UI::SliderFloat("Metallic", &material.Metalness, 0.0f, 1.0f)) changed = true;

		// Un'unica roughness per riflesso e trasmissione, come il Principled.
		if (UI::SliderFloat("Roughness", &material.Roughness, 0.0f, 1.0f)) {
			material.RefractionRoughness = material.Roughness;
			changed = true;
		}

		// Specular tint: colora il riflesso di un dielettrico verso la base color. Un metallo
		// lo ignora (il riflesso e' gia' tinto dall'albedo), quindi si grigia a metallo pieno.
		ImGui::BeginDisabled(material.Metalness >= 0.999f);
		if (UI::SliderFloat("Specular tint", &material.SpecularTint, 0.0f, 1.0f)) changed = true;
		ImGui::EndDisabled();

		// IOR: il Fresnel lo usa anche per il riflesso dielettrico, non solo per il vetro.
		// Su un metallo non ha effetto (rifrazione spenta, Fresnel saturo).
		ImGui::BeginDisabled(isMetal);
		if (UI::SliderFloat("IOR", &material.RefractionRatio, 1.0f, 2.5f)) changed = true;

		// Transmission = quanto e' vetroso. Un metallo non trasmette.
		float transmission = material.RefractionProbability;
		if (UI::SliderFloat("Transmission", &transmission, 0.0f, 1.0f)) {
			material.RefractionProbability = transmission;
			changed = true;
		}
		ImGui::EndDisabled();
		if (isMetal)
			ImGui::TextDisabled("metal: no diffuse or transmission, reflection tinted by albedo");

		// Specular level (F0 dielettrico) sepolto in Advanced, come in Blender: quasi
		// nessuno lo tocca. F0 = 0.08 * livello, quindi 0.5 -> 0.04 (dielettrico standard).
		if (ImGui::TreeNode("Advanced")) {
			ImGui::BeginDisabled(isMetal);
			float specLevel = std::clamp(material.SpecularProbability / 0.08f, 0.0f, 1.0f);
			if (UI::SliderFloat("Specular", &specLevel, 0.0f, 1.0f)) {
				material.SpecularProbability = 0.08f * specLevel;
				changed = true;
			}
			ImGui::EndDisabled();
			ImGui::TextDisabled("dielectric F0. 0.5 = 0.04, the standard physical value");
			ImGui::TreePop();
		}

		// I campi derivati si "curano" ad ogni modifica: il riflesso dielettrico e' bianco
		// (non tinto) e la tinta del vetro segue la base color. Legacy con SpecularColor
		// colorato vengono normalizzati alla prima modifica.
		if (changed) {
			material.SpecularColor = glm::vec4(1.0f);
			material.RefractionColor = material.Color;
			edited = true;
		}

		ImGui::SeparatorText("Emission");
		if (UI::DragFloat("Emissive Strength", &material.EmissiveStrenght, 0.1f, 0.0f, FLT_MAX)) edited = true;
		if (UI::ColorEdit3("Emissive Color", glm::value_ptr(material.EmissiveColor))) edited = true;

		ImGui::SeparatorText("Base Color texture");
		int checkerMode = static_cast<int>(material.Checker + 0.5f);
		if (UI::Combo("Checker", &checkerMode, "Off\0UV (debug)\0World space\0\0")) {
			material.Checker = static_cast<float>(checkerMode);
			edited = true;
		}
		if (checkerMode != 0) {
			if (UI::DragFloat("Checker scale", &material.CheckerScale, 0.1f, 0.01f, 64.0f))
				edited = true;
		}

		// Nello shader checker e texture sono in catena else-if: il checker vince e la texture
		// non viene mai letta. Invece di mostrarli come se si combinassero, la disabilito e lo
		// dico, cosi' l'esclusione non e' piu' silenziosa.
		const bool checkerWins = checkerMode != 0;
		ImGui::BeginDisabled(checkerWins);
		if (TextureCombo("Albedo texture", material.AlbedoTexture, TextureKind::Color))
			edited = true;
		ImGui::EndDisabled();
		if (checkerWins)
			ImGui::TextDisabled("checker on: it takes precedence, the texture is ignored");

		// Normal map (ADR 0007): solo le mesh hanno le UV e i triangoli da cui si ricava la tangente
		ImGui::SeparatorText("Normal map");
		{
			if (TextureCombo("Normal map", material.NormalTexture, TextureKind::Data))
				edited = true;
			if (!material.NormalTexture.IsNil()) {
				if (UI::SliderFloat("Normal strength", &material.NormalStrength, 0.0f, 2.0f))
					edited = true;
				bool flipY = material.NormalFlipY > 0.5f;
				if (UI::Checkbox("Flip green (DirectX)", &flipY)) {
					material.NormalFlipY = flipY ? 1.0f : 0.0f;
					edited = true;
				}
				ImGui::TextDisabled("applies to meshes only");
			}
		}

		// Alpha mask (ADR 0008): i loader decidono quali materiali sono mascherati, perche' i loro
		// triangoli stanno in una geometria a parte del BLAS. Qui si cambia solo la soglia.
		if (material.AlphaCutoff > 0.0f) {
			ImGui::SeparatorText("Alpha mask");
			if (UI::SliderFloat("Alpha cutoff", &material.AlphaCutoff, 0.01f, 1.0f))
				edited = true;
		}

		ImGui::SeparatorText("Procedural noise");
		int noiseMode = static_cast<int>(material.NoiseType + 0.5f);
		if (UI::Combo("Noise", &noiseMode, "Off\0Perlin\0Turbulence\0Marble\0\0")) {
			material.NoiseType = static_cast<float>(noiseMode);
			edited = true;
		}
		if (noiseMode != 0) {
			if (UI::DragFloat("Noise scale", &material.NoiseScale, 0.05f, 0.01f, 64.0f)) edited = true;
			if (UI::SliderFloat("Noise blend", &material.NoiseBlend, 0.0f, 1.0f)) edited = true;

			// Turbulence e Marble sommano ottave: il costo per raggio e' lineare in questo
			// valore, e viene pagato a ogni rimbalzo su questo materiale.
			if (noiseMode >= 2) {
				int octaves = static_cast<int>(material.NoiseOctaves + 0.5f);
				if (UI::SliderInt("Octaves", &octaves, 1, 8)) {
					material.NoiseOctaves = static_cast<float>(octaves);
					edited = true;
				}
			}
			// La fase e' quello che trasforma il rumore in venature: senza, il marmo e' nebbia.
			if (noiseMode == 3) {
				if (UI::DragFloat("Turbulence phase", &material.NoiseTurbulence, 0.1f, 0.0f, 40.0f))
					edited = true;
			}

			if (UI::ColorEdit3("Noise color A", glm::value_ptr(material.NoiseColorA))) edited = true;
			if (UI::ColorEdit3("Noise color B", glm::value_ptr(material.NoiseColorB))) edited = true;

			ImGui::TextDisabled("Evaluated in world space: the object \"slides\" through the noise if you move it");
		}

		ImGui::Spacing();
		ImGui::TextDisabled("Material %d", index);
		return edited;
	}

	// Le texture sono asset (issue #26): i materiali le citano per id, e il layer lo sceglie il
	// RenderScene. Una texture che nessun materiale usa si puo' togliere, e libera il suo layer.
	void Application::DrawTextureManager() {
		ImGui::SeparatorText("Textures");

		if (m_Assets.Textures().empty())
			ImGui::TextDisabled("no textures loaded");

		ObjectId remove;
		for (const auto& texture : m_Assets.Textures()) {
			ImGui::PushID(texture.Name.c_str());
			const bool used = m_Assets.IsTextureUsed(texture.Id);
			ImGui::BeginDisabled(used);
			if (ImGui::SmallButton("x"))
				remove = texture.Id;
			ImGui::EndDisabled();
			if (used && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
				ImGui::SetTooltip("used by a material");
			ImGui::SameLine();
			ImGui::Text("%s  (%s)", texture.Name.c_str(), texture.Kind == TextureKind::Color ? "color" : "data");
			ImGui::PopID();
		}
		if (!remove.IsNil())
			m_Assets.RemoveTexture(remove);

		// elenco dei file disponibili, letto una volta sola. EnvironmentMap e' un'altra cosa:
		// finisce nella cubemap, non nel sampler2DArray degli albedo.
		if (m_TextureFiles.empty() && std::filesystem::exists("textures")) {
			for (const auto& entry : std::filesystem::recursive_directory_iterator("textures")) {
				if (!entry.is_regular_file()) continue;

				const std::string path = entry.path().string();
				if (path.find("EnvironmentMap") != std::string::npos) continue;

				const std::string ext = entry.path().extension().string();
				if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".tga" || ext == ".bmp")
					m_TextureFiles.push_back(path);
			}

			std::sort(m_TextureFiles.begin(), m_TextureFiles.end());
		}

		if (m_TextureFiles.empty()) {
			ImGui::TextDisabled("(no files in res/textures)");
			return;
		}

		m_SelectedTextureFile = std::clamp(m_SelectedTextureFile, 0, (int)m_TextureFiles.size() - 1);

		ImGui::PushItemWidth(240.0f);
		if (ImGui::BeginCombo("##texture", m_TextureFiles[m_SelectedTextureFile].c_str())) {
			for (int i = 0; i < (int)m_TextureFiles.size(); i++)
				if (ImGui::Selectable(m_TextureFiles[i].c_str(), i == m_SelectedTextureFile))
					m_SelectedTextureFile = i;

			ImGui::EndCombo();
		}
		ImGui::PopItemWidth();
		ImGui::SameLine();

		// Una stessa immagine puo' servire come colore e come dato: sono due asset (sRGB e UNORM)
		const std::string& selected = m_TextureFiles[m_SelectedTextureFile];
		if (ImGui::Button("+ Color"))
			m_Assets.RegisterTexture(selected, TextureKind::Color); // gia' caricata = nessun doppione
		ImGui::SameLine();
		if (ImGui::Button("+ Data"))
			m_Assets.RegisterTexture(selected, TextureKind::Data);
		ImGui::TextDisabled("color = albedo (sRGB), data = normal map or alpha mask");
	}

	// --------------------------------------------------------------- camere

	// Le due camere (ADR 0011). La vista agganciata scrive nell'inquadratura quando si naviga;
	// in entrambe le modalita', se l'inquadratura cambia da fuori (un load, "Align", PT_CAM) la
	// vista la raggiunge. L'obiettivo e' sempre quello dell'inquadratura.
	void Application::SyncCameras(bool viewMoved) {
		if (m_Renderer.View.LockViewToCamera && viewMoved)
			m_Scene.MoveCamera(m_Camera.GetPosition(), m_Camera.GetDirection());
		const SceneCamera& shot = m_Scene.Camera();
		if (!shot.SamePose(m_LastShot))
			m_Camera.SetView(shot.Position, shot.Direction);
		if (m_Camera.m_VerticalFOV != shot.VerticalFOV) {
			m_Camera.m_VerticalFOV = shot.VerticalFOV;
			m_Camera.RecalculateProjection();
		}
		m_LastShot = shot;
	}

	// L'immagine del renderer come texture ImGui. Il renderer la ricrea al resize (la versione
	// cambia): si registra la nuova, e la vecchia si libera solo quando la GPU ha finito il frame
	// che la usava ancora.
	ImTextureID Application::DisplayTexture() {
		if (m_Renderer.GetDisplayView() == VK_NULL_HANDLE)
			return 0;
		if (m_DisplaySet == VK_NULL_HANDLE || m_DisplayVersion != m_Renderer.GetDisplayVersion()) {
			if (VkDescriptorSet old = m_DisplaySet; old != VK_NULL_HANDLE)
				m_Vulkan.Defer([old]() { ImGui_ImplVulkan_RemoveTexture(old); });
			m_DisplaySet = ImGui_ImplVulkan_AddTexture(m_Renderer.GetDisplaySampler(), m_Renderer.GetDisplayView(), VK_IMAGE_LAYOUT_GENERAL);
			m_DisplayVersion = m_Renderer.GetDisplayVersion();
		}
		return reinterpret_cast<ImTextureID>(m_DisplaySet);
	}

	// Dove ancorare il gizmo: il centro VISIVO dell'oggetto, non l'origine della sua
	// parametrizzazione (il pivot di un quad e' l'angolo, quello di una mesh e' l'origine
	// della sua Transform: entrambi possono cadere lontano dalla geometria).
	glm::vec3 Application::GetSelectionPivot() const {
		switch (m_Selection.Type) {
		case SelectionType::Sphere:
			return glm::vec3(m_Scene.Spheres().at(SelectedIndex()).Position);
		case SelectionType::Quad: {
			const Quad& q = m_Scene.Quads().at(SelectedIndex());
			return glm::vec3(q.PositionLLC)
				 + glm::vec3(q.U) * (q.Width * 0.5f)
				 + glm::vec3(q.V) * (q.Height * 0.5f);
		}
		case SelectionType::Box: {
			const Box& b = m_Scene.Boxes().at(SelectedIndex());
			return 0.5f * (glm::vec3(b.Min) + glm::vec3(b.Max));
		}
		case SelectionType::Mesh: {
			const AABB bounds = WorldBounds(m_Scene.Meshes().at(SelectedIndex()), m_Assets);
			return 0.5f * (bounds.min + bounds.max);
		}
		case SelectionType::Light:
			return m_Scene.PunctualLights().at(SelectedIndex()).Position;
		default:
			return glm::vec3(0.0f);
		}
	}

	// AABB world dell'oggetto selezionato, usata per disegnarne il contorno.
	void Application::GetSelectionBounds(glm::vec3& outMin, glm::vec3& outMax) const {
		outMin = glm::vec3(0.0f);
		outMax = glm::vec3(0.0f);

		switch (m_Selection.Type) {
		case SelectionType::Sphere: {
			const Sphere& s = m_Scene.Spheres().at(SelectedIndex());
			glm::vec3 center(s.Position);
			outMin = center - glm::vec3(s.Position.w);
			outMax = center + glm::vec3(s.Position.w);
			break;
		}
		case SelectionType::Quad: {
			const Quad& q = m_Scene.Quads().at(SelectedIndex());
			glm::vec3 llc(q.PositionLLC);
			glm::vec3 edgeU = glm::vec3(q.U) * q.Width;
			glm::vec3 edgeV = glm::vec3(q.V) * q.Height;

			// AABB dei 4 angoli del quad
			outMin = outMax = llc;
			for (const glm::vec3& corner : { llc + edgeU, llc + edgeV, llc + edgeU + edgeV }) {
				outMin = glm::min(outMin, corner);
				outMax = glm::max(outMax, corner);
			}
			break;
		}
		case SelectionType::Box: {
			const Box& b = m_Scene.Boxes().at(SelectedIndex());
			outMin = glm::vec3(b.Min);
			outMax = glm::vec3(b.Max);
			break;
		}
		case SelectionType::Mesh: {
			const AABB bounds = WorldBounds(m_Scene.Meshes().at(SelectedIndex()), m_Assets);
			outMin = bounds.min;
			outMax = bounds.max;
			break;
		}
		case SelectionType::Light: {
			// una luce non ha estensione: un piccolo cubo attorno alla posizione per vederla
			const glm::vec3 p = m_Scene.PunctualLights().at(SelectedIndex()).Position;
			outMin = p - glm::vec3(0.1f);
			outMax = p + glm::vec3(0.1f);
			break;
		}
		default:
			break;
		}
	}

	// Sfere e box non sono ruotabili: se e' selezionata la rotazione si ripiega su
	// translate. Passa da qui sia il gizmo sia l'etichetta, cosi' non possono divergere.
	ImGuizmo::OPERATION Application::EffectiveGizmoOperation() const {
		if (m_GizmoOperation == ImGuizmo::ROTATE && !m_Selection.SupportsRotation())
			return ImGuizmo::TRANSLATE;

		return m_GizmoOperation;
	}

	// Solo il nome dell'oggetto, senza l'operazione del gizmo: serve anche alla modale
	// di cancellazione, dove "Eliminare Sphere #0 - Translate?" non avrebbe senso.
	std::string Application::ObjectName(const Selection& selection) const {
		const int i = IndexOf(selection);
		if (i < 0) return {};
		switch (selection.Type) {
		case SelectionType::Sphere: return m_Scene.Spheres()[i].Name;
		case SelectionType::Quad:   return m_Scene.Quads()[i].Name;
		case SelectionType::Box:    return m_Scene.Boxes()[i].Name;
		case SelectionType::Mesh:   return m_Scene.Meshes()[i].Name;
		case SelectionType::Light:  return m_Scene.PunctualLights()[i].Name;
		default:                    return {};
		}
	}

	std::string Application::GetSelectionName() const {
		std::string name;
		switch (m_Selection.Type) {
		case SelectionType::Sphere: name = "Sphere"; break;
		case SelectionType::Quad:   name = "Quad";   break;
		case SelectionType::Box:    name = "Box";    break;
		case SelectionType::Light:
			name = m_Scene.PunctualLights().at(SelectedIndex()).Spot ? "Spot light" : "Point light";
			break;
		case SelectionType::Mesh:   name = "Mesh";   break;
		default: return "?";
		}
		return name + " \"" + ObjectName(m_Selection) + "\"";
	}

	std::string Application::GetSelectionLabel() const {
		if (!m_Selection.IsValid())
			return "No selection  -  click an object to select it";

		const ImGuizmo::OPERATION effective = EffectiveGizmoOperation();
		const char* operation = effective == ImGuizmo::ROTATE ? "Rotate"
							  : effective == ImGuizmo::SCALE  ? "Scale" : "Translate";

		std::string label = GetSelectionName() + "  -  " + operation;
		if (!m_Selection.SupportsRotation())
			label += "  (no rotazione)";

		return label;
	}

	// Etichetta a schermo + contorno dell'AABB: feedback immediato su cosa e' selezionato,
	// utile perche' il gizmo da solo puo' finire fuori vista (es. la sfera-terreno).
	void Application::DrawSelectionOverlay() {
		ImDrawList* drawList = ImGui::GetWindowDrawList();

		if (!m_Selection.IsValid())
			return;
		const ImU32 accent = ImGui::GetColorU32(Theme::Accent);

		glm::vec3 boundsMin, boundsMax;
		GetSelectionBounds(boundsMin, boundsMax);

		glm::mat4 viewProjection = m_Camera.GetProjection() * m_Camera.GetView();

		// gli 8 angoli: il bit i-esimo sceglie min/max sull'asse i
		ImVec2 corners[8];
		bool visible[8];
		for (int i = 0; i < 8; i++) {
			glm::vec3 corner((i & 1) ? boundsMax.x : boundsMin.x,
							 (i & 2) ? boundsMax.y : boundsMin.y,
							 (i & 4) ? boundsMax.z : boundsMin.z);
			visible[i] = WorldToScreen(viewProjection, corner, m_ViewportPos, m_ViewportSize, corners[i]);
		}

		// i 12 spigoli collegano angoli che differiscono per un solo bit
		static const int edges[12][2] = {
			{0,1},{0,2},{0,4},{1,3},{1,5},{2,3},
			{2,6},{3,7},{4,5},{4,6},{5,7},{6,7}
		};

		for (const auto& edge : edges) {
			// se un estremo sta dietro la camera lo spigolo andrebbe clippato: lo saltiamo
			if (visible[edge[0]] && visible[edge[1]])
				drawList->AddLine(corners[edge[0]], corners[edge[1]], accent, 1.5f);
		}

		ImVec2 pivot;
		if (WorldToScreen(viewProjection, GetSelectionPivot(), m_ViewportPos, m_ViewportSize, pivot))
			drawList->AddCircleFilled(pivot, 4.0f, accent);

		// uno spot non si vede: una linea mostra dove punta
		if (m_Selection.Type == SelectionType::Light) {
			const PunctualLight& light = m_Scene.PunctualLights().at(SelectedIndex());
			ImVec2 tip;
			if (light.Spot && WorldToScreen(viewProjection, light.Position + light.Direction * 0.75f, m_ViewportPos, m_ViewportSize, tip)
			    && WorldToScreen(viewProjection, light.Position, m_ViewportPos, m_ViewportSize, pivot))
				drawList->AddLine(pivot, tip, accent, 2.0f);
		}
	}

	// 'delta' e' una trasformazione affine world-space: per un punto p, p' = delta * p.
	// Applicare il delta (invece di ricostruire una matrice assoluta) permette di ancorare
	// il gizmo dove vogliamo senza dover reinventare la parametrizzazione di ogni primitiva.
	void Application::ApplyDelta(const glm::mat4& delta) {
		const glm::mat3 linear(delta); // parte lineare: rotazione + scala

		// fattori di scala per asse = lunghezza delle colonne della parte lineare
		const glm::vec3 axisScale(glm::length(linear[0]), glm::length(linear[1]), glm::length(linear[2]));

		switch (m_Selection.Type) {
		case SelectionType::Sphere: {
			Sphere s = m_Scene.Spheres().at(SelectedIndex());
			glm::vec3 center = glm::vec3(delta * glm::vec4(glm::vec3(s.Position), 1.0f));

			// una sfera ha un solo raggio: la scala puo' essere solo uniforme
			float uniformScale = (axisScale.x + axisScale.y + axisScale.z) / 3.0f;
			s.Position = glm::vec4(center, glm::max(s.Position.w * uniformScale, 1e-4f));
			m_Scene.Set(m_Selection.Id, s);
			break;
		}
		case SelectionType::Quad: {
			Quad q = m_Scene.Quads().at(SelectedIndex());

			// i lati del quad sono U*Width e V*Height: si trasformano come direzioni
			glm::vec3 edgeU = linear * (glm::vec3(q.U) * q.Width);
			glm::vec3 edgeV = linear * (glm::vec3(q.V) * q.Height);

			float width = glm::length(edgeU);
			float height = glm::length(edgeV);
			if (width < 1e-6f || height < 1e-6f)
				break; // quad degenere: ignoriamo la manipolazione

			q.PositionLLC = glm::vec4(glm::vec3(delta * glm::vec4(glm::vec3(q.PositionLLC), 1.0f)), 0.0f);
			q.Width = width;
			q.Height = height;
			q.U = glm::vec4(edgeU / width, 0.0f);
			q.V = glm::vec4(edgeV / height, 0.0f);
			m_Scene.Set(m_Selection.Id, q);
			break;
		}
		case SelectionType::Box: {
			Box b = m_Scene.Boxes().at(SelectedIndex());
			glm::vec3 center = 0.5f * (glm::vec3(b.Min) + glm::vec3(b.Max));
			glm::vec3 size = glm::vec3(b.Max) - glm::vec3(b.Min);

			center = glm::vec3(delta * glm::vec4(center, 1.0f));
			// il box e' axis-aligned: si puo' scalare, non ruotare
			// UpdateBox normalizza gli assi, quindi una dimensione nulla darebbe NaN
			size = glm::max(size * axisScale, glm::vec3(1e-3f));

			b.Min = glm::vec4(center - size * 0.5f, 0.0f);
			b.Max = glm::vec4(center + size * 0.5f, 0.0f);
			m_Scene.Set(m_Selection.Id, b); // riscrive i 6 quad che compongono il box
			break;
		}
		case SelectionType::Mesh: {
			// nessuna ricostruzione dei BLAS: cambia solo la trasformazione dell'istanza
			Mesh mesh = m_Scene.Meshes().at(SelectedIndex());
			mesh.Transform = delta * mesh.Transform;
			m_Scene.Set(m_Selection.Id, mesh);
			break;
		}
		case SelectionType::Light: {
			// la scala non ha senso su una luce delta: contano posizione e direzione
			PunctualLight light = m_Scene.PunctualLights().at(SelectedIndex());
			light.Position = glm::vec3(delta * glm::vec4(light.Position, 1.0f));
			const glm::vec3 dir = linear * light.Direction;
			if (glm::length(dir) > 1e-6f)
				light.Direction = glm::normalize(dir);
			m_Scene.Set(m_Selection.Id, light);
			break;
		}
		default:
			break;
		}
	}

	// Rete di sicurezza: la selezione e' un indice in una collezione che puo' rimpicciolirsi
	// sotto i piedi (cambio scena e, in futuro, cancellazione di oggetti). Invece di
	// affidarsi al fatto che ogni punto di mutazione si ricordi di azzerarla, la si
	// verifica contro il mondo corrente prima di usarla.
	void Application::ValidateSelection() {
		if (!m_Selection.IsValid())
			return;

		if (SelectedIndex() < 0)
			m_Selection = {};
	}

	ObjectId Application::IdOf(SelectionType type, int index) const {
		auto idAt = [index](const auto& list) { return index >= 0 && index < (int)list.size() ? list[index].Id : ObjectId{}; };
		switch (type) {
		case SelectionType::Sphere: return idAt(m_Scene.Spheres());
		case SelectionType::Quad:   return idAt(m_Scene.Quads());
		case SelectionType::Box:    return idAt(m_Scene.Boxes());
		case SelectionType::Mesh:   return idAt(m_Scene.Meshes());
		case SelectionType::Light:  return idAt(m_Scene.PunctualLights());
		default:                    return {};
		}
	}

	// ponytail: ricerca lineare per id a ogni uso; con migliaia di oggetti servira' una mappa
	int Application::IndexOf(const Selection& selection) const {
		if (!selection.IsValid())
			return -1;
		switch (selection.Type) {
		case SelectionType::Sphere: return m_Scene.Spheres().IndexOf(selection.Id);
		case SelectionType::Quad:   return m_Scene.Quads().IndexOf(selection.Id);
		case SelectionType::Box:    return m_Scene.Boxes().IndexOf(selection.Id);
		case SelectionType::Mesh:   return m_Scene.Meshes().IndexOf(selection.Id);
		case SelectionType::Light:  return m_Scene.PunctualLights().IndexOf(selection.Id);
		default:                    return -1;
		}
	}

	// Il buffer di ogni oggetto vive tra un frame e l'altro: mentre si scrive ImGui lo modifica,
	// quando il campo e' fermo si riallinea al nome (che puo' cambiare da fuori, es. un load).
	void Application::NameField(const ObjectId& id, const std::string& name) {
		std::array<char, 128>& buffer = m_NameBuffers[id];
		if (buffer[0] == '\0' && !name.empty())
			snprintf(buffer.data(), buffer.size(), "%s", name.c_str());
		UI::InputText("Name", buffer.data(), buffer.size());
		if (ImGui::IsItemDeactivatedAfterEdit())
			m_Scene.Rename(id, buffer.data()) || m_Assets.Rename(id, buffer.data()); // unico nella lista: puo' diventare "nome.001"
		if (!ImGui::IsItemActive())
			snprintf(buffer.data(), buffer.size(), "%s", name.c_str());
	}

	void Application::SetSelectionFromPick(int objectType, int objectIndex) {
		if (objectType < 0 || objectIndex < 0) {
			m_Selection = {};
			return;
		}

		// lo shader non conosce i box: riporta l'indice del quad colpito, e i 6 quad
		// consecutivi a partire da Box::index appartengono a quel box
		if (objectType == static_cast<int>(SelectionType::Quad)) {
			for (size_t b = 0; b < m_Scene.Boxes().size(); b++) {
				int firstQuad = static_cast<int>(m_Scene.Boxes()[b].index);
				if (objectIndex >= firstQuad && objectIndex < firstQuad + 6) {
					m_Selection = Select(SelectionType::Box, static_cast<int>(b));
					return;
				}
			}
		}

		m_Selection = Select(static_cast<SelectionType>(objectType), objectIndex);
	}

	void Application::UpdateSelection() {
		ImGuiIO& io = ImGui::GetIO();





		// Le scorciatoie non devono agire mentre si scrive in un widget o si naviga con
		// la camera (tasto destro + WASD). Il test giusto e' WantTextInput: con
		// NavEnableKeyboard attivo, WantCaptureKeyboard resta sempre vero e bloccherebbe
		// ogni scorciatoia.
		// con la modale di conferma aperta, Escape deve chiuderla e non deselezionare,
		// e Canc non deve riaprirla: le scorciatoie restano sospese
		if (!io.WantTextInput && !io.KeyCtrl && !m_ShowDeletePopup && !Input::IsMouseButtonDown(MouseButton::Right)) {
			if (ImGui::IsKeyPressed(ImGuiKey_G)) m_GizmoOperation = ImGuizmo::TRANSLATE;
			if (ImGui::IsKeyPressed(ImGuiKey_R)) m_GizmoOperation = ImGuizmo::ROTATE;
			if (ImGui::IsKeyPressed(ImGuiKey_S)) m_GizmoOperation = ImGuizmo::SCALE;
			if (ImGui::IsKeyPressed(ImGuiKey_Escape)) m_Selection = {};

			if (ImGui::IsKeyPressed(ImGuiKey_Delete) && CanDeleteSelection())
				m_DeleteRequested = true;
		}

		// il gizmo ha la precedenza sul picking: trascinarlo non deve deselezionare.
		// In modalita' fuoco pero' si clicca per mettere a fuoco, anche sopra il gizmo.
		const bool gizmoBusy = m_Selection.IsValid() && (ImGuizmo::IsOver() || ImGuizmo::IsUsing());

		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && m_ViewportHovered && m_WindowEdges == 0 && (!gizmoBusy || m_FocusPickArmed)) {
			ImVec2 mouse = ImGui::GetMousePos();

			// coordinate locali al rettangolo dell'immagine, non alla finestra OS
			float localX = mouse.x - m_ViewportPos.x;
			float localY = mouse.y - m_ViewportPos.y;

			if (localX >= 0.0f && localY >= 0.0f && localX < m_ViewportSize.x && localY < m_ViewportSize.y) {
				// la render e' in pixel del framebuffer, il mouse in coordinate della finestra
				int px = static_cast<int>(localX * m_FramebufferScale);
				// AddImage campiona con V ribaltata (0,1)->(1,0): la riga 0 dell'immagine
				// del compute sta in BASSO, mentre il mouse ha origine in alto
				int py = static_cast<int>((m_ViewportSize.y - localY) * m_FramebufferScale) - 1;

				// il pick e' in volo per un frame: ricorda ORA se e' per il fuoco, prima
				// che il bottone/modalita' cambino
				m_PickForFocus = m_FocusPickArmed;
				m_FocusPickArmed = false;

				m_LastPickPixel = { px, py };
				m_PickRequestCount++;
				m_Renderer.RequestPick(px, py);
			}
		}

		// il risultato e' pronto il frame dopo la richiesta
		int pickedType, pickedIndex;
		float pickedDistance;
		if (m_Renderer.ConsumePickResult(pickedType, pickedIndex, pickedDistance)) {
			m_LastPickType = pickedType;
			m_LastPickIndex = pickedIndex;

			if (m_PickForFocus) {
				m_PickForFocus = false;
				// distanza < 0 = il raggio ha mancato tutto (sfondo): non spostare il fuoco
				if (pickedDistance > 0.0f) {
					SceneCamera camera = m_Scene.Camera();
					camera.FocusDistance = pickedDistance;
					m_Scene.SetCamera(camera); // il fuoco cambia i raggi primari
				}
			}
			else {
				// il pick e' stato richiesto un frame fa: se nel mezzo la scena e' cambiata,
				// l'indice si riferisce a oggetti che non esistono piu'
				SetSelectionFromPick(pickedType, pickedIndex);
				ValidateSelection();

				if (m_Selection.IsValid())
					m_GizmoOperation = ImGuizmo::TRANSLATE; // default alla selezione, come Blender
			}
		}
	}

	void Application::DrawGizmo() {
		if (!m_Selection.IsValid())
			return;

		ImGuizmo::SetOrthographic(false);
		ImGuizmo::AllowAxisFlip(false);
		// draw list della finestra "Viewport": e' li' che vive l'immagine path-traced
		ImGuizmo::SetDrawlist(ImGui::GetWindowDrawList());
		ImGuizmo::SetRect(m_ViewportPos.x, m_ViewportPos.y, m_ViewportSize.x, m_ViewportSize.y);

		const ImGuizmo::OPERATION operation = EffectiveGizmoOperation();

		// Mentre si trascina, la matrice deve conservare lo stato accumulato da ImGuizmo;
		// appena si rilascia, la si ri-ancora al centro corrente dell'oggetto.
		if (!ImGuizmo::IsUsing())
			m_GizmoMatrix = glm::translate(glm::mat4(1.0f), GetSelectionPivot());

		glm::mat4 view = m_Camera.GetView();
		glm::mat4 projection = m_Camera.GetProjection();
		glm::mat4 delta(1.0f);

		if (ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection),
								 operation, ImGuizmo::WORLD,
								 glm::value_ptr(m_GizmoMatrix), glm::value_ptr(delta))) {
			ApplyDelta(delta); // passa dalla Scene: l'accumulo riparte da solo
		}
	}

	void Application::Render(float deltaTime) {
		// Test/CI headless: PT_VIEWPORT=<L>x<A> fissa la risoluzione di render. Il layout ImGui
		// cambia da un avvio all'altro e renderebbe non confrontabili immagini e tempi.
		if (const char* vp = getenv("PT_VIEWPORT"))
			sscanf(vp, "%ux%u", &m_ViewportWidth, &m_ViewportHeight);

		// La viewport e' in coordinate della finestra; la render la riempie in pixel veri (Wayland
		// HiDPI: il framebuffer e' piu' grande della finestra). PT_VIEWPORT e' gia' in pixel.
		uint32_t width = m_ViewportWidth, height = m_ViewportHeight;
		if (!getenv("PT_VIEWPORT")) {
			width = static_cast<uint32_t>(m_ViewportWidth * m_FramebufferScale);
			height = static_cast<uint32_t>(m_ViewportHeight * m_FramebufferScale);
		}
		m_Camera.OnResize(width, height);
		m_Renderer.OnResize(width, height);
		m_Renderer.Render(m_Camera.GetRenderView(), m_Scene, m_Assets);
	}

	ObjectId Application::SelectedMaterial() const {
		if (!m_Selection.IsValid())
			return {};
		const int i = SelectedIndex();
		switch (m_Selection.Type) {
		case SelectionType::Sphere: return m_Scene.Spheres()[i].Material;
		case SelectionType::Quad:   return m_Scene.Quads()[i].Material;
		case SelectionType::Box:    return m_Scene.Boxes()[i].Material;
		case SelectionType::Mesh:   return m_Scene.Meshes()[i].Material;
		default:                    return {};
		}
	}
}
