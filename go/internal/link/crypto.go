package link

import (
	"crypto/hmac"
	"crypto/rand"
	"crypto/sha256"
)

// hmacSHA256 computes the keyed digest used by the link authentication
// handshake.
func hmacSHA256(key, data []byte) [32]byte {
	mac := hmac.New(sha256.New, key)
	_, _ = mac.Write(data)
	var digest [32]byte
	copy(digest[:], mac.Sum(nil))
	return digest
}

// randomBytes fills data with cryptographically secure random bytes.
func randomBytes(data []byte) error {
	_, err := rand.Read(data)
	return err
}

// constantTimeEqual compares two digests without leaking timing information.
func constantTimeEqual(a, b []byte) bool {
	return hmac.Equal(a, b)
}
