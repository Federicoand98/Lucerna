#include "Editor/Theme.h"
#include "Editor/Icons.h"

#include <filesystem>
#include <iostream>

namespace Lucerna::Theme {

	namespace {
		constexpr float TEXT_SIZE = 13.0f;
		constexpr float MONO_SIZE = 12.0f;

		// Un font di testo con le icone Lucide fuse dentro, alla stessa altezza
		ImFont* LoadWithIcons(const char* path, float size) {
			ImGuiIO& io = ImGui::GetIO();
			if (!std::filesystem::exists(path) || !std::filesystem::exists("fonts/lucide.ttf")) {
				std::cerr << "Font mancante: " << path << ", uso quello di default" << std::endl;
				return nullptr;
			}
			ImFont* font = io.Fonts->AddFontFromFileTTF(path, size);
			static const ImWchar iconRange[] = { ICON_MIN, ICON_MAX, 0 };
			ImFontConfig merge;
			merge.MergeMode = true;
			merge.PixelSnapH = true;
			merge.GlyphMinAdvanceX = size;           // icone a larghezza fissa: si allineano in colonna
			merge.GlyphOffset = ImVec2(0.0f, size * 0.15f); // Lucide sta un po' alto rispetto al testo
			io.Fonts->AddFontFromFileTTF("fonts/lucide.ttf", size, &merge, iconRange);
			return font;
		}
	}

	Fonts LoadFonts(float scale) {
		Fonts fonts;
		fonts.Regular = LoadWithIcons("fonts/Inter-Regular.ttf", TEXT_SIZE * scale);
		fonts.Bold = LoadWithIcons("fonts/Inter-SemiBold.ttf", TEXT_SIZE * scale);
		if (std::filesystem::exists("fonts/JetBrainsMono-Regular.ttf"))
			fonts.Mono = ImGui::GetIO().Fonts->AddFontFromFileTTF("fonts/JetBrainsMono-Regular.ttf", MONO_SIZE * scale);
		if (fonts.Regular == nullptr)
			fonts.Regular = ImGui::GetIO().Fonts->AddFontDefault();
		if (fonts.Bold == nullptr)
			fonts.Bold = fonts.Regular;
		if (fonts.Mono == nullptr)
			fonts.Mono = fonts.Regular;
		ImGui::GetIO().FontDefault = fonts.Regular;
		return fonts;
	}

	void Apply(float scale) {
		ImGuiStyle& style = ImGui::GetStyle();
		style = ImGuiStyle();

		style.WindowPadding = ImVec2(10, 10);
		style.FramePadding = ImVec2(8, 4);
		style.ItemSpacing = ImVec2(8, 7);
		style.ItemInnerSpacing = ImVec2(8, 4);
		style.IndentSpacing = 18;
		style.ScrollbarSize = 10;
		style.GrabMinSize = 8;
		style.WindowBorderSize = 1;
		style.FrameBorderSize = 0;
		style.PopupBorderSize = 1;
		style.TabBorderSize = 0;
		style.WindowRounding = 0;   // pannelli agganciati, e finestre staccate senza angoli strani
		style.ChildRounding = 3;
		style.FrameRounding = 3;
		style.PopupRounding = 4;
		style.ScrollbarRounding = 3;
		style.GrabRounding = 3;
		style.TabRounding = 3;
		style.WindowMenuButtonPosition = ImGuiDir_None;
		style.SeparatorTextBorderSize = 1;
		style.SeparatorTextPadding = ImVec2(0, 3);

		ImVec4* c = style.Colors;
		c[ImGuiCol_Text] = Text;
		c[ImGuiCol_TextDisabled] = TextDim;
		c[ImGuiCol_WindowBg] = Panel;
		c[ImGuiCol_ChildBg] = Rgb(0, 0.0f);
		c[ImGuiCol_PopupBg] = Rgb(0x2a2a2f);
		c[ImGuiCol_Border] = Border;
		c[ImGuiCol_BorderShadow] = Rgb(0, 0.0f);
		c[ImGuiCol_FrameBg] = Field;
		c[ImGuiCol_FrameBgHovered] = Rgb(0x42424a);
		c[ImGuiCol_FrameBgActive] = Rgb(0x4a4a52);
		c[ImGuiCol_TitleBg] = Panel;
		c[ImGuiCol_TitleBgActive] = Header;
		c[ImGuiCol_TitleBgCollapsed] = Panel;
		c[ImGuiCol_MenuBarBg] = Panel;
		c[ImGuiCol_ScrollbarBg] = Rgb(0, 0.0f);
		c[ImGuiCol_ScrollbarGrab] = Rgb(0x44444b);
		c[ImGuiCol_ScrollbarGrabHovered] = Rgb(0x55555d);
		c[ImGuiCol_ScrollbarGrabActive] = Rgb(0x66666e);
		c[ImGuiCol_CheckMark] = AccentText;
		c[ImGuiCol_SliderGrab] = Accent;
		c[ImGuiCol_SliderGrabActive] = Rgb(0xffd08a);
		c[ImGuiCol_Button] = Header;
		c[ImGuiCol_ButtonHovered] = Rgb(0x3a3a42);
		c[ImGuiCol_ButtonActive] = Rgb(0x44444c);
		c[ImGuiCol_Header] = Header;
		c[ImGuiCol_HeaderHovered] = Rgb(0x36363d);
		c[ImGuiCol_HeaderActive] = Selected;
		c[ImGuiCol_Separator] = Border;
		c[ImGuiCol_SeparatorHovered] = AccentFill;
		c[ImGuiCol_SeparatorActive] = Accent;
		c[ImGuiCol_ResizeGrip] = Rgb(0, 0.0f);
		c[ImGuiCol_ResizeGripHovered] = AccentFill;
		c[ImGuiCol_ResizeGripActive] = Accent;
		c[ImGuiCol_Tab] = Panel;
		c[ImGuiCol_TabHovered] = Header;
		c[ImGuiCol_TabActive] = Header;
		c[ImGuiCol_TabUnfocused] = Panel;
		c[ImGuiCol_TabUnfocusedActive] = Header;
		c[ImGuiCol_DockingPreview] = Rgb(0xffb547, 0.35f);
		c[ImGuiCol_DockingEmptyBg] = Background;
		c[ImGuiCol_PlotLines] = Accent;
		c[ImGuiCol_PlotHistogram] = Accent;
		c[ImGuiCol_TableHeaderBg] = Header;
		c[ImGuiCol_TableBorderStrong] = Border;
		c[ImGuiCol_TableBorderLight] = Border;
		c[ImGuiCol_TextSelectedBg] = Rgb(0xffb547, 0.30f);
		c[ImGuiCol_DragDropTarget] = Accent;
		c[ImGuiCol_NavHighlight] = Accent;
		c[ImGuiCol_ModalWindowDimBg] = Rgb(0x000000, 0.45f);

		style.ScaleAllSizes(scale);
	}
}
