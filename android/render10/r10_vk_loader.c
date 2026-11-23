/* The game and the launcher probe load each Vulkan ICD through the same entry point. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "r10.h"
#include "panvk_identity.h"
#include "replacements/vulkan_driver.h"
#include <limits.h>
#include <stdlib.h>
#include <sys/system_properties.h>

#define TURNIP_DRIVER "libut3_turnip.so"

static char driver_path[PATH_MAX];
static int panvk;

PFN_vkGetInstanceProcAddr r10_vk_loader(char *error, size_t size)
{
    const char *mode = getenv("BIOSHOCK_VULKAN_DRIVER");
    driver_path[0] = 0;
    if (!mode || !*mode || !strcmp(mode, "system")) {
        PFN_vkGetInstanceProcAddr get = port_open_system_vulkan();
        if (!get) {
            const char *detail = dlerror();
            snprintf(error, size, "system Vulkan loader: %s", detail ? detail : "unavailable");
        }
        return get;
    }

    panvk = !strcmp(mode, "panvk");
    if (!panvk && strcmp(mode, "turnip")) {
        snprintf(error, size, "unknown Vulkan driver mode %s", mode);
        return NULL;
    }

    Dl_info info;
    const char *slash = dladdr((void *)&r10_vk_loader, &info) && info.dli_fname ? strrchr(info.dli_fname, '/') : NULL;
    char directory[PATH_MAX];
    if (!slash || snprintf(directory, sizeof directory, "%.*s", (int)(slash - info.dli_fname), info.dli_fname) >= (int)sizeof directory) {
        snprintf(error, size, "native library directory unknown");
        return NULL;
    }

    const char *custom = getenv(panvk ? "PORT_PANVK_DRIVER" : "PORT_TURNIP_DRIVER");
    if (custom && !*custom) custom = NULL;
    if (custom && custom[0] != '/') {
        snprintf(error, size, "the external Vulkan driver path must be absolute");
        return NULL;
    }
    if (panvk) {
        const char *page_error = port_panvk_page_size_error(!custom);
        if (page_error || access("/dev/mali0", R_OK | W_OK)) {
            snprintf(error, size, "%s", page_error ? page_error : "PanVK requires an accessible Mali GPU (/dev/mali0)");
            return NULL;
        }
        char product[PROP_VALUE_MAX];
        /* The begonia gralloc cannot display the image layout PanVK otherwise selects. */
        if (__system_property_get("ro.product.device", product) && !strcmp(product, "begonia"))
            setenv("MESA_GRALLOC_LINEAR", "1", 0);
    }

    const char *name = custom ? custom : panvk ? PORT_PANVK_DRIVER : TURNIP_DRIVER;
    if (snprintf(driver_path, sizeof driver_path, "%s%s%s", custom ? "" : directory, custom ? "" : "/", name) >= (int)sizeof driver_path) {
        snprintf(error, size, "Vulkan driver path too long");
        return NULL;
    }
    return port_open_vulkan_driver(directory, name, error, size);
}

int r10_vk_driver_active(PFN_vkGetInstanceProcAddr get, VkInstance instance, VkPhysicalDevice gpu,
                         const VkPhysicalDeviceProperties *properties, uint32_t instance_api)
{
    if (!driver_path[0]) {
        R10_LOG("VULKAN_DRIVER system");
        return 1;
    }

    VkPhysicalDeviceDriverProperties driver = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    if (instance_api >= VK_API_VERSION_1_1 && properties->apiVersion >= VK_API_VERSION_1_2) {
        PFN_vkGetPhysicalDeviceProperties2 get_properties =
            (PFN_vkGetPhysicalDeviceProperties2)get(instance, "vkGetPhysicalDeviceProperties2");
        if (get_properties) {
            VkPhysicalDeviceProperties2 p = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver };
            get_properties(gpu, &p);
        }
    }
    const char *mode = panvk ? "panvk" : "turnip";
    int correct = panvk ? properties->vendorID == 0x13B5 && driver.driverID == VK_DRIVER_ID_MESA_PANVK
                       : port_vulkan_is_turnip(properties, driver.driverID);
    if (!correct || !port_vulkan_driver_mapped(driver_path)) {
        R10_LOG("Vulkan: %s driver not active (device %s, vendor %04X, driver %u)",
                mode, properties->deviceName, properties->vendorID, driver.driverID);
        return 0;
    }
    R10_LOG("VULKAN_DRIVER %s %s (%s) Vulkan %u.%u", mode, properties->deviceName, driver_path,
            VK_API_VERSION_MAJOR(properties->apiVersion), VK_API_VERSION_MINOR(properties->apiVersion));
    return 1;
}
