#!/bin/bash

# Script para sincronizar automaticamente o código fonte com a Raspberry Pi
# Monitora mudanças no diretório local e sincroniza automaticamente via rsync/SSH
#
# Uso:
#   ./sync-source-raspiberry.sh                    # Sincronização contínua (monitora mudanças)
#   ./sync-source-raspiberry.sh --once              # Sincronização única (sem monitoramento)
#   ./sync-source-raspiberry.sh --config            # Configurar parâmetros
#   ./sync-source-raspiberry.sh --help             # Mostrar ajuda
#
# Parâmetros:
#   --ip IP                    IP ou hostname do servidor remoto
#   --user USER                Usuário SSH
#   --port PORT                Porta SSH (padrão: 22)
#   --source DIR               Diretório fonte local (padrão: diretório atual)
#   --dest DIR                 Diretório destino remoto
#   --once                     Sincronização única (sem monitoramento)
#   --config                   Configurar parâmetros interativamente
#   --help                     Mostrar esta ajuda

set -e

# Cores para output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

# Arquivo de configuração
CONFIG_FILE="$HOME/.retrocapture-sync-config"

# Valores padrão
DEFAULT_SSH_PORT=22
DEFAULT_SOURCE_DIR="$(pwd)"

# Função para mostrar ajuda
show_help() {
    cat << EOF
${CYAN}=== RetroCapture - Sincronização com Raspberry Pi ===${NC}

${GREEN}Uso:${NC}
  $0 [OPÇÕES]

${GREEN}Opções:${NC}
  --ip IP                    IP ou hostname do servidor remoto
  --user USER                Usuário SSH
  --port PORT                Porta SSH (padrão: 22)
  --source DIR               Diretório fonte local (padrão: diretório atual)
  --dest DIR                 Diretório destino remoto
  --once                     Sincronização única (sem monitoramento)
  --config                   Configurar parâmetros interativamente
  --help                     Mostrar esta ajuda

${GREEN}Exemplos:${NC}
  # Configurar pela primeira vez
  $0 --config

  # Sincronização única
  $0 --once

  # Sincronização contínua (monitora mudanças)
  $0

  # Sincronização com parâmetros específicos
  $0 --ip 192.168.1.100 --user pi --source /path/to/source --dest /home/pi/Projects/RetroCapture

${GREEN}Configuração SSH:${NC}
  Para evitar solicitar senha toda vez, configure autenticação por chave SSH:
  
  1. Gerar chave SSH (se não tiver):
     ssh-keygen -t ed25519 -C "retrocapture-sync"
  
  2. Copiar chave para o servidor remoto:
     ssh-copy-id -p PORT USER@IP
  
  3. Testar conexão:
     ssh -p PORT USER@IP

${GREEN}Arquivo de configuração:${NC}
  $CONFIG_FILE
  (Criado automaticamente na primeira execução com --config)

EOF
}

# Função para carregar configuração
load_config() {
    if [ -f "$CONFIG_FILE" ]; then
        source "$CONFIG_FILE"
    fi
}

# Função para salvar configuração
save_config() {
    cat > "$CONFIG_FILE" << EOF
# RetroCapture Sync Configuration
# Gerado automaticamente em $(date)

REMOTE_IP="$REMOTE_IP"
REMOTE_USER="$REMOTE_USER"
SSH_PORT="$SSH_PORT"
SOURCE_DIR="$SOURCE_DIR"
REMOTE_DIR="$REMOTE_DIR"
EOF
    chmod 600 "$CONFIG_FILE"
    echo -e "${GREEN}✓ Configuração salva em: $CONFIG_FILE${NC}"
}

# Função para configurar interativamente
configure_interactive() {
    echo -e "${CYAN}=== Configuração de Sincronização RetroCapture ===${NC}"
    echo ""
    
    # Carregar valores existentes se houver
    load_config
    
    # IP ou hostname
    if [ -z "$REMOTE_IP" ]; then
        read -p "IP ou hostname do servidor remoto: " REMOTE_IP
    else
        read -p "IP ou hostname do servidor remoto [$REMOTE_IP]: " input
        REMOTE_IP="${input:-$REMOTE_IP}"
    fi
    
    # Usuário
    if [ -z "$REMOTE_USER" ]; then
        read -p "Usuário SSH: " REMOTE_USER
    else
        read -p "Usuário SSH [$REMOTE_USER]: " input
        REMOTE_USER="${input:-$REMOTE_USER}"
    fi
    
    # Porta SSH
    if [ -z "$SSH_PORT" ]; then
        read -p "Porta SSH [$DEFAULT_SSH_PORT]: " input
        SSH_PORT="${input:-$DEFAULT_SSH_PORT}"
    else
        read -p "Porta SSH [$SSH_PORT]: " input
        SSH_PORT="${input:-$SSH_PORT}"
    fi
    
    # Diretório fonte
    if [ -z "$SOURCE_DIR" ]; then
        read -p "Diretório fonte local [$DEFAULT_SOURCE_DIR]: " input
        SOURCE_DIR="${input:-$DEFAULT_SOURCE_DIR}"
    else
        read -p "Diretório fonte local [$SOURCE_DIR]: " input
        SOURCE_DIR="${input:-$SOURCE_DIR}"
    fi
    
    # Diretório destino
    if [ -z "$REMOTE_DIR" ]; then
        read -p "Diretório destino remoto: " REMOTE_DIR
    else
        read -p "Diretório destino remoto [$REMOTE_DIR]: " input
        REMOTE_DIR="${input:-$REMOTE_DIR}"
    fi
    
    # Validar diretório fonte
    if [ ! -d "$SOURCE_DIR" ]; then
        echo -e "${RED}Erro: Diretório fonte não encontrado: $SOURCE_DIR${NC}"
        exit 1
    fi
    
    # Salvar configuração
    save_config
    
    echo ""
    echo -e "${GREEN}✓ Configuração concluída!${NC}"
    echo ""
    echo -e "${YELLOW}Próximos passos:${NC}"
    echo "  1. Configure autenticação SSH por chave:"
    echo "     ssh-copy-id -p $SSH_PORT $REMOTE_USER@$REMOTE_IP"
    echo ""
    echo "  2. Teste a conexão:"
    echo "     ssh -p $SSH_PORT $REMOTE_USER@$REMOTE_IP"
    echo ""
    echo "  3. Execute a sincronização:"
    echo "     $0 --once"
    echo ""
}

# Função para verificar e configurar SSH
setup_ssh() {
    REMOTE_HOST="$REMOTE_USER@$REMOTE_IP"
    
    # Verificar se já existe chave SSH
    if [ ! -f "$HOME/.ssh/id_ed25519" ] && [ ! -f "$HOME/.ssh/id_rsa" ] && [ ! -f "$HOME/.ssh/id_ecdsa" ]; then
        echo -e "${YELLOW}⚠ Nenhuma chave SSH encontrada${NC}"
        echo -e "${CYAN}Deseja gerar uma chave SSH agora? (s/N)${NC}"
        read -r response
        if [[ "$response" =~ ^([sS][iI][mM]|[sS])$ ]]; then
            echo -e "${YELLOW}Gerando chave SSH...${NC}"
            ssh-keygen -t ed25519 -C "retrocapture-sync" -f "$HOME/.ssh/id_ed25519" -N ""
            echo -e "${GREEN}✓ Chave SSH gerada${NC}"
        fi
    fi
    
    # Verificar se a chave já está no servidor
    echo -e "${YELLOW}Verificando autenticação SSH...${NC}"
    if ssh -p "$SSH_PORT" -o ConnectTimeout=5 -o BatchMode=yes "$REMOTE_HOST" "echo 'OK'" &> /dev/null; then
        echo -e "${GREEN}✓ Autenticação SSH por chave configurada${NC}"
        return 0
    else
        echo -e "${YELLOW}⚠ Autenticação por chave não configurada${NC}"
        echo -e "${CYAN}Deseja copiar a chave SSH para o servidor? (s/N)${NC}"
        read -r response
        if [[ "$response" =~ ^([sS][iI][mM]|[sS])$ ]]; then
            echo -e "${YELLOW}Copiando chave SSH...${NC}"
            ssh-copy-id -p "$SSH_PORT" "$REMOTE_HOST"
            if [ $? -eq 0 ]; then
                echo -e "${GREEN}✓ Chave SSH copiada com sucesso${NC}"
                return 0
            else
                echo -e "${RED}✗ Erro ao copiar chave SSH${NC}"
                echo -e "${YELLOW}Você precisará informar a senha a cada sincronização${NC}"
                return 1
            fi
        else
            echo -e "${YELLOW}Você precisará informar a senha a cada sincronização${NC}"
            return 1
        fi
    fi
}

# Função para testar conexão SSH
test_connection() {
    REMOTE_HOST="$REMOTE_USER@$REMOTE_IP"
    echo -e "${YELLOW}Testando conexão SSH...${NC}"
    
    if ssh -p "$SSH_PORT" -o ConnectTimeout=5 "$REMOTE_HOST" "echo 'Conexão OK'" &> /dev/null; then
        echo -e "${GREEN}✓ Conexão SSH OK${NC}"
        return 0
    else
        echo -e "${RED}✗ Erro: Não foi possível conectar ao servidor remoto${NC}"
        echo ""
        echo -e "${YELLOW}Verifique:${NC}"
        echo "  - Se o servidor está acessível: ping $REMOTE_IP"
        echo "  - Se o SSH está rodando na porta $SSH_PORT"
        echo "  - Se a autenticação SSH está configurada (chaves SSH)"
        echo ""
        echo -e "${CYAN}Configure autenticação SSH:${NC}"
        echo "  ssh-copy-id -p $SSH_PORT $REMOTE_HOST"
        return 1
    fi
}

# Função para sincronizar
sync_files() {
    REMOTE_HOST="$REMOTE_USER@$REMOTE_IP"
    
    echo -e "${YELLOW}[$(date +'%H:%M:%S')] Sincronizando...${NC}"
    echo -e "${BLUE}  Origem: $SOURCE_DIR${NC}"
    echo -e "${BLUE}  Destino: $REMOTE_HOST:$REMOTE_DIR${NC}"
    
    # Usar rsync com SSH
    # Opções:
    # -a: archive mode (preserva permissões, timestamps, etc)
    # -v: verbose
    # -z: compressão durante transferência
    # --delete: deleta arquivos no destino que não existem no origem
    # --exclude: excluir arquivos/diretórios desnecessários
    # --progress: mostrar progresso
    rsync -avz --delete --progress \
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
        --exclude='*.log' \
        -e "ssh -p $SSH_PORT" \
        "$SOURCE_DIR/" "$REMOTE_HOST:$REMOTE_DIR/"
    
    if [ $? -eq 0 ]; then
        echo -e "${GREEN}[$(date +'%H:%M:%S')] ✓ Sincronização concluída com sucesso${NC}"
        return 0
    else
        echo -e "${RED}[$(date +'%H:%M:%S')] ✗ Erro na sincronização${NC}"
        return 1
    fi
}

# Processar argumentos
MODE_ONCE=false
MODE_CONFIG=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --ip)
            REMOTE_IP="$2"
            shift 2
            ;;
        --user)
            REMOTE_USER="$2"
            shift 2
            ;;
        --port)
            SSH_PORT="$2"
            shift 2
            ;;
        --source)
            SOURCE_DIR="$2"
            shift 2
            ;;
        --dest)
            REMOTE_DIR="$2"
            shift 2
            ;;
        --once)
            MODE_ONCE=true
            shift
            ;;
        --config)
            MODE_CONFIG=true
            shift
            ;;
        --help|-h)
            show_help
            exit 0
            ;;
        *)
            echo -e "${RED}Erro: Opção desconhecida: $1${NC}"
            echo "Use --help para ver as opções disponíveis"
            exit 1
            ;;
    esac
done

# Modo configuração
if [ "$MODE_CONFIG" = true ]; then
    configure_interactive
    exit 0
fi

# Carregar configuração salva
load_config

# Validar parâmetros obrigatórios
if [ -z "$REMOTE_IP" ] || [ -z "$REMOTE_USER" ] || [ -z "$REMOTE_DIR" ]; then
    echo -e "${YELLOW}⚠ Configuração incompleta${NC}"
    echo -e "${CYAN}Execute '$0 --config' para configurar pela primeira vez${NC}"
    echo ""
    echo -e "${YELLOW}Ou forneça os parâmetros:${NC}"
    echo "  $0 --ip IP --user USER --dest DIR [--source DIR] [--port PORT]"
    echo ""
    echo "Use --help para mais informações"
    exit 1
fi

# Valores padrão
SOURCE_DIR="${SOURCE_DIR:-$DEFAULT_SOURCE_DIR}"
SSH_PORT="${SSH_PORT:-$DEFAULT_SSH_PORT}"

# Verificar dependências
if ! command -v inotifywait &> /dev/null && [ "$MODE_ONCE" = false ]; then
    echo -e "${RED}Erro: inotifywait não está instalado${NC}"
    echo "Instale com: sudo apt-get install inotify-tools"
    exit 1
fi

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

# Configurar SSH (apenas na primeira vez ou se solicitado)
if [ ! -f "$CONFIG_FILE" ] || [ "$MODE_ONCE" = true ]; then
    setup_ssh
fi

# Sincronização inicial
echo ""
echo -e "${CYAN}=== Sincronização RetroCapture → Raspberry Pi ===${NC}"
echo -e "${BLUE}Diretório fonte: $SOURCE_DIR${NC}"
echo -e "${BLUE}Destino remoto: $REMOTE_USER@$REMOTE_IP:$REMOTE_DIR${NC}"
echo -e "${BLUE}Porta SSH: $SSH_PORT${NC}"
echo ""

# Testar conexão antes de começar
if ! test_connection; then
    exit 1
fi

# Sincronização inicial completa
echo -e "${YELLOW}Realizando sincronização inicial...${NC}"
if ! sync_files; then
    exit 1
fi

# Verificar se foi solicitado apenas sincronização única
if [ "$MODE_ONCE" = true ]; then
    echo ""
    echo -e "${GREEN}✓ Sincronização única concluída${NC}"
    exit 0
fi

echo ""
echo -e "${GREEN}✓ Monitorando mudanças em: $SOURCE_DIR${NC}"
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
    --exclude '\.(git|swp|swo|o|a|so|dylib|dll|exe|log)$|build.*|cmake-build-.*|CMakeFiles|\.vscode|\.idea' \
    -e modify,create,delete,move,attrib \
    "$SOURCE_DIR" 2>/dev/null | while read path action file; do
    
    # Ignorar alguns eventos de sistema
    if [[ "$file" =~ ^\.# ]] || [[ "$file" =~ ~$ ]]; then
        continue
    fi
    
    # Debounce: aguardar 1 segundo antes de sincronizar (evita múltiplas sincronizações)
    sleep 1
    
    # Sincronizar
    sync_files
done
