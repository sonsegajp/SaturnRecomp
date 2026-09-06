#version 450
layout(location=0) in vec2 position;
layout(location=1) in vec2 uv;
layout(location=2) in vec4 color;
layout(push_constant) uniform Transform { vec2 scale; } transform;
layout(location=0) out vec2 fragment_uv;
layout(location=1) out vec4 fragment_color;
void main() {
    gl_Position=vec4(position*transform.scale-vec2(1.0),0.0,1.0);
    fragment_uv=uv;
    fragment_color=color;
}
