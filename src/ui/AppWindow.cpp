#include "AppWindow.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <SDL.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_sdlrenderer2.h"

#include "Fonts.h"
#include "Theme.h"

namespace sf
{

namespace
{

// Roughly 60Hz. A pedalboard UI has no reason to draw faster than this, and
// on a Pi sharing a machine with a real-time audio thread, every frame not
// drawn is headroom.
constexpr uint64_t kMinFrameMs = 16;

// What SDL clears to before Dear ImGui draws over it. The app background, so
// that the one frame between clear and draw isn't a different colour, and so
// that a resize showing bare renderer for an instant shows the right bare
// renderer.
constexpr Uint8 kClearRed = 22;    // --bg-app #16171a
constexpr Uint8 kClearGreen = 23;
constexpr Uint8 kClearBlue = 26;

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

	// The design system, in one call (SF-60). Everything below this line that
	// looks like a colour decision has already been made.
	ApplyTheme();

	// The typefaces (SF-61). A failure here is reported and then ignored: the
	// bundled bitmap font is worse-looking, not broken, and a board that
	// refuses to open a window because a .ttf moved is a board that can't be
	// played.
	std::string font_warning;

	if (!fonts::Load(font_warning))
	{
		std::fprintf(stderr, "signalforge: %s\n", font_warning.c_str());
	}

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

	SDL_SetRenderDrawColor(renderer_, kClearRed, kClearGreen, kClearBlue, 255);
	SDL_RenderClear(renderer_);

	ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), renderer_);

	// Before presenting, because reading back after a present is reading a
	// buffer the driver may already have swapped away from.
	CaptureFrame();

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

// Writes one rendered frame to a file, when asked to by the environment.
//
// This exists because **a look-and-feel change cannot be verified by a test
// that only asserts structure** (Milestone 10, SF-84): every ui_smoke
// assertion is about what was drawn, not about what it looks like, and the
// dummy video driver the tests run under has no window to photograph. Driven
// by the environment rather than a CLI flag because it is a development aid,
// not a feature of the program:
//
//   SIGNALFORGE_UI_CAPTURE=/tmp/board.bmp SIGNALFORGE_UI_CAPTURE_FRAME=3 \
//     signalforge --test-ui 60
//
// One frame, named by number, because the alternative is either 200 files or
// whichever frame happened to be last -- and the last frame of a self-test run
// is mid-click with a modal open.
void AppWindow::CaptureFrame()
{
	const char* path = std::getenv("SIGNALFORGE_UI_CAPTURE");

	if (!path)
	{
		return;
	}

	const char* frame_text = std::getenv("SIGNALFORGE_UI_CAPTURE_FRAME");
	const long wanted = frame_text ? std::strtol(frame_text, nullptr, 10) : 0;

	if (frame_index_++ != wanted)
	{
		return;
	}

	int width = 0;
	int height = 0;
	SDL_GetRendererOutputSize(renderer_, &width, &height);

	SDL_Surface* surface = SDL_CreateRGBSurfaceWithFormat(0, width, height, 32, SDL_PIXELFORMAT_ARGB8888);

	if (!surface)
	{
		std::fprintf(stderr, "signalforge: could not allocate a capture surface: %s\n", SDL_GetError());
		return;
	}

	if (SDL_RenderReadPixels(renderer_, nullptr, SDL_PIXELFORMAT_ARGB8888, surface->pixels, surface->pitch) != 0)
	{
		std::fprintf(stderr, "signalforge: could not read the frame back: %s\n", SDL_GetError());
	}
	else if (SDL_SaveBMP(surface, path) != 0)
	{
		std::fprintf(stderr, "signalforge: could not write %s: %s\n", path, SDL_GetError());
	}
	else
	{
		std::fprintf(stderr, "signalforge: wrote frame %ld to %s\n", wanted, path);
	}

	SDL_FreeSurface(surface);
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
