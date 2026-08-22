import asyncio
import json
import socket
import unittest

from websockets.asyncio.client import connect
from websockets.exceptions import ConnectionClosedError

from dictation_websocket_server import (
    DictationMessage,
    DictationProtocolError,
    DictationRejected,
    acknowledgement_payload,
    dictation_messages,
    error_payload,
    parse_dictation_message,
    start_server,
)


REQUEST_ID = "1a2b3c4d"


def request_payload(transcript="hello"):
    return {
        "version": 1,
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
        payload["version"] = 2

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
            acknowledgement_payload(REQUEST_ID),
            {
                "version": 1,
                "type": "ack",
                "requestId": REQUEST_ID,
            },
        )
        self.assertEqual(
            error_payload(REQUEST_ID, "rejected"),
            {
                "version": 1,
                "type": "error",
                "requestId": REQUEST_ID,
                "code": "rejected",
            },
        )


class DictationServerTest(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.messages = []

        async def handler(message):
            if message.transcript == "reject":
                raise DictationRejected("not_allowed")
            self.messages.append(message)

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

    async def test_acknowledges_after_handler_accepts_message(self):
        async with connect(self.uri) as websocket:
            await websocket.send(json.dumps(request_payload("accepted")))
            response = json.loads(await websocket.recv())

        self.assertEqual(
            response,
            {
                "version": 1,
                "type": "ack",
                "requestId": REQUEST_ID,
            },
        )
        self.assertEqual(
            self.messages,
            [DictationMessage(REQUEST_ID, "accepted")],
        )

    async def test_acknowledges_before_handler_completes(self):
        handler_started = asyncio.Event()
        allow_handler_to_finish = asyncio.Event()

        async def handler(message):
            self.messages.append(message)
            handler_started.set()
            await allow_handler_to_finish.wait()

        self.server.close()
        await self.server.wait_closed()
        self.server = await start_server(handler, host="127.0.0.1", port=0)
        self.port = self.server.sockets[0].getsockname()[1]
        self.uri = "ws://127.0.0.1:{}".format(self.port)

        async with connect(self.uri) as websocket:
            await websocket.send(json.dumps(request_payload("accepted")))
            response = json.loads(await websocket.recv())
            await handler_started.wait()

        self.assertEqual(response, acknowledgement_payload(REQUEST_ID))
        allow_handler_to_finish.set()

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

    async def test_acknowledges_application_rejection(self):
        async with connect(self.uri) as websocket:
            await websocket.send(json.dumps(request_payload("reject")))
            response = json.loads(await websocket.recv())

        self.assertEqual(response, acknowledgement_payload(REQUEST_ID))
        self.assertEqual(self.messages, [])

    async def test_closes_uncorrelated_invalid_request(self):
        async with connect(self.uri) as websocket:
            await websocket.send("not json")
            with self.assertRaises(ConnectionClosedError) as raised:
                await websocket.recv()

        self.assertEqual(raised.exception.rcvd.code, 1008)


class DictationMessageGeneratorTest(unittest.IsolatedAsyncioTestCase):
    async def test_yields_message_after_acknowledgement(self):
        with socket.socket() as socket_probe:
            socket_probe.bind(("127.0.0.1", 0))
            port = socket_probe.getsockname()[1]

        messages = dictation_messages(host="127.0.0.1", port=port)
        next_message = asyncio.ensure_future(messages.__anext__())
        await asyncio.sleep(0.01)

        try:
            async with connect("ws://127.0.0.1:{}".format(port)) as websocket:
                await websocket.send(json.dumps(request_payload("streamed")))
                response = json.loads(await websocket.recv())
                message = await next_message

            self.assertEqual(response, acknowledgement_payload(REQUEST_ID))
            self.assertEqual(
                message,
                DictationMessage(REQUEST_ID, "streamed"),
            )
        finally:
            await messages.aclose()


if __name__ == "__main__":
    unittest.main()
