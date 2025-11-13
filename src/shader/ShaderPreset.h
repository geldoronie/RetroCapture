#pragma once

#include <string>
#include <vector>
#include <unordered_map>

struct ShaderPass {
    std::string shaderPath;
    bool filterLinear = true;
    std::string wrapMode = "clamp_to_edge"; // Padrão mais seguro e universalmente suportado
    bool mipmapInput = false;
    std::string alias;
    bool floatFramebuffer = false;
    bool srgbFramebuffer = false;
    unsigned int frameCountMod = 0; // Módulo para FrameCount (0 = desabilitado)
    
    // Scaling
    std::string scaleTypeX = "source"; // "source", "viewport", "absolute"
    float scaleX = 1.0f;
    std::string scaleTypeY = "source";
    float scaleY = 1.0f;
};

struct ShaderTexture {
    std::string path;
    std::string wrapMode = "clamp_to_border";
    bool mipmap = false;
    bool linear = true; // Linear filtering (padrão true)
};

class ShaderPreset {
public:
    ShaderPreset();
    ~ShaderPreset();
    
    bool load(const std::string& presetPath);
    bool save(const std::string& presetPath, const std::unordered_map<std::string, float>& customParameters = {}) const;
    bool saveAs(const std::string& presetPath, const std::unordered_map<std::string, float>& customParameters = {}) const;
    
    const std::vector<ShaderPass>& getPasses() const { return m_passes; }
    const std::unordered_map<std::string, ShaderTexture>& getTextures() const { return m_textures; }
    const std::unordered_map<std::string, float>& getParameters() const { return m_parameters; }
    
    std::string getBasePath() const { return m_basePath; }
    std::string getPresetPath() const { return m_presetPath; }
    
    void setParameter(const std::string& name, float value) { m_parameters[name] = value; }
    
private:
    std::vector<ShaderPass> m_passes;
    std::unordered_map<std::string, ShaderTexture> m_textures;
    std::unordered_map<std::string, float> m_parameters;
    std::string m_basePath;
    std::string m_presetPath; // Caminho do preset carregado
    
    bool parseLine(const std::string& line, int& passIndex);
    std::string resolvePath(const std::string& path);
    float parseFloat(const std::string& value);
};

