/*
 * tu_bench: offscreen GPU micro-benchmarks for A/B testing Vulkan drivers on
 * the device from `adb shell`. Loads the driver .so directly (Turnip or the
 * stock Adreno blob), no Android loader / no window needed.
 *
 * Scenarios (1920x1080 unless noted):
 *   fill       1 render pass, 24 fullscreen alpha-blended layers      (fill rate / bandwidth)
 *   heavy_fs   1 render pass, 2 fullscreen ALU-heavy fragment draws    (shader throughput)
 *   drawcalls  1 render pass, 10000 small depth-tested quads           (driver CPU overhead)
 *   multipass  40 render passes on 960x540 targets, LOAD/STORE + sample
 *              of the previous target, 3 draws each                    (emulator-like pass churn)
 *   pipelines  150 uncached graphics pipeline compiles                 (shader-compile stutter)
 *   subpass    1 render pass, 2 subpasses: 12 depth-tested G-buffer draws into two transient
 *              colour targets, then a fullscreen lighting draw reading them as input
 *              attachments                                              (deferred shading)
 *   resolve    1 render pass, 4x MSAA colour, 8 blended fullscreen layers, resolve
 *              attachment                                               (MSAA resolve)
 *   subpass and resolve are the cases where GMEM should beat SYSMEM; they are not part of
 *   "all" (run them by name, or "gmemcases" for both).
 *
 * usage: tu_bench <driver.so> [frames] [scenario|all] [scale]
 * output: one "BENCH <name> cpu_ms=<median record time> frame_ms=<median total>" per scenario
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "quad_vert.h"
#include "simple_frag.h"
#include "heavy_frag.h"
#include "tex_frag.h"
#include "gbuf_frag.h"
#include "light_frag.h"

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

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult _r = (x);                                                      \
      if (_r != VK_SUCCESS) {                                                 \
         printf("FAIL %s -> %d (line %d)\n", #x, _r, __LINE__);               \
         fflush(stdout);                                                      \
         _exit(3);                                                            \
      }                                                                       \
   } while (0)

/* All entrypoints we use, resolved once into globals. */
#define FUNCS(X)                                                              \
   X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties)             \
   X(vkGetPhysicalDeviceMemoryProperties)                                     \
   X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkCreateDevice)              \
   X(vkGetDeviceQueue) X(vkCreateImage) X(vkGetImageMemoryRequirements)       \
   X(vkAllocateMemory) X(vkBindImageMemory) X(vkCreateImageView)              \
   X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCreateShaderModule)       \
   X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines)                     \
   X(vkDestroyPipeline) X(vkCreateCommandPool) X(vkAllocateCommandBuffers)    \
   X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer)      \
   X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) X(vkCmdBindPipeline)         \
   X(vkCmdDraw) X(vkCmdPushConstants) X(vkCmdBindDescriptorSets)              \
   X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkQueueSubmit)           \
   X(vkQueueWaitIdle) X(vkCreateDescriptorSetLayout)                          \
   X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets)                      \
   X(vkUpdateDescriptorSets) X(vkCreateSampler) X(vkCmdNextSubpass)
#define DECL(n) static PFN_##n n;
FUNCS(DECL)
#define LOAD(n)                                                               \
   n = (PFN_##n)gipa(inst, #n);                                               \
   if (!n) {                                                                  \
      printf("FAIL missing entrypoint %s\n", #n);                             \
      _exit(2);                                                               \
   }

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb;
static VkPhysicalDeviceMemoryProperties mp;

static double now_ms(void)
{
   struct timespec ts;
   clock_gettime(CLOCK_MONOTONIC, &ts);
   return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int cmp_d(const void *a, const void *b)
{
   double x = *(const double *)a, y = *(const double *)b;
   return x < y ? -1 : x > y;
}

static double median(double *v, int n)
{
   qsort(v, n, sizeof(double), cmp_d);
   return v[n / 2];
}

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   printf("FAIL no memory type\n");
   _exit(3);
}

struct target {
   VkImage img;
   VkImageView view;
};

static struct target make_image_ms(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                                   VkImageAspectFlags aspect, VkSampleCountFlagBits samples)
{
   struct target t;
   VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           .imageType = VK_IMAGE_TYPE_2D,
                           .format = fmt,
                           .extent = {w, h, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = samples,
                           .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = usage};
   CHECK(vkCreateImage(dev, &ci, NULL, &t.img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, t.img, &mr);
   VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                              .allocationSize = mr.size,
                              .memoryTypeIndex =
                                 find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &ai, NULL, &mem));
   CHECK(vkBindImageMemory(dev, t.img, mem, 0));
   VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                               .image = t.img,
                               .viewType = VK_IMAGE_VIEW_TYPE_2D,
                               .format = fmt,
                               .subresourceRange = {aspect, 0, 1, 0, 1}};
   CHECK(vkCreateImageView(dev, &vi, NULL, &t.view));
   return t;
}

static struct target make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                                VkImageAspectFlags aspect)
{
   return make_image_ms(w, h, fmt, usage, aspect, VK_SAMPLE_COUNT_1_BIT);
}

struct pc {
   float tint[4];
   float xform[4]; /* scale.xy, offset.xy in NDC */
   float z;
   float dyn_iter; /* runtime loop count for heavy.frag MODE >= 100 */
   float pad[2];
};

static VkPipelineLayout layout;
static VkShaderModule vs_mod;

static int spec_mode = 0; /* heavy.frag MODE specialization constant for the next pipeline */

/* Extra pipeline parameters for the multi-subpass / MSAA scenarios. */
struct pipe_ex {
   VkPipelineLayout layout; /* 0: the shared layout */
   uint32_t subpass;
   uint32_t color_count;    /* 0: 1 */
   VkSampleCountFlagBits samples;
};

static VkPipeline make_pipeline_ex(VkRenderPass rp, VkShaderModule fs, int blend, int depth,
                                   uint32_t w, uint32_t h, int spec_iter, struct pipe_ex ex);

static VkPipeline make_pipeline(VkRenderPass rp, VkShaderModule fs, int blend, int depth,
                                uint32_t w, uint32_t h, int spec_iter)
{
   return make_pipeline_ex(rp, fs, blend, depth, w, h, spec_iter, (struct pipe_ex){0});
}

static VkPipeline make_pipeline_ex(VkRenderPass rp, VkShaderModule fs, int blend, int depth,
                                   uint32_t w, uint32_t h, int spec_iter, struct pipe_ex ex)
{
   uint32_t ncolor = ex.color_count ? ex.color_count : 1;
   VkSpecializationMapEntry sme[2] = {{0, 0, sizeof(int32_t)},
                                      {1, sizeof(int32_t), sizeof(int32_t)}};
   int32_t spec_data[2] = {spec_iter > 0 ? spec_iter : 24, spec_mode};
   VkSpecializationInfo spec = {2, sme, sizeof(spec_data), spec_data};
   VkPipelineShaderStageCreateInfo stages[2] = {
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs_mod, .pName = "main"},
      {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
       .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main",
       .pSpecializationInfo = (spec_iter > 0 || spec_mode > 0) ? &spec : NULL}};
   VkPipelineVertexInputStateCreateInfo vi = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
   VkPipelineInputAssemblyStateCreateInfo ia = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
      .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
   VkViewport vp = {0, 0, (float)w, (float)h, 0, 1};
   VkRect2D sc = {{0, 0}, {w, h}};
   VkPipelineViewportStateCreateInfo vps = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
      .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc};
   VkPipelineRasterizationStateCreateInfo rs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
      .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f};
   VkPipelineMultisampleStateCreateInfo ms = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
      .rasterizationSamples = ex.samples ? ex.samples : VK_SAMPLE_COUNT_1_BIT};
   VkPipelineDepthStencilStateCreateInfo dss = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
      .depthTestEnable = depth, .depthWriteEnable = depth,
      .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL};
   VkPipelineColorBlendAttachmentState cba = {
      .blendEnable = blend,
      .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
      .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = 0xf};
   VkPipelineColorBlendAttachmentState cbas[4] = {cba, cba, cba, cba};
   VkPipelineColorBlendStateCreateInfo cbs = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
      .attachmentCount = ncolor, .pAttachments = cbas};
   VkGraphicsPipelineCreateInfo gpi = {
      .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
      .stageCount = 2, .pStages = stages,
      .pVertexInputState = &vi, .pInputAssemblyState = &ia, .pViewportState = &vps,
      .pRasterizationState = &rs, .pMultisampleState = &ms, .pDepthStencilState = &dss,
      .pColorBlendState = &cbs, .layout = ex.layout ? ex.layout : layout, .renderPass = rp,
      .subpass = ex.subpass};
   VkPipeline p;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &p));
   return p;
}

static void push(const struct pc *p)
{
   vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                      sizeof(*p), p);
}

/* ---- scene state shared by the record callbacks ---- */
enum { W = 1920, H = 1080, SW = 960, SH = 540 };
static VkRenderPass rp_main, rp_load;
static VkFramebuffer fb_main, fb_small[2];
static VkPipeline p_fill, p_heavy, p_quad, p_tex;
static VkDescriptorSet ds_small[2];
static int scale = 1; /* multiplies layers / passes so GPU time dwarfs timing jitter */
/* subpass / resolve scenarios */
static VkRenderPass rp_deferred, rp_msaa;
static VkFramebuffer fb_deferred, fb_msaa;
static VkPipeline p_gbuf, p_light, p_msaa_fill;
static VkPipelineLayout layout_in;
static VkDescriptorSet ds_gbuf;

static void begin_main(void)
{
   VkClearValue cv[2] = {{.color = {.float32 = {0.05f, 0.05f, 0.1f, 1}}},
                         {.depthStencil = {1.0f, 0}}};
   VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                               .renderPass = rp_main, .framebuffer = fb_main,
                               .renderArea = {{0, 0}, {W, H}},
                               .clearValueCount = 2, .pClearValues = cv};
   vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
}

static void rec_fill(int frame)
{
   begin_main();
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_fill);
   for (int i = 0; i < 24 * scale; i++) {
      struct pc p = {{(i & 1) * 1.0f, ((i >> 1) & 1) * 1.0f, ((i + frame) & 3) * 0.33f, 0.1f},
                     {2, 2, -1, -1}, 0.5f};
      push(&p);
      vkCmdDraw(cb, 6, 1, 0, 0);
   }
   vkCmdEndRenderPass(cb);
}

static void rec_heavy(int frame)
{
   begin_main();
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_heavy);
   for (int i = 0; i < 2 * scale; i++) {
      struct pc p = {{1, 1 - (i & 1) * 0.5f, (frame & 7) / 8.0f, 1}, {2, 2, -1, -1}, 0.5f, 24.0f};
      push(&p);
      vkCmdDraw(cb, 6, 1, 0, 0);
   }
   vkCmdEndRenderPass(cb);
}

static void rec_drawcalls(int frame)
{
   begin_main();
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_quad);
   for (int i = 0; i < 10000; i++) {
      int gx = i % 100, gy = i / 100;
      struct pc p = {{gx / 100.0f, gy / 100.0f, ((i + frame) % 7) / 7.0f, 1},
                     {0.019f, 0.019f, -1 + gx * 0.02f, -1 + gy * 0.02f},
                     ((i * 37) % 100) / 100.0f};
      push(&p);
      vkCmdDraw(cb, 6, 1, 0, 0);
   }
   vkCmdEndRenderPass(cb);
}

static void rec_multipass(int frame)
{
   for (int i = 0; i < 40 * scale; i++) {
      int dst = i & 1;
      VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = rp_load, .framebuffer = fb_small[dst],
                                  .renderArea = {{0, 0}, {SW, SH}}};
      vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_tex);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1,
                              &ds_small[dst ^ 1], 0, NULL);
      struct pc p0 = {{1, 0.5f, (frame & 15) / 16.0f, 1}, {2, 2, -1, -1}, 0.5f};
      push(&p0);
      vkCmdDraw(cb, 6, 1, 0, 0);
      struct pc p1 = {{0, 1, 0, 1}, {0.5f, 0.5f, -0.9f + (i % 5) * 0.2f, -0.5f}, 0.5f};
      push(&p1);
      vkCmdDraw(cb, 6, 1, 0, 0);
      struct pc p2 = {{0, 0, 1, 1}, {0.3f, 0.8f, 0.2f, -0.9f + (i % 7) * 0.1f}, 0.5f};
      push(&p2);
      vkCmdDraw(cb, 6, 1, 0, 0);
      vkCmdEndRenderPass(cb);
   }
}

static void rec_subpass(int frame)
{
   VkClearValue cv[4] = {{.color = {.float32 = {0, 0, 0, 1}}},
                         {.color = {.float32 = {0, 0, 0, 0}}},
                         {.color = {.float32 = {0, 0, 1, 0}}},
                         {.depthStencil = {1.0f, 0}}};
   VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                               .renderPass = rp_deferred, .framebuffer = fb_deferred,
                               .renderArea = {{0, 0}, {W, H}},
                               .clearValueCount = 4, .pClearValues = cv};
   vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_gbuf);
   for (int i = 0; i < 12 * scale; i++) {
      /* overlapping quads at varying depth, most of them covering the screen */
      float s = 1.2f + 0.8f * ((i * 7) % 5) / 4.0f;
      struct pc p = {{(i & 1) * 0.8f + 0.2f, ((i >> 1) & 1) * 0.8f + 0.2f,
                      ((i + frame) & 3) * 0.25f, 1},
                     {s, s, -1 + 0.1f * (i % 3), -1 + 0.07f * (i % 4)},
                     ((i * 37) % 100) / 100.0f};
      push(&p);
      vkCmdDraw(cb, 6, 1, 0, 0);
   }
   vkCmdNextSubpass(cb, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_light);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_in, 0, 1, &ds_gbuf, 0,
                           NULL);
   struct pc pl = {{(frame & 7) / 8.0f, 0.5f, 0.5f, 1}, {2, 2, -1, -1}, 0.5f};
   vkCmdPushConstants(cb, layout_in, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                      0, sizeof(pl), &pl);
   vkCmdDraw(cb, 6, 1, 0, 0);
   vkCmdEndRenderPass(cb);
}

static void rec_resolve(int frame)
{
   VkClearValue cv[2] = {{.color = {.float32 = {0.05f, 0.05f, 0.1f, 1}}},
                         {.color = {.float32 = {0, 0, 0, 1}}}};
   VkRenderPassBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                               .renderPass = rp_msaa, .framebuffer = fb_msaa,
                               .renderArea = {{0, 0}, {W, H}},
                               .clearValueCount = 2, .pClearValues = cv};
   vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p_msaa_fill);
   for (int i = 0; i < 8 * scale; i++) {
      /* slightly smaller than the screen and offset, so edges get multisampled coverage */
      struct pc p = {{(i & 1) * 1.0f, ((i >> 1) & 1) * 1.0f, ((i + frame) & 3) * 0.33f, 0.3f},
                     {1.9f, 1.9f, -0.97f + 0.01f * (i % 5), -0.96f + 0.01f * (i % 3)}, 0.5f};
      push(&p);
      vkCmdDraw(cb, 6, 1, 0, 0);
   }
   vkCmdEndRenderPass(cb);
}

/* Current GPU clock in MHz from KGSL sysfs (readable by the shell user), 0 if unavailable.
 * The governor moves between 255 and 950 MHz on the G2, so frame times are only
 * comparable between runs that sat at a similar clock. */
static double gpu_mhz(void)
{
   static int fd = -2;
   char buf[32];
   if (fd == -2)
      fd = open("/sys/class/kgsl/kgsl-3d0/gpuclk", O_RDONLY | O_CLOEXEC);
   if (fd < 0)
      return 0;
   ssize_t r = pread(fd, buf, sizeof(buf) - 1, 0);
   if (r <= 0)
      return 0;
   buf[r] = 0;
   return atof(buf) / 1e6;
}

static void run(const char *name, void (*rec)(int), int frames)
{
   int warm = frames / 4;
   double *cpu = calloc(frames, sizeof(double)), *tot = calloc(frames, sizeof(double));
   double *all = calloc(frames, sizeof(double)), *clk = calloc(frames, sizeof(double));
   double clk_sum = 0;
   VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
   VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                      .commandBufferCount = 1, .pCommandBuffers = &cb};
   int n = 0;
   for (int f = 0; f < frames; f++) {
      double t0 = now_ms();
      CHECK(vkResetCommandBuffer(cb, 0));
      CHECK(vkBeginCommandBuffer(cb, &cbi));
      rec(f);
      CHECK(vkEndCommandBuffer(cb));
      double t1 = now_ms();
      CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(queue));
      double t2 = now_ms();
      all[f] = t2 - t0;
      clk[f] = gpu_mhz();
      if (f >= warm) {
         cpu[n] = t1 - t0;
         tot[n] = t2 - t0;
         clk_sum += clk[f];
         n++;
      }
   }
   double med_cpu = median(cpu, n), med_tot = median(tot, n); /* median() sorts in place */
   printf("BENCH %-10s cpu_ms=%.3f frame_ms=%.3f min_ms=%.3f gpu_mhz=%.0f\n", name, med_cpu,
          med_tot, tot[0], clk_sum / n);
   if (getenv("TU_BENCH_SERIES")) {
      /* median frame time and mean GPU clock for each tenth of the run, warm-up included */
      enum { CH = 10 };
      int per = frames / CH;
      printf("SERIES %-9s", name);
      for (int c = 0; c < CH && per > 0; c++) {
         double m = 0;
         for (int i = 0; i < per; i++) m += clk[c * per + i];
         printf(" %.1f@%.0f", median(all + c * per, per), m / per);
      }
      printf("\n");
   }
   fflush(stdout);
   free(cpu);
   free(tot);
   free(all);
   free(clk);
}

int main(int argc, char **argv)
{
   if (argc < 2) {
      printf("usage: %s driver.so [frames] [scenario]\n", argv[0]);
      return 1;
   }
   int frames = argc > 2 ? atoi(argv[2]) : 80;
   const char *only = argc > 3 && strcmp(argv[3], "all") ? argv[3] : NULL;
   if (argc > 4) scale = atoi(argv[4]);
   if (scale < 1) scale = 1;
   if (frames < 8) frames = 8;

   void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!h) {
      printf("FAIL dlopen: %s\n", dlerror());
      return 2;
   }
   PFN_vkCreateInstance create_instance = NULL;
   gipa = (gipa_t)dlsym(h, "vk_icdGetInstanceProcAddr");
   if (gipa) {
      create_instance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   } else {
      struct hw_module_t *hmi = dlsym(h, "HMI");
      struct hw_device_t *hd = NULL;
      if (!hmi || hmi->methods->open(hmi, "vk0", &hd) || !hd) {
         printf("FAIL no usable driver entrypoint\n");
         return 2;
      }
      struct hwvulkan_device_t *hv = (struct hwvulkan_device_t *)hd;
      gipa = (gipa_t)hv->GetInstanceProcAddr;
      create_instance = hv->CreateInstance;
   }
   VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                            .pApplicationName = "tu_bench",
                            .apiVersion = VK_API_VERSION_1_1};
   VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                               .pApplicationInfo = &app};
   CHECK(create_instance(&ici, NULL, &inst));
   FUNCS(LOAD)

   uint32_t n = 1;
   VkPhysicalDevice pd;
   vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (!n) {
      printf("FAIL no physical device\n");
      return 3;
   }
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   printf("DRIVER %s (driverVersion 0x%x)\n", props.deviceName, props.driverVersion);
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);

   uint32_t nq = 8, qfi = 0;
   VkQueueFamilyProperties qf[8];
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
   for (uint32_t i = 0; i < nq; i++)
      if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
         qfi = i;
         break;
      }
   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                  .queueFamilyIndex = qfi, .queueCount = 1,
                                  .pQueuePriorities = &prio};
   VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                             .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci};
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, qfi, 0, &queue);

   VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                  .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                  .queueFamilyIndex = qfi};
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpi, NULL, &pool));
   VkCommandBufferAllocateInfo cai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                      .commandPool = pool,
                                      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                      .commandBufferCount = 1};
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));

   /* ---- targets ---- */
   const VkFormat CF = VK_FORMAT_R8G8B8A8_UNORM, DF = VK_FORMAT_D32_SFLOAT;
   struct target color = make_image(W, H, CF, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
   struct target depth = make_image(W, H, DF, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                    VK_IMAGE_ASPECT_DEPTH_BIT);
   struct target small[2];
   for (int i = 0; i < 2; i++)
      small[i] = make_image(SW, SH, CF, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                        VK_IMAGE_USAGE_SAMPLED_BIT |
                                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            VK_IMAGE_ASPECT_COLOR_BIT);

   /* ---- render passes ---- */
   {
      VkAttachmentDescription att[2] = {
         {.format = CF, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
         {.format = DF, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
      VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkAttachmentReference dr = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  .colorAttachmentCount = 1, .pColorAttachments = &cr,
                                  .pDepthStencilAttachment = &dr};
      VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 2, .pAttachments = att,
                                   .subpassCount = 1, .pSubpasses = &sub};
      CHECK(vkCreateRenderPass(dev, &ri, NULL, &rp_main));
      VkImageView views[2] = {color.view, depth.view};
      VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp_main, .attachmentCount = 2,
                                    .pAttachments = views, .width = W, .height = H, .layers = 1};
      CHECK(vkCreateFramebuffer(dev, &fi, NULL, &fb_main));
   }
   {
      VkAttachmentDescription att = {
         .format = CF, .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
         .finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkAttachmentReference cr = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription sub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  .colorAttachmentCount = 1, .pColorAttachments = &cr};
      VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 1, .pAttachments = &att,
                                   .subpassCount = 1, .pSubpasses = &sub};
      CHECK(vkCreateRenderPass(dev, &ri, NULL, &rp_load));
      for (int i = 0; i < 2; i++) {
         VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                       .renderPass = rp_load, .attachmentCount = 1,
                                       .pAttachments = &small[i].view,
                                       .width = SW, .height = SH, .layers = 1};
         CHECK(vkCreateFramebuffer(dev, &fi, NULL, &fb_small[i]));
      }
   }

   /* ---- descriptors ---- */
   VkSamplerCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                              .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
   VkSampler sampler;
   CHECK(vkCreateSampler(dev, &sci, NULL, &sampler));
   VkDescriptorSetLayoutBinding dlb = {.binding = 0,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT};
   VkDescriptorSetLayoutCreateInfo dli = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 1, .pBindings = &dlb};
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkDescriptorPoolSize dps = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
   VkDescriptorPoolCreateInfo dpi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                     .maxSets = 2, .poolSizeCount = 1, .pPoolSizes = &dps};
   VkDescriptorPool dpool;
   CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
   for (int i = 0; i < 2; i++) {
      VkDescriptorSetAllocateInfo dai = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl};
      CHECK(vkAllocateDescriptorSets(dev, &dai, &ds_small[i]));
      VkDescriptorImageInfo dii = {sampler, small[i].view,
                                   VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
      VkWriteDescriptorSet w = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = ds_small[i], .dstBinding = 0, .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = &dii};
      vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);
   }

   /* ---- pipelines ---- */
   VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                              sizeof(struct pc)};
   VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                     .setLayoutCount = 1, .pSetLayouts = &dsl,
                                     .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
   CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &layout));
   VkShaderModuleCreateInfo smi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
   VkShaderModule fs_simple, fs_heavy, fs_tex;
   smi.codeSize = sizeof(quad_vert); smi.pCode = quad_vert;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &vs_mod));
   smi.codeSize = sizeof(simple_frag); smi.pCode = simple_frag;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs_simple));
   smi.codeSize = sizeof(heavy_frag); smi.pCode = heavy_frag;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs_heavy));
   smi.codeSize = sizeof(tex_frag); smi.pCode = tex_frag;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs_tex));

   p_fill = make_pipeline(rp_main, fs_simple, 1, 0, W, H, 0);
   p_heavy = make_pipeline(rp_main, fs_heavy, 0, 0, W, H, 0);
   p_quad = make_pipeline(rp_main, fs_simple, 0, 1, W, H, 0);
   p_tex = make_pipeline(rp_load, fs_tex, 0, 0, SW, SH, 0);

   /* ---- subpass / resolve scenarios (only set up when asked for) ---- */
   int want_gmemcases = only && (!strcmp(only, "subpass") || !strcmp(only, "resolve") ||
                                 !strcmp(only, "gmemcases"));
   if (want_gmemcases) {
      /* Deferred: 0 = final colour (stored), 1 = albedo RGBA8, 2 = normal RGBA16F,
       * 3 = depth; 1-3 are transient (DONT_CARE store). */
      const VkFormat NF = VK_FORMAT_R16G16B16A16_SFLOAT;
      struct target fin = make_image(W, H, CF, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_SAMPLED_BIT,
                                     VK_IMAGE_ASPECT_COLOR_BIT);
      struct target g0 = make_image(W, H, CF, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
      struct target g1 = make_image(W, H, NF, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
      struct target gd = make_image(W, H, DF, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                    VK_IMAGE_ASPECT_DEPTH_BIT);
      VkAttachmentDescription att[4];
      const VkFormat fmts[4] = {CF, CF, NF, DF};
      for (int i = 0; i < 4; i++)
         att[i] = (VkAttachmentDescription){
            .format = fmts[i], .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = i == 0 ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = i == 3 ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
                                  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkAttachmentReference g_out[2] = {{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
                                        {2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
      VkAttachmentReference g_depth = {3, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      VkAttachmentReference g_in[2] = {{1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
      VkAttachmentReference fin_ref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subs[2] = {
         {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 2,
          .pColorAttachments = g_out, .pDepthStencilAttachment = &g_depth},
         {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .inputAttachmentCount = 2,
          .pInputAttachments = g_in, .colorAttachmentCount = 1, .pColorAttachments = &fin_ref}};
      VkSubpassDependency dep = {
         .srcSubpass = 0, .dstSubpass = 1,
         .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT,
         .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT};
      VkRenderPassCreateInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                   .attachmentCount = 4, .pAttachments = att,
                                   .subpassCount = 2, .pSubpasses = subs,
                                   .dependencyCount = 1, .pDependencies = &dep};
      CHECK(vkCreateRenderPass(dev, &ri, NULL, &rp_deferred));
      VkImageView views[4] = {fin.view, g0.view, g1.view, gd.view};
      VkFramebufferCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                    .renderPass = rp_deferred, .attachmentCount = 4,
                                    .pAttachments = views, .width = W, .height = H, .layers = 1};
      CHECK(vkCreateFramebuffer(dev, &fi, NULL, &fb_deferred));

      VkDescriptorSetLayoutBinding ib[2] = {
         {.binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT},
         {.binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
          .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT}};
      VkDescriptorSetLayoutCreateInfo ili = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
         .bindingCount = 2, .pBindings = ib};
      VkDescriptorSetLayout dsl_in;
      CHECK(vkCreateDescriptorSetLayout(dev, &ili, NULL, &dsl_in));
      VkDescriptorPoolSize ips = {VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 2};
      VkDescriptorPoolCreateInfo ipi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                        .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &ips};
      VkDescriptorPool ipool;
      CHECK(vkCreateDescriptorPool(dev, &ipi, NULL, &ipool));
      VkDescriptorSetAllocateInfo idai = {
         .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
         .descriptorPool = ipool, .descriptorSetCount = 1, .pSetLayouts = &dsl_in};
      CHECK(vkAllocateDescriptorSets(dev, &idai, &ds_gbuf));
      VkDescriptorImageInfo iii[2] = {
         {VK_NULL_HANDLE, g0.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
         {VK_NULL_HANDLE, g1.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
      VkWriteDescriptorSet iw = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                 .dstSet = ds_gbuf, .dstBinding = 0, .descriptorCount = 2,
                                 .descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT,
                                 .pImageInfo = iii};
      vkUpdateDescriptorSets(dev, 1, &iw, 0, NULL);
      VkPipelineLayoutCreateInfo ipli = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
         .setLayoutCount = 1, .pSetLayouts = &dsl_in,
         .pushConstantRangeCount = 1, .pPushConstantRanges = &pcr};
      CHECK(vkCreatePipelineLayout(dev, &ipli, NULL, &layout_in));

      VkShaderModule fs_gbuf, fs_light;
      smi.codeSize = sizeof(gbuf_frag); smi.pCode = gbuf_frag;
      CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs_gbuf));
      smi.codeSize = sizeof(light_frag); smi.pCode = light_frag;
      CHECK(vkCreateShaderModule(dev, &smi, NULL, &fs_light));
      p_gbuf = make_pipeline_ex(rp_deferred, fs_gbuf, 0, 1, W, H, 0,
                                (struct pipe_ex){.subpass = 0, .color_count = 2});
      p_light = make_pipeline_ex(rp_deferred, fs_light, 0, 0, W, H, 0,
                                 (struct pipe_ex){.layout = layout_in, .subpass = 1});

      /* MSAA: 0 = 4x colour (transient), 1 = resolve target (stored). */
      struct target ms = make_image_ms(W, H, CF, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
                                       VK_IMAGE_ASPECT_COLOR_BIT, VK_SAMPLE_COUNT_4_BIT);
      struct target rs = make_image(W, H, CF, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                                              VK_IMAGE_USAGE_SAMPLED_BIT,
                                    VK_IMAGE_ASPECT_COLOR_BIT);
      VkAttachmentDescription matt[2] = {
         {.format = CF, .samples = VK_SAMPLE_COUNT_4_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
         {.format = CF, .samples = VK_SAMPLE_COUNT_1_BIT,
          .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}};
      VkAttachmentReference mref = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkAttachmentReference rref = {1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription msub = {.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   .colorAttachmentCount = 1, .pColorAttachments = &mref,
                                   .pResolveAttachments = &rref};
      VkRenderPassCreateInfo mri = {.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                    .attachmentCount = 2, .pAttachments = matt,
                                    .subpassCount = 1, .pSubpasses = &msub};
      CHECK(vkCreateRenderPass(dev, &mri, NULL, &rp_msaa));
      VkImageView mviews[2] = {ms.view, rs.view};
      VkFramebufferCreateInfo mfi = {.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                     .renderPass = rp_msaa, .attachmentCount = 2,
                                     .pAttachments = mviews, .width = W, .height = H,
                                     .layers = 1};
      CHECK(vkCreateFramebuffer(dev, &mfi, NULL, &fb_msaa));
      p_msaa_fill = make_pipeline_ex(rp_msaa, fs_simple, 1, 0, W, H, 0,
                                     (struct pipe_ex){.samples = VK_SAMPLE_COUNT_4_BIT});
   }

   /* ---- put the small targets into a defined state/layout ---- */
   {
      VkCommandBufferBeginInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
      CHECK(vkBeginCommandBuffer(cb, &cbi));
      for (int i = 0; i < 2; i++) {
         VkImageSubresourceRange r = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
         VkImageMemoryBarrier b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                   .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                   .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                   .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                   .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                   .image = small[i].img, .subresourceRange = r};
         vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
         VkClearColorValue c = {.float32 = {0.2f * i, 0.3f, 0.4f, 1}};
         vkCmdClearColorImage(cb, small[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &r);
         b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
         b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
         b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
         b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
         vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
      }
      CHECK(vkEndCommandBuffer(cb));
      VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                         .commandBufferCount = 1, .pCommandBuffers = &cb};
      CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE));
      CHECK(vkQueueWaitIdle(queue));
   }

#define WANT(s) (!only || !strcmp(only, s))
   if (WANT("fill")) run("fill", rec_fill, frames);
   if (WANT("heavy_fs")) run("heavy_fs", rec_heavy, frames);
   /* Per-operation variants of the heavy fragment shader; only on request
    * ("fsmodes", or "fsmode<N>" for a single one), not part of "all". */
   if (only && !strncmp(only, "fsmode", 6)) {
      static const struct { int mode; const char *name; } modes[] = {
         {0, "fs_mix"}, {1, "fs_sin"}, {2, "fs_cos"}, {3, "fs_muladd"}, {4, "fs_sqrt"},
         {5, "fs_explog"}, {6, "fs_divmix"}, {7, "fs_atan"}, {100, "fs_dynloop"}};
      int single = only[6] && only[6] != 's' ? atoi(only + 6) : -1;
      VkPipeline saved = p_heavy;
      for (unsigned m = 0; m < sizeof(modes) / sizeof(modes[0]); m++) {
         if (single >= 0 && modes[m].mode != single)
            continue;
         spec_mode = modes[m].mode;
         p_heavy = make_pipeline(rp_main, fs_heavy, 0, 0, W, H, 0);
         run(modes[m].name, rec_heavy, frames);
         vkDestroyPipeline(dev, p_heavy, NULL);
      }
      spec_mode = 0;
      p_heavy = saved;
   }
   if (WANT("drawcalls")) run("drawcalls", rec_drawcalls, frames);
   if (WANT("multipass")) run("multipass", rec_multipass, frames);
   if (want_gmemcases && strcmp(only, "resolve")) run("subpass", rec_subpass, frames);
   if (want_gmemcases && strcmp(only, "subpass")) run("resolve", rec_resolve, frames);
   if (WANT("pipelines")) {
      enum { NP = 150 };
      double t0 = now_ms();
      for (int i = 0; i < NP; i++) {
         VkPipeline p = make_pipeline(rp_main, fs_heavy, i & 1, (i >> 1) & 1, W, H, 8 + i);
         vkDestroyPipeline(dev, p, NULL);
      }
      printf("BENCH %-10s per_pipeline_ms=%.3f\n", "pipelines", (now_ms() - t0) / NP);
   }
   fflush(stdout);
   _exit(0);
}
