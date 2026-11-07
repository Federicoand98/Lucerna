#pragma once

#ifndef PREFERENCES_H
#define PREFERENCES_H

#include "Core/Fields.h"

#include <string>
#include <vector>

namespace Lucerna {

	// Preferenze dell'applicazione (ADR 0010): valgono per ogni scena e restano tra un avvio e
	// l'altro, in preferences.json accanto a imgui.ini.
	struct Preferences {
		bool Vsync = true;
		// I file aperti o salvati di recente, il piu' recente per primo (Open Recent)
		std::vector<std::string> RecentFiles;
		void AddRecent(const std::string& path); // in testa, senza doppioni, al massimo MAX_RECENT
		static constexpr size_t MAX_RECENT = 10;

		// File mancante o illeggibile: i default, senza errore.
		static Preferences Load();
		void Save() const;
	};

	constexpr auto JsonFields(const Preferences*) {
		return std::make_tuple(Field("vsync", &Preferences::Vsync), Field("recentFiles", &Preferences::RecentFiles));
	}
}

#endif // PREFERENCES_H
