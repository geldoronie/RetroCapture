// RetroCapture Stream Player
(function() {
    'use strict';
    
    const video = document.getElementById('videoPlayer');
    const status = document.getElementById('status');
    const codecEl = document.getElementById('codec');
    const resolutionEl = document.getElementById('resolution');
    const streamUrlEl = document.getElementById('streamUrl');
    const warningDiv = document.getElementById('warning');
    const warningContent = document.getElementById('warningContent');
    const streamLink = document.getElementById('streamLink');
    
    // URLs do stream - usar protocolo relativo para seguir o protocolo da página
    // Detectar prefixo base (para suporte a proxy reverso)
    const basePath = window.location.pathname.split('/').slice(0, -1).join('/') || '';
    const streamUrl = basePath + '/stream';
    const hlsUrl = basePath + '/stream.m3u8'; // HLS playlist
    
    // Garantir que as URLs usem o mesmo protocolo da página atual
    const baseUrl = window.location.origin;
    streamUrlEl.textContent = baseUrl + streamUrl;
    streamLink.href = streamUrl; // URL relativa, seguirá o protocolo da página
    
    function updateStatus(text, className) {
        status.textContent = text;
        // Remove todas as classes de status
        status.classList.remove('bg-warning', 'bg-success', 'bg-danger', 'text-dark', 'text-light', 'connecting', 'playing', 'error');
        // Adiciona a classe apropriada
        if (className === 'connecting') {
            status.classList.add('bg-warning', 'text-dark', 'connecting');
        } else if (className === 'playing') {
            status.classList.add('bg-success', 'text-light', 'playing');
        } else if (className === 'error') {
            status.classList.add('bg-danger', 'text-light', 'error');
        }
    }
    
    function updateCodec() {
        if (video.videoWidth && video.videoHeight) {
            resolutionEl.textContent = video.videoWidth + 'x' + video.videoHeight;
        }
        codecEl.textContent = 'HLS (H.264/AAC)';
    }
    
    function showWarning(type, title, content) {
        warningDiv.classList.remove('d-none', 'alert-warning', 'alert-danger', 'alert-success');
        warningDiv.classList.add('alert-' + type, 'show');
        
        const icon = type === 'danger' ? 'exclamation-triangle' : 
                     type === 'success' ? 'check-circle' : 'info-circle';
        
        warningContent.innerHTML = `
            <h5 class="mb-2"><i class="bi bi-${icon} me-2"></i>${title}</h5>
            ${content}
        `;
    }
    
    function hideWarning() {
        warningDiv.classList.add('d-none');
    }
    
    // Verificar se HLS.js está disponível
    if (typeof Hls === 'undefined') {
        updateStatus('HLS.js não carregado', 'error');
        codecEl.textContent = 'Erro ao carregar biblioteca';
        showWarning('danger', 'Erro ao carregar HLS.js', 
            '<p class="mb-0">A biblioteca HLS.js não foi carregada. Verifique sua conexão com a internet.</p>');
        return;
    }
    
    // Verificar suporte nativo a HLS
    if (video.canPlayType('application/vnd.apple.mpegurl')) {
        // Safari suporta HLS nativamente
        // Usar URL absoluta com o protocolo correto e prefixo base
        video.src = window.location.protocol + '//' + window.location.host + hlsUrl;
        video.addEventListener('loadedmetadata', () => {
            updateStatus('Carregado', 'connecting');
            updateCodec();
        });
    } else if (Hls.isSupported()) {
        // Usar HLS.js para outros navegadores
        const hls = new Hls({
            enableWorker: true,
            lowLatencyMode: true,
            backBufferLength: 90
        });
        
        // Usar URL absoluta com o protocolo correto e prefixo base
        const absoluteHlsUrl = window.location.protocol + '//' + window.location.host + hlsUrl;
        hls.loadSource(absoluteHlsUrl);
        hls.attachMedia(video);
        
        hls.on(Hls.Events.MANIFEST_PARSED, () => {
            updateStatus('Pronto para reproduzir', 'connecting');
            updateCodec();
            hideWarning();
            video.play().catch(err => {
                console.log('Autoplay bloqueado:', err);
                updateStatus('Clique para reproduzir', 'connecting');
            });
        });
        
        hls.on(Hls.Events.ERROR, (event, data) => {
            console.error('HLS error:', data);
            if (data.fatal) {
                switch(data.type) {
                    case Hls.ErrorTypes.NETWORK_ERROR:
                        updateStatus('Erro de rede', 'error');
                        showWarning('danger', 'Erro de Rede', 
                            '<p class="mb-0">Ocorreu um erro de rede ao carregar o stream. Tentando reconectar...</p>');
                        hls.startLoad();
                        break;
                    case Hls.ErrorTypes.MEDIA_ERROR:
                        updateStatus('Erro de mídia', 'error');
                        showWarning('danger', 'Erro de Mídia', 
                            '<p class="mb-0">Ocorreu um erro ao decodificar o stream. Tentando recuperar...</p>');
                        hls.recoverMediaError();
                        break;
                    default:
                        updateStatus('Erro fatal', 'error');
                        showWarning('danger', 'Erro Fatal', 
                            '<p class="mb-0">Ocorreu um erro fatal. Por favor, recarregue a página.</p>');
                        hls.destroy();
                        break;
                }
            }
        });
    } else {
        // Navegador não suporta HLS
        updateStatus('HLS não suportado', 'error');
        codecEl.textContent = 'HLS não suportado pelo navegador';
        showWarning('danger', 'HLS não é suportado pelo navegador', 
            `<p class="mb-2">Seu navegador não suporta HLS (HTTP Live Streaming).</p>
             <p class="mb-2"><strong>Use um dos seguintes métodos:</strong></p>
             <ul class="mb-2">
                 <li><strong>VLC Media Player:</strong> Abra VLC → Mídia → Abrir Localização de Rede → Cole a URL abaixo</li>
                 <li><strong>ffplay:</strong> Execute no terminal: <code>ffplay ${streamUrl}</code></li>
                 <li><strong>mpv:</strong> Execute no terminal: <code>mpv ${streamUrl}</code></li>
             </ul>`);
        return;
    }
    
    // Event listeners do vídeo
    video.addEventListener('loadstart', () => {
        console.log('Video loadstart - iniciando carregamento');
        updateStatus('Carregando...', 'connecting');
    });
    
    video.addEventListener('loadedmetadata', () => {
        console.log('Video loadedmetadata - metadados carregados');
        updateCodec();
    });
    
    video.addEventListener('loadeddata', () => {
        console.log('Video loadeddata - dados carregados');
        updateCodec();
    });
    
    video.addEventListener('canplay', () => {
        console.log('Video canplay - pronto para reproduzir');
        updateCodec();
    });
    
    video.addEventListener('playing', () => {
        console.log('Video playing - reproduzindo');
        updateStatus('Reproduzindo', 'playing');
        hideWarning();
    });
    
    video.addEventListener('pause', () => {
        console.log('Video pause - pausado');
        updateStatus('Pausado', 'connecting');
    });
    
    video.addEventListener('error', (e) => {
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
        updateStatus('Erro: ' + errorMsg, 'error');
        showWarning('danger', 'Erro no Vídeo', `<p class="mb-0">${errorMsg}</p>`);
    });
    
    // Atualizar resolução quando disponível
    video.addEventListener('resize', updateCodec);
    
    // Função global para copiar URL
    window.copyStreamUrl = function() {
        const fullUrl = window.location.origin + streamUrl;
        navigator.clipboard.writeText(fullUrl).then(() => {
            // Mostrar toast de sucesso (Bootstrap 5)
            const toast = document.createElement('div');
            toast.className = 'toast align-items-center text-white bg-success border-0 position-fixed top-0 end-0 m-3';
            toast.setAttribute('role', 'alert');
            toast.setAttribute('aria-live', 'assertive');
            toast.setAttribute('aria-atomic', 'true');
            toast.innerHTML = `
                <div class="d-flex">
                    <div class="toast-body">
                        <i class="bi bi-check-circle me-2"></i>
                        URL copiada para a área de transferência!
                    </div>
                    <button type="button" class="btn-close btn-close-white me-2 m-auto" data-bs-dismiss="toast"></button>
                </div>
            `;
            document.body.appendChild(toast);
            const bsToast = new bootstrap.Toast(toast);
            bsToast.show();
            toast.addEventListener('hidden.bs.toast', () => {
                document.body.removeChild(toast);
            });
        }).catch(err => {
            // Fallback para navegadores antigos
            const textarea = document.createElement('textarea');
            textarea.value = fullUrl;
            document.body.appendChild(textarea);
            textarea.select();
            document.execCommand('copy');
            document.body.removeChild(textarea);
            
            // Mostrar toast de sucesso mesmo no fallback
            const toast = document.createElement('div');
            toast.className = 'toast align-items-center text-white bg-success border-0 position-fixed top-0 end-0 m-3';
            toast.setAttribute('role', 'alert');
            toast.innerHTML = `
                <div class="d-flex">
                    <div class="toast-body">
                        <i class="bi bi-check-circle me-2"></i>
                        URL copiada para a área de transferência!
                    </div>
                    <button type="button" class="btn-close btn-close-white me-2 m-auto" data-bs-dismiss="toast"></button>
                </div>
            `;
            document.body.appendChild(toast);
            const bsToast = new bootstrap.Toast(toast);
            bsToast.show();
            toast.addEventListener('hidden.bs.toast', () => {
                document.body.removeChild(toast);
            });
        });
    };
})();
