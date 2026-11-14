"""A short burst followed by a freeze must never pass the full frame measurement window."""
import importlib.util
from pathlib import Path
import unittest

SOURCE = Path(__file__).resolve().parents[1] / 'scripts/android/measure_frames.py'
spec = importlib.util.spec_from_file_location('measure_frames', SOURCE)
measure = importlib.util.module_from_spec(spec)
spec.loader.exec_module(measure)


def frames(count, fps=60, offset=0):
    return [offset + round(index * 1e9 / fps) for index in range(count)]


class FrameBudgetTest(unittest.TestCase):
    def test_steady_sixty_passes(self):
        self.assertTrue(measure.frame_budget(frames(900), 15, 60)['meets_budget'])

    def test_early_burst_then_freeze_fails(self):
        result = measure.frame_budget(frames(120), 15, 60, trailing_stall_s=13)
        self.assertFalse(result['meets_budget'])
        self.assertEqual(result['fps'], 8)

    def test_late_burst_after_freeze_fails(self):
        self.assertFalse(measure.frame_budget(frames(120, offset=13_000_000_000), 15, 60)['meets_budget'])

    def test_short_end_freeze_fails_even_with_good_average(self):
        self.assertFalse(measure.frame_budget(frames(880), 15, 60, trailing_stall_s=.2)['meets_budget'])

    def test_single_large_stutter_fails(self):
        stamps = frames(900)
        stamps[450:] = [stamp + 150_000_000 for stamp in stamps[450:]]
        self.assertFalse(measure.frame_budget(stamps, 15.15, 60)['meets_budget'])

    def test_slow_and_missing_samples_do_not_pass(self):
        self.assertFalse(measure.frame_budget(frames(600, fps=40), 15, 60)['meets_budget'])
        with self.assertRaises(RuntimeError):
            measure.frame_budget([], 15, 60)


if __name__ == '__main__':
    unittest.main()
