"""WebSocket server and parser for Pebble Dictate WS messages.

Run the example server with:

    python dictation_websocket_server.py --host 0.0.0.0 --port 8080

Or provide an application callback:

    from dictation_websocket_server import ServerResult, run_server

    def handle(message: DictationMessage) -> ServerResult:
        process(message.transcript)
        return ServerResult(success=True, response="Accepted")

    run_server(handle, host="0.0.0.0", port=8080)
"""

from __future__ import annotations

import argparse
import asyncio
import inspect
import json
import logging
from dataclasses import dataclass
from enum import Enum, auto
from typing import AsyncIterator, Awaitable, Callable, Dict, Optional, Set, Union

from websockets.asyncio.server import Server, ServerConnection, serve
from websockets.exceptions import ConnectionClosed

PROTOCOL_VERSION = 2
MAX_RESPONSE_BYTES = 1024
DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8080
DEFAULT_MAX_SIZE = 1024 * 1024

LOGGER = logging.getLogger(__name__)


@dataclass(frozen=True)
class DictationMessage:
    """One validated dictation request."""

    request_id: str
    transcript: str


@dataclass(frozen=True)
class ServerResult:
    """One application-level result returned to the watch."""

    success: bool
    response: str


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


class BridgeState(Enum):
    """Lifecycle of one validated WebSocket-to-handler request."""

    RECEIVED = auto()
    DISPATCHING = auto()
    SENDING_RESULT = auto()
    COMPLETE = auto()
    FAILED = auto()


class BridgeError(Enum):
    """Failure owned by the WebSocket-to-handler bridge."""

    NONE = auto()
    PROTOCOL = auto()
    SEND = auto()
    HANDLER = auto()
    HANDLER_RESULT = auto()


@dataclass
class BridgeSession:
    """State and most recent error for one validated request."""

    message: DictationMessage
    state: BridgeState = BridgeState.RECEIVED
    error: BridgeError = BridgeError.NONE



HandlerResult = Union[ServerResult, Awaitable[ServerResult]]
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

    if (
        type(payload.get("version")) is not int
        or payload["version"] != PROTOCOL_VERSION
    ):
        raise DictationProtocolError(
            "unsupported_version",
            f"dictation request version must be {PROTOCOL_VERSION}",
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


def _validate_server_result(result: object) -> ServerResult:
    if not isinstance(result, ServerResult):
        raise TypeError("dictation handler must return ServerResult")
    if type(result.success) is not bool:
        raise ValueError("server result success must be a boolean")
    if not isinstance(result.response, str):
        raise ValueError("server result response must be a string")
    try:
        response_bytes = len(result.response.encode("utf-8"))
    except UnicodeEncodeError as error:
        raise ValueError("server result response must be valid UTF-8") from error
    if response_bytes > MAX_RESPONSE_BYTES:
        raise ValueError("server result response exceeds 1024 UTF-8 bytes")
    return result


def result_payload(
    request_id: str,
    result: ServerResult,
) -> Dict[str, object]:
    """Build one correlated final result object."""

    if not _is_request_id(request_id):
        raise ValueError("invalid request ID")
    validated = _validate_server_result(result)
    return {
        "version": PROTOCOL_VERSION,
        "type": "result",
        "requestId": request_id,
        "success": validated.success,
        "response": validated.response,
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
    bridge = DictationWebSocketBridge(connection, handler)
    return await bridge.dispatch(BridgeSession(message))


class DictationWebSocketBridge:
    """Own protocol, handler, and final-result dispatch for a connection."""

    def __init__(
        self,
        connection: ServerConnection,
        handler: MessageHandler,
    ) -> None:
        self.connection = connection
        self.handler = handler
        self.last_error = BridgeError.NONE
        self.last_session: Optional[BridgeSession] = None

    async def handle_frame(self, frame: Union[str, bytes]) -> bool:
        try:
            message = parse_dictation_message(frame)
        except DictationProtocolError as error:
            self.last_error = BridgeError.PROTOCOL
            LOGGER.info("Rejected dictation frame: %s", error.code)
            if error.request_id is None:
                await self.connection.close(code=1008, reason=error.code)
                return False
            return await _send_payload(
                self.connection,
                error_payload(error.request_id, error.code),
            )

        session = BridgeSession(message)
        self.last_session = session
        return await self.dispatch(session)

    async def dispatch(self, session: BridgeSession) -> bool:
        try:
            session.state = BridgeState.DISPATCHING
            result = self.handler(session.message)
            if inspect.isawaitable(result):
                result = await result
        except Exception as error:
            session.state = BridgeState.FAILED
            session.error = BridgeError.HANDLER
            self.last_error = session.error
            LOGGER.error(
                "Delivered dictation handler failed for %s (%s)",
                session.message.request_id,
                type(error).__name__,
            )
            return await _send_payload(
                self.connection,
                error_payload(session.message.request_id, "handler_error"),
            )

        try:
            payload = result_payload(session.message.request_id, result)
        except (TypeError, ValueError) as error:
            session.state = BridgeState.FAILED
            session.error = BridgeError.HANDLER_RESULT
            self.last_error = session.error
            LOGGER.error(
                "Dictation handler returned an invalid result for %s (%s)",
                session.message.request_id,
                type(error).__name__,
            )
            return await _send_payload(
                self.connection,
                error_payload(
                    session.message.request_id,
                    "invalid_handler_result",
                ),
            )

        session.state = BridgeState.SENDING_RESULT
        if not await _send_payload(self.connection, payload):
            session.state = BridgeState.FAILED
            session.error = BridgeError.SEND
            self.last_error = session.error
            return False
        session.state = BridgeState.COMPLETE
        return True


async def _handle_connection(
    connection: ServerConnection,
    handler: MessageHandler,
) -> None:
    bridge = DictationWebSocketBridge(connection, handler)
    try:
        async for frame in connection:
            if not await bridge.handle_frame(frame):
                return
    except ConnectionClosed:
        return


def _accept_message(message: DictationMessage) -> ServerResult:
    del message
    return ServerResult(success=True, response="Accepted")


class DictationExchange:
    """A yielded dictation request awaiting one application response."""

    def __init__(self, message: DictationMessage) -> None:
        self.message = message
        self._response: asyncio.Future[ServerResult] = (
            asyncio.get_running_loop().create_future()
        )

    def respond(self, success: bool, response: str = "") -> None:
        if self._response.done():
            raise RuntimeError("dictation exchange already resolved")
        self._response.set_result(ServerResult(success=success, response=response))

    async def wait_for_response(self) -> ServerResult:
        return await self._response

    def cancel(self) -> None:
        if not self._response.done():
            self._response.cancel()


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
) -> AsyncIterator[DictationExchange]:
    """Yield dictation exchanges until iteration is closed.

    Each exchange must receive exactly one `ServerResult` through `respond()`.
    """

    exchanges: asyncio.Queue[DictationExchange] = asyncio.Queue()
    pending: Set[DictationExchange] = set()

    async def enqueue(message: DictationMessage) -> ServerResult:
        exchange = DictationExchange(message)
        pending.add(exchange)
        exchanges.put_nowait(exchange)
        try:
            return await exchange.wait_for_response()
        finally:
            pending.discard(exchange)

    server = await start_server(
        enqueue,
        host,
        port,
        max_size=max_size,
    )
    try:
        while True:
            yield await exchanges.get()
    finally:
        for exchange in pending:
            exchange.cancel()
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


def _print_message(message: DictationMessage) -> ServerResult:
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
    return ServerResult(success=True, response="Accepted")


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Receive and respond to Pebble Dictate WS messages.",
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
