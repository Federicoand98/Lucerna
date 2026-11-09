#pragma once

#ifndef EDITOR_BLENDERLINK_H
#define EDITOR_BLENDERLINK_H

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace Lucerna {

	/*
	* Il collegamento con un Blender lanciato da Lucerna (ADR 0024). Blender parte con l'addon
	* tools/blender/lucerna_link.py caricato solo per quella sessione; dopo ogni export completo
	* l'addon riscrive un manifest (un JSON nella cartella temporanea, uno per processo di
	* Lucerna), e Poll lo legge. Sync now e' un file di richiesta accanto al manifest.
	*
	* Il manifest, versione 1:
	*   { "version": 1, "sync": N, "status": "exported" | "unsaved",
	*     "blend": "<.blend>", "gltf": "<.gltf>", "exportMs": 123.4 }
	*/
	class BlenderLink {
	public:
		enum class State {
			Off,      // mai lanciato, o scollegato
			Starting, // Blender e' partito, l'addon non ha ancora scritto
			Unsaved,  // in Blender c'e' un file mai salvato: niente da esportare
			Linked,   // c'e' un export
			Closed,   // Blender e' stato chiuso
		};

		// Un export completo, da aprire in Lucerna
		struct Sync {
			std::string Gltf, Blend;
			uint64_t Number = 0;
			float ExportMs = 0.0f;
		};

		explicit BlenderLink(std::filesystem::path manifest = DefaultManifest());

		// L'eseguibile da lanciare: quello delle preferenze se esiste, se no il primo che si trova
		// nel PATH o dove lo installano i pacchetti. Vuoto = nessuno.
		static std::string FindBlender(const std::string& preferred);
		static std::filesystem::path ScriptPath(); // tools/blender/lucerna_link.py accanto a res
		static std::filesystem::path DefaultManifest();
		static void ShowInFileManager(const std::filesystem::path& folder);

		bool Launch(const std::string& blender, std::string& error);
		// Ogni frame: l'export nuovo, se l'addon ne ha finito uno dall'ultima volta
		std::optional<Sync> Poll();
		void RequestSync();
		void Disconnect(); // smette di ascoltare; Blender resta aperto

		State GetState() const { return m_State; }
		const std::string& Blend() const { return m_Blend; }
		const std::string& Gltf() const { return m_Gltf; }
		float LastExportMs() const { return m_ExportMs; }
		double SecondsSinceSync() const;
		bool CanSync() const { return m_Running && (m_State == State::Linked || m_State == State::Unsaved); }
		const std::filesystem::path& ManifestPath() const { return m_Manifest; }

	private:
		bool ProcessRunning();

		std::filesystem::path m_Manifest;
		static constexpr double ManifestPollSeconds = 0.25;
		std::filesystem::file_time_type m_ManifestTime{};
		double m_ManifestRead = 0.0;
		State m_State = State::Off;
		bool m_Running = false;
		uint64_t m_LastSync = 0;
		std::string m_Blend, m_Gltf;
		float m_ExportMs = 0.0f;
		double m_SyncTime = 0.0;
#ifdef _WIN32
		void* m_Process = nullptr; // HANDLE
#else
		int m_Pid = 0;
#endif
	};
}

#endif // EDITOR_BLENDERLINK_H
