#version 330

uniform sampler2D Texture;
uniform vec2 TextureSize;
uniform vec2 InputSize;
uniform vec2 OutputSize;

in vec2 TexCoord;
out vec4 FragColor;

void main() {
    // Shader de teste simples - apenas passa a textura através
    // Você pode substituir por qualquer shader do RetroArch
    vec2 coord = TexCoord;
    FragColor = texture(Texture, coord);
}

