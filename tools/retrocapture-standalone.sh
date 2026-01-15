#!/bin/bash

LOCKFILE="/tmp/retrocapture.lock"

exec 200>"$LOCKFILE"
executing=false

flock -n 200 || {
    echo "⚠️ RetroCapture já está em execução. Saindo."
    executing=true
}

if [ "$executing" = false ]; then
    echo "✅ Lock adquirido. Iniciando RetroCapture watchdog."
    
    cleanup() {
        echo "🧹 Encerrando RetroCapture..."
        pkill -f pw-loopback
    }
    trap cleanup EXIT INT TERM
    
    create_pw_links() {
        local max_attempts=30
        local attempt=0
        
        echo "⏳ Aguardando RetroCapture sink estar pronto..."
        
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
        
        echo "🔍 Procurando loopbacks..."
        
        local max_loopback_attempts=20
        local loopback_attempt=0
        local loopback_id=""
        local output_loopback=""
        local input_loopback=""
        local output_sink="alsa_output.platform-fef00700.hdmi.hdmi-stereo"
        
        while [ $loopback_attempt -lt $max_loopback_attempts ]; do
            local available_inputs=$(pw-link -i 2>/dev/null | grep -oE "input\.loopback-[0-9]+-[0-9]+" | sort -u)
            local available_outputs=$(pw-link -o 2>/dev/null | grep -oE "output\.loopback-[0-9]+-[0-9]+" | sort -u)
            
            if [ -n "$available_inputs" ]; then
                loopback_id=$(echo "$available_inputs" | head -1 | grep -oE "[0-9]+-[0-9]+")
                if [ -n "$loopback_id" ]; then
                    input_loopback="input.loopback-${loopback_id}"
                    output_loopback="output.loopback-${loopback_id}"
                    break
                fi
            fi
            
            if [ -z "$loopback_id" ] && [ -n "$available_outputs" ]; then
                loopback_id=$(echo "$available_outputs" | head -1 | grep -oE "[0-9]+-[0-9]+")
                if [ -n "$loopback_id" ]; then
                    input_loopback="input.loopback-${loopback_id}"
                    output_loopback="output.loopback-${loopback_id}"
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
            return 1
        fi
        
        echo "✅ Loopbacks encontrados: $output_loopback, $input_loopback"
        
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
        done
        
        sleep 1
        
        if ! pw-link -i 2>/dev/null | grep -q "${output_sink}:playback"; then
            local available_output_sinks=$(pw-link -i 2>/dev/null | grep -oE "alsa_output\.[^:]+" | head -1)
            if [ -n "$available_output_sinks" ]; then
                output_sink="$available_output_sinks"
            else
                output_sink=""
            fi
        fi
        
        echo "🔗 Criando conexões PipeWire..."
        
        pw-link -d "${output_loopback}:output_FL" RetroCapture:input_FL 2>/dev/null
        pw-link -d "${output_loopback}:output_FR" RetroCapture:input_FR 2>/dev/null
        if [ -n "$output_sink" ]; then
            pw-link -d "${input_loopback}:monitor_FL" "${output_sink}:playback_FL" 2>/dev/null
            pw-link -d "${input_loopback}:monitor_FR" "${output_sink}:playback_FR" 2>/dev/null
        fi
        sleep 0.5
        
        local link_attempts=0
        local max_link_attempts=5
        
        while [ $link_attempts -lt $max_link_attempts ]; do
            if pw-link "${output_loopback}:output_FL" RetroCapture:input_FL 2>/dev/null; then
                echo "   ✅ Link FL criado"
                break
            fi
            link_attempts=$((link_attempts + 1))
            [ $link_attempts -lt $max_link_attempts ] && sleep 1
        done
        
        link_attempts=0
        while [ $link_attempts -lt $max_link_attempts ]; do
            if pw-link "${output_loopback}:output_FR" RetroCapture:input_FR 2>/dev/null; then
                echo "   ✅ Link FR criado"
                break
            fi
            link_attempts=$((link_attempts + 1))
            [ $link_attempts -lt $max_link_attempts ] && sleep 1
        done
        
        if [ -n "$output_sink" ]; then
            link_attempts=0
            while [ $link_attempts -lt $max_link_attempts ]; do
                if pw-link "${input_loopback}:monitor_FL" "${output_sink}:playback_FL" 2>/dev/null; then
                    echo "   ✅ Link monitor FL criado"
                    break
                fi
                link_attempts=$((link_attempts + 1))
                [ $link_attempts -lt $max_link_attempts ] && sleep 1
            done
            
            link_attempts=0
            while [ $link_attempts -lt $max_link_attempts ]; do
                if pw-link "${input_loopback}:monitor_FR" "${output_sink}:playback_FR" 2>/dev/null; then
                    echo "   ✅ Link monitor FR criado"
                    break
                fi
                link_attempts=$((link_attempts + 1))
                [ $link_attempts -lt $max_link_attempts ] && sleep 1
            done
        fi
        
        echo "✅ Processo concluído"
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
        
        create_pw_links &
        LINK_PID=$!
        
        wait $RETROCAPTURE_PID
        RETROCAPTURE_EXIT=$?
        
        if kill -0 $LINK_PID 2>/dev/null; then
            kill $LINK_PID 2>/dev/null
        fi
        
        echo "⚠️ RetroCapture finalizou (exit code: $RETROCAPTURE_EXIT). Reiniciando em 2s..."
        sleep 2
    done
fi