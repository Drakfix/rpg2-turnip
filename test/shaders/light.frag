#version 450
/* Lighting pass for tu_bench "subpass": reads the G-buffer as input attachments. */
layout(input_attachment_index = 0, set = 0, binding = 0) uniform subpassInput g_albedo;
layout(input_attachment_index = 1, set = 0, binding = 1) uniform subpassInput g_normal;
layout(push_constant) uniform PC { vec4 tint; vec4 xform; float z; } pc;
layout(location = 0) out vec4 o;
void main()
{
   vec4 a = subpassLoad(g_albedo);
   vec3 n = subpassLoad(g_normal).xyz;
   float l = max(dot(n, normalize(pc.tint.xyz + vec3(0.2, 0.3, 1.0))), 0.0);
   o = vec4(a.rgb * (0.2 + 0.8 * l), 1.0);
}
