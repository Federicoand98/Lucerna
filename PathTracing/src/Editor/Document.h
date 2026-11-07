#pragma once

#ifndef EDITOR_DOCUMENT_H
#define EDITOR_DOCUMENT_H

#include "Render/RenderSettings.h"
#include "Scene/Scene.h"
#include "Serialization/BuiltinScenes.h"

#include <string>

namespace Lucerna {

	// Da dove viene la scena aperta: decide se Save riscrive il file o chiede dove salvare
	enum class DocumentOrigin {
		New,        // una scena vuota, mai salvata
		SceneFile,  // un file di scena: Save lo riscrive
		Imported,   // un glTF o un OBJ: non si riscrive, Save diventa Save As
		Builtin,    // una scena integrata: un modello, Save diventa Save As
	};

	/*
	* La scena aperta come documento: quale file e', e se ci sono modifiche non salvate. Le
	* modifiche si riconoscono dalle revisioni di Scene e AssetLibrary e dalle impostazioni di
	* render, che il file salva anche loro; navigare non conta (MoveCamera non tocca la revisione).
	* Le operazioni riuscite rendono il documento quel file, salvato.
	*/
	class Document {
	public:
		DocumentOrigin Origin() const { return m_Origin; }
		const std::string& Path() const { return m_Path; } // il file da cui viene; vuoto per una scena nuova
		bool CanSaveInPlace() const { return m_Origin == DocumentOrigin::SceneFile; }
		std::string DisplayName() const;                     // "sponza.json", "Untitled"
		bool IsModified(const Scene& scene, const AssetLibrary& assets, const RenderSettings& settings) const;

		void New(Scene& scene, AssetLibrary& assets, const RenderSettings& settings);
		// Un .json si apre come scena; ogni altro formato passa dagli importer
		bool Open(const std::string& path, Scene& scene, AssetLibrary& assets, RenderSettings& settings);
		bool OpenBuiltin(const BuiltinScene& builtin, Scene& scene, AssetLibrary& assets, RenderSettings& settings);
		bool Save(Scene& scene, AssetLibrary& assets, RenderSettings& settings); // solo se CanSaveInPlace
		bool SaveAs(const std::string& path, Scene& scene, AssetLibrary& assets, RenderSettings& settings);

	private:
		void MarkSaved(DocumentOrigin origin, const std::string& path, const Scene& scene, const AssetLibrary& assets, const RenderSettings& settings);

		DocumentOrigin m_Origin = DocumentOrigin::New;
		std::string m_Path;
		uint64_t m_SavedScene = 0, m_SavedAssets = 0;
		RenderSettings m_SavedSettings;
	};
}

#endif // EDITOR_DOCUMENT_H
