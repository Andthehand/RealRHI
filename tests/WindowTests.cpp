#include "Device.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>

namespace {
	using namespace RealRHI;
	void Expect(bool condition, const char* message) {
		if (!condition) {
			std::cerr << "Test failed: " << message << '\n';
			std::exit(1);
		}
	}
}

int main() {
	using namespace RealRHI;
	std::unique_ptr<Device> device;
	Expect(Device::Create({}, device) == Result::Success, "Create window test device");
	Ref<Window> window;
	Expect(
		device->CreateWindow({.title = "Window API test", .width = 320, .height = 240, .enableHighPixelDensity = true},
			window) == Result::Success,
		"Create high-density window");
	Expect(window->GetTitle() == "Window API test", "Read initial title");
	Expect(window->SetTitle(nullptr) == Result::InvalidParameter && window->GetTitle() == "Window API test",
		"Reject null title without mutation");
	Expect(window->SetTitle("") == Result::Success && window->GetTitle().empty(), "Allow empty title");
	uint32_t width = 0, height = 0, pixelWidth = 0, pixelHeight = 0;
	Expect(window->GetSize(width, height) == Result::Success && width > 0 && height > 0, "Query window coordinates");
	Expect(window->GetSizeInPixels(pixelWidth, pixelHeight) == Result::Success && pixelWidth == window->GetWidth() &&
			pixelHeight == window->GetHeight(),
		"Preserve framebuffer size semantics");
	const float density = window->GetPixelDensity();
	Expect(density > 0 && window->GetDisplayScale() > 0, "Query positive DPI scales");
	Expect(std::abs(float(pixelWidth) / width - density) < 0.05f, "Density agrees with pixel-to-window ratio");
	Expect(window->SetSize(0, 200) == Result::InvalidParameter && window->SetSize(200, 0) == Result::InvalidParameter &&
			window->SetSize(std::numeric_limits<uint32_t>::max(), 200) == Result::InvalidParameter &&
			window->SetSize(200, std::numeric_limits<uint32_t>::max()) == Result::InvalidParameter,
		"Reject zero and overflowing sizes");
	uint32_t afterWidth = 0, afterHeight = 0;
	Expect(window->GetSize(afterWidth, afterHeight) == Result::Success && afterWidth == width && afterHeight == height,
		"Rejected sizes preserve dimensions");
	Expect(window->SetSize(400, 300) == Result::Success, "Request valid resize");
	int32_t x = 0, y = 0;
	Expect(window->GetPosition(x, y) == Result::Success && window->SetPosition(x, y) == Result::Success,
		"Read and request position");
	Expect(window->SetResizable(false) == Result::Success && window->SetResizable(true) == Result::Success,
		"Toggle resize control");
	Expect(window->SetBordered(false) == Result::Success && window->SetBordered(true) == Result::Success,
		"Toggle borders");
	Expect(window->SetAlwaysOnTop(false) == Result::Success, "Set stacking policy");
	// Use SDL's synchronization only in the test; production requests stay asynchronous.
	int count = 0;
	SDL_Window** nativeWindows = SDL_GetWindows(&count);
	Expect(nativeWindows && count == 1, "Find single test window");
	auto* nativeWindow = nativeWindows[0];
	SDL_free(nativeWindows);
	Expect(SDL_SyncWindow(nativeWindow), "Wait for window requests");
	SDL_PumpEvents();
	Expect(window->Hide() == Result::Success && SDL_SyncWindow(nativeWindow), "Hide test window");
	SDL_PumpEvents();
	Expect(!window->IsVisible(), "Query hidden state");
	Expect(window->Show() == Result::Success && SDL_SyncWindow(nativeWindow), "Show test window");
	SDL_PumpEvents();
	Expect(window->IsVisible() && !window->IsFullscreen(), "Query shown windowed state");
	const auto flags = SDL_GetWindowFlags(nativeWindow);
	Expect(window->IsMaximized() == ((flags & SDL_WINDOW_MAXIMIZED) != 0) &&
			window->IsMinimized() == ((flags & SDL_WINDOW_MINIMIZED) != 0) &&
			window->HasInputFocus() == ((flags & SDL_WINDOW_INPUT_FOCUS) != 0),
		"State queries match native window");
	window.Reset();
	Ref<Window> invalid;
	Expect(device->CreateWindow({.width = 0}, invalid) == Result::InvalidParameter && !invalid,
		"Reject invalid creation size");
	std::cout << "Window tests passed\n";
}
