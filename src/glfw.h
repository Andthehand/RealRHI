#pragma once
#include "SwapchainDesc.h"

namespace RealRHI {
    inline WindowHandle GetWindowHandleFromGLFW(GLFWwindow* window) {
#if defined(_WIN32)
        HWND hwnd = glfwGetWin32Window(window);
        return WindowHandle::fromHwnd(hwnd);
#endif
        return {};
    }
}