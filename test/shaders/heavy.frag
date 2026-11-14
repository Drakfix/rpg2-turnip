#version 450
/* ITER and MODE are specialization constants, so each (ITER, MODE) pair is a
 * different shader after specialization and dead branches disappear. */
layout(constant_id = 0) const int ITER = 24;
layout(constant_id = 1) const int MODE = 0;
layout(push_constant) uniform PC { vec4 tint; vec4 xform; float z; float dyn_iter; } pc;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 o;

vec2 step_fn(vec2 p)
{
   switch (MODE) {
   default:
   case 0: /* original mix: 2 sin + 2 cos */
      return vec2(sin(p.x * 1.3 + p.y) + cos(p.y * 0.7), cos(p.x - p.y * 1.1) + sin(p.x * 0.9));
   case 1: /* sin only (4 per iteration) */
      return vec2(sin(p.x * 1.3 + p.y) + sin(p.y * 0.7), sin(p.x - p.y * 1.1) + sin(p.x * 0.9));
   case 2: /* cos only (4 per iteration) */
      return vec2(cos(p.x * 1.3 + p.y) + cos(p.y * 0.7), cos(p.x - p.y * 1.1) + cos(p.x * 0.9));
   case 3: /* plain mul/add/fract, no transcendental */
      return vec2(fract(p.x * 1.3 + p.y) + fract(p.y * 0.7), fract(p.x - p.y * 1.1) + fract(p.x * 0.9)) * 1.7 - 1.3;
   case 4: /* sqrt / inversesqrt / normalize / length */
      return normalize(p + vec2(0.37, 1.21)) * (1.0 + sqrt(abs(p.x) + 0.5)) + inversesqrt(abs(p.yx) + 1.0);
   case 5: /* exp / log / pow */
      return vec2(exp(-abs(p.x) * 0.7) + log(abs(p.y) + 1.5), pow(abs(p.x) + 0.5, 1.3) - exp2(-abs(p.y)));
   case 6: /* division / mix / clamp / min / max */
      return mix(clamp(p * 1.3, -2.0, 2.0), 1.0 / (abs(p.yx) + 0.75), 0.4) + max(p, -p.yx) * 0.1 - min(p, vec2(0.3));
   case 7: /* tan / atan */
      return vec2(atan(p.y, p.x + 2.5), tan(clamp(p.x * 0.3, -1.0, 1.0)) + atan(p.y));
   }
}

void main()
{
   vec2 p = uv * 3.0;
   float a = 0.0;
   /* MODE >= 100: same step functions, but the trip count comes from a push
    * constant so the compiler cannot unroll the loop. */
   if (MODE >= 100) {
      int n = int(pc.dyn_iter);
      for (int i = 0; i < n; i++) {
         vec2 q = p;
         p = vec2(sin(q.x * 1.3 + q.y) + cos(q.y * 0.7), cos(q.x - q.y * 1.1) + sin(q.x * 0.9));
         a += dot(p, p);
      }
   } else {
      for (int i = 0; i < ITER; i++) {
         p = step_fn(p);
         a += dot(p, p);
      }
   }
   o = vec4(fract(a * 0.1), p * 0.25 + 0.5, 1.0) * pc.tint;
}
