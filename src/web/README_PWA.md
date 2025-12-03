# PWA - Progressive Web App

O RetroCapture agora suporta instalação como PWA (Progressive Web App) no celular!

## Como usar

1. Acesse o portal web no seu celular: `https://192.168.0.10:8080` (ou o IP do seu servidor)
2. O navegador mostrará uma opção para "Adicionar à tela inicial" ou "Instalar app"
3. Após instalar, o app aparecerá como um aplicativo nativo no seu celular

## Ícones

Os ícones PWA precisam ser gerados. Você pode:

1. **Usar um gerador online**: https://www.pwabuilder.com/imageGenerator
2. **Usar ImageMagick** (se instalado):

   ```bash
   # Criar um ícone base (substitua icon.png pelo seu ícone)
   for size in 72 96 128 144 152 192 384 512; do
       convert icon.png -resize ${size}x${size} icon-${size}x${size}.png
   done
   ```

3. **Usar um script Python** (se tiver Pillow instalado):
   ```python
   from PIL import Image
   sizes = [72, 96, 128, 144, 152, 192, 384, 512]
   base_icon = Image.open('icon.png')  # Seu ícone base (512x512 recomendado)
   for size in sizes:
       icon = base_icon.resize((size, size), Image.Resampling.LANCZOS)
       icon.save(f'icon-{size}x{size}.png')
   ```

## Arquivos PWA

- `manifest.json` - Configuração do PWA
- `service-worker.js` - Service Worker para cache e funcionalidade offline
- `icon-*.png` - Ícones em diferentes tamanhos (precisam ser criados)

## Funcionalidades

- ✅ Instalação no celular
- ✅ Cache de recursos para funcionamento offline
- ✅ Atualização automática do Service Worker
- ✅ Interface responsiva para mobile
- ✅ Suporte a HTTPS (necessário para PWA)

## Notas

- O PWA requer HTTPS para funcionar (já implementado)
- Os ícones são opcionais, mas recomendados para melhor experiência
- O Service Worker cacheia recursos estáticos e APIs (com fallback offline)
