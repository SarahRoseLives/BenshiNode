package main

import (
	"os"
	"path/filepath"
	"testing"

	"benshinode/internal/config"
)

func loadTestConfig(t *testing.T, content string) *config.Config {
	t.Helper()

	path := filepath.Join(t.TempDir(), "test.ini")
	if err := os.WriteFile(path, []byte(content), 0o644); err != nil {
		t.Fatalf("write config: %v", err)
	}
	cfg, err := config.Load(path)
	if err != nil {
		t.Fatalf("load config: %v", err)
	}
	return cfg
}

func TestResolveWebAddress(t *testing.T) {
	tests := []struct {
		name    string
		content string
		flag    string
		want    string
		wantErr bool
	}{
		{
			name:    "disabled by default",
			content: "[WEB]\nENABLED=0\n",
		},
		{
			name:    "enabled from config",
			content: "[WEB]\nENABLED=1\nBIND=127.0.0.1\nPORT=8080\n",
			want:    "127.0.0.1:8080",
		},
		{
			name:    "config binds all interfaces when empty",
			content: "[WEB]\nENABLED=1\nBIND=\nPORT=9000\n",
			want:    ":9000",
		},
		{
			name:    "flag enables address",
			content: "[WEB]\nENABLED=0\n",
			flag:    "127.0.0.1:7777",
			want:    "127.0.0.1:7777",
		},
		{
			name:    "flag enables configured address",
			content: "[WEB]\nENABLED=0\nBIND=127.0.0.1\nPORT=8080\n",
			flag:    "true",
			want:    "127.0.0.1:8080",
		},
		{
			name: "flag port uses configured bind", content: "[WEB]\nENABLED=0\nBIND=0.0.0.0\n",
			flag: "7777",
			want: "0.0.0.0:7777",
		},
		{
			name:    "flag colon address",
			content: "[WEB]\nENABLED=0\n",
			flag:    ":7777",
			want:    ":7777",
		},
		{
			name:    "flag disables config",
			content: "[WEB]\nENABLED=1\nPORT=8080\n",
			flag:    "off",
		},
		{
			name:    "invalid address",
			content: "[WEB]\nENABLED=0\n",
			flag:    "not-an-address",
			wantErr: true,
		},
		{
			name:    "invalid port",
			content: "[WEB]\nENABLED=0\n",
			flag:    "70000",
			wantErr: true,
		},
	}

	for _, test := range tests {
		t.Run(test.name, func(t *testing.T) {
			cfg := loadTestConfig(t, test.content)
			got, err := resolveWebAddress(cfg, test.flag)
			if test.wantErr {
				if err == nil {
					t.Fatalf("resolveWebAddress = %q, want error", got)
				}
				return
			}
			if err != nil {
				t.Fatalf("resolveWebAddress: %v", err)
			}
			if got != test.want {
				t.Fatalf("resolveWebAddress = %q, want %q", got, test.want)
			}
		})
	}
}
