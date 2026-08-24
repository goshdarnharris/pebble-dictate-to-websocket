# Pebble Dictation-to-WebSocket Functional Specification

## 1. Purpose

This document specifies a small Pebble watchapp that captures a spoken
transcription and delivers it to a remote WebSocket server through PebbleKit
JS.

The intended interaction is:

1. The user launches the app, normally with a Quick Launch long press.
2. Pebble's native dictation UI starts immediately.
3. Pebble finalizes the transcription using its native speech and silence
   detection.
4. The watchapp transfers the complete transcription to PebbleKit JS.
5. PebbleKit JS sends the transcription to a build-time-configured WebSocket
   endpoint.
6. The remote server acknowledges the transcription.
7. The watchapp closes.

The first version prioritizes a short interaction, deterministic behavior, and
no retained data.

## 2. Target environment

- Watch platform: Pebble Time 2 (`emery`) only.
- Watch SDK: Pebble C SDK 4.x.
- Phone integration: PebbleKit JS packaged with the watchapp.
- Network endpoint: one WebSocket URL supplied as a build-time constant.
- Expected deployment: the phone and WebSocket server are on a low-latency
  local network.

Other Pebble platforms are not required to build or run this version.

## 3. Platform constraint and agreed interaction

The original interaction proposed that dictation run only while the Quick
Launch button remained pressed. The public Pebble C API cannot implement that
interaction while retaining the result:

- Button subscriptions belong to the watchapp's visible `Window`, while
  dictation presents a native system UI.
- `dictation_session_stop()` terminates the active dictation UI and guarantees
  that no status callback, and therefore no transcription, will be delivered
  afterward.

Consequently, releasing the Quick Launch button does **not** stop dictation and
does not control the watchapp lifetime. The supported interaction is:

- Launch starts dictation.
- Pebble's native speech/silence behavior finalizes dictation.
- Delivery success or failure ends the watchapp.

This behavior applies whether the app was launched by a Quick Launch hold, from
the launcher, or by another supported launch mechanism. The implementation may
record `launch_reason()`, `launch_button()`, and
`launch_get_quick_launch_action()` for diagnostics, but launch origin does not
alter the functional flow.

## 4. Scope

### 4.1 In scope

- Immediate native dictation on app launch.
- Automatic transcription finalization without a watch-side confirmation
  screen.
- Transfer of the complete UTF-8 transcription over one or more AppMessages.
- A single WebSocket request per successful transcription.
- A correlated, application-level server acknowledgement.
- Strict, short timeouts and visible failures.
- Deterministic cleanup after success, cancellation, or failure.

### 4.2 Out of scope

- A configuration page or runtime endpoint selection.
- Retry, offline queueing, persistence, or background delivery.
- Custom audio capture or a replacement dictation UI.
- Delivery of partial or deliberately truncated transcriptions.
- Multiple simultaneous dictation requests.
- WebSocket authentication or an application authorization protocol.
- Server discovery on the local network.
- Support for watch platforms other than `emery`.
- Recovery from the Pebble app, PebbleKit JS runtime, or phone process being
  forcibly terminated.

The configured URL may use `ws://` or `wss://`. Transport security,
certificates, authentication, and authorization are deployment concerns rather
than behavior defined by this version.

## 5. Components and responsibilities

### 5.1 Watchapp

The watchapp:

- initializes AppMessage and creates a single request identifier;
- tells PebbleKit JS to prepare the WebSocket connection;
- starts one native dictation session;
- maps native dictation results to success, cancellation, or error;
- divides a successful transcription into lossless AppMessage chunks;
- sends those chunks sequentially;
- waits for PebbleKit JS and remote-server status messages;
- owns the user-visible state and terminal error display; and
- exits only at a terminal state.

### 5.2 PebbleKit JS

PebbleKit JS:

- maintains at most one active request;
- opens the configured WebSocket while dictation is in progress;
- validates and reassembles transcript chunks;
- sends exactly one JSON request after receiving the complete transcription;
- accepts only a matching server acknowledgement as success;
- reports send start, acknowledgement, or failure to the watchapp;
- performs no retry and retains no completed transcription; and
- closes the request's WebSocket when the request reaches a terminal state.

### 5.3 Remote WebSocket server

The server:

- accepts the request JSON specified in section 9;
- returns an acknowledgement on reception of valid dictation data
- processes each request according to its own policy;
- responds quickly enough to satisfy the remote acknowledgement deadline.

Server-side deduplication is recommended but is outside the watchapp's
responsibility.

## 6. User experience

### 6.1 Normal flow

1. The app shows a minimal `Starting...` state while initializing.
2. Native dictation appears immediately and handles listening and
   transcription.
3. After a successful transcription, the watchapp shows `Sending...`.
4. When the matching remote acknowledgement reaches the watch, the app closes
   immediately. No additional success screen or user input is required.

The app must not claim success based only on AppMessage delivery or a successful
call to `WebSocket.send()`.

### 6.2 Failure and cancellation flow

The app displays a concise status for three seconds and then closes. The
implementation must distinguish at least these user-facing categories:

| Category | Example display | Causes |
| --- | --- | --- |
| Cancelled | `Dictation cancelled` | User exits or rejects native dictation |
| No speech | `No speech heard` | Native dictation reports no speech |
| Dictation error | `Dictation error` | Dictation is disabled, cannot be created, or cannot start |
| Phone/voice connection | `Phone unavailable` | Native voice connectivity or initial AppMessage failure |
| Delivery | `Delivery failed` | Chunk, WebSocket, protocol, server error, or bridge failure |
| Timeout | `WS server timed out` | WebSocket acknowledgement deadline expires |

Exact typography and layout are implementation details, but each message must
fit on `emery` without scrolling. The three-second duration begins when the
watchapp's error state becomes visible.

## 7. Watchapp behavior

### 7.1 Initialization

On launch, the watchapp must:

1. Create and show its root window.
2. Register AppMessage inbox, dropped-message, outbox-sent, and outbox-failed
   callbacks before opening AppMessage.
3. Open AppMessage with buffers large enough for the protocol. Runtime maximum
   buffer sizes should be used when available.
4. Generate a nonzero request identifier that is unique within the last 12 hours 
   or current PebbleKit JS connection, whichever ends first.
5. Send `SESSION_BEGIN` to PebbleKit JS.
6. Create and start the native dictation session without waiting for the
   WebSocket to finish connecting.

If synchronous setup fails before dictation can start, the app exits or enters
an appropriate error state. If asynchronous AppMessage or WebSocket preparation
fails while dictation is active, the app stops the dictation
session, shows the error, and sends a best-effort `SESSION_END`. Stopping
dictation in this failure path intentionally discards any incomplete result.

### 7.2 Dictation configuration

The dictation session must:

- allocate enough space for the complete native transcription by requesting an
  unbounded transcription buffer (`buffer_size` of `0`);
- disable transcription confirmation with
  `dictation_session_enable_confirmation(session, false)`; and
- disable native error dialogs with
  `dictation_session_enable_error_dialogs(session, false)`.

Disabling confirmation allows native speech/silence detection to complete the
interaction without another button press. Disabling native error dialogs
prevents automatic retries and lets the watchapp provide one consistent
three-second error state.

The callback-provided transcription must be copied into watch-owned memory
before the callback returns because the SDK frees the callback string
afterward.

### 7.3 Dictation result handling

| SDK status | Required behavior |
| --- | --- |
| `DictationSessionStatusSuccess` with nonempty text | Begin transcript transfer |
| `DictationSessionStatusFailureTranscriptionRejected` | Show cancellation |
| `DictationSessionStatusFailureTranscriptionRejectedWithError` | Show dictation error |
| `DictationSessionStatusFailureNoSpeechDetected` | Show no-speech error |
| `DictationSessionStatusFailureConnectivityError` | Show phone/voice connection error |
| `DictationSessionStatusFailureDisabled` | Show dictation error |
| `DictationSessionStatusFailureSystemAborted` | Show dictation error |
| `DictationSessionStatusFailureInternalError` | Show dictation error |
| `DictationSessionStatusFailureRecognizerError` | Show dictation error |

A success status with an empty transcription is treated as no speech and must
not be sent.

Any non-success result sends a best-effort `SESSION_END`, displays its
three-second status, and closes. There is no automatic second dictation attempt.

### 7.4 State machine

The watchapp has the following logical states:

| State | Description | Valid next states |
| --- | --- | --- |
| `INITIALIZING` | Root window, AppMessage, request, and dictation are being created | `DICTATING`, `ERROR` |
| `DICTATING` | Native dictation owns the foreground interaction | `TRANSFERRING`, `ERROR` |
| `TRANSFERRING` | Transcript chunks are sent sequentially | `AWAITING_SEND_START`, `ERROR` |
| `AWAITING_SEND_START` | Final chunk reached PebbleKit JS; transcript chunk watchdog is active | `AWAITING_SERVER_ACK`, `ERROR` |
| `AWAITING_SERVER_ACK` | PebbleKit JS called `WebSocket.send()`; acknowledgement watchdog is active | `SUCCESS`, `ERROR` |
| `SUCCESS` | Matching remote acknowledgement reached the watch | closed |
| `ERROR` | A status is visible for three seconds | closed |

Unexpected events for the current state are ignored if they belong to another
request. An event with the current request identifier but an invalid type or
sequence is a protocol failure.

### 7.5 Exit and cleanup

On `SUCCESS`, the watchapp cancels timers, releases owned transcript and
dictation resources, removes its window, and exits immediately.

On `ERROR`, cleanup may begin immediately, but the root window remains visible
for the three-second error duration before the app exits.

Closing the app must not leave an active dictation session or watch timer.
`dictation_session_stop()` is used only when cancelling an in-progress session;
it is not used to finalize successful dictation.

## 8. AppMessage protocol

### 8.1 General rules

- Protocol version: `1`.
- Only one request may be active.
- The request identifier is an opaque, nonempty ASCII string and is identical
  in AppMessage and WebSocket messages.
- A recommended representation is eight lowercase hexadecimal characters
  derived from a nonzero 32-bit watch-generated value.
- Metadata integer values are nonnegative 32-bit integers.
- Message types and dictionary keys are fixed by this section.
- Unknown keys are ignored. Unknown versions or message types for the active
  request are protocol errors.

### 8.2 Dictionary keys

| Numeric key | Name | Type | Meaning |
| ---: | --- | --- | --- |
| `0` | `PROTOCOL_VERSION` | integer | Must be `1` |
| `1` | `MESSAGE_TYPE` | integer | Value from section 8.3 |
| `2` | `REQUEST_ID` | C string | Active request identifier |
| `3` | `CHUNK_INDEX` | integer | Zero-based chunk position |
| `4` | `CHUNK_COUNT` | integer | Total number of chunks |
| `5` | `TOTAL_BYTES` | integer | UTF-8 transcript bytes, excluding C terminator |
| `6` | `CHUNK_TEXT` | C string | One valid UTF-8 transcript segment |
| `7` | `STATUS_CODE` | integer | Failure or terminal reason code |

Required keys depend on message type. A required key with the wrong type,
missing value, or out-of-range value is a protocol error.

### 8.3 Message types

| Value | Name | Direction | Required fields |
| ---: | --- | --- | --- |
| `1` | `SESSION_BEGIN` | Watch to JS | version, type, request ID |
| `2` | `TRANSCRIPT_CHUNK` | Watch to JS | version, type, request ID, chunk index, chunk count, total bytes, chunk text |
| `3` | `SESSION_END` | Watch to JS | version, type, request ID, status code |
| `10` | `SEND_STARTED` | JS to watch | version, type, request ID |
| `11` | `REMOTE_ACK` | JS to watch | version, type, request ID |
| `12` | `FAILURE` | JS to watch | version, type, request ID, status code |

`SESSION_END` is a best-effort cleanup notification for a request that ends
before remote acknowledgement. It does not require an application-level reply.

### 8.4 Chunking

The watchapp must deliver the full transcription without truncation:

1. Encode the native transcription as its original UTF-8 byte sequence.
2. Determine a conservative chunk payload size that allows all required
   dictionary fields and the C-string terminator to fit in the open AppMessage
   outbox.
3. Split only at UTF-8 code-point boundaries. Every `CHUNK_TEXT` value must
   independently be valid UTF-8.
4. Calculate one stable, positive `CHUNK_COUNT` before sending the first chunk.
5. Put the same `CHUNK_COUNT` and `TOTAL_BYTES` on every chunk.
6. Send chunks in strictly increasing `CHUNK_INDEX` order from `0` through
   `CHUNK_COUNT - 1`.
7. Wait for the AppMessage outbox-sent callback before writing and sending the
   next chunk.

`SESSION_BEGIN` participates in the same sequential outbox discipline. Its
outbox-sent callback must occur before the first `TRANSCRIPT_CHUNK` is sent. If
dictation finishes sooner, the watchapp retains the completed transcription
while the begin message is in flight.

If the complete text cannot be allocated, represented by the metadata fields,
or divided into valid chunks, the watchapp fails before sending the first
chunk. If any chunk fails after transfer begins, the operation fails and
PebbleKit JS must discard all accumulated chunks. A partial transcript is never
sent over the WebSocket.

### 8.5 Reassembly

PebbleKit JS accepts transcript chunks only after a matching `SESSION_BEGIN`.
It must validate that:

- the first index is `0`;
- each subsequent index is exactly the previous index plus one;
- chunk count and total byte count are positive and identical on all chunks;
- no index is greater than or equal to chunk count;
- every chunk is valid UTF-8;
- the final index is `CHUNK_COUNT - 1`; and
- the UTF-8 byte length of the concatenated text equals `TOTAL_BYTES`.

Duplicate, missing, reordered, inconsistent, or malformed chunks terminate the
request as a protocol failure. PebbleKit JS must not call `WebSocket.send()`
until all validations succeed.

## 9. WebSocket protocol

### 9.1 Connection lifecycle

On `SESSION_BEGIN`, PebbleKit JS must discard any stale request state, close any
stale WebSocket owned by that state, and create one WebSocket using the
build-time URL.

Connection setup proceeds in parallel with dictation. PebbleKit JS must not
reconnect if connection setup fails or the socket closes. It reports a
`FAILURE` immediately when possible.

When the final transcript chunk is validated:

- If the socket is `OPEN`, PebbleKit JS sends immediately.
- If the socket is still connecting, PebbleKit JS may wait only for the
  remaining handoff deadline described in section 10.1.
- If the socket is closing or closed, the request fails immediately.

PebbleKit JS sends exactly one text frame for the request.

### 9.2 Request

The text frame is UTF-8 JSON with this exact required shape:

```json
{
  "version": 1,
  "type": "dictation",
  "requestId": "1a2b3c4d",
  "transcript": "Example transcription"
}
```

Rules:

- `version` is the JSON number `1`.
- `type` is the JSON string `"dictation"`.
- `requestId` exactly equals the AppMessage request identifier.
- `transcript` exactly equals the reconstructed transcription.
- No additional metadata is required in version 1.

### 9.3 Acknowledgement

Successful transcript reception is represented by:

```json
{
  "version": 1,
  "type": "ack",
  "requestId": "1a2b3c4d"
}
```

The acknowledgement must be a WebSocket text frame containing valid JSON. It
counts only while PebbleKit JS is awaiting a response and only when `version`,
`type`, and `requestId` exactly match the active request. Additional object
properties are ignored.

Malformed JSON, binary frames, unknown message types, acknowledgements for
another request, and otherwise unrelated frames do not count as success. They
are ignored until a matching response arrives or the deadline expires.

### 9.4 Server error

The server may reject a request with:

```json
{
  "version": 1,
  "type": "error",
  "requestId": "1a2b3c4d",
  "code": "invalid_request"
}
```

A valid error with the matching version and request identifier fails the
request immediately. `code` must be a nonempty string for logging but is not
shown verbatim on the watch. Additional properties are ignored. A malformed or
nonmatching error is treated as an unrelated frame and does not stop the
acknowledgement timer.

### 9.5 PebbleKit JS completion

Immediately after successfully invoking `WebSocket.send()` with the request,
PebbleKit JS:

1. starts the exact server acknowledgement timer;
2. sends `SEND_STARTED` to the watch; and
3. waits for a matching acknowledgement, matching server error, socket error,
   socket close, or timeout.

Phone-to-watch status AppMessages must be serialized. If a remote response
arrives before `SEND_STARTED` has reached the watch, PebbleKit JS queues
`REMOTE_ACK` or `FAILURE` until the `SEND_STARTED` AppMessage succeeds.

After a matching acknowledgement, PebbleKit JS sends `REMOTE_ACK`. It closes
the WebSocket and clears the transcript. On any terminal failure it sends 
`FAILURE` when possible, closes the WebSocket, and clears all request and 
transcript state.

## 10. Deadlines

All deadlines use monotonic elapsed time where the runtime provides it.

### 10.1 PebbleKit JS transcript chunk transmission deadline

- Duration: 2,000 ms.
- Watch start point: the outbox-sent callback for the final
  `TRANSCRIPT_CHUNK`.
- PebbleKit JS start point: receipt of that final chunk.
- Success condition: the watch receives `SEND_STARTED` for the active request.
- Early failure condition: the watch receives `FAILURE`.

Both sides enforce their local view of this deadline. PebbleKit JS must either
invoke `WebSocket.send()` and report `SEND_STARTED`, or report `FAILURE`, within
this timer's interval. If the watch's interval expires first, the watch shows
a delivery failure, sends best-effort `SESSION_END`, and exits after the error
display.

### 10.2 Remote acknowledgement deadline

- Duration: 5,000 ms.
- Authoritative start point: immediately after `WebSocket.send()` successfully
  accepts the request without throwing.
- Authoritative owner: PebbleKit JS.
- Success condition: PebbleKit JS processes a matching `ack` frame before its
  timer callback runs.
- Failure conditions: timer expiry, a matching server error, WebSocket error,
  or WebSocket close before acknowledgement.

After receiving `SEND_STARTED`, the watch starts a websocket request timeout
guard to avoid hanging if PebbleKit JS becomes unresponsive. This guard is not
the server acknowledgement deadline: the PebbleKit JS timer remains
authoritative for whether the server responded within the deadline.

On PebbleKit JS timeout, it sends `FAILURE` with an acknowledgement-timeout
status. On remote acknowledgement expiry without either `REMOTE_ACK` or `FAILURE`, the
watch shows `WS request timed out`, sends best-effort `SESSION_END`, and exits after
the error display.

### 10.3 Error display

- Duration: 3,000 ms.
- Start point: the error or cancellation state is visible on the watch.
- End condition: watchapp cleanup and exit.

No deadline triggers a retry.

## 11. Failure handling

### 11.1 Watch-local failure

Examples include dictation setup failure, dictation callback failure, memory
allocation failure, malformed JS status, AppMessage inbox drop, or outbox
failure.

The watchapp:

1. cancels active transfer timers;
2. stops dictation if it is still in progress;
3. sends best-effort `SESSION_END` if AppMessage is usable;
4. releases transcript state;
5. displays the mapped status for three seconds; and
6. closes.

The best-effort cleanup message must not delay the error display or extend the
three-second lifetime.

### 11.2 PebbleKit JS or WebSocket failure

Examples include connection failure, invalid chunk sequence, JSON serialization
failure, `WebSocket.send()` throwing, matching server error, socket error,
socket close, or acknowledgement timeout.

PebbleKit JS:

1. sends `FAILURE` with a stable status code when the bridge is available;
2. closes the WebSocket;
3. discards every transcript chunk and serialized request; and
4. performs no retry.

### 11.3 Status codes

Implementations must share stable numeric status constants. At minimum:

| Code | Name | Origin |
| ---: | --- | --- |
| `0` | `STATUS_NORMAL` | Watch cleanup after non-error completion |
| `1` | `STATUS_CANCELLED` | Watch |
| `2` | `STATUS_DICTATION` | Watch |
| `3` | `STATUS_NO_SPEECH` | Watch |
| `4` | `STATUS_CONNECTIVITY` | Watch or JS |
| `5` | `STATUS_PROTOCOL` | Watch or JS |
| `6` | `STATUS_TRANSFER` | Watch or JS |
| `7` | `STATUS_SERVER_ERROR` | JS |
| `8` | `STATUS_ACK_TIMEOUT` | JS or watch |

The watch uses these codes for mapping and diagnostics; it must not display
server-controlled strings.

## 12. Data handling and privacy

- The watchapp holds the transcription only in volatile memory for the current
  request.
- PebbleKit JS holds chunks and serialized JSON only until terminal cleanup.
- Neither component logs transcript content in production.
- Neither component writes the transcript to persistent storage.
- No partial transcript is sent to the WebSocket server.
- Native Pebble dictation may use phone and cloud voice services; this
  specification's local WebSocket deployment does not make transcription
  itself an offline operation.

## 13. Acceptance criteria

### 13.1 Launch and dictation

1. A Quick Launch long press on an assigned button launches the app and starts
   native dictation without another watch input.
2. Launcher-based launch starts the same dictation flow.
3. Releasing the Quick Launch button does not cancel dictation.
4. Dictation confirmation and native retry/error dialogs are disabled.
5. Natural speech completion proceeds to `Sending...`.
6. User cancellation, no speech, disabled dictation, and connectivity failure
   each show an appropriate status for three seconds and close without sending a
   WebSocket request.

### 13.2 Transfer correctness

7. A short ASCII transcript produces one chunk and an exactly matching JSON
   transcript.
8. A transcript larger than one AppMessage is divided into multiple sequential
   chunks and reconstructed exactly.
9. Multibyte UTF-8 characters at candidate chunk boundaries are neither split
   nor changed.
10. PebbleKit JS never sends when a chunk is missing, duplicated, reordered, or
    inconsistent.
11. Allocation or transfer failure never results in a partial WebSocket
    transcript.
12. Only one transcript request and one WebSocket text frame are sent per app
    run.

### 13.3 Acknowledgement and timing

13.  A matching acknowledgement received within five seconds of
    `WebSocket.send()` causes `REMOTE_ACK` and immediate watchapp exit. If this timeout
    expires, `WS server timed out` is produced for three seconds followed by exit. A race
    condition may produce either result.
14.  AppMessage outbox success and `WebSocket.send()` success alone do not close
    the watchapp as a success.
15.  A malformed, binary, wrong-version, wrong-type, or wrong-request
    acknowledgement is ignored and cannot produce success.
16.  A matching server error produces a three-second delivery error and exit.
17.  Lack of `SEND_STARTED` within two seconds of final-chunk delivery produces a
    three-second delivery error and exit.
18.  No timeout or failure path retries dictation, AppMessage transfer,
    WebSocket connection, or WebSocket send.

### 13.4 Cleanup

19. Successful completion leaves no active watch timer or dictation session.
20. Cancellation and every error path release transcript and chunk state.
21. PebbleKit JS closes the request WebSocket and clears volatile request data
    after acknowledgement, failure, or `SESSION_END`.
22. Transcript content is not persisted or logged by watchapp or PebbleKit JS.

## 14. SDK references

- [Dictation API](https://developer.repebble.com/docs/c/Foundation/Dictation/)
- [AppMessage API](https://developer.repebble.com/docs/c/Foundation/AppMessage/)
- [Launch Reason API](https://developer.repebble.com/docs/c/Foundation/Launch_Reason/)
- [Window and click subscriptions](https://developer.repebble.com/docs/c/User_Interface/Window/)
- Local `emery` SDK declaration:
  `/home/vagrant/.local/share/pebble-sdk/SDKs/4.17/sdk-core/pebble/emery/include/pebble.h`
