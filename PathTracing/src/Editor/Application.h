#pragma once

#ifndef APPLICATION_H
#define APPLICATION_H

#include "Editor/Preferences.h"
#include <iostream>
#include <filesystem>
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_vulkan.h"
#include <GLFW/glfw3.h>
#include "Vulkan/VulkanContext.h"
#include "Render/Renderer.h"
#include "Serialization/BuiltinScenes.h"
#include "Editor/Document.h"
#include <functional>
#include "Editor/Camera.h"
#include "ImGuizmo.h"
#include "ptpch.h"

/*
* @Class Application
* @brief The Application class is the main entry poiny for the application.
* 
* This class is responsible for initializating, running, and shutting down the application.
* 
* @member m_Renderer: An instance of the Renderer ckass that handles all rendering tasks.
* @member m_Camera: An instace of the Camera class that represents the camera in the 3D world.
* @member m_Scene, m_Assets: the scene description and the assets it references by id.
*/

namespace Lucerna {

	// I valori 0/1/2 combaciano con ObjectType dello shader (pickType).
	// Box non esiste nello shader: e' un gruppo di 6 quad, mappato lato CPU.
	enum class SelectionType {
		None = -1,
		Sphere = 0,
		Quad = 1,
		Mesh = 2,
		Box = 3,
		Light = 4   // luce punto o spot: nessun raggio la colpisce, si seleziona dall'outliner
	};

	// Cosa e' selezionato: il tipo e l'id dell'oggetto (issue #23). L'indice nella sua lista si
	// ricava quando serve (Application::IndexOf): cancellare un altro oggetto non lo sposta.
	struct Selection {
		SelectionType Type = SelectionType::None;
		ObjectId Id;

		bool IsValid() const { return Type != SelectionType::None && !Id.IsNil(); }
		// una sfera non ha orientamento, un box e' axis-aligned: niente rotazione.
		// La rotazione di una luce orienta lo spot.
		bool SupportsRotation() const { return Type == SelectionType::Quad || Type == SelectionType::Mesh || Type == SelectionType::Light; }
	};

	class Application {
	public:
		Application();
		~Application();

		static Application& Get();

		bool Initialize(int width, int height);
		void RunLoop();
		void Shutdown();

		GLFWwindow* GetWindow() const { return m_Window; }
	private:
		void CalculateTime();
		void RenderUI(float deltaTime);
		void Render(float deltaTime);
		void HandleCaptures();

		// ---- l'area di lavoro (Workspace.cpp): layout, menu, pannelli, sezioni delle proprieta'
		void BuildDefaultLayout(ImGuiID dockspace);
		void DrawStatusBar();
		void DrawEditMenu();
		void DrawAddMenu();
		void DrawViewMenu();
		void DrawHelpMenu();
		void DrawPreferencesWindow();
		void DrawOutlinerPanel();
		bool OutlinerGroup(const char* label, size_t count, bool defaultOpen);
		bool OutlinerRow(const char* icon, const ImVec4& iconColor, const std::string& name, bool selected, const char* note = nullptr);
		void DrawMaterialsPanel();
		void DrawViewportPanel(float deltaTime);
		void DrawViewportToolbar();
		void DrawHud();
		void DrawPropertiesPanel();
		void DrawObjectSection();
		void DrawMaterialSection();
		void DrawWorldSection();
		void DrawCameraSection();
		void DrawRenderSection();
		void DrawFilmSection();
		void DrawDeveloperSection();

		void DrawSelectionProperties();
		bool DrawMaterialEditor(Material& material, int index); // true = la copia e' cambiata
		void DrawTextureManager();
		ObjectId SelectedMaterial() const;   // il materiale dell'oggetto selezionato, nil se non ne ha
		void DrawDeletePopup();
		void DeleteSelection();
		bool CanDeleteSelection() const;
		int SelectionCollectionSize(SelectionType type) const;
		bool MaterialCombo(const char* label, ObjectId& material);
		bool TextureCombo(const char* label, ObjectId& texture, TextureKind kind);

		ObjectId EnsureMaterialExists();     // garantisce almeno un materiale, ne ritorna l'id
		glm::vec3 SpawnPosition() const;     // davanti alla camera

		// selezione + gizmo
		void UpdateSelection();
		void DrawGizmo();
		void SetSelectionFromPick(int objectType, int objectIndex);
		void ValidateSelection();
		void DrawSelectionOverlay();
		ImGuizmo::OPERATION EffectiveGizmoOperation() const;
		glm::vec3 GetSelectionPivot() const;
		void GetSelectionBounds(glm::vec3& outMin, glm::vec3& outMax) const;
		std::string GetSelectionLabel() const;
		std::string GetSelectionName() const;
		void ApplyDelta(const glm::mat4& delta);

		// Apre un file (rimpiazzando la scena) e registra path+mtime per il live-link;
		// PollLiveLink controlla ogni frame se il file e' cambiato e in tal caso lo riapre.
		// ---- documento (il menu File)
		bool OpenDocument(const std::string& path);
		bool LoadBuiltinScene(int index);
		void NewDocument();
		bool SaveDocument();      // riscrive il file aperto, o chiede dove salvare
		bool SaveDocumentAs();
		void WhenSaved(std::function<void()> action); // chiede di salvare prima di un'azione che butta via le modifiche
		void DrawSavePrompt();
		void ShowOpenDialog();
		void DrawMainMenu();
		void HandleShortcuts();
		void UpdateWindowTitle();
		// ---- finestra (ADR 0021): la barra del titolo disegnata dall'app e la scala HiDPI
		void ApplyUiScale();        // font, stile e scala del framebuffer alla scala del monitor attuale
		void RebuildUiForScale();   // dopo un cambio di monitor con un'altra scala
		void DrawWindowControls();  // riduci, ingrandisci, chiudi, nella barra dei menu
		void HandleWindowEdges();   // i bordi che ridimensionano una finestra senza cornice
		static void OnContentScale(GLFWwindow* window, float x, float y);
		void RememberRecent(const std::string& path);
		void PollLiveLink();
		static std::vector<std::string> FileFilters(); // per il file dialog, dal registry degli importer
	private:
		Selection m_Selection;
		ImGuizmo::OPERATION m_GizmoOperation = ImGuizmo::TRANSLATE;
		glm::mat4 m_GizmoMatrix{ 1.0f };

		// Rettangolo a schermo dell'immagine path-traced dentro la finestra "Viewport".
		// La 3D view non e' il background: e' una AddImage in un pannello ImGui, quindi
		// picking, gizmo e overlay devono essere ancorati a QUESTO rettangolo.
		ImVec2 m_ViewportPos{ 0.0f, 0.0f };
		ImVec2 m_ViewportSize{ 0.0f, 0.0f };
		bool m_ViewportHovered = false;

		// I pannelli (redesign, concept A) e la sezione delle proprieta' aperta
		enum class PropertiesTab { Object, Material, World, Camera, Render, Film, Developer };
		PropertiesTab m_PropertiesTab = PropertiesTab::Object;
		bool m_ShowOutliner = true, m_ShowMaterials = true, m_ShowProperties = true;
		bool m_ShowHud = true, m_ShowDeveloper = false, m_ShowPreferences = false;
		bool m_ResetLayout = false;
		// Il materiale aperto nella sezione Material: quello cliccato nel pannello, o quello
		// dell'oggetto selezionato quando la selezione cambia (m_MaterialFollowsSelection)
		ObjectId m_EditedMaterial;
		ObjectId m_MaterialFollowsSelection;
		std::string m_IniPath;           // imgui.ini nella cartella dell'utente: ImGui ne tiene il puntatore
		float m_UiScale = 1.0f;          // la scala delle misure della UI, in coordinate della finestra
		float m_FramebufferScale = 1.0f; // pixel del framebuffer per unita' della finestra (Wayland HiDPI)
		bool m_CustomTitleBar = false;   // finestra senza cornice: la barra e' quella dei menu
		bool m_RescaleRequested = false;
		int m_WindowEdges = 0;           // i bordi sotto il mouse (GLFW_WINDOW_EDGE_*), 0 = nessuno
		ImGui_ImplVulkan_InitInfo m_ImGuiVulkanInfo{};

		// La richiesta e' separata dallo stato "aperta" perche' OpenPopup usa l'ID stack
		// della finestra corrente: chiamandola dal bottone dentro "Settings" produrrebbe un
		// ID diverso da quello della modale, disegnata a livello top. Quindi si alza solo
		// un flag e la OpenPopup la fa sempre DrawDeletePopup.
		bool m_DeleteRequested = false;
		bool m_ShowDeletePopup = false;   // finche' e' vero, le scorciatoie sono sospese

		// DOF: "pick per fuoco". Armato dal bottone, il click successivo nel viewport
		// mette a fuoco il punto colpito invece di selezionare. m_PickForFocus ricorda,
		// mentre il pick e' in volo (risultato pronto il frame dopo), che era per il fuoco.
		bool m_FocusPickArmed = false;
		bool m_PickForFocus = false;

		// salva/carica scena

		// creazione oggetti
		ObjectId m_NewObjectMaterial;
		std::vector<std::string> m_TextureFiles;
		int m_SelectedTextureFile = 0;

		// diagnostica del picking (View > Developer)
		glm::ivec2 m_LastPickPixel{ -1, -1 };
		int m_LastPickType = -1;
		int m_LastPickIndex = -1;
		int m_PickRequestCount = 0;

		VulkanContext m_Vulkan;
		VkDescriptorPool m_ImGuiPool = VK_NULL_HANDLE;
		bool m_ScreenshotRequested = false;

		Renderer m_Renderer;
		Camera m_Camera;
		Scene m_Scene;          // la descrizione della scena (issue #29)
		AssetLibrary m_Assets;  // geometria, materiali e texture che la scena cita per id

		// Live-link glTF (ADR 0003): il renderer osserva il file caricato e lo ricarica quando
		// cambia (tipicamente perche' Blender l'ha ri-esportato su save). Read-only verso Blender.
		Document m_Document;                            // il file aperto e se ci sono modifiche da salvare
		std::function<void()> m_PendingAction;          // in attesa della risposta a "Save changes?"
		bool m_SavePromptRequested = false;
		std::string m_WindowTitle;
		std::vector<BuiltinScene> m_BuiltinScenes;     // res/scenes/builtin (issue #28)
		int m_CurrentScene = -1;                        // in m_BuiltinScenes, -1 = un file aperto o caricato
		std::string m_WatchPath;                        // il file aperto, vuoto = nessuno
		bool m_LiveLink = true;                         // riapre il file quando cambia
		std::filesystem::file_time_type m_WatchMtime{}; // ultimo timestamp visto
		bool m_IsRunning;
		Preferences m_Preferences;
		// La texture ImGui dell'immagine del renderer, e la versione dell'immagine a cui si riferisce
		VkDescriptorSet m_DisplaySet = VK_NULL_HANDLE;
		uint32_t m_DisplayVersion = 0;
		ImTextureID DisplayTexture();
		SceneCamera m_LastShot;

		// Selezione per id (issue #23)
		ObjectId IdOf(SelectionType type, int index) const;
		int IndexOf(const Selection& selection) const; // -1 se l'oggetto non c'e' piu'
		Selection Select(SelectionType type, int index) const { return { type, IdOf(type, index) }; }
		int SelectedIndex() const { return IndexOf(m_Selection); }
		std::string ObjectName(const Selection& selection) const;
		// Campo "Name": rinomina all'uscita dal campo, con il nome reso unico dalla lista
		void NameField(const ObjectId& id, const std::string& name);
		std::unordered_map<ObjectId, std::array<char, 128>> m_NameBuffers; // l'inquadratura del frame scorso: se cambia da fuori, la vista la segue
		void SyncCameras(bool viewMoved);
		bool m_Vsync = true; // effettivo: PT_NOVSYNC lo spegne per la sessione senza toccare le preferenze
		float m_Timer = 0.0f;
		float m_DeltaTime = 0.0f;
		float m_ResetTimer = 0.0f;
		float m_LastFrameTime = 0.0f;
		int m_NFrames = 0;
		double m_FPS = 0.0f;
		int m_Width;
		int m_Height;
		uint32_t m_ViewportWidth = 0;
		uint32_t m_ViewportHeight = 0;
		GLFWwindow* m_Window;
	};
}

#endif // APPLICATION_H
