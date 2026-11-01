/* SPDX-License-Identifier: MIT
 * Validate the installed Linux ICD before publishing the graphics selection. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>

static void check(VkResult result, const char *operation)
{
    if (result != VK_SUCCESS) {
        fprintf(stderr, "%s failed: VkResult %d\n", operation, result);
        exit(1);
    }
}

int main(void)
{
    VkApplicationInfo application = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "arlinux-gpu-selection", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &application };
    VkInstance instance;
    check(vkCreateInstance(&info, NULL, &instance), "vkCreateInstance");
    uint32_t count = 0;
    check(vkEnumeratePhysicalDevices(instance, &count, NULL), "vkEnumeratePhysicalDevices(count)");
    VkPhysicalDevice *devices = calloc(count ? count : 1, sizeof(*devices));
    if (!devices) return 1;
    check(vkEnumeratePhysicalDevices(instance, &count, devices), "vkEnumeratePhysicalDevices");
    int usable = 0;
    for (uint32_t index = 0; index < count && !usable; ++index) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(devices[index], &properties);
        if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) continue;
        uint32_t queue_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &queue_count, NULL);
        VkQueueFamilyProperties *queues = calloc(queue_count ? queue_count : 1, sizeof(*queues));
        if (!queues) return 1;
        vkGetPhysicalDeviceQueueFamilyProperties(devices[index], &queue_count, queues);
        for (uint32_t queue = 0; queue < queue_count && !usable; ++queue) {
            if (!queues[queue].queueCount || !(queues[queue].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            float priority = 1.0f;
            VkDeviceQueueCreateInfo queue_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                .queueFamilyIndex = queue, .queueCount = 1, .pQueuePriorities = &priority };
            VkDeviceCreateInfo device_info = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                .queueCreateInfoCount = 1, .pQueueCreateInfos = &queue_info };
            VkDevice device;
            VkResult result = vkCreateDevice(devices[index], &device_info, NULL, &device);
            if (result == VK_SUCCESS) {
                printf("Hardware Vulkan ready: %s\n", properties.deviceName);
                vkDestroyDevice(device, NULL);
                usable = 1;
            } else fprintf(stderr, "vkCreateDevice(%s) failed: VkResult %d\n", properties.deviceName, result);
        }
        free(queues);
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    if (!usable) fprintf(stderr, "No usable hardware Vulkan graphics queue\n");
    return usable ? 0 : 1;
}
