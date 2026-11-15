/* SPDX-License-Identifier: MIT
 * Validate the installed Linux ICD before publishing the graphics selection. */
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Image and buffer masks need not overlap (for example on XRing O1/O3).
 * Validate real resources rather than treating device creation as usable GPU
 * memory, and never assume the first device-local type backs every resource. */
static VkResult resource_memory(VkPhysicalDevice physical, VkDevice device,
    VkBufferUsageFlags usage, const char *name)
{
    VkImage image = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory allocation = VK_NULL_HANDLE;
    VkMemoryRequirements requirements;
    VkResult result;
    if (!usage) {
        VkImageCreateInfo info = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
            .extent = {64, 64, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        result = vkCreateImage(device, &info, NULL, &image);
        if (result != VK_SUCCESS) goto done;
        vkGetImageMemoryRequirements(device, image, &requirements);
    } else {
        VkBufferCreateInfo info = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = 4096, .usage = usage, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
        result = vkCreateBuffer(device, &info, NULL, &buffer);
        if (result != VK_SUCCESS) goto done;
        vkGetBufferMemoryRequirements(device, buffer, &requirements);
    }
    VkPhysicalDeviceMemoryProperties memory;
    vkGetPhysicalDeviceMemoryProperties(physical, &memory);
    const int upload = usage == VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    const VkMemoryPropertyFlags preferred = upload ? VK_MEMORY_PROPERTY_HOST_COHERENT_BIT :
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = memory.memoryTypeCount;
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        const VkMemoryPropertyFlags flags = memory.memoryTypes[i].propertyFlags;
        if (!(requirements.memoryTypeBits & (1u << i)) ||
            (flags & (VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | VK_MEMORY_PROPERTY_PROTECTED_BIT)) ||
            (upload && !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) continue;
        if (type == memory.memoryTypeCount) type = i;
        if (flags & preferred) { type = i; break; }
    }
    if (type == memory.memoryTypeCount) {
        fprintf(stderr, "%s: no usable memory type in driver mask 0x%x (%u types)\n",
            name, requirements.memoryTypeBits, memory.memoryTypeCount);
        result = VK_ERROR_FEATURE_NOT_PRESENT;
        goto done;
    }
    VkMemoryAllocateInfo info = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size, .memoryTypeIndex = type };
    result = vkAllocateMemory(device, &info, NULL, &allocation);
    if (result != VK_SUCCESS) goto done;
    result = image ? vkBindImageMemory(device, image, allocation, 0) :
        vkBindBufferMemory(device, buffer, allocation, 0);
    if (result != VK_SUCCESS) goto done;
    if (upload) {
        void *mapping = NULL;
        result = vkMapMemory(device, allocation, 0, VK_WHOLE_SIZE, 0, &mapping);
        if (result != VK_SUCCESS) goto done;
        memset(mapping, 0x5a, 4096);
        if (!(memory.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            VkMappedMemoryRange range = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                .memory = allocation, .size = VK_WHOLE_SIZE };
            result = vkFlushMappedMemoryRanges(device, 1, &range);
        }
        vkUnmapMemory(device, allocation);
    }
    if (result == VK_SUCCESS)
        printf("%s: driver mask 0x%x, memory type %u flags 0x%x ready\n", name,
            requirements.memoryTypeBits, type, memory.memoryTypes[type].propertyFlags);
done:
    if (result != VK_SUCCESS) fprintf(stderr, "%s failed: VkResult %d\n", name, result);
    if (image) vkDestroyImage(device, image, NULL);
    if (buffer) vkDestroyBuffer(device, buffer, NULL);
    if (allocation) vkFreeMemory(device, allocation, NULL);
    return result;
}

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
                result = resource_memory(devices[index], device, 0, "color image");
                if (result == VK_SUCCESS) result = resource_memory(devices[index], device,
                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT, "CPU upload buffer");
                if (result == VK_SUCCESS) result = resource_memory(devices[index], device,
                    VK_BUFFER_USAGE_INDEX_BUFFER_BIT, "index buffer");
                if (result == VK_SUCCESS) result = resource_memory(devices[index], device,
                    VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, "indirect buffer");
                vkDestroyDevice(device, NULL);
                usable = result == VK_SUCCESS;
                if (usable) printf("Hardware Vulkan ready: %s\n", properties.deviceName);
                else fprintf(stderr, "GPU resource memory rejected: %s\n", properties.deviceName);
            } else fprintf(stderr, "vkCreateDevice(%s) failed: VkResult %d\n", properties.deviceName, result);
        }
        free(queues);
    }
    free(devices);
    vkDestroyInstance(instance, NULL);
    if (!usable) fprintf(stderr, "No usable hardware Vulkan graphics queue and resource memory\n");
    return usable ? 0 : 1;
}
