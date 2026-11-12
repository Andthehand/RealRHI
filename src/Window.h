#pragma once
#include "RefCounted.h"
#include "Result.h"

#include <cstdint>
#include <string>

namespace RealRHI {
	class Window : public RefCounted {
	public:
		virtual ~Window() = default;

		// All window operations must run on the main thread. State changes are
		// requests to the window manager and can take effect asynchronously.
		virtual Result SetTitle(const char* title) = 0;
		virtual std::string GetTitle() const = 0;

		// Framebuffer dimensions in physical pixels. Zero indicates query failure.
		virtual uint32_t GetWidth() const = 0;
		virtual uint32_t GetHeight() const = 0;

		// Client size in window coordinates, which can differ from physical pixels.
		// Failed output-parameter queries leave the outputs unchanged.
		virtual Result GetSize(uint32_t& width, uint32_t& height) const = 0;
		virtual Result GetSizeInPixels(uint32_t& width, uint32_t& height) const = 0;
		virtual Result SetSize(uint32_t width, uint32_t height) = 0;

		virtual Result GetPosition(int32_t& x, int32_t& y) const = 0;
		virtual Result SetPosition(int32_t x, int32_t y) = 0;

		// Recommended UI content scale and physical pixels per window coordinate.
		// These are distinct values; both return zero on query failure.
		virtual float GetDisplayScale() const = 0;
		virtual float GetPixelDensity() const = 0;

		virtual Result SetResizable(bool resizable) = 0;
		virtual Result SetBordered(bool bordered) = 0;
		virtual Result SetAlwaysOnTop(bool alwaysOnTop) = 0;

		// Desktop fullscreen; exclusive display-mode selection is not exposed.
		virtual Result SetFullscreen(bool fullscreen) = 0;

		virtual Result Show() = 0;
		virtual Result Hide() = 0;

		virtual Result Minimize() = 0;
		virtual Result Maximize() = 0;
		virtual Result Restore() = 0;

		virtual bool IsVisible() const = 0;
		virtual bool IsMinimized() const = 0;
		virtual bool IsMaximized() const = 0;
		virtual bool IsFullscreen() const = 0;
		virtual bool HasInputFocus() const = 0;
	};
}
