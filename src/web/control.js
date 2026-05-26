/**
 * RetroCapture Control Panel - Main Control Logic
 */

// Estado da aplicação
let appState = {
    source: { type: 0 },
    shader: { name: '' },
    shaderParameters: [],
    capture: { width: 0, height: 0, fps: 0 },
    image: { brightness: 0, contrast: 1, maintainAspect: false, fullscreen: false },
    streaming: {},
    recording: {},
    v4l2: { devices: [], controls: [], currentDevice: '' },
    ds: { devices: [], currentDevice: '' }, // DirectShow (Windows)
    platform: { platform: 'linux', availableSourceTypes: [] },
    status: { streamingActive: false, active: false, streamUrl: '', url: '', clientCount: 0 }
};

// Atualizar estado periodicamente
let statusUpdateInterval = null;

/**
 * Mostra uma mensagem de alerta
 */
function showAlert(message, type = 'info') {
    const alertContainer = document.getElementById('alertContainer');
    const alertId = 'alert-' + Date.now();
    const alert = document.createElement('div');
    alert.id = alertId;
    alert.className = `alert alert-${type} alert-dismissible fade show`;
    alert.innerHTML = `
        ${message}
        <button type="button" class="btn-close" data-bs-dismiss="alert"></button>
    `;
    alertContainer.appendChild(alert);

    // Remover após 5 segundos
    setTimeout(() => {
        const alertEl = document.getElementById(alertId);
        if (alertEl) {
            alertEl.remove();
        }
    }, 5000);
}

/**
 * Copia a URL do stream
 */
function copyStreamUrl() {
    const urlInput = document.getElementById('streamUrl') || { value: window.location.origin + '/stream' };
    const url = urlInput.value || window.location.origin + '/stream';
    
    navigator.clipboard.writeText(url).then(() => {
        showAlert('URL copied to clipboard!', 'success');
    }).catch(() => {
        showAlert('Failed to copy URL', 'danger');
    });
}

/**
 * Carrega informações da plataforma
 */
async function loadPlatform() {
    try {
        console.log('Loading platform info...');
        const platform = await api.getPlatform();
        console.log('Plataforma recebida:', platform);
        appState.platform = platform;
        
        // Atualizar UI baseado na plataforma
        updatePlatformUI();
    } catch (error) {
        console.error('Failed to load platform info:', error);
        // Usar padrão baseado na detecção do user agent se falhar
        const isWindows = navigator.platform.toLowerCase().includes('win') || 
                         navigator.userAgent.toLowerCase().includes('windows');
        appState.platform = { 
            platform: isWindows ? 'windows' : 'linux', 
            availableSourceTypes: isWindows ? [
                { value: 0, name: 'None' },
                { value: 2, name: 'DirectShow' }
            ] : [
                { value: 0, name: 'None' },
                { value: 1, name: 'V4L2' }
            ]
        };
        console.log('Usando fallback de plataforma:', appState.platform);
        updatePlatformUI();
    }
}

/**
 * Atualiza a UI baseado na plataforma detectada
 */
function updatePlatformUI() {
    // Ocultar aba de áudio no Windows
    const audioTab = document.getElementById('audio-tab');
    const audioTabPane = document.getElementById('audio');
    if (audioTab && audioTabPane) {
        if (appState.platform.platform === 'windows') {
            audioTab.style.display = 'none';
            audioTabPane.style.display = 'none';
        } else {
            audioTab.style.display = '';
            audioTabPane.style.display = '';
        }
    }
    
    const sourceTypeSelect = document.getElementById('sourceType');
    if (!sourceTypeSelect) {
        console.warn('sourceType select not found');
        return;
    }
    
    // Limpar opções existentes
    sourceTypeSelect.innerHTML = '';
    
    // Verificar se temos tipos de source disponíveis
    if (!appState.platform.availableSourceTypes || appState.platform.availableSourceTypes.length === 0) {
        console.warn('No source types available on this platform');
        const option = document.createElement('option');
        option.value = '0';
        option.textContent = 'Carregando...';
        sourceTypeSelect.appendChild(option);
        return;
    }
    
    // Adicionar opções baseadas na plataforma
    appState.platform.availableSourceTypes.forEach(type => {
        const option = document.createElement('option');
        option.value = type.value;
        option.textContent = type.name;
        sourceTypeSelect.appendChild(option);
    });
    
    console.log('Platform UI atualizada:', appState.platform.platform, 'com', appState.platform.availableSourceTypes.length, 'tipos de source');
}

/**
 * Carrega todos os dados da aplicação
 */
async function loadAllData() {
    try {
        // Carregar informações da plataforma primeiro
        await loadPlatform();
        
        // Carregar status
        await loadStatus();
        
        // Carregar source
        await loadSource();
        
        // Carregar shader e lista de shaders
        await loadShaderList();
        await loadShader();
        
        // Carregar image settings
        await loadImageSettings();
        
        // Carregar streaming settings
        await loadStreamingSettings();
        
        // Carregar recording settings
        await loadRecordingSettings();
    } catch (error) {
        console.error('Failed to load data:', error);
        showAlert('Failed to load data: ' + error.message, 'danger');
    }
}

/**
 * Carrega o status do streaming
 */
async function loadStatus() {
    try {
        const status = await api.getStatus();
        appState.status = status;
        
        // A API retorna 'streamingActive', não 'active'
        const isActive = status.streamingActive !== undefined ? status.streamingActive : status.active;
        
        document.getElementById('streamStatus').textContent = isActive ? 'Active' : 'Inactive';
        document.getElementById('streamStatusIcon').className = isActive
            ? 'bi bi-broadcast text-success'
            : 'bi bi-broadcast text-secondary';
        document.getElementById('clientCount').textContent = status.clientCount || 0;

        // Recording / Source status — fire-and-forget so a slow status
        // refresh doesn't block the streaming-status path.
        updateRecordingStatusBadge();
        updateSourceStatusBadge();
        
        // Atualizar URL do stream se disponível
        if (status.streamUrl) {
            const streamLink = document.getElementById('streamLink');
            if (streamLink) {
                streamLink.href = status.streamUrl;
            }
        }
        
        // Atualizar botão de streaming com informações de cooldown
        const canStart = status.streamingCanStart !== undefined ? status.streamingCanStart : true;
        const cooldownMs = status.streamingCooldownRemainingMs !== undefined ? status.streamingCooldownRemainingMs : 0;
        updateStreamingButton(isActive, canStart, cooldownMs);
        
        // Sincronizar configurações de imagem se disponíveis no status
        if (status.image) {
            const maintainAspectEl = document.getElementById('maintainAspect');
            if (maintainAspectEl && status.image.maintainAspect !== undefined) {
                // Só atualizar se o valor mudou para evitar flicker
                if (maintainAspectEl.checked !== status.image.maintainAspect) {
                    maintainAspectEl.checked = status.image.maintainAspect;
                    // Atualizar appState
                    if (appState.image) {
                        appState.image.maintainAspect = status.image.maintainAspect;
                    }
                }
            }
        }
    } catch (error) {
        console.error('Failed to load status:', error);
    }
}

/**
 * Atualiza o botão de iniciar/parar streaming
 */
function updateStreamingButton(isActive, canStart, cooldownMs) {
    const btn = document.getElementById('streamingStartStopBtn');
    const text = document.getElementById('streamingStartStopText');
    
    if (!btn || !text) return;
    
    if (isActive) {
        btn.className = 'btn btn-danger btn-lg w-100';
        btn.disabled = false;
        btn.style.pointerEvents = 'auto';
        btn.style.cursor = 'pointer';
        text.innerHTML = '<i class="bi bi-stop-circle me-2"></i>Parar Streaming';
        btn.title = '';
    } else {
        if (cooldownMs > 0 || !canStart) {
            // Em cooldown - desabilitar botão completamente e mostrar tempo restante
            btn.className = 'btn btn-secondary btn-lg w-100';
            btn.disabled = true;
            btn.style.pointerEvents = 'none'; // Garantir que não é clicável
            btn.style.cursor = 'not-allowed';
            const cooldownSeconds = Math.ceil((cooldownMs || 0) / 1000);
            text.innerHTML = '<i class="bi bi-clock me-2"></i>Waiting (' + cooldownSeconds + 's)';
            btn.title = 'Wait for the cooldown to end before starting streaming again';
        } else {
            // Pode iniciar
            btn.className = 'btn btn-primary btn-lg w-100';
            btn.disabled = false;
            btn.style.pointerEvents = 'auto';
            btn.style.cursor = 'pointer';
            text.innerHTML = '<i class="bi bi-broadcast me-2"></i>Iniciar Streaming';
            btn.title = '';
        }
    }
}

/**
 * Alterna o streaming (inicia ou para)
 */
async function toggleStreaming() {
    try {
        const status = await api.getStatus();
        const isActive = status.streamingActive !== undefined ? status.streamingActive : status.active;
        const action = isActive ? 'stop' : 'start';
        
        // Verificar cooldown antes de tentar iniciar
        if (action === 'start' && !status.streamingCanStart) {
            const cooldownMs = status.streamingCooldownRemainingMs || 0;
            const cooldownSeconds = Math.ceil(cooldownMs / 1000);
            showAlert('Streaming still in cooldown. Wait ' + cooldownSeconds + ' seconds before starting again.', 'warning');
            await loadStatus(); // Recarregar status para atualizar o botão
            return;
        }
        
        const btn = document.getElementById('streamingStartStopBtn');
        const text = document.getElementById('streamingStartStopText');
        if (btn) {
            btn.disabled = true;
            if (text) {
                text.innerHTML = '<span class="spinner-border spinner-border-sm me-2"></span>Processando...';
            }
        }
        
        const result = await api.setStreamingControl(action);
        
        if (result.success) {
            // Aguardar um pouco e recarregar o status para atualizar a UI
            setTimeout(async () => {
                await loadStatus();
                const btn = document.getElementById('streamingStartStopBtn');
                if (btn) {
                    btn.disabled = false;
                }
            }, 1000);
        } else {
            throw new Error(result.message || 'Failed to ' + (action === 'start' ? 'start' : 'stop') + ' streaming');
        }
    } catch (error) {
        console.error('Failed to toggle streaming:', error);
        
        // Verificar se o erro é de cooldown
        if (error.message && error.message.includes('cooldown')) {
            showAlert(error.message, 'warning');
        } else {
            const status = await api.getStatus();
            const isActive = status.streamingActive !== undefined ? status.streamingActive : status.active;
            const action = isActive ? 'stop' : 'start';
            showAlert('Failed to ' + (action === 'start' ? 'start' : 'stop') + ' streaming: ' + error.message, 'danger');
        }
        
        const btn = document.getElementById('streamingStartStopBtn');
        if (btn) {
            btn.disabled = false;
        }
        
        // Recarregar status para restaurar o estado do botão
        await loadStatus();
    }
}

/**
 * Carrega informações da fonte
 */
async function loadSource() {
    try {
        const source = await api.getSource();
        appState.source = source;
        appState.v4l2.currentDevice = source.device || '';
        appState.ds.currentDevice = source.device || '';
        
        const sourceType = source.type || 0;
        const sourceTypeSelect = document.getElementById('sourceType');
        if (sourceTypeSelect) {
            // Verificar se o valor existe nas opções antes de definir
            let optionExists = false;
            for (let i = 0; i < sourceTypeSelect.options.length; i++) {
                if (parseInt(sourceTypeSelect.options[i].value) === sourceType) {
                    optionExists = true;
                    break;
                }
            }
            if (optionExists) {
                sourceTypeSelect.value = sourceType;
            } else {
                console.warn('Source type', sourceType, 'not found in options. Available options:', 
                    Array.from(sourceTypeSelect.options).map(opt => opt.value + '=' + opt.textContent));
            }
        }
        
        // Atualizar visibilidade dos controles baseado no tipo de fonte
        updateSourceUI(sourceType);
        
        // Carregar dispositivos e controles baseado no tipo de source
        if (sourceType === 1) {
            // V4L2 (Linux)
            await loadV4L2DevicesForSource();
            await loadV4L2ControlsForSource();
            
            // Se houver dispositivo, carregar informações de captura
            if (source.device) {
                const captureInfo = document.getElementById('v4l2CaptureInfo');
                if (captureInfo) {
                    captureInfo.style.display = 'block';
                }
                // Carregar valores de captura
                await loadCapture();
            }
        } else if (sourceType === 2) {
            // DirectShow (Windows)
            await loadDSDevicesForSource();
            
            // Se houver dispositivo, carregar informações de captura
            if (source.device) {
                const captureInfo = document.getElementById('dsCaptureInfo');
                if (captureInfo) {
                    captureInfo.style.display = 'block';
                }
                // Carregar valores de captura
                await loadCapture();
            }
        }
    } catch (error) {
        console.error('Failed to load source:', error);
    }
}

/**
 * Atualiza a UI baseado no tipo de fonte selecionado
 */
function updateSourceUI(sourceType) {
    const v4l2Container = document.getElementById('v4l2ControlsContainer');
    const dsContainer = document.getElementById('dsControlsContainer');
    const noneMessage = document.getElementById('noneSourceMessage');

    if (v4l2Container) v4l2Container.style.display = 'none';
    if (dsContainer) dsContainer.style.display = 'none';
    if (noneMessage) noneMessage.style.display = 'none';

    if (sourceType === 1) {
        // V4L2 selecionado (Linux)
        if (v4l2Container) v4l2Container.style.display = 'block';
    } else if (sourceType === 2) {
        // DirectShow selecionado (Windows)
        if (dsContainer) dsContainer.style.display = 'block';
        // Carregar dispositivos DirectShow
        loadDSDevicesForSource();
    } else {
        // None selecionado
        if (noneMessage) noneMessage.style.display = 'block';
    }
}

/**
 * Carrega dispositivos V4L2 para a aba Source
 */
async function loadV4L2DevicesForSource() {
    try {
        const response = await api.getV4L2Devices();
        const devices = response.devices || [];
        
        const select = document.getElementById('v4l2Device');
        if (!select) return;
        
        select.innerHTML = '';
        
        // Adicionar "None (No device)" como primeira opção
        const noneOption = document.createElement('option');
        noneOption.value = '';
        noneOption.textContent = 'None (No device)';
        select.appendChild(noneOption);
        
        // Adicionar dispositivos
        devices.forEach(device => {
            const option = document.createElement('option');
            option.value = device;
            option.textContent = device;
            select.appendChild(option);
        });
        
        // Marcar dispositivo atual se disponível (vem do source)
        const currentDevice = appState.source.device || '';
        appState.v4l2.currentDevice = currentDevice;
        for (let i = 0; i < select.options.length; i++) {
            if (select.options[i].value === currentDevice) {
                select.selectedIndex = i;
                break;
            }
        }
        
        // Mostrar/esconder informações de captura baseado se há dispositivo
        const captureInfo = document.getElementById('v4l2CaptureInfo');
        if (captureInfo) {
            captureInfo.style.display = currentDevice ? 'block' : 'none';
        }
    } catch (error) {
        console.error('Failed to load V4L2 devices:', error);
    }
}

/**
 * Carrega controles V4L2 para a aba Source
 */
async function loadV4L2ControlsForSource() {
    try {
        const response = await api.getV4L2Controls();
        const controls = response.controls || [];
        
        const container = document.getElementById('v4l2ControlsInSource');
        if (!container) return;
        
        container.innerHTML = '';
        
        if (controls.length === 0) {
            container.innerHTML = '<div class="col-12 text-muted">No controls available</div>';
            return;
        }
        
        controls.forEach(control => {
            if (!control.available) return;
            
            const col = document.createElement('div');
            col.className = 'col-md-6';
            
            const controlId = `v4l2ControlSource_${control.name.replace(/'/g, "\\'")}`;
            const valueId = `v4l2ControlValueSource_${control.name.replace(/'/g, "\\'")}`;
            
            col.innerHTML = `
                <label class="form-label">${control.name}</label>
                <div class="input-group">
                    <input type="range" class="form-range" 
                           id="${controlId}" 
                           min="${control.min}" 
                           max="${control.max}" 
                           step="${control.step || 1}" 
                           value="${control.value}">
                    <span class="input-group-text" style="min-width: 80px;" id="${valueId}">${control.value}</span>
                </div>
            `;
            container.appendChild(col);
            
            // Adicionar event listener após criar o elemento
            const slider = document.getElementById(controlId);
            if (slider) {
                // Armazenar timeout por controle usando data attribute
                let timeout = null;
                slider.addEventListener('input', function() {
                    const value = this.value;
                    // Atualizar valor visual imediatamente
                    const valueEl = document.getElementById(valueId);
                    if (valueEl) {
                        valueEl.textContent = parseInt(value);
                    }
                    // Debounce para atualizar na API
                    if (timeout) {
                        clearTimeout(timeout);
                    }
                    timeout = setTimeout(() => {
                        updateV4L2ControlSource(control.name, value);
                        timeout = null;
                    }, 500);
                });
            }
        });
    } catch (error) {
        console.error('Failed to load V4L2 controls:', error);
    }
}

/**
 * Atualiza um controle V4L2 na aba Source
 */
async function updateV4L2ControlSource(name, value) {
    try {
        await api.setV4L2Control(name, parseInt(value));
    } catch (error) {
        showAlert(`Failed to atualizar controle ${name}: ` + error.message, 'danger');
    }
}

/**
 * Carrega dispositivos DirectShow para a aba Source
 */
async function loadDSDevicesForSource() {
    try {
        const response = await api.getDSDevices();
        const devices = response.devices || [];
        
        const select = document.getElementById('dsDevice');
        if (!select) return;
        
        select.innerHTML = '';
        
        // Adicionar opção "None"
        const noneOption = document.createElement('option');
        noneOption.value = '';
        noneOption.textContent = 'None (No device)';
        select.appendChild(noneOption);
        
        // Adicionar dispositivos
        devices.forEach(device => {
            const option = document.createElement('option');
            option.value = device.id || device.name;
            option.textContent = device.name || device.id;
            if (!device.available) {
                option.disabled = true;
                option.textContent += ' (Not available)';
            }
            select.appendChild(option);
        });
        
        // Marcar dispositivo atual se disponível
        const currentDevice = appState.source.device || '';
        appState.ds.currentDevice = currentDevice;
        if (currentDevice) {
            for (let i = 0; i < select.options.length; i++) {
                if (select.options[i].value === currentDevice) {
                    select.selectedIndex = i;
                    break;
                }
            }
        }
        
        // Mostrar informações de captura se dispositivo estiver selecionado
        const captureInfo = document.getElementById('dsCaptureInfo');
        if (captureInfo && currentDevice) {
            captureInfo.style.display = 'block';
        }
    } catch (error) {
        console.error('Failed to load DirectShow devices:', error);
        showAlert('Failed to load DirectShow devices: ' + error.message, 'danger');
    }
}

/**
 * Atualiza o dispositivo DirectShow na aba Source
 */
async function updateDSDeviceSource() {
    try {
        const select = document.getElementById('dsDevice');
        if (!select) return;
        
        const device = select.value;
        await api.setDSDevice(device);
        appState.ds.currentDevice = device;
        
        // Recarregar informações de captura
        await loadCapture();
        
        // Mostrar/esconder informações de captura
        const captureInfo = document.getElementById('mfCaptureInfo');
        if (captureInfo) {
            captureInfo.style.display = device ? 'block' : 'none';
        }
        
        showAlert('Dispositivo DirectShow atualizado', 'success');
    } catch (error) {
        console.error('Failed to update DirectShow device:', error);
        showAlert('Failed to update DirectShow device: ' + error.message, 'danger');
    }
}

/**
 * Atualiza a lista de dispositivos DirectShow
 */
async function refreshDSDevices() {
    try {
        await api.refreshDSDevices();
        await loadDSDevicesForSource();
        showAlert('Lista de dispositivos DirectShow atualizada', 'success');
    } catch (error) {
        console.error('Failed to refresh DirectShow device list:', error);
        showAlert('Failed to refresh list: ' + error.message, 'danger');
    }
}

/**
 * Atualiza o dispositivo V4L2 na aba Source
 */
async function updateV4L2DeviceSource() {
    try {
        const select = document.getElementById('v4l2Device');
        const device = select ? select.value : '';
        
        await api.setV4L2Device(device);
        appState.v4l2.currentDevice = device;
        
        // Recarregar controles e informações de captura
        await loadV4L2ControlsForSource();
        
        // Mostrar/esconder informações de captura baseado se há dispositivo
        const captureInfo = document.getElementById('v4l2CaptureInfo');
        if (captureInfo) {
            captureInfo.style.display = device ? 'block' : 'none';
        }
        
        if (device) {
            // Carregar informações de captura
            await loadCapture();
        }
        
        // Atualização em tempo real - sem alerta
    } catch (error) {
        showAlert('Failed to update V4L2 device: ' + error.message, 'danger');
    }
}

/**
 * Define FPS rápido
 */
async function setQuickFPS(fps) {
    try {
        // Atualizar input baseado no tipo de source
        const sourceType = appState.source.type || 0;
        let fpsInput = null;
        if (sourceType === 1) {
            fpsInput = document.getElementById('v4l2CaptureFPS');
        } else if (sourceType === 2) {
            fpsInput = document.getElementById('dsCaptureFPS');
        }
        if (fpsInput) {
            fpsInput.value = fps;
        }
        await api.setCaptureFPS(fps);
        // Atualizar status bar
        const captureFPSEl = document.getElementById('captureFPS');
        if (captureFPSEl) {
            captureFPSEl.textContent = fps;
        }
        showAlert(`FPS definido para ${fps}!`, 'success');
    } catch (error) {
        showAlert('Failed to set FPS: ' + error.message, 'danger');
    }
}

/**
 * Define resolução rápida
 */
async function setQuickResolution(width, height) {
    try {
        // Atualizar inputs baseado no tipo de source
        const sourceType = appState.source.type || 0;
        let widthInput = null, heightInput = null;
        if (sourceType === 1) {
            widthInput = document.getElementById('v4l2CaptureWidth');
            heightInput = document.getElementById('v4l2CaptureHeight');
        } else if (sourceType === 2) {
            widthInput = document.getElementById('dsCaptureWidth');
            heightInput = document.getElementById('dsCaptureHeight');
        }
        if (widthInput) widthInput.value = width;
        if (heightInput) heightInput.value = height;
        
        await api.setCaptureResolution(width, height);
        // Atualizar status bar
        const captureResolutionEl = document.getElementById('captureResolution');
        if (captureResolutionEl) {
            captureResolutionEl.textContent = `${width}x${height}`;
        }
        showAlert(`Resolução definida para ${width}x${height}!`, 'success');
    } catch (error) {
        showAlert('Failed to set resolution: ' + error.message, 'danger');
    }
}

/**
 * Atualiza resolução/FPS de captura da aba Source
 */
async function updateV4L2CaptureSettings() {
    try {
        // Obter inputs baseado no tipo de source
        const sourceType = appState.source.type || 0;
        let widthInput = null, heightInput = null, fpsInput = null;
        
        if (sourceType === 1) {
            // V4L2
            widthInput = document.getElementById('v4l2CaptureWidth');
            heightInput = document.getElementById('v4l2CaptureHeight');
            fpsInput = document.getElementById('v4l2CaptureFPS');
        } else if (sourceType === 2) {
            // DirectShow
            widthInput = document.getElementById('dsCaptureWidth');
            heightInput = document.getElementById('dsCaptureHeight');
            fpsInput = document.getElementById('dsCaptureFPS');
        }

        if (!widthInput || !heightInput || !fpsInput) {
            // Não mostrar erro se não houver campos (pode ser que o tipo de source não tenha esses campos)
            return;
        }

        const width = parseInt(widthInput.value);
        const height = parseInt(heightInput.value);
        const fps = parseInt(fpsInput.value);

        if (width && height) {
            await api.setCaptureResolution(width, height);
        }

        if (fps) {
            await api.setCaptureFPS(fps);
        }
        
        // Atualizar status bar
        const captureResolutionEl = document.getElementById('captureResolution');
        const captureFPSEl = document.getElementById('captureFPS');
        if (captureResolutionEl) {
            captureResolutionEl.textContent = `${width}x${height}`;
        }
        if (captureFPSEl) {
            captureFPSEl.textContent = `${fps} FPS`;
        }
    } catch (error) {
        showAlert('Failed to update configuration: ' + error.message, 'danger');
    }
}

/**
 * Atualiza a lista de dispositivos V4L2
 */
async function refreshV4L2Devices() {
    try {
        // Forçar refresh no servidor
        await api.refreshV4L2Devices();
        // Recarregar a lista
        await loadV4L2DevicesForSource();
        showAlert('Lista de dispositivos atualizada!', 'success');
    } catch (error) {
        showAlert('Failed to update devices: ' + error.message, 'danger');
    }
}

/**
 * Atualiza a fonte
 */
async function updateSource() {
    try {
        const sourceType = parseInt(document.getElementById('sourceType').value);
        await api.setSource(sourceType);
        // Atualização em tempo real - sem alerta
        
        // Atualizar UI imediatamente
        updateSourceUI(sourceType);

        // Carregar dispositivos e controles baseado no tipo de source
        if (sourceType === 1) {
            // V4L2 (Linux)
            await loadV4L2DevicesForSource();
            await loadV4L2ControlsForSource();
        } else if (sourceType === 2) {
            // DirectShow (Windows)
            await loadDSDevicesForSource();
        }
        
        // Recarregar dados completos
        await loadSource();
    } catch (error) {
        showAlert('Failed to update source: ' + error.message, 'danger');
    }
}

/**
 * Carrega informações do shader
 */
async function loadShader() {
    try {
        const shader = await api.getShader();
        appState.shader = shader;

        const currentShaderName = shader.name || '';
        document.getElementById('currentShader').value = currentShaderName || 'Nenhum';

        const pipelineToggle = document.getElementById('shaderPipelineEnabled');
        if (pipelineToggle) pipelineToggle.checked = shader.pipelineEnabled !== false;

        // Atualizar seleção no dropdown
        const select = document.getElementById('shaderSelect');
        if (select) {
            // Marcar o shader atual como selecionado
            for (let i = 0; i < select.options.length; i++) {
                if (select.options[i].value === currentShaderName) {
                    select.selectedIndex = i;
                    break;
                }
            }
        }
        
        // Carregar parâmetros
        await loadShaderParameters();
    } catch (error) {
        console.error('Failed to load shader:', error);
    }
}

/**
 * Carrega a lista de shaders disponíveis
 */
async function loadShaderList() {
    try {
        const response = await api.getShaderList();
        const shaders = response.shaders || [];
        appState.shaderList = shaders;
        
        const select = document.getElementById('shaderSelect');
        if (!select) return;
        
        select.innerHTML = '';
        
        // Adicionar opção "None"
        const noneOption = document.createElement('option');
        noneOption.value = '';
        noneOption.textContent = 'None';
        select.appendChild(noneOption);
        
        // Adicionar shaders
        shaders.forEach(shader => {
            const option = document.createElement('option');
            option.value = shader;
            option.textContent = shader;
            select.appendChild(option);
        });
        
        // Marcar o shader atual como selecionado
        const currentShader = appState.shader.name || '';
        for (let i = 0; i < select.options.length; i++) {
            if (select.options[i].value === currentShader) {
                select.selectedIndex = i;
                break;
            }
        }
    } catch (error) {
        console.error('Failed to carregar lista de shaders:', error);
        showAlert('Failed to load shader list: ' + error.message, 'danger');
    }
}

/**
 * Carrega parâmetros do shader
 */
async function loadShaderParameters() {
    try {
        const response = await api.getShaderParameters();
        appState.shaderParameters = response.parameters || [];

        const container = document.getElementById('shaderParameters');
        container.innerHTML = '';

        if (appState.shaderParameters.length === 0) {
            container.innerHTML = '<div class="col-12 text-muted">Nenhum shader ativo ou sem parâmetros</div>';
            return;
        }

        appState.shaderParameters.forEach(param => {
            const safeName = param.name.replace(/'/g, "\\'");
            const col = document.createElement('div');
            col.className = 'col-md-6';
            const tooltip = `range ${Number(param.min).toFixed(2)} – ${Number(param.max).toFixed(2)}, default ${Number(param.defaultValue).toFixed(2)}`;
            col.innerHTML = `
                <div class="d-flex justify-content-between align-items-baseline">
                    <label class="form-label mb-1" for="shaderParam_${param.name}" title="${escapeHtml(tooltip)}">${param.name}${param.description ? ': ' + param.description : ''}</label>
                    <button type="button" class="btn btn-link btn-sm p-0 text-decoration-none" title="Reset to default" onclick="resetShaderParameter('${safeName}', ${param.defaultValue})"><i class="bi bi-arrow-counterclockwise small"></i></button>
                </div>
                <div class="input-group">
                    <input type="range" class="form-range"
                           id="shaderParam_${param.name}"
                           min="${param.min}"
                           max="${param.max}"
                           step="${param.step || 0.01}"
                           value="${param.value}"
                           title="${escapeHtml(tooltip)}"
                           oninput="onShaderParameterInput('${safeName}', this.value)">
                    <span class="input-group-text" style="min-width: 80px;" id="shaderParamValue_${param.name}">${Number(param.value).toFixed(2)}</span>
                </div>
            `;
            container.appendChild(col);
        });
    } catch (error) {
        console.error('Failed to load shader parameters:', error);
    }
}

// Throttle map for live shader parameter updates — keyed by parameter name.
// Each entry holds a pending value + a timer id; we send at most once per
// throttle window to keep the encoder loop free of API request floods.
const _shaderParamThrottle = {};
const SHADER_PARAM_THROTTLE_MS = 40; // ~25 updates/sec — feels live, easy on the server

function onShaderParameterInput(name, value) {
    const display = document.getElementById('shaderParamValue_' + name);
    if (display) display.textContent = Number(value).toFixed(2);

    const entry = _shaderParamThrottle[name] || (_shaderParamThrottle[name] = { pending: null, timer: null });
    entry.pending = value;
    if (entry.timer) return;
    entry.timer = setTimeout(async () => {
        const v = entry.pending;
        entry.timer = null;
        entry.pending = null;
        try {
            await api.setShaderParameter(name, parseFloat(v));
        } catch (err) {
            console.error('Failed to set shader parameter ' + name + ':', err);
        }
    }, SHADER_PARAM_THROTTLE_MS);
}

function resetShaderParameter(name, defaultValue) {
    const slider = document.getElementById('shaderParam_' + name);
    if (slider) slider.value = defaultValue;
    onShaderParameterInput(name, defaultValue);
}

// Backwards-compat alias for any inline onchange attributes that may still
// reference these names elsewhere.
function updateShaderParameterValue(name, value) {
    onShaderParameterInput(name, value);
}
async function updateShaderParameter(name, value) {
    try {
        await api.setShaderParameter(name, parseFloat(value));
    } catch (error) {
        console.error('Failed to set shader parameter ' + name + ':', error);
    }
}

/**
 * Atualiza o shader
 */
async function updateShader() {
    try {
        const select = document.getElementById('shaderSelect');
        const shaderName = select ? select.value.trim() : '';
        
        await api.setShader(shaderName);
        // Atualizar shader atual e parâmetros
        await loadShader();
    } catch (error) {
        console.error('Failed to update shader:', error);
        // Não mostrar alerta para atualizações em tempo real
    }
}

/**
 * Carrega configurações de captura
 */
async function loadCapture() {
    try {
        const resolution = await api.getCaptureResolution();
        const fps = await api.getCaptureFPS();
        
        appState.capture = {
            width: resolution.width || 0,
            height: resolution.height || 0,
            fps: fps.fps || 0
        };
        
        // Atualizar campos na aba Source (V4L2 e MF)
        const v4l2Width = document.getElementById('v4l2CaptureWidth');
        const v4l2Height = document.getElementById('v4l2CaptureHeight');
        const v4l2FPS = document.getElementById('v4l2CaptureFPS');
        const dsWidth = document.getElementById('dsCaptureWidth');
        const dsHeight = document.getElementById('dsCaptureHeight');
        const dsFPS = document.getElementById('dsCaptureFPS');
        
        if (v4l2Width) v4l2Width.value = appState.capture.width;
        if (v4l2Height) v4l2Height.value = appState.capture.height;
        if (v4l2FPS) v4l2FPS.value = appState.capture.fps;
        if (dsWidth) dsWidth.value = appState.capture.width;
        if (dsHeight) dsHeight.value = appState.capture.height;
        if (dsFPS) dsFPS.value = appState.capture.fps;
        
        // Atualizar status bar
        const captureResolutionEl = document.getElementById('captureResolution');
        const captureFPSEl = document.getElementById('captureFPS');
        
        if (captureResolutionEl) {
            captureResolutionEl.textContent = `${appState.capture.width}x${appState.capture.height}`;
        }
        if (captureFPSEl) {
            captureFPSEl.textContent = appState.capture.fps;
        }
    } catch (error) {
        console.error('Failed to load capture configuration:', error);
    }
}

/**
 * Carrega configurações de imagem
 */
async function loadImageSettings() {
    try {
        const settings = await api.getImageSettings();
        appState.image = settings;
        
        const brightnessEl = document.getElementById('brightness');
        const contrastEl = document.getElementById('contrast');
        const maintainAspectEl = document.getElementById('maintainAspect');
        const fullscreenEl = document.getElementById('fullscreen');
        const monitorIndexEl = document.getElementById('monitorIndex');
        const outputWidthEl = document.getElementById('outputWidth');
        const outputHeightEl = document.getElementById('outputHeight');
        
        if (brightnessEl) {
            brightnessEl.value = settings.brightness || 0;
            const brightnessValueEl = document.getElementById('brightnessValue');
            if (brightnessValueEl) {
                brightnessValueEl.textContent = (settings.brightness || 0).toFixed(2);
            }
        }
        
        if (contrastEl) {
            contrastEl.value = settings.contrast || 1;
            const contrastValueEl = document.getElementById('contrastValue');
            if (contrastValueEl) {
                contrastValueEl.textContent = (settings.contrast || 1).toFixed(2);
            }
        }
        
        if (maintainAspectEl) {
            maintainAspectEl.checked = settings.maintainAspect || false;
        }
        
        if (fullscreenEl) {
            fullscreenEl.checked = settings.fullscreen || false;
        }
        
        if (monitorIndexEl) {
            monitorIndexEl.value = settings.monitorIndex !== undefined ? settings.monitorIndex : -1;
        }
        
        if (outputWidthEl) {
            outputWidthEl.value = settings.outputWidth !== undefined ? settings.outputWidth : 0;
            const outputWidthValueEl = document.getElementById('outputWidthValue');
            if (outputWidthValueEl) {
                outputWidthValueEl.textContent = (settings.outputWidth || 0) + (settings.outputWidth === 0 ? ' (auto)' : '');
            }
        }
        
        if (outputHeightEl) {
            outputHeightEl.value = settings.outputHeight !== undefined ? settings.outputHeight : 0;
            const outputHeightValueEl = document.getElementById('outputHeightValue');
            if (outputHeightValueEl) {
                outputHeightValueEl.textContent = (settings.outputHeight || 0) + (settings.outputHeight === 0 ? ' (auto)' : '');
            }
        }
    } catch (error) {
        console.error('Failed to load image configuration:', error);
    }
}

/**
 * Define resolução de saída (helper function)
 */
function setOutputResolution(width, height) {
    const outputWidthEl = document.getElementById('outputWidth');
    const outputHeightEl = document.getElementById('outputHeight');
    if (outputWidthEl) outputWidthEl.value = width;
    if (outputHeightEl) outputHeightEl.value = height;
    
    const outputWidthValueEl = document.getElementById('outputWidthValue');
    const outputHeightValueEl = document.getElementById('outputHeightValue');
    if (outputWidthValueEl) {
        outputWidthValueEl.textContent = width + (width === 0 ? ' (auto)' : '');
    }
    if (outputHeightValueEl) {
        outputHeightValueEl.textContent = height + (height === 0 ? ' (auto)' : '');
    }
    
    updateImageSettings();
}

/**
 * Atualiza configurações de imagem
 */
async function updateImageSettings() {
    try {
        const brightnessEl = document.getElementById('brightness');
        const contrastEl = document.getElementById('contrast');
        const maintainAspectEl = document.getElementById('maintainAspect');
        const fullscreenEl = document.getElementById('fullscreen');
        const monitorIndexEl = document.getElementById('monitorIndex');
        const outputWidthEl = document.getElementById('outputWidth');
        const outputHeightEl = document.getElementById('outputHeight');
        
        const settings = {
            brightness: brightnessEl ? parseFloat(brightnessEl.value) : 0,
            contrast: contrastEl ? parseFloat(contrastEl.value) : 1,
            maintainAspect: maintainAspectEl ? maintainAspectEl.checked : false,
            fullscreen: fullscreenEl ? fullscreenEl.checked : false,
            monitorIndex: monitorIndexEl ? parseInt(monitorIndexEl.value) : -1,
            outputWidth: outputWidthEl ? parseInt(outputWidthEl.value) : 0,
            outputHeight: outputHeightEl ? parseInt(outputHeightEl.value) : 0
        };
        
        await api.setImageSettings(settings);
        
        // Atualizar appState para manter sincronizado
        appState.image = {
            brightness: settings.brightness,
            contrast: settings.contrast,
            maintainAspect: settings.maintainAspect,
            fullscreen: settings.fullscreen
        };
        
        // Não recarregar para evitar flicker - atualização em tempo real
    } catch (error) {
        console.error('Failed to update image configuration:', error);
        // Não mostrar alerta para atualizações em tempo real
    }
}

/**
 * Atualiza controles específicos do codec
 */
function updateCodecSpecificControls(codec) {
    const h264PresetContainer = document.getElementById('h264PresetContainer');
    const h265PresetContainer = document.getElementById('h265PresetContainer');
    const h265ProfileContainer = document.getElementById('h265ProfileContainer');
    const h265LevelContainer = document.getElementById('h265LevelContainer');
    const vp8SpeedContainer = document.getElementById('vp8SpeedContainer');
    const vp9SpeedContainer = document.getElementById('vp9SpeedContainer');
    
    if (h264PresetContainer) h264PresetContainer.style.display = 'none';
    if (h265PresetContainer) h265PresetContainer.style.display = 'none';
    if (h265ProfileContainer) h265ProfileContainer.style.display = 'none';
    if (h265LevelContainer) h265LevelContainer.style.display = 'none';
    if (vp8SpeedContainer) vp8SpeedContainer.style.display = 'none';
    if (vp9SpeedContainer) vp9SpeedContainer.style.display = 'none';
    
    // Mostrar controles específicos do codec
    if (codec === 'h264' && h264PresetContainer) {
        h264PresetContainer.style.display = 'block';
    } else if (codec === 'h265') {
        if (h265PresetContainer) h265PresetContainer.style.display = 'block';
        if (h265ProfileContainer) h265ProfileContainer.style.display = 'block';
        if (h265LevelContainer) h265LevelContainer.style.display = 'block';
    } else if (codec === 'vp8' && vp8SpeedContainer) {
        vp8SpeedContainer.style.display = 'block';
    } else if (codec === 'vp9' && vp9SpeedContainer) {
        vp9SpeedContainer.style.display = 'block';
    }
}

/**
 * Carrega configurações de streaming
 */
async function loadStreamingSettings() {
    try {
        const settings = await api.getStreamingSettings();
        appState.streaming = settings;
        
        const portEl = document.getElementById('streamingPort');
        if (portEl && settings.port) portEl.value = settings.port;

        const applyEl = document.getElementById('streamingApplyShader');
        if (applyEl) applyEl.checked = settings.applyShader !== false;

        const bitrateEl = document.getElementById('streamingBitrate');
        if (bitrateEl && settings.bitrate) bitrateEl.value = settings.bitrate;
        
        const audioBitrateEl = document.getElementById('streamingAudioBitrate');
        if (audioBitrateEl && settings.audioBitrate) audioBitrateEl.value = settings.audioBitrate;
        
        const widthEl = document.getElementById('streamingWidth');
        if (widthEl && settings.width) widthEl.value = settings.width;
        
        const heightEl = document.getElementById('streamingHeight');
        if (heightEl && settings.height) heightEl.value = settings.height;
        
        const fpsEl = document.getElementById('streamingFPS');
        if (fpsEl && settings.fps) fpsEl.value = settings.fps;
        const videoCodecEl = document.getElementById('streamingVideoCodec');
        if (videoCodecEl && settings.videoCodec) {
            videoCodecEl.value = settings.videoCodec;
            // Atualizar controles específicos do codec primeiro
            updateCodecSpecificControls(settings.videoCodec);
        }
        
        const audioCodecEl = document.getElementById('streamingAudioCodec');
        if (audioCodecEl && settings.audioCodec) {
            audioCodecEl.value = settings.audioCodec;
        }
        
        const h264PresetEl = document.getElementById('streamingH264Preset');
        if (h264PresetEl && settings.h264Preset) {
            h264PresetEl.value = settings.h264Preset;
        }
        
        const h265PresetEl = document.getElementById('streamingH265Preset');
        if (h265PresetEl && settings.h265Preset) {
            h265PresetEl.value = settings.h265Preset;
        }
        
        const h265ProfileEl = document.getElementById('streamingH265Profile');
        if (h265ProfileEl && settings.h265Profile) {
            h265ProfileEl.value = settings.h265Profile;
        }
        
        const h265LevelEl = document.getElementById('streamingH265Level');
        if (h265LevelEl && settings.h265Level) {
            h265LevelEl.value = settings.h265Level;
        }
        
        const vp8SpeedEl = document.getElementById('streamingVP8Speed');
        if (vp8SpeedEl && settings.vp8Speed !== undefined) {
            vp8SpeedEl.value = settings.vp8Speed;
        }
        
        const vp9SpeedEl = document.getElementById('streamingVP9Speed');
        if (vp9SpeedEl && settings.vp9Speed !== undefined) {
            vp9SpeedEl.value = settings.vp9Speed;
        }
        
        // Garantir que os controles específicos do codec estejam visíveis
        if (settings.videoCodec) {
            updateCodecSpecificControls(settings.videoCodec);
        }
    } catch (error) {
        console.error('Failed to load streaming configuration:', error);
    }
}

/**
 * Atualiza configurações de streaming
 */
async function updateStreamingSettings() {
    try {
        const videoCodecEl = document.getElementById('streamingVideoCodec');
        const portEl = document.getElementById('streamingPort');
        const bitrateEl = document.getElementById('streamingBitrate');
        const audioBitrateEl = document.getElementById('streamingAudioBitrate');
        const widthEl = document.getElementById('streamingWidth');
        const heightEl = document.getElementById('streamingHeight');
        const fpsEl = document.getElementById('streamingFPS');
        const audioCodecEl = document.getElementById('streamingAudioCodec');
        
        if (!videoCodecEl || !portEl || !bitrateEl || !audioBitrateEl || !widthEl || !heightEl || !fpsEl || !audioCodecEl) {
            showAlert('Error: form elements not found', 'danger');
            return;
        }
        
        const videoCodec = videoCodecEl.value;
        const settings = {
            port: parseInt(portEl.value),
            bitrate: parseInt(bitrateEl.value),
            audioBitrate: parseInt(audioBitrateEl.value),
            width: parseInt(widthEl.value),
            height: parseInt(heightEl.value),
            fps: parseInt(fpsEl.value),
            videoCodec: videoCodec,
            audioCodec: audioCodecEl.value
        };
        
        // Adicionar configurações específicas do codec
        if (videoCodec === 'h264') {
            const h264PresetEl = document.getElementById('streamingH264Preset');
            if (h264PresetEl) settings.h264Preset = h264PresetEl.value;
        } else if (videoCodec === 'h265') {
            const h265PresetEl = document.getElementById('streamingH265Preset');
            const h265ProfileEl = document.getElementById('streamingH265Profile');
            const h265LevelEl = document.getElementById('streamingH265Level');
            if (h265PresetEl) settings.h265Preset = h265PresetEl.value;
            if (h265ProfileEl) settings.h265Profile = h265ProfileEl.value;
            if (h265LevelEl) settings.h265Level = h265LevelEl.value;
        } else if (videoCodec === 'vp8') {
            const vp8SpeedEl = document.getElementById('streamingVP8Speed');
            if (vp8SpeedEl) settings.vp8Speed = parseInt(vp8SpeedEl.value);
        } else if (videoCodec === 'vp9') {
            const vp9SpeedEl = document.getElementById('streamingVP9Speed');
            if (vp9SpeedEl) settings.vp9Speed = parseInt(vp9SpeedEl.value);
        }
        
        await api.setStreamingSettings(settings);
        // Não recarregar para evitar flicker - atualização em tempo real
    } catch (error) {
        console.error('Failed to update streaming configuration:', error);
        // Não mostrar alerta para atualizações em tempo real
    }
}

/**
 * Carrega dispositivos V4L2
 */
async function loadV4L2Devices() {
    try {
        const response = await api.getV4L2Devices();
        const devices = response.devices || [];
        appState.v4l2.devices = devices;
        
        const select = document.getElementById('v4l2Device');
        if (!select) return;
        
        select.innerHTML = '';
        
        if (devices.length === 0) {
            select.innerHTML = '<option value="">Nenhum dispositivo encontrado</option>';
            return;
        }
        
        devices.forEach(device => {
            const option = document.createElement('option');
            option.value = device;
            option.textContent = device;
            select.appendChild(option);
        });
        
        // Marcar dispositivo atual se disponível
        if (appState.v4l2.currentDevice) {
            for (let i = 0; i < select.options.length; i++) {
                if (select.options[i].value === appState.v4l2.currentDevice) {
                    select.selectedIndex = i;
                    break;
                }
            }
        }
    } catch (error) {
        console.error('Failed to load V4L2 devices:', error);
    }
}

/**
 * Atualiza o dispositivo V4L2
 */
async function updateV4L2Device() {
    try {
        const select = document.getElementById('v4l2Device');
        const device = select ? select.value : '';
        
        await api.setV4L2Device(device);
        appState.v4l2.currentDevice = device;
        // Atualização em tempo real - sem alerta
    } catch (error) {
        showAlert('Failed to update V4L2 device: ' + error.message, 'danger');
    }
}


// Event listeners
document.addEventListener('DOMContentLoaded', function() {
    // Atualizar URL do stream
    const currentUrl = window.location.origin + window.location.pathname.replace(/\/index\.html$/, '');
    const streamUrl = currentUrl + '/stream';
    const streamLink = document.getElementById('streamLink');
    if (streamLink) {
        streamLink.href = streamUrl;
    }
    
    // Carregar dados iniciais
    loadAllData();
    
    // Atualizar status a cada 2 segundos
    statusUpdateInterval = setInterval(() => {
        loadStatus();
        updateRecordingStatus();
    }, 2000);
    
    // Inicializar botão de streaming com estado atual
    setTimeout(() => {
        loadStatus();
        updateRecordingStatus();
    }, 500);
    
    // Event listeners para sliders
    document.getElementById('brightness').addEventListener('input', function() {
        document.getElementById('brightnessValue').textContent = parseFloat(this.value).toFixed(2);
    });
    
    document.getElementById('contrast').addEventListener('input', function() {
        document.getElementById('contrastValue').textContent = parseFloat(this.value).toFixed(2);
    });
    
    // Event listener para mudança de tipo de fonte - aplica automaticamente
    const sourceTypeSelect = document.getElementById('sourceType');
    if (sourceTypeSelect) {
        sourceTypeSelect.addEventListener('change', function() {
            const sourceType = parseInt(this.value);
            updateSource(); // Aplicar mudança automaticamente
        });
    } else {
        console.error('sourceType select not found when adding event listener');
    }
    
    // Event listener para mudança de dispositivo V4L2 na aba Source
    const v4l2DeviceSelect = document.getElementById('v4l2Device');
    if (v4l2DeviceSelect) {
        v4l2DeviceSelect.addEventListener('change', function() {
            updateV4L2DeviceSource();
        });
    }
    
    // Event listener para mudança de dispositivo DirectShow na aba Source
    const dsDeviceSelect = document.getElementById('dsDevice');
    if (dsDeviceSelect) {
        dsDeviceSelect.addEventListener('change', function() {
            updateDSDeviceSource();
        });
    }
    
    // Event listeners para controles V4L2 são adicionados dinamicamente em loadV4L2ControlsForSource
    
    // Event listener para mudança de codec de vídeo
    document.getElementById('streamingVideoCodec').addEventListener('change', function() {
        updateCodecSpecificControls(this.value);
    });
    
    // Event listeners para parâmetros de shader (debounce)
    let shaderParamTimeout = {};
    window.updateShaderParameterValue = function(name, value) {
        // Atualizar valor visual
        const valueEl = document.getElementById(`shaderParamValue_${name}`);
        if (valueEl) {
            valueEl.textContent = parseFloat(value).toFixed(2);
        }
        // Debounce para atualizar na API
        clearTimeout(shaderParamTimeout[name]);
        shaderParamTimeout[name] = setTimeout(() => {
            updateShaderParameter(name, value);
        }, 500);
    };
    
    // Resolução/FPS de captura (V4L2 e MF) - atualização em tempo real com debounce
    let captureSettingsTimeout = null;
    const v4l2CaptureWidth = document.getElementById('v4l2CaptureWidth');
    const v4l2CaptureHeight = document.getElementById('v4l2CaptureHeight');
    const v4l2CaptureFPS = document.getElementById('v4l2CaptureFPS');
    const dsCaptureWidth = document.getElementById('dsCaptureWidth');
    const dsCaptureHeight = document.getElementById('dsCaptureHeight');
    const dsCaptureFPS = document.getElementById('dsCaptureFPS');
    
    function updateCaptureSettingsDebounced() {
        clearTimeout(captureSettingsTimeout);
        captureSettingsTimeout = setTimeout(() => {
            updateV4L2CaptureSettings();
        }, 500);
    }
    
    if (v4l2CaptureWidth) {
        v4l2CaptureWidth.addEventListener('input', updateCaptureSettingsDebounced);
        v4l2CaptureWidth.addEventListener('change', updateCaptureSettingsDebounced);
    }
    if (v4l2CaptureHeight) {
        v4l2CaptureHeight.addEventListener('input', updateCaptureSettingsDebounced);
        v4l2CaptureHeight.addEventListener('change', updateCaptureSettingsDebounced);
    }
    if (v4l2CaptureFPS) {
        v4l2CaptureFPS.addEventListener('input', updateCaptureSettingsDebounced);
        v4l2CaptureFPS.addEventListener('change', updateCaptureSettingsDebounced);
    }
    
    // Event listeners para controles DirectShow (similar ao V4L2)
    if (dsCaptureWidth) {
        dsCaptureWidth.addEventListener('input', updateCaptureSettingsDebounced);
        dsCaptureWidth.addEventListener('change', updateCaptureSettingsDebounced);
    }
    if (dsCaptureHeight) {
        dsCaptureHeight.addEventListener('input', updateCaptureSettingsDebounced);
        dsCaptureHeight.addEventListener('change', updateCaptureSettingsDebounced);
    }
    if (dsCaptureFPS) {
        dsCaptureFPS.addEventListener('input', updateCaptureSettingsDebounced);
        dsCaptureFPS.addEventListener('change', updateCaptureSettingsDebounced);
    }
    
    // Shader select - atualização em tempo real
    const shaderSelect = document.getElementById('shaderSelect');
    if (shaderSelect) {
        shaderSelect.addEventListener('change', function() {
            updateShader();
        });
    }
    
    // Image settings - atualização em tempo real com debounce
    let imageSettingsTimeout = null;
    function updateImageSettingsDebounced() {
        clearTimeout(imageSettingsTimeout);
        imageSettingsTimeout = setTimeout(() => {
            updateImageSettings();
        }, 500);
    }
    
    const brightness = document.getElementById('brightness');
    const contrast = document.getElementById('contrast');
    const maintainAspect = document.getElementById('maintainAspect');
    const fullscreen = document.getElementById('fullscreen');
    const monitorIndex = document.getElementById('monitorIndex');
    
    if (brightness) {
        brightness.addEventListener('input', updateImageSettingsDebounced);
        brightness.addEventListener('change', updateImageSettingsDebounced);
    }
    if (contrast) {
        contrast.addEventListener('input', updateImageSettingsDebounced);
        contrast.addEventListener('change', updateImageSettingsDebounced);
    }
    if (maintainAspect) {
        maintainAspect.addEventListener('change', updateImageSettingsDebounced);
    }
    if (fullscreen) {
        fullscreen.addEventListener('change', updateImageSettingsDebounced);
    }
    if (monitorIndex) {
        monitorIndex.addEventListener('input', updateImageSettingsDebounced);
        monitorIndex.addEventListener('change', updateImageSettingsDebounced);
    }
    
    const outputWidth = document.getElementById('outputWidth');
    const outputHeight = document.getElementById('outputHeight');
    if (outputWidth) {
        outputWidth.addEventListener('input', function() {
            const outputWidthValueEl = document.getElementById('outputWidthValue');
            if (outputWidthValueEl) {
                outputWidthValueEl.textContent = outputWidth.value + (outputWidth.value == 0 ? ' (auto)' : '');
            }
            updateImageSettingsDebounced();
        });
    }
    if (outputHeight) {
        outputHeight.addEventListener('input', function() {
            const outputHeightValueEl = document.getElementById('outputHeightValue');
            if (outputHeightValueEl) {
                outputHeightValueEl.textContent = outputHeight.value + (outputHeight.value == 0 ? ' (auto)' : '');
            }
            updateImageSettingsDebounced();
        });
    }
    
    // Streaming settings - atualização em tempo real com debounce
    let streamingSettingsTimeout = null;
    function updateStreamingSettingsDebounced() {
        clearTimeout(streamingSettingsTimeout);
        streamingSettingsTimeout = setTimeout(() => {
            updateStreamingSettings();
        }, 500);
    }
    
    const streamingPort = document.getElementById('streamingPort');
    const streamingBitrate = document.getElementById('streamingBitrate');
    const streamingAudioBitrate = document.getElementById('streamingAudioBitrate');
    const streamingWidth = document.getElementById('streamingWidth');
    const streamingHeight = document.getElementById('streamingHeight');
    const streamingFPS = document.getElementById('streamingFPS');
    const streamingAudioCodec = document.getElementById('streamingAudioCodec');
    
    if (streamingPort) {
        streamingPort.addEventListener('input', updateStreamingSettingsDebounced);
        streamingPort.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (streamingBitrate) {
        streamingBitrate.addEventListener('input', updateStreamingSettingsDebounced);
        streamingBitrate.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (streamingAudioBitrate) {
        streamingAudioBitrate.addEventListener('input', updateStreamingSettingsDebounced);
        streamingAudioBitrate.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (streamingWidth) {
        streamingWidth.addEventListener('input', updateStreamingSettingsDebounced);
        streamingWidth.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (streamingHeight) {
        streamingHeight.addEventListener('input', updateStreamingSettingsDebounced);
        streamingHeight.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (streamingFPS) {
        streamingFPS.addEventListener('input', updateStreamingSettingsDebounced);
        streamingFPS.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (streamingVideoCodec) {
        streamingVideoCodec.addEventListener('change', function() {
            updateCodecSpecificControls(this.value);
            updateStreamingSettingsDebounced();
        });
    }
    if (streamingAudioCodec) {
        streamingAudioCodec.addEventListener('change', updateStreamingSettingsDebounced);
    }
    
    // Codec-specific controls - atualização em tempo real com debounce
    const h264Preset = document.getElementById('streamingH264Preset');
    const h265Preset = document.getElementById('streamingH265Preset');
    const h265Profile = document.getElementById('streamingH265Profile');
    const h265Level = document.getElementById('streamingH265Level');
    const vp8Speed = document.getElementById('streamingVP8Speed');
    const vp9Speed = document.getElementById('streamingVP9Speed');
    
    if (h264Preset) {
        h264Preset.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (h265Preset) {
        h265Preset.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (h265Profile) {
        h265Profile.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (h265Level) {
        h265Level.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (vp8Speed) {
        vp8Speed.addEventListener('input', updateStreamingSettingsDebounced);
        vp8Speed.addEventListener('change', updateStreamingSettingsDebounced);
    }
    if (vp9Speed) {
        vp9Speed.addEventListener('input', updateStreamingSettingsDebounced);
        vp9Speed.addEventListener('change', updateStreamingSettingsDebounced);
    }
    
    // Load presets when presets tab is shown
    const presetsTab = document.getElementById('presets-tab');
    if (presetsTab) {
        presetsTab.addEventListener('shown.bs.tab', function() {
            loadPresets();
        });
    }
    
});

// ========== Preset Functions ==========

/**
 * Load and display all presets
 */
async function loadPresets() {
    const grid = document.getElementById('presetsGrid');
    if (!grid) return;
    
    try {
        grid.innerHTML = '<div class="col-12 text-muted">Carregando presets...</div>';
        const response = await api.getPresets();
        
        if (!response.presets || response.presets.length === 0) {
            grid.innerHTML = '<div class="col-12 text-muted">No presets found. Create a new preset with the "Create Preset" button.</div>';
            return;
        }
        
        grid.innerHTML = '';
        response.presets.forEach(preset => {
            const presetCard = createPresetCard(preset);
            grid.appendChild(presetCard);
        });
    } catch (error) {
        console.error('Failed to load presets:', error);
        grid.innerHTML = `<div class="col-12 text-danger">Failed to carregar presets: ${error.message}</div>`;
    }
}

/**
 * Create a preset card element
 */
function createPresetCard(preset) {
    const col = document.createElement('div');
    col.className = 'col-md-4 col-lg-3';
    
    const card = document.createElement('div');
    card.className = 'card preset-card';
    card.style.cursor = 'pointer';
    card.onclick = function(e) {
        // Don't apply if clicking on buttons
        if (e.target.closest('button')) {
            return;
        }
        applyPreset(preset.name);
    };
    
    // Extract just the filename from thumbnail path
    let thumbnailFilename = '';
    if (preset.thumbnail) {
        // If thumbnail path contains directory separators, extract just the filename
        const parts = preset.thumbnail.split(/[/\\]/);
        thumbnailFilename = parts[parts.length - 1];
    }
    
    // Thumbnail - square aspect ratio
    let thumbnailHtml = '<div class="card-img-top preset-thumbnail bg-dark d-flex align-items-center justify-content-center"><i class="bi bi-image text-muted fs-1"></i></div>';
    if (thumbnailFilename) {
        // Thumbnail path is relative, need to construct full URL
        const thumbnailUrl = `/assets/thumbnails/${thumbnailFilename}`;
        thumbnailHtml = `<img src="${thumbnailUrl}" class="card-img-top preset-thumbnail" style="object-fit: cover;" onerror="this.onerror=null; this.parentElement.innerHTML='<div class=\\'card-img-top preset-thumbnail bg-dark d-flex align-items-center justify-content-center\\'><i class=\\'bi bi-image text-muted fs-1\\'></i></div>'">`;
    }
    
    card.innerHTML = `
        ${thumbnailHtml}
        <div class="card-body">
            <h6 class="card-title">${escapeHtml(preset.displayName || preset.name)}</h6>
            ${preset.description ? `<p class="card-text text-muted small">${escapeHtml(preset.description)}</p>` : ''}
        </div>
        <div class="card-footer bg-transparent">
            <div class="btn-group w-100" role="group">
                <button type="button" class="btn btn-sm btn-primary" onclick="event.stopPropagation(); applyPreset('${escapeHtml(preset.name)}')">
                    <i class="bi bi-play-circle me-1"></i>Apply
                </button>
                <button type="button" class="btn btn-sm btn-secondary" onclick="event.stopPropagation(); openEditPresetParams('${escapeHtml(preset.name)}')" title="Edit shader parameters">
                    <i class="bi bi-sliders"></i>
                </button>
                <button type="button" class="btn btn-sm btn-danger" onclick="event.stopPropagation(); deletePreset('${escapeHtml(preset.name)}')">
                    <i class="bi bi-trash"></i>
                </button>
            </div>
        </div>
    `;
    
    col.appendChild(card);
    return col;
}

/**
 * Escape HTML to prevent XSS
 */
function escapeHtml(text) {
    const div = document.createElement('div');
    div.textContent = text;
    return div.innerHTML;
}

/**
 * Refresh presets list
 */
async function refreshPresets() {
    await loadPresets();
    showAlert('Presets atualizados', 'success');
}

/**
 * Show create preset dialog
 */
function showCreatePresetDialog() {
    const modal = new bootstrap.Modal(document.getElementById('createPresetModal'));
    document.getElementById('newPresetName').value = '';
    document.getElementById('newPresetDescription').value = '';
    document.getElementById('captureThumbnail').checked = true;
    modal.show();
}

/**
 * Create a new preset from current state
 */
async function createPreset() {
    const name = document.getElementById('newPresetName').value.trim();
    if (!name) {
        showAlert('Por favor, insira um nome para o preset', 'warning');
        return;
    }
    
    const description = document.getElementById('newPresetDescription').value.trim();
    const captureThumbnail = document.getElementById('captureThumbnail').checked;
    
    try {
        // Note: thumbnail capture is handled server-side, so we just pass the flag
        await api.createPreset(name, description, captureThumbnail);
        
        const modal = bootstrap.Modal.getInstance(document.getElementById('createPresetModal'));
        modal.hide();
        
        showAlert(`Preset "${name}" criado com sucesso!`, 'success');
        await loadPresets();
    } catch (error) {
        console.error('Failed to criar preset:', error);
        showAlert(`Failed to criar preset: ${error.message}`, 'danger');
    }
}

/**
 * Apply a preset
 */
async function applyPreset(presetName) {
    try {
        await api.applyPreset(presetName);
        showAlert(`Preset "${presetName}" aplicado com sucesso!`, 'success');
        
        // Reload all data to reflect changes
        await loadAllData();
    } catch (error) {
        console.error('Failed to aplicar preset:', error);
        showAlert(`Failed to aplicar preset: ${error.message}`, 'danger');
    }
}

/**
 * Delete a preset
 */
async function deletePreset(presetName) {
    if (!confirm(`Tem certeza que deseja deletar o preset "${presetName}"?`)) {
        return;
    }
    
    try {
        await api.deletePreset(presetName);
        showAlert(`Preset "${presetName}" deletado com sucesso!`, 'success');
        await loadPresets();
    } catch (error) {
        console.error('Failed to deletar preset:', error);
        showAlert(`Failed to deletar preset: ${error.message}`, 'danger');
    }
}

/**
 * Carrega configurações de recording
 */
async function loadRecordingSettings() {
    try {
        const settings = await api.getRecordingSettings();
        appState.recording = settings;
        
        const widthEl = document.getElementById('recordingWidth');
        if (widthEl && settings.width) widthEl.value = settings.width;

        const heightEl = document.getElementById('recordingHeight');
        if (heightEl && settings.height) heightEl.value = settings.height;

        const fpsEl = document.getElementById('recordingFPS');
        if (fpsEl && settings.fps) fpsEl.value = settings.fps;

        const applyEl = document.getElementById('recordingApplyShader');
        if (applyEl) applyEl.checked = settings.applyShader !== false;

        const bitrateEl = document.getElementById('recordingBitrate');
        if (bitrateEl && settings.bitrate) bitrateEl.value = settings.bitrate;
        
        const audioBitrateEl = document.getElementById('recordingAudioBitrate');
        if (audioBitrateEl && settings.audioBitrate) audioBitrateEl.value = settings.audioBitrate;
        
        const videoCodecEl = document.getElementById('recordingVideoCodec');
        if (videoCodecEl && settings.codec) videoCodecEl.value = settings.codec;
        
        const audioCodecEl = document.getElementById('recordingAudioCodec');
        if (audioCodecEl && settings.audioCodec) audioCodecEl.value = settings.audioCodec;
        
        const containerEl = document.getElementById('recordingContainer');
        if (containerEl && settings.container) containerEl.value = settings.container;
        
        const outputPathEl = document.getElementById('recordingOutputPath');
        if (outputPathEl && settings.outputPath) outputPathEl.value = settings.outputPath;
        
        const filenameTemplateEl = document.getElementById('recordingFilenameTemplate');
        if (filenameTemplateEl && settings.filenameTemplate) filenameTemplateEl.value = settings.filenameTemplate;
        
        const includeAudioEl = document.getElementById('recordingIncludeAudio');
        if (includeAudioEl) includeAudioEl.checked = settings.includeAudio !== false;
        
        // Update status
        await updateRecordingStatus();
    } catch (error) {
        console.error('Failed to load recording configuration:', error);
    }
}

/**
 * Atualiza status de recording
 */
async function updateRecordingStatus() {
    try {
        const status = await api.getRecordingStatus();
        
        const statusAlert = document.getElementById('recordingStatusAlert');
        const statusIcon = document.getElementById('recordingStatusIcon');
        const statusText = document.getElementById('recordingStatusText');
        const statusInfo = document.getElementById('recordingStatusInfo');
        const btn = document.getElementById('recordingStartStopBtn');
        const btnText = document.getElementById('recordingStartStopText');
        
        if (status.isRecording) {
            if (statusAlert) {
                statusAlert.className = 'alert alert-danger';
            }
            if (statusIcon) {
                statusIcon.className = 'bi bi-circle-fill me-2 text-danger';
            }
            if (statusText) {
                statusText.textContent = 'Recording';
            }
            if (statusInfo) {
                const duration = status.duration || 0;
                const seconds = Math.floor(duration / 1000000);
                const minutes = Math.floor(seconds / 60);
                const hours = Math.floor(minutes / 60);
                const fileSize = status.fileSize || 0;
                const sizeMB = (fileSize / (1024 * 1024)).toFixed(2);
                statusInfo.textContent = `${String(hours).padStart(2, '0')}:${String(minutes % 60).padStart(2, '0')}:${String(seconds % 60).padStart(2, '0')} - ${sizeMB} MB`;
            }
            if (btn) {
                btn.className = 'btn btn-danger btn-lg w-100';
            }
            if (btnText) {
                btnText.innerHTML = '<i class="bi bi-stop-circle me-2"></i>Stop Recording';
            }
        } else {
            if (statusAlert) {
                statusAlert.className = 'alert alert-secondary';
            }
            if (statusIcon) {
                statusIcon.className = 'bi bi-circle-fill me-2 text-secondary';
            }
            if (statusText) {
                statusText.textContent = 'Stopped';
            }
            if (statusInfo) {
                statusInfo.textContent = '';
            }
            if (btn) {
                btn.className = 'btn btn-primary btn-lg w-100';
            }
            if (btnText) {
                btnText.innerHTML = '<i class="bi bi-record-circle me-2"></i>Start Recording';
            }
        }
    } catch (error) {
        console.error('Failed to atualizar status de recording:', error);
    }
}

/**
 * Toggle recording (start/stop)
 */
async function toggleRecording() {
    try {
        const status = await api.getRecordingStatus();
        const action = status.isRecording ? 'stop' : 'start';
        
        const btn = document.getElementById('recordingStartStopBtn');
        const text = document.getElementById('recordingStartStopText');
        if (btn) {
            btn.disabled = true;
            if (text) {
                text.innerHTML = '<span class="spinner-border spinner-border-sm me-2"></span>Processing...';
            }
        }
        
        await api.setRecordingControl(action);
        showAlert(`Recording ${action === 'start' ? 'started' : 'stopped'} successfully!`, 'success');
        
        // Recarregar status após um pequeno delay
        setTimeout(async () => {
            await updateRecordingStatus();
            if (btn) btn.disabled = false;
        }, 500);
    } catch (error) {
        console.error('Failed to toggle recording:', error);
        showAlert(`Failed to ${status.isRecording ? 'stop' : 'start'} recording: ${error.message}`, 'danger');
        
        const btn = document.getElementById('recordingStartStopBtn');
        if (btn) btn.disabled = false;
        await updateRecordingStatus();
    }
}

/**
 * Define resolução rápida para gravação
 */
async function setRecordingResolution(width, height) {
    try {
        const widthInput = document.getElementById('recordingWidth');
        const heightInput = document.getElementById('recordingHeight');
        
        if (widthInput) widthInput.value = width;
        if (heightInput) heightInput.value = height;
        
        // Atualizar as configurações de gravação
        await updateRecordingSettings();
        showAlert(`Recording resolution set to ${width}x${height}!`, 'success');
    } catch (error) {
        showAlert('Failed to set recording resolution: ' + error.message, 'danger');
    }
}

/**
 * Atualiza configurações de recording
 */
async function updateRecordingSettings() {
    try {
        const widthEl = document.getElementById('recordingWidth');
        const heightEl = document.getElementById('recordingHeight');
        const fpsEl = document.getElementById('recordingFPS');
        const bitrateEl = document.getElementById('recordingBitrate');
        const audioBitrateEl = document.getElementById('recordingAudioBitrate');
        const videoCodecEl = document.getElementById('recordingVideoCodec');
        const audioCodecEl = document.getElementById('recordingAudioCodec');
        const containerEl = document.getElementById('recordingContainer');
        const outputPathEl = document.getElementById('recordingOutputPath');
        const filenameTemplateEl = document.getElementById('recordingFilenameTemplate');
        const includeAudioEl = document.getElementById('recordingIncludeAudio');
        
        if (!widthEl || !heightEl || !fpsEl || !bitrateEl || !audioBitrateEl || !videoCodecEl || !audioCodecEl) {
            showAlert('Error: Form elements not found', 'danger');
            return;
        }
        
        const settings = {
            width: parseInt(widthEl.value) || 1920,
            height: parseInt(heightEl.value) || 1080,
            fps: parseInt(fpsEl.value) || 60,
            bitrate: parseInt(bitrateEl.value) || 8000000,
            audioBitrate: parseInt(audioBitrateEl.value) || 256000,
            codec: videoCodecEl.value || 'h264',
            audioCodec: audioCodecEl.value || 'aac',
            container: containerEl ? containerEl.value : 'mp4',
            outputPath: outputPathEl ? outputPathEl.value : 'recordings/',
            filenameTemplate: filenameTemplateEl ? filenameTemplateEl.value : 'recording_%Y%m%d_%H%M%S',
            includeAudio: includeAudioEl ? includeAudioEl.checked : true
        };
        
        await api.setRecordingSettings(settings);
        showAlert('Recording settings updated!', 'success');
    } catch (error) {
        console.error('Failed to update recording configuration:', error);
        showAlert(`Failed to atualizar configurações: ${error.message}`, 'danger');
    }
}

// Audio functions
let audioState = {
    inputSources: [],
    status: { available: false, open: false, currentInputSource: '' }
};

/**
 * Load audio status
 */
async function loadAudioStatus() {
    try {
        const status = await api.getAudioStatus();
        audioState.status = status;
        updateAudioUI();
    } catch (error) {
        console.error('Failed to load audio status:', error);
        audioState.status = { available: false, open: false, currentInputSource: '' };
        updateAudioUI();
    }
}

/**
 * Load audio input sources
 */
async function loadAudioInputSources() {
    try {
        const response = await api.getAudioInputSources();
        audioState.inputSources = response.sources || [];
        updateAudioInputSourceSelect();
    } catch (error) {
        console.error('Failed to load audio input sources:', error);
        showAlert('Failed to load audio input sources', 'danger');
    }
}

/**
 * Refresh audio input sources
 */
async function refreshAudioInputSources() {
    await loadAudioInputSources();
    showAlert('Input devices refreshed', 'success');
}

/**
 * Update audio input source select dropdown
 */
function updateAudioInputSourceSelect() {
    const select = document.getElementById('audioInputSource');
    if (!select) return;

    select.innerHTML = '<option value="">Select input device...</option>';

    audioState.inputSources.forEach(source => {
        const option = document.createElement('option');
        option.value = source.id;
        option.textContent = source.description || source.name;
        if (source.id === audioState.status.currentInputSource) {
            option.selected = true;
        }
        select.appendChild(option);
    });
}


/**
 * Update audio UI based on current state
 */
function updateAudioUI() {
    const statusInfo = document.getElementById('audioStatusInfo');
    const currentInputSource = document.getElementById('currentInputSource');
    const currentInputFormat = document.getElementById('currentInputFormat');
    const connectBtn = document.getElementById('connectInputBtn');
    const disconnectBtn = document.getElementById('disconnectInputBtn');

    if (statusInfo) {
        if (!audioState.status.available) {
            statusInfo.textContent = 'Audio capture not available';
        } else if (!audioState.status.open) {
            statusInfo.textContent = 'Audio capture not open';
        } else {
            statusInfo.textContent = `Sample Rate: ${audioState.status.sampleRate} Hz, Channels: ${audioState.status.channels}`;
        }
    }

    if (currentInputSource) {
        if (audioState.status.currentInputSource) {
            const source = audioState.inputSources.find(s => s.id === audioState.status.currentInputSource);
            const label = source ? (source.description || source.name) : audioState.status.currentInputSource;
            currentInputSource.textContent = `Capturing from: ${label} — published as 'RetroCapture' source`;
        } else {
            currentInputSource.textContent = 'No input device selected';
        }
    }

    if (currentInputFormat) {
        if (audioState.status.open && audioState.status.currentInputSource) {
            currentInputFormat.textContent =
                `Format: ${audioState.status.sampleRate} Hz, ${audioState.status.channels} channel${audioState.status.channels === 1 ? '' : 's'}`;
        } else {
            currentInputFormat.textContent = '';
        }
    }

    // Update button states
    const hasInput = !!audioState.status.currentInputSource;
    if (connectBtn) connectBtn.disabled = hasInput;
    if (disconnectBtn) disconnectBtn.disabled = !hasInput;
    const resyncBtn = document.getElementById('resyncMonitorBtn');
    if (resyncBtn) resyncBtn.disabled = !hasInput;
}

/**
 * Connect audio input source
 */
async function connectAudioInput() {
    const select = document.getElementById('audioInputSource');
    if (!select || !select.value) {
        showAlert('Please pick an input device', 'warning');
        return;
    }

    try {
        await api.setAudioInputSource(select.value);
        showAlert('Capturing from selected device', 'success');
        await loadAudioStatus();
        updateAudioInputSourceSelect();
    } catch (error) {
        console.error('Failed to start capture:', error);
        showAlert(`Failed to start capture: ${error.message}`, 'danger');
    }
}

/**
 * Stop capturing from the selected input device.
 */
async function disconnectAudioInput() {
    try {
        await api.disconnectAudioInput();
        showAlert('Capture stopped', 'success');
        await loadAudioStatus();
        updateAudioInputSourceSelect();
    } catch (error) {
        console.error('Failed to stop capture:', error);
        showAlert(`Failed to stop capture: ${error.message}`, 'danger');
    }
}

/**
 * Force the monitor playback to drop any backlog (typically after a
 * stall) and snap back to live audio.
 */
async function resyncAudioMonitor() {
    try {
        await api.resyncAudioMonitor();
        showAlert('Monitor resync requested', 'success');
    } catch (error) {
        console.error('Failed to resync monitor:', error);
        showAlert(`Failed to resync monitor: ${error.message}`, 'danger');
    }
}


/**
 * Load all audio data
 */
async function loadAudioData() {
    await loadAudioStatus();
    await loadAudioInputSources();
}

// Load audio data when audio tab is shown (only on Linux)
document.addEventListener('DOMContentLoaded', () => {
    const audioTab = document.getElementById('audio-tab');
    if (audioTab) {
        audioTab.addEventListener('shown.bs.tab', () => {
            // Only load if not Windows
            if (appState.platform && appState.platform.platform !== 'windows') {
                loadAudioData();
            }
        });
        
        // Also load on initial page load if audio tab is active (only on Linux)
        const activeTab = document.querySelector('#audio-tab.active, #audio-tab[aria-selected="true"]');
        if (activeTab && appState.platform && appState.platform.platform !== 'windows') {
            loadAudioData();
        }
    }
});


// ============================================================
// Profile management (recording / streaming)
// ============================================================

const PROFILE_KINDS = {
    recording: {
        select: "recordingProfileSelect",
        applyBtn: "recordingProfileApplyBtn",
        deleteBtn: "recordingProfileDeleteBtn",
        saveBtn: "recordingProfileSaveBtn",
        refreshBtn: "recordingProfileRefreshBtn",
        list: () => api.getRecordingProfiles(),
        save: (name) => api.saveRecordingProfile(name),
        apply: (name) => api.applyRecordingProfile(name),
        remove: (name) => api.deleteRecordingProfile(name),
        afterApply: () => loadRecordingSettings && loadRecordingSettings(),
        label: "recording",
    },
    streaming: {
        select: "streamingProfileSelect",
        applyBtn: "streamingProfileApplyBtn",
        deleteBtn: "streamingProfileDeleteBtn",
        saveBtn: "streamingProfileSaveBtn",
        refreshBtn: "streamingProfileRefreshBtn",
        list: () => api.getStreamingProfiles(),
        save: (name) => api.saveStreamingProfile(name),
        apply: (name) => api.applyStreamingProfile(name),
        remove: (name) => api.deleteStreamingProfile(name),
        afterApply: () => loadStreamingSettings && loadStreamingSettings(),
        label: "streaming",
    },
};

const profileCache = { recording: [], streaming: [] };

async function refreshProfiles(kind) {
    const cfg = PROFILE_KINDS[kind];
    if (!cfg) return;
    try {
        const resp = await cfg.list();
        const names = (resp && Array.isArray(resp.profiles)) ? resp.profiles : [];
        profileCache[kind] = names;
        const sel = document.getElementById(cfg.select);
        if (!sel) return;
        const previous = sel.value;
        sel.innerHTML = "";
        if (names.length === 0) {
            const opt = document.createElement("option");
            opt.value = "";
            opt.textContent = "(no profiles saved)";
            sel.appendChild(opt);
        } else {
            for (const n of names) {
                const opt = document.createElement("option");
                opt.value = n;
                opt.textContent = n;
                sel.appendChild(opt);
            }
            if (names.includes(previous)) sel.value = previous;
        }
        updateProfileButtons(kind);
    } catch (err) {
        console.error("Failed to refresh " + kind + " profiles:", err);
    }
}

function updateProfileButtons(kind) {
    const cfg = PROFILE_KINDS[kind];
    const sel = document.getElementById(cfg.select);
    const has = sel && sel.value && profileCache[kind].includes(sel.value);
    const applyBtn = document.getElementById(cfg.applyBtn);
    const deleteBtn = document.getElementById(cfg.deleteBtn);
    if (applyBtn) applyBtn.disabled = !has;
    if (deleteBtn) deleteBtn.disabled = !has;
}

function bindProfileControls(kind) {
    const cfg = PROFILE_KINDS[kind];
    const sel = document.getElementById(cfg.select);
    if (sel) sel.addEventListener("change", () => updateProfileButtons(kind));

    const applyBtn = document.getElementById(cfg.applyBtn);
    if (applyBtn) applyBtn.addEventListener("click", async () => {
        const name = document.getElementById(cfg.select).value;
        if (!name) return;
        try {
            await cfg.apply(name);
            if (cfg.afterApply) await cfg.afterApply();
        } catch (err) {
            showAlert("Failed to apply " + cfg.label + " profile: " + err.message, "danger");
        }
    });

    const deleteBtn = document.getElementById(cfg.deleteBtn);
    if (deleteBtn) deleteBtn.addEventListener("click", async () => {
        const name = document.getElementById(cfg.select).value;
        if (!name) return;
        if (!confirm("Delete " + cfg.label + " profile \"" + name + "\"?")) return;
        try {
            await cfg.remove(name);
            await refreshProfiles(kind);
        } catch (err) {
            showAlert("Failed to delete " + cfg.label + " profile: " + err.message, "danger");
        }
    });

    const saveBtn = document.getElementById(cfg.saveBtn);
    if (saveBtn) saveBtn.addEventListener("click", () => openSaveProfileModal(kind));

    const refreshBtn = document.getElementById(cfg.refreshBtn);
    if (refreshBtn) refreshBtn.addEventListener("click", () => refreshProfiles(kind));
}

let _saveProfileTargetKind = null;

function openSaveProfileModal(kind) {
    _saveProfileTargetKind = kind;
    const cfg = PROFILE_KINDS[kind];
    document.getElementById("saveProfileModalTitle").textContent = "Save " + cfg.label + " profile";
    const input = document.getElementById("saveProfileNameInput");
    input.value = "";
    document.getElementById("saveProfileExistsWarning").classList.add("d-none");
    const modal = new bootstrap.Modal(document.getElementById("saveProfileModal"));
    modal.show();
    setTimeout(() => input.focus(), 200);
}

document.addEventListener("DOMContentLoaded", () => {
    bindProfileControls("recording");
    bindProfileControls("streaming");
    refreshProfiles("recording");
    refreshProfiles("streaming");

    const input = document.getElementById("saveProfileNameInput");
    if (input) {
        input.addEventListener("input", () => {
            if (!_saveProfileTargetKind) return;
            const exists = profileCache[_saveProfileTargetKind].includes(input.value.trim());
            document.getElementById("saveProfileExistsWarning").classList.toggle("d-none", !exists);
        });
    }

    const confirmBtn = document.getElementById("saveProfileConfirmBtn");
    if (confirmBtn) {
        confirmBtn.addEventListener("click", async () => {
            const kind = _saveProfileTargetKind;
            const cfg = PROFILE_KINDS[kind];
            const name = document.getElementById("saveProfileNameInput").value.trim();
            if (!name) return;
            try {
                await cfg.save(name);
                await refreshProfiles(kind);
                bootstrap.Modal.getInstance(document.getElementById("saveProfileModal")).hide();
            } catch (err) {
                showAlert("Failed to save " + cfg.label + " profile: " + err.message, "danger");
            }
        });
    }
});



// ============================================================
// Live HTTP-TS player (mpegts.js)
// ============================================================

let _livePlayer = null;

function liveStreamUrl() {
    // Stream is served on the same HTTPServer as the portal, so a relative
    // URL works regardless of port. Add a cache-buster so reconnects don't
    // hit a stale TS segment cached by intermediaries.
    return '/stream?t=' + Date.now();
}

function startLivePlayer() {
    const video = document.getElementById('livePlayer');
    const status = document.getElementById('livePlayerStatus');
    const startBtn = document.getElementById('livePlayerStartBtn');
    const stopBtn = document.getElementById('livePlayerStopBtn');
    if (!video) return;

    stopLivePlayer(); // tear down any previous instance

    if (typeof mpegts === 'undefined' || !mpegts.isSupported || !mpegts.isSupported()) {
        // Safari plays MPEG-TS natively; for everything else without MSE
        // support we fall back to a direct <video> src and hope the
        // browser can deal with it.
        video.src = liveStreamUrl();
        video.play().catch(() => {});
        if (status) status.textContent = 'Using native browser playback (Safari / fallback).';
    } else {
        try {
            _livePlayer = mpegts.createPlayer({
                type: 'mpegts',
                isLive: true,
                url: liveStreamUrl(),
            }, {
                liveBufferLatencyChasing: true,
                liveBufferLatencyMaxLatency: 1.5,
                liveBufferLatencyMinRemain: 0.5,
                lazyLoad: false,
            });
            _livePlayer.attachMediaElement(video);
            _livePlayer.load();
            _livePlayer.play().catch(() => {});
            if (status) status.textContent = 'Playing live stream.';
        } catch (err) {
            console.error('mpegts.js error:', err);
            if (status) status.textContent = 'Live player error: ' + err.message;
            return;
        }
    }
    if (startBtn) startBtn.disabled = true;
    if (stopBtn) stopBtn.disabled = false;
}

function stopLivePlayer() {
    const video = document.getElementById('livePlayer');
    const startBtn = document.getElementById('livePlayerStartBtn');
    const stopBtn = document.getElementById('livePlayerStopBtn');
    const status = document.getElementById('livePlayerStatus');

    if (_livePlayer) {
        try {
            _livePlayer.pause();
            _livePlayer.unload();
            _livePlayer.detachMediaElement();
            _livePlayer.destroy();
        } catch (err) {
            console.warn('Error tearing down mpegts player:', err);
        }
        _livePlayer = null;
    }
    if (video) {
        video.pause();
        video.removeAttribute('src');
        video.load();
    }
    if (startBtn) startBtn.disabled = false;
    if (stopBtn) stopBtn.disabled = true;
    if (status) status.textContent = 'Stopped.';
}

document.addEventListener('DOMContentLoaded', () => {
    const startBtn = document.getElementById('livePlayerStartBtn');
    const stopBtn = document.getElementById('livePlayerStopBtn');
    if (startBtn) startBtn.addEventListener('click', startLivePlayer);
    if (stopBtn) stopBtn.addEventListener('click', stopLivePlayer);
});


// ============================================================
// Status badges (recording / source) — top-bar enrichment
// ============================================================

async function updateRecordingStatusBadge() {
    const valueEl = document.getElementById('recordStatus');
    const iconEl = document.getElementById('recordStatusIcon');
    if (!valueEl) return;
    try {
        const status = await api.getRecordingStatus();
        const recording = !!(status && status.recording);
        valueEl.textContent = recording ? 'Recording' : 'Stopped';
        if (iconEl) {
            iconEl.className = recording
                ? 'bi bi-record-circle-fill text-danger'
                : 'bi bi-record-circle text-secondary';
        }
    } catch (err) {
        valueEl.textContent = '-';
    }
}

async function updateSourceStatusBadge() {
    const valueEl = document.getElementById('sourceStatus');
    if (!valueEl) return;
    try {
        const src = await api.getSource();
        // Source type: 0=None, 1=V4L2, 2=DirectShow
        const labels = { 0: 'None', 1: 'V4L2', 2: 'DirectShow' };
        const label = labels[src && src.type] || 'Unknown';
        valueEl.textContent = label;
    } catch (err) {
        valueEl.textContent = '-';
    }
}


// ============================================================
// Preset filtering — visible-vs-hidden matched against the search box.
// Cards are kept in DOM and just toggled with .d-none, so filtering is
// instant and we don't lose state (e.g. partially loaded thumbnails).
// ============================================================

function applyPresetFilter() {
    const input = document.getElementById('presetSearchInput');
    if (!input) return;
    const query = input.value.trim().toLowerCase();
    const grid = document.getElementById('presetsGrid');
    if (!grid) return;
    const cards = grid.querySelectorAll('.preset-card');
    let visible = 0;
    cards.forEach(card => {
        if (!query) {
            card.parentElement.classList.remove('d-none');
            visible++;
            return;
        }
        const haystack = card.textContent.toLowerCase();
        const matches = haystack.includes(query);
        card.parentElement.classList.toggle('d-none', !matches);
        if (matches) visible++;
    });
    // Show "no matches" hint inline (only when filtered)
    let hint = document.getElementById('presetFilterHint');
    if (visible === 0 && query) {
        if (!hint) {
            hint = document.createElement('div');
            hint.id = 'presetFilterHint';
            hint.className = 'col-12 text-muted small';
            hint.textContent = 'No presets match this filter.';
            grid.appendChild(hint);
        }
    } else if (hint) {
        hint.remove();
    }
}

document.addEventListener('DOMContentLoaded', () => {
    const input = document.getElementById('presetSearchInput');
    if (input) input.addEventListener('input', applyPresetFilter);
});


// ============================================================
// Preset shader-parameter editing
// ============================================================
//
// Lets the user tweak the shader parameter values stored inside a
// capture preset without having to apply it, mess with the live shader
// sliders, then re-create the preset by hand. Backed by the new
// PUT /api/v1/presets/<name>/parameters endpoint.

let _editPresetCurrentName = null;

async function openEditPresetParams(presetName) {
    _editPresetCurrentName = presetName;
    document.getElementById('editPresetParamsName').textContent = presetName;
    const body = document.getElementById('editPresetParamsBody');
    body.innerHTML = '<div class="text-muted">Loading…</div>';

    const modalEl = document.getElementById('editPresetParamsModal');
    const modal = new bootstrap.Modal(modalEl);
    modal.show();

    try {
        const preset = await api.getPreset(presetName);
        const params = (preset && preset.shader && preset.shader.parameters) || {};
        const names = Object.keys(params).sort();

        if (names.length === 0) {
            body.innerHTML = '<div class="text-muted">This preset has no stored shader parameters.</div>';
            return;
        }

        body.innerHTML = '';
        const table = document.createElement('div');
        table.className = 'row g-3';
        names.forEach(name => {
            const value = Number(params[name]);
            const col = document.createElement('div');
            col.className = 'col-md-6';
            const safeId = 'editParam_' + name.replace(/[^A-Za-z0-9_]/g, '_');
            col.innerHTML = `
                <label class="form-label" for="${safeId}">${escapeHtml(name)}</label>
                <input type="number" step="any" class="form-control form-control-sm preset-param-input"
                       id="${safeId}" data-param-name="${escapeHtml(name)}"
                       value="${Number.isFinite(value) ? value : 0}">
            `;
            table.appendChild(col);
        });
        body.appendChild(table);
    } catch (err) {
        body.innerHTML = `<div class="text-danger">Failed to load preset: ${escapeHtml(err.message)}</div>`;
    }
}

async function saveEditPresetParams() {
    if (!_editPresetCurrentName) return;
    const inputs = document.querySelectorAll('.preset-param-input');
    if (inputs.length === 0) {
        // Nothing to save (preset had no params); just close.
        bootstrap.Modal.getInstance(document.getElementById('editPresetParamsModal')).hide();
        return;
    }
    const params = {};
    inputs.forEach(input => {
        const name = input.dataset.paramName;
        const v = parseFloat(input.value);
        if (Number.isFinite(v)) params[name] = v;
    });
    try {
        await api.updatePresetParameters(_editPresetCurrentName, params);
        showAlert('Preset parameters saved.', 'success');
        bootstrap.Modal.getInstance(document.getElementById('editPresetParamsModal')).hide();
    } catch (err) {
        showAlert('Failed to save preset parameters: ' + err.message, 'danger');
    }
}

document.addEventListener('DOMContentLoaded', () => {
    const saveBtn = document.getElementById('editPresetParamsSaveBtn');
    if (saveBtn) saveBtn.addEventListener('click', saveEditPresetParams);
});


// ============================================================
// Source overscan — mirrors the native UI's Source Overscan
// section. X/Y sliders share a "lock" toggle that copies one onto
// the other; values are persisted server-side via the new
// /api/v1/source/overscan endpoint.
// ============================================================

let _overscanLoaded = false;
let _overscanLocked = false;

function fmtOverscan(v) {
    return Number(v).toFixed(1) + '%';
}

async function loadOverscan() {
    try {
        const data = await api.getSourceOverscan();
        const xEl = document.getElementById('overscanX');
        const yEl = document.getElementById('overscanY');
        const lockEl = document.getElementById('overscanLocked');
        if (!xEl || !yEl || !lockEl) return;
        xEl.value = data.x ?? 0;
        yEl.value = data.y ?? 0;
        lockEl.checked = !!data.locked;
        _overscanLocked = lockEl.checked;
        document.getElementById('overscanXValue').textContent = fmtOverscan(xEl.value);
        document.getElementById('overscanYValue').textContent = fmtOverscan(yEl.value);
        _overscanLoaded = true;
    } catch (err) {
        console.warn('Failed to load overscan:', err);
    }
}

const _overscanThrottle = { pending: null, timer: null };
function commitOverscan() {
    if (!_overscanLoaded) return;
    const xEl = document.getElementById('overscanX');
    const yEl = document.getElementById('overscanY');
    const payload = {
        x: parseFloat(xEl.value),
        y: parseFloat(yEl.value),
        locked: _overscanLocked,
    };
    _overscanThrottle.pending = payload;
    if (_overscanThrottle.timer) return;
    _overscanThrottle.timer = setTimeout(async () => {
        const p = _overscanThrottle.pending;
        _overscanThrottle.timer = null;
        _overscanThrottle.pending = null;
        try {
            await api.setSourceOverscan(p.x, p.y, p.locked);
        } catch (err) {
            console.warn('Failed to set overscan:', err);
        }
    }, 80);
}

document.addEventListener('DOMContentLoaded', () => {
    const xEl = document.getElementById('overscanX');
    const yEl = document.getElementById('overscanY');
    const lockEl = document.getElementById('overscanLocked');
    if (!xEl || !yEl || !lockEl) return;

    const onInput = (sourceAxis, value) => {
        document.getElementById('overscan' + sourceAxis + 'Value').textContent = fmtOverscan(value);
        if (_overscanLocked) {
            const otherAxis = sourceAxis === 'X' ? 'Y' : 'X';
            const otherEl = document.getElementById('overscan' + otherAxis);
            if (otherEl) {
                otherEl.value = value;
                document.getElementById('overscan' + otherAxis + 'Value').textContent = fmtOverscan(value);
            }
        }
        commitOverscan();
    };

    xEl.addEventListener('input', () => onInput('X', xEl.value));
    yEl.addEventListener('input', () => onInput('Y', yEl.value));
    lockEl.addEventListener('change', () => {
        _overscanLocked = lockEl.checked;
        if (_overscanLocked) {
            // Lock just enabled — mirror X to Y so they start in sync.
            yEl.value = xEl.value;
            document.getElementById('overscanYValue').textContent = fmtOverscan(yEl.value);
        }
        commitOverscan();
    });

    loadOverscan();
});


// ============================================================
// Master shader pipeline toggle (config.html shader tab)
// ============================================================

document.addEventListener('DOMContentLoaded', () => {
    const toggle = document.getElementById('shaderPipelineEnabled');
    if (!toggle) return;
    toggle.addEventListener('change', async () => {
        try {
            await api.setShaderPipelineEnabled(toggle.checked);
        } catch (err) {
            showAlert('Failed to toggle shader pipeline: ' + err.message, 'danger');
            toggle.checked = !toggle.checked; // revert
        }
    });
});


// ============================================================
// Per-pipeline shader override (streaming / recording tabs)
// ============================================================

document.addEventListener('DOMContentLoaded', () => {
    const stream = document.getElementById('streamingApplyShader');
    if (stream) stream.addEventListener('change', async () => {
        try {
            await api.setStreamingSettings({ applyShader: stream.checked });
        } catch (err) {
            showAlert('Failed to update streaming shader override: ' + err.message, 'danger');
            stream.checked = !stream.checked;
        }
    });

    const rec = document.getElementById('recordingApplyShader');
    if (rec) rec.addEventListener('change', async () => {
        try {
            await api.setRecordingSettings({ applyShader: rec.checked });
        } catch (err) {
            showAlert('Failed to update recording shader override: ' + err.message, 'danger');
            rec.checked = !rec.checked;
        }
    });
});

// =============================================================
// AVFoundation (macOS) — device + format selection
// =============================================================
// Endpoints are gated on the backend; calling them on Linux/Windows
// returns 404 and the corresponding cards stay hidden.

const avfState = {
    devices: [],
    formats: [],
    audioDevices: [],
    formatsForDeviceId: '',
    supported: null  // null until first probe; true/false thereafter
};

async function probeAVFoundationSupport() {
    if (avfState.supported !== null) return avfState.supported;
    try {
        await api.getAVFoundationDevices();
        avfState.supported = true;
    } catch (err) {
        avfState.supported = false;
    }
    // Show / hide the AVFoundation cards based on platform support.
    const sourceContainer = document.getElementById('avfoundationControlsContainer');
    const audioContainer  = document.getElementById('avfoundationAudioDeviceContainer');
    if (avfState.supported) {
        if (sourceContainer) sourceContainer.style.display = '';
        if (audioContainer)  audioContainer.style.display  = '';
    }
    return avfState.supported;
}

async function refreshAVFoundationDevices() {
    if (!(await probeAVFoundationSupport())) return;
    try {
        const resp = await api.getAVFoundationDevices();
        avfState.devices = resp.devices || [];
    } catch (err) {
        console.error('AVFoundation: getAVFoundationDevices failed:', err);
        avfState.devices = [];
    }
    const sel = document.getElementById('avfoundationDevice');
    if (!sel) return;
    sel.innerHTML = '';
    if (avfState.devices.length === 0) {
        const opt = document.createElement('option');
        opt.value = '';
        opt.textContent = 'No AVFoundation devices found';
        sel.appendChild(opt);
        return;
    }
    avfState.devices.forEach(d => {
        const opt = document.createElement('option');
        opt.value = d.id;
        opt.textContent = d.name || d.id;
        sel.appendChild(opt);
    });
}

async function refreshAVFoundationFormats() {
    if (!(await probeAVFoundationSupport())) return;
    const sel = document.getElementById('avfoundationDevice');
    const deviceId = sel ? sel.value : '';
    try {
        const resp = await api.getAVFoundationFormats(deviceId);
        avfState.formats = resp.formats || [];
        avfState.formatsForDeviceId = deviceId;
    } catch (err) {
        console.error('AVFoundation: getAVFoundationFormats failed:', err);
        avfState.formats = [];
    }
    const formatSel = document.getElementById('avfoundationFormat');
    if (!formatSel) return;
    formatSel.innerHTML = '';
    if (avfState.formats.length === 0) {
        const opt = document.createElement('option');
        opt.value = '';
        opt.textContent = 'No formats available';
        formatSel.appendChild(opt);
        return;
    }
    avfState.formats.forEach(f => {
        const opt = document.createElement('option');
        opt.value = f.id;
        opt.textContent = f.displayName || `${f.width}x${f.height}`;
        formatSel.appendChild(opt);
    });
}

async function refreshAVFoundationAudioDevices() {
    if (!(await probeAVFoundationSupport())) return;
    try {
        const resp = await api.getAVFoundationAudioDevices();
        avfState.audioDevices = resp.devices || [];
        const sel = document.getElementById('avfoundationAudioDevice');
        if (!sel) return;
        sel.innerHTML = '';
        avfState.audioDevices.forEach(d => {
            const opt = document.createElement('option');
            opt.value = d.id;
            opt.textContent = d.name || d.id;
            if (d.id === resp.current) opt.selected = true;
            sel.appendChild(opt);
        });
    } catch (err) {
        console.error('AVFoundation: getAVFoundationAudioDevices failed:', err);
    }
}

document.addEventListener('DOMContentLoaded', () => {
    const deviceSel = document.getElementById('avfoundationDevice');
    if (deviceSel) {
        deviceSel.addEventListener('change', async () => {
            try {
                await api.setAVFoundationDevice(deviceSel.value);
                await refreshAVFoundationFormats();
                showAlert('AVFoundation device updated', 'success');
            } catch (err) {
                showAlert('Failed to switch AVFoundation device: ' + err.message, 'danger');
            }
        });
    }
    const formatSel = document.getElementById('avfoundationFormat');
    if (formatSel) {
        formatSel.addEventListener('change', async () => {
            const dev = document.getElementById('avfoundationDevice');
            try {
                await api.setAVFoundationFormat(formatSel.value, dev ? dev.value : '');
                showAlert('AVFoundation format applied', 'success');
            } catch (err) {
                showAlert('Failed to apply AVFoundation format: ' + err.message, 'danger');
            }
        });
    }
    const audioSel = document.getElementById('avfoundationAudioDevice');
    if (audioSel) {
        audioSel.addEventListener('change', async () => {
            try {
                await api.setAVFoundationAudioDevice(audioSel.value);
                showAlert('AVFoundation audio device updated', 'success');
            } catch (err) {
                showAlert('Failed to switch AVFoundation audio device: ' + err.message, 'danger');
            }
        });
    }
    // First-load probe — populates the dropdowns and reveals the cards
    // on macOS.
    probeAVFoundationSupport().then(supported => {
        if (supported) {
            refreshAVFoundationDevices().then(refreshAVFoundationFormats);
            refreshAVFoundationAudioDevices();
        }
    });
});
