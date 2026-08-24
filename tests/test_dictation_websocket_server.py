import asyncio
import json
import socket
import unittest

from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosedError

from dictation_websocket_server import (
    BridgeError,
    BridgeState,
    DictationMessage,
    DictationProtocolError,
    DictationWebSocketBridge,
    ServerResult,
    dictation_messages,
    error_payload,
    parse_dictation_message,
    result_payload,
    start_server,
)


REQUEST_ID = "1a2b3c4d"


class FakeConnection:
    def __init__(self):
        self.sent = []
        self.closed = None

    async def send(self, payload):
        self.sent.append(payload)

    async def close(self, code, reason):
        self.closed = (code, reason)


def request_payload(transcript="hello"):
    return {
        "version": 2,
        "type": "dictation",
        "requestId": REQUEST_ID,
        "transcript": transcript,
    }


class DictationParserTest(unittest.TestCase):
    def test_parses_valid_message(self):
        message = parse_dictation_message(
            json.dumps(request_payload("caf\u00e9"))
        )

        self.assertEqual(
            message,
            DictationMessage(
                request_id=REQUEST_ID,
                transcript="caf\u00e9",
            ),
        )

    def test_rejects_binary_frame(self):
        with self.assertRaisesRegex(
            DictationProtocolError,
            "text frames",
        ) as raised:
            parse_dictation_message(b"binary")

        self.assertEqual(raised.exception.code, "unsupported_frame")
        self.assertIsNone(raised.exception.request_id)

    def test_rejects_wrong_version_with_request_id(self):
        payload = request_payload()
        payload["version"] = 1

        with self.assertRaises(DictationProtocolError) as raised:
            parse_dictation_message(json.dumps(payload))

        self.assertEqual(raised.exception.code, "unsupported_version")
        self.assertEqual(raised.exception.request_id, REQUEST_ID)

    def test_rejects_boolean_version(self):
        payload = request_payload()
        payload["version"] = True

        with self.assertRaises(DictationProtocolError) as raised:
            parse_dictation_message(json.dumps(payload))

        self.assertEqual(raised.exception.code, "unsupported_version")

    def test_rejects_empty_transcript(self):
        with self.assertRaises(DictationProtocolError) as raised:
            parse_dictation_message(json.dumps(request_payload("")))

        self.assertEqual(raised.exception.code, "invalid_transcript")
        self.assertEqual(raised.exception.request_id, REQUEST_ID)

    def test_builds_protocol_responses(self):
        self.assertEqual(
            result_payload(
                REQUEST_ID,
                ServerResult(success=True, response="accepted"),
            ),
            {
                "version": 2,
                "type": "result",
                "requestId": REQUEST_ID,
                "success": True,
                "response": "accepted",
            },
        )
        self.assertEqual(
            error_payload(REQUEST_ID, "rejected"),
            {
                "version": 2,
                "type": "error",
                "requestId": REQUEST_ID,
                "code": "rejected",
            },
        )

    def test_rejects_oversized_server_response(self):
        with self.assertRaisesRegex(ValueError, "exceeds 1024"):
            result_payload(
                REQUEST_ID,
                ServerResult(success=True, response="x" * 1025),
            )


class DictationServerTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.messages = []

        async def handler(message):
            self.messages.append(message)
            return ServerResult(True, "accepted")

        self.server = await start_server(
            handler,
            host="127.0.0.1",
            port=0,
        )
        self.port = self.server.sockets[0].getsockname()[1]
        self.uri = "ws://127.0.0.1:{}".format(self.port)

    async def asyncTearDown(self):
        self.server.close()
        await self.server.wait_closed()

    async def test_returns_result_after_handler_accepts_message(self):
        async with connect(self.uri) as websocket:
            await websocket.send(json.dumps(request_payload("accepted")))
            response = json.loads(await websocket.recv())

        self.assertEqual(
            response,
            {
                "version": 2,
                "type": "result",
                "requestId": REQUEST_ID,
                "success": True,
                "response": "accepted",
            },
        )
        self.assertEqual(
            self.messages,
            [DictationMessage(REQUEST_ID, "accepted")],
        )

    async def test_waits_for_handler_before_returning_result(self):
        handler_started = asyncio.Event()
        allow_handler_to_finish = asyncio.Event()

        async def handler(message):
            self.messages.append(message)
            handler_started.set()
            await allow_handler_to_finish.wait()
            return ServerResult(True, "finished")

        self.server.close()
        await self.server.wait_closed()
        self.server = await start_server(handler, host="127.0.0.1", port=0)
        self.port = self.server.sockets[0].getsockname()[1]
        self.uri = "ws://127.0.0.1:{}".format(self.port)

        async with connect(self.uri) as websocket:
            await websocket.send(json.dumps(request_payload("accepted")))
            await handler_started.wait()
            response_task = asyncio.create_task(websocket.recv())
            self.assertFalse(response_task.done())
            allow_handler_to_finish.set()
            response = json.loads(await response_task)

        self.assertEqual(
            response,
            result_payload(REQUEST_ID, ServerResult(True, "finished")),
        )

    async def test_sends_correlated_protocol_error(self):
        payload = request_payload()
        payload["type"] = "other"

        async with connect(self.uri) as websocket:
            await websocket.send(json.dumps(payload))
            response = json.loads(await websocket.recv())

        self.assertEqual(response["type"], "error")
        self.assertEqual(response["requestId"], REQUEST_ID)
        self.assertEqual(response["code"], "invalid_type")
        self.assertEqual(self.messages, [])

    async def test_closes_uncorrelated_invalid_request(self):
        async with connect(self.uri) as websocket:
            await websocket.send("not json")
            with self.assertRaises(ConnectionClosedError) as raised:
                await websocket.recv()

        self.assertEqual(raised.exception.rcvd.code, 1008)


class DictationBridgeTest(unittest.IsolatedAsyncioTestCase):
    async def test_records_completed_handler_session(self):
        connection = FakeConnection()
        delivered = []

        def handler(message):
            delivered.append(message)
            return ServerResult(True, "bridged")

        bridge = DictationWebSocketBridge(connection, handler)

        self.assertTrue(
            await bridge.handle_frame(json.dumps(request_payload("bridged")))
        )

        self.assertEqual(
            delivered,
            [DictationMessage(REQUEST_ID, "bridged")],
        )
        self.assertEqual(bridge.last_session.state, BridgeState.COMPLETE)
        self.assertEqual(bridge.last_session.error, BridgeError.NONE)
        self.assertEqual(
            json.loads(connection.sent[0]),
            result_payload(REQUEST_ID, ServerResult(True, "bridged")),
        )

    async def test_records_nonfatal_handler_failure(self):
        connection = FakeConnection()

        def fail_handler(message):
            del message
            raise RuntimeError("failed")

        bridge = DictationWebSocketBridge(connection, fail_handler)

        self.assertTrue(
            await bridge.handle_frame(json.dumps(request_payload("bridged")))
        )
        self.assertEqual(bridge.last_session.state, BridgeState.FAILED)
        self.assertEqual(bridge.last_session.error, BridgeError.HANDLER)
        self.assertEqual(json.loads(connection.sent[0])["code"], "handler_error")

    async def test_reports_invalid_handler_result(self):
        connection = FakeConnection()
        bridge = DictationWebSocketBridge(connection, lambda message: None)

        self.assertTrue(
            await bridge.handle_frame(json.dumps(request_payload("bridged")))
        )
        self.assertEqual(bridge.last_session.state, BridgeState.FAILED)
        self.assertEqual(
            bridge.last_session.error,
            BridgeError.HANDLER_RESULT,
        )
        self.assertEqual(
            json.loads(connection.sent[0])["code"],
            "invalid_handler_result",
        )


class DictationMessageGeneratorTest(unittest.IsolatedAsyncioTestCase):
    async def test_yields_exchange_and_returns_its_response(self):
        with socket.socket() as socket_probe:
            socket_probe.bind(("127.0.0.1", 0))
            port = socket_probe.getsockname()[1]

        messages = dictation_messages(host="127.0.0.1", port=port)
        next_message = asyncio.ensure_future(messages.__anext__())
        await asyncio.sleep(0.01)

        try:
            async with connect("ws://127.0.0.1:{}".format(port)) as websocket:
                await websocket.send(json.dumps(request_payload("streamed")))
                exchange = await next_message
                exchange.respond(ServerResult(False, "rejected"))
                response = json.loads(await websocket.recv())

            self.assertEqual(
                response,
                result_payload(REQUEST_ID, ServerResult(False, "rejected")),
            )
            self.assertEqual(
                exchange.message,
                DictationMessage(REQUEST_ID, "streamed"),
            )
        finally:
            await messages.aclose()


if __name__ == "__main__":
    unittest.main()
