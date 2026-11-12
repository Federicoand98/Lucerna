#pragma once

#ifndef EDITOR_KEYMAP_H
#define EDITOR_KEYMAP_H

#include "imgui.h"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace Lucerna {

	// Dove vale una scorciatoia (ADR 0027). Un pannello vince sul globale quando ha il fuoco;
	// la navigazione sono tasti tenuti, letti solo col tasto destro premuto.
	enum class KeyContext { Global, Profiler, Navigation };

	// Un'azione dell'interfaccia: la eseguono il menu, la scorciatoia e i pulsanti che la mostrano.
	struct Action {
		std::string Id;           // stabile: e' la chiave in preferences.json
		const char* Group = "";   // la sezione nella tabella delle Preferences e nel menu Help
		const char* Name = "";
		const char* Icon = nullptr;
		KeyContext Context = KeyContext::Global;
		ImGuiKeyChord Default = ImGuiKey_None;
		std::function<void()> Run;
		std::function<bool()> Enabled; // vuota = sempre
		std::function<bool()> Checked; // vuota = non e' un interruttore
	};

	// Il registro delle azioni e i tasti assegnati. Un tasto e' una combinazione di Ctrl, Shift,
	// Alt e un tasto (ImGuiKeyChord), uno per azione; due azioni dello stesso contesto non hanno
	// mai lo stesso tasto.
	class Keymap {
	public:
		void Add(Action action);
		const std::vector<Action>& Actions() const { return m_Actions; }
		const Action* Find(const std::string& id) const;
		ImGuiKeyChord Key(const std::string& id) const;
		bool Run(const std::string& id) const; // false = non c'e' o non e' disponibile

		// I tasti dalle sole differenze salvate: prima quelle, poi i default che non scontrano
		void Apply(const std::map<std::string, std::string>& overrides);
		std::map<std::string, std::string> Overrides() const; // id -> tasto, "" = nessuno
		// Assegna un tasto (ImGuiKey_None lo toglie). Ritorna l'azione che lo aveva, o "".
		std::string Assign(const std::string& id, ImGuiKeyChord chord);
		void ResetAll();

		// L'azione disponibile il cui tasto e' stato premuto in questo frame, in quel contesto
		const Action* Pressed(KeyContext context) const;
		bool Down(const std::string& id) const; // un tasto di navigazione tenuto premuto

		static std::string Serialize(ImGuiKeyChord chord); // "Ctrl+Shift+S", come lo scrive il file
		static ImGuiKeyChord Parse(const std::string& text); // ImGuiKey_None se non lo riconosce
		static std::string Format(ImGuiKeyChord chord);    // per i menu: "Ctrl+,", "Esc", "" = nessuno

	private:
		int IndexOf(const std::string& id) const;
		std::vector<Action> m_Actions;
		std::vector<ImGuiKeyChord> m_Keys; // il tasto di ogni azione, nello stesso ordine
	};
}

#endif // EDITOR_KEYMAP_H
