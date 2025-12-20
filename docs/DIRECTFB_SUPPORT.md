# Suporte DirectFB / Framebuffer

## Situação Atual

O RetroCapture atualmente usa **GLFW** para gerenciamento de janelas, que **não suporta DirectFB** ou acesso direto ao framebuffer Linux. GLFW suporta apenas:

- X11 (Linux desktop)
- Wayland (Linux moderno)
- Cocoa (macOS)
- Win32 (Windows)

## Alternativas para DirectFB/Framebuffer

### Opção 1: SDL2 com Backend DirectFB (Recomendado)

**SDL2** pode ser compilado com suporte a DirectFB e oferece uma API similar ao GLFW.

**Vantagens:**

- API similar ao GLFW (fácil migração)
- Suporte nativo a DirectFB
- Suporte a framebuffer Linux direto
- Suporte a OpenGL ES (importante para Raspberry Pi)
- Bem mantido e amplamente usado

**Desvantagens:**

- Requer refatoração do código de WindowManager
- Dependência adicional (SDL2)

**Implementação:**

1. Adicionar SDL2 como dependência opcional
2. Criar `WindowManagerSDL` como alternativa ao `WindowManagerGLFW`
3. Usar SDL2 quando compilado com `-DBUILD_WITH_SDL2=ON`
4. SDL2 pode ser configurado para usar DirectFB ou framebuffer

### Opção 2: EGL + Framebuffer Direto

Acessar o framebuffer diretamente via EGL (sem window manager).

**Vantagens:**

- Controle total sobre o framebuffer
- Sem dependências de window manager
- Leve e eficiente

**Desvantagens:**

- Requer implementação manual de input handling
- Mais complexo de implementar
- Sem suporte a múltiplas janelas

**Implementação:**

1. Usar EGL para criar contexto OpenGL ES
2. Acessar `/dev/fb0` diretamente
3. Implementar input via `/dev/input/event*`

### Opção 3: Wayland (Se disponível)

Se a Raspberry Pi tiver suporte a Wayland, o GLFW pode funcionar.

**Verificar:**

```bash
echo $XDG_SESSION_TYPE
# Se retornar "wayland", pode funcionar
```

### Opção 4: X11 Minimal (Xvfb ou Xephyr)

Usar X11 mesmo que seja virtual.

**Já implementado:**

```bash
xvfb-run -a ./retrocapture
```

## Recomendação: SDL2 com DirectFB

Para suporte real a DirectFB/framebuffer, recomendo usar **SDL2**:

### Passos para Implementação

1. **Adicionar SDL2 ao CMakeLists.txt:**

```cmake
option(BUILD_WITH_SDL2 "Use SDL2 instead of GLFW" OFF)

if(BUILD_WITH_SDL2)
    find_package(SDL2 REQUIRED)
    # Configurar SDL2 para DirectFB
    set(SDL2_VIDEO_DRIVER directfb)
endif()
```

2. **Criar WindowManagerSDL:**

- Similar ao WindowManager atual
- Usar SDL2 em vez de GLFW
- Suporte a DirectFB e framebuffer

3. **Compilar com SDL2:**

```bash
cmake .. -DBUILD_WITH_SDL2=ON -DSDL2_VIDEO_DRIVER=directfb
```

### Exemplo de Uso SDL2 com DirectFB

```cpp
// Inicializar SDL2 com DirectFB
SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "directfb");
SDL_Init(SDL_INIT_VIDEO);

// Criar janela
SDL_Window* window = SDL_CreateWindow(
    "RetroCapture",
    SDL_WINDOWPOS_UNDEFINED,
    SDL_WINDOWPOS_UNDEFINED,
    width, height,
    SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN
);

// Criar contexto OpenGL ES
SDL_GLContext context = SDL_GL_CreateContext(window);
```

## Comparação

| Solução           | Complexidade | Performance | Compatibilidade |
| ----------------- | ------------ | ----------- | --------------- |
| SDL2 + DirectFB   | Média        | Alta        | Excelente       |
| EGL + Framebuffer | Alta         | Muito Alta  | Boa             |
| Xvfb              | Baixa        | Média       | Excelente       |
| Wayland           | Baixa        | Alta        | Limitada        |

## Conclusão

Para suporte real a DirectFB, a melhor opção é **SDL2 com backend DirectFB**. Isso requer:

- Refatoração do WindowManager
- Adicionar SDL2 como dependência opcional
- Compilar SDL2 com suporte a DirectFB

**Alternativa rápida:** Continuar usando Xvfb para modo headless, que já funciona.

Quer que eu implemente o suporte a SDL2 com DirectFB?
