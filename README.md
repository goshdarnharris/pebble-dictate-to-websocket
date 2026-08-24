# Pebble Dictate to WebSocket

A Pebble Time 2 watchapp that starts native dictation on launch and forwards the
complete transcript through PebbleKit JS to a WebSocket server.

See [the functional specification](docs/functional-specification.md) for the
interaction and protocol contracts.

## Configure

Set `WEBSOCKET_URL` near the top of `src/pkjs/index.js` to a `ws://` or `wss://`
endpoint reachable by the phone. The checked-in value is an example local
network address and must be changed for the deployment network.

The server must return a matching final result within 20 seconds:

```json
{
  "version": 2,
  "type": "result",
  "requestId": "1a2b3c4d",
  "success": true,
  "response": "Accepted"
}
```

`response` must contain at most 1,024 UTF-8 bytes. PebbleKit JS divides it into
UTF-8-safe AppMessage chunks, and the watch shows it under a `Success` or
`Failure` heading for five seconds, with one short vibration for success or
three for failure. Pressing any button cancels automatic close; UP and DOWN
continue scrolling, and BACK exits manually. Protocol-v1 `ack` servers are not
compatible.

## Run the example server

The single-file `dictation_websocket_server.py` module parses the application's
JSON requests, waits for application processing, and returns the handler's
validated final result.

Start its command-line server on port 8080:

```sh
pixi run server
```

The CLI binds to all interfaces and prints each accepted message as JSON. Set
`WEBSOCKET_URL` to `ws://<computer-lan-address>:8080/` so the phone can reach
it. The server has no authentication and should only be exposed on a trusted
network.

It can also be imported:

```python
from dictation_websocket_server import (
  DictationMessage,
  ServerResult,
  run_server,
)


def handle(message: DictationMessage) -> ServerResult:
    process(message.transcript)
  return ServerResult(success=True, response="Accepted")


run_server(handle, host="0.0.0.0", port=8080)
```

The callback may be synchronous or asynchronous and must return
`ServerResult`. Callback exceptions produce a correlated `handler_error` frame;
invalid or oversized return values produce `invalid_handler_result`. These are
bridge failures and are not shown verbatim on the watch.

For generator-oriented applications, consume `dictation_messages()` as an
async generator. It starts the server when iteration begins and closes it when
the generator is closed:

```python
from dictation_websocket_server import ServerResult, dictation_messages


async for exchange in dictation_messages(host="0.0.0.0", port=8080):
    process(exchange.message.transcript)
    exchange.respond(ServerResult(success=True, response="Accepted"))
```

Each exchange accepts exactly one response. Closing the iterator cancels any
unresolved exchange and shuts down the server.

## Build and test

Enter the repository's Nix development environment, then run:

```sh
pixi run npm test
pixi run test-server
pixi run pebble build
```

Install on an `emery` emulator for UI and lifecycle smoke testing:

```sh
pebble install --emulator emery --vnc
pebble logs --emulator emery --vnc
```

Native dictation and the complete network flow require a physical Pebble Time
2, a connected phone running the Pebble app, and the configured WebSocket
server.
