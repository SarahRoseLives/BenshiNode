package web

import (
	"bufio"
	"bytes"
	"context"
	"encoding/base64"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"strings"
	"testing"
	"time"

	"benshinode/internal/link"
	"benshinode/internal/logging"
)

func startTestServers(t *testing.T) (*link.Server, *Server) {
	t.Helper()

	logger := logging.New(logging.Error, io.Discard)
	reflector := link.NewServer(link.ServerConfig{
		ControlPort: 0,
		AudioPort:   0,
		Key:         "",
		MaxClients:  4,
	}, logger)
	if err := reflector.Start(); err != nil {
		t.Fatalf("start reflector: %v", err)
	}
	t.Cleanup(reflector.Close)

	webServer := New(reflector, logger)
	if err := webServer.Start("127.0.0.1:0"); err != nil {
		t.Fatalf("start web server: %v", err)
	}
	t.Cleanup(webServer.Close)

	return reflector, webServer
}

type testClient struct {
	session uint32
	conn    *net.UDPConn
}

func connectTestClient(t *testing.T, reflector *link.Server) *testClient {
	t.Helper()

	controlConn, err := net.DialTimeout(
		"tcp4", reflector.ControlAddr().String(), 2*time.Second)
	if err != nil {
		t.Fatalf("dial control: %v", err)
	}
	defer func() { _ = controlConn.Close() }()
	_ = controlConn.SetDeadline(time.Now().Add(2 * time.Second))

	challengeData := make([]byte, 40)
	if _, err := io.ReadFull(controlConn, challengeData); err != nil {
		t.Fatalf("read challenge: %v", err)
	}
	if _, ok := link.DecodeChallenge(challengeData); !ok {
		t.Fatal("decode challenge failed")
	}

	response := link.EncodeAuthResponse(link.AuthResponse{Callsign: "WEBTEST"})
	if _, err := controlConn.Write(response); err != nil {
		t.Fatalf("write auth response: %v", err)
	}

	okData := make([]byte, 12)
	if _, err := io.ReadFull(controlConn, okData); err != nil {
		t.Fatalf("read auth ok: %v", err)
	}
	auth, ok := link.DecodeAuthOK(okData)
	if !ok {
		t.Fatal("decode auth ok failed")
	}

	audioAddr := reflector.AudioAddr().(*net.UDPAddr)
	if auth.UDPPort != uint16(audioAddr.Port) {
		t.Fatalf("auth UDP port = %d, want %d", auth.UDPPort, audioAddr.Port)
	}
	udpConn, err := net.DialUDP("udp4", nil, &net.UDPAddr{
		IP:   net.ParseIP("127.0.0.1"),
		Port: audioAddr.Port,
	})
	if err != nil {
		t.Fatalf("dial audio: %v", err)
	}
	t.Cleanup(func() { _ = udpConn.Close() })

	register := link.AudioPacket{Type: link.PacketRegister, Session: auth.Session}
	if _, err := udpConn.Write(link.EncodeAudioPacket(register)); err != nil {
		t.Fatalf("write register: %v", err)
	}
	heartbeat := link.AudioPacket{Type: link.PacketHeartbeat, Session: auth.Session}
	if _, err := udpConn.Write(link.EncodeAudioPacket(heartbeat)); err != nil {
		t.Fatalf("write heartbeat: %v", err)
	}
	_ = udpConn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buffer := make([]byte, 2048)
	size, err := udpConn.Read(buffer)
	if err != nil {
		t.Fatalf("read heartbeat reply: %v", err)
	}
	if reply, ok := link.DecodeAudioPacket(buffer[:size]); !ok || reply.Type != link.PacketHeartbeat {
		t.Fatalf("heartbeat reply = %+v", reply)
	}

	return &testClient{session: auth.Session, conn: udpConn}
}

func (c *testClient) sendAudio(t *testing.T, frames int) <-chan struct{} {
	t.Helper()

	done := make(chan struct{})
	go func() {
		defer close(done)
		for i := 0; i < frames; i++ {
			packet := link.AudioPacket{
				Type:     link.PacketAudio,
				Flags:    link.FlagStart,
				Session:  c.session,
				Sequence: uint32(i),
				Payload:  bytes.Repeat([]byte{0x00}, 160),
			}
			if _, err := c.conn.Write(link.EncodeAudioPacket(packet)); err != nil {
				return
			}
			time.Sleep(20 * time.Millisecond)
		}
	}()
	return done
}

func openStream(t *testing.T, webServer *Server, path string) (*http.Response, context.CancelFunc) {
	t.Helper()

	ctx, cancel := context.WithCancel(context.Background())
	request, err := http.NewRequestWithContext(ctx, http.MethodGet,
		"http://"+webServer.Addr().String()+path, nil)
	if err != nil {
		cancel()
		t.Fatalf("build %s request: %v", path, err)
	}
	stream, err := http.DefaultClient.Do(request)
	if err != nil {
		cancel()
		t.Fatalf("open %s stream: %v", path, err)
	}
	t.Cleanup(func() {
		cancel()
		_ = stream.Body.Close()
	})
	return stream, cancel
}

func TestIndexStatsAndHealth(t *testing.T) {
	reflector, webServer := startTestServers(t)
	baseURL := "http://" + webServer.Addr().String()

	response, err := http.Get(baseURL + "/")
	if err != nil {
		t.Fatalf("get index: %v", err)
	}
	body, _ := io.ReadAll(response.Body)
	_ = response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("index status = %d", response.StatusCode)
	}
	if !bytes.Contains(body, []byte("BenshiNode Reflector")) {
		t.Fatal("index page missing title")
	}

	response, err = http.Get(baseURL + "/audio-worklet.js")
	if err != nil {
		t.Fatalf("get audio worklet: %v", err)
	}
	body, _ = io.ReadAll(response.Body)
	_ = response.Body.Close()
	if response.StatusCode != http.StatusOK {
		t.Fatalf("audio worklet status = %d", response.StatusCode)
	}
	if !bytes.Contains(body, []byte("registerProcessor")) {
		t.Fatal("audio worklet missing processor registration")
	}

	response, err = http.Get(baseURL + "/api/stats")
	if err != nil {
		t.Fatalf("get stats: %v", err)
	}
	if response.StatusCode != http.StatusOK {
		t.Fatalf("stats status = %d", response.StatusCode)
	}
	var stats link.Stats
	if err := json.NewDecoder(response.Body).Decode(&stats); err != nil {
		t.Fatalf("decode stats: %v", err)
	}
	_ = response.Body.Close()
	if stats.MaxClients != 4 {
		t.Fatalf("max clients = %d", stats.MaxClients)
	}
	if stats.ControlAddress != reflector.ControlAddr().String() {
		t.Fatalf("control address = %q", stats.ControlAddress)
	}
	if stats.AudioAddress != reflector.AudioAddr().String() {
		t.Fatalf("audio address = %q", stats.AudioAddress)
	}

	response, err = http.Get(baseURL + "/healthz")
	if err != nil {
		t.Fatalf("get health: %v", err)
	}
	body, _ = io.ReadAll(response.Body)
	_ = response.Body.Close()
	if string(body) != "ok\n" {
		t.Fatalf("health body = %q", body)
	}

	response, err = http.Get(baseURL + "/missing")
	if err != nil {
		t.Fatalf("get missing: %v", err)
	}
	_ = response.Body.Close()
	if response.StatusCode != http.StatusNotFound {
		t.Fatalf("missing status = %d", response.StatusCode)
	}
}

func TestWAVAudioStreamDeliversReflectorAudio(t *testing.T) {
	reflector, webServer := startTestServers(t)
	client := connectTestClient(t, reflector)

	stream, cancel := openStream(t, webServer, "/audio")
	if stream.StatusCode != http.StatusOK {
		t.Fatalf("audio status = %d", stream.StatusCode)
	}
	if contentType := stream.Header.Get("Content-Type"); !strings.HasPrefix(contentType, "audio/wav") {
		t.Fatalf("audio content type = %q", contentType)
	}

	header := make([]byte, wavHeaderSize)
	if _, err := io.ReadFull(stream.Body, header); err != nil {
		t.Fatalf("read WAV header: %v", err)
	}
	if string(header[0:4]) != "RIFF" || string(header[8:12]) != "WAVE" {
		t.Fatalf("WAV header = %x", header[:12])
	}
	if channels := binary.LittleEndian.Uint16(header[22:24]); channels != 1 {
		t.Fatalf("WAV channels = %d", channels)
	}
	if rate := binary.LittleEndian.Uint32(header[24:28]); rate != sampleRate {
		t.Fatalf("WAV sample rate = %d", rate)
	}
	if bits := binary.LittleEndian.Uint16(header[34:36]); bits != 16 {
		t.Fatalf("WAV bits = %d", bits)
	}

	senderDone := client.sendAudio(t, 40)

	pcm := make([]byte, 3200) // 200 ms of 16-bit, 8 kHz mono
	if _, err := io.ReadFull(stream.Body, pcm); err != nil {
		t.Fatalf("read PCM: %v", err)
	}
	nonZero := false
	for _, sample := range pcm {
		if sample != 0 {
			nonZero = true
			break
		}
	}
	if !nonZero {
		t.Fatal("audio stream contained only silence")
	}

	cancel()
	<-senderDone
}

func TestRawAudioStreamDeliversReflectorAudio(t *testing.T) {
	reflector, webServer := startTestServers(t)
	client := connectTestClient(t, reflector)

	stream, cancel := openStream(t, webServer, "/audio/raw")
	if stream.StatusCode != http.StatusOK {
		t.Fatalf("raw audio status = %d", stream.StatusCode)
	}
	if contentType := stream.Header.Get("Content-Type"); contentType != "application/octet-stream" {
		t.Fatalf("raw audio content type = %q", contentType)
	}

	senderDone := client.sendAudio(t, 40)

	ulaw := make([]byte, 1600) // 200 ms of 8 kHz u-law
	if _, err := io.ReadFull(stream.Body, ulaw); err != nil {
		t.Fatalf("read raw audio: %v", err)
	}
	heardAudio := false
	for _, sample := range ulaw {
		if sample != 0xFF { // 0xFF is u-law silence
			heardAudio = true
			break
		}
	}
	if !heardAudio {
		t.Fatal("raw audio stream contained only silence")
	}

	cancel()
	<-senderDone
}

func TestWebSocketAudioStreamDeliversReflectorAudio(t *testing.T) {
	reflector, webServer := startTestServers(t)

	conn, err := net.DialTimeout("tcp4", webServer.Addr().String(), 2*time.Second)
	if err != nil {
		t.Fatalf("dial web server: %v", err)
	}
	defer func() { _ = conn.Close() }()

	key := base64.StdEncoding.EncodeToString([]byte("0123456789abcdef"))
	request := fmt.Sprintf(
		"GET /audio/ws HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"+
			"Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"+
			"Sec-WebSocket-Version: 13\r\n\r\n",
		webServer.Addr().String(), key)
	if _, err := io.WriteString(conn, request); err != nil {
		t.Fatalf("write websocket handshake: %v", err)
	}

	reader := bufio.NewReader(conn)
	statusLine, err := reader.ReadString('\n')
	if err != nil {
		t.Fatalf("read status line: %v", err)
	}
	if !strings.Contains(statusLine, "101") {
		t.Fatalf("websocket status = %q", strings.TrimSpace(statusLine))
	}

	headers := make(map[string]string)
	for {
		line, err := reader.ReadString('\n')
		if err != nil {
			t.Fatalf("read websocket headers: %v", err)
		}
		if line == "\r\n" {
			break
		}
		parts := strings.SplitN(line, ":", 2)
		if len(parts) == 2 {
			headers[strings.ToLower(strings.TrimSpace(parts[0]))] = strings.TrimSpace(parts[1])
		}
	}
	if got := headers["sec-websocket-accept"]; got != websocketAccept(key) {
		t.Fatalf("Sec-WebSocket-Accept = %q", got)
	}

	client := connectTestClient(t, reflector)
	senderDone := client.sendAudio(t, 40)

	for {
		opcode, payload, err := readServerWSFrame(reader)
		if err != nil {
			t.Fatalf("read websocket frame: %v", err)
		}
		if opcode == wsOpPing {
			continue
		}
		if opcode != wsOpBinary {
			t.Fatalf("websocket opcode = %d", opcode)
		}
		if len(payload) != 160 {
			t.Fatalf("websocket payload length = %d", len(payload))
		}
		heardAudio := false
		for _, sample := range payload {
			if sample != 0xFF {
				heardAudio = true
				break
			}
		}
		if !heardAudio {
			t.Fatal("websocket audio contained only silence")
		}
		break
	}

	<-senderDone
}

func readServerWSFrame(reader *bufio.Reader) (byte, []byte, error) {
	var header [2]byte
	if _, err := io.ReadFull(reader, header[:]); err != nil {
		return 0, nil, err
	}

	opcode := header[0] & 0x0F
	length := uint64(header[1] & 0x7F)
	switch length {
	case 126:
		var extended [2]byte
		if _, err := io.ReadFull(reader, extended[:]); err != nil {
			return 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(extended[:]))
	case 127:
		var extended [8]byte
		if _, err := io.ReadFull(reader, extended[:]); err != nil {
			return 0, nil, err
		}
		length = binary.BigEndian.Uint64(extended[:])
	}

	payload := make([]byte, int(length))
	if _, err := io.ReadFull(reader, payload); err != nil {
		return 0, nil, err
	}
	return opcode, payload, nil
}
