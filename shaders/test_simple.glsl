#version 330 core

in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D Source;

void main() {
    // Shader de teste - apenas passar a cor
    vec4 color = texture(Source, vTexCoord);
    
    // Se a cor for preta, mostrar vermelho
    if (length(color.rgb) < 0.01) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);
    } else {
        FragColor = color;
    }
}

