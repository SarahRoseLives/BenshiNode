// AudioWorklet processor for BenshiNode live audio.
//
// It receives 8 kHz G.711 u-law frames from the main thread, decodes and
// resamples them on the audio thread, and maintains a small jitter buffer.
// Playback is clocked by the AudioContext, so main-thread jank cannot create
// gaps between frames.

class BenshiNodeULawPlayer extends AudioWorkletProcessor {
  constructor() {
    super();

    this.capacity = 16000; // 2 seconds at 8 kHz
    this.ring = new Float32Array(this.capacity);
    this.writeIndex = 0;
    this.readIndex = 0;

    this.targetSamples = 640;        // ~80 ms jitter buffer at 8 kHz
    this.startSamples = 400;         // wait for ~50 ms before starting
    this.maxSamples = 2400;          // drop down to target above ~300 ms
    this.started = false;

    this.table = new Float32Array(256);
    for (let i = 0; i < 256; i++) {
      const value = (~i) & 0xFF;
      const sign = value & 0x80;
      const exponent = (value >> 4) & 0x07;
      const mantissa = value & 0x0F;
      let magnitude = ((mantissa << 3) + 0x84) << exponent;
      magnitude -= 0x84;
      if (sign) { magnitude = -magnitude; }
      this.table[i] = magnitude / 32768;
    }

    this.port.onmessage = (event) => {
      const data = event.data;
      if (!data || data.length === 0) { return; }
      this.append(data);
    };
  }

  available() {
    const readFloor = Math.floor(this.readIndex) % this.capacity;
    return (this.writeIndex - readFloor + this.capacity) % this.capacity;
  }

  append(data) {
    let available = this.available();
    let free = this.capacity - available - 1;

    if (data.length > free) {
      let drop = data.length - free;
      if (drop > available) { drop = available; }
      this.readIndex = (this.readIndex + drop) % this.capacity;
    }

    for (let i = 0; i < data.length; i++) {
      this.ring[this.writeIndex] = this.table[data[i]];
      this.writeIndex = (this.writeIndex + 1) % this.capacity;
    }
  }

  process(inputs, outputs) {
    const output = outputs[0][0];
    if (!output) { return true; }

    let available = this.available();
    if (available > this.maxSamples) {
      // Keep latency bounded when a burst arrives.
      this.readIndex = (this.writeIndex - this.targetSamples + this.capacity) % this.capacity;
      available = this.available();
    }

    if (!this.started) {
      if (available >= this.startSamples) {
        this.started = true;
      } else {
        output.fill(0);
        return true;
      }
    }

    if (available < 2) {
      this.started = false;
      output.fill(0);
      return true;
    }

    const error = (available - this.targetSamples) / this.targetSamples;
    let correction = error * 0.5;
    if (correction > 0.02) { correction = 0.02; }
    if (correction < -0.02) { correction = -0.02; }

    const advance = (8000 / sampleRate) * (1 + correction);
    let read = this.readIndex;
    let i = 0;

    for (; i < output.length; i++) {
      const readFloor = Math.floor(read) % this.capacity;
      const buffered = (this.writeIndex - readFloor + this.capacity) % this.capacity;
      if (buffered < 2) {
        this.started = false;
        break;
      }

      const next = (readFloor + 1) % this.capacity;
      const fraction = read - Math.floor(read);
      output[i] = this.ring[readFloor] + (this.ring[next] - this.ring[readFloor]) * fraction;

      read += advance;
      if (read >= this.capacity) { read -= this.capacity; }
    }

    this.readIndex = read;
    for (; i < output.length; i++) {
      output[i] = 0;
    }
    return true;
  }
}

registerProcessor("benshinode-ulaw", BenshiNodeULawPlayer);
