from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
DRIVER = ROOT / "tuyaos/tuyaos_adapter/src/driver"


class TklContractGuardrailsTest(unittest.TestCase):
    def test_i2c_master_rejects_addresses_outside_7bit_range(self):
        source = (DRIVER / "tkl_i2c.c").read_text(encoding="utf-8")
        self.assertEqual(source.count("dev_addr > 0x7Fu"), 2)

    def test_uart_deinit_keeps_handle_open_if_rx_worker_did_not_exit(self):
        source = (DRIVER / "tkl_uart.c").read_text(encoding="utf-8")
        deinit = source[
            source.index("OPERATE_RET tkl_uart_deinit"):
            source.index("int tkl_uart_write")
        ]
        timeout_check = deinit.index("if (s_uart_rx_thread[port_id] != NULL)")
        close_handle = deinit.index("dev_close(s_uart_handles[port_id])")
        self.assertLess(timeout_check, close_handle)
        self.assertIn("return OPRT_COM_ERROR", deinit[timeout_check:close_handle])


if __name__ == "__main__":
    unittest.main()
