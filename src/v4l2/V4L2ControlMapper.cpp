#include "V4L2ControlMapper.h"
#include <linux/videodev2.h>

// Static member initialization
bool V4L2ControlMapper::s_mapsInitialized = false;

// Control name to ID mapping
const std::unordered_map<std::string, uint32_t> V4L2ControlMapper::s_nameToIdMap = {
    {"Brightness", V4L2_CID_BRIGHTNESS},
    {"Contrast", V4L2_CID_CONTRAST},
    {"Saturation", V4L2_CID_SATURATION},
    {"Hue", V4L2_CID_HUE},
    {"Gain", V4L2_CID_GAIN},
    {"Exposure", V4L2_CID_EXPOSURE_ABSOLUTE},
    {"Sharpness", V4L2_CID_SHARPNESS},
    {"Gamma", V4L2_CID_GAMMA},
    {"White Balance", V4L2_CID_WHITE_BALANCE_TEMPERATURE},
};

// Control ID to name mapping (reverse lookup)
const std::unordered_map<uint32_t, std::string> V4L2ControlMapper::s_idToNameMap = {
    {V4L2_CID_BRIGHTNESS, "Brightness"},
    {V4L2_CID_CONTRAST, "Contrast"},
    {V4L2_CID_SATURATION, "Saturation"},
    {V4L2_CID_HUE, "Hue"},
    {V4L2_CID_GAIN, "Gain"},
    {V4L2_CID_EXPOSURE_ABSOLUTE, "Exposure"},
    {V4L2_CID_SHARPNESS, "Sharpness"},
    {V4L2_CID_GAMMA, "Gamma"},
    {V4L2_CID_WHITE_BALANCE_TEMPERATURE, "White Balance"},
};

uint32_t V4L2ControlMapper::nameToControlId(const std::string& name) {
    initializeMaps();
    auto it = s_nameToIdMap.find(name);
    if (it != s_nameToIdMap.end()) {
        return it->second;
    }
    return 0; // Invalid control ID
}

std::string V4L2ControlMapper::controlIdToName(uint32_t cid) {
    initializeMaps();
    auto it = s_idToNameMap.find(cid);
    if (it != s_idToNameMap.end()) {
        return it->second;
    }
    return ""; // Empty string for invalid control
}

std::vector<std::string> V4L2ControlMapper::getAvailableControls() {
    initializeMaps();
    std::vector<std::string> controls;
    controls.reserve(s_nameToIdMap.size());
    for (const auto& pair : s_nameToIdMap) {
        controls.push_back(pair.first);
    }
    return controls;
}

bool V4L2ControlMapper::isValidControl(const std::string& name) {
    initializeMaps();
    return s_nameToIdMap.find(name) != s_nameToIdMap.end();
}

void V4L2ControlMapper::initializeMaps() {
    // Maps are already initialized as static const members
    // This function exists for future extensibility if needed
    s_mapsInitialized = true;
}

