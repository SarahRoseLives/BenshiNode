package link

import (
	"context"
	"fmt"
	"io"
	"net"
	"sort"
	"sync"
	"time"

	"benshinode/internal/logging"
)

const (
	maxAuthDuration = 4 * time.Second
	sessionTimeout  = 30 * time.Second
	talkerTimeout   = 600 * time.Millisecond
	cleanupInterval = 5 * time.Second
	maxDatagramSize = 2048
	maxEventHistory = 100
)

// ServerConfig controls the reflector listener and authentication.
type ServerConfig struct {
	// ControlPort is the TCP authentication port. Zero selects an ephemeral
	// port, which is useful for tests.
	ControlPort int
	// AudioPort is the UDP relay port. Zero selects an ephemeral port.
	AudioPort int
	// Key is the shared HMAC key. An empty key allows unauthenticated clients.
	Key string
	// MaxClients limits authenticated sessions plus in-flight handshakes.
	MaxClients int
}

func (c ServerConfig) normalized() ServerConfig {
	if c.MaxClients < 1 {
		c.MaxClients = 16
	}
	return c
}

type session struct {
	id           uint32
	callsign     string
	addr         net.UDPAddr
	hasAddr      bool
	connectedAt  time.Time
	lastSeen     time.Time
	audioPackets uint64
	audioBytes   uint64
}

type counters struct {
	authenticated  uint64
	rejected       uint64
	expired        uint64
	audioPackets   uint64
	audioBytes     uint64
	relayedPackets uint64
	relayedBytes   uint64
	heartbeats     uint64
	audioDropped   uint64
}

// Event is a recent reflector activity entry.
type Event struct {
	Time    time.Time `json:"time"`
	Level   string    `json:"level"`
	Message string    `json:"message"`
}

// SessionStats describes one authenticated link session.
type SessionStats struct {
	ID           uint32    `json:"id"`
	Callsign     string    `json:"callsign"`
	Remote       string    `json:"remote"`
	ConnectedAt  time.Time `json:"connected_at"`
	LastSeen     time.Time `json:"last_seen"`
	Talking      bool      `json:"talking"`
	AudioPackets uint64    `json:"audio_packets"`
	AudioBytes   uint64    `json:"audio_bytes"`
}

// Stats is a point-in-time snapshot of reflector activity.
type Stats struct {
	StartedAt            time.Time      `json:"started_at"`
	UptimeSeconds        int64          `json:"uptime_seconds"`
	ControlAddress       string         `json:"control_address"`
	AudioAddress         string         `json:"audio_address"`
	MaxClients           int            `json:"max_clients"`
	SessionCount         int            `json:"session_count"`
	Sessions             []SessionStats `json:"sessions"`
	ActiveTalker         uint32         `json:"active_talker"`
	ActiveTalkerCallsign string         `json:"active_talker_callsign"`
	Listeners            int            `json:"listeners"`
	Authenticated        uint64         `json:"authenticated"`
	Rejected             uint64         `json:"rejected"`
	Expired              uint64         `json:"expired"`
	AudioPackets         uint64         `json:"audio_packets"`
	AudioBytes           uint64         `json:"audio_bytes"`
	RelayedPackets       uint64         `json:"relayed_packets"`
	RelayedBytes         uint64         `json:"relayed_bytes"`
	Heartbeats           uint64         `json:"heartbeats"`
	AudioDropped         uint64         `json:"audio_dropped"`
	Events               []Event        `json:"events"`
}

// Server is a BenshiNode link reflector. It accepts TCP authentication
// handshakes and relays G.711 u-law audio frames between authenticated UDP
// sessions.
type Server struct {
	cfg ServerConfig
	log *logging.Logger

	startMu      sync.Mutex
	started      bool
	ctx          context.Context
	cancel       context.CancelFunc
	listener     net.Listener
	udp          *net.UDPConn
	boundUDPPort uint16
	startedAt    time.Time
	controlAddr  string
	audioAddr    string
	wg           sync.WaitGroup

	connMu sync.Mutex
	conns  map[net.Conn]struct{}

	mu              sync.Mutex
	sessions        map[uint32]*session
	nextSession     uint32
	pendingAuth     int
	activeTalker    uint32
	lastTalkerAudio time.Time
	stats           counters
	listeners       map[uint64]chan []byte
	nextListener    uint64

	eventMu sync.Mutex
	events  []Event
}

// NewServer creates a reflector. Start binds the sockets.
func NewServer(cfg ServerConfig, logger *logging.Logger) *Server {
	if logger == nil {
		logger = logging.New(logging.Error, io.Discard)
	}
	return &Server{
		cfg:         cfg.normalized(),
		log:         logger,
		conns:       make(map[net.Conn]struct{}),
		sessions:    make(map[uint32]*session),
		listeners:   make(map[uint64]chan []byte),
		nextSession: 1,
	}
}

// Start binds the TCP and UDP sockets and launches the server goroutines.
func (s *Server) Start() error {
	s.startMu.Lock()
	defer s.startMu.Unlock()

	if s.started {
		return nil
	}

	listener, err := net.Listen("tcp4", fmt.Sprintf(":%d", s.cfg.ControlPort))
	if err != nil {
		return fmt.Errorf("listen TCP %d: %w", s.cfg.ControlPort, err)
	}

	udp, err := net.ListenUDP("udp4", &net.UDPAddr{
		IP:   net.IPv4zero,
		Port: s.cfg.AudioPort,
	})
	if err != nil {
		_ = listener.Close()
		return fmt.Errorf("listen UDP %d: %w", s.cfg.AudioPort, err)
	}

	ctx, cancel := context.WithCancel(context.Background())

	s.mu.Lock()
	s.sessions = make(map[uint32]*session)
	s.listeners = make(map[uint64]chan []byte)
	s.nextListener = 0
	s.stats = counters{}
	s.mu.Unlock()

	s.eventMu.Lock()
	s.events = nil
	s.eventMu.Unlock()

	s.ctx = ctx
	s.cancel = cancel
	s.listener = listener
	s.udp = udp
	s.boundUDPPort = uint16(udp.LocalAddr().(*net.UDPAddr).Port)
	s.startedAt = time.Now()
	s.controlAddr = listener.Addr().String()
	s.audioAddr = udp.LocalAddr().String()
	s.started = true

	s.wg.Add(3)
	go s.acceptLoop(ctx)
	go s.udpLoop(ctx)
	go s.cleanupLoop(ctx)

	s.log.Info(fmt.Sprintf("BenshiNode server listening: TCP %d, UDP %d",
		listener.Addr().(*net.TCPAddr).Port, s.boundUDPPort))
	if s.cfg.Key == "" {
		s.log.Warning("Server key is empty; anyone can connect")
	}
	return nil
}

// Close stops the server and waits for its goroutines to exit. It is safe to
// call more than once.
func (s *Server) Close() {
	s.startMu.Lock()
	defer s.startMu.Unlock()

	if !s.started {
		return
	}
	s.started = false

	if s.cancel != nil {
		s.cancel()
	}
	if s.listener != nil {
		_ = s.listener.Close()
	}
	if s.udp != nil {
		_ = s.udp.Close()
	}
	s.closeConnections()
	s.wg.Wait()

	s.ctx = nil
	s.cancel = nil
	s.listener = nil
	s.udp = nil
	s.boundUDPPort = 0
	s.startedAt = time.Time{}
	s.controlAddr = ""
	s.audioAddr = ""

	s.mu.Lock()
	s.sessions = make(map[uint32]*session)
	s.pendingAuth = 0
	s.nextSession = 1
	s.activeTalker = 0
	s.lastTalkerAudio = time.Time{}
	s.listeners = make(map[uint64]chan []byte)
	s.nextListener = 0
	s.mu.Unlock()

	s.log.Info("Server stopped")
}

// ControlAddr returns the bound TCP address, or nil before Start.
func (s *Server) ControlAddr() net.Addr {
	s.startMu.Lock()
	defer s.startMu.Unlock()
	if s.listener == nil {
		return nil
	}
	return s.listener.Addr()
}

// AudioAddr returns the bound UDP address, or nil before Start.
func (s *Server) AudioAddr() net.Addr {
	s.startMu.Lock()
	defer s.startMu.Unlock()
	if s.udp == nil {
		return nil
	}
	return s.udp.LocalAddr()
}

// Stats returns a snapshot of current sessions, counters, and recent events.
func (s *Server) Stats() Stats {
	s.startMu.Lock()
	started := s.started
	startedAt := s.startedAt
	controlAddr := s.controlAddr
	audioAddr := s.audioAddr
	s.startMu.Unlock()

	stats := Stats{
		StartedAt:      startedAt,
		ControlAddress: controlAddr,
		AudioAddress:   audioAddr,
		MaxClients:     s.cfg.MaxClients,
	}
	if started && !startedAt.IsZero() {
		stats.UptimeSeconds = int64(time.Since(startedAt).Seconds())
	}

	s.mu.Lock()
	stats.SessionCount = len(s.sessions)
	stats.Sessions = make([]SessionStats, 0, len(s.sessions))
	for _, sess := range s.sessions {
		remote := ""
		if sess.hasAddr {
			remote = sess.addr.String()
		}
		stats.Sessions = append(stats.Sessions, SessionStats{
			ID:           sess.id,
			Callsign:     sess.callsign,
			Remote:       remote,
			ConnectedAt:  sess.connectedAt,
			LastSeen:     sess.lastSeen,
			Talking:      s.activeTalker == sess.id,
			AudioPackets: sess.audioPackets,
			AudioBytes:   sess.audioBytes,
		})
	}
	stats.ActiveTalker = s.activeTalker
	if sess, ok := s.sessions[s.activeTalker]; ok {
		stats.ActiveTalkerCallsign = sess.callsign
	}
	stats.Listeners = len(s.listeners)
	stats.Authenticated = s.stats.authenticated
	stats.Rejected = s.stats.rejected
	stats.Expired = s.stats.expired
	stats.AudioPackets = s.stats.audioPackets
	stats.AudioBytes = s.stats.audioBytes
	stats.RelayedPackets = s.stats.relayedPackets
	stats.RelayedBytes = s.stats.relayedBytes
	stats.Heartbeats = s.stats.heartbeats
	stats.AudioDropped = s.stats.audioDropped
	s.mu.Unlock()

	sort.Slice(stats.Sessions, func(i, j int) bool {
		return stats.Sessions[i].ID < stats.Sessions[j].ID
	})
	stats.Events = s.snapshotEvents()
	return stats
}

// SubscribeAudio registers a listener for live relayed audio frames. The
// returned channel receives raw G.711 u-law payloads; frames are dropped when
// the buffer is full. Call the returned function to unsubscribe.
func (s *Server) SubscribeAudio(buffer int) (<-chan []byte, func()) {
	if buffer < 1 {
		buffer = 1
	}

	s.mu.Lock()
	id := s.nextListener
	s.nextListener++
	ch := make(chan []byte, buffer)
	s.listeners[id] = ch
	count := len(s.listeners)
	s.mu.Unlock()

	s.recordEvent("info", fmt.Sprintf("Web audio listener connected (%d active)", count))

	var once sync.Once
	unsubscribe := func() {
		once.Do(func() {
			s.mu.Lock()
			_, present := s.listeners[id]
			if present {
				delete(s.listeners, id)
			}
			count := len(s.listeners)
			s.mu.Unlock()

			if present {
				s.recordEvent("info",
					fmt.Sprintf("Web audio listener disconnected (%d active)", count))
			}
		})
	}
	return ch, unsubscribe
}

func (s *Server) acceptLoop(ctx context.Context) {
	defer s.wg.Done()

	for {
		conn, err := s.listener.Accept()
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			time.Sleep(10 * time.Millisecond)
			continue
		}

		if !s.reserveAuthSlot() {
			s.recordRejected("Rejecting client: maximum clients reached")
			_ = conn.Close()
			continue
		}
		if ctx.Err() != nil {
			s.releaseAuthSlot()
			_ = conn.Close()
			return
		}

		s.trackConn(conn, true)
		s.wg.Add(1)
		go func() {
			defer s.wg.Done()
			s.handleClient(conn)
		}()
	}
}

func (s *Server) handleClient(conn net.Conn) {
	defer func() {
		s.trackConn(conn, false)
		s.releaseAuthSlot()
		_ = conn.Close()
	}()

	_ = conn.SetDeadline(time.Now().Add(maxAuthDuration))

	var challenge AuthChallenge
	if err := randomBytes(challenge.Nonce[:]); err != nil {
		s.log.Error("Could not generate authentication challenge")
		return
	}
	if err := writeAll(conn, EncodeChallenge(challenge)); err != nil {
		s.log.Debug("Could not send link authentication challenge")
		return
	}
	s.log.Debug("Sent link authentication challenge")

	responseData := make([]byte, responseSize)
	if _, err := io.ReadFull(conn, responseData); err != nil {
		s.recordRejected("Link client did not send an authentication response")
		return
	}

	response, ok := DecodeAuthResponse(responseData)
	if !ok {
		s.recordRejected("Link client sent an invalid authentication response")
		_ = writeAll(conn, EncodeAuthFail())
		return
	}
	s.log.Debug("Received link authentication response from " + response.Callsign)

	if s.cfg.Key != "" {
		expected := hmacSHA256([]byte(s.cfg.Key), challenge.Nonce[:])
		if !constantTimeEqual(expected[:], response.Digest[:]) {
			s.recordRejected("Rejected client with invalid key: " + response.Callsign)
			_ = writeAll(conn, EncodeAuthFail())
			return
		}
	}

	sessionID, ok := s.addSession(response.Callsign)
	if !ok {
		s.recordRejected("Rejecting client: maximum clients reached")
		_ = writeAll(conn, EncodeAuthFail())
		return
	}

	okData := EncodeAuthOK(AuthOK{
		Session: sessionID,
		UDPPort: s.boundUDPPort,
	})
	if err := writeAll(conn, okData); err != nil {
		s.log.Warning("Could not send authentication result")
		return
	}

	message := fmt.Sprintf("Link client authenticated: %s (session %d)",
		response.Callsign, sessionID)
	s.log.Info(message)
	s.recordEvent("info", message)
}

func (s *Server) udpLoop(ctx context.Context) {
	defer s.wg.Done()

	buffer := make([]byte, maxDatagramSize)
	for {
		size, address, err := s.udp.ReadFromUDP(buffer)
		if err != nil {
			if ctx.Err() != nil {
				return
			}
			time.Sleep(10 * time.Millisecond)
			continue
		}

		packet, ok := DecodeAudioPacket(buffer[:size])
		if !ok {
			continue
		}
		s.handleUDP(packet, address, time.Now())
	}
}

func (s *Server) cleanupLoop(ctx context.Context) {
	defer s.wg.Done()

	ticker := time.NewTicker(cleanupInterval)
	defer ticker.Stop()

	for {
		select {
		case <-ctx.Done():
			return
		case now := <-ticker.C:
			s.cleanupSessions(now)
		}
	}
}

func (s *Server) handleUDP(packet AudioPacket, address *net.UDPAddr, now time.Time) {
	s.mu.Lock()
	sess, ok := s.sessions[packet.Session]
	if !ok {
		s.mu.Unlock()
		return
	}

	sess.addr = *cloneUDPAddr(address)
	sess.hasAddr = true
	sess.lastSeen = now

	switch packet.Type {
	case PacketHeartbeat:
		s.stats.heartbeats++
		s.mu.Unlock()
		reply := EncodeAudioPacket(AudioPacket{Type: PacketHeartbeat})
		_, _ = s.udp.WriteToUDP(reply, address)

	case PacketAudio:
		accepted := false
		switched := false
		if s.activeTalker == 0 || s.activeTalker == packet.Session ||
			now.Sub(s.lastTalkerAudio) > talkerTimeout {
			switched = s.activeTalker != packet.Session
			s.activeTalker = packet.Session
			s.lastTalkerAudio = now
			accepted = true

			sess.audioPackets++
			sess.audioBytes += uint64(len(packet.Payload))
			s.stats.audioPackets++
			s.stats.audioBytes += uint64(len(packet.Payload))
		}
		callsign := sess.callsign
		s.mu.Unlock()

		if switched {
			message := "Talker: " + callsign
			s.log.Info(message)
			s.recordEvent("info", message)
		}
		if accepted {
			s.relayAudio(packet, packet.Session)
		}

	case PacketEnd:
		ended := false
		callsign := ""
		if s.activeTalker == packet.Session {
			s.activeTalker = 0
			ended = true
			callsign = sess.callsign
		}
		s.mu.Unlock()

		if ended {
			message := "Talker stopped: " + callsign
			s.log.Info(message)
			s.recordEvent("info", message)
			s.relayAudio(packet, packet.Session)
		}

	default:
		s.mu.Unlock()
	}
}

func (s *Server) cleanupSessions(now time.Time) {
	s.mu.Lock()
	defer s.mu.Unlock()

	for id, sess := range s.sessions {
		if now.Sub(sess.lastSeen) <= sessionTimeout {
			continue
		}
		message := "Link client expired: " + sess.callsign
		s.log.Info(message)
		s.recordEvent("warn", message)
		s.stats.expired++
		if s.activeTalker == id {
			s.activeTalker = 0
		}
		delete(s.sessions, id)
	}
}

func (s *Server) relayAudio(packet AudioPacket, fromSession uint32) {
	s.mu.Lock()
	recipients := make([]*net.UDPAddr, 0, len(s.sessions))
	for id, sess := range s.sessions {
		if id != fromSession && sess.hasAddr {
			recipients = append(recipients, cloneUDPAddr(&sess.addr))
		}
	}
	s.mu.Unlock()

	if packet.Type == PacketAudio && len(packet.Payload) > 0 {
		s.broadcastAudio(packet.Payload)
	}

	if len(recipients) == 0 {
		return
	}

	data := EncodeAudioPacket(packet)
	var sentPackets, sentBytes uint64
	for _, address := range recipients {
		if _, err := s.udp.WriteToUDP(data, address); err == nil {
			sentPackets++
			sentBytes += uint64(len(packet.Payload))
		}
	}
	if sentPackets > 0 {
		s.mu.Lock()
		s.stats.relayedPackets += sentPackets
		s.stats.relayedBytes += sentBytes
		s.mu.Unlock()
	}
}

func (s *Server) broadcastAudio(payload []byte) {
	s.mu.Lock()
	channels := make([]chan []byte, 0, len(s.listeners))
	for _, ch := range s.listeners {
		channels = append(channels, ch)
	}
	s.mu.Unlock()

	if len(channels) == 0 {
		return
	}

	dropped := uint64(0)
	for _, ch := range channels {
		select {
		case ch <- payload:
		default:
			dropped++
		}
	}
	if dropped > 0 {
		s.mu.Lock()
		s.stats.audioDropped += dropped
		s.mu.Unlock()
	}
}

func (s *Server) recordRejected(message string) {
	s.mu.Lock()
	s.stats.rejected++
	s.mu.Unlock()

	s.log.Warning(message)
	s.recordEvent("warn", message)
}

func (s *Server) recordEvent(level, message string) {
	event := Event{Time: time.Now(), Level: level, Message: message}

	s.eventMu.Lock()
	defer s.eventMu.Unlock()

	if len(s.events) >= maxEventHistory {
		copy(s.events, s.events[1:])
		s.events[len(s.events)-1] = event
		return
	}
	s.events = append(s.events, event)
}

func (s *Server) snapshotEvents() []Event {
	s.eventMu.Lock()
	defer s.eventMu.Unlock()

	events := make([]Event, len(s.events))
	for i := range s.events {
		events[i] = s.events[len(s.events)-1-i]
	}
	return events
}

func (s *Server) reserveAuthSlot() bool {
	s.mu.Lock()
	defer s.mu.Unlock()

	if len(s.sessions)+s.pendingAuth >= s.cfg.MaxClients {
		return false
	}
	s.pendingAuth++
	return true
}

func (s *Server) releaseAuthSlot() {
	s.mu.Lock()
	if s.pendingAuth > 0 {
		s.pendingAuth--
	}
	s.mu.Unlock()
}

func (s *Server) addSession(callsign string) (uint32, bool) {
	s.mu.Lock()
	defer s.mu.Unlock()

	if len(s.sessions) >= s.cfg.MaxClients {
		return 0, false
	}

	for {
		id := s.nextSession
		s.nextSession++
		if id == 0 {
			continue
		}
		if _, exists := s.sessions[id]; exists {
			continue
		}
		now := time.Now()
		s.sessions[id] = &session{
			id:          id,
			callsign:    callsign,
			connectedAt: now,
			lastSeen:    now,
		}
		s.stats.authenticated++
		return id, true
	}
}

func (s *Server) trackConn(conn net.Conn, add bool) {
	s.connMu.Lock()
	if add {
		s.conns[conn] = struct{}{}
	} else {
		delete(s.conns, conn)
	}
	s.connMu.Unlock()
}

func (s *Server) closeConnections() {
	s.connMu.Lock()
	conns := make([]net.Conn, 0, len(s.conns))
	for conn := range s.conns {
		conns = append(conns, conn)
	}
	s.connMu.Unlock()

	for _, conn := range conns {
		_ = conn.Close()
	}
}

func writeAll(conn net.Conn, data []byte) error {
	for len(data) > 0 {
		written, err := conn.Write(data)
		if err != nil {
			return err
		}
		data = data[written:]
	}
	return nil
}

func cloneUDPAddr(address *net.UDPAddr) *net.UDPAddr {
	return &net.UDPAddr{
		IP:   append(net.IP(nil), address.IP...),
		Port: address.Port,
		Zone: address.Zone,
	}
}
