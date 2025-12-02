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
    v4l2: { devices: [], controls: [], currentDevice: '' },
    status: { active: false, url: '', clientCount: 0 }
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
        showAlert('URL copiada para a área de transferência!', 'success');
    }).catch(() => {
        showAlert('Erro ao copiar URL', 'danger');
    });
}

/**
 * Carrega todos os dados da aplicação
 */
async function loadAllData() {
    try {
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
    } catch (error) {
        console.error('Erro ao carregar dados:', error);
        showAlert('Erro ao carregar dados: ' + error.message, 'danger');
    }
}

/**
 * Carrega o status do streaming
 */
async function loadStatus() {
    try {
        const status = await api.getStatus();
        appState.status = status;
        
        document.getElementById('streamStatus').textContent = status.active ? 'Ativo' : 'Inativo';
        document.getElementById('streamStatusIcon').className = status.active 
            ? 'bi bi-broadcast text-success' 
            : 'bi bi-broadcast text-secondary';
        document.getElementById('clientCount').textContent = status.clientCount || 0;
    } catch (error) {
        console.error('Erro ao carregar status:', error);
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
        
        const sourceType = source.type || 0;
        document.getElementById('sourceType').value = sourceType;
        
        // Atualizar visibilidade dos controles baseado no tipo de fonte
        updateSourceUI(sourceType);
        
        // Se for V4L2, carregar dispositivos e controles
        if (sourceType === 1) {
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
        }
    } catch (error) {
        console.error('Erro ao carregar source:', error);
    }
}

/**
 * Atualiza a UI baseado no tipo de fonte selecionado
 */
function updateSourceUI(sourceType) {
    const v4l2Container = document.getElementById('v4l2ControlsContainer');
    const noneMessage = document.getElementById('noneSourceMessage');
    
    if (sourceType === 1) {
        // V4L2 selecionado - mostrar controles V4L2
        if (v4l2Container) v4l2Container.style.display = 'block';
        if (noneMessage) noneMessage.style.display = 'none';
    } else {
        // None selecionado - esconder controles V4L2
        if (v4l2Container) v4l2Container.style.display = 'none';
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
        console.error('Erro ao carregar dispositivos V4L2:', error);
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
            container.innerHTML = '<div class="col-12 text-muted">Nenhum controle disponível</div>';
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
        console.error('Erro ao carregar controles V4L2:', error);
    }
}

/**
 * Atualiza um controle V4L2 na aba Source
 */
async function updateV4L2ControlSource(name, value) {
    try {
        await api.setV4L2Control(name, parseInt(value));
    } catch (error) {
        showAlert(`Erro ao atualizar controle ${name}: ` + error.message, 'danger');
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
        showAlert('Erro ao atualizar dispositivo V4L2: ' + error.message, 'danger');
    }
}

/**
 * Define FPS rápido
 */
async function setQuickFPS(fps) {
    try {
        const fpsInput = document.getElementById('v4l2CaptureFPS');
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
        showAlert('Erro ao definir FPS: ' + error.message, 'danger');
    }
}

/**
 * Define resolução rápida
 */
async function setQuickResolution(width, height) {
    try {
        const widthInput = document.getElementById('v4l2CaptureWidth');
        const heightInput = document.getElementById('v4l2CaptureHeight');
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
        showAlert('Erro ao definir resolução: ' + error.message, 'danger');
    }
}

/**
 * Atualiza resolução/FPS de captura da aba Source
 */
async function updateV4L2CaptureSettings() {
    try {
        const widthInput = document.getElementById('v4l2CaptureWidth');
        const heightInput = document.getElementById('v4l2CaptureHeight');
        const fpsInput = document.getElementById('v4l2CaptureFPS');
        
        if (!widthInput || !heightInput || !fpsInput) {
            showAlert('Erro: Campos de captura não encontrados', 'danger');
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
        showAlert('Erro ao atualizar configurações: ' + error.message, 'danger');
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
        showAlert('Erro ao atualizar dispositivos: ' + error.message, 'danger');
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
        
        // Se for V4L2, carregar dispositivos e controles
        if (sourceType === 1) {
            await loadV4L2DevicesForSource();
            await loadV4L2ControlsForSource();
        }
        
        // Recarregar dados completos
        await loadSource();
    } catch (error) {
        showAlert('Erro ao atualizar fonte: ' + error.message, 'danger');
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
        console.error('Erro ao carregar shader:', error);
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
        console.error('Erro ao carregar lista de shaders:', error);
        showAlert('Erro ao carregar lista de shaders: ' + error.message, 'danger');
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
            const col = document.createElement('div');
            col.className = 'col-md-6';
            col.innerHTML = `
                <label class="form-label">${param.name}${param.description ? ': ' + param.description : ''}</label>
                <div class="input-group">
                    <input type="range" class="form-range" 
                           id="shaderParam_${param.name}" 
                           min="${param.min}" 
                           max="${param.max}" 
                           step="${param.step || 0.01}" 
                           value="${param.value}"
                           oninput="updateShaderParameterValue('${param.name.replace(/'/g, "\\'")}', this.value)">
                    <span class="input-group-text" style="min-width: 80px;" id="shaderParamValue_${param.name}">${param.value.toFixed(2)}</span>
                </div>
            `;
            container.appendChild(col);
        });
    } catch (error) {
        console.error('Erro ao carregar parâmetros do shader:', error);
    }
}

/**
 * Atualiza o valor de um parâmetro do shader (visual)
 */
function updateShaderParameterValue(name, value) {
    document.getElementById(`shaderParamValue_${name}`).textContent = parseFloat(value).toFixed(2);
}

/**
 * Atualiza um parâmetro do shader na API
 */
async function updateShaderParameter(name, value) {
    try {
        await api.setShaderParameter(name, parseFloat(value));
        // Atualização em tempo real - sem alerta
    } catch (error) {
        showAlert(`Erro ao atualizar parâmetro ${name}: ` + error.message, 'danger');
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
        console.error('Erro ao atualizar shader:', error);
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
        
        // Atualizar campos na aba Source (V4L2)
        const v4l2Width = document.getElementById('v4l2CaptureWidth');
        const v4l2Height = document.getElementById('v4l2CaptureHeight');
        const v4l2FPS = document.getElementById('v4l2CaptureFPS');
        
        if (v4l2Width) v4l2Width.value = appState.capture.width;
        if (v4l2Height) v4l2Height.value = appState.capture.height;
        if (v4l2FPS) v4l2FPS.value = appState.capture.fps;
        
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
        console.error('Erro ao carregar configurações de captura:', error);
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
    } catch (error) {
        console.error('Erro ao carregar configurações de imagem:', error);
    }
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
        
        const settings = {
            brightness: brightnessEl ? parseFloat(brightnessEl.value) : 0,
            contrast: contrastEl ? parseFloat(contrastEl.value) : 1,
            maintainAspect: maintainAspectEl ? maintainAspectEl.checked : false,
            fullscreen: fullscreenEl ? fullscreenEl.checked : false,
            monitorIndex: monitorIndexEl ? parseInt(monitorIndexEl.value) : -1
        };
        
        await api.setImageSettings(settings);
        // Não recarregar para evitar flicker - atualização em tempo real
    } catch (error) {
        console.error('Erro ao atualizar configurações de imagem:', error);
        // Não mostrar alerta para atualizações em tempo real
    }
}

/**
 * Atualiza controles específicos do codec
 */
function updateCodecSpecificControls(codec) {
    // Esconder todos os containers
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
        console.error('Erro ao carregar configurações de streaming:', error);
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
            showAlert('Erro: Elementos do formulário não encontrados', 'danger');
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
        console.error('Erro ao atualizar configurações de streaming:', error);
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
        console.error('Erro ao carregar dispositivos V4L2:', error);
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
        showAlert('Erro ao atualizar dispositivo V4L2: ' + error.message, 'danger');
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
    statusUpdateInterval = setInterval(loadStatus, 2000);
    
    // Event listeners para sliders
    document.getElementById('brightness').addEventListener('input', function() {
        document.getElementById('brightnessValue').textContent = parseFloat(this.value).toFixed(2);
    });
    
    document.getElementById('contrast').addEventListener('input', function() {
        document.getElementById('contrastValue').textContent = parseFloat(this.value).toFixed(2);
    });
    
    // Event listener para mudança de tipo de fonte - aplica automaticamente
    document.getElementById('sourceType').addEventListener('change', function() {
        const sourceType = parseInt(this.value);
        updateSource(); // Aplicar mudança automaticamente
    });
    
    // Event listener para mudança de dispositivo V4L2 na aba Source
    const v4l2DeviceSelect = document.getElementById('v4l2Device');
    if (v4l2DeviceSelect) {
        v4l2DeviceSelect.addEventListener('change', function() {
            updateV4L2DeviceSource();
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
    
    // ===== ATUALIZAÇÃO EM TEMPO REAL PARA TODOS OS CONTROLES =====
    
    // Resolução/FPS de captura (V4L2) - atualização em tempo real com debounce
    let captureSettingsTimeout = null;
    const v4l2CaptureWidth = document.getElementById('v4l2CaptureWidth');
    const v4l2CaptureHeight = document.getElementById('v4l2CaptureHeight');
    const v4l2CaptureFPS = document.getElementById('v4l2CaptureFPS');
    
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
    
});
