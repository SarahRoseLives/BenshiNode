package link

import (
	"bytes"
	"encoding/hex"
	"testing"
)

func TestAudioPacketRoundTrip(t *testing.T) {
	packet := AudioPacket{
		Type:     PacketAudio,
		Flags:    FlagStart,
		Session:  42,
		Sequence: 7,
		Payload:  []byte{1, 2, 3, 4, 5},
	}

	data := EncodeAudioPacket(packet)
	if len(data) != audioHeaderSize+len(packet.Payload) {
		t.Fatalf("encoded length = %d", len(data))
	}

	decoded, ok := DecodeAudioPacket(data)
	if !ok {
		t.Fatal("decode failed")
	}
	if decoded.Type != packet.Type ||
		decoded.Flags != packet.Flags ||
		decoded.Session != packet.Session ||
		decoded.Sequence != packet.Sequence {
		t.Fatalf("decoded header = %+v", decoded)
	}
	if !bytes.Equal(decoded.Payload, packet.Payload) {
		t.Fatalf("decoded payload = %v", decoded.Payload)
	}
}

func TestAudioPacketTruncatesLargePayload(t *testing.T) {
	payload := bytes.Repeat([]byte{0xAB}, MaxAudioPayload+256)
	data := EncodeAudioPacket(AudioPacket{Type: PacketAudio, Payload: payload})
	if len(data) != audioHeaderSize+MaxAudioPayload {
		t.Fatalf("encoded length = %d", len(data))
	}

	decoded, ok := DecodeAudioPacket(data)
	if !ok {
		t.Fatal("decode failed")
	}
	if len(decoded.Payload) != MaxAudioPayload {
		t.Fatalf("payload length = %d", len(decoded.Payload))
	}
}

func TestAudioPacketRejectsMalformed(t *testing.T) {
	valid := EncodeAudioPacket(AudioPacket{Type: PacketAudio, Payload: []byte{1}})

	tests := []struct {
		name string
		data []byte
	}{
		{name: "empty", data: nil},
		{name: "short header", data: valid[:audioHeaderSize-1]},
		{name: "bad magic", data: append([]byte{0, 0, 0, 0}, valid[4:]...)},
		{name: "payload past end", data: valid[:len(valid)-1]},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if _, ok := DecodeAudioPacket(test.data); ok {
				t.Fatal("decode unexpectedly succeeded")
			}
		})
	}
}

func TestAuthMessageRoundTrip(t *testing.T) {
	var challenge AuthChallenge
	for i := range challenge.Nonce {
		challenge.Nonce[i] = byte(i)
	}

	challengeData := EncodeChallenge(challenge)
	if len(challengeData) != challengeSize {
		t.Fatalf("challenge length = %d", len(challengeData))
	}
	if challengeData[4] != ProtocolVersion {
		t.Fatalf("protocol version = %d", challengeData[4])
	}
	decodedChallenge, ok := DecodeChallenge(challengeData)
	if !ok || decodedChallenge.Nonce != challenge.Nonce {
		t.Fatal("challenge round trip failed")
	}

	var digest [32]byte
	for i := range digest {
		digest[i] = byte(255 - i)
	}
	responseData := EncodeAuthResponse(AuthResponse{
		Callsign: "N0CALL",
		Digest:   digest,
	})
	if len(responseData) != responseSize {
		t.Fatalf("response length = %d", len(responseData))
	}
	decodedResponse, ok := DecodeAuthResponse(responseData)
	if !ok || decodedResponse.Callsign != "N0CALL" || decodedResponse.Digest != digest {
		t.Fatalf("response round trip failed: %+v", decodedResponse)
	}

	okData := EncodeAuthOK(AuthOK{Session: 1234, UDPPort: 7301})
	if len(okData) != authOKSize {
		t.Fatalf("auth ok length = %d", len(okData))
	}
	decodedOK, ok := DecodeAuthOK(okData)
	if !ok || decodedOK.Session != 1234 || decodedOK.UDPPort != 7301 {
		t.Fatalf("auth ok round trip failed: %+v", decodedOK)
	}

	failData := EncodeAuthFail()
	if len(failData) != authFailSize {
		t.Fatalf("auth fail length = %d", len(failData))
	}
}

func TestAuthCallsignTruncation(t *testing.T) {
	longCallsign := "ABCDEFGHIJKLMNOPQRST"
	data := EncodeAuthResponse(AuthResponse{Callsign: longCallsign})
	decoded, ok := DecodeAuthResponse(data)
	if !ok {
		t.Fatal("decode failed")
	}
	if decoded.Callsign != "ABCDEFGHIJKLMNO" {
		t.Fatalf("callsign = %q", decoded.Callsign)
	}
}

func TestAuthMessagesRejectBadMagic(t *testing.T) {
	challengeData := EncodeChallenge(AuthChallenge{})
	challengeData[0] = 0
	if _, ok := DecodeChallenge(challengeData); ok {
		t.Fatal("challenge with bad magic accepted")
	}

	responseData := EncodeAuthResponse(AuthResponse{})
	responseData[0] = 0
	if _, ok := DecodeAuthResponse(responseData); ok {
		t.Fatal("response with bad magic accepted")
	}

	okData := EncodeAuthOK(AuthOK{})
	okData[0] = 0
	if _, ok := DecodeAuthOK(okData); ok {
		t.Fatal("auth ok with bad magic accepted")
	}
}

func TestHMACSHA256(t *testing.T) {
	// RFC 4231 test case 2.
	digest := hmacSHA256([]byte("Jefe"), []byte("what do ya want for nothing?"))
	const expected = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"
	if hex.EncodeToString(digest[:]) != expected {
		t.Fatalf("digest = %x", digest)
	}
}

func TestConstantTimeEqual(t *testing.T) {
	a := []byte{1, 2, 3, 4}
	other := []byte{1, 2, 3, 4}
	different := []byte{1, 2, 3, 5}
	if !constantTimeEqual(a, other) {
		t.Fatal("equal values reported different")
	}
	if constantTimeEqual(a, different) {
		t.Fatal("different values reported equal")
	}
	if constantTimeEqual(a, []byte{1, 2, 3}) {
		t.Fatal("different lengths reported equal")
	}
}

func TestRandomBytes(t *testing.T) {
	var first [32]byte
	var second [32]byte
	if err := randomBytes(first[:]); err != nil {
		t.Fatalf("randomBytes: %v", err)
	}
	if err := randomBytes(second[:]); err != nil {
		t.Fatalf("randomBytes: %v", err)
	}
	if first == second {
		t.Fatal("two random reads were identical")
	}
}
