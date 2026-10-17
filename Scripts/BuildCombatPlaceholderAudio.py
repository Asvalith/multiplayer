"""生成原创短提示音；只作可替换的占位表现，不冒充正式音效资源。"""
import argparse
import math
import pathlib
import random
import struct
import wave


def synthesize(destination, name, length, start_hz, end_hz, noise=0.0, chord=0.0):
    rate = 24000
    rng = random.Random(name)
    samples = []
    phase = 0.0
    for index in range(round(length * rate)):
        time = index / rate
        progress = time / length
        phase += 2.0 * math.pi * (start_hz + (end_hz - start_hz) * progress) / rate
        # 平滑起落，控制响度；短提示而不是长循环，避免高频技能叠音。
        envelope = min(time / 0.015, 1.0) * (1.0 - progress) ** 2
        value = math.sin(phase) + chord * math.sin(phase * 1.5) + noise * rng.uniform(-1.0, 1.0)
        samples.append(struct.pack('<h', int(max(-1.0, min(1.0, value * envelope * 0.18)) * 32767)))
    with wave.open(str(destination / (name + '.wav')), 'wb') as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(rate)
        output.writeframes(b''.join(samples))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--output', type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / 'Saved/CombatAssetImport/Audio')
    options = parser.parse_args()
    options.output.mkdir(parents=True, exist_ok=True)
    for spec in [
        ('SW_Cast_Placeholder', 0.24, 280.0, 760.0, 0.1, 0.2),
        ('SW_Hit_Placeholder', 0.16, 220.0, 70.0, 0.7, 0.0),
        ('SW_Heal_Placeholder', 0.4, 520.0, 850.0, 0.0, 0.5),
        ('SW_Shield_Placeholder', 0.3, 190.0, 360.0, 0.05, 0.4),
        ('SW_Denied_Placeholder', 0.18, 270.0, 120.0, 0.0, 0.1),
        ('SW_Area_Placeholder', 0.45, 100.0, 55.0, 0.6, 0.2),
    ]:
        synthesize(options.output, *spec)
    print('Generated 6 original placeholder WAV files:', options.output)


if __name__ == '__main__':
    main()
