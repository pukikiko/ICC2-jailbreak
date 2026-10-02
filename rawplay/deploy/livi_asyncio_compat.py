# LIVI's python helper predates python 3.14, where asyncio.get_event_loop() raises
# RuntimeError when there is no current loop (up to 3.13 it created one). its iap2
# link_layer calls get_event_loop() at import time, so without this the helper dies in a
# restart loop and wireless CarPlay/Android Auto never comes up. loaded from a .pth file
# because ubuntu ships its own /usr/lib/python3.14/sitecustomize.py that shadows ours.
import asyncio
import asyncio.events

_orig_get_event_loop = asyncio.events.get_event_loop


def _get_event_loop():
    try:
        return _orig_get_event_loop()
    except RuntimeError:
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        return loop


asyncio.events.get_event_loop = _get_event_loop
asyncio.get_event_loop = _get_event_loop
