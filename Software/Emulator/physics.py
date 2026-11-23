"""Independent forward model: target coordinates -> acoustic sensor arrivals."""
import math
from dataclasses import dataclass


def speed_of_sound(temperature=20.0, humidity=50.0):
    """mm/us, using the same air properties as main/speed_of_sound.c.

    This is a forward propagation model, not the firmware's position solver.
    """
    if not -20 <= temperature <= 40 or not 0 <= humidity <= 100:
        raise ValueError("temperature must be -20..40 C and humidity 0..100%")
    humidity = humidity if temperature >= 0 else 0
    tk = 273.15 + temperature
    vapor = math.exp(-7511.52 / tk + 96.5389644 + 0.02399897 * tk
                     - 0.000011654551 * tk**2 - 0.000000012810336 * tk**3
                     + 0.000000000020998405 * tk**4 - 12.150799 * math.log(tk))
    fraction = 0.01 * humidity * vapor / 101325
    specific_heat = (7 + fraction) / (5 + fraction)
    molar_weight = 28.966 - (28.966 - 18.01528) * fraction
    change = 100 / math.sqrt(1.4 / 28.966) * math.sqrt(specific_heat / molar_weight) - 100
    gamma = 1.40092 - 0.0000196395 * temperature - 0.000000162593 * temperature**2
    return (math.sqrt(gamma * 8314.46261815324 * tk / 28.966) + 3.3138 * change) / 1000


@dataclass(frozen=True)
class Target:
    diameter_mm: float = 232.0
    angle_deg: float = 45.0
    depth_mm: float = 0.0
    temperature: float = 20.0
    humidity: float = 50.0

    def sensors(self):
        """N/E/S/W positions in displayed mm: top-left clockwise by default."""
        if not math.isfinite(self.diameter_mm) or not math.isfinite(self.angle_deg) or not 0 < self.diameter_mm <= 10000:
            raise ValueError("invalid sensor geometry")
        radius = self.diameter_mm / 2
        angle = math.radians(self.angle_deg)
        return [{"name": name, "x": sx * math.cos(angle) - sy * math.sin(angle),
                 "y": -(sx * math.sin(angle) + sy * math.cos(angle))}
                for name, (sx, sy) in zip("NESW", ((0, radius), (radius, 0), (0, -radius), (-radius, 0)))]

    def arrivals(self, x, y):
        """Public target coordinates in mm: centre (0, 0), negative Y up."""
        if not all(math.isfinite(v) for v in (x, y, self.diameter_mm, self.angle_deg, self.depth_mm)):
            raise ValueError("coordinates and geometry must be finite")
        if self.diameter_mm <= 0 or self.depth_mm < 0:
            raise ValueError("invalid target geometry")
        if max(abs(x), abs(y), self.diameter_mm, self.depth_mm) > 10000:
            raise ValueError("coordinates and dimensions must be within 10 metres")
        # Work directly in the displayed coordinate system. Rotating the
        # physical sensors also agrees with the stock firmware's ANGLE setting.
        speed = speed_of_sound(self.temperature, self.humidity)
        # A count is 100 ns. Use an explicit nearest-tick rule for reproducible fixtures.
        return [int(math.floor(math.sqrt((x - sensor["x"])**2 + (y - sensor["y"])**2 + self.depth_mm**2) / speed * 10 + 0.5)) * 100
                for sensor in self.sensors()]

