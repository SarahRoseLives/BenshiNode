package link

import "testing"

func TestULawToLinear(t *testing.T) {
	tests := []struct {
		ulaw   byte
		linear int16
	}{
		{ulaw: 0xFF, linear: 0},
		{ulaw: 0x7F, linear: 0},
		{ulaw: 0x00, linear: -32124},
		{ulaw: 0x80, linear: 32124},
	}

	for _, test := range tests {
		if got := ULawToLinear(test.ulaw); got != test.linear {
			t.Fatalf("ULawToLinear(0x%02X) = %d, want %d", test.ulaw, got, test.linear)
		}
	}
}
