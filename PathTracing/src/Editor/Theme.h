#pragma once

#ifndef EDITOR_THEME_H
#define EDITOR_THEME_H

#include "imgui.h"

namespace Lucerna {

	// L'aspetto della UI (redesign, concept A): la palette scura con l'accento ambra, Inter per il
	// testo e JetBrains Mono per i numeri. I colori hanno un nome: il disegno a mano (HUD, outliner,
	// campioni dei materiali) usa gli stessi della UI.
	namespace Theme {
		inline constexpr ImVec4 Rgb(int hex, float alpha = 1.0f) {
			return ImVec4(((hex >> 16) & 0xff) / 255.0f, ((hex >> 8) & 0xff) / 255.0f, (hex & 0xff) / 255.0f, alpha);
		}

		inline constexpr ImVec4 Background = Rgb(0x1b1b1e);   // dietro la viewport
		inline constexpr ImVec4 Panel      = Rgb(0x232327);   // pannelli, barre
		inline constexpr ImVec4 Rail       = Rgb(0x1f1f23);   // la colonna di icone delle proprieta'
		inline constexpr ImVec4 Header     = Rgb(0x2e2e34);   // intestazioni delle sezioni, pulsanti
		inline constexpr ImVec4 Field      = Rgb(0x38383f);   // campi e slider
		inline constexpr ImVec4 Border     = Rgb(0x38383e);
		inline constexpr ImVec4 Text       = Rgb(0xe8e8ec);
		inline constexpr ImVec4 TextDim    = Rgb(0xa3a3ab);
		inline constexpr ImVec4 Accent     = Rgb(0xffb547);   // selezione, strumento attivo
		inline constexpr ImVec4 AccentText = Rgb(0x1a1306);   // testo sopra l'accento
		inline constexpr ImVec4 AccentFill = Rgb(0x5a4524);   // il riempimento degli slider
		inline constexpr ImVec4 Selected   = Rgb(0x4a3a1c);   // la riga selezionata
		inline constexpr ImVec4 SelectedText = Rgb(0xffe2b0);
		inline constexpr ImVec4 Hud        = Rgb(0x141416, 0.78f); // le etichette sopra la render

		// I colori delle icone nell'outliner, come nel mockup
		inline constexpr ImVec4 IconSun = Accent;
		inline constexpr ImVec4 IconSky = Rgb(0x8fb8ff);

		struct Fonts {
			ImFont* Regular = nullptr; // Inter, con le icone Lucide
			ImFont* Bold = nullptr;    // Inter SemiBold, con le icone: intestazioni
			ImFont* Mono = nullptr;    // JetBrains Mono: valori numerici, HUD
		};

		// Carica i font da fonts/ alla scala del monitor (HiDPI). Un file che manca lascia il font
		// di default di ImGui al suo posto, cosi' l'app parte comunque.
		Fonts LoadFonts(float scale);
		// Colori, arrotondamenti e spaziature della palette, alla stessa scala
		void Apply(float scale);
	}
}

#endif // EDITOR_THEME_H
