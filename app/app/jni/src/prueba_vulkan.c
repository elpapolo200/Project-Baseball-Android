/* prueba_vulkan.c — primera prueba del port de Android (Project Baseball).
 *
 * Comprueba la cadena completa en el teléfono: SDL2 abre la ventana a pantalla completa, Vulkan crea
 * instancia, superficie, dispositivo y swapchain, y se borra la pantalla con un color que va
 * cambiando. Tocar la pantalla cambia la velocidad; el botón Atrás sale. Todo lo que pasa se escribe
 * en logcat (etiqueta SDL/APP) y en un archivo prueba.txt de la carpeta de la app, para poder
 * revisarlo sin conectar el móvil al PC. */
#include <SDL.h>
#include <SDL_vulkan.h>
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

static FILE *g_log;
static void anota(const char *fmt, ...) {
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof(b), fmt, ap); va_end(ap);
    SDL_Log("%s", b);
    if (g_log) { fprintf(g_log, "%s\n", b); fflush(g_log); }
}
#define VK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { anota("FALLO %s = %d", #x, (int)_r); return 1; } } while (0)

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    const char *dir = SDL_AndroidGetExternalStoragePath();
    char ruta[512];
    if (dir) { snprintf(ruta, sizeof(ruta), "%s/prueba.txt", dir); g_log = fopen(ruta, "w"); }
    anota("Project Baseball: prueba de Vulkan. Carpeta de la app: %s", dir ? dir : "(sin almacenamiento)");
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER) != 0) { anota("SDL_Init: %s", SDL_GetError()); return 1; }
    SDL_Window *win = SDL_CreateWindow("Project Baseball", 0, 0, 0, 0, SDL_WINDOW_VULKAN | SDL_WINDOW_FULLSCREEN);
    if (!win) { anota("SDL_CreateWindow: %s", SDL_GetError()); return 1; }

    unsigned int next = 0;
    SDL_Vulkan_GetInstanceExtensions(win, &next, NULL);
    const char *exts[16];
    if (next > 16) next = 16;
    SDL_Vulkan_GetInstanceExtensions(win, &next, exts);
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    app.pApplicationName = "Project Baseball"; app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &app; ici.enabledExtensionCount = next; ici.ppEnabledExtensionNames = exts;
    VkInstance inst; VK(vkCreateInstance(&ici, NULL, &inst));
    VkSurfaceKHR surf;
    if (!SDL_Vulkan_CreateSurface(win, inst, &surf)) { anota("CreateSurface: %s", SDL_GetError()); return 1; }

    uint32_t n = 1; VkPhysicalDevice phys;
    vkEnumeratePhysicalDevices(inst, &n, &phys);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp);
    anota("GPU: %s, Vulkan %u.%u.%u, driver 0x%X", pp.deviceName, VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion),
          VK_VERSION_PATCH(pp.apiVersion), pp.driverVersion);
    {   /* formatos que usa el port: vértices de 3 componentes y BC (no suele haber en móvil) */
        static const struct { VkFormat f; const char *n; int vtx; } fs[] = {
            { VK_FORMAT_R8G8B8_UNORM, "vertice u8n x3", 1 }, { VK_FORMAT_R16G16B16_SFLOAT, "vertice f16 x3", 1 },
            { VK_FORMAT_R8G8B8_SNORM, "vertice s8n x3", 1 }, { VK_FORMAT_R32G32B32_SFLOAT, "vertice f32 x3", 1 },
            { VK_FORMAT_BC1_RGBA_UNORM_BLOCK, "textura BC1", 0 }, { VK_FORMAT_R16G16B16A16_SFLOAT, "textura f16x4", 0 } };
        for (unsigned i = 0; i < sizeof(fs) / sizeof(fs[0]); i++) {
            VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(phys, fs[i].f, &fp);
            int ok = fs[i].vtx ? !!(fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT)
                               : !!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT);
            anota("  %s: %s", fs[i].n, ok ? "si" : "NO");
        }
    }
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, NULL);
    VkQueueFamilyProperties qf[8]; if (nq > 8) nq = 8; vkGetPhysicalDeviceQueueFamilyProperties(phys, &nq, qf);
    uint32_t fam = 0;
    for (uint32_t i = 0; i < nq; i++) { VkBool32 pr = 0; vkGetPhysicalDeviceSurfaceSupportKHR(phys, i, surf, &pr); if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && pr) { fam = i; break; } }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO }; qi.queueFamilyIndex = fam; qi.queueCount = 1; qi.pQueuePriorities = &prio;
    const char *dext[1] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO }; di.queueCreateInfoCount = 1; di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = 1; di.ppEnabledExtensionNames = dext;
    VkDevice dev; VK(vkCreateDevice(phys, &di, NULL, &dev));
    VkQueue q; vkGetDeviceQueue(dev, fam, 0, &q);

    VkSurfaceCapabilitiesKHR caps; vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surf, &caps);
    VkSurfaceFormatKHR sf[16]; uint32_t nf = 16; vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surf, &nf, sf);
    VkSwapchainCreateInfoKHR si = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    si.surface = surf; si.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && si.minImageCount > caps.maxImageCount) si.minImageCount = caps.maxImageCount;
    si.imageFormat = sf[0].format; si.imageColorSpace = sf[0].colorSpace; si.imageExtent = caps.currentExtent;
    si.imageArrayLayers = 1; si.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    si.preTransform = caps.currentTransform;
    si.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR : VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    si.presentMode = VK_PRESENT_MODE_FIFO_KHR; si.clipped = VK_TRUE;
    VkSwapchainKHR sw; VK(vkCreateSwapchainKHR(dev, &si, NULL, &sw));
    anota("pantalla %ux%u, formato %d, transformación 0x%X", caps.currentExtent.width, caps.currentExtent.height, (int)sf[0].format, caps.currentTransform);
    uint32_t ni = 0; vkGetSwapchainImagesKHR(dev, sw, &ni, NULL);
    VkImage imgs[8]; if (ni > 8) ni = 8; vkGetSwapchainImagesKHR(dev, sw, &ni, imgs);

    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO }; pci.queueFamilyIndex = fam;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    VkCommandPool pool; VK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO }; cai.commandPool = pool; cai.commandBufferCount = 1;
    VkCommandBuffer cb; VK(vkAllocateCommandBuffers(dev, &cai, &cb));
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore s_img, s_fin; vkCreateSemaphore(dev, &sci, NULL, &s_img); vkCreateSemaphore(dev, &sci, NULL, &s_fin);
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fen; vkCreateFence(dev, &fci, NULL, &fen);

    float t = 0.0f, vel = 1.0f; int fotos = 0; Uint32 t0 = SDL_GetTicks(), seg = t0;
    for (int fin = 0; !fin;) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_AC_BACK)) fin = 1;
            if (e.type == SDL_FINGERDOWN) { vel = vel >= 4.0f ? 0.5f : vel * 2.0f; anota("toque en %.2f, %.2f -> velocidad %.1f", e.tfinger.x, e.tfinger.y, vel); }
        }
        vkWaitForFences(dev, 1, &fen, VK_TRUE, UINT64_MAX); vkResetFences(dev, 1, &fen);
        uint32_t idx;
        VkResult r = vkAcquireNextImageKHR(dev, sw, UINT64_MAX, s_img, VK_NULL_HANDLE, &idx);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) { anota("acquire = %d", (int)r); break; }
        vkResetCommandBuffer(cb, 0);
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO }; vkBeginCommandBuffer(cb, &bi);
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = imgs[idx];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT; b.subresourceRange.levelCount = 1; b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        t += 0.02f * vel;
        VkClearColorValue col = { { 0.10f + 0.10f * sinf(t), 0.35f + 0.25f * sinf(t * 0.7f), 0.20f + 0.15f * sinf(t * 1.3f), 1.0f } };   /* verdes de campo */
        vkCmdClearColorImage(cb, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &col, 1, &b.subresourceRange);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b.dstAccessMask = 0;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b);
        vkEndCommandBuffer(cb);
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo sub = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        sub.waitSemaphoreCount = 1; sub.pWaitSemaphores = &s_img; sub.pWaitDstStageMask = &ws;
        sub.commandBufferCount = 1; sub.pCommandBuffers = &cb; sub.signalSemaphoreCount = 1; sub.pSignalSemaphores = &s_fin;
        vkQueueSubmit(q, 1, &sub, fen);
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1; pi.pWaitSemaphores = &s_fin; pi.swapchainCount = 1; pi.pSwapchains = &sw; pi.pImageIndices = &idx;
        vkQueuePresentKHR(q, &pi);
        fotos++;
        Uint32 ahora = SDL_GetTicks();
        if (ahora - seg >= 5000) { anota("%.1f FPS", fotos * 1000.0f / (ahora - seg)); fotos = 0; seg = ahora; }
    }
    vkDeviceWaitIdle(dev);
    anota("fin tras %.1f s", (SDL_GetTicks() - t0) / 1000.0f);
    if (g_log) fclose(g_log);
    SDL_Quit();
    return 0;
}
