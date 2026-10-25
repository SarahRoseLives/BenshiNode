// Package web serves the reflector dashboard, stats API, and live audio
// stream.
package web

import (
	"context"
	_ "embed"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"
	"net"
	"net/http"
	"sync"
	"time"

	"benshinode/internal/link"
	"benshinode/internal/logging"
)

//go:embed index.html
var indexHTML []byte

//go:embed audio-worklet.js
var workletJS []byte

const (
	sampleRate      = 8000
	samplesPerFrame = 160 // 20 ms at 8 kHz
	silenceULaw     = 0xFF
	wavHeaderSize   = 44
)

// Server exposes the reflector over HTTP.
type Server struct {
	reflector *link.Server
	log       *logging.Logger

	mu         sync.Mutex
	started    bool
	listener   net.Listener
	httpServer *http.Server
	cancel     context.CancelFunc
	baseCtx    context.Context
	wsConns    map[net.Conn]struct{}
	wsWG       sync.WaitGroup
}

// New creates a web server for the given reflector.
func New(reflector *link.Server, logger *logging.Logger) *Server {
	if logger == nil {
		logger = logging.New(logging.Error, io.Discard)
	}
	return &Server{
		reflector: reflector,
		log:       logger,
		wsConns:   make(map[net.Conn]struct{}),
	}
}

// Start binds address and serves in the background.
func (s *Server) Start(address string) error {
	s.mu.Lock()
	defer s.mu.Unlock()

	if s.started {
		return nil
	}

	listener, err := net.Listen("tcp", address)
	if err != nil {
		return fmt.Errorf("listen web %s: %w", address, err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/", s.handleIndex)
	mux.HandleFunc("/api/stats", s.handleStats)
	mux.HandleFunc("/audio", s.handleAudio)
	mux.HandleFunc("/audio/raw", s.handleAudioRaw)
	mux.HandleFunc("/audio/ws", s.handleAudioWebSocket)
	mux.HandleFunc("/audio-worklet.js", s.handleWorklet)
	mux.HandleFunc("/healthz", s.handleHealth)

	baseCtx, cancel := context.WithCancel(context.Background())
	httpServer := &http.Server{
		Handler:           s.withHeaders(mux),
		ReadHeaderTimeout: 5 * time.Second,
		IdleTimeout:       60 * time.Second,
	}

	s.listener = listener
	s.httpServer = httpServer
	s.cancel = cancel
	s.baseCtx = baseCtx
	s.started = true

	s.log.Info("Web interface listening on http://" + listener.Addr().String())

	go func() {
		err := httpServer.Serve(listener)
		if err != nil && err != http.ErrServerClosed {
			s.log.Error("Web server stopped: " + err.Error())
		}
	}()
	return nil
}

// Close stops the web server and any active audio streams.
func (s *Server) Close() {
	s.mu.Lock()
	if !s.started {
		s.mu.Unlock()
		return
	}
	s.started = false
	cancel := s.cancel
	httpServer := s.httpServer
	s.listener = nil
	s.httpServer = nil
	s.cancel = nil
	s.mu.Unlock()

	if cancel != nil {
		cancel()
	}
	s.closeWSConns()
	if httpServer != nil {
		ctx, stop := context.WithTimeout(context.Background(), 3*time.Second)
		defer stop()
		if err := httpServer.Shutdown(ctx); err != nil {
			_ = httpServer.Close()
		}
	}
	s.wsWG.Wait()

	s.log.Info("Web interface stopped")
}

// Addr returns the bound HTTP address, or nil before Start.
func (s *Server) Addr() net.Addr {
	s.mu.Lock()
	defer s.mu.Unlock()
	if s.listener == nil {
		return nil
	}
	return s.listener.Addr()
}

func (s *Server) withHeaders(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("X-Content-Type-Options", "nosniff")
		w.Header().Set("X-Frame-Options", "DENY")
		w.Header().Set("Referrer-Policy", "no-referrer")
		next.ServeHTTP(w, r)
	})
}

func (s *Server) handleIndex(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	if r.URL.Path != "/" {
		http.NotFound(w, r)
		return
	}

	w.Header().Set("Content-Type", "text/html; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write(indexHTML)
}

func (s *Server) handleStats(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	w.Header().Set("Content-Type", "application/json; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	if err := json.NewEncoder(w).Encode(s.reflector.Stats()); err != nil {
		s.log.Debug("Could not encode stats response: " + err.Error())
	}
}

func (s *Server) handleHealth(w http.ResponseWriter, _ *http.Request) {
	w.Header().Set("Content-Type", "text/plain; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = io.WriteString(w, "ok\n")
}

func (s *Server) handleWorklet(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		w.Header().Set("Allow", "GET, HEAD")
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}
	w.Header().Set("Content-Type", "application/javascript; charset=utf-8")
	w.Header().Set("Cache-Control", "no-store")
	_, _ = w.Write(workletJS)
}

func (s *Server) handleAudio(w http.ResponseWriter, r *http.Request) {
	s.streamAudio(w, r, true)
}

func (s *Server) handleAudioRaw(w http.ResponseWriter, r *http.Request) {
	s.streamAudio(w, r, false)
}

func (s *Server) streamAudio(w http.ResponseWriter, r *http.Request, wav bool) {
	if r.Method != http.MethodGet {
		w.Header().Set("Allow", http.MethodGet)
		http.Error(w, "method not allowed", http.StatusMethodNotAllowed)
		return
	}

	flusher, ok := w.(http.Flusher)
	if !ok {
		http.Error(w, "streaming unsupported", http.StatusInternalServerError)
		return
	}

	frames, unsubscribe := s.reflector.SubscribeAudio(16)
	defer unsubscribe()

	if wav {
		w.Header().Set("Content-Type", "audio/wav")
	} else {
		w.Header().Set("Content-Type", "application/octet-stream")
	}
	w.Header().Set("Cache-Control", "no-store")
	w.Header().Set("X-Accel-Buffering", "no")
	w.WriteHeader(http.StatusOK)

	if wav {
		if _, err := w.Write(makeWAVHeader()); err != nil {
			return
		}
	}
	flusher.Flush()

	if !wav {
		s.streamRawAudio(w, r, frames, flusher)
		return
	}
	s.streamWAVAudio(w, r, frames, flusher)
}

// streamRawAudio forwards each relayed u-law frame immediately. The browser
// performs jitter buffering and clock-drift compensation with the Web Audio
// API. Pacing on the server would fight the client's clock and cause drops.
func (s *Server) streamRawAudio(w http.ResponseWriter, r *http.Request,
	frames <-chan []byte, flusher http.Flusher) {
	var raw [samplesPerFrame]byte

	for {
		select {
		case <-r.Context().Done():
			return
		case <-s.baseCtx.Done():
			return
		case frame := <-frames:
			for i := range raw {
				raw[i] = silenceULaw
			}
			copy(raw[:], frame)
			if _, err := w.Write(raw[:]); err != nil {
				return
			}
			flusher.Flush()
		}
	}
}

func (s *Server) streamWAVAudio(w http.ResponseWriter, r *http.Request,
	frames <-chan []byte, flusher http.Flusher) {
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()

	var raw [samplesPerFrame]byte
	var pcm [samplesPerFrame * 2]byte
	for {
		select {
		case <-r.Context().Done():
			return
		case <-s.baseCtx.Done():
			return
		case <-ticker.C:
		}

		for i := range raw {
			raw[i] = silenceULaw
		}
		select {
		case frame := <-frames:
			copy(raw[:], frame)
		default:
		}

		for i, sample := range raw {
			binary.LittleEndian.PutUint16(
				pcm[i*2:i*2+2], uint16(link.ULawToLinear(sample)))
		}
		if _, err := w.Write(pcm[:]); err != nil {
			return
		}
		flusher.Flush()
	}
}

func makeWAVHeader() []byte {
	header := make([]byte, wavHeaderSize)
	copy(header[0:4], "RIFF")
	binary.LittleEndian.PutUint32(header[4:8], 0xFFFFFFFF)
	copy(header[8:12], "WAVE")
	copy(header[12:16], "fmt ")
	binary.LittleEndian.PutUint32(header[16:20], 16)
	binary.LittleEndian.PutUint16(header[20:22], 1) // PCM
	binary.LittleEndian.PutUint16(header[22:24], 1) // mono
	binary.LittleEndian.PutUint32(header[24:28], sampleRate)
	binary.LittleEndian.PutUint32(header[28:32], sampleRate*2) // byte rate
	binary.LittleEndian.PutUint16(header[32:34], 2)            // block align
	binary.LittleEndian.PutUint16(header[34:36], 16)           // bits per sample
	copy(header[36:40], "data")
	binary.LittleEndian.PutUint32(header[40:44], 0xFFFFFFFF)
	return header
}
