#!/bin/bash

# Script para sincronizar automaticamente o código fonte com a Raspberry Pi
# Monitora mudanças no diretório local e sincroniza automaticamente via rsync/SSH
#
# Uso:
#   ./sync-source-raspiberry.sh          # Sincronização contínua (monitora mudanças)
#   ./sync-source-raspiberry.sh --once   # Sincronização única (sem monitoramento)

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Configurações
SOURCE_DIR="/home/deck/Projects/RetroCapture"
REMOTE_HOST="retrocapture@192.168.5.193"
REMOTE_DIR="/home/retrocapture/Projects/RetroCapture"
SSH_PORT=22

# Verificar se inotifywait está instalado
if ! command -v inotifywait &> /dev/null; then
    echo -e "${RED}Erro: inotifywait não está instalado${NC}"
    echo "Instale com: sudo apt-get install inotify-tools"
    exit 1
fi

# Verificar se rsync está instalado
if ! command -v rsync &> /dev/null; then
    echo -e "${RED}Erro: rsync não está instalado${NC}"
    echo "Instale com: sudo apt-get install rsync"
    exit 1
fi

# Verificar se o diretório fonte existe
if [ ! -d "$SOURCE_DIR" ]; then
    echo -e "${RED}Erro: Diretório fonte não encontrado: $SOURCE_DIR${NC}"
    exit 1
fi

# Função para sincronizar
sync_files() {
    echo -e "${YELLOW}[$(date +'%H:%M:%S')] Sincronizando...${NC}"
    
    # Usar rsync com SSH
    # Opções:
    # -a: archive mode (preserva permissões, timestamps, etc)
    # -v: verbose
    # -z: compressão durante transferência
    # --delete: deleta arquivos no destino que não existem no origem
    # --exclude: excluir arquivos/diretórios desnecessários
    rsync -avz --delete \
        --exclude='.git/' \
        --exclude='build*/' \
        --exclude='cmake-build-*/' \
        --exclude='.vscode/' \
        --exclude='.idea/' \
        --exclude='*.o' \
        --exclude='*.a' \
        --exclude='*.so' \
        --exclude='*.dylib' \
        --exclude='*.dll' \
        --exclude='*.exe' \
        --exclude='CMakeFiles/' \
        --exclude='CMakeCache.txt' \
        --exclude='cmake_install.cmake' \
        --exclude='Makefile' \
        --exclude='*.swp' \
        --exclude='*.swo' \
        --exclude='*~' \
        --exclude='.DS_Store' \
        -e "ssh -p $SSH_PORT" \
        "$SOURCE_DIR/" "$REMOTE_HOST:$REMOTE_DIR/"
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}[$(date +'%H:%M:%S')] Sincronização concluída com sucesso${NC}"
    else
        echo -e "${RED}[$(date +'%H:%M:%S')] Erro na sincronização${NC}"
    fi
}

# Função para testar conexão SSH
test_connection() {
    echo -e "${YELLOW}Testando conexão SSH...${NC}"
    ssh -p $SSH_PORT -o ConnectTimeout=5 "$REMOTE_HOST" "echo 'Conexão OK'" &> /dev/null
    if [ $? -ne 0 ]; then
        echo -e "${RED}Erro: Não foi possível conectar ao servidor remoto${NC}"
        echo "Verifique:"
        echo "  - Se o servidor está acessível: ping 192.168.5.193"
        echo "  - Se o SSH está rodando na porta $SSH_PORT"
        echo "  - Se a autenticação SSH está configurada (chaves SSH)"
        exit 1
    fi
    echo -e "${GREEN}Conexão SSH OK${NC}"
}

# Sincronização inicial
echo -e "${GREEN}=== Sincronização Automática RetroCapture → Raspberry Pi ===${NC}"
echo "Diretório fonte: $SOURCE_DIR"
echo "Destino remoto: $REMOTE_HOST:$REMOTE_DIR"
echo ""

# Testar conexão antes de começar
test_connection

# Sincronização inicial completa
echo -e "${YELLOW}Realizando sincronização inicial...${NC}"
sync_files

# Verificar se foi solicitado apenas sincronização única
if [ "$1" == "--once" ]; then
    echo -e "${GREEN}Sincronização única concluída${NC}"
    exit 0
fi

echo ""
echo -e "${GREEN}Monitorando mudanças em: $SOURCE_DIR${NC}"
echo -e "${YELLOW}Pressione Ctrl+C para parar${NC}"
echo ""

# Monitorar mudanças e sincronizar automaticamente
# inotifywait monitora:
# - modify: arquivo modificado
# - create: arquivo/diretório criado
# - delete: arquivo/diretório deletado
# - move: arquivo/diretório movido
# - attrib: atributos do arquivo mudados (permissões, etc)
# -r: recursivo
# -m: monitor mode (não sai após primeiro evento)
# --exclude: excluir padrões
inotifywait -m -r \
    --exclude '\.(git|swp|swo|o|a|so|dylib|dll|exe)$|build.*|cmake-build-.*|CMakeFiles|\.vscode|\.idea' \
    -e modify,create,delete,move,attrib \
    "$SOURCE_DIR" | while read path action file; do
    
    # Ignorar alguns eventos de sistema
    if [[ "$file" =~ ^\.# ]] || [[ "$file" =~ ~$ ]]; then
        continue
    fi
    
    # Debounce: aguardar 1 segundo antes de sincronizar (evita múltiplas sincronizações)
    sleep 1
    
    # Sincronizar
    sync_files
done

