#pragma once

#include <string>
#include <cstdint>
#include <vector>
#include <unordered_map>

/**
 * Maps V4L2 control names to control IDs and vice versa.
 * 
 * This class follows the Open/Closed Principle - new controls can be added
 * without modifying existing code by extending the registry.
 */
class V4L2ControlMapper {
public:
    /**
     * Convert a control name to its V4L2 control ID.
     * @param name Control name (e.g., "Brightness", "Contrast")
     * @return Control ID, or 0 if not found
     */
    static uint32_t nameToControlId(const std::string& name);
    
    /**
     * Convert a V4L2 control ID to its name.
     * @param cid Control ID
     * @return Control name, or empty string if not found
     */
    static std::string controlIdToName(uint32_t cid);
    
    /**
     * Get list of all available control names.
     * @return Vector of control names
     */
    static std::vector<std::string> getAvailableControls();
    
    /**
     * Check if a control name is valid.
     * @param name Control name
     * @return true if valid, false otherwise
     */
    static bool isValidControl(const std::string& name);

private:
    // Registry mapping control names to IDs
    static const std::unordered_map<std::string, uint32_t> s_nameToIdMap;
    
    // Registry mapping control IDs to names (reverse lookup)
    static const std::unordered_map<uint32_t, std::string> s_idToNameMap;
    
    // Initialize the registries (called once on first access)
    static void initializeMaps();
    static bool s_mapsInitialized;
};

