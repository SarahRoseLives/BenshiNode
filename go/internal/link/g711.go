package link

// ULawToLinear decodes an 8-bit G.711 u-law sample to 16-bit PCM. The mapping
// matches the C++ link G.711 implementation.
func ULawToLinear(ulaw byte) int16 {
	return ulawTable[ulaw]
}

var ulawTable = func() [256]int16 {
	const bias = 0x84

	var table [256]int16
	for i := 0; i < 256; i++ {
		value := ^byte(i)
		sign := int(value & 0x80)
		exponent := int((value >> 4) & 0x07)
		mantissa := int(value & 0x0F)

		magnitude := ((mantissa << 3) + bias) << exponent
		magnitude -= bias
		if sign != 0 {
			magnitude = -magnitude
		}
		table[i] = int16(magnitude)
	}
	return table
}()
