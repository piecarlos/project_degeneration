// Scale2x (EPX) fragment shader for EKA2L1 upscale pipeline
// Matches LegacyMobileForge PixelArtFilter.java behavior:
// Edge-aware 2x pixel enlargement on game framebuffer only (before bezel),
// with zero blur and zero invented colors.
// filter="nearest" output_width="200%" output_height="200%"

#ifdef GL_ES
precision highp float;
precision mediump int;
#endif

uniform sampler2D u_tex;
uniform vec2 u_texelDelta;

in vec2 r_texcoord;
out vec4 o_color;

bool same_texel(vec3 a, vec3 b) {
    return all(lessThan(abs(a - b), vec3(0.001953125)));
}

void main() {
    vec2 pos = r_texcoord / u_texelDelta;
    vec2 center = (floor(pos) + vec2(0.5)) * u_texelDelta;
    vec2 fp = fract(pos);

    vec3 E = texture(u_tex, center).rgb;
    vec3 B = texture(u_tex, center + vec2(0.0, -u_texelDelta.y)).rgb;
    vec3 H = texture(u_tex, center + vec2(0.0,  u_texelDelta.y)).rgb;
    vec3 D = texture(u_tex, center + vec2(-u_texelDelta.x, 0.0)).rgb;
    vec3 F = texture(u_tex, center + vec2( u_texelDelta.x, 0.0)).rgb;

    vec3 out_rgb = E;
    if (!same_texel(B, H) && !same_texel(D, F)) {
        if (fp.y < 0.5) {
            if (fp.x < 0.5) {
                if (same_texel(D, B)) out_rgb = D;
            } else {
                if (same_texel(B, F)) out_rgb = F;
            }
        } else {
            if (fp.x < 0.5) {
                if (same_texel(D, H)) out_rgb = D;
            } else {
                if (same_texel(H, F)) out_rgb = F;
            }
        }
    }

    o_color = vec4(out_rgb, 1.0);
}
