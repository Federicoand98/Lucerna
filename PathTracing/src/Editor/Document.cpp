#include "Editor/Document.h"

#include "Core/Paths.h"
#include "Import/SceneFiles.h"
#include "Serialization/SceneSerializer.h"

#include <filesystem>

namespace Lucerna {

	std::string Document::DisplayName() const {
		return m_Path.empty() ? "Untitled" : std::filesystem::path(m_Path).filename().string();
	}

	bool Document::IsModified(const Scene& scene, const AssetLibrary& assets, const RenderSettings& settings) const {
		return scene.Revision() != m_SavedScene || assets.Revision() != m_SavedAssets || !(settings == m_SavedSettings);
	}

	void Document::New(Scene& scene, AssetLibrary& assets, const RenderSettings& settings) {
		scene.Clear();
		assets.Clear();
		MarkSaved(DocumentOrigin::New, {}, scene, assets, settings);
	}

	bool Document::Open(const std::string& path, Scene& scene, AssetLibrary& assets, RenderSettings& settings) {
		const bool sceneFile = std::filesystem::path(path).extension() == ".json";
		const bool ok = sceneFile ? SceneSerializer(scene, assets, &settings).Deserialize(path) : OpenFile(path, scene, assets);
		if (ok)
			MarkSaved(sceneFile ? DocumentOrigin::SceneFile : DocumentOrigin::Imported, CanonicalPath(path), scene, assets, settings);
		return ok;
	}

	bool Document::OpenBuiltin(const BuiltinScene& builtin, Scene& scene, AssetLibrary& assets, RenderSettings& settings) {
		if (!SceneSerializer(scene, assets, &settings).Deserialize(builtin.Path))
			return false;
		MarkSaved(DocumentOrigin::Builtin, builtin.Path, scene, assets, settings);
		return true;
	}

	bool Document::Save(Scene& scene, AssetLibrary& assets, RenderSettings& settings) {
		return CanSaveInPlace() && SaveAs(m_Path, scene, assets, settings);
	}

	bool Document::SaveAs(const std::string& path, Scene& scene, AssetLibrary& assets, RenderSettings& settings) {
		if (!SceneSerializer(scene, assets, &settings).Serialize(path))
			return false;
		MarkSaved(DocumentOrigin::SceneFile, CanonicalPath(path), scene, assets, settings);
		return true;
	}

	void Document::MarkSaved(DocumentOrigin origin, const std::string& path, const Scene& scene, const AssetLibrary& assets, const RenderSettings& settings) {
		m_Origin = origin;
		m_Path = path;
		m_SavedScene = scene.Revision();
		m_SavedAssets = assets.Revision();
		m_SavedSettings = settings;
	}
}
