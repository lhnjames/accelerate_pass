import unittest
from unittest.mock import patch

from src.correctness import (
    _decode_text_output, compare_numeric, detect_correctness_mode,
    extract_numbers,
)


class TestTextClassification(unittest.TestCase):
    def test_plain_numeric_output_is_text(self):
        self.assertEqual(_decode_text_output(b"1.25 2.5\n"), "1.25 2.5\n")

    def test_invalid_utf8_binary_is_not_text(self):
        self.assertIsNone(_decode_text_output(b"BZh91AY&SY\x00\xff123"))

    def test_control_heavy_utf8_is_not_text(self):
        self.assertIsNone(_decode_text_output(b"1\x01\x02\x03\x04\x05\x06\x07\x08\x09"))


class TestCorrectnessModeSelection(unittest.TestCase):
    def test_text_numbers_use_numeric(self):
        with patch("src.correctness._run_capture", return_value=(0, b"1.0 2.0\n", None)):
            self.assertEqual(detect_correctness_mode("unused"), "numeric")

    def test_deterministic_binary_with_digits_uses_hash(self):
        binary = b"BZh91AY&SY\x00\xff123456"
        with patch("src.correctness._run_capture", return_value=(0, binary, None)):
            self.assertEqual(detect_correctness_mode("unused"), "hash")

    def test_nondeterministic_binary_uses_exit_only(self):
        runs = [(0, b"\x00first1", None), (0, b"\x00second2", None)]
        with patch("src.correctness._run_capture", side_effect=runs):
            self.assertEqual(detect_correctness_mode("unused"), "exit_only")


class TestNumericCorrectness(unittest.TestCase):
    def test_nan_and_inf_are_never_silently_skipped(self):
        self.assertEqual(extract_numbers("1.0 nan 2.0"), "NaN in output")
        self.assertEqual(extract_numbers("1.0 -Infinity 2.0"), "Inf in output")
        # Identifiers containing the same letters are not special values.
        self.assertEqual(extract_numbers("infinite_loop value1 2.0"), [2.0])

    def test_near_zero_values_use_absolute_tolerance(self):
        self.assertTrue(compare_numeric([0.0], [5e-5], epsilon=1e-4)[0])
        self.assertFalse(compare_numeric([0.0], [2e-4], epsilon=1e-4)[0])

    def test_relative_comparison_is_symmetric(self):
        forward = compare_numeric([100.0], [100.005], epsilon=1e-4)[0]
        reverse = compare_numeric([100.005], [100.0], epsilon=1e-4)[0]
        self.assertEqual(forward, reverse)
        self.assertTrue(forward)


if __name__ == "__main__":
    unittest.main()
