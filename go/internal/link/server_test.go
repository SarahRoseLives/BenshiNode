package link

import (
	"bytes"
	"encoding/binary"
	"io"
	"net"
	"testing"
	"time"

	"benshinode/internal/logging"
)

const testKey = "test-shared-key"

func startTestServer(t *testing.T, key string, maxClients int) *Server {
	t.Helper()

	logger := logging.New(logging.Error, io.Discard)
	server := NewServer(ServerConfig{
		ControlPort: 0,
		AudioPort:   0,
		Key:         key,
		MaxClients:  maxClients,
	}, logger)
	if err := server.Start(); err != nil {
		t.Fatalf("start server: %v", err)
	}
	t.Cleanup(server.Close)
	return server
}

type handshakeResult struct {
	session uint32
	udpPort uint16
	failed  bool
}

func handshake(t *testing.T, server *Server, key, callsign string) handshakeResult {
	t.Helper()

	conn, err := net.DialTimeout("tcp4", server.ControlAddr().String(), 2*time.Second)
	if err != nil {
		t.Fatalf("dial control: %v", err)
	}
	defer func() { _ = conn.Close() }()
	_ = conn.SetDeadline(time.Now().Add(2 * time.Second))

	challengeData := make([]byte, challengeSize)
	if _, err := io.ReadFull(conn, challengeData); err != nil {
		t.Fatalf("read challenge: %v", err)
	}
	challenge, ok := DecodeChallenge(challengeData)
	if !ok {
		t.Fatal("decode challenge failed")
	}

	var digest [32]byte
	if key != "" {
		digest = hmacSHA256([]byte(key), challenge.Nonce[:])
	}
	response := EncodeAuthResponse(AuthResponse{
		Callsign: callsign,
		Digest:   digest,
	})
	if err := writeAll(conn, response); err != nil {
		t.Fatalf("write response: %v", err)
	}

	header := make([]byte, 4)
	if _, err := io.ReadFull(conn, header); err != nil {
		t.Fatalf("read auth result: %v", err)
	}

	switch binary.LittleEndian.Uint32(header) {
	case AuthOKMagic:
		rest := make([]byte, authOKSize-4)
		if _, err := io.ReadFull(conn, rest); err != nil {
			t.Fatalf("read auth ok: %v", err)
		}
		result, ok := DecodeAuthOK(append(header, rest...))
		if !ok {
			t.Fatal("decode auth ok failed")
		}
		audioPort := uint16(server.AudioAddr().(*net.UDPAddr).Port)
		if result.UDPPort != audioPort {
			t.Fatalf("auth ok UDP port = %d, want %d", result.UDPPort, audioPort)
		}
		return handshakeResult{session: result.Session, udpPort: result.UDPPort}

	case AuthFailMagic:
		rest := make([]byte, authFailSize-4)
		if _, err := io.ReadFull(conn, rest); err != nil {
			t.Fatalf("read auth fail: %v", err)
		}
		return handshakeResult{failed: true}

	default:
		t.Fatalf("unexpected auth magic %08x", binary.LittleEndian.Uint32(header))
		return handshakeResult{}
	}
}

func dialAudio(t *testing.T, server *Server, udpPort uint16) *net.UDPConn {
	t.Helper()

	conn, err := net.DialUDP("udp4", nil, &net.UDPAddr{
		IP:   net.ParseIP("127.0.0.1"),
		Port: int(udpPort),
	})
	if err != nil {
		t.Fatalf("dial audio: %v", err)
	}
	t.Cleanup(func() { _ = conn.Close() })
	return conn
}

func registerSession(t *testing.T, conn *net.UDPConn, session uint32) {
	t.Helper()

	register := AudioPacket{Type: PacketRegister, Session: session}
	if _, err := conn.Write(EncodeAudioPacket(register)); err != nil {
		t.Fatalf("write register: %v", err)
	}

	// A heartbeat proves the server processed the registration and gives the
	// session a known UDP endpoint for relayed audio.
	heartbeat := AudioPacket{Type: PacketHeartbeat, Session: session}
	if _, err := conn.Write(EncodeAudioPacket(heartbeat)); err != nil {
		t.Fatalf("write heartbeat: %v", err)
	}

	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buffer := make([]byte, maxDatagramSize)
	size, err := conn.Read(buffer)
	if err != nil {
		t.Fatalf("read heartbeat reply: %v", err)
	}
	reply, ok := DecodeAudioPacket(buffer[:size])
	if !ok || reply.Type != PacketHeartbeat {
		t.Fatalf("heartbeat reply = %+v", reply)
	}
	_ = conn.SetReadDeadline(time.Time{})
}

func readPacket(t *testing.T, conn *net.UDPConn, timeout time.Duration) AudioPacket {
	t.Helper()

	_ = conn.SetReadDeadline(time.Now().Add(timeout))
	buffer := make([]byte, maxDatagramSize)
	size, err := conn.Read(buffer)
	if err != nil {
		t.Fatalf("read audio: %v", err)
	}
	packet, ok := DecodeAudioPacket(buffer[:size])
	if !ok {
		t.Fatal("decode audio failed")
	}
	return packet
}

func TestServerAuthenticatesAndRelaysAudio(t *testing.T) {
	server := startTestServer(t, testKey, 4)

	first := handshake(t, server, testKey, "N0CALL")
	if first.failed || first.session == 0 {
		t.Fatalf("first handshake = %+v", first)
	}
	second := handshake(t, server, testKey, "N1CALL")
	if second.failed || second.session == 0 || second.session == first.session {
		t.Fatalf("second handshake = %+v", second)
	}
	if first.udpPort != second.udpPort {
		t.Fatalf("UDP ports differ: %d and %d", first.udpPort, second.udpPort)
	}

	firstConn := dialAudio(t, server, first.udpPort)
	secondConn := dialAudio(t, server, second.udpPort)
	registerSession(t, firstConn, first.session)
	registerSession(t, secondConn, second.session)

	audio := AudioPacket{
		Type:     PacketAudio,
		Flags:    FlagStart,
		Session:  first.session,
		Sequence: 7,
		Payload:  bytes.Repeat([]byte{0x7F}, 160),
	}
	if _, err := firstConn.Write(EncodeAudioPacket(audio)); err != nil {
		t.Fatalf("write audio: %v", err)
	}

	relayed := readPacket(t, secondConn, 2*time.Second)
	if relayed.Type != PacketAudio ||
		relayed.Session != audio.Session ||
		relayed.Sequence != audio.Sequence ||
		relayed.Flags != audio.Flags ||
		!bytes.Equal(relayed.Payload, audio.Payload) {
		t.Fatalf("relayed packet = %+v", relayed)
	}

	// The sender must not receive its own audio back.
	_ = firstConn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, err := firstConn.Read(make([]byte, maxDatagramSize)); err == nil {
		t.Fatal("sender received its own audio")
	}

	end := AudioPacket{Type: PacketEnd, Session: first.session, Sequence: 8}
	if _, err := firstConn.Write(EncodeAudioPacket(end)); err != nil {
		t.Fatalf("write end: %v", err)
	}
	relayedEnd := readPacket(t, secondConn, 2*time.Second)
	if relayedEnd.Type != PacketEnd || relayedEnd.Session != first.session {
		t.Fatalf("relayed end = %+v", relayedEnd)
	}

	// After End the other session may become the talker.
	audioBack := AudioPacket{
		Type:     PacketAudio,
		Flags:    FlagStart,
		Session:  second.session,
		Sequence: 1,
		Payload:  bytes.Repeat([]byte{0x22}, 160),
	}
	if _, err := secondConn.Write(EncodeAudioPacket(audioBack)); err != nil {
		t.Fatalf("write return audio: %v", err)
	}
	received := readPacket(t, firstConn, 2*time.Second)
	if received.Session != second.session || !bytes.Equal(received.Payload, audioBack.Payload) {
		t.Fatalf("return packet = %+v", received)
	}
}

func TestServerRejectsInvalidKey(t *testing.T) {
	server := startTestServer(t, "correct-key", 4)
	result := handshake(t, server, "wrong-key", "BADKEY")
	if !result.failed {
		t.Fatal("handshake with invalid key succeeded")
	}
}

func TestServerAllowsEmptyKey(t *testing.T) {
	server := startTestServer(t, "", 4)
	result := handshake(t, server, "", "OPEN")
	if result.failed {
		t.Fatal("handshake with empty key failed")
	}
}

func TestServerEnforcesMaxClients(t *testing.T) {
	server := startTestServer(t, testKey, 1)

	first := handshake(t, server, testKey, "FIRST")
	if first.failed {
		t.Fatal("first handshake failed")
	}

	conn, err := net.DialTimeout("tcp4", server.ControlAddr().String(), 2*time.Second)
	if err != nil {
		t.Fatalf("dial control: %v", err)
	}
	defer func() { _ = conn.Close() }()
	_ = conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	if _, err := conn.Read(make([]byte, 1)); err == nil {
		t.Fatal("second client was not rejected")
	}
}

func TestServerTalkerTimeout(t *testing.T) {
	server := startTestServer(t, testKey, 4)

	first := handshake(t, server, testKey, "FIRST")
	second := handshake(t, server, testKey, "SECOND")
	firstConn := dialAudio(t, server, first.udpPort)
	secondConn := dialAudio(t, server, second.udpPort)
	registerSession(t, firstConn, first.session)
	registerSession(t, secondConn, second.session)

	firstAudio := AudioPacket{
		Type:     PacketAudio,
		Flags:    FlagStart,
		Session:  first.session,
		Sequence: 1,
		Payload:  bytes.Repeat([]byte{0x11}, 160),
	}
	if _, err := firstConn.Write(EncodeAudioPacket(firstAudio)); err != nil {
		t.Fatalf("write first audio: %v", err)
	}
	if packet := readPacket(t, secondConn, 2*time.Second); packet.Session != first.session {
		t.Fatalf("first relay = %+v", packet)
	}

	// A second talker is ignored while the first talker is active.
	blocked := AudioPacket{
		Type:     PacketAudio,
		Flags:    FlagStart,
		Session:  second.session,
		Sequence: 1,
		Payload:  bytes.Repeat([]byte{0x22}, 160),
	}
	if _, err := secondConn.Write(EncodeAudioPacket(blocked)); err != nil {
		t.Fatalf("write blocked audio: %v", err)
	}
	_ = firstConn.SetReadDeadline(time.Now().Add(200 * time.Millisecond))
	if _, err := firstConn.Read(make([]byte, maxDatagramSize)); err == nil {
		t.Fatal("second talker was relayed while first talker was active")
	}

	time.Sleep(talkerTimeout + 150*time.Millisecond)

	takeover := AudioPacket{
		Type:     PacketAudio,
		Flags:    FlagStart,
		Session:  second.session,
		Sequence: 2,
		Payload:  bytes.Repeat([]byte{0x33}, 160),
	}
	if _, err := secondConn.Write(EncodeAudioPacket(takeover)); err != nil {
		t.Fatalf("write takeover audio: %v", err)
	}
	received := readPacket(t, firstConn, 2*time.Second)
	if received.Session != second.session || !bytes.Equal(received.Payload, takeover.Payload) {
		t.Fatalf("takeover packet = %+v", received)
	}
}

func TestServerStatsAndListener(t *testing.T) {
	server := startTestServer(t, testKey, 4)

	if stats := server.Stats(); stats.SessionCount != 0 || stats.Listeners != 0 {
		t.Fatalf("initial stats = %+v", stats)
	}

	frames, unsubscribe := server.SubscribeAudio(8)
	defer unsubscribe()

	if stats := server.Stats(); stats.Listeners != 1 {
		t.Fatalf("listener count = %d", stats.Listeners)
	}

	client := handshake(t, server, testKey, "STATS")
	conn := dialAudio(t, server, client.udpPort)
	registerSession(t, conn, client.session)

	audio := AudioPacket{
		Type:     PacketAudio,
		Flags:    FlagStart,
		Session:  client.session,
		Sequence: 3,
		Payload:  bytes.Repeat([]byte{0x55}, 160),
	}
	if _, err := conn.Write(EncodeAudioPacket(audio)); err != nil {
		t.Fatalf("write audio: %v", err)
	}

	select {
	case payload := <-frames:
		if !bytes.Equal(payload, audio.Payload) {
			t.Fatalf("listener payload = %v", payload)
		}
	case <-time.After(2 * time.Second):
		t.Fatal("timed out waiting for listener audio")
	}

	stats := server.Stats()
	if stats.SessionCount != 1 {
		t.Fatalf("session count = %d", stats.SessionCount)
	}
	if stats.ActiveTalker != client.session || stats.ActiveTalkerCallsign != "STATS" {
		t.Fatalf("active talker = %+v", stats)
	}
	if stats.Authenticated != 1 {
		t.Fatalf("authenticated = %d", stats.Authenticated)
	}
	if stats.AudioPackets != 1 || stats.AudioBytes != uint64(len(audio.Payload)) {
		t.Fatalf("audio counters = %d/%d", stats.AudioPackets, stats.AudioBytes)
	}
	if len(stats.Sessions) != 1 || stats.Sessions[0].Callsign != "STATS" {
		t.Fatalf("session snapshot = %+v", stats.Sessions)
	}
	if len(stats.Events) == 0 {
		t.Fatal("no events recorded")
	}

	unsubscribe()
	if stats := server.Stats(); stats.Listeners != 0 {
		t.Fatalf("listener count after unsubscribe = %d", stats.Listeners)
	}
}
