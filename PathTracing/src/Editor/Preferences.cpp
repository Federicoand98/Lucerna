#include "Editor/Preferences.h"

#include "Core/Json.h"

#include <algorithm>
#include <fstream>
#include <iostream>

namespace Lucerna {

	// Relativo alla cartella dell'eseguibile, come imgui.ini (Main.cpp ci fa chdir).
	static constexpr const char* PREFERENCES_PATH = "preferences.json";

	Preferences Preferences::Load() {
		Preferences prefs;
		std::ifstream file(PREFERENCES_PATH);
		if (!file)
			return prefs;
		try {
			nlohmann::json::parse(file).get_to(prefs);
		}
		catch (const std::exception& e) {
			std::cerr << "Preferences: " << PREFERENCES_PATH << " non valido (" << e.what() << "), uso i default" << std::endl;
		}
		return prefs;
	}

	void Preferences::Save() const {
		std::ofstream file(PREFERENCES_PATH);
		if (!file) {
			std::cerr << "Preferences: impossibile scrivere " << PREFERENCES_PATH << std::endl;
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
