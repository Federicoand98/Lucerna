#include "Editor/Preferences.h"

#include "Core/Json.h"
#include "Core/Paths.h"

#include <algorithm>
#include <fstream>
#include <iostream>

namespace Lucerna {

	// Nella cartella dell'utente, con imgui.ini (ADR 0022). Il file che stava in res prima della
	// riorganizzazione si legge ancora una volta, se quello nuovo non c'e'.
	static std::filesystem::path PreferencesPath() { return UserConfigDirectory() / "preferences.json"; }
	static constexpr const char* OLD_PREFERENCES_PATH = "preferences.json";

	Preferences Preferences::Load() {
		Preferences prefs;
		std::ifstream file(std::filesystem::exists(PreferencesPath()) ? PreferencesPath() : std::filesystem::path(OLD_PREFERENCES_PATH));
		if (!file)
			return prefs;
		try {
			nlohmann::json::parse(file).get_to(prefs);
		}
		catch (const std::exception& e) {
			std::cerr << "Preferences: " << PreferencesPath() << " non valido (" << e.what() << "), uso i default" << std::endl;
		}
		return prefs;
	}

	void Preferences::Save() const {
		std::error_code error;
		std::filesystem::create_directories(UserConfigDirectory(), error);
		std::ofstream file(PreferencesPath());
		if (!file) {
			std::cerr << "Preferences: impossibile scrivere " << PreferencesPath() << std::endl;
			return;
		}
		file << nlohmann::json(*this).dump(2);
	}

	void Preferences::AddRecent(const std::string& path) {
		RecentFiles.erase(std::remove(RecentFiles.begin(), RecentFiles.end(), path), RecentFiles.end());
		RecentFiles.insert(RecentFiles.begin(), path);
		if (RecentFiles.size() > MAX_RECENT)
			RecentFiles.resize(MAX_RECENT);
	}
}
