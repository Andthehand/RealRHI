#pragma once
#include "SwapchainDesc.h"
#include <SDL3/SDL.h>

namespace RealRHI {
    inline WindowHandle GetWindowHandleFromSDL(SDL_Window* window) {
#if defined(_WIN32)
        HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
        return WindowHandle::fromHwnd(hwnd);
#elif defined(__APPLE__)
        void* nswindow = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        return WindowHandle::fromNSWindow(nswindow);
#elif defined(__linux__)
        if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
            void* xdisplay = SDL_GetPointerProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
            uint32_t xwindow = (uint32_t)SDL_GetNumberProperty(SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
            return WindowHandle::fromXlibWindow(xdisplay, xwindow);
        }
#endif
        return {};
    }
}
