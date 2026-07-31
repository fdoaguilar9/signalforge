#include "AppWindow.h"

#include <chrono>
#include <thread>

#include <SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

namespace sf
{

namespace
{

// Roughly 60Hz. A pedalboard UI has no reason to draw faster than this, and
// on a Pi sharing a machine with a real-time audio thread, every frame not
// drawn is headroom.
constexpr uint64_t kMinFrameMs = 16;

}  // namespace

bool AppWindow::Open(const char* title, int width, int height, std::string& error)
{
	// Ctrl-C has to keep reaching our own handler, which stops the engine and
	// tears the chain down in order. SDL installs handlers for SIGINT/SIGTERM
	// by default, and its idea of quitting doesn't know about any of that.
	SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");

	// Video only, deliberately. SDL has an audio subsystem of its own, and
	// there is exactly one audio path in this program -- JACK's.
	if (SDL_Init(SDL_INIT_VIDEO) != 0)
	{
		error = std::string("Could not initialise SDL: ") + SDL_GetError();
		return false;
	}

	sdl_ready_ = true;

	window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height,
		SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);

	if (!window_)
	{
		error = std::string("Could not open a window: ") + SDL_GetError();
		Close();
		return false;
	}

	renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);

	// A machine without working acceleration should still get a usable UI --
	// this is a pedalboard, not a game.
	if (!renderer_)
	{
		renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_SOFTWARE);
	}

	if (!renderer_)
	{
		error = std::string("Could not create a renderer: ") + SDL_GetError();
		Close();
		return false;
	}

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGui::StyleColorsDark();

	ImGuiIO& io = ImGui::GetIO();

	// No imgui.ini: a stray file in whatever directory the program happened to
	// start in is worse than no layout memory at all, and the one piece of
	// layout worth keeping -- the window's size -- is saved by the caller into
	// its own settings file (SF-32). There are no dockable panels or movable
	// windows here for Dear ImGui to have an opinion about.
	io.IniFilename = nullptr;

	if (!ImGui_ImplSDL2_InitForSDLRenderer(window_, renderer_) || !ImGui_ImplSDLRenderer2_Init(renderer_))
	{
		error = "Could not initialise the Dear ImGui SDL backend.";
		Close();
		return false;
	}

	imgui_ready_ = true;
	last_present_ms_ = SDL_GetTicks64();

	return true;
}

bool AppWindow::BeginFrame()
{
	SDL_Event event;
	bool quit = false;

	while (SDL_PollEvent(&event))
	{
		ImGui_ImplSDL2_ProcessEvent(&event);

		if (event.type == SDL_QUIT)
		{
			quit = true;
		}

		if (event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE &&
			event.window.windowID == SDL_GetWindowID(window_))
		{
			quit = true;
		}
	}

	if (quit)
	{
		return false;
	}

	ImGui_ImplSDLRenderer2_NewFrame();
	ImGui_ImplSDL2_NewFrame();
	ImGui::NewFrame();

	return true;
}

void AppWindow::EndFrame()
{
	ImGui::Render();

	const ImGuiIO& io = ImGui::GetIO();
	SDL_RenderSetScale(renderer_, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);

	SDL_SetRenderDrawColor(renderer_, 18, 18, 20, 255);
	SDL_RenderClear(renderer_);

	ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);
	SDL_RenderPresent(renderer_);

	// Presenting is *supposed* to block until the display is ready, but that
	// depends on a driver actually honouring vsync -- and SDL reports the flag
	// as set even when it doesn't. So pace on the clock: sleep only if the
	// frame really did come back early, which costs nothing when vsync works
	// and keeps the UI thread off a core spin when it doesn't.
	const uint64_t now_ms = SDL_GetTicks64();
	const uint64_t elapsed_ms = now_ms - last_present_ms_;

	if (elapsed_ms < kMinFrameMs)
	{
		std::this_thread::sleep_for(std::chrono::milliseconds(kMinFrameMs - elapsed_ms));
	}

	last_present_ms_ = SDL_GetTicks64();
}

void AppWindow::Size(int& width, int& height) const
{
	if (!window_)
	{
		return;
	}

	// Screen coordinates rather than the renderer's pixel size: on a HiDPI
	// display those differ by the scale factor, and handing a pixel size back
	// to SDL_CreateWindow next time would grow the window on every run.
	SDL_GetWindowSize(window_, &width, &height);
}

void AppWindow::Close()
{
	// Reverse order of construction, and each step guarded: Close also runs on
	// the failure paths in Open, where only some of this exists.
	if (imgui_ready_)
	{
		ImGui_ImplSDLRenderer2_Shutdown();
		ImGui_ImplSDL2_Shutdown();
		imgui_ready_ = false;
	}

	if (ImGui::GetCurrentContext())
	{
		ImGui::DestroyContext();
	}

	if (renderer_)
	{
		SDL_DestroyRenderer(renderer_);
		renderer_ = nullptr;
	}

	if (window_)
	{
		SDL_DestroyWindow(window_);
		window_ = nullptr;
	}

	if (sdl_ready_)
	{
		SDL_Quit();
		sdl_ready_ = false;
	}
}

AppWindow::~AppWindow()
{
	Close();
}

}  // namespace sf
