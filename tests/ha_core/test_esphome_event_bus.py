"""Exercise the native transport with Home Assistant's real event dispatcher."""

import asyncio
import importlib.util
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import threading
from types import ModuleType
import unittest

from homeassistant.core import HassJob, HassJobType, HomeAssistant

ROOT = Path(__file__).resolve().parents[2] / "custom_components/meizu_remoter_gateway"
PACKAGE = "meizu_real_ha_event_test"
package = ModuleType(PACKAGE)
package.__path__ = [str(ROOT)]
sys.modules[PACKAGE] = package
SPEC = importlib.util.spec_from_file_location(PACKAGE + ".esphome_client", ROOT / "esphome_client.py")
module = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = module
SPEC.loader.exec_module(module)

SETTINGS = {"esphome_device_id": "synthetic-node", "esphome_service": "test_node_meizu_puck", "api_source": "meizu_puck",
            "address": "02:00:00:00:00:01", "serialno": "ABC123"}


def event(request_id, operation, **changes):
    return {"protocol": "meizu_ble_v1", "device_id": SETTINGS["esphome_device_id"], "source": SETTINGS["api_source"],
            "address": SETTINGS["address"], "request_id": request_id, "operation": operation, "phase": "complete", "status": "ok",
            "available": "0", "measurement_age_ms": "-1", "update_interval": "1", "version": "0.1.0-esphome", **changes}


class RealEventBusTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.folder = TemporaryDirectory()
        self.addCleanup(self.folder.cleanup)
        asyncio.get_running_loop().set_debug(True)
        self.loop_thread = threading.get_ident()
        self.hass = HomeAssistant(self.folder.name)
        self.calls = asyncio.Queue()
        self.event_threads = []

        async def native_action(call):
            args = dict(call.data)
            self.calls.put_nowait(args)
            if args["operation"] == "status":
                self.hass.bus.async_fire("esphome.meizu_ble", event(args["request_id"], "status"))

        def on_message(message):
            self.event_threads.append(threading.get_ident())
            # Exercise a real HA state mutation from the listener's call chain.
            self.hass.states.async_set("sensor.synthetic_meizu_status", message["type"])

        self.hass.services.async_register("esphome", SETTINGS["esphome_service"], native_action)
        self.client = module.ESPHomeGatewayClient(self.hass, SETTINGS, on_message)

    async def asyncTearDown(self):
        await self.client.close()
        await self.hass.async_stop(force=True)

    async def test_real_status_handshake_and_poll_update_stay_in_event_loop(self):
        self.assertEqual(HassJob(self.client._event).job_type, HassJobType.Callback)
        info = await asyncio.wait_for(self.client.connect(), 2)
        self.assertEqual(info["serialno"], SETTINGS["serialno"])
        self.hass.bus.async_fire("esphome.meizu_ble", event("", "poll", phase="failed", status="connect_failed"))
        await self.hass.async_block_till_done()
        self.assertEqual(self.hass.states.get("sensor.synthetic_meizu_status").state, "devices")
        self.assertEqual(self.event_threads, [self.loop_thread])

    async def test_real_event_bus_completes_correlated_send_future(self):
        self.assertEqual(HassJob(self.client._event).job_type, HassJobType.Callback)
        await asyncio.wait_for(self.client.connect(), 2)
        self.calls.get_nowait()  # Status handshake.
        packet = {"type": "irsend", "data": {"device": SETTINGS["address"], "key": "1e001c000000000000"}}
        task = asyncio.create_task(self.client.send_ir(packet))
        call = await asyncio.wait_for(self.calls.get(), 2)
        self.assertEqual(call["operation"], "send")
        self.hass.bus.async_fire("esphome.meizu_ble", event(call["request_id"], "send", confirmed="1", cached="1", ack="4"))
        result = await asyncio.wait_for(task, 2)
        self.assertTrue(result["confirmed"] and result["bridge_acknowledged"])
        self.assertEqual(result["ack"], 4)


if __name__ == "__main__":
    unittest.main()
