#pragma once
#include <cstdint>

namespace RealRHI {
    enum class WindowHandleType {
        Undefined,
        HWND,
        NSWindow,
        XlibWindow,
        AndroidWindow,
    };

    struct WindowHandle {
        WindowHandleType Type = WindowHandleType::Undefined;
        uint64_t HandleValues[2];

        static WindowHandle fromHwnd(void* hwnd) {
            WindowHandle handle = {};
            handle.Type = WindowHandleType::HWND;
            handle.HandleValues[0] = (uint64_t)(hwnd);
            return handle;
        }
        static WindowHandle fromNSWindow(void* nswindow) {
            WindowHandle handle = {};
            handle.Type = WindowHandleType::NSWindow;
            handle.HandleValues[0] = (uint64_t)(nswindow);
            return handle;
        }
        static WindowHandle fromXlibWindow(void* xdisplay, uint32_t xwindow) {
            WindowHandle handle = {};
            handle.Type = WindowHandleType::XlibWindow;
            handle.HandleValues[0] = (uint64_t)(xdisplay);
            handle.HandleValues[1] = xwindow;
            return handle;
        }
        static WindowHandle fromAndroidWindow(void* window) {
            WindowHandle handle = {};
            handle.Type = WindowHandleType::AndroidWindow;
            handle.HandleValues[0] = (uint64_t)(window);
            return handle;
        }
    };

    struct SwapchainDesc {
        WindowHandle Window;
        uint32_t Width, Height;
	};
}