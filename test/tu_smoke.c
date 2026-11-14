/*
 * tu_smoke: load a Vulkan driver .so directly (no Android loader), then
 *   1. print device identity + extension list
 *   2. render an offscreen triangle and verify pixels
 *   3. run a compute shader and verify results
 *
 * Works with Mesa Turnip (vk_icdGetInstanceProcAddr) and with Android HAL
 * drivers such as the stock Adreno blob (HMI / hwvulkan), so the same test
 * can A/B both drivers on the device from `adb shell`.
 *
 * usage: tu_smoke <path-to-driver.so> [--ext] [--bench N] [--trig]
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tri_vert.h"
#include "tri_frag.h"
#include "sq_comp.h"
#include "trig_comp.h"
#include <math.h>

/* minimal Android hwvulkan HAL definitions */
struct hw_module_t;
struct hw_device_t;
struct hw_module_methods_t {
   int (*open)(const struct hw_module_t *, const char *, struct hw_device_t **);
};
struct hw_module_t {
   uint32_t tag;
   uint16_t module_api_version, hal_api_version;
   const char *id, *name, *author;
   struct hw_module_methods_t *methods;
   void *dso;
   uint64_t reserved[32 - 7];
};
struct hw_device_t {
   uint32_t tag, version;
   struct hw_module_t *module;
   uint64_t reserved[12];
   int (*close)(struct hw_device_t *);
};
struct hwvulkan_device_t {
   struct hw_device_t common;
   PFN_vkEnumerateInstanceExtensionProperties EnumerateInstanceExtensionProperties;
   PFN_vkCreateInstance CreateInstance;
   PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
};

typedef PFN_vkVoidFunction (VKAPI_PTR *gipa_t)(VkInstance, const char *);
static gipa_t gipa;
static VkInstance inst;
static PFN_vkCreateInstance hal_create_instance;

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult _r = (x);                                                      \
      if (_r != VK_SUCCESS) {                                                 \
         printf("FAIL %s -> %d (line %d)\n", #x, _r, __LINE__);               \
         exit(3);                                                             \
      }                                                                       \
   } while (0)

#define F(name)                                                               \
   PFN_##name name = (PFN_##name)gipa(inst, #name);                           \
   if (!name) {                                                               \
      printf("FAIL missing entrypoint %s\n", #name);                          \
      exit(2);                                                                \
   }

static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits,
                         VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want)
         return i;
   printf("FAIL no memory type for flags 0x%x\n", want);
   exit(3);
}

int main(int argc, char **argv)
{
   if (argc < 2) {
      printf("usage: %s driver.so [--ext] [--bench N]\n", argv[0]);
      return 1;
   }
   int show_ext = 0, bench = 0, trig = 0;
   for (int i = 2; i < argc; i++) {
      if (!strcmp(argv[i], "--ext")) show_ext = 1;
      if (!strcmp(argv[i], "--trig")) trig = 1;
      if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
   }

   void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("FAIL dlopen: %s\n", dlerror());
      return 2;
   }
   gipa = (gipa_t)dlsym(h, "vk_icdGetInstanceProcAddr");
   if (gipa) {
      printf("entry: vk_icdGetInstanceProcAddr\n");
   } else {
      struct hw_module_t *hmi = dlsym(h, "HMI");
      if (!hmi) {
         printf("FAIL no vk_icdGetInstanceProcAddr and no HMI symbol\n");
         return 2;
      }
      struct hw_device_t *dev = NULL;
      int r = hmi->methods->open(hmi, "vk0", &dev);
      if (r || !dev) {
         printf("FAIL HMI open -> %d\n", r);
         return 2;
      }
      struct hwvulkan_device_t *hv = (struct hwvulkan_device_t *)dev;
      gipa = (gipa_t)hv->GetInstanceProcAddr;
      hal_create_instance = hv->CreateInstance;
      printf("entry: HMI (%s)\n", hmi->name ? hmi->name : "?");
   }

   /* ---- instance ---- */
   PFN_vkCreateInstance vkCreateInstance =
      hal_create_instance ? hal_create_instance
                          : (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!vkCreateInstance) {
      printf("FAIL no vkCreateInstance\n");
      return 2;
   }
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "tu_smoke",
                            .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
   CHECK(vkCreateInstance(&ici, NULL, &inst));

   F(vkEnumeratePhysicalDevices)
   F(vkGetPhysicalDeviceProperties2)
   F(vkGetPhysicalDeviceMemoryProperties)
   F(vkGetPhysicalDeviceQueueFamilyProperties)
   F(vkEnumerateDeviceExtensionProperties)
   F(vkCreateDevice)

   uint32_t n = 1;
   VkPhysicalDevice pd;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) {
      printf("FAIL no physical device (result %d, count %u)\n", er, n);
      return 3;
   }

   VkPhysicalDeviceDriverProperties drv = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
   VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                     .pNext = &drv};
   vkGetPhysicalDeviceProperties2(pd, &p2);
   VkPhysicalDeviceProperties *p = &p2.properties;
   printf("deviceName : %s\n", p->deviceName);
   printf("vendor/dev : 0x%04x / 0x%08x\n", p->vendorID, p->deviceID);
   printf("apiVersion : %u.%u.%u\n", VK_VERSION_MAJOR(p->apiVersion),
          VK_VERSION_MINOR(p->apiVersion), VK_VERSION_PATCH(p->apiVersion));
   printf("driverName : %s\n", drv.driverName);
   printf("driverInfo : %s\n", drv.driverInfo);
   printf("conformance: %u.%u.%u.%u\n", drv.conformanceVersion.major,
          drv.conformanceVersion.minor, drv.conformanceVersion.subminor,
          drv.conformanceVersion.patch);
   printf("maxStorageBufferRange: %u\n", p->limits.maxStorageBufferRange);

   uint32_t next = 0;
   vkEnumerateDeviceExtensionProperties(pd, NULL, &next, NULL);
   VkExtensionProperties *exts = calloc(next ? next : 1, sizeof(*exts));
   vkEnumerateDeviceExtensionProperties(pd, NULL, &next, exts);
   printf("extensions : %u\n", next);
   if (show_ext)
      for (uint32_t i = 0; i < next; i++)
         printf("EXT %s\n", exts[i].extensionName);

   /* ---- device ---- */
   uint32_t nq = 8;
   VkQueueFamilyProperties qf[8];
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   uint32_t qfi = 0;
   for (uint32_t i = 0; i < nq; i++)
      if ((qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) ==
          (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) {
         qfi = i;
         break;
      }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = qfi,
                                  .queueCount = 1,
                                  .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1,
                             .pQueueCreateInfos = &qci};
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   printf("device     : created\n");

   F(vkGetDeviceQueue)
   F(vkCreateImage)
   F(vkGetImageMemoryRequirements)
   F(vkAllocateMemory)
   F(vkBindImageMemory)
   F(vkCreateImageView)
   F(vkCreateRenderPass)
   F(vkCreateFramebuffer)
   F(vkCreateShaderModule)
   F(vkCreatePipelineLayout)
   F(vkCreateGraphicsPipelines)
   F(vkCreateComputePipelines)
   F(vkCreateBuffer)
   F(vkGetBufferMemoryRequirements)
   F(vkBindBufferMemory)
   F(vkMapMemory)
   F(vkCreateCommandPool)
   F(vkAllocateCommandBuffers)
   F(vkBeginCommandBuffer)
   F(vkEndCommandBuffer)
   F(vkCmdBeginRenderPass)
   F(vkCmdEndRenderPass)
   F(vkCmdBindPipeline)
   F(vkCmdDraw)
   F(vkCmdCopyImageToBuffer)
   F(vkCmdBindDescriptorSets)
   F(vkCmdDispatch)
   F(vkQueueSubmit)
   F(vkQueueWaitIdle)
   F(vkCreateDescriptorSetLayout)
   F(vkCreateDescriptorPool)
   F(vkAllocateDescriptorSets)
   F(vkUpdateDescriptorSets)
   F(vkResetCommandBuffer)

   VkQueue q;
   vkGetDeviceQueue(dev, qfi, 0, &q);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);

   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = qfi};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1,
                      .pCommandBuffers = &cb};

   /* ================= render test ================= */
   enum { W = 256, H = 256 };
   VkImageCreateInfo imi = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                            .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM,
                            .extent = {W, H, 1},
                            .mipLevels = 1,
                            .arrayLayers = 1,
                            .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
   VkImage img;
   CHECK(vkCreateImage(dev, &imi, NULL, &img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo mai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = mr.size,
                               .memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits,
                                                           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   VkDeviceMemory imem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));
   VkImageViewCreateInfo ivi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                .image = img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = imi.format,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
   VkImageView view;
   CHECK(vkCreateImageView(dev, &ivi, NULL, &view));

   VkAttachmentDescription att = {.format = imi.format,
                                  .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
   VkAttachmentReference aref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
   VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                               .colorAttachmentCount = 1,
                               .pColorAttachments = &aref};
   VkRenderPassCreateInfo rpi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                 .attachmentCount = 1,
                                 .pAttachments = &att,
                                 .subpassCount = 1,
                                 .pSubpasses = &sub};
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
   VkFramebufferCreateInfo fbi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                  .renderPass = rp,
                                  .attachmentCount = 1,
                                  .pAttachments = &view,
                                  .width = W,
                                  .height = H,
                                  .layers = 1};
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));

   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   VkShaderModule vs, fs, cs;
   smi.codeSize = sizeof(tri_vert); smi.pCode = tri_vert;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs));
   smi.codeSize = sizeof(tri_frag); smi.pCode = tri_frag;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs));
   smi.codeSize = sizeof(sq_comp); smi.pCode = sq_comp;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &cs));

   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
   VkPipelineLayout gl;
   CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &gl));

   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main"}};
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, W, H, 0, 1};
   VkRect2D sc = {{0, 0}, {W, H}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
   VkPipelineColorBlendAttachmentState cba = {.colorWriteMask = 0xf};
   VkPipelineColorBlendStateCreateInfo cbs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = 1, .pAttachments = &cba};
   VkGraphicsPipelineCreateInfo gpi = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
      .pRasterizationState = &rs, .pMultisampleState = &ms, .pColorBlendState = &cbs,
      .layout = gl, .renderPass = rp};
   VkPipeline gp;
   double t0 = now_ms();
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &gp));
   double t_gp = now_ms() - t0;

   VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                            .size = W * H * 4,
                            .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
   VkBuffer rb;
   CHECK(vkCreateBuffer(dev, &bi, NULL, &rb));
   vkGetBufferMemoryRequirements(dev, rb, &mr);
   mai.allocationSize = mr.size;
   mai.memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkDeviceMemory rmem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &rmem));
   CHECK(vkBindBufferMemory(dev, rb, rmem, 0));
   uint8_t *px;
   CHECK(vkMapMemory(dev, rmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));

   VkClearValue clear = {.color = {.float32 = {1, 0, 1, 1}}};
   VkRenderPassBeginInfo rbi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                .renderPass = rp, .framebuffer = fb,
                                .renderArea = {{0, 0}, {W, H}},
                                .clearValueCount = 1, .pClearValues = &clear};
   VkBufferImageCopy bic = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                            .imageExtent = {W, H, 1}};

   int frames = bench > 0 ? bench : 1;
   t0 = now_ms();
   for (int f = 0; f < frames; f++) {
      CHECK(vkResetCommandBuffer(cb, 0));
      CHECK(vkBeginCommandBuffer(cb, &cbi));
      vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRenderPass(cb);
      vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb, 1, &bic);
      CHECK(vkEndCommandBuffer(cb));
      CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(q));
   }
   double t_frames = now_ms() - t0;

   int bad = 0;
   const int pts[5][2] = {{128, 64}, {8, 8}, {247, 247}, {32, 200}, {200, 32}};
   for (int i = 0; i < 5; i++) {
      int x = pts[i][0], y = pts[i][1];
      const uint8_t *c = px + (y * W + x) * 4;
      int er_ = (int)((x + 0.5f) / W * 255.0f + 0.5f);
      int eg = (int)((y + 0.5f) / H * 255.0f + 0.5f);
      int ok = abs(c[0] - er_) <= 2 && abs(c[1] - eg) <= 2 && abs(c[2] - 64) <= 2 && c[3] == 255;
      if (!ok) {
         bad++;
         printf("  pixel(%d,%d) = %u,%u,%u,%u expected ~%d,%d,64,255\n", x, y, c[0], c[1],
                c[2], c[3], er_, eg);
      }
   }
   printf("render     : %s (pipeline %.1f ms, %d frame(s) %.2f ms avg)\n",
          bad ? "FAIL" : "PASS", t_gp, frames, t_frames / frames);

   /* ================= compute test ================= */
   enum { N = 1024 };
   bi.size = N * 4;
   bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
   VkBuffer sb;
   CHECK(vkCreateBuffer(dev, &bi, NULL, &sb));
   vkGetBufferMemoryRequirements(dev, sb, &mr);
   mai.allocationSize = mr.size;
   mai.memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                  VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkDeviceMemory smem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &smem));
   CHECK(vkBindBufferMemory(dev, sb, smem, 0));
   uint32_t *data;
   CHECK(vkMapMemory(dev, smem, 0, VK_WHOLE_SIZE, 0, (void **)&data));
   for (uint32_t i = 0; i < N; i++) data[i] = i;

   VkDescriptorSetLayoutBinding dlb = {.binding = 0,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &dlb};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &dps};
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
   VkDescriptorSetAllocateInfo dai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                      .descriptorPool = dp, .descriptorSetCount = 1,
                                      .pSetLayouts = &dsl};
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &dai, &ds));
   VkDescriptorBufferInfo dbi = {sb, 0, VK_WHOLE_SIZE};
   VkWriteDescriptorSet wds = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = ds, .dstBinding = 0, .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = &dbi};
   vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

   pli.setLayoutCount = 1;
   pli.pSetLayouts = &dsl;
   VkPipelineLayout cl;
   CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &cl));
   VkComputePipelineCreateInfo cpci = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = cs, .pName = "main"},
      .layout = cl};
   VkPipeline cp;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &cp));

   CHECK(vkResetCommandBuffer(cb, 0));
   CHECK(vkBeginCommandBuffer(cb, &cbi));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cp);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cl, 0, 1, &ds, 0, NULL);
   vkCmdDispatch(cb, N / 64, 1, 1);
   CHECK(vkEndCommandBuffer(cb));
   CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(q));
   int cbad = 0;
   for (uint32_t i = 0; i < N; i++)
      if (data[i] != i * i + 7) cbad++;
   printf("compute    : %s (%d/%d wrong)\n", cbad ? "FAIL" : "PASS", cbad, N);

   /* ================= sin/cos accuracy (--trig) =================
    * Runs sin(x) and cos(x) on the GPU for x spread over growing ranges and
    * reports the worst absolute error against the CPU's double-precision result.
    */
   if (trig) {
      VkShaderModule ts;
      smi.codeSize = sizeof(trig_comp); smi.pCode = trig_comp;
      CHECK(vkCreateShaderModule(dev, &smi, NULL, &ts));
      cpci.stage.module = ts;
      VkPipeline tp;
      CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &tp));
      enum { BUCKETS = 8, PER = N / 2 / BUCKETS };
      static const double maxes[BUCKETS] = {3.14159265358979, 10, 100, 1e3, 1e4, 1e5, 1e6, 1e7};
      static float in[N];
      float *fd = (float *)data;
      for (int b = 0; b < BUCKETS; b++)
         for (int k = 0; k < PER; k++) {
            float x = (float)(maxes[b] * (((k + 0.37) / PER) * 2.0 - 1.0));
            in[(b * PER + k) * 2] = in[(b * PER + k) * 2 + 1] = x;
         }
      memcpy(fd, in, sizeof(in));
      CHECK(vkResetCommandBuffer(cb, 0));
      CHECK(vkBeginCommandBuffer(cb, &cbi));
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, tp);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, cl, 0, 1, &ds, 0, NULL);
      vkCmdDispatch(cb, N / 64, 1, 1);
      CHECK(vkEndCommandBuffer(cb));
      CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(q));
      for (int b = 0; b < BUCKETS; b++) {
         double es = 0, ec = 0;
         for (int k = 0; k < PER; k++) {
            int i = (b * PER + k) * 2;
            double x = in[i];
            double ds_ = fabs((double)fd[i] - sin(x)), dc = fabs((double)fd[i + 1] - cos(x));
            if (!(ds_ <= es)) es = ds_; /* written this way so NaN counts as the max */
            if (!(dc <= ec)) ec = dc;
         }
         printf("trig |x|<=%-8g max_err sin=%.2e cos=%.2e\n", maxes[b], es, ec);
      }

      /* Fingerprint: hash the raw output bits for 256 rounds of pseudo-random
       * inputs (random sign/exponent/mantissa over 2^-30..2^60, plus inf, NaN,
       * zeros and denormals). Two drivers that print the same hash produce
       * bit-identical sin/cos for all ~262k inputs.
       */
      uint64_t s0 = 0x9e3779b97f4a7c15ull, hash = 0xcbf29ce484222325ull;
      uint32_t *ud = (uint32_t *)data;
      unsigned nan_out = 0;
      for (int round = 0; round < 256; round++) {
         for (int k = 0; k < N / 2; k++) {
            s0 ^= s0 << 13; s0 ^= s0 >> 7; s0 ^= s0 << 17;
            uint32_t mant = (uint32_t)(s0 >> 20) & 0x7fffff;
            uint32_t exp = 127 - 30 + (uint32_t)((s0 >> 44) % 91); /* 2^-30 .. 2^60 */
            uint32_t bits = ((uint32_t)(s0 >> 63) << 31) | (exp << 23) | mant;
            if (round == 0 && k < 8) {
               static const uint32_t special[8] = {0x00000000, 0x80000000, 0x7f800000, 0xff800000,
                                                   0x7fc00000, 0x00000001, 0x7f7fffff, 0x00800000};
               bits = special[k];
            }
            ud[k * 2] = ud[k * 2 + 1] = bits;
         }
         CHECK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
         CHECK(vkQueueWaitIdle(q));
         for (int i = 0; i < N; i++) {
            uint32_t v = ud[i];
            if ((v & 0x7f800000) == 0x7f800000 && (v & 0x7fffff)) {
               nan_out++;
               v = 0x7fc00000; /* all NaN payloads count as equal */
            }
            hash = (hash ^ v) * 0x100000001b3ull;
         }
      }
      printf("trig fingerprint: %016llx (NaN outputs: %u of %d)\n", (unsigned long long)hash,
             nan_out, 256 * N);
   }

   printf("RESULT %s\n", (bad || cbad) ? "FAIL" : "PASS");
   fflush(stdout);
   /* Skip teardown: we only care about results and exit immediately. */
   _exit((bad || cbad) ? 4 : 0);
}
