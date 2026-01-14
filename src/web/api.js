/**
 * RetroCapture API Client
 * Cliente JavaScript para comunicação com a API REST do RetroCapture
 */

class RetroCaptureAPI {
    constructor(baseUrl = '') {
        this.baseUrl = baseUrl;
        this.apiPrefix = '/api/v1';
    }

    /**
     * Faz uma requisição HTTP para a API
     */
    async request(method, endpoint, data = null) {
        const url = this.baseUrl + this.apiPrefix + endpoint;
        const options = {
            method: method,
            headers: {
                'Content-Type': 'application/json',
            },
        };

        if (data && (method === 'POST' || method === 'PUT')) {
            options.body = JSON.stringify(data);
        }

        try {
            const response = await fetch(url, options);
            const text = await response.text();
            
            if (!response.ok) {
                let errorMessage = `HTTP ${response.status}`;
                try {
                    const errorJson = JSON.parse(text);
                    if (errorJson.message) {
                        errorMessage = errorJson.message;
                    }
                } catch (e) {
                    errorMessage = text || errorMessage;
                }
                throw new Error(errorMessage);
            }

            // Tentar parsear como JSON, se falhar retornar texto
            try {
                return JSON.parse(text);
            } catch (e) {
                return text;
            }
        } catch (error) {
            console.error(`API Error [${method} ${endpoint}]:`, error);
            throw error;
        }
    }

    // ========== GET Methods ==========

    async getSource() {
        return await this.request('GET', '/source');
    }

    async getShader() {
        return await this.request('GET', '/shader');
    }

    async getShaderList() {
        return await this.request('GET', '/shader/list');
    }

    async getShaderParameters() {
        return await this.request('GET', '/shader/parameters');
    }

    async getCaptureResolution() {
        return await this.request('GET', '/capture/resolution');
    }

    async getCaptureFPS() {
        return await this.request('GET', '/capture/fps');
    }

    async getImageSettings() {
        return await this.request('GET', '/image/settings');
    }

    async getStreamingSettings() {
        return await this.request('GET', '/streaming/settings');
    }

    async getV4L2Devices() {
        return await this.request('GET', '/v4l2/devices');
    }

    async refreshV4L2Devices() {
        return await this.request('GET', '/v4l2/devices/refresh');
    }

    async getV4L2Controls() {
        return await this.request('GET', '/v4l2/controls');
    }

    async getDSDevices() {
        return await this.request('GET', '/ds/devices');
    }

    async refreshDSDevices() {
        return await this.request('GET', '/ds/devices/refresh');
    }

    async getStatus() {
        return await this.request('GET', '/status');
    }

    async getPlatform() {
        return await this.request('GET', '/platform');
    }

    async getPresets() {
        return await this.request('GET', '/presets');
    }

    async getPreset(name) {
        return await this.request('GET', `/presets/${encodeURIComponent(name)}`);
    }

    async getRecordingSettings() {
        return await this.request('GET', '/recording/settings');
    }

    async getRecordingStatus() {
        return await this.request('GET', '/recording/status');
    }

    async getRecordings() {
        return await this.request('GET', '/recordings');
    }

    async getRecording(id) {
        return await this.request('GET', `/recordings/${encodeURIComponent(id)}`);
    }

    // ========== SET Methods ==========

    async setSource(sourceType) {
        return await this.request('POST', '/source', { type: sourceType });
    }

    async setShader(shaderName) {
        return await this.request('POST', '/shader', { shader: shaderName });
    }

    async setShaderParameter(name, value) {
        return await this.request('POST', '/shader/parameter', { name, value });
    }

    async setCaptureResolution(width, height) {
        return await this.request('POST', '/capture/resolution', { width, height });
    }

    async setCaptureFPS(fps) {
        return await this.request('POST', '/capture/fps', { fps });
    }

    async setImageSettings(settings) {
        return await this.request('POST', '/image/settings', settings);
    }

    async setStreamingSettings(settings) {
        return await this.request('POST', '/streaming/settings', settings);
    }

    async setV4L2Control(name, value) {
        return await this.request('POST', '/v4l2/control', { name, value });
    }

    async setV4L2Device(device) {
        return await this.request('POST', '/v4l2/device', { device });
    }

    async setDSDevice(device) {
        return await this.request('POST', '/ds/device', { device });
    }

    async setStreamingControl(action) {
        // action deve ser 'start' ou 'stop'
        return await this.request('POST', '/streaming/control', { action });
    }

    async createPreset(name, description, captureThumbnail = true) {
        return await this.request('POST', '/presets', { name, description, captureThumbnail });
    }

    async applyPreset(name) {
        return await this.request('POST', `/presets/${encodeURIComponent(name)}/apply`, { name });
    }

    async deletePreset(name) {
        return await this.request('DELETE', `/presets/${encodeURIComponent(name)}`);
    }

    async setRecordingSettings(settings) {
        return await this.request('POST', '/recording/settings', settings);
    }

    async setRecordingControl(action) {
        // action deve ser 'start' ou 'stop'
        return await this.request('POST', '/recording/control', { action });
    }

    async deleteRecording(id) {
        return await this.request('DELETE', `/recordings/${encodeURIComponent(id)}`);
    }

    async renameRecording(id, newName) {
        return await this.request('PUT', `/recordings/${encodeURIComponent(id)}`, { name: newName });
    }

    // Audio API methods
    async getAudioInputSources() {
        return await this.request('GET', '/audio/input-sources');
    }

    async getAudioStatus() {
        return await this.request('GET', '/audio/status');
    }

    async setAudioInputSource(sourceId) {
        return await this.request('POST', '/audio/input-source', { sourceId });
    }

    async disconnectAudioInput() {
        return await this.request('POST', '/audio/disconnect-input');
    }
}

// Instância global da API
const api = new RetroCaptureAPI();
