#pragma once

#ifndef EDITOR_WIDGETS_H
#define EDITOR_WIDGETS_H

#include "Editor/Theme.h"

namespace Lucerna::UI {

	/*
	* I widget delle proprieta' nello stile del concept A: l'etichetta a sinistra in una colonna
	* fissa, il campo a destra fino al bordo, i numeri in JetBrains Mono, gli slider riempiti
	* d'ambra fino al valore. Hanno le firme di ImGui, cosi' si sostituiscono riga per riga.
	* L'etichetta e' anche l'ID: due campi con lo stesso nome nella stessa finestra vanno
	* distinti con PushID, come in ImGui.
	*/

	void Configure(const Theme::Fonts& fonts, float scale); // una volta, dopo LoadFonts

	void Label(const char* text);  // l'etichetta a sinistra; il widget dopo occupa il resto della riga

	bool SliderFloat(const char* label, float* v, float min, float max, const char* format = "%.3f", int flags = 0);
	bool SliderInt(const char* label, int* v, int min, int max, const char* format = "%d");
	bool DragFloat(const char* label, float* v, float speed = 1.0f, float min = 0.0f, float max = 0.0f, const char* format = "%.3f", int flags = 0);
	bool DragFloat3(const char* label, float v[3], float speed = 1.0f, float min = 0.0f, float max = 0.0f, const char* format = "%.3f");
	bool ColorEdit3(const char* label, float col[3], int flags = 0);
	bool Combo(const char* label, int* current, const char* itemsZeroSeparated);
	bool Combo(const char* label, int* current, const char* const items[], int count);
	bool Checkbox(const char* label, bool* v);
	bool InputText(const char* label, char* buffer, size_t size);

	// Una sezione richiudibile, con l'intestazione grigia del mockup
	bool Section(const char* label, bool defaultOpen = true);
	// Un pulsante a pillola: ambra quando e' attivo (lo strumento scelto, la vista)
	bool Pill(const char* label, bool active = false, bool enabled = true);

	ImFont* MonoFont();
	ImFont* BoldFont();
	float Scale();
}

#endif // EDITOR_WIDGETS_H
