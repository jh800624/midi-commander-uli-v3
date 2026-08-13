import unittest

from lib.cmdBinaryPacker import get_hid_code, safe_int


class PackerLogicTests(unittest.TestCase):
    def test_safe_int(self):
        self.assertEqual(safe_int("5.0"), 5)
        self.assertEqual(safe_int("127"), 127)
        self.assertEqual(safe_int(""), 0)
        self.assertEqual(safe_int("nan"), 0)

    def test_hid_codes(self):
        self.assertEqual(get_hid_code("a"), 4)
        self.assertEqual(get_hid_code("z"), 29)
        self.assertEqual(get_hid_code("1"), 30)
        self.assertEqual(get_hid_code("0"), 39)


if __name__ == "__main__":
    unittest.main()
