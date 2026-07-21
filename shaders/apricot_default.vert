#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec2 in_uv;
layout(location = 3) in vec4 in_color;

layout(push_constant) uniform PC {
    mat4  mvp;
    vec4  color;
} pc;

layout(location = 0) out vec4 frag_color;

void main() {
    gl_Position = pc.mvp * vec4(in_position, 1.0);
    frag_color  = pc.color * in_color;
}
