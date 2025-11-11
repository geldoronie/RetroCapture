#version 330 core

#if defined(VERTEX)
in vec4 Position;
in vec2 TexCoord;
out vec2 vTexCoord;

void main() {
    gl_Position = Position;
    vTexCoord = TexCoord;
}

#elif defined(FRAGMENT)
in vec2 vTexCoord;
out vec4 FragColor;
uniform sampler2D Source;

void main() {
    // Tenta várias variações para ver qual funciona
    vec4 color1 = texture(Source, vTexCoord);
    vec4 color2 = texture(Source, vec2(vTexCoord.x, 1.0 - vTexCoord.y));
    
    // Se ambas são pretas, mostrar vermelho para indicar erro
    if (length(color1.rgb) < 0.01 && length(color2.rgb) < 0.01) {
        FragColor = vec4(1.0, 0.0, 0.0, 1.0);  // Vermelho = erro
    } else if (length(color1.rgb) > length(color2.rgb)) {
        FragColor = color1;
    } else {
        FragColor = color2;
    }
}

#endif

