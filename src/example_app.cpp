#include "example_app.h"

#include "app.h"
#include "host.h"
#include "imgui.h"
#include "imgui_util.h"
#include "os_window.h"

void example_app_init()
{

}

static constexpr i32 TITLE_BAR_HEIGHT = 32;
static constexpr i32 TITLE_BAR_BUTTONS = 2;

void example_app_tick(TempAllocator* fa)
{
    ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
	ImGui::SetNextWindowSize(io.DisplaySize);
	ImGui::Begin("example app", nullptr,
		ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBringToFrontOnFocus);

	draw_title_bar_buttons(TITLE_BAR_HEIGHT);
	draw_title_bar_title(TITLE_BAR_HEIGHT, "Example app", "- very cool");

    ImGui::Button("click me");

    ImGui::End();

    if(ImGui::IsKeyDown(ImGuiKey_Escape))
    {
        host_quit();
    }
}

App* example_app_get_app()
{
	static App app = {};
	app.name              = "Goto All";
	app.app_id            = "gotofile";
	app.init              = example_app_init;
	app.tick              = example_app_tick;
	app.on_activated      = nullptr;
	app.on_resize         = nullptr;
	app.begin_shutdown    = nullptr;
	app.wait_for_shutdown = nullptr;
	app.hotkeys           = nullptr;
	app.hotkey_count      = 0;
	app.initial_width     = 0;
	app.initial_height    = 0;
	app.title_bar_height        = TITLE_BAR_HEIGHT;
	app.title_bar_buttons_width = TITLE_BAR_HEIGHT * TITLE_BAR_BUTTONS;
	app.use_system_tray   = true;
	return &app;
}
