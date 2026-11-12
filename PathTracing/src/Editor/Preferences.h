#pragma once

#ifndef PREFERENCES_H
#define PREFERENCES_H

#include "Core/Fields.h"

#include <string>
#include <vector>

namespace Lucerna {

	// Preferenze dell'applicazione (ADR 0010): valgono per ogni scena e restano tra un avvio e
	// l'altro, in preferences.json accanto a imgui.ini, nella cartella dell'utente (ADR 0022).
	struct Preferences {
		bool Vsync = true;
		// La barra del titolo del sistema invece di quella disegnata dall'app (ADR 0021): vale
		// al prossimo avvio, perche' la cornice si sceglie quando si crea la finestra
		bool SystemTitleBar = false;
		// I file aperti o salvati di recente, il piu' recente per primo (Open Recent)
		std::vector<std::string> RecentFiles;
		// L'eseguibile di Blender per File > Open in Blender (ADR 0024); vuoto = lo cerca Lucerna
		std::string BlenderPath;
		// Come la Resolution Scale di Blender: ingrandisce font e misure della UI, sopra la scala del monitor
		float UiScale = 1.0f;
		static constexpr float MIN_UI_SCALE = 1.0f, MAX_UI_SCALE = 2.0f;
		void AddRecent(const std::string& path); // in testa, senza doppioni, al massimo MAX_RECENT
		static constexpr size_t MAX_RECENT = 10;

		// File mancante o illeggibile: i default, senza errore.
		static Preferences Load();
		void Save() const;
	};

	constexpr auto JsonFields(const Preferences*) {
		return std::make_tuple(Field("vsync", &Preferences::Vsync), Field("systemTitleBar", &Preferences::SystemTitleBar), Field("recentFiles", &Preferences::RecentFiles),
		                       Field("blenderPath", &Preferences::BlenderPath), Field("uiScale", &Preferences::UiScale));
	}
}

#endif // PREFERENCES_H
