#include "Editor/Keymap.h"

#include <cstring>

namespace Lucerna {

	void Keymap::Add(Action action) {
		m_Keys.push_back(action.Default);
		m_Actions.push_back(std::move(action));
	}

	int Keymap::IndexOf(const std::string& id) const {
		for (size_t i = 0; i < m_Actions.size(); i++)
			if (m_Actions[i].Id == id)
				return static_cast<int>(i);
		return -1;
	}

	const Action* Keymap::Find(const std::string& id) const {
		const int i = IndexOf(id);
		return i < 0 ? nullptr : &m_Actions[i];
	}

	ImGuiKeyChord Keymap::Key(const std::string& id) const {
		const int i = IndexOf(id);
		return i < 0 ? ImGuiKey_None : m_Keys[i];
	}

	bool Keymap::Run(const std::string& id) const {
		const Action* action = Find(id);
		if (!action || (action->Enabled && !action->Enabled()))
			return false;
		action->Run();
		return true;
	}

	void Keymap::Apply(const std::map<std::string, std::string>& overrides) {
		std::vector<bool> overridden(m_Actions.size(), false);
		for (size_t i = 0; i < m_Actions.size(); i++) {
			const auto found = overrides.find(m_Actions[i].Id);
			overridden[i] = found != overrides.end();
			m_Keys[i] = overridden[i] ? Parse(found->second) : ImGuiKey_None;
		}
		// un default vale solo se nessuna azione dello stesso contesto ha gia' preso quel tasto
		for (size_t i = 0; i < m_Actions.size(); i++) {
			if (overridden[i] || m_Actions[i].Default == ImGuiKey_None)
				continue;
			bool taken = false;
			for (size_t j = 0; j < m_Actions.size() && !taken; j++)
				taken = overridden[j] && m_Keys[j] == m_Actions[i].Default && m_Actions[j].Context == m_Actions[i].Context;
			if (!taken)
				m_Keys[i] = m_Actions[i].Default;
		}
	}

	std::map<std::string, std::string> Keymap::Overrides() const {
		std::map<std::string, std::string> overrides;
		for (size_t i = 0; i < m_Actions.size(); i++)
			if (m_Keys[i] != m_Actions[i].Default)
				overrides[m_Actions[i].Id] = Serialize(m_Keys[i]);
		return overrides;
	}

	std::string Keymap::Assign(const std::string& id, ImGuiKeyChord chord) {
		const int i = IndexOf(id);
		if (i < 0)
			return {};
		std::string lost;
		if (chord != ImGuiKey_None)
			for (size_t j = 0; j < m_Actions.size(); j++)
				if (static_cast<int>(j) != i && m_Keys[j] == chord && m_Actions[j].Context == m_Actions[i].Context) {
					m_Keys[j] = ImGuiKey_None;
					lost = m_Actions[j].Id;
				}
		m_Keys[i] = chord;
		return lost;
	}

	void Keymap::ResetAll() {
		for (size_t i = 0; i < m_Actions.size(); i++)
			m_Keys[i] = m_Actions[i].Default;
	}

	const Action* Keymap::Pressed(KeyContext context) const {
		const ImGuiKeyChord mods = ImGui::GetIO().KeyMods & (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiMod_Alt);
		for (size_t i = 0; i < m_Actions.size(); i++) {
			const ImGuiKeyChord chord = m_Keys[i];
			if (m_Actions[i].Context != context || chord == ImGuiKey_None || (chord & ImGuiMod_Mask_) != mods)
				continue;
			if (!ImGui::IsKeyPressed(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_), false))
				continue;
			if (m_Actions[i].Enabled && !m_Actions[i].Enabled())
				continue;
			return &m_Actions[i];
		}
		return nullptr;
	}

	bool Keymap::Down(const std::string& id) const {
		const ImGuiKeyChord chord = Key(id);
		return chord != ImGuiKey_None && ImGui::IsKeyDown(static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_));
	}

	std::string Keymap::Serialize(ImGuiKeyChord chord) {
		const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
		if (key == ImGuiKey_None)
			return {};
		std::string text;
		if (chord & ImGuiMod_Ctrl) text += "Ctrl+";
		if (chord & ImGuiMod_Shift) text += "Shift+";
		if (chord & ImGuiMod_Alt) text += "Alt+";
		return text + ImGui::GetKeyName(key);
	}

	ImGuiKeyChord Keymap::Parse(const std::string& text) {
		ImGuiKeyChord mods = 0;
		size_t start = 0;
		for (size_t plus; (plus = text.find('+', start)) != std::string::npos && plus + 1 < text.size(); start = plus + 1) {
			const std::string mod = text.substr(start, plus - start);
			if (mod == "Ctrl") mods |= ImGuiMod_Ctrl;
			else if (mod == "Shift") mods |= ImGuiMod_Shift;
			else if (mod == "Alt") mods |= ImGuiMod_Alt;
			else return ImGuiKey_None;
		}
		const std::string name = text.substr(start);
		for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; key++)
			if (name == ImGui::GetKeyName(static_cast<ImGuiKey>(key)))
				return mods | key;
		return ImGuiKey_None;
	}

	std::string Keymap::Format(ImGuiKeyChord chord) {
		const ImGuiKey key = static_cast<ImGuiKey>(chord & ~ImGuiMod_Mask_);
		if (key == ImGuiKey_None)
			return {};
		static const std::pair<ImGuiKey, const char*> shorter[] = {
			{ ImGuiKey_Comma, "," }, { ImGuiKey_Period, "." }, { ImGuiKey_Minus, "-" }, { ImGuiKey_Equal, "=" },
			{ ImGuiKey_Slash, "/" }, { ImGuiKey_Semicolon, ";" }, { ImGuiKey_Apostrophe, "'" },
			{ ImGuiKey_LeftBracket, "[" }, { ImGuiKey_RightBracket, "]" }, { ImGuiKey_Backslash, "\\" },
			{ ImGuiKey_GraveAccent, "`" }, { ImGuiKey_Escape, "Esc" }, { ImGuiKey_Delete, "Del" } };
		const std::string full = Serialize(chord);
		const std::string name = ImGui::GetKeyName(key);
		std::string text = full.substr(0, full.size() - name.size()); // i modificatori
		for (const auto& [k, symbol] : shorter)
			if (k == key)
				return text + symbol;
		return text + name;
	}
}
