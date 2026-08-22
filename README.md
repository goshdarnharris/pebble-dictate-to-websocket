# Pebble Dictate to WebSocket

A Pebble Time 2 watchapp that starts native dictation on launch and forwards the
complete transcript through PebbleKit JS to a WebSocket server.

See [the functional specification](docs/functional-specification.md) for the
interaction and protocol contracts.

## Configure

Set `WEBSOCKET_URL` near the top of `src/pkjs/index.js` to a `ws://` or `wss://`
endpoint reachable by the phone. The checked-in value is an example local
network address and must be changed for the deployment network.

The server must reply within one second with a matching acknowledgement:

```json
{
  "version": 1,
  "type": "ack",
  "requestId": "1a2b3c4d"
}
```

## Run the example server

The single-file `dictation_websocket_server.py` module parses the application's
JSON requests and sends the required acknowledgement after a handler accepts a
message.

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
from dictation_websocket_server import DictationMessage, run_server


def handle(message: DictationMessage) -> None:
    process(message.transcript)


run_server(handle, host="0.0.0.0", port=8080)
```

The callback may be synchronous or asynchronous. Returning normally sends an
`ack`. Raising `DictationRejected("code")` sends a correlated application
error. Other callback exceptions are logged without transcript content and
return `internal_error`.

## Build and test

Enter the repository's Nix development environment, then run:

```sh
npm test
pebble build
pixi run test-server
```

Install on an `emery` emulator for UI and lifecycle smoke testing:

```sh
pebble install --emulator emery
pebble logs --emulator emery
```

Native dictation and the complete network flow require a physical Pebble Time
2, a connected phone running the Pebble app, and the configured WebSocket
server.
