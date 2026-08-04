#pragma once

#include <cstdint>
#include <string>

// SDL2 typedefs these as struct tags, so the header can stay out of the rest
// of the program -- nothing but AppWindow.cpp needs to know what a renderer is.
struct SDL_Window;
struct SDL_Renderer;

namespace sf
{

// The desktop window and its Dear ImGui context. Owns the platform, not the
// content: what gets drawn is the caller's business.
//
// Everything here runs on the main thread. Audio does not -- it runs in JACK's
// own real-time callback, and stays running whether or not this window is
// being drawn, resized, or dragged.
class AppWindow
{
public:
	~AppWindow();

	AppWindow() = default;
	AppWindow(const AppWindow&) = delete;
	AppWindow& operator=(const AppWindow&) = delete;

	bool Open(const char* title, int width, int height, std::string& error);

	// Pumps events and starts a frame. Returns false when the user has asked
	// to close the window, in which case no frame was started and EndFrame
	// must not be called.
	bool BeginFrame();

	// Draws the frame and waits for the display. This is what paces the loop.
	void EndFrame();

	// The window's current size in screen coordinates, for remembering it
	// across runs (SF-32). Leaves the outputs alone if there is no window.
	void Size(int& width, int& height) const;

	void Close();

	bool IsOpen() const { return window_ != nullptr; }

private:
	// Writes one frame to a file when SIGNALFORGE_UI_CAPTURE names one. A
	// development aid for looking at what was actually drawn -- see the
	// definition for why it exists at all.
	void CaptureFrame();

	// Counts every frame presented, so a capture can name one.
	long frame_index_ = 0;

	SDL_Window* window_ = nullptr;
	SDL_Renderer* renderer_ = nullptr;

	// When the last frame was presented, for the frame-rate cap. Measured
	// rather than inferred from the renderer's vsync flag, which SDL reports
	// as set even under the dummy driver, where presenting returns instantly.
	uint64_t last_present_ms_ = 0;

	bool imgui_ready_ = false;
	bool sdl_ready_ = false;
};

}  // namespace sf
