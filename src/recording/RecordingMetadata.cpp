#include "RecordingMetadata.h"
#include <ctime>
#include <iomanip>
#include <sstream>

nlohmann::json RecordingMetadata::toJSON() const
{
    nlohmann::json j;
    j["id"] = id;
    j["filename"] = filename;
    j["filepath"] = filepath;
    j["container"] = container;
    j["videoCodec"] = videoCodec;
    j["audioCodec"] = audioCodec;
    j["width"] = width;
    j["height"] = height;
    j["fps"] = fps;
    j["fileSize"] = fileSize;
    j["duration"] = durationUs;
    j["createdAt"] = createdAt;
    if (!thumbnailPath.empty())
    {
        j["thumbnailPath"] = thumbnailPath;
    }
    return j;
}

RecordingMetadata RecordingMetadata::fromJSON(const nlohmann::json& json)
{
    RecordingMetadata metadata;
    
    if (json.contains("id"))
        metadata.id = json["id"].get<std::string>();
    if (json.contains("filename"))
        metadata.filename = json["filename"].get<std::string>();
    if (json.contains("filepath"))
        metadata.filepath = json["filepath"].get<std::string>();
    if (json.contains("container"))
        metadata.container = json["container"].get<std::string>();
    if (json.contains("videoCodec"))
        metadata.videoCodec = json["videoCodec"].get<std::string>();
    if (json.contains("audioCodec"))
        metadata.audioCodec = json["audioCodec"].get<std::string>();
    if (json.contains("width"))
        metadata.width = json["width"].get<uint32_t>();
    if (json.contains("height"))
        metadata.height = json["height"].get<uint32_t>();
    if (json.contains("fps"))
        metadata.fps = json["fps"].get<uint32_t>();
    if (json.contains("fileSize"))
        metadata.fileSize = json["fileSize"].get<uint64_t>();
    if (json.contains("duration"))
        metadata.durationUs = json["duration"].get<uint64_t>();
    if (json.contains("createdAt"))
        metadata.createdAt = json["createdAt"].get<std::string>();
    if (json.contains("thumbnailPath"))
        metadata.thumbnailPath = json["thumbnailPath"].get<std::string>();
    
    return metadata;
}
