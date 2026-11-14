#version 450
layout(push_constant) uniform PC { vec4 tint; vec4 xform; float z; } pc;
layout(location = 0) out vec2 uv;
void main()
{
   int i = gl_VertexIndex;
   /* two triangles: (0,0)(1,0)(1,1) and (0,0)(1,1)(0,1) */
   vec2 q = vec2((i == 1 || i == 2 || i == 4) ? 1.0 : 0.0,
                 (i == 2 || i == 4 || i == 5) ? 1.0 : 0.0);
   uv = q;
   gl_Position = vec4(pc.xform.zw + q * pc.xform.xy, pc.z, 1.0);
}
