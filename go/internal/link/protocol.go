package link

import (
	"encoding/binary"
)

const (
	DefaultControlPort uint16 = 7300
	DefaultAudioPort   uint16 = 7301
	ProtocolVersion    uint8  = 1

	AuthMagic     uint32 = 0x31534E42 // "BNS1"
	AuthOKMagic   uint32 = 0x4B4F4E42 // "BNOK"
	AuthFailMagic uint32 = 0x52454E42 // "BNER"
	AudioMagic    uint32 = 0x31444E42 // "BND1"

	// MaxAudioPayload is the largest payload encodeAudioPacket will emit. The
	// C++ implementation truncates anything larger to preserve the 16-bit
	// length field and the 2 KiB receive buffer used by clients.
	MaxAudioPayload = 1024
)

const (
	audioHeaderSize = 16
	challengeSize   = 40
	responseSize    = 52
	authOKSize      = 12
	authFailSize    = 8
)

// PacketType identifies the kind of a UDP link packet.
type PacketType uint8

const (
	PacketRegister  PacketType = 1
	PacketAudio     PacketType = 2
	PacketEnd       PacketType = 3
	PacketHeartbeat PacketType = 4
	PacketServerEnd PacketType = 5
)

// PacketFlags carries stream markers on audio packets.
type PacketFlags uint8

const (
	FlagStart PacketFlags = 0x01
	FlagEnd   PacketFlags = 0x02
)

// AudioPacket is the UDP framing shared by clients and the reflector.
type AudioPacket struct {
	Type     PacketType
	Flags    PacketFlags
	Session  uint32
	Sequence uint32
	Payload  []byte
}

// AuthChallenge is the 40-byte server challenge.
type AuthChallenge struct {
	Nonce [32]byte
}

// AuthResponse is the 52-byte client response.
type AuthResponse struct {
	Callsign string
	Digest   [32]byte
}

// AuthOK is the 12-byte success reply.
type AuthOK struct {
	Session uint32
	UDPPort uint16
}

// EncodeAudioPacket serializes an AudioPacket. Payloads larger than
// MaxAudioPayload are truncated, matching the C++ implementation.
func EncodeAudioPacket(packet AudioPacket) []byte {
	payloadSize := len(packet.Payload)
	if payloadSize > MaxAudioPayload {
		payloadSize = MaxAudioPayload
	}

	data := make([]byte, audioHeaderSize+payloadSize)
	binary.LittleEndian.PutUint32(data[0:4], AudioMagic)
	data[4] = byte(packet.Type)
	data[5] = byte(packet.Flags)
	binary.LittleEndian.PutUint16(data[6:8], uint16(payloadSize))
	binary.LittleEndian.PutUint32(data[8:12], packet.Session)
	binary.LittleEndian.PutUint32(data[12:16], packet.Sequence)
	copy(data[audioHeaderSize:], packet.Payload[:payloadSize])
	return data
}

// DecodeAudioPacket parses a UDP link packet.
func DecodeAudioPacket(data []byte) (AudioPacket, bool) {
	if len(data) < audioHeaderSize {
		return AudioPacket{}, false
	}
	if binary.LittleEndian.Uint32(data[0:4]) != AudioMagic {
		return AudioPacket{}, false
	}

	payloadSize := int(binary.LittleEndian.Uint16(data[6:8]))
	if audioHeaderSize+payloadSize > len(data) {
		return AudioPacket{}, false
	}

	payload := make([]byte, payloadSize)
	copy(payload, data[audioHeaderSize:audioHeaderSize+payloadSize])

	return AudioPacket{
		Type:     PacketType(data[4]),
		Flags:    PacketFlags(data[5]),
		Session:  binary.LittleEndian.Uint32(data[8:12]),
		Sequence: binary.LittleEndian.Uint32(data[12:16]),
		Payload:  payload,
	}, true
}

// EncodeChallenge serializes an authentication challenge.
func EncodeChallenge(challenge AuthChallenge) []byte {
	data := make([]byte, challengeSize)
	binary.LittleEndian.PutUint32(data[0:4], AuthMagic)
	data[4] = ProtocolVersion
	copy(data[8:40], challenge.Nonce[:])
	return data
}

// DecodeChallenge parses an authentication challenge.
func DecodeChallenge(data []byte) (AuthChallenge, bool) {
	if len(data) < challengeSize {
		return AuthChallenge{}, false
	}
	if binary.LittleEndian.Uint32(data[0:4]) != AuthMagic {
		return AuthChallenge{}, false
	}

	var challenge AuthChallenge
	copy(challenge.Nonce[:], data[8:40])
	return challenge, true
}

// EncodeAuthResponse serializes an authentication response. Callsigns are
// truncated to 15 bytes, matching the C++ client and server.
func EncodeAuthResponse(response AuthResponse) []byte {
	data := make([]byte, responseSize)
	binary.LittleEndian.PutUint32(data[0:4], AuthMagic)

	callsign := []byte(response.Callsign)
	if len(callsign) > 15 {
		callsign = callsign[:15]
	}
	copy(data[4:19], callsign)
	copy(data[20:52], response.Digest[:])
	return data
}

// DecodeAuthResponse parses an authentication response.
func DecodeAuthResponse(data []byte) (AuthResponse, bool) {
	if len(data) < responseSize {
		return AuthResponse{}, false
	}
	if binary.LittleEndian.Uint32(data[0:4]) != AuthMagic {
		return AuthResponse{}, false
	}

	// The C++ implementation copies 15 bytes into a zeroed char[16], so the
	// callsign ends at the first NUL byte.
	callsignBytes := data[4:19]
	end := 0
	for end < len(callsignBytes) && callsignBytes[end] != 0 {
		end++
	}

	var response AuthResponse
	response.Callsign = string(callsignBytes[:end])
	copy(response.Digest[:], data[20:52])
	return response, true
}

// EncodeAuthOK serializes an authentication success reply.
func EncodeAuthOK(ok AuthOK) []byte {
	data := make([]byte, authOKSize)
	binary.LittleEndian.PutUint32(data[0:4], AuthOKMagic)
	binary.LittleEndian.PutUint32(data[4:8], ok.Session)
	binary.LittleEndian.PutUint16(data[8:10], ok.UDPPort)
	return data
}

// DecodeAuthOK parses an authentication success reply.
func DecodeAuthOK(data []byte) (AuthOK, bool) {
	if len(data) < authOKSize {
		return AuthOK{}, false
	}
	if binary.LittleEndian.Uint32(data[0:4]) != AuthOKMagic {
		return AuthOK{}, false
	}
	return AuthOK{
		Session: binary.LittleEndian.Uint32(data[4:8]),
		UDPPort: binary.LittleEndian.Uint16(data[8:10]),
	}, true
}

// EncodeAuthFail serializes an authentication failure reply.
func EncodeAuthFail() []byte {
	data := make([]byte, authFailSize)
	binary.LittleEndian.PutUint32(data[0:4], AuthFailMagic)
	return data
}
