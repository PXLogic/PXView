import math
class BitstreamBuilder:
    def __init__(self, num_channels, sample_count, samplerate=1000000):
        self.num_channels = num_channels
        self.sample_count = sample_count
        self.samplerate = samplerate
        self.channels = [[0] * sample_count for _ in range(num_channels)]
        self.pos = 0

    def set_pos(self, pos):
        self.pos = pos

    def get_pos(self):
        return self.pos

    def set_level(self, ch, level, duration_samples=1):
        if duration_samples == 0:
            # Overlay mode: set value at current position without advancing pos
            if self.pos < self.sample_count:
                self.channels[ch][self.pos] = 1 if level else 0
            return
        for i in range(duration_samples):
            if self.pos + i < self.sample_count:
                self.channels[ch][self.pos + i] = 1 if level else 0
        self.pos += duration_samples

    def write_channels(self, channel_levels, duration_samples):
        """Set multiple channels simultaneously for the given duration.
        channel_levels: dict of {channel_index: level (0 or 1)}.
        Advances pos by duration_samples."""
        for ch, level in channel_levels.items():
            for i in range(duration_samples):
                if self.pos + i < self.sample_count:
                    self.channels[ch][self.pos + i] = 1 if level else 0
        self.pos += duration_samples

    def set_idle(self, ch, level):
        for i in range(self.pos, self.sample_count):
            self.channels[ch][i] = 1 if level else 0

    def get_bitpacked(self):
        """Convert list of 0/1 to bit-packed bytes."""
        result = bytearray()
        bytes_per_channel = math.ceil(self.sample_count / 8)
        for ch in range(self.num_channels):
            packed = bytearray(bytes_per_channel)
            for i, val in enumerate(self.channels[ch]):
                if val:
                    packed[i // 8] |= (1 << (i % 8))
            result.extend(packed)
        return bytes(result)



class ProtocolFuzzer:
    """Base class for all protocol fuzzers. Future AI agents should inherit from this."""
    def __init__(self, builder, **kwargs):
        self.builder = builder
        
    def inject_error(self, error_type):
        """Override this to inject specific errors."""
        pass
