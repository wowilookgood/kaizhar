// Copyright 2026 wowilookgood
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

// The title picker's screen: drawn by the CPU (1280x720) and shown through a
// small Vulkan swapchain of its own on the default window, which the
// emulator's renderer takes over while a title runs. See switch_menu.h.
#include <switch.h>

#include <stdio.h>
#include <string.h>

#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_VI_NN
#include <vulkan/vulkan.h>

#include "menu_font.h"
#include "switch_menu.h"

#define MENU_W 1280
#define MENU_H 720

#define COLOR_BACKGROUND 0xff1c1714u // ABGR, as the image's bytes are R, G, B, A
#define COLOR_PANEL 0xff2a231fu
#define COLOR_SELECTED 0xffb86a2du
#define COLOR_TEXT 0xfff2efedu
#define COLOR_DIM 0xffa89c94u
#define COLOR_ACCENT 0xff5aa0f0u

// The driver's entry point (OunceVK is linked in; vk_platform.cpp uses it the same way).
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char* name);

// The Vulkan functions used, loaded by name.
#define VK_FUNCTIONS(X)                                                                            \
    X(CreateInstance) X(DestroyInstance) X(CreateViSurfaceNN) X(DestroySurfaceKHR)                \
    X(EnumeratePhysicalDevices) X(GetPhysicalDeviceMemoryProperties) X(CreateDevice)              \
    X(DestroyDevice) X(GetDeviceQueue) X(CreateSwapchainKHR) X(DestroySwapchainKHR)               \
    X(GetSwapchainImagesKHR) X(AcquireNextImageKHR) X(QueuePresentKHR) X(CreateCommandPool)       \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer)     \
    X(ResetCommandBuffer) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) X(QueueSubmit)            \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory)             \
    X(FreeMemory) X(BindBufferMemory) X(MapMemory) X(CreateFence) X(DestroyFence)                 \
    X(WaitForFences) X(ResetFences) X(CreateSemaphore) X(DestroySemaphore) X(DeviceWaitIdle)
#define X(name) static PFN_vk##name vk_##name;
VK_FUNCTIONS(X)
#undef X

static struct {
    VkInstance instance;
    VkSurfaceKHR surface;
    VkDevice device;
    VkQueue queue;
    VkSwapchainKHR swapchain;
    VkImage images[8];
    uint32_t imageCount;
    VkCommandPool pool;
    VkCommandBuffer cmd;
    VkBuffer buffer; // the frame, as the CPU draws it
    VkDeviceMemory memory;
    VkFence fence;
    VkSemaphore acquired, rendered;
} s_vk;

static bool s_open;
static PadState s_pad;
static bool s_padReady;
static u32* s_pixels; // the frame being drawn
static u32 s_stride;  // in pixels
static u64 s_repeatAt;
// What the menu draws into: MENU_W x MENU_H, or MENU_H x MENU_W in portrait (its top is then at
// the display's left edge, as the emulator's upright picture is; flipped, at the right edge).
static u32 s_canvas[MENU_W * MENU_H];
static bool s_portrait, s_flipped;
static int s_dw = MENU_W, s_dh = MENU_H;

void sw_menu_set_portrait(bool portrait, bool flipped) {
    s_portrait = portrait;
    s_flipped = portrait && flipped;
    s_dw = portrait ? MENU_H : MENU_W;
    s_dh = portrait ? MENU_W : MENU_H;
}

static bool check(VkResult result, const char* what) {
    if (result != VK_SUCCESS) {
        printf("hen: menu: %s: error %d\n", what, (int)result);
        return false;
    }
    return true;
}

void sw_menu_close(void) {
    if (s_vk.device != VK_NULL_HANDLE) {
        vk_DeviceWaitIdle(s_vk.device);
        if (s_vk.rendered) vk_DestroySemaphore(s_vk.device, s_vk.rendered, NULL);
        if (s_vk.acquired) vk_DestroySemaphore(s_vk.device, s_vk.acquired, NULL);
        if (s_vk.fence) vk_DestroyFence(s_vk.device, s_vk.fence, NULL);
        if (s_vk.buffer) vk_DestroyBuffer(s_vk.device, s_vk.buffer, NULL);
        if (s_vk.memory) vk_FreeMemory(s_vk.device, s_vk.memory, NULL);
        if (s_vk.pool) vk_DestroyCommandPool(s_vk.device, s_vk.pool, NULL);
        if (s_vk.swapchain) vk_DestroySwapchainKHR(s_vk.device, s_vk.swapchain, NULL);
        vk_DestroyDevice(s_vk.device, NULL);
    }
    if (s_vk.instance != VK_NULL_HANDLE) {
        if (s_vk.surface) vk_DestroySurfaceKHR(s_vk.instance, s_vk.surface, NULL);
        vk_DestroyInstance(s_vk.instance, NULL);
    }
    memset(&s_vk, 0, sizeof(s_vk));
    s_pixels = NULL;
    s_open = false;
    // The window's display layer holds the process's one grant of a GPU session
    // (native_window.h, nwindowReleaseVi2Layer): once a Vulkan driver has shut down, the
    // layer must be released before the GPU can be opened again (by the title's renderer).
    nwindowReleaseVi2Layer(nwindowGetDefault());
}

// The menu draws with Vulkan as the titles' renderer does (one image per frame, copied from
// memory): the display then only ever has Vulkan swapchains on it. (A libnx framebuffer
// before a title works, but can't be created again after one: 0x2559.)
bool sw_menu_open(void) {
    if (s_open) {
        return true;
    }
    memset(&s_vk, 0, sizeof(s_vk));
    // (As in sw_menu_close, after a title's renderer has shut down.)
    const Result released = nwindowReleaseVi2Layer(nwindowGetDefault());
    if (R_FAILED(released)) {
        printf("hen: menu: releasing the display layer: 0x%x\n", released);
    }
#define X(name) vk_##name = (PFN_vk##name)vkGetInstanceProcAddr(s_vk.instance, "vk" #name);
    vk_CreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    const char* instanceExtensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_NN_VI_SURFACE_EXTENSION_NAME};
    const VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                                   .pApplicationName = "Kaizhar menu",
                                   .apiVersion = VK_API_VERSION_1_1};
    const VkInstanceCreateInfo instanceInfo = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                               .pApplicationInfo = &app,
                                               .enabledExtensionCount = 2,
                                               .ppEnabledExtensionNames = instanceExtensions};
    if (vk_CreateInstance == NULL || !check(vk_CreateInstance(&instanceInfo, NULL, &s_vk.instance), "instance")) {
        return false;
    }
    VK_FUNCTIONS(X)
#undef X
    NWindow* win = nwindowGetDefault();
    nwindowSetDimensions(win, MENU_W, MENU_H);
    const VkViSurfaceCreateInfoNN surfaceInfo = {.sType = VK_STRUCTURE_TYPE_VI_SURFACE_CREATE_INFO_NN, .window = win};
    VkPhysicalDevice gpu = VK_NULL_HANDLE;
    uint32_t count = 1;
    VkResult result = vk_EnumeratePhysicalDevices(s_vk.instance, &count, &gpu);
    if ((result != VK_SUCCESS && result != VK_INCOMPLETE) || count == 0) {
        check(result != VK_SUCCESS ? result : VK_ERROR_INITIALIZATION_FAILED, "physical device");
        sw_menu_close();
        return false;
    }
    const float priority = 1.0f;
    const VkDeviceQueueCreateInfo queueInfo = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                               .queueFamilyIndex = 0,
                                               .queueCount = 1,
                                               .pQueuePriorities = &priority};
    const char* deviceExtensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    const VkDeviceCreateInfo deviceInfo = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                                           .queueCreateInfoCount = 1,
                                           .pQueueCreateInfos = &queueInfo,
                                           .enabledExtensionCount = 1,
                                           .ppEnabledExtensionNames = deviceExtensions};
    const VkSwapchainCreateInfoKHR swapchainInfo = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .minImageCount = 3,
        .imageFormat = VK_FORMAT_R8G8B8A8_UNORM,
        .imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR,
        .imageExtent = {MENU_W, MENU_H},
        .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
        .presentMode = VK_PRESENT_MODE_FIFO_KHR,
        .clipped = VK_TRUE,
    };
    VkSwapchainCreateInfoKHR swapchain = swapchainInfo;
    const VkCommandPoolCreateInfo poolInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                              .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                              .queueFamilyIndex = 0};
    const VkBufferCreateInfo bufferInfo = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                           .size = (VkDeviceSize)MENU_W * MENU_H * 4,
                                           .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    const VkFenceCreateInfo fenceInfo = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const VkSemaphoreCreateInfo semaphoreInfo = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    bool ok = check(vk_CreateViSurfaceNN(s_vk.instance, &surfaceInfo, NULL, &s_vk.surface), "surface") &&
              check(vk_CreateDevice(gpu, &deviceInfo, NULL, &s_vk.device), "device");
    if (ok) {
        vk_GetDeviceQueue(s_vk.device, 0, 0, &s_vk.queue);
        swapchain.surface = s_vk.surface;
        s_vk.imageCount = 8;
        ok = check(vk_CreateSwapchainKHR(s_vk.device, &swapchain, NULL, &s_vk.swapchain), "swapchain");
    }
    if (ok) {
        result = vk_GetSwapchainImagesKHR(s_vk.device, s_vk.swapchain, &s_vk.imageCount, s_vk.images);
        ok = (result == VK_SUCCESS || result == VK_INCOMPLETE) && s_vk.imageCount > 0;
        if (!ok) {
            check(VK_ERROR_INITIALIZATION_FAILED, "swapchain images");
        }
    }
    ok = ok && check(vk_CreateCommandPool(s_vk.device, &poolInfo, NULL, &s_vk.pool), "command pool");
    if (ok) {
        const VkCommandBufferAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                                      .commandPool = s_vk.pool,
                                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                                      .commandBufferCount = 1};
        ok = check(vk_AllocateCommandBuffers(s_vk.device, &allocate, &s_vk.cmd), "command buffer") &&
             check(vk_CreateBuffer(s_vk.device, &bufferInfo, NULL, &s_vk.buffer), "buffer");
    }
    if (ok) {
        VkMemoryRequirements need;
        vk_GetBufferMemoryRequirements(s_vk.device, s_vk.buffer, &need);
        VkPhysicalDeviceMemoryProperties memory;
        vk_GetPhysicalDeviceMemoryProperties(gpu, &memory);
        const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount; i++) {
            if ((need.memoryTypeBits & 1u << i) && (memory.memoryTypes[i].propertyFlags & want) == want) {
                type = i;
                break;
            }
        }
        const VkMemoryAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                               .allocationSize = need.size,
                                               .memoryTypeIndex = type};
        void* mapped = NULL;
        ok = type != UINT32_MAX && check(vk_AllocateMemory(s_vk.device, &allocate, NULL, &s_vk.memory), "memory") &&
             check(vk_BindBufferMemory(s_vk.device, s_vk.buffer, s_vk.memory, 0), "bind") &&
             check(vk_MapMemory(s_vk.device, s_vk.memory, 0, VK_WHOLE_SIZE, 0, &mapped), "map");
        s_pixels = mapped;
        s_stride = MENU_W;
    }
    ok = ok && check(vk_CreateFence(s_vk.device, &fenceInfo, NULL, &s_vk.fence), "fence") &&
         check(vk_CreateSemaphore(s_vk.device, &semaphoreInfo, NULL, &s_vk.acquired), "semaphore") &&
         check(vk_CreateSemaphore(s_vk.device, &semaphoreInfo, NULL, &s_vk.rendered), "semaphore");
    if (!ok) {
        sw_menu_close();
        return false;
    }
    if (!s_padReady) {
        padConfigureInput(1, HidNpadStyleSet_NpadStandard);
        padInitializeDefault(&s_pad);
        s_padReady = true;
    }
    s_open = true;
    return true;
}

static bool begin(void);

// Shows the frame drawn since begin(): copies it into the next swapchain image.
static void present(void) {
    if (!s_portrait) {
        for (int y = 0; y < MENU_H; y++) {
            memcpy(s_pixels + (size_t)y * s_stride, s_canvas + (size_t)y * MENU_W, MENU_W * sizeof(u32));
        }
    } else {
        for (int y = 0; y < MENU_H; y++) {
            u32* out = s_pixels + (size_t)y * s_stride;
            // A row of the display is a column of the canvas: read downwards from its top, or
            // (flipped: the same picture turned half a turn) upwards from its bottom.
            const u32* in = s_flipped ? s_canvas + (size_t)(MENU_W - 1) * MENU_H + y : s_canvas + (MENU_H - 1 - y);
            const ptrdiff_t step = s_flipped ? -MENU_H : MENU_H;
            for (int x = 0; x < MENU_W; x++) {
                out[x] = in[x * step];
            }
        }
    }
    uint32_t index = 0;
    VkResult result = vk_AcquireNextImageKHR(s_vk.device, s_vk.swapchain, UINT64_MAX, s_vk.acquired, VK_NULL_HANDLE, &index);
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        check(result, "acquire");
        svcSleepThread(16000000);
        return;
    }
    const VkCommandBufferBeginInfo beginInfo = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = s_vk.images[index],
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    const VkBufferImageCopy copy = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                                    .imageExtent = {MENU_W, MENU_H, 1}};
    vk_ResetCommandBuffer(s_vk.cmd, 0);
    vk_BeginCommandBuffer(s_vk.cmd, &beginInfo);
    vk_CmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    vk_CmdCopyBufferToImage(s_vk.cmd, s_vk.buffer, s_vk.images[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = 0;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    vk_CmdPipelineBarrier(s_vk.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &barrier);
    vk_EndCommandBuffer(s_vk.cmd);
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    const VkSubmitInfo submit = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                                 .waitSemaphoreCount = 1,
                                 .pWaitSemaphores = &s_vk.acquired,
                                 .pWaitDstStageMask = &waitStage,
                                 .commandBufferCount = 1,
                                 .pCommandBuffers = &s_vk.cmd,
                                 .signalSemaphoreCount = 1,
                                 .pSignalSemaphores = &s_vk.rendered};
    vk_ResetFences(s_vk.device, 1, &s_vk.fence);
    if (!check(vk_QueueSubmit(s_vk.queue, 1, &submit, s_vk.fence), "submit")) {
        return;
    }
    vk_WaitForFences(s_vk.device, 1, &s_vk.fence, VK_TRUE, UINT64_MAX);
    const VkPresentInfoKHR presentInfo = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                                          .waitSemaphoreCount = 1,
                                          .pWaitSemaphores = &s_vk.rendered,
                                          .swapchainCount = 1,
                                          .pSwapchains = &s_vk.swapchain,
                                          .pImageIndices = &index};
    vk_QueuePresentKHR(s_vk.queue, &presentInfo);
}

static void fill(int x, int y, int w, int h, u32 color) {
    if (x < 0) {
        w += x;
        x = 0;
    }
    if (y < 0) {
        h += y;
        y = 0;
    }
    if (x + w > s_dw) {
        w = s_dw - x;
    }
    if (y + h > s_dh) {
        h = s_dh - y;
    }
    for (int row = 0; row < h; row++) {
        u32* p = s_canvas + (size_t)(y + row) * s_dw + x;
        for (int i = 0; i < w; i++) {
            p[i] = color;
        }
    }
}

// The next character of UTF-8 text as Latin-1 ('?' for anything else).
static unsigned next_char(const char** text) {
    const unsigned char* s = (const unsigned char*)*text;
    unsigned c = *s++;
    if (c >= 0xc0) {
        const int extra = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : 1;
        c &= 0x3f >> extra;
        for (int i = 0; i < extra && (*s & 0xc0) == 0x80; i++) {
            c = c << 6 | (*s++ & 0x3f);
        }
    }
    *text = (const char*)s;
    return c < 32 || c > 255 || (c >= 127 && c < 160) ? '?' : c;
}

static int text_width(const struct menu_font* font, const char* text) {
    int width = 0;
    while (*text != 0) {
        width += font->glyphs[next_char(&text) - 32].advance;
    }
    return width;
}

// Draws text with its top left at x, y, no further right than max_x. Returns the x after it.
static int draw_text(const struct menu_font* font, int x, int y, int max_x, u32 color, const char* text) {
    const unsigned cr = color & 0xff, cg = color >> 8 & 0xff, cb = color >> 16 & 0xff;
    while (*text != 0) {
        const struct menu_glyph* g = &font->glyphs[next_char(&text) - 32];
        if (x + g->advance > max_x) {
            break;
        }
        for (int row = 0; row < g->h; row++) {
            const int py = y + g->top + row;
            if (py < 0 || py >= s_dh) {
                continue;
            }
            const unsigned char* src = font->pixels + (size_t)(g->y + row) * font->atlas_width + g->x;
            for (int col = 0; col < g->w; col++) {
                const int px = x + g->left + col;
                const unsigned a = src[col];
                if (a == 0 || px < 0 || px >= s_dw) {
                    continue;
                }
                u32* p = s_canvas + (size_t)py * s_dw + px;
                const u32 d = *p;
                const unsigned r = ((d & 0xff) * (255 - a) + cr * a) / 255;
                const unsigned gr = ((d >> 8 & 0xff) * (255 - a) + cg * a) / 255;
                const unsigned b = ((d >> 16 & 0xff) * (255 - a) + cb * a) / 255;
                *p = 0xff000000u | b << 16 | gr << 8 | r;
            }
        }
        x += g->advance;
    }
    return x;
}

static bool begin(void) {
    if (!s_open || s_pixels == NULL) {
        return false;
    }
    fill(0, 0, s_dw, s_dh, COLOR_BACKGROUND);
    return true;
}

static void heading(const char* text) {
    draw_text(&menu_font_heading, 56, 36, s_dw - 56, COLOR_TEXT, text);
    fill(56, 104, s_dw - 112, 2, COLOR_PANEL);
}

int sw_menu_frame(const char* title, const struct sw_menu_item* items, int count, int* selected,
                  const char* option_line, const char* hint_line) {
    if (!appletMainLoop()) {
        return SW_MENU_EXIT;
    }
    int action = SW_MENU_NONE;
    padUpdate(&s_pad);
    const u64 down = padGetButtonsDown(&s_pad);
    const u64 held = padGetButtons(&s_pad);
    // Up and down repeat while held.
    int move = 0;
    const u64 now = armGetSystemTick();
    // In portrait the display's left edge is up: with the controllers on the console, the
    // way to the next entry is the pad's right. The buttons marked up and down move too
    // (before 2026-11-13 they did nothing in portrait): the list has one
    // axis, so both readings can hold, and with the controllers off the console and held
    // the usual way they are the natural ones. Flipped (the right edge up), left and right
    // change places.
    const u64 before = s_flipped ? HidNpadButton_AnyRight : HidNpadButton_AnyLeft;
    const u64 after = s_flipped ? HidNpadButton_AnyLeft : HidNpadButton_AnyRight;
    const u64 up = s_portrait ? before | HidNpadButton_AnyUp : HidNpadButton_AnyUp;
    const u64 dn = s_portrait ? after | HidNpadButton_AnyDown : HidNpadButton_AnyDown;
    if (down & (up | dn)) {
        move = down & up ? -1 : 1;
        s_repeatAt = now + armNsToTicks(350000000ull);
    } else if ((held & (up | dn)) && now >= s_repeatAt) {
        move = held & up ? -1 : 1;
        s_repeatAt = now + armNsToTicks(80000000ull);
    }
    if (count > 0 && move != 0) {
        *selected = (*selected + move + count) % count;
    }
    if (down & HidNpadButton_A) {
        action = count > 0 ? SW_MENU_SELECT : SW_MENU_NONE;
    } else if (down & HidNpadButton_X) {
        action = SW_MENU_OPTION;
    } else if (down & HidNpadButton_Y) {
        action = SW_MENU_REFRESH;
    } else if (down & HidNpadButton_Plus) {
        action = SW_MENU_EXIT;
    }

    if (!begin()) {
        return action;
    }
    heading(title);
    // Below the list: a rule, the screens line, then the hint lines.
    int hint_lines = 0;
    for (const char* c = hint_line; c != NULL && *c != 0; c++) {
        hint_lines += *c == '\n';
    }
    hint_lines += hint_line != NULL;
    const int hint_top = s_dh - 32 - (hint_lines > 0 ? hint_lines : 1) * 32;
    const int option_top = hint_top - 64;
    enum { ROW = 62, TOP = 124 };
    const int ROWS = (option_top - 16 - TOP - 4) / ROW;
    int first = 0;
    if (count > ROWS) {
        first = *selected - ROWS / 2;
        if (first < 0) {
            first = 0;
        }
        if (first > count - ROWS) {
            first = count - ROWS;
        }
    }
    for (int i = first; i < count && i < first + ROWS; i++) {
        const int y = TOP + (i - first) * ROW;
        const bool on = i == *selected;
        if (on) {
            fill(40, y, s_dw - 80, ROW - 6, COLOR_SELECTED);
        }
        int right = s_dw - 64;
        if (items[i].detail != NULL && items[i].detail[0] != 0) {
            right -= text_width(&menu_font_small, items[i].detail);
            draw_text(&menu_font_small, right, y + 16, s_dw - 56, on ? COLOR_TEXT : COLOR_DIM, items[i].detail);
            right -= 24;
        }
        draw_text(&menu_font_text, 64, y + 9, right, COLOR_TEXT, items[i].title);
    }
    if (count == 0) {
        draw_text(&menu_font_text, 64, TOP + 9, s_dw - 64, COLOR_DIM, "No titles found.");
    }
    if (count > ROWS) {
        // A scroll bar.
        const int track = ROWS * ROW - 6;
        const int bar = track * ROWS / count;
        fill(s_dw - 30, TOP, 6, track, COLOR_PANEL);
        fill(s_dw - 30, TOP + (track - bar) * first / (count - ROWS), 6, bar, COLOR_DIM);
    }
    fill(56, option_top - 16, s_dw - 112, 2, COLOR_PANEL);
    if (option_line != NULL) {
        int x = 56;
        if (!s_portrait) {
            x = draw_text(&menu_font_text, x, option_top, s_dw - 56, COLOR_DIM, "Screens:  ");
        }
        draw_text(&menu_font_text, x, option_top, s_dw - 56, COLOR_ACCENT, option_line);
    }
    for (int line = 0; line < hint_lines; line++) {
        char text[160];
        size_t length = strcspn(hint_line, "\n");
        if (length >= sizeof(text)) {
            length = sizeof(text) - 1;
        }
        memcpy(text, hint_line, length);
        text[length] = 0;
        draw_text(&menu_font_small, 56, hint_top + line * 32, s_dw - 56, COLOR_DIM, text);
        hint_line += strcspn(hint_line, "\n");
        hint_line += *hint_line == '\n';
    }
    present();
    return action;
}

bool sw_menu_message(const char* title, const char* line1, const char* line2, float progress) {
    if (!begin()) {
        return appletMainLoop();
    }
    heading(title);
    if (line1 != NULL) {
        draw_text(&menu_font_text, 64, 150, s_dw - 64, COLOR_TEXT, line1);
    }
    if (line2 != NULL) {
        draw_text(&menu_font_text, 64, 200, s_dw - 64, COLOR_DIM, line2);
    }
    if (progress >= 0.0f) {
        const int width = s_dw - 128;
        fill(64, 280, width, 18, COLOR_PANEL);
        fill(64, 280, (int)(width * (progress > 1.0f ? 1.0f : progress)), 18, COLOR_ACCENT);
    }
    present();
    return appletMainLoop();
}

bool sw_menu_wait_button(void) {
    for (;;) {
        if (!appletMainLoop()) {
            return false;
        }
        padUpdate(&s_pad);
        if (padGetButtonsDown(&s_pad) & (HidNpadButton_A | HidNpadButton_B)) {
            return true;
        }
        svcSleepThread(16000000);
    }
}
