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
    
    // Detectar navegador para ajustar comportamento (deve estar no escopo global)
    const isChrome = /Chrome/.test(navigator.userAgent) && /Google Inc/.test(navigator.vendor);
    const isFirefox = /Firefox/.test(navigator.userAgent);
    console.log('Navegador detectado:', isChrome ? 'Chrome' : (isFirefox ? 'Firefox' : 'Outro'));
    
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
        
        // Atualizar overlay baseado no estado do vídeo
        const minReadyState = isChrome ? 3 : 2;
        const isVideoReady = video.readyState >= minReadyState && video.buffered.length > 0;
        
        if (className === 'status-playing') {
            // Vídeo está reproduzindo - esconder overlay
            videoOverlay.classList.add('hidden');
        } else if (className === 'status-paused' && isVideoReady) {
            // Vídeo está pausado mas pronto - mostrar botão de play
            showPlayButton();
        } else if (className === 'status-connecting' && isVideoReady) {
            // Status "Pronto" mas vídeo está pronto - esconder overlay ou mostrar botão
            if (video.paused) {
                showPlayButton();
            } else {
                videoOverlay.classList.add('hidden');
            }
        } else if (!isVideoReady) {
            // Vídeo ainda não está pronto - mostrar overlay de carregamento
            videoOverlay.classList.remove('hidden');
            videoOverlay.innerHTML = `
                <div class="overlay-content">
                    <div class="spinner-border text-primary" role="status">
                        <span class="visually-hidden">Carregando...</span>
                    </div>
                    <p class="mt-3 mb-0">${text}</p>
                </div>
            `;
        } else {
            // Outros casos - esconder overlay se vídeo está pronto
            if (isVideoReady) {
                videoOverlay.classList.add('hidden');
            }
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
    
    function showPlayButton() {
        // Mostrar botão de play no overlay se o vídeo estiver pausado e pronto
        const minReadyState = isChrome ? 3 : 2;
        if (video.paused && video.readyState >= minReadyState && video.buffered.length > 0) {
            const overlay = videoOverlay;
            if (overlay) {
                overlay.classList.remove('hidden');
                overlay.innerHTML = `
                    <div class="overlay-content">
                        <button id="playButton" class="btn btn-primary btn-lg">
                            <i class="bi bi-play-fill me-2"></i>Reproduzir Stream
                        </button>
                        <p class="mt-3 mb-0 text-muted">Clique para iniciar a reprodução</p>
                    </div>
                `;
                
                // Adicionar event listener ao botão (não usar onclick inline)
                const playButton = document.getElementById('playButton');
                if (playButton) {
                    playButton.addEventListener('click', () => {
                        video.play().then(() => {
                            console.log('Reprodução iniciada pelo botão');
                            updateStatus('Reproduzindo', 'status-playing');
                        }).catch(e => {
                            console.error('Erro ao reproduzir:', e);
                            showAlert('danger', 'Erro ao Reproduzir', 
                                'Não foi possível iniciar a reprodução. Tente novamente.');
                        });
                    });
                }
            }
        }
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
        // Usar HLS.js com parâmetros configuráveis
        // Usar configuração global se disponível, senão usar valores padrão
        const hlsConfig = window.RETROCAPTURE_HLS_CONFIG || {
            lowLatencyMode: true,
            backBufferLength: 30,
            maxBufferLength: 10,
            maxMaxBufferLength: 30,
            enableWorker: true
        };
        
        // Configuração do HLS.js com opções específicas para compatibilidade com Chrome
        hls = new Hls({
            debug: false,
            enableWorker: hlsConfig.enableWorker,
            lowLatencyMode: hlsConfig.lowLatencyMode,
            backBufferLength: hlsConfig.backBufferLength,
            maxBufferLength: hlsConfig.maxBufferLength,
            maxMaxBufferLength: hlsConfig.maxMaxBufferLength,
            // Configurações adicionais para Chrome
            startLevel: -1, // Deixar HLS.js escolher o nível inicial
            capLevelToPlayerSize: false,
            // Forçar reload da playlist periodicamente
            manifestLoadingTimeOut: 20000,
            manifestLoadingMaxRetry: 3,
            levelLoadingTimeOut: 10000,
            fragLoadingTimeOut: 20000,
            // Configurações de buffer para Chrome
            maxBufferHole: 0.5, // Tolerância para gaps no buffer
            highBufferWatchdogPeriod: 2, // Verificar buffer alto a cada 2s
            nudgeOffset: 0.1, // Offset para ajuste de sincronização
            nudgeMaxRetry: 3
        });
        
        const absoluteHlsUrl = window.location.protocol + '//' + window.location.host + hlsUrl;
        console.log('Carregando HLS stream:', absoluteHlsUrl);
        
        // IMPORTANTE: Ordem de inicialização diferente para Chrome vs Firefox
        // Chrome é mais rigoroso e precisa de uma sequência específica
        if (isChrome) {
            // Para Chrome: anexar mídia primeiro, depois carregar fonte, depois iniciar
            hls.attachMedia(video);
            
            // Registrar eventos ANTES de loadSource no Chrome
            // (alguns eventos podem não ser disparados se registrados depois)
            setupHLSEvents();
            
            // Carregar fonte
            hls.loadSource(absoluteHlsUrl);
            
            // Chrome precisa de startLoad explícito, mas não imediatamente
            // Aguardar um pouco para garantir que tudo está pronto
            setTimeout(() => {
                try {
                    if (hls.media) {
                        hls.startLoad();
                        console.log('Chrome: startLoad() chamado após loadSource');
                    }
                } catch(e) {
                    console.warn('Chrome: Erro ao chamar startLoad inicial:', e);
                }
            }, 200);
        } else {
            // Para Firefox e outros: ordem mais flexível
            hls.attachMedia(video);
            hls.loadSource(absoluteHlsUrl);
            
            // Registrar eventos (Firefox é mais tolerante com a ordem)
            setupHLSEvents();
            
            // Firefox geralmente inicia automaticamente, mas podemos forçar se necessário
            setTimeout(() => {
                try {
                    if (hls.media && !hls.media.readyState) {
                        hls.startLoad();
                        console.log('Firefox: startLoad() chamado');
                    }
                } catch(e) {
                    console.warn('Firefox: Erro ao chamar startLoad:', e);
                }
            }, 100);
        }
        
        // Função para configurar eventos do HLS (separada para reutilização)
        function setupHLSEvents() {
        
            hls.on(Hls.Events.MANIFEST_PARSED, (event, data) => {
                console.log('MANIFEST_PARSED:', data);
                updateStatus('Pronto', 'status-connecting');
                updateStats();
                hideAlert();
                
                // Não esconder overlay ainda - aguardar dados no buffer
                // O overlay será escondido quando BUFFER_APPENDED ou canplay for disparado
                
                // Chrome pode precisar de startLoad explícito após MANIFEST_PARSED
                // Firefox geralmente não precisa
                if (isChrome) {
                    try {
                        // Pequeno delay para garantir que tudo está processado
                        setTimeout(() => {
                            if (hls.media) {
                                hls.startLoad();
                                console.log('Chrome: startLoad() chamado após MANIFEST_PARSED');
                            }
                        }, 50);
                    } catch(e) {
                        console.warn('Erro ao chamar startLoad após MANIFEST_PARSED:', e);
                    }
                }
            });
        
        // Evento quando há dados suficientes no buffer para começar a reproduzir
        hls.on(Hls.Events.BUFFER_APPENDING, () => {
            console.log('BUFFER_APPENDING - dados sendo adicionados ao buffer');
        });
        
        // Evento quando o buffer está pronto
        hls.on(Hls.Events.BUFFER_APPENDED, (event, data) => {
            console.log('BUFFER_APPENDED - dados adicionados ao buffer:', data);
            console.log('Video readyState:', video.readyState, 'paused:', video.paused, 'buffered:', video.buffered.length);
            
            // Atualizar overlay quando há dados suficientes
            const minReadyState = isChrome ? 3 : 2;
            if (video.readyState >= minReadyState && video.buffered.length > 0) {
                // Vídeo está pronto - esconder overlay de carregamento ou mostrar botão de play
                if (video.paused) {
                    showPlayButton();
                } else {
                    videoOverlay.classList.add('hidden');
                }
            }
            
            // Tentar reproduzir quando há dados no buffer
            if (video.readyState >= minReadyState && video.paused) {
                console.log('Buffer pronto (readyState >= ' + minReadyState + '), tentando reproduzir...');
                const playPromise = video.play();
                if (playPromise !== undefined) {
                    playPromise.then(() => {
                        console.log('Reprodução iniciada com sucesso após buffer');
                        updateStatus('Reproduzindo', 'status-playing');
                    }).catch(err => {
                        console.log('Autoplay bloqueado após buffer:', err);
                        updateStatus('Clique para reproduzir', 'status-paused');
                        // Mostrar botão de play se autoplay falhar
                        showPlayButton();
                    });
                }
            } else if (video.readyState >= 2 && video.paused) {
                console.log('Buffer parcial (readyState >= 2), aguardando mais dados...');
            }
        });
        
            // Evento quando o nível é carregado (importante para live streams)
            hls.on(Hls.Events.LEVEL_LOADED, (event, data) => {
                console.log('LEVEL_LOADED:', data);
                console.log('Playlist carregada, níveis disponíveis:', data.details?.levels?.length || 0);
                
                // Chrome pode precisar de startLoad após LEVEL_LOADED
                // Firefox geralmente não precisa
                if (isChrome && hls.media && video.readyState >= 2) {
                    try {
                        hls.startLoad();
                        console.log('Chrome: startLoad() chamado após LEVEL_LOADED');
                    } catch(e) {
                        console.warn('Erro ao chamar startLoad após LEVEL_LOADED:', e);
                    }
                }
            });
        
            // Evento quando um fragmento é carregado com sucesso
            hls.on(Hls.Events.FRAG_LOADED, (event, data) => {
                if (data.frag) {
                    console.log('FRAG_LOADED:', data.frag.url, 'tipo:', data.frag.type);
                    
                    // Tentar reproduzir quando o primeiro fragmento é carregado
                    // Chrome precisa de readyState >= 3, Firefox pode funcionar com >= 2
                    const minReadyState = isChrome ? 3 : 2;
                    if (video.readyState >= minReadyState && video.paused) {
                        const playPromise = video.play();
                        if (playPromise !== undefined) {
                            playPromise.then(() => {
                                console.log('Reprodução iniciada após FRAG_LOADED');
                                updateStatus('Reproduzindo', 'status-playing');
                            }).catch(err => {
                                console.log('Autoplay bloqueado após FRAG_LOADED:', err);
                            });
                        }
                    }
                    
                    // Forçar atualização da playlist periodicamente para live streams
                    // Chrome precisa disso mais frequentemente
                    if (hls.levels && hls.levels.length > 0) {
                        const now = Date.now();
                        const reloadInterval = isChrome ? 5000 : 10000; // Chrome a cada 5s, Firefox a cada 10s
                        if (!window.lastPlaylistReload || (now - window.lastPlaylistReload) > reloadInterval) {
                            try {
                                if (isChrome) {
                                    hls.startLoad();
                                }
                                window.lastPlaylistReload = now;
                            } catch(e) {
                                console.warn('Erro ao recarregar playlist:', e);
                            }
                        }
                    }
                }
            });
        
            // Tratar quando um fragmento está sendo carregado
            hls.on(Hls.Events.FRAG_LOADING, (event, data) => {
                // Log para debug
                if (data.frag) {
                    console.log('Carregando fragmento:', data.frag.url);
                }
            });
            
            // Tratar quando um fragmento é analisado (parsed)
            hls.on(Hls.Events.FRAG_PARSED, (event, data) => {
                // Fragmento foi parseado com sucesso
                if (data.frag) {
                    console.log('FRAG_PARSED:', data.frag.url, 'tipo:', data.frag.type);
                    
                    // Tentar reproduzir quando fragmentos são parseados
                    // Chrome precisa de readyState >= 3, Firefox pode funcionar com >= 2
                    const minReadyState = isChrome ? 3 : 2;
                    if (video.readyState >= minReadyState && video.paused) {
                        const playPromise = video.play();
                        if (playPromise !== undefined) {
                            playPromise.then(() => {
                                console.log('Reprodução iniciada após FRAG_PARSED');
                                updateStatus('Reproduzindo', 'status-playing');
                            }).catch(err => {
                                // Ignorar erros de autoplay aqui
                            });
                        }
                    }
                }
            });
            
            hls.on(Hls.Events.ERROR, (event, data) => {
                console.error('HLS error:', data);
            
            // Tratar erros não-fatais primeiro
            if (!data.fatal) {
                switch(data.details) {
                    case 'bufferFullError':
                        // Buffer cheio - tentar reduzir o buffer ou limpar dados antigos
                        console.warn('Buffer cheio detectado. Tentando recuperar...');
                        // Tentar recuperar pausando e retomando o buffer
                        if (video.buffered && video.buffered.length > 0) {
                            const bufferedEnd = video.buffered.end(video.buffered.length - 1);
                            const currentTime = video.currentTime;
                            // Se o buffer está muito à frente, tentar avançar a reprodução
                            if (bufferedEnd - currentTime > 10) {
                                video.currentTime = bufferedEnd - 5; // Avançar para 5s antes do fim do buffer
                            }
                        }
                        // Tentar recuperar o erro de mídia
                        hls.recoverMediaError();
                        break;
                    case 'bufferStalledError':
                        console.warn('Buffer parado. Aguardando dados...');
                        // Tentar recarregar a playlist se o buffer estiver parado por muito tempo
                        setTimeout(() => {
                            if (video.readyState < 3) { // Se ainda não tem dados suficientes
                                console.log('Buffer ainda parado, tentando recarregar playlist...');
                                try {
                                    hls.startLoad();
                                } catch(e) {
                                    console.error('Erro ao recarregar:', e);
                                }
                            }
                        }, 3000); // Aguardar 3 segundos antes de tentar recarregar
                        break;
                    case 'bufferSeekOver':
                        console.warn('Seek além do buffer disponível');
                        break;
                    default:
                        console.warn('Erro não-fatal do HLS:', data.details);
                        break;
                }
                return; // Não processar erros não-fatais como fatais
            }
            
            // Tratar erros fatais
            if (data.fatal) {
                switch(data.type) {
                    case Hls.ErrorTypes.NETWORK_ERROR:
                        updateStatus('Erro de rede', 'status-error');
                        console.warn('Erro de rede do HLS, tentando reconectar...');
                        // Tentar recarregar a playlist e continuar
                        setTimeout(() => {
                            try {
                                hls.startLoad();
                            } catch(e) {
                                console.error('Erro ao tentar reconectar:', e);
                                // Se falhar, tentar recarregar completamente
                                hls.loadSource(absoluteHlsUrl);
                            }
                        }, 1000);
                        break;
                    case Hls.ErrorTypes.MEDIA_ERROR:
                        updateStatus('Erro de mídia', 'status-error');
                        console.error('Erro de mídia detalhado:', data);
                        
                        // Verificar tipo específico de erro de mídia
                        if (data.details) {
                            if (data.details.includes('demuxer') || data.details.includes('parse') || data.details.includes('format')) {
                                showAlert('danger', 'Erro de Formato', 
                                    'Ocorreu um erro ao processar o formato do stream. Isso pode indicar um problema com os segmentos. Tente recarregar a página.');
                                console.error('Erro de formato/demuxer detectado:', data.details);
                            } else {
                                showAlert('danger', 'Erro de Mídia', 
                                    'Ocorreu um erro ao decodificar o stream. Tentando recuperar...');
                            }
                        } else {
                            showAlert('danger', 'Erro de Mídia', 
                                'Ocorreu um erro ao decodificar o stream. Tentando recuperar...');
                        }
                        
                        // Tentar recuperar
                        try {
                            hls.recoverMediaError();
                        } catch(e) {
                            console.error('Erro ao tentar recuperar:', e);
                            // Se falhar, tentar recarregar completamente
                            setTimeout(() => {
                                try {
                                    hls.loadSource(absoluteHlsUrl);
                                    hls.startLoad();
                                } catch(err) {
                                    console.error('Erro ao recarregar stream:', err);
                                }
                            }, 1000);
                        }
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
        } // Fim de setupHLSEvents()
        
        // Forçar tentativa de reprodução após alguns segundos se ainda estiver pausado
        let playAttemptInterval = null;
        let playAttemptCount = 0;
        const maxPlayAttempts = 10; // Limitar tentativas
        
        const attemptPlay = () => {
            playAttemptCount++;
            console.log(`Tentativa de reprodução #${playAttemptCount} - readyState: ${video.readyState}, paused: ${video.paused}, buffered: ${video.buffered.length}`);
            
            // Chrome precisa de readyState >= 3, Firefox pode funcionar com >= 2
            const minReadyState = isChrome ? 3 : 2;
            if (video.paused && video.readyState >= minReadyState && video.buffered.length > 0) {
                const bufferedEnd = video.buffered.end(video.buffered.length - 1);
                if (bufferedEnd > 0.5) { // Pelo menos 0.5s de buffer
                    console.log('Tentativa automática de reprodução - buffer:', bufferedEnd.toFixed(2), 's, readyState:', video.readyState);
                    const playPromise = video.play();
                    if (playPromise !== undefined) {
                        playPromise.then(() => {
                            console.log('Reprodução iniciada na tentativa automática #' + playAttemptCount);
                            updateStatus('Reproduzindo', 'status-playing');
                            if (playAttemptInterval) {
                                clearInterval(playAttemptInterval);
                                playAttemptInterval = null;
                            }
                        }).catch(err => {
                            console.log('Autoplay ainda bloqueado na tentativa #' + playAttemptCount + ':', err.name, err.message);
                            if (playAttemptCount >= maxPlayAttempts) {
                                console.log('Máximo de tentativas atingido, mostrando botão de play');
                                showPlayButton();
                                if (playAttemptInterval) {
                                    clearInterval(playAttemptInterval);
                                    playAttemptInterval = null;
                                }
                            }
                        });
                    }
                } else {
                    console.log('Buffer insuficiente para reprodução:', bufferedEnd.toFixed(2), 's');
                }
            } else {
                console.log('Condições não atendidas - readyState:', video.readyState, 'paused:', video.paused, 'buffered:', video.buffered.length);
            }
        };
        
        // Tentar reproduzir a cada 2 segundos se ainda estiver pausado
        setTimeout(() => {
            if (video.paused) {
                playAttemptInterval = setInterval(attemptPlay, 2000);
                console.log('Iniciado intervalo de tentativas de reprodução (máximo', maxPlayAttempts, 'tentativas)');
            }
        }, 3000);
        
        // Também tentar quando o usuário interagir com a página (permite autoplay)
        document.addEventListener('click', () => {
            const minReadyState = isChrome ? 3 : 2;
            if (video.paused && video.readyState >= minReadyState && video.buffered.length > 0) {
                console.log('Interação do usuário detectada, tentando reproduzir...');
                video.play().then(() => {
                    console.log('Reprodução iniciada após interação do usuário');
                    updateStatus('Reproduzindo', 'status-playing');
                }).catch(err => {
                    console.log('Erro ao reproduzir após interação:', err);
                });
            }
        }, { once: true });
        
        // Evento quando há dados suficientes para começar a reproduzir
        video.addEventListener('canplay', () => {
            console.log('canplay event - vídeo pode começar a reproduzir, readyState:', video.readyState);
            console.log('Buffered ranges:', video.buffered.length);
            if (video.buffered.length > 0) {
                console.log('Buffered start:', video.buffered.start(0), 'end:', video.buffered.end(0));
            }
            
            // Vídeo está pronto - atualizar overlay
            const minReadyState = isChrome ? 3 : 2;
            if (video.readyState >= minReadyState && video.buffered.length > 0) {
                if (video.paused) {
                    // Mostrar botão de play se pausado
                    showPlayButton();
                } else {
                    // Esconder overlay se reproduzindo
                    videoOverlay.classList.add('hidden');
                }
            }
            
            if (video.paused) {
                console.log('Tentando reproduzir no evento canplay...');
                const playPromise = video.play();
                if (playPromise !== undefined) {
                    playPromise.then(() => {
                        console.log('Reprodução iniciada após canplay');
                        updateStatus('Reproduzindo', 'status-playing');
                    }).catch(err => {
                        console.log('Autoplay bloqueado no canplay:', err.name, err.message);
                        updateStatus('Clique para reproduzir', 'status-paused');
                        showPlayButton();
                    });
                }
            }
        }, { once: false });
        
        video.addEventListener('canplaythrough', () => {
            console.log('canplaythrough event - vídeo pode reproduzir sem interrupção, readyState:', video.readyState);
            
            // Vídeo está completamente pronto - esconder overlay de carregamento
            if (video.paused) {
                // Mostrar botão de play se pausado
                showPlayButton();
            } else {
                // Esconder overlay se reproduzindo
                videoOverlay.classList.add('hidden');
            }
            
            if (video.paused) {
                console.log('Tentando reproduzir no evento canplaythrough...');
                const playPromise = video.play();
                if (playPromise !== undefined) {
                    playPromise.then(() => {
                        console.log('Reprodução iniciada após canplaythrough');
                        updateStatus('Reproduzindo', 'status-playing');
                    }).catch(err => {
                        console.log('Autoplay bloqueado no canplaythrough:', err.name, err.message);
                    });
                }
            }
        }, { once: false });
        
        // Verificação periódica para esconder overlay quando vídeo estiver pronto
        // Isso garante que o overlay seja escondido mesmo se os eventos não forem disparados
        const overlayCheckInterval = setInterval(() => {
            const minReadyState = isChrome ? 3 : 2;
            if (video.readyState >= minReadyState && video.buffered.length > 0) {
                if (video.paused) {
                    // Se pausado mas pronto, mostrar botão de play
                    if (!videoOverlay.classList.contains('hidden') || 
                        !videoOverlay.querySelector('button')) {
                        showPlayButton();
                    }
                } else {
                    // Se reproduzindo, esconder overlay
                    if (!videoOverlay.classList.contains('hidden')) {
                        videoOverlay.classList.add('hidden');
                    }
                }
                // Limpar intervalo se vídeo está pronto e reproduzindo
                if (!video.paused) {
                    clearInterval(overlayCheckInterval);
                }
            }
        }, 500); // Verificar a cada 500ms
        
        video.addEventListener('play', () => {
            // Sempre esconder overlay quando começar a reproduzir
            videoOverlay.classList.add('hidden');
            clearInterval(overlayCheckInterval);
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
