#!/bin/bash
# Script para diagnosticar problemas de compatibilidade de CPU
# Uso: ./tools/diagnose-cpu-compatibility.sh [caminho-do-executável]

set -e

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

echo -e "${BLUE}=== Diagnóstico de Compatibilidade de CPU ===${NC}"
echo ""

# Encontrar executável
EXECUTABLE=""

if [ -n "$1" ]; then
    EXECUTABLE="$1"
    elif [ -f "build-linux-x86_64/bin/retrocapture" ]; then
    EXECUTABLE="build-linux-x86_64/bin/retrocapture"
    elif [ -f "./bin/retrocapture" ]; then
    EXECUTABLE="./bin/retrocapture"
    elif command -v retrocapture &> /dev/null; then
    EXECUTABLE="$(command -v retrocapture)"
else
    echo -e "${RED}❌ Executável não encontrado!${NC}"
    echo ""
    echo "Uso: $0 [caminho-do-executável]"
    exit 1
fi

if [ ! -f "$EXECUTABLE" ]; then
    echo -e "${RED}❌ Arquivo não encontrado: $EXECUTABLE${NC}"
    exit 1
fi

echo "📁 Executável: $EXECUTABLE"
echo ""

# Informações da CPU atual
echo -e "${BLUE}=== Informações da CPU Atual ===${NC}"
if [ -f /proc/cpuinfo ]; then
    CPU_MODEL=$(grep -m1 "model name" /proc/cpuinfo | cut -d: -f2 | sed 's/^[ \t]*//')
    CPU_FLAGS=$(grep -m1 "flags" /proc/cpuinfo | cut -d: -f2 | sed 's/^[ \t]*//')
    echo "   Modelo: $CPU_MODEL"
    echo ""
    echo "   Flags suportadas:"
    echo "$CPU_FLAGS" | tr ' ' '\n' | grep -E "(avx|sse|mmx|fma)" | sed 's/^/     • /' || echo "     (nenhuma flag relevante encontrada)"
else
    echo "   Não foi possível ler informações da CPU"
fi

echo ""

# Verificar instruções usadas no binário
echo -e "${BLUE}=== Instruções Detectadas no Binário ===${NC}"

if ! command -v objdump &> /dev/null; then
    echo -e "${YELLOW}⚠️  objdump não está disponível. Instale: binutils${NC}"
    echo ""
else
    # Função auxiliar para contar instruções de forma segura
    count_instructions() {
        local pattern="$1"
        local count=$(objdump -d "$EXECUTABLE" 2>/dev/null | grep -c "$pattern" 2>/dev/null || echo "0")
        # Garantir que é um número válido (remover quebras de linha e espaços)
        count=$(echo "$count" | tr -d '\n\r ' | head -1)
        # Se vazio ou não numérico, retornar 0
        if [ -z "$count" ] || ! echo "$count" | grep -qE '^[0-9]+$'; then
            echo "0"
        else
            echo "$count"
        fi
    }
    
    # Verificar instruções AVX2
    AVX2_COUNT=$(count_instructions "vpbroadcast\|vpmulld\|vpermd\|vpaddd\|vpsubd")
    if [ "$AVX2_COUNT" -gt 0 ] 2>/dev/null; then
        echo -e "${RED}   ❌ AVX2 detectado: $AVX2_COUNT instruções${NC}"
        echo "      O binário requer AVX2 (não suportado em CPUs antigas)"
    else
        echo -e "${GREEN}   ✅ AVX2 não detectado${NC}"
        AVX2_COUNT=0
    fi
    
    # Verificar instruções AVX-512
    AVX512_COUNT=$(count_instructions "vpbroadcast.*zmm\|vpmulld.*zmm")
    if [ "$AVX512_COUNT" -gt 0 ] 2>/dev/null; then
        echo -e "${RED}   ❌ AVX-512 detectado: $AVX512_COUNT instruções${NC}"
        echo "      O binário requer AVX-512 (apenas CPUs muito recentes)"
    else
        echo -e "${GREEN}   ✅ AVX-512 não detectado${NC}"
        AVX512_COUNT=0
    fi
    
    # Verificar instruções AVX (básico)
    AVX_COUNT=$(count_instructions "vaddps\|vmulps\|vsubps")
    if [ "$AVX_COUNT" -gt 0 ] 2>/dev/null; then
        echo -e "${YELLOW}   ⚠️  AVX detectado: $AVX_COUNT instruções${NC}"
        echo "      Requer CPU com suporte AVX (Sandy Bridge/AMD Bulldozer ou mais novo)"
    else
        AVX_COUNT=0
    fi
    
    # Verificar SSE4.2
    SSE42_COUNT=$(count_instructions "pcmpgtq\|crc32")
    if [ "$SSE42_COUNT" -gt 0 ] 2>/dev/null; then
        echo -e "${GREEN}   ✅ SSE4.2 detectado: $SSE42_COUNT instruções${NC}"
        echo "      Compatível com CPUs desde Core 2 (2006)"
    else
        SSE42_COUNT=0
    fi
fi

echo ""

# Verificar se a CPU suporta as instruções necessárias
echo -e "${BLUE}=== Compatibilidade ===${NC}"

if [ -f /proc/cpuinfo ]; then
    # Verificar suporte AVX2
    if echo "$CPU_FLAGS" | grep -q "avx2"; then
        echo -e "${GREEN}   ✅ CPU suporta AVX2${NC}"
    else
        echo -e "${RED}   ❌ CPU NÃO suporta AVX2${NC}"
        if [ "$AVX2_COUNT" -gt 0 ]; then
            echo -e "${RED}      ⚠️  PROBLEMA: Binário requer AVX2 mas CPU não suporta!${NC}"
        fi
    fi
    
    # Verificar suporte AVX
    if echo "$CPU_FLAGS" | grep -q " avx "; then
        echo -e "${GREEN}   ✅ CPU suporta AVX${NC}"
    else
        echo -e "${YELLOW}   ⚠️  CPU não suporta AVX (muito antiga)${NC}"
    fi
    
    # Verificar suporte SSE4.2
    if echo "$CPU_FLAGS" | grep -q "sse4_2"; then
        echo -e "${GREEN}   ✅ CPU suporta SSE4.2${NC}"
    else
        echo -e "${RED}   ❌ CPU não suporta SSE4.2 (muito antiga, pré-2006)${NC}"
    fi
fi

echo ""

# Soluções
echo -e "${BLUE}=== Soluções ===${NC}"
echo ""

if [ "$AVX2_COUNT" -gt 0 ] && [ -f /proc/cpuinfo ] && ! echo "$CPU_FLAGS" | grep -q "avx2"; then
    echo -e "${YELLOW}🔧 SOLUÇÃO: Recompilar com flags compatíveis${NC}"
    echo ""
    echo "O binário foi compilado com -march=native em uma CPU mais nova."
    echo "Para compatibilidade com CPUs mais antigas, recompile com:"
    echo ""
    echo "   mkdir -p build-linux-x86_64-compatible"
    echo "   cd build-linux-x86_64-compatible"
    echo "   cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_COMPATIBLE_X86_64=ON"
    echo "   cmake --build . -j\$(nproc)"
    echo ""
    echo "Ou compile diretamente na máquina antiga (recomendado)."
else
    echo -e "${GREEN}✅ Binário parece compatível com a CPU atual${NC}"
    echo ""
    echo "Se ainda houver problemas, tente:"
    echo "   1. Verificar dependências: ./tools/check-dependencies.sh"
    echo "   2. Recompilar na máquina atual"
    echo "   3. Verificar logs de erro para mais detalhes"
fi

echo ""
echo -e "${BLUE}=== Informações Adicionais ===${NC}"
echo ""
echo "Para ver todas as instruções do binário:"
echo "   objdump -d $EXECUTABLE | grep -E '(vpbroadcast|vpmulld|vpermd)'"
echo ""
echo "Para verificar arquitetura do binário:"
echo "   file $EXECUTABLE"
echo "   readelf -h $EXECUTABLE | grep Machine"
