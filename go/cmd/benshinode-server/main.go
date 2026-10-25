// Command benshinode-server runs the BenshiNode link reflector.
package main

import (
	"context"
	"flag"
	"fmt"
	"net"
	"os"
	"os/signal"
	"strconv"
	"strings"
	"syscall"

	"benshinode/internal/config"
	"benshinode/internal/link"
	"benshinode/internal/logging"
	"benshinode/internal/web"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintf(os.Stderr, "benshinode-server: %v\n", err)
		os.Exit(1)
	}
}

func run() error {
	configPath := flag.String("config", "server.ini",
		"path to the server INI configuration file")
	verbose := flag.Bool("verbose", false, "enable debug logging")
	webFlag := flag.String("web", "",
		"enable web UI/audio at host:port or port (empty/true uses [WEB]; \"off\" disables)")
	flag.Usage = func() {
		fmt.Fprintf(os.Stderr, "Usage: %s [options]\n\n", os.Args[0])
		fmt.Fprintln(os.Stderr, "Runs the BenshiNode link server/reflector.")
		fmt.Fprintln(os.Stderr, "\nOptions:")
		flag.PrintDefaults()
	}
	flag.Parse()
	if flag.NArg() > 0 {
		return fmt.Errorf("unexpected argument: %s", flag.Arg(0))
	}

	cfg, err := config.Load(*configPath)
	if err != nil {
		return err
	}

	level := logging.Info
	if parsed, ok := logging.ParseLevel(cfg.GetString("LOG", "LEVEL", "info")); ok {
		level = parsed
	}
	if *verbose {
		level = logging.Debug
	}

	logger := logging.New(level, os.Stdout)
	defer func() { _ = logger.Close() }()

	if logFile := cfg.GetString("LOG", "FILE", ""); logFile != "" {
		if err := logger.OpenFile(logFile); err != nil {
			logger.Warning("Could not open log file: " + logFile)
		}
	}

	webAddress, err := resolveWebAddress(cfg, *webFlag)
	if err != nil {
		return err
	}

	serverConfig := link.ServerConfig{
		ControlPort: clampPort(cfg.GetInt("SERVER", "PORT", int(link.DefaultControlPort))),
		AudioPort:   clampPort(cfg.GetInt("SERVER", "AUDIO_PORT", int(link.DefaultAudioPort))),
		Key:         cfg.GetString("SERVER", "KEY", ""),
		MaxClients:  cfg.GetInt("SERVER", "MAX_CLIENTS", 16),
	}
	if serverConfig.MaxClients < 1 {
		serverConfig.MaxClients = 1
	}

	server := link.NewServer(serverConfig, logger)
	if err := server.Start(); err != nil {
		return fmt.Errorf("could not start link server: %w", err)
	}

	var webServer *web.Server
	if webAddress != "" {
		webServer = web.New(server, logger)
		if err := webServer.Start(webAddress); err != nil {
			server.Close()
			return fmt.Errorf("could not start web interface: %w", err)
		}
	}

	logger.Info("Server running. Press Ctrl+C to stop.")

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	<-ctx.Done()

	if webServer != nil {
		webServer.Close()
	}
	server.Close()
	return nil
}

func resolveWebAddress(cfg *config.Config, flagValue string) (string, error) {
	value := strings.TrimSpace(flagValue)
	enabled := cfg.GetBool("WEB", "ENABLED", false)

	if value != "" {
		switch strings.ToLower(value) {
		case "off", "false", "0", "no":
			return "", nil
		case "on", "true", "yes", "1":
			value = ""
		}
		enabled = true
	}

	if !enabled {
		return "", nil
	}

	bind := cfg.GetString("WEB", "BIND", "127.0.0.1")
	if value == "" {
		port := clampPort(cfg.GetInt("WEB", "PORT", 8080))
		return joinHostPort(bind, port), nil
	}

	if _, _, err := net.SplitHostPort(value); err == nil {
		return value, nil
	}
	if port, err := strconv.Atoi(value); err == nil {
		if port < 1 || port > 65535 {
			return "", fmt.Errorf("invalid web port %d", port)
		}
		return joinHostPort(bind, port), nil
	}
	return "", fmt.Errorf("invalid web address %q (use host:port or port)", value)
}

func joinHostPort(host string, port int) string {
	if host == "" {
		return ":" + strconv.Itoa(port)
	}
	return net.JoinHostPort(host, strconv.Itoa(port))
}

func clampPort(port int) int {
	if port < 1 {
		return 1
	}
	if port > 65535 {
		return 65535
	}
	return port
}
