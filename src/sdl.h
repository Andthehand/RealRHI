#pragma once
#include "SwapchainDesc.h"
#include <SDL3/SDL.h>

namespace RealRHI {
    // Get WindowHandle from SDL3 window for cross-platform window surface creation
    // Supports: Windows (HWND), macOS (NSWindow), Linux X11 (Xlib)
    // Note: Wayland is not yet supported - will return empty WindowHandle on Wayland
    // Returns empty WindowHandle{} if window handle extraction fails
    inline WindowHandle GetWindowHandleFromSDL(SDL_Window* window) {
#if defined(_WIN32)
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        if (hwnd) {
            return WindowHandle::fromHwnd(hwnd);
        }
#elif defined(__APPLE__)
        void* nswindow = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (nswindow) {
            return WindowHandle::fromNSWindow(nswindow);
        }
#elif defined(__linux__)
        // Check if running under X11
        const char* videoDriver = SDL_GetCurrentVideoDriver();
        if (videoDriver && SDL_strcmp(videoDriver, "x11") == 0) {
            void* xdisplay = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            uint32_t xwindow = (uint32_t)SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
            if (xdisplay) {
                return WindowHandle::fromXlibWindow(xdisplay, xwindow);
            }
        }
        // TODO: Add Wayland support using SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER 
        // and SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER when WindowHandle supports it
#endif
        return {};
    }
}
