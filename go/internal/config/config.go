// Package config reads the subset of the BenshiNode INI format required by
// the Go server. It mirrors the parsing rules of the C++ Config class.
package config

import (
	"fmt"
	"os"
	"strconv"
	"strings"
)

// Config holds parsed INI sections and values. Section and key lookups are
// case-sensitive, like the C++ implementation.
type Config struct {
	sections map[string]map[string]string
}

// Load reads and parses an INI file.
func Load(path string) (*Config, error) {
	data, err := os.ReadFile(path)
	if err != nil {
		return nil, fmt.Errorf("cannot open config file %s: %w", path, err)
	}

	config := &Config{sections: make(map[string]map[string]string)}
	lines := strings.Split(strings.TrimPrefix(string(data), "\ufeff"), "\n")
	current := ""

	for index, raw := range lines {
		lineNumber := index + 1
		line := strings.TrimSuffix(raw, "\r")
		text := strings.TrimSpace(line)
		if text == "" || text[0] == ';' || text[0] == '#' {
			continue
		}

		if text[0] == '[' && text[len(text)-1] == ']' {
			current = strings.TrimSpace(text[1 : len(text)-1])
			if current == "" {
				return nil, fmt.Errorf("empty section name at line %d", lineNumber)
			}
			if _, ok := config.sections[current]; !ok {
				config.sections[current] = make(map[string]string)
			}
			continue
		}

		equals := strings.IndexByte(text, '=')
		if equals < 0 {
			return nil, fmt.Errorf("expected key=value at line %d", lineNumber)
		}
		if current == "" {
			return nil, fmt.Errorf("key outside of a section at line %d", lineNumber)
		}

		key := strings.TrimSpace(text[:equals])
		if key == "" {
			return nil, fmt.Errorf("empty key at line %d", lineNumber)
		}
		value := stripInlineComment(strings.TrimSpace(text[equals+1:]))
		value = stripQuotes(value)
		config.sections[current][key] = value
	}

	return config, nil
}

// stripInlineComment removes a semicolon comment that is separated from the
// value by whitespace while respecting quoted text. A '#' inside a value is
// preserved, so DTMF_SUFFIX=# keeps working.
func stripInlineComment(value string) string {
	var quote byte
	for i := 0; i < len(value); i++ {
		if quote != 0 {
			if value[i] == quote {
				quote = 0
			}
			continue
		}
		switch value[i] {
		case '"', '\'':
			quote = value[i]
		case ';':
			if i > 0 && (value[i-1] == ' ' || value[i-1] == '\t') {
				return strings.TrimSpace(value[:i])
			}
		}
	}
	return value
}

func stripQuotes(value string) string {
	if len(value) >= 2 {
		first := value[0]
		last := value[len(value)-1]
		if (first == '"' && last == '"') || (first == '\'' && last == '\'') {
			return value[1 : len(value)-1]
		}
	}
	return value
}

// GetString returns a value or the supplied default.
func (c *Config) GetString(section, key, defaultValue string) string {
	values, ok := c.sections[section]
	if !ok {
		return defaultValue
	}
	value, ok := values[key]
	if !ok {
		return defaultValue
	}
	return value
}

// GetInt parses a base-0 integer (decimal, 0x hex, or leading-zero octal) and
// returns defaultValue when the key is absent or invalid.
func (c *Config) GetInt(section, key string, defaultValue int) int {
	value := c.GetString(section, key, "")
	if value == "" {
		return defaultValue
	}
	parsed, err := strconv.ParseInt(value, 0, 64)
	if err != nil {
		return defaultValue
	}
	return int(parsed)
}

// GetBool parses the same truthy values as the C++ configuration reader.
func (c *Config) GetBool(section, key string, defaultValue bool) bool {
	value := strings.ToUpper(strings.TrimSpace(c.GetString(section, key, "")))
	switch value {
	case "":
		return defaultValue
	case "1", "TRUE", "YES", "ON":
		return true
	case "0", "FALSE", "NO", "OFF":
		return false
	default:
		return defaultValue
	}
}

// Has reports whether a key exists.
func (c *Config) Has(section, key string) bool {
	values, ok := c.sections[section]
	if !ok {
		return false
	}
	_, ok = values[key]
	return ok
}
