#!/bin/bash
#pw-link alsa_input.usb-MACROSILICON_Hagibis_20210623-02.analog-stereo alsa_output.platform-fef00700.hdmi.hdmi-stereo
#pw-link alsa_input.usb-MACROSILICON_fifine_Video_Capture_48956587-02.analog-stereo:capture_FL alsa_output.platform-fef00700.hdmi.hdmi-stereo:playback_FL
#pw-link alsa_input.usb-MACROSILICON_fifine_Video_Capture_48956587-02.analog-stereo:capture_FR alsa_output.platform-fef00700.hdmi.hdmi-stereo:playback_FR

LOCKFILE="/tmp/retrocapture.lock"

# Abre FD 200 associado ao lockfile
exec 200>"$LOCKFILE"
executing=false

# Tenta adquirir lock exclusivo
flock -n 200 || {
    echo "⚠️ RetroCapture já está em execução. Saindo."
    #nao podemos dar exit aqui isso fecha o shell
    #exit 0
    executing=true
}

if [ "$executing" = false ]; then
    echo "✅ Lock adquirido. Iniciando RetroCapture watchdog."
    # Função para cleanup ao sair
    cleanup() {
        echo "🧹 Encerrando RetroCapture..."
        pkill -f pw-loopback
    }
    trap cleanup EXIT INT TERM
    
    # Função para encontrar o loopback dinamicamente
    find_loopback() {
        local target_name="$1"  # Nome do sink/source de destino (ex: "RetroCapture" ou source de entrada)
        local loopback_type="$2"  # "output" ou "input"
        
        # Lista todos os links
        local links=$(pw-link -l 2>/dev/null)
        
        if [ -z "$links" ]; then
            return 1
        fi
        
        # Procura por loopbacks conectados ao target
        # Formato 1: target:port <- loopback-XXX-YY:port (loopback conectado ao target)
        # Formato 2: loopback-XXX-YY:port -> target:port (loopback conectado ao target)
        local loopback_id=$(echo "$links" | grep -E "${target_name}:" | grep -oE "${loopback_type}\.loopback-[0-9]+-[0-9]+" | head -1)
        
        if [ -n "$loopback_id" ]; then
            echo "$loopback_id"
            return 0
        fi
        
        # Tenta também listar todos os loopbacks disponíveis e verificar conexões
        local all_loopbacks=$(pw-link -o 2>/dev/null | grep -oE "${loopback_type}\.loopback-[0-9]+-[0-9]+" | sort -u)
        
        for loopback in $all_loopbacks; do
            # Verifica se este loopback está conectado ao target
            if echo "$links" | grep -qE "${loopback}.*${target_name}|${target_name}.*${loopback}"; then
                echo "$loopback"
                return 0
            fi
        done
        
        return 1
    }
    
    # Função para criar os pw-links quando o RetroCapture estiver pronto
    create_pw_links() {
        local max_attempts=30
        local attempt=0
        
        # Source de entrada (ajuste conforme necessário)
        local input_source="alsa_input.usb-MACROSILICON_fifine_Video_Capture_48956587-02.analog-stereo"
        
        echo "⏳ Aguardando RetroCapture sink estar pronto..."
        
        # Aguarda o sink RetroCapture estar disponível
        while [ $attempt -lt $max_attempts ]; do
            if pw-link -o 2>/dev/null | grep -q "RetroCapture:input_FL\|RetroCapture:monitor_FL"; then
                echo "✅ RetroCapture sink detectado!"
                break
            fi
            sleep 1
            attempt=$((attempt + 1))
            if [ $((attempt % 5)) -eq 0 ]; then
                echo "   Tentativa $attempt/$max_attempts..."
            fi
        done
        
        if [ $attempt -ge $max_attempts ]; then
            echo "⚠️ Timeout aguardando RetroCapture sink. Tentando criar links mesmo assim..."
        fi
        
        echo "🔍 Procurando loopbacks e verificando/criando links..."
        
        # O RetroCapture cria automaticamente um loopback quando connectInputSource() é chamado
        # Precisamos encontrar tanto output.loopback quanto input.loopback (mesmo ID)
        # E conectar:
        # - output.loopback:output_FL/FR -> RetroCapture:input_FL/FR
        # - input.loopback:monitor_FL/FR -> alsa_output.platform-fef00700.hdmi.hdmi-stereo:playback_FL/FR
        
        local max_loopback_attempts=20
        local loopback_attempt=0
        local loopback_id=""  # ID numérico do loopback (ex: 1074-13)
        local output_loopback=""
        local input_loopback=""
        local output_sink="alsa_output.platform-fef00700.hdmi.hdmi-stereo"
        
        # Aguarda o loopback ser criado pelo RetroCapture ou encontra um existente
        while [ $loopback_attempt -lt $max_loopback_attempts ]; do
            # Procura por loopbacks em pw-link -i e pw-link -o
            local available_inputs=$(pw-link -i 2>/dev/null | grep -oE "input\.loopback-[0-9]+-[0-9]+" | sort -u)
            local available_outputs=$(pw-link -o 2>/dev/null | grep -oE "output\.loopback-[0-9]+-[0-9]+" | sort -u)
            
            # Extrai o ID numérico do loopback (ex: 1074-13)
            if [ -n "$available_inputs" ]; then
                loopback_id=$(echo "$available_inputs" | head -1 | grep -oE "[0-9]+-[0-9]+")
                if [ -n "$loopback_id" ]; then
                    input_loopback="input.loopback-${loopback_id}"
                    output_loopback="output.loopback-${loopback_id}"
                    echo "   Loopback encontrado: ID=$loopback_id"
                    break
                fi
            fi
            
            # Se não encontrou em inputs, tenta em outputs
            if [ -z "$loopback_id" ] && [ -n "$available_outputs" ]; then
                loopback_id=$(echo "$available_outputs" | head -1 | grep -oE "[0-9]+-[0-9]+")
                if [ -n "$loopback_id" ]; then
                    input_loopback="input.loopback-${loopback_id}"
                    output_loopback="output.loopback-${loopback_id}"
                    echo "   Loopback encontrado em outputs: ID=$loopback_id"
                    break
                fi
            fi
            
            sleep 1
            loopback_attempt=$((loopback_attempt + 1))
            if [ $((loopback_attempt % 3)) -eq 0 ]; then
                echo "   Aguardando loopback... ($loopback_attempt/$max_loopback_attempts)"
            fi
        done
        
        if [ -z "$loopback_id" ] || [ -z "$output_loopback" ] || [ -z "$input_loopback" ]; then
            echo "⚠️ Loopback não encontrado."
            echo "   O RetroCapture deve criar o loopback quando uma source é conectada."
            echo "   Verifique se uma source de entrada foi selecionada na interface."
            echo "   Você pode verificar com: pw-link -i | grep loopback"
            return 1
        fi
        
        echo "✅ Loopbacks encontrados:"
        echo "   Output: $output_loopback"
        echo "   Input: $input_loopback"
        
        # Aguarda os sinks RetroCapture estarem totalmente prontos antes de criar links
        echo "⏳ Aguardando sinks RetroCapture estarem prontos..."
        local sink_ready_attempts=0
        local max_sink_ready_attempts=10
        
        while [ $sink_ready_attempts -lt $max_sink_ready_attempts ]; do
            local available_inputs=$(pw-link -i 2>/dev/null)
            if echo "$available_inputs" | grep -q "RetroCapture:input_FL" && echo "$available_inputs" | grep -q "RetroCapture:input_FR"; then
                echo "✅ Sinks RetroCapture prontos!"
                break
            fi
            sleep 1
            sink_ready_attempts=$((sink_ready_attempts + 1))
            if [ $((sink_ready_attempts % 2)) -eq 0 ]; then
                echo "   Aguardando sinks... ($sink_ready_attempts/$max_sink_ready_attempts)"
            fi
        done
        
        if [ $sink_ready_attempts -ge $max_sink_ready_attempts ]; then
            echo "⚠️ Timeout aguardando sinks RetroCapture. Tentando criar links mesmo assim..."
        fi
        
        # Aguarda mais um pouco para garantir que tudo está estável
        sleep 1
        
        # Encontra o sink de saída (output sink) - pode ser configurável no futuro
        # Por padrão, usa o HDMI como no exemplo
        local output_sink="alsa_output.platform-fef00700.hdmi.hdmi-stereo"
        
        # Verifica se o sink de saída existe, se não, tenta encontrar outro
        if ! pw-link -i 2>/dev/null | grep -q "${output_sink}:playback"; then
            echo "⚠️ Sink de saída padrão não encontrado, procurando alternativas..."
            local available_output_sinks=$(pw-link -i 2>/dev/null | grep -oE "alsa_output\.[^:]+" | head -1)
            if [ -n "$available_output_sinks" ]; then
                output_sink="$available_output_sinks"
                echo "   Usando sink alternativo: $output_sink"
            else
                echo "⚠️ Nenhum sink de saída encontrado, pulando conexões de monitoramento"
                output_sink=""
            fi
        fi
        
        echo "🔗 Criando conexões PipeWire..."
        
        # Remove links antigos se existirem
        pw-link -d "${output_loopback}:output_FL" RetroCapture:input_FL 2>/dev/null
        pw-link -d "${output_loopback}:output_FR" RetroCapture:input_FR 2>/dev/null
        if [ -n "$output_sink" ]; then
            pw-link -d "${input_loopback}:monitor_FL" "${output_sink}:playback_FL" 2>/dev/null
            pw-link -d "${input_loopback}:monitor_FR" "${output_sink}:playback_FR" 2>/dev/null
        fi
        sleep 0.5
        
        # Cria as conexões principais: output.loopback -> RetroCapture
        local link_attempts=0
        local max_link_attempts=5
        local fl_connected=false
        local fr_connected=false
        
        # Conecta output.loopback:output_FL -> RetroCapture:input_FL
        while [ $link_attempts -lt $max_link_attempts ]; do
            if pw-link "${output_loopback}:output_FL" RetroCapture:input_FL 2>/dev/null; then
                echo "   ✅ Link FL criado (${output_loopback}:output_FL -> RetroCapture:input_FL)"
                fl_connected=true
                break
            else
                link_attempts=$((link_attempts + 1))
                if [ $link_attempts -lt $max_link_attempts ]; then
                    sleep 1
                fi
            fi
        done
        if [ "$fl_connected" = false ]; then
            echo "   ⚠️ Falha ao criar link FL após $max_link_attempts tentativas"
        fi
        
        # Conecta output.loopback:output_FR -> RetroCapture:input_FR
        link_attempts=0
        while [ $link_attempts -lt $max_link_attempts ]; do
            if pw-link "${output_loopback}:output_FR" RetroCapture:input_FR 2>/dev/null; then
                echo "   ✅ Link FR criado (${output_loopback}:output_FR -> RetroCapture:input_FR)"
                fr_connected=true
                break
            else
                link_attempts=$((link_attempts + 1))
                if [ $link_attempts -lt $max_link_attempts ]; then
                    sleep 1
                fi
            fi
        done
        if [ "$fr_connected" = false ]; then
            echo "   ⚠️ Falha ao criar link FR após $max_link_attempts tentativas"
        fi
        
        # Cria as conexões de monitoramento: input.loopback:monitor -> output sink
        if [ -n "$output_sink" ]; then
            link_attempts=0
            local monitor_fl_connected=false
            local monitor_fr_connected=false
            
            # Conecta input.loopback:monitor_FL -> output_sink:playback_FL
            while [ $link_attempts -lt $max_link_attempts ]; do
                if pw-link "${input_loopback}:monitor_FL" "${output_sink}:playback_FL" 2>/dev/null; then
                    echo "   ✅ Link monitor FL criado (${input_loopback}:monitor_FL -> ${output_sink}:playback_FL)"
                    monitor_fl_connected=true
                    break
                else
                    link_attempts=$((link_attempts + 1))
                    if [ $link_attempts -lt $max_link_attempts ]; then
                        sleep 1
                    fi
                fi
            done
            if [ "$monitor_fl_connected" = false ]; then
                echo "   ⚠️ Falha ao criar link monitor FL após $max_link_attempts tentativas"
            fi
            
            # Conecta input.loopback:monitor_FR -> output_sink:playback_FR
            link_attempts=0
            while [ $link_attempts -lt $max_link_attempts ]; do
                if pw-link "${input_loopback}:monitor_FR" "${output_sink}:playback_FR" 2>/dev/null; then
                    echo "   ✅ Link monitor FR criado (${input_loopback}:monitor_FR -> ${output_sink}:playback_FR)"
                    monitor_fr_connected=true
                    break
                else
                    link_attempts=$((link_attempts + 1))
                    if [ $link_attempts -lt $max_link_attempts ]; then
                        sleep 1
                    fi
                fi
            done
            if [ "$monitor_fr_connected" = false ]; then
                echo "   ⚠️ Falha ao criar link monitor FR após $max_link_attempts tentativas"
            fi
        fi
        
        # Verifica se os links foram criados
        sleep 0.5
        local links=$(pw-link -l 2>/dev/null)
        local fl_verified=$(echo "$links" | grep -qE "${output_loopback}:output_FL.*RetroCapture:input_FL|RetroCapture:input_FL.*${output_loopback}:output_FL" && echo "yes" || echo "no")
        local fr_verified=$(echo "$links" | grep -qE "${output_loopback}:output_FR.*RetroCapture:input_FR|RetroCapture:input_FR.*${output_loopback}:output_FR" && echo "yes" || echo "no")
        
        if [ "$fl_verified" = "yes" ] && [ "$fr_verified" = "yes" ]; then
            echo "✅ Links principais verificados com sucesso!"
        else
            echo "⚠️ Alguns links principais podem não ter sido criados corretamente"
        fi
        
        echo "✅ Processo de verificação/criação de links concluído"
    }
    
    while true; do
        cd ~/RetroCapture || exit 1
        
        echo "🎮 Iniciando RetroCapture..."
        ./retrocapture \
        --web-portal-enable \
        --web-portal-start \
        --hide-ui \
        --fullscreen &
        
        RETROCAPTURE_PID=$!
        
        # Executa a criação de links em background, aguardando o RetroCapture estar pronto
        create_pw_links &
        LINK_PID=$!
        
        # Aguarda o RetroCapture terminar
        wait $RETROCAPTURE_PID
        RETROCAPTURE_EXIT=$?
        
        # Mata o processo de criação de links se ainda estiver rodando
        if kill -0 $LINK_PID 2>/dev/null; then
            kill $LINK_PID 2>/dev/null
        fi
        
        echo "⚠️ RetroCapture finalizou (exit code: $RETROCAPTURE_EXIT). Reiniciando em 2s..."
        sleep 2
    done
fi