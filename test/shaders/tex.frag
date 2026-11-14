#version 450
layout(set = 0, binding = 0) uniform sampler2D s;
layout(push_constant) uniform PC { vec4 tint; vec4 xform; float z; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
   o = texture(s, uv) * 0.98 + pc.tint * 0.02;
}
