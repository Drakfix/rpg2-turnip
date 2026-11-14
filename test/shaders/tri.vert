#version 450
layout(location = 0) out vec3 c;
void main()
{
   vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
   /* The triangle spans clip -1..3, so p is 0..1 across the visible screen. */
   c = vec3(p, 0.25);
   gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
