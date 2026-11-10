#pragma once

#ifndef RENDERSETTINGS_H
#define RENDERSETTINGS_H

#include "Core/Fields.h"

#include <iterator>
#include <string>

namespace Lucerna {

	// L'operatore che porta l'HDR sul display. L'ordine combacia con TONEMAP_* in
	// shaders/common/tonemap.glsl e con ToneMapperKeys.
	enum class ToneMapper : int { None, ACES, ACESFitted, AgX, Neutral, Reinhard, Hable };
	// le chiavi nel file di scena
	inline constexpr const char* ToneMapperKeys[] = { "none", "aces", "acesFitted", "agx", "neutral", "reinhard", "hable" };

	// Come si calcola l'immagine (ADR 0010): stessa scena, qualita' diversa. Si salva con la
	// scena, cosi' una scena preparata per il render finale si riapre con la sua qualita'.
	// I default sono il preset Preview.
	struct RenderSettings {
		// ---- integratore
		int SamplesPerPixel = 1;
		int RayDepth = 5;
		// Next Event Estimation (light sampling + MIS): a ogni vertice non-speculare campiona
		// direttamente gli Emitter invece di aspettare che un rimbalzo casuale li colpisca.
		// Lato shader e' numLights: qui lo si azzera per spegnerla senza toccare la light list.
		bool NEE = true;
		float FireflyClamp = 0.0f;   // luminanza massima per campione, 0 = off

		// ---- accumulazione
		// Riproiezione temporale (ADR 0002): al posto di azzerare l'accumulo quando la camera
		// si muove, si riusa il frame precedente riproiettandolo. Il cap limita il ghosting da
		// storia stantia e vale solo in movimento; da fermo N cresce libero e converge.
		bool Reproject = true;
		int HistoryCap = 32;
		float RenderScale = 1.0f;    // 1 = piena risoluzione; < 1 = ridotta mentre ci si muove

		// ---- denoiser à-trous (ADR 0001): filtro di sola vista, agisce sulla vista Beauty
		bool Denoise = false;
		float DenoiseStrength = 1.0f; // [0,1] peso del blend col beauty grezzo (1 = tutto denoised)
		// Auto-attenuazione: scala lo strength ~1/sqrt(N) col numero di campioni. A immagine
		// convergente il denoiser si spegne; resta forte solo sui frame rumorosi.
		bool DenoiseAutoFade = true;
		float DenoiseCPhi = 1.0f;    // sigma luminanza (grande = piu' sfoca ombre/rumore)
		float DenoiseNPhi = 64.0f;   // esponente normale (grande = spigoli piu' netti)
		float DenoisePPhi = 1.0f;    // sigma profondita' in unita' mondo (separa le silhouette)
		float DenoiseAPhi = 0.2f;    // sigma albedo (piccolo = blocca i bordi di materiale/tinta)

		// ---- film
		ToneMapper ToneMap = ToneMapper::ACES; // None = valori lineari clampati
		float Exposure = 1.0f;

		// Preset: punti di partenza, non modalita'. Cambiare un campo dopo averne scelto uno
		// e' normale.
		// Il documento confronta le impostazioni con quelle salvate: stanno nel file di scena
		bool operator==(const RenderSettings&) const = default;

		static RenderSettings Draft() {
			RenderSettings s;
			s.RayDepth = 3;
			s.RenderScale = 0.5f;
			s.Denoise = true;
			return s;
		}
		static RenderSettings Preview() { return {}; }
		static RenderSettings Final() {
			RenderSettings s;
			s.SamplesPerPixel = 4;
			s.RayDepth = 12;
			return s;
		}
	};

	constexpr auto JsonFields(const RenderSettings*) {
		using S = RenderSettings;
		return std::make_tuple(
			Field("samplesPerPixel", &S::SamplesPerPixel), Field("rayDepth", &S::RayDepth),
			Field("nee", &S::NEE), Field("fireflyClamp", &S::FireflyClamp),
			Field("reproject", &S::Reproject), Field("historyCap", &S::HistoryCap), Field("renderScale", &S::RenderScale),
			Field("denoise", &S::Denoise), Field("denoiseStrength", &S::DenoiseStrength), Field("denoiseAutoFade", &S::DenoiseAutoFade),
			Field("denoiseCPhi", &S::DenoiseCPhi), Field("denoiseNPhi", &S::DenoiseNPhi),
			Field("denoisePPhi", &S::DenoisePPhi), Field("denoiseAPhi", &S::DenoiseAPhi),
			// "tonemap" e' il vecchio bool (ACES o niente): si legge prima, poi "toneMapper" lo sostituisce
			Custom<S>("tonemap", [](const S& s) { return s.ToneMap != ToneMapper::None; },
				[](S& s, bool on) { if (!on) s.ToneMap = ToneMapper::None; }),
			Custom<S>("toneMapper", [](const S& s) { return std::string(ToneMapperKeys[static_cast<int>(s.ToneMap)]); },
				[](S& s, const std::string& key) {
					for (int i = 0; i < static_cast<int>(std::size(ToneMapperKeys)); i++)
						if (key == ToneMapperKeys[i])
							s.ToneMap = static_cast<ToneMapper>(i);
				}),
			Field("exposure", &S::Exposure));
	}

	// Cosa si sta guardando: non cambia mai l'immagine calcolata e non si salva.
	struct ViewSettings {
		int AOVView = 0;             // 0 = beauty, 1 = albedo, 2 = normal, 3 = depth, 4 = storia
		bool PathTracing = true;     // false = in pausa: l'accumulo resta com'e', riprende da li'
		// true = la vista guarda attraverso la camera della scena e navigando la sposta;
		// false = vista libera, l'inquadratura resta dov'e' (ADR 0011)
		bool LockViewToCamera = true;
	};
}

#endif // RENDERSETTINGS_H
