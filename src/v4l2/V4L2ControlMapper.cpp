#include "V4L2ControlMapper.h"

uint32_t V4L2ControlMapper::getControlId(const std::string& name)
{
    if (name == "Brightness") return V4L2_CID_BRIGHTNESS;
    else if (name == "Contrast") return V4L2_CID_CONTRAST;
    else if (name == "Saturation") return V4L2_CID_SATURATION;
    else if (name == "Hue") return V4L2_CID_HUE;
    else if (name == "Gain") return V4L2_CID_GAIN;
    else if (name == "Exposure") return V4L2_CID_EXPOSURE_ABSOLUTE;
    else if (name == "Sharpness") return V4L2_CID_SHARPNESS;
    else if (name == "Gamma") return V4L2_CID_GAMMA;
    else if (name == "White Balance") return V4L2_CID_WHITE_BALANCE_TEMPERATURE;
    
    return 0;
}

bool V4L2ControlMapper::isControlSupported(const std::string& name)
{
    return getControlId(name) != 0;
}

