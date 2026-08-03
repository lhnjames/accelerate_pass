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
    def test_fractional_values_use_numeric(self):
        # A real FP kernel dump: vectorization may reassociate and move the
        # last bits, so the tolerant tier is the correct gate here.
        with patch("src.correctness._run_capture",
                   return_value=(0, b"0.155941 2.71828\n", None)):
            self.assertEqual(detect_correctness_mode("unused"), "numeric")

    def test_deterministic_binary_with_digits_uses_hash(self):
        binary = b"BZh91AY&SY\x00\xff123456"
        with patch("src.correctness._run_capture", return_value=(0, binary, None)):
            self.assertEqual(detect_correctness_mode("unused"), "hash")

    def test_nondeterministic_binary_uses_exit_only(self):
        runs = [(0, b"\x00first1", None), (0, b"\x00second2", None)]
        with patch("src.correctness._run_capture", side_effect=runs):
            self.assertEqual(detect_correctness_mode("unused"), "exit_only")

    def test_integral_text_output_uses_hash_not_numeric(self):
        # telecom_crc32 prints a ~4e9 checksum. Under `numeric` a 1e-4
        # RELATIVE tolerance would let a wrong CRC differ by ±400000 and
        # still pass, so discrete output must be hashed exactly.
        with patch("src.correctness._run_capture",
                   return_value=(0, b"FFFFFFFF78DBCD64    3954 data.txt\n", None)):
            self.assertEqual(detect_correctness_mode("unused"), "hash")

    def test_integral_zero_prefixed_output_uses_hash(self):
        # network_patricia prints "0.000000 <hex id>: Found." -- the float
        # FORMATTING is decoration; every value is integral, so the payload
        # is discrete and must be hashed.
        with patch("src.correctness._run_capture",
                   return_value=(0, b"0.000000 00000035: Found.\n", None)):
            self.assertEqual(detect_correctness_mode("unused"), "hash")

    def test_nondeterministic_bytes_with_stable_numbers_use_numeric(self):
        # Same values, different surrounding text (e.g. an echoed scratch
        # path): hashing is impossible but the values can still be compared.
        # Note a varying *numeric* field (a printed wall-clock time) would
        # correctly fall through to exit_only -- it is extracted as a value.
        runs = [(0, b"scratch /tmp/aaa\nresult 1.5 2.5\n", None),
                (0, b"scratch /tmp/bbb\nresult 1.5 2.5\n", None)]
        with patch("src.correctness._run_capture", side_effect=runs):
            self.assertEqual(detect_correctness_mode("unused"), "numeric")

    def test_nondeterministic_values_use_exit_only(self):
        # cBench security_sha: its own reference digest differs on every run
        # (uninitialised stack bytes reach sha_transform on LP64), so no
        # candidate can ever be validated against it.
        runs = [(0, b"318b06fea9b9a83a 4908a0b0ad98fa45\n", None),
                (0, b"9ec89acc1f298e04 22b59f12235ec290\n", None)]
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


class TestReferenceHealth(unittest.TestCase):
    """Catches a benchmark that isn't actually running.

    cBench bzip2_encode could not open its input file on one node, printed the
    error to stderr, and still EXITED 0 -- so exit code passed, stdout was
    empty so the hash check compared nothing to nothing and passed, and a
    0.96 ms no-op was scored as an 85 ms benchmark.
    """
    def test_empty_output_is_unhealthy(self):
        from src.correctness import reference_health
        with patch("src.correctness._run_capture", return_value=(0, b"", None)):
            r = reference_health("unused")
        self.assertFalse(r["ok"])
        self.assertIn("no output", r["reason"])

    def test_io_error_in_output_is_unhealthy(self):
        from src.correctness import reference_health
        msg = b"kernel_bzip2_encode: Can't open input file /data: No such file or directory.\n"
        with patch("src.correctness._run_capture", return_value=(0, msg, None)):
            r = reference_health("unused")
        self.assertFalse(r["ok"])

    def test_nonzero_exit_is_unhealthy(self):
        from src.correctness import reference_health
        with patch("src.correctness._run_capture", return_value=(1, b"output\n", None)):
            self.assertFalse(reference_health("unused")["ok"])

    def test_real_output_is_healthy(self):
        from src.correctness import reference_health
        with patch("src.correctness._run_capture", return_value=(0, b"1.25 2.5\n", None)):
            self.assertTrue(reference_health("unused")["ok"])


class TestHashModeRejectsEmptyReference(unittest.TestCase):
    def test_empty_reference_cannot_pass(self):
        from src.correctness import check_correctness
        with patch("src.correctness._run_capture", return_value=(0, b"", None)):
            ok, err = check_correctness("ref", "opt", "hash")
        self.assertFalse(ok)
        self.assertIn("no output", err)

if __name__ == "__main__":
    unittest.main()
