#version 450
layout(set=0,binding=0) uniform sampler2D font_atlas;
layout(location=0) in vec2 fragment_uv;
layout(location=1) in vec4 fragment_color;
layout(location=0) out vec4 output_color;
void main() {
    output_color=fragment_color*texture(font_atlas,fragment_uv);
}
