package web

import (
	"bufio"
	"context"
	"crypto/sha1"
	"encoding/base64"
	"encoding/binary"
	"errors"
	"fmt"
	"io"
	"net"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"
)

const (
	wsOpText   byte = 0x1
	wsOpBinary byte = 0x2
	wsOpClose  byte = 0x8
	wsOpPing   byte = 0x9
	wsOpPong   byte = 0xA

	wsAcceptGUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
	wsMaxPayload = 64 * 1024
)

type wsConn struct {
	conn    net.Conn
	reader  *bufio.Reader
	writeMu sync.Mutex
}

func (c *wsConn) writeFrame(opcode byte, payload []byte) error {
	c.writeMu.Lock()
	defer c.writeMu.Unlock()

	var header [10]byte
	header[0] = 0x80 | (opcode & 0x0F)
	headerSize := 2
	switch length := len(payload); {
	case length < 126:
		header[1] = byte(length)
	case length <= 65535:
		header[1] = 126
		binary.BigEndian.PutUint16(header[2:4], uint16(length))
		headerSize = 4
	default:
		header[1] = 127
		binary.BigEndian.PutUint64(header[2:10], uint64(length))
		headerSize = 10
	}

	frame := make([]byte, headerSize+len(payload))
	copy(frame, header[:headerSize])
	copy(frame[headerSize:], payload)

	_ = c.conn.SetWriteDeadline(time.Now().Add(5 * time.Second))
	for len(frame) > 0 {
		written, err := c.conn.Write(frame)
		if err != nil {
			return err
		}
		frame = frame[written:]
	}
	return nil
}

func (c *wsConn) readFrame() (byte, []byte, error) {
	var header [2]byte
	if _, err := io.ReadFull(c.reader, header[:]); err != nil {
		return 0, nil, err
	}

	opcode := header[0] & 0x0F
	masked := header[1]&0x80 != 0
	length := uint64(header[1] & 0x7F)
	switch length {
	case 126:
		var extended [2]byte
		if _, err := io.ReadFull(c.reader, extended[:]); err != nil {
			return 0, nil, err
		}
		length = uint64(binary.BigEndian.Uint16(extended[:]))
	case 127:
		var extended [8]byte
		if _, err := io.ReadFull(c.reader, extended[:]); err != nil {
			return 0, nil, err
		}
		length = binary.BigEndian.Uint64(extended[:])
	}

	if length > wsMaxPayload {
		return 0, nil, fmt.Errorf("websocket frame too large: %d", length)
	}
	if !masked {
		return 0, nil, errors.New("client websocket frame is not masked")
	}

	var mask [4]byte
	if _, err := io.ReadFull(c.reader, mask[:]); err != nil {
		return 0, nil, err
	}

	payload := make([]byte, int(length))
	if _, err := io.ReadFull(c.reader, payload); err != nil {
		return 0, nil, err
	}
	for i := range payload {
		payload[i] ^= mask[i%4]
	}
	return opcode, payload, nil
}

func (c *wsConn) readLoop() {
	for {
		opcode, payload, err := c.readFrame()
		if err != nil {
			return
		}
		switch opcode {
		case wsOpClose:
			_ = c.writeFrame(wsOpClose, payload)
			return
		case wsOpPing:
			_ = c.writeFrame(wsOpPong, payload)
		case wsOpPong:
			// Keepalive reply; nothing to do.
		case wsOpText, wsOpBinary:
			// Clients do not send audio; ignore application data.
		}
	}
}

func (s *Server) handleAudioWebSocket(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if !strings.EqualFold(strings.TrimSpace(r.Header.Get("Upgrade")), "websocket") ||
		!headerContainsToken(r.Header, "Connection", "upgrade") {
		http.Error(w, "expected websocket upgrade", http.StatusBadRequest)
		return
	}
	if r.Header.Get("Sec-WebSocket-Version") != "13" {
		w.Header().Set("Sec-WebSocket-Version", "13")
		http.Error(w, "unsupported websocket version", http.StatusUpgradeRequired)
		return
	}
	key := strings.TrimSpace(r.Header.Get("Sec-WebSocket-Key"))
	if key == "" {
		http.Error(w, "missing websocket key", http.StatusBadRequest)
		return
	}
	if origin := r.Header.Get("Origin"); origin != "" {
		parsed, err := url.Parse(origin)
		if err != nil || !strings.EqualFold(parsed.Host, r.Host) {
			http.Error(w, "websocket origin not allowed", http.StatusForbidden)
			return
		}
	}

	hijacker, ok := w.(http.Hijacker)
	if !ok {
		http.Error(w, "websocket unsupported", http.StatusInternalServerError)
		return
	}

	s.wsWG.Add(1)
	defer s.wsWG.Done()

	netConn, readWriter, err := hijacker.Hijack()
	if err != nil {
		return
	}
	defer func() { _ = netConn.Close() }()

	s.trackWSConn(netConn, true)
	defer s.trackWSConn(netConn, false)

	response := "HTTP/1.1 101 Switching Protocols\r\n" +
		"Upgrade: websocket\r\n" +
		"Connection: Upgrade\r\n" +
		"Sec-WebSocket-Accept: " + websocketAccept(key) + "\r\n\r\n"
	if _, err := readWriter.WriteString(response); err != nil {
		return
	}
	if err := readWriter.Flush(); err != nil {
		return
	}

	ws := &wsConn{conn: netConn, reader: readWriter.Reader}
	ctx, cancel := context.WithCancel(r.Context())
	defer cancel()

	go func() {
		ws.readLoop()
		cancel()
		_ = netConn.Close()
	}()

	frames, unsubscribe := s.reflector.SubscribeAudio(16)
	defer unsubscribe()

	ping := time.NewTicker(20 * time.Second)
	defer ping.Stop()

	var raw [samplesPerFrame]byte
	for {
		select {
		case <-ctx.Done():
			return
		case <-s.baseCtx.Done():
			return
		case <-ping.C:
			if err := ws.writeFrame(wsOpPing, nil); err != nil {
				return
			}
		case frame := <-frames:
			for i := range raw {
				raw[i] = silenceULaw
			}
			copy(raw[:], frame)
			if err := ws.writeFrame(wsOpBinary, raw[:]); err != nil {
				return
			}
		}
	}
}

func websocketAccept(key string) string {
	sum := sha1.Sum([]byte(key + wsAcceptGUID))
	return base64.StdEncoding.EncodeToString(sum[:])
}

func headerContainsToken(header http.Header, name, token string) bool {
	for _, value := range header.Values(name) {
		for _, part := range strings.Split(value, ",") {
			if strings.EqualFold(strings.TrimSpace(part), token) {
				return true
			}
		}
	}
	return false
}

func (s *Server) trackWSConn(conn net.Conn, add bool) {
	s.mu.Lock()
	if add {
		s.wsConns[conn] = struct{}{}
	} else {
		delete(s.wsConns, conn)
	}
	s.mu.Unlock()
}

func (s *Server) closeWSConns() {
	s.mu.Lock()
	conns := make([]net.Conn, 0, len(s.wsConns))
	for conn := range s.wsConns {
		conns = append(conns, conn)
	}
	s.mu.Unlock()

	for _, conn := range conns {
		_ = conn.Close()
	}
}
