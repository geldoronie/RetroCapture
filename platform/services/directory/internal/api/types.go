package api

// Wire types for the directory protocol. See docs/DIRECTORY_PROTOCOL.md
// for the spec these mirror; tag everything with the camelCase names
// the spec uses so the JSON round-trip is direct.
//
// Anything that's optional on the wire is a pointer here; that lets
// PATCH handlers distinguish "not specified" from "set to zero value".

// --- Envelope ---

type Envelope struct {
	Data  any        `json:"data"`
	Error *APIError  `json:"error"`
}

type APIError struct {
	Code    string `json:"code"`
	Message string `json:"message"`
}

const (
	CodeInvalidRequest = "invalid_request"
	CodeNotFound       = "not_found"
	CodeForbidden      = "forbidden"
	CodeRateLimited    = "rate_limited"
	CodeInternal       = "internal"
)

// --- Shared sub-types ---

type Resolution struct {
	W int `json:"w"`
	H int `json:"h"`
}

// StreamView is the public read view of a directory entry. The owner
// token is intentionally absent; only the host that registered an
// entry ever sees its token (returned once by /register).
type StreamView struct {
	StreamID         string     `json:"streamId"`
	Name             string     `json:"name"`
	HostNickname     string     `json:"hostNickname,omitempty"`
	Shader           string     `json:"shader"`
	Resolution       Resolution `json:"resolution"`
	FPS              int        `json:"fps"`
	Codec            string     `json:"codec"`
	PasswordRequired bool       `json:"passwordRequired"`
	Endpoint         string     `json:"endpoint"`
	EndpointMode     string     `json:"endpointMode"`
	ClientCount      int        `json:"clientCount"`
	PublicIP         string     `json:"publicIp"`
	Version          string     `json:"version"`
	RegisteredAt     string     `json:"registeredAt"`     // RFC 3339
	LastHeartbeatAt  string     `json:"lastHeartbeatAt"`  // RFC 3339
	ExpiresAt        string     `json:"expiresAt"`        // RFC 3339
}

// --- Health ---

type HealthResponse struct {
	Status          string `json:"status"`
	ProtocolVersion int    `json:"protocol_version"`
}

// --- POST /register ---

type RegisterRequest struct {
	Name             string     `json:"name"`
	HostNickname     string     `json:"hostNickname"`
	Shader           string     `json:"shader"`
	Resolution       Resolution `json:"resolution"`
	FPS              int        `json:"fps"`
	Codec            string     `json:"codec"`
	PasswordRequired bool       `json:"passwordRequired"`
	Endpoint         string     `json:"endpoint"`
	EndpointMode     string     `json:"endpointMode"`
	Version          string     `json:"version"`
}

type RegisterResponse struct {
	StreamID   string `json:"streamId"`
	OwnerToken string `json:"ownerToken"`
	ExpiresAt  string `json:"expiresAt"`
}

// --- POST /heartbeat ---

type HeartbeatRequest struct {
	StreamID    string `json:"streamId"`
	OwnerToken  string `json:"ownerToken"`
	ClientCount *int   `json:"clientCount,omitempty"`
}

type HeartbeatResponse struct {
	ExpiresAt string `json:"expiresAt"`
}

// --- PATCH /streams/<id> ---

type PatchRequest struct {
	OwnerToken       string      `json:"ownerToken"`
	Name             *string     `json:"name,omitempty"`
	HostNickname     *string     `json:"hostNickname,omitempty"`
	Shader           *string     `json:"shader,omitempty"`
	Resolution       *Resolution `json:"resolution,omitempty"`
	FPS              *int        `json:"fps,omitempty"`
	Codec            *string     `json:"codec,omitempty"`
	PasswordRequired *bool       `json:"passwordRequired,omitempty"`
	Endpoint         *string     `json:"endpoint,omitempty"`
	EndpointMode     *string     `json:"endpointMode,omitempty"`
}

// --- DELETE /streams/<id> ---

type DeleteRequest struct {
	OwnerToken string `json:"ownerToken"`
}

// --- GET /streams ---

type ListResponse struct {
	Streams    []StreamView `json:"streams"`
	TotalCount int          `json:"totalCount"`
}

// --- POST /streams/<id>/report ---

type ReportRequest struct {
	Reason          string `json:"reason"`
	ReporterContact string `json:"reporterContact"`
}
