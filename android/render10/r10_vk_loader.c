/* The game and the launcher probe load each Vulkan ICD through the same entry point. */
#define _GNU_SOURCE
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include "r10.h"
#include "panvk_identity.h"
#include "replacements/vulkan_driver.h"
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/system_properties.h>

#define TURNIP_DRIVER "libut3_turnip.so"

static char driver_path[PATH_MAX];
static char turnip_auto_file[PATH_MAX];
static int panvk;
int void_files_cache_directory(const char *profile, const char *name, char *path, size_t cap);

static int write_turnip_mode(const char *mode)
{
    FILE *file = fopen(turnip_auto_file, "w");
    if (!file) return 0;
    int written = fputs(mode, file) >= 0;
    return fclose(file) == 0 && written;
}

static void watch_turnip_mode(void)
{
    const char *profile_debug = getenv("TU_DEBUG_FILE");
    if (!profile_debug || access(profile_debug, F_OK) == 0 || errno != ENOENT) return;
    const char *profile = void_data_dir();
    char cache[PATH_MAX];
    if (!profile || !void_files_cache_directory(profile, "ShaderCache", cache, sizeof cache)) return;
    int n = snprintf(turnip_auto_file, sizeof turnip_auto_file, "%s/turnip-auto.txt", cache);
    /* This driver captures the path once. Its file watcher accepts changes after we identify the GPU. */
    if (n < 0 || n >= (int)sizeof turnip_auto_file || !write_turnip_mode("") || setenv("TU_DEBUG_FILE", turnip_auto_file, 1))
        turnip_auto_file[0] = 0;
}

PFN_vkGetInstanceProcAddr r10_vk_loader(char *error, size_t size)
{
    const char *mode = getenv("BIOSHOCK_VULKAN_DRIVER");
    driver_path[0] = 0;
    turnip_auto_file[0] = 0;
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
    if (!panvk && !custom) watch_turnip_mode();
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
    /* The bundled Turnip can lose the GPU in high-detail bloom passes on Adreno 618. Its sysmem path runs them. */
    if (turnip_auto_file[0] && !strcmp(properties->deviceName, "Turnip Adreno (TM) 618") && write_turnip_mode("sysmem"))
        R10_LOG("Vulkan: bundled Turnip on Adreno 618 uses sysmem for render passes");
    return 1;
}
