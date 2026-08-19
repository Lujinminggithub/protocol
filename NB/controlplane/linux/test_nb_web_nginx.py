#!/usr/bin/env python3
import pathlib
import re
import unittest


TEMPLATE = pathlib.Path(__file__).with_name("nb-web.nginx.conf.example")


class NBWebNginxConfigTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.config = TEMPLATE.read_text(encoding="utf-8")

    def test_does_not_restrict_operator_source_ip(self):
        directives = re.findall(r"(?m)^\s*(?:allow|deny)\s+[^;]+;", self.config)
        self.assertEqual([], directives)

    def test_terminates_tls_and_proxies_only_to_loopback_web(self):
        self.assertIn("listen 9091 ssl;", self.config)
        self.assertIn("ssl_certificate ", self.config)
        self.assertIn("ssl_certificate_key ", self.config)
        self.assertIn("proxy_pass http://127.0.0.1:19091;", self.config)


if __name__ == "__main__":
    unittest.main()
