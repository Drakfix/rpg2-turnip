#version 450
/* G-buffer pass for tu_bench "subpass": writes two colour targets. */
layout(push_constant) uniform PC { vec4 tint; vec4 xform; float z; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 albedo;
layout(location = 1) out vec4 normal;
void main()
{
   albedo = vec4(pc.tint.rgb * (0.5 + 0.5 * uv.x), 1.0);
   normal = vec4(normalize(vec3(uv * 2.0 - 1.0, 1.0)), pc.z);
}
