#version 450
layout(push_constant) uniform PC { vec4 tint; vec4 xform; float z; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;
void main()
{
   o = pc.tint;
}
