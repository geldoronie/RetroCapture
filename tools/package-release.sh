#!/bin/bash
# Orquestrador de release: roda todas as builds e empacota artefatos em dist/.
#
# Por padrão constrói tudo (Linux x86_64 AppImage, Linux ARM64, Linux ARM32, Windows).
# Use as flags --skip-* pra pular plataformas específicas. Cada build é independente,
# então uma falha não interrompe as outras (cada plataforma é reportada no resumo final).
#
# Saída em $REPO_ROOT/dist/:
#   RetroCapture-<version>-alpha-linux-x86_64.AppImage
#   RetroCapture-<version>-alpha-linux-arm64v8.tar.gz
#   RetroCapture-<version>-alpha-linux-arm32v7.tar.gz
#   RetroCapture-<version>-alpha-windows-x86_64-Setup.exe
#   SHA256SUMS

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
cd "$REPO_ROOT"

if [ ! -f "CMakeLists.txt" ]; then
    echo "❌ CMakeLists.txt não encontrado em $REPO_ROOT"
    exit 1
fi

VERSION=$(grep -E "^project\(RetroCapture VERSION" CMakeLists.txt | sed -E 's/.*VERSION ([0-9.]+[^ ]*).*/\1/')
RELEASE_VERSION="${VERSION}-alpha"

# Flags
SKIP_X86=0
SKIP_ARM64=0
SKIP_ARM32=0
SKIP_WIN=0
BUILD_TYPE="Release"

for arg in "$@"; do
    case "$arg" in
        --skip-x86|--skip-appimage) SKIP_X86=1 ;;
        --skip-arm64) SKIP_ARM64=1 ;;
        --skip-arm32) SKIP_ARM32=1 ;;
        --skip-win|--skip-windows) SKIP_WIN=1 ;;
        --only-x86)  SKIP_ARM64=1; SKIP_ARM32=1; SKIP_WIN=1 ;;
        --only-arm)  SKIP_X86=1;   SKIP_WIN=1 ;;
        --only-arm64) SKIP_X86=1;  SKIP_ARM32=1; SKIP_WIN=1 ;;
        --only-arm32) SKIP_X86=1;  SKIP_ARM64=1; SKIP_WIN=1 ;;
        --only-win)  SKIP_X86=1;   SKIP_ARM64=1; SKIP_ARM32=1 ;;
        Release|Debug) BUILD_TYPE="$arg" ;;
        --help|-h)
            cat <<EOF
Uso: $0 [Release|Debug] [flags]

Por padrão constrói TUDO. Flags pra escolher subconjunto:
  --skip-x86       Pula AppImage Linux x86_64
  --skip-arm64     Pula tarball Linux ARM64
  --skip-arm32     Pula tarball Linux ARM32
  --skip-win       Pula instalador Windows
  --only-x86       Só AppImage
  --only-arm       Só tarballs ARM (64+32)
  --only-arm64     Só tarball ARM64
  --only-arm32     Só tarball ARM32
  --only-win       Só Windows

Saída: \$REPO_ROOT/dist/ com SHA256SUMS no final.
EOF
            exit 0
            ;;
        *)
            echo "⚠️  Argumento desconhecido: $arg (ignorado)"
            ;;
    esac
done

mkdir -p dist

echo "🎬 RetroCapture release $RELEASE_VERSION"
echo "   Build type: $BUILD_TYPE"
echo "   Saída: $REPO_ROOT/dist/"
echo ""

declare -a RESULTS

run_build() {
    local label="$1"
    local script="$2"
    shift 2
    echo "──────────────────────────────────────────"
    echo "▶ $label"
    echo "──────────────────────────────────────────"
    if bash "$script" "$@"; then
        RESULTS+=("✅ $label")
    else
        RESULTS+=("❌ $label (FAILED — verifique log)")
    fi
    echo ""
}

if [ "$SKIP_X86" -eq 0 ]; then
    run_build "Linux x86_64 (AppImage)" "tools/build-linux-appimage-x86_64.sh" "$BUILD_TYPE"
fi

if [ "$SKIP_ARM64" -eq 0 ]; then
    run_build "Linux ARM64 (tarball)" "tools/build-linux-arm64v8-docker.sh" "$BUILD_TYPE"
fi

if [ "$SKIP_ARM32" -eq 0 ]; then
    run_build "Linux ARM32v7 (tarball)" "tools/build-linux-arm32v7-docker.sh" "$BUILD_TYPE"
fi

if [ "$SKIP_WIN" -eq 0 ]; then
    run_build "Windows x86_64 (NSIS installer)" "tools/build-windows-installer.sh" "$BUILD_TYPE"
fi

echo "══════════════════════════════════════════"
echo "📊 Resumo"
echo "══════════════════════════════════════════"
for r in "${RESULTS[@]}"; do
    echo "$r"
done
echo ""

# Gerar SHA256SUMS dos artefatos finais (não dos staging dirs)
echo "🔐 Gerando SHA256SUMS..."
if compgen -G "dist/RetroCapture-*" > /dev/null; then
    (cd dist && sha256sum RetroCapture-* > SHA256SUMS 2>/dev/null && \
        echo "   ✅ dist/SHA256SUMS" && \
        cat SHA256SUMS)
else
    echo "   ⚠️  Nenhum artefato em dist/ — pulando SHA256SUMS"
fi

echo ""
echo "📁 Artefatos finais:"
ls -lh dist/ 2>/dev/null | grep -v "^total\|^d" || echo "   (vazio)"
