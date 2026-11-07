#include "Editor/Widgets.h"

#include "imgui_internal.h"

#include <cmath>
#include <string>

namespace Lucerna::UI {

	namespace {
		Theme::Fonts s_Fonts;
		float s_Scale = 1.0f;
		constexpr float LABEL_WIDTH = 120.0f;

		std::string Id(const char* label) { return std::string("##") + label; }

		// Lo slider del mockup: un campo con la parte fino al valore riempita d'ambra, senza
		// maniglia. ImGui disegna il campo trasparente sopra; sotto, su un canale a parte, si
		// disegnano lo sfondo e il riempimento, che cosi' restano dietro al testo.
		template<class Draw>
		bool FilledField(float fraction, Draw draw) {
			ImDrawList* list = ImGui::GetWindowDrawList();
			list->ChannelsSplit(2);
			list->ChannelsSetCurrent(1);
			ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0, 0, 0, 0));
			ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, ImVec4(0, 0, 0, 0));
			ImGui::PushFont(s_Fonts.Mono);
			const bool changed = draw();
			ImGui::PopFont();
			ImGui::PopStyleColor(5);

			const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
			const ImVec4 background = ImGui::IsItemActive() ? Theme::Rgb(0x4a4a52)
				: ImGui::IsItemHovered() ? Theme::Rgb(0x42424a) : Theme::Field;
			const float rounding = ImGui::GetStyle().FrameRounding;
			list->ChannelsSetCurrent(0);
			list->AddRectFilled(min, max, ImGui::GetColorU32(background), rounding);
			if (fraction > 0.0f)
				list->AddRectFilled(min, ImVec2(min.x + (max.x - min.x) * std::fmin(fraction, 1.0f), max.y),
				                    ImGui::GetColorU32(Theme::AccentFill), rounding);
			list->ChannelsMerge();
			return changed;
		}

		float Fraction(float v, float min, float max, bool logarithmic) {
			if (max <= min)
				return 0.0f;
			if (logarithmic && min > 0.0f)
				return std::log(std::fmax(v, min) / min) / std::log(max / min);
			return (v - min) / (max - min);
		}

		// I campi numerici senza riempimento: solo il font a larghezza fissa
		template<class Draw>
		bool MonoField(Draw draw) {
			ImGui::PushFont(s_Fonts.Mono);
			const bool changed = draw();
			ImGui::PopFont();
			return changed;
		}
	}

	void Configure(const Theme::Fonts& fonts, float scale) {
		s_Fonts = fonts;
		s_Scale = scale;
	}

	ImFont* MonoFont() { return s_Fonts.Mono; }
	ImFont* BoldFont() { return s_Fonts.Bold; }
	float Scale() { return s_Scale; }

	void Label(const char* text) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(text, ImGui::FindRenderedTextEnd(text));
		ImGui::SameLine(LABEL_WIDTH * s_Scale);
		ImGui::SetNextItemWidth(-FLT_MIN);
	}

	bool SliderFloat(const char* label, float* v, float min, float max, const char* format, int flags) {
		Label(label);
		return FilledField(Fraction(*v, min, max, flags & ImGuiSliderFlags_Logarithmic),
			[&] { return ImGui::SliderFloat(Id(label).c_str(), v, min, max, format, flags); });
	}

	bool SliderInt(const char* label, int* v, int min, int max, const char* format) {
		Label(label);
		return FilledField(Fraction(static_cast<float>(*v), static_cast<float>(min), static_cast<float>(max), false),
			[&] { return ImGui::SliderInt(Id(label).c_str(), v, min, max, format); });
	}

	bool DragFloat(const char* label, float* v, float speed, float min, float max, const char* format, int flags) {
		Label(label);
		return MonoField([&] { return ImGui::DragFloat(Id(label).c_str(), v, speed, min, max, format, flags); });
	}

	bool DragFloat3(const char* label, float v[3], float speed, float min, float max, const char* format) {
		Label(label);
		return MonoField([&] { return ImGui::DragFloat3(Id(label).c_str(), v, speed, min, max, format); });
	}

	bool ColorEdit3(const char* label, float col[3], int flags) {
		Label(label);
		return MonoField([&] { return ImGui::ColorEdit3(Id(label).c_str(), col, flags); });
	}

	bool Combo(const char* label, int* current, const char* itemsZeroSeparated) {
		Label(label);
		return ImGui::Combo(Id(label).c_str(), current, itemsZeroSeparated);
	}

	bool Combo(const char* label, int* current, const char* const items[], int count) {
		Label(label);
		return ImGui::Combo(Id(label).c_str(), current, items, count);
	}

	// Etichetta a sinistra e la casella nella colonna dei campi, come le altre righe
	bool Checkbox(const char* label, bool* v) {
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted(label, ImGui::FindRenderedTextEnd(label));
		ImGui::SameLine(LABEL_WIDTH * s_Scale);
		ImGui::PushStyleColor(ImGuiCol_FrameBg, *v ? Theme::Accent : Theme::Field);
		ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, *v ? Theme::Rgb(0xffc56b) : Theme::Rgb(0x42424a));
		const bool changed = ImGui::Checkbox(Id(label).c_str(), v);
		ImGui::PopStyleColor(2);
		return changed;
	}

	bool InputText(const char* label, char* buffer, size_t size) {
		Label(label);
		return ImGui::InputText(Id(label).c_str(), buffer, size);
	}

	bool Section(const char* label, bool defaultOpen) {
		ImGui::PushFont(s_Fonts.Bold);
		const bool open = ImGui::CollapsingHeader(label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
		ImGui::PopFont();
		return open;
	}

	bool Pill(const char* label, bool active, bool enabled) {
		if (active) {
			ImGui::PushStyleColor(ImGuiCol_Button, Theme::Accent);
			ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Theme::Rgb(0xffc56b));
			ImGui::PushStyleColor(ImGuiCol_ButtonActive, Theme::Rgb(0xffd08a));
			ImGui::PushStyleColor(ImGuiCol_Text, Theme::AccentText);
		}
		ImGui::BeginDisabled(!enabled);
		const bool clicked = ImGui::Button(label);
		ImGui::EndDisabled();
		if (active)
			ImGui::PopStyleColor(4);
		return clicked;
	}
}
