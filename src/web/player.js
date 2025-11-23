// RetroCapture Stream Player - Modernized
(function() {
    'use strict';
    
    const video = document.getElementById('videoPlayer');
    const status = document.getElementById('status');
    const statusIcon = document.getElementById('statusIcon');
    const codecEl = document.getElementById('codec');
    const resolutionEl = document.getElementById('resolution');
    const qualityEl = document.getElementById('quality');
    const streamUrlEl = document.getElementById('streamUrl');
    const formatEl = document.getElementById('format');
    const protocolEl = document.getElementById('protocol');
    const playbackTimeEl = document.getElementById('playbackTime');
    const bufferLevelEl = document.getElementById('bufferLevel');
    const networkQualityEl = document.getElementById('networkQuality');
    const bitrateEl = document.getElementById('bitrate');
    const fpsEl = document.getElementById('fps');
    const videoOverlay = document.getElementById('videoOverlay');
    const alertContainer = document.getElementById('alertContainer');
    const streamLink = document.getElementById('streamLink');
    
    let hls = null;
    let statsInterval = null;
    let playbackStartTime = null;
    let lastTimeUpdate = 0;
    let frameCount = 0;
    let lastFpsTime = Date.now();
    
    // URLs do stream
    const basePath = window.location.pathname.split('/').slice(0, -1).join('/') || '';
    const streamUrl = basePath + '/stream';
    const hlsUrl = basePath + '/stream.m3u8';
    const fullStreamUrl = window.location.origin + streamUrl;
    
    // Atualizar URLs
    streamUrlEl.textContent = fullStreamUrl;
    streamLink.href = streamUrl;
    protocolEl.textContent = window.location.protocol === 'https:' ? 'HTTPS' : 'HTTP';
    
    function updateStatus(text, className) {
        status.textContent = text;
        status.className = 'stat-value ' + className;
        statusIcon.className = 'bi bi-circle-fill ' + className;
        
        // Atualizar overlay
        if (className === 'status-playing' || className === 'status-paused') {
            videoOverlay.classList.add('hidden');
        } else {
            videoOverlay.classList.remove('hidden');
        }
    }
    
    function updateStats() {
        // Resolução
        if (video.videoWidth && video.videoHeight) {
            resolutionEl.textContent = `${video.videoWidth}×${video.videoHeight}`;
            
            // Calcular qualidade aproximada
            const pixels = video.videoWidth * video.videoHeight;
            if (pixels >= 2073600) { // 1920x1080
                qualityEl.textContent = '1080p';
            } else if (pixels >= 921600) { // 1280x720
                qualityEl.textContent = '720p';
            } else if (pixels >= 480000) { // 800x600
                qualityEl.textContent = 'SD';
            } else {
                qualityEl.textContent = 'Low';
            }
        }
        
        // Codec
        if (video.videoTracks && video.videoTracks.length > 0) {
            codecEl.textContent = 'H.264/AAC';
        } else {
            codecEl.textContent = 'HLS';
        }
        
        // Tempo de reprodução
        if (video.currentTime > 0) {
            const minutes = Math.floor(video.currentTime / 60);
            const seconds = Math.floor(video.currentTime % 60);
            playbackTimeEl.textContent = `${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;
        }
        
        // Buffer level
        if (video.buffered && video.buffered.length > 0) {
            const bufferedEnd = video.buffered.end(video.buffered.length - 1);
            const bufferTime = bufferedEnd - video.currentTime;
            if (bufferTime > 0) {
                bufferLevelEl.textContent = `${bufferTime.toFixed(1)}s`;
            } else {
                bufferLevelEl.textContent = '-';
            }
        }
        
        // FPS (aproximado)
        const now = Date.now();
        if (video.readyState >= 2) {
            frameCount++;
            if (now - lastFpsTime >= 1000) {
                const fps = frameCount;
                fpsEl.textContent = fps > 0 ? `${fps} fps` : '-';
                frameCount = 0;
                lastFpsTime = now;
            }
        }
        
        // Network quality (baseado em buffered)
        if (video.buffered && video.buffered.length > 0) {
            const bufferedEnd = video.buffered.end(video.buffered.length - 1);
            const bufferTime = bufferedEnd - video.currentTime;
            if (bufferTime > 10) {
                networkQualityEl.textContent = 'Excelente';
                networkQualityEl.className = 'info-value status-playing';
            } else if (bufferTime > 5) {
                networkQualityEl.textContent = 'Boa';
                networkQualityEl.className = 'info-value status-connecting';
            } else if (bufferTime > 2) {
                networkQualityEl.textContent = 'Média';
                networkQualityEl.className = 'info-value status-connecting';
            } else {
                networkQualityEl.textContent = 'Baixa';
                networkQualityEl.className = 'info-value status-error';
            }
        }
        
        // Bitrate (se disponível via HLS)
        if (hls && hls.levels && hls.levels.length > 0) {
            const currentLevel = hls.levels[hls.currentLevel];
            if (currentLevel && currentLevel.bitrate) {
                const bitrateMbps = (currentLevel.bitrate / 1000000).toFixed(2);
                bitrateEl.textContent = `${bitrateMbps} Mbps`;
            } else {
                bitrateEl.textContent = '-';
            }
        } else {
            bitrateEl.textContent = '-';
        }
    }
    
    function showAlert(type, title, content) {
        // Remover alertas anteriores
        alertContainer.innerHTML = '';
        
        const alert = document.createElement('div');
        alert.className = `alert alert-${type} alert-dismissible fade show`;
        alert.setAttribute('role', 'alert');
        
        const icon = type === 'danger' ? 'exclamation-triangle' : 
                     type === 'success' ? 'check-circle' : 'info-circle';
        
        alert.innerHTML = `
            <div class="d-flex align-items-start">
                <i class="bi bi-${icon} me-2 fs-5"></i>
                <div class="flex-grow-1">
                    <strong>${title}</strong>
                    <div class="mt-1">${content}</div>
                </div>
                <button type="button" class="btn-close btn-close-white" data-bs-dismiss="alert"></button>
            </div>
        `;
        
        alertContainer.appendChild(alert);
        
        // Auto-dismiss após 5 segundos
        setTimeout(() => {
            if (alert.parentNode) {
                alert.classList.remove('show');
                setTimeout(() => alert.remove(), 300);
            }
        }, 5000);
    }
    
    function hideAlert() {
        const alerts = alertContainer.querySelectorAll('.alert');
        alerts.forEach(alert => {
            alert.classList.remove('show');
            setTimeout(() => alert.remove(), 300);
        });
    }
    
    // Verificar se HLS.js está disponível
    if (typeof Hls === 'undefined') {
        updateStatus('HLS.js não carregado', 'status-error');
        codecEl.textContent = 'Erro';
        showAlert('danger', 'Erro ao carregar HLS.js', 
            'A biblioteca HLS.js não foi carregada. Verifique sua conexão com a internet.');
        return;
    }
    
    // Verificar suporte nativo a HLS (Safari)
    if (video.canPlayType('application/vnd.apple.mpegurl')) {
        video.src = window.location.protocol + '//' + window.location.host + hlsUrl;
        
        video.addEventListener('loadedmetadata', () => {
            updateStatus('Pronto', 'status-connecting');
            updateStats();
        });
        
        video.addEventListener('play', () => {
            updateStatus('Reproduzindo', 'status-playing');
            hideAlert();
            playbackStartTime = Date.now();
            startStatsUpdate();
        });
        
        video.addEventListener('pause', () => {
            updateStatus('Pausado', 'status-paused');
            stopStatsUpdate();
        });
        
        video.addEventListener('error', handleVideoError);
    } else if (Hls.isSupported()) {
        // Usar HLS.js
        hls = new Hls({
            enableWorker: true,
            lowLatencyMode: true,
            backBufferLength: 90,
            maxBufferLength: 30,
            maxMaxBufferLength: 60
        });
        
        const absoluteHlsUrl = window.location.protocol + '//' + window.location.host + hlsUrl;
        hls.loadSource(absoluteHlsUrl);
        hls.attachMedia(video);
        
        hls.on(Hls.Events.MANIFEST_PARSED, () => {
            updateStatus('Pronto', 'status-connecting');
            updateStats();
            hideAlert();
            
            video.play().catch(err => {
                console.log('Autoplay bloqueado:', err);
                updateStatus('Clique para reproduzir', 'status-paused');
            });
        });
        
        hls.on(Hls.Events.ERROR, (event, data) => {
            console.error('HLS error:', data);
            if (data.fatal) {
                switch(data.type) {
                    case Hls.ErrorTypes.NETWORK_ERROR:
                        updateStatus('Erro de rede', 'status-error');
                        showAlert('danger', 'Erro de Rede', 
                            'Ocorreu um erro de rede ao carregar o stream. Tentando reconectar...');
                        hls.startLoad();
                        break;
                    case Hls.ErrorTypes.MEDIA_ERROR:
                        updateStatus('Erro de mídia', 'status-error');
                        showAlert('danger', 'Erro de Mídia', 
                            'Ocorreu um erro ao decodificar o stream. Tentando recuperar...');
                        hls.recoverMediaError();
                        break;
                    default:
                        updateStatus('Erro fatal', 'status-error');
                        showAlert('danger', 'Erro Fatal', 
                            'Ocorreu um erro fatal. Por favor, recarregue a página.');
                        hls.destroy();
                        break;
                }
            }
        });
        
        video.addEventListener('play', () => {
            updateStatus('Reproduzindo', 'status-playing');
            hideAlert();
            playbackStartTime = Date.now();
            startStatsUpdate();
        });
        
        video.addEventListener('pause', () => {
            updateStatus('Pausado', 'status-paused');
            stopStatsUpdate();
        });
        
        video.addEventListener('error', handleVideoError);
    } else {
        updateStatus('HLS não suportado', 'status-error');
        codecEl.textContent = 'Não suportado';
        showAlert('danger', 'HLS não é suportado', 
            `<p class="mb-2">Seu navegador não suporta HLS (HTTP Live Streaming).</p>
             <p class="mb-0"><strong>Use VLC Media Player ou ffplay/mpv com a URL do stream.</strong></p>`);
        return;
    }
    
    function handleVideoError(e) {
        console.error('Video error:', video.error);
        let errorMsg = 'Erro desconhecido';
        if (video.error) {
            switch(video.error.code) {
                case video.error.MEDIA_ERR_ABORTED:
                    errorMsg = 'Reprodução abortada';
                    break;
                case video.error.MEDIA_ERR_NETWORK:
                    errorMsg = 'Erro de rede';
                    break;
                case video.error.MEDIA_ERR_DECODE:
                    errorMsg = 'Erro de decodificação';
                    break;
                case video.error.MEDIA_ERR_SRC_NOT_SUPPORTED:
                    errorMsg = 'Formato não suportado';
                    break;
            }
        }
        updateStatus('Erro: ' + errorMsg, 'status-error');
        showAlert('danger', 'Erro no Vídeo', errorMsg);
    }
    
    function startStatsUpdate() {
        if (statsInterval) return;
        statsInterval = setInterval(updateStats, 500);
    }
    
    function stopStatsUpdate() {
        if (statsInterval) {
            clearInterval(statsInterval);
            statsInterval = null;
        }
    }
    
    // Event listeners
    video.addEventListener('loadstart', () => {
        updateStatus('Carregando...', 'status-connecting');
    });
    
    video.addEventListener('loadedmetadata', () => {
        updateStats();
    });
    
    video.addEventListener('loadeddata', () => {
        updateStats();
    });
    
    video.addEventListener('canplay', () => {
        updateStats();
    });
    
    video.addEventListener('timeupdate', () => {
        if (video.currentTime !== lastTimeUpdate) {
            frameCount++;
            lastTimeUpdate = video.currentTime;
        }
    });
    
    video.addEventListener('resize', updateStats);
    
    // Função global para copiar URL
    window.copyStreamUrl = function() {
        navigator.clipboard.writeText(fullStreamUrl).then(() => {
            showToast('success', 'URL copiada para a área de transferência!');
        }).catch(err => {
            // Fallback
            const textarea = document.createElement('textarea');
            textarea.value = fullStreamUrl;
            document.body.appendChild(textarea);
            textarea.select();
            document.execCommand('copy');
            document.body.removeChild(textarea);
            showToast('success', 'URL copiada para a área de transferência!');
        });
    };
    
    function showToast(type, message) {
        const toast = document.createElement('div');
        toast.className = `toast align-items-center text-white bg-${type === 'success' ? 'success' : 'danger'} border-0`;
        toast.setAttribute('role', 'alert');
        toast.setAttribute('aria-live', 'assertive');
        toast.setAttribute('aria-atomic', 'true');
        toast.innerHTML = `
            <div class="d-flex">
                <div class="toast-body">
                    <i class="bi bi-${type === 'success' ? 'check-circle' : 'exclamation-triangle'} me-2"></i>
                    ${message}
                </div>
                <button type="button" class="btn-close btn-close-white me-2 m-auto" data-bs-dismiss="toast"></button>
            </div>
        `;
        
        const container = document.getElementById('toastContainer');
        container.appendChild(toast);
        
        const bsToast = new bootstrap.Toast(toast);
        bsToast.show();
        
        toast.addEventListener('hidden.bs.toast', () => {
            container.removeChild(toast);
        });
    }
    
    // Inicializar stats
    updateStats();
})();
