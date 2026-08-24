"""WebSocket server and parser for Pebble Dictate WS messages.

Run the example server with:

    python dictation_websocket_server.py --host 0.0.0.0 --port 8080

Or provide an application callback:

    from dictation_websocket_server import DictationMessage, run_server

    def handle(message: DictationMessage) -> None:
        process(message.transcript)

    run_server(handle, host="0.0.0.0", port=8080)
"""

from __future__ import annotations

import argparse
import asyncio
import inspect
import json
import logging
from dataclasses import dataclass
from typing import AsyncIterator, Awaitable, Callable, Dict, Optional, Union

from websockets.asyncio.server import Server, ServerConnection, serve
from websockets.exceptions import ConnectionClosed

PROTOCOL_VERSION = 1
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8080
DEFAULT_MAX_SIZE = 1024 * 1024

LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True)
class DictationMessage:
    """One validated dictation request."""

    request_id: str
    transcript: str


class DictationProtocolError(ValueError):
    """A request that doesn't match the Dictate WS protocol."""

    def __init__(
        self,
        code: str,
        message: str,
        request_id: Optional[str] = None,
    ) -> None:
        super().__init__(message)
        self.code = code
        self.request_id = request_id



HandlerResult = Optional[Awaitable[None]]
MessageHandler = Callable[[DictationMessage], HandlerResult]


def _is_request_id(value: object) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and all(0x20 <= ord(character) <= 0x7E for character in value)
    )


def _is_error_code(value: object) -> bool:
    return (
        isinstance(value, str)
        and bool(value)
        and all(0x21 <= ord(character) <= 0x7E for character in value)
    )


def parse_dictation_message(frame: Union[str, bytes]) -> DictationMessage:
    """Parse and validate one WebSocket text frame.

    Raises:
        DictationProtocolError: If the frame isn't a version-1 dictation
            request. The exception includes a request ID when the incoming
            frame supplied a valid one.
    """

    if not isinstance(frame, str):
        raise DictationProtocolError(
            "unsupported_frame",
            "dictation requests must use WebSocket text frames",
        )

    try:
        payload = json.loads(frame)
    except json.JSONDecodeError as error:
        raise DictationProtocolError(
            "invalid_json",
            "dictation request is not valid JSON",
        ) from error

    if not isinstance(payload, dict):
        raise DictationProtocolError(
            "invalid_message",
            "dictation request must be a JSON object",
        )

    raw_request_id = payload.get("requestId")
    request_id = raw_request_id if _is_request_id(raw_request_id) else None

    if type(payload.get("version")) is not int or payload["version"] != 1:
        raise DictationProtocolError(
            "unsupported_version",
            "dictation request version must be 1",
            request_id,
        )

    if payload.get("type") != "dictation":
        raise DictationProtocolError(
            "invalid_type",
            'dictation request type must be "dictation"',
            request_id,
        )

    if request_id is None:
        raise DictationProtocolError(
            "invalid_request_id",
            "dictation request ID must be nonempty printable ASCII",
        )

    transcript = payload.get("transcript")
    if not isinstance(transcript, str) or not transcript:
        raise DictationProtocolError(
            "invalid_transcript",
            "dictation transcript must be a nonempty string",
            request_id,
        )

    return DictationMessage(request_id=request_id, transcript=transcript)


def acknowledgement_payload(request_id: str) -> Dict[str, object]:
    """Build the protocol acknowledgement object for a request."""

    if not _is_request_id(request_id):
        raise ValueError("invalid request ID")
    return {
        "version": PROTOCOL_VERSION,
        "type": "ack",
        "requestId": request_id,
    }


def error_payload(request_id: str, code: str) -> Dict[str, object]:
    """Build a correlated protocol error object."""

    if not _is_request_id(request_id):
        raise ValueError("invalid request ID")
    if not _is_error_code(code):
        raise ValueError("invalid error code")
    return {
        "version": PROTOCOL_VERSION,
        "type": "error",
        "requestId": request_id,
        "code": code,
    }


def _encode_payload(payload: Dict[str, object]) -> str:
    return json.dumps(payload, ensure_ascii=False, separators=(",", ":"))


async def _send_payload(
    connection: ServerConnection,
    payload: Dict[str, object],
) -> bool:
    try:
        await connection.send(_encode_payload(payload))
    except ConnectionClosed:
        return False
    return True


async def _dispatch_message(
    connection: ServerConnection,
    handler: MessageHandler,
    message: DictationMessage,
) -> bool:
    if not await _send_payload(
        connection,
        acknowledgement_payload(message.request_id),
    ):
        return False

    try:
        result = handler(message)
        if inspect.isawaitable(result):
            await result
        elif result is not None:
            raise TypeError("dictation handler must return None or an awaitable")
    except Exception as error:
        LOGGER.error(
            "Delivered dictation handler failed for %s (%s)",
            message.request_id,
            type(error).__name__,
        )

    return True


async def _handle_connection(
    connection: ServerConnection,
    handler: MessageHandler,
) -> None:
    try:
        async for frame in connection:
            try:
                message = parse_dictation_message(frame)
            except DictationProtocolError as error:
                LOGGER.info("Rejected dictation frame: %s", error.code)
                if error.request_id is None:
                    await connection.close(code=1008, reason=error.code)
                    return
                if not await _send_payload(
                    connection,
                    error_payload(error.request_id, error.code),
                ):
                    return
                continue

            if not await _dispatch_message(connection, handler, message):
                return
    except ConnectionClosed:
        return


def _accept_message(message: DictationMessage) -> None:
    del message


async def start_server(
    handler: Optional[MessageHandler] = None,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    *,
    max_size: int = DEFAULT_MAX_SIZE,
) -> Server:
    """Start a server and return its `websockets` Server handle.

    The caller owns the returned server and should call `close()` followed by
    `await wait_closed()` when finished. A port of `0` asks the OS to choose an
    available port.
    """

    callback = handler or _accept_message

    async def connection_handler(connection: ServerConnection) -> None:
        await _handle_connection(connection, callback)

    return await serve(
        connection_handler,
        host,
        port,
        max_size=max_size,
    )


async def dictation_messages(
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    *,
    max_size: int = DEFAULT_MAX_SIZE,
) -> AsyncIterator[DictationMessage]:
    """Yield delivered dictation messages until iteration is closed.

    The server is started when iteration begins and closed when the generator
    is closed. Each valid message is acknowledged before it is yielded.
    """

    messages: asyncio.Queue[DictationMessage] = asyncio.Queue()

    def enqueue(message: DictationMessage) -> None:
        messages.put_nowait(message)

    server = await start_server(
        enqueue,
        host,
        port,
        max_size=max_size,
    )
    try:
        while True:
            yield await messages.get()
    finally:
        server.close()
        await server.wait_closed()


async def serve_forever(
    handler: Optional[MessageHandler] = None,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    *,
    max_size: int = DEFAULT_MAX_SIZE,
) -> None:
    """Start the dictation server and run until cancelled."""

    server = await start_server(
        handler,
        host,
        port,
        max_size=max_size,
    )
    try:
        await server.serve_forever()
    finally:
        server.close()
        await server.wait_closed()


def run_server(
    handler: Optional[MessageHandler] = None,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    *,
    max_size: int = DEFAULT_MAX_SIZE,
) -> None:
    """Synchronously run the dictation server until interrupted."""

    asyncio.run(
        serve_forever(
            handler,
            host,
            port,
            max_size=max_size,
        )
    )


def _print_message(message: DictationMessage) -> None:
    print(
        json.dumps(
            {
                "requestId": message.request_id,
                "transcript": message.transcript,
            },
            ensure_ascii=False,
        ),
        flush=True,
    )


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Receive and acknowledge Pebble Dictate WS messages.",
    )
    parser.add_argument(
        "--host",
        default="0.0.0.0",
        help="interface to bind (default: %(default)s)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=DEFAULT_PORT,
        help="TCP port to bind (default: %(default)s)",
    )
    parser.add_argument(
        "--max-size",
        type=int,
        default=DEFAULT_MAX_SIZE,
        help="maximum WebSocket frame size in bytes (default: %(default)s)",
    )
    return parser.parse_args()


def main() -> None:
    args = _parse_args()
    logging.basicConfig(level=logging.INFO)
    LOGGER.info("Listening for dictation on ws://%s:%d", args.host, args.port)
    try:
        run_server(
            _print_message,
            host=args.host,
            port=args.port,
            max_size=args.max_size,
        )
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
