package config

import (
	"os"
	"path/filepath"
	"testing"
)

func writeConfig(t *testing.T, content string) string {
	t.Helper()
	path := filepath.Join(t.TempDir(), "test.ini")
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatalf("write config: %v", err)
	}
	return path
}

func TestLoadParsesValues(t *testing.T) {
	path := writeConfig(t, "\ufeff# comment\n"+
		"[SERVER]\n"+
		"PORT=7300\n"+
		"AUDIO_PORT = 7301 ; inline comment\n"+
		"KEY=\"shared;key\"\n"+
		"QUOTED='single value'\n"+
		"[NODE]\n"+
		"DTMF_SUFFIX=#\n"+
		"HEX=0x10\n"+
		"BOOL=YES\n")

	cfg, err := Load(path)
	if err != nil {
		t.Fatalf("Load: %v", err)
	}

	if got := cfg.GetString("SERVER", "PORT", ""); got != "7300" {
		t.Fatalf("PORT = %q", got)
	}
	if got := cfg.GetString("SERVER", "AUDIO_PORT", ""); got != "7301" {
		t.Fatalf("AUDIO_PORT = %q", got)
	}
	if got := cfg.GetString("SERVER", "KEY", ""); got != "shared;key" {
		t.Fatalf("KEY = %q", got)
	}
	if got := cfg.GetString("SERVER", "QUOTED", ""); got != "single value" {
		t.Fatalf("QUOTED = %q", got)
	}
	if got := cfg.GetString("NODE", "DTMF_SUFFIX", ""); got != "#" {
		t.Fatalf("DTMF_SUFFIX = %q", got)
	}
	if got := cfg.GetInt("NODE", "HEX", 0); got != 16 {
		t.Fatalf("HEX = %d", got)
	}
	if !cfg.GetBool("NODE", "BOOL", false) {
		t.Fatal("BOOL = false")
	}
	if got := cfg.GetString("NODE", "MISSING", "fallback"); got != "fallback" {
		t.Fatalf("MISSING = %q", got)
	}
	if got := cfg.GetInt("NODE", "MISSING", 12); got != 12 {
		t.Fatalf("missing int = %d", got)
	}
	if got := cfg.GetInt("NODE", "BAD", 99); got != 99 {
		t.Fatalf("bad int = %d", got)
	}
}

func TestLoadRejectsMalformed(t *testing.T) {
	tests := []struct {
		name    string
		content string
	}{
		{name: "key outside section", content: "PORT=7300\n"},
		{name: "missing equals", content: "[SERVER]\nPORT\n"},
		{name: "empty section", content: "[]\n"},
		{name: "empty key", content: "[SERVER]\n=7300\n"},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			if _, err := Load(writeConfig(t, test.content)); err == nil {
				t.Fatal("Load unexpectedly succeeded")
			}
		})
	}
}

func TestLoadMissingFile(t *testing.T) {
	if _, err := Load(filepath.Join(t.TempDir(), "missing.ini")); err == nil {
		t.Fatal("Load unexpectedly succeeded")
	}
}
