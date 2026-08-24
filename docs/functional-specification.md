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
6. The remote server returns a correlated application result.
7. PebbleKit JS transfers the result to the watchapp.
8. The watchapp displays the result for three seconds and closes unless the
  user interacts with it.

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
- A correlated, application-level server result containing success/failure and
  a UTF-8 response of at most 1,024 bytes.
- Lossless transfer of the result back to the watch over one or more
  AppMessages.
- Strict, short timeouts and visible failures.
- Deterministic cleanup after success, cancellation, or failure.

### 4.2 Out of scope

- A configuration page or runtime endpoint selection.
- Retry, offline queueing, persistence, or background delivery.
- Custom audio capture or a replacement dictation UI.
- Delivery of partial or deliberately truncated transcriptions.
- Truncation of server responses; an oversized response is rejected instead.
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
- waits for PebbleKit JS and remote-server events;
- validates and reassembles server-result chunks;
- owns the user-visible state and terminal result/error display; and
- exits only at a terminal state.

The watchapp represents the lifecycle with one aggregate bridge context. Its
watch/phone and phone/server bridge records each own their state, last typed
error, timeout duration values, and active timer handles. Top-level application
state covers dictation and UI lifecycle and does not duplicate bridge progress.

### 5.2 PebbleKit JS

PebbleKit JS:

- maintains at most one active request;
- opens the configured WebSocket while dictation is in progress;
- validates and reassembles transcript chunks;
- sends exactly one JSON request after receiving the complete transcription;
- accepts only a matching final server result;
- reports send start, ordered result chunks, or bridge failure to the watchapp;
- performs no retry and retains no completed transcription; and
- closes the request's WebSocket when the request reaches a terminal state.

PebbleKit JS implements a watch bridge and WebSocket bridge under one request
coordinator. The watch bridge owns transcript assembly and serialized
AppMessage delivery. The WebSocket bridge owns socket state and its handoff and
server-result timers. The coordinator alone routes events between them and
maps typed bridge errors to numeric status codes.

### 5.3 Remote WebSocket server

The server:

- accepts the request JSON specified in section 9;
- processes each request according to its own policy;
- returns exactly one correlated final result after processing; and
- responds quickly enough to satisfy the server-result deadline.

The included Python server represents each request with a typed bridge session.
Sync and async handlers return a `ServerResult`. The async-iterator API yields
a one-shot exchange which application code resolves with `respond()`.

Server-side deduplication is recommended but is outside the watchapp's
responsibility.

## 6. User experience

### 6.1 Normal flow

1. The app shows a minimal `Starting...` state while initializing.
2. Native dictation appears immediately and handles listening and
   transcription.
3. After a successful transcription, the watchapp shows `Sending...`.
4. While the server processes the request, the watchapp continues to show
  `Sending...` for up to the server-result deadline.
5. The watch displays a `Success` or `Failure` heading and the server's response
  for three seconds, then closes.

When a complete result becomes visible, `success: true` produces one short
vibration and `success: false` produces three short vibrations. Bridge and
protocol errors do not use these result vibrations.

When the result becomes visible, the watchapp triggers the backlight for the
system's standard auto-off interval while respecting the user's backlight
setting.

The result body is vertically scrollable with the UP and DOWN buttons when it
does not fit on the Emery display. Any result-screen button press cancels the
three-second close timer. UP and DOWN retain their normal scrolling behavior
while cancelling the timer; after cancellation, the user exits manually with
BACK. An empty response is valid and displays an empty body.

The app must not claim success based only on AppMessage delivery or a successful
call to `WebSocket.send()`.

### 6.2 Failure and cancellation flow

The app displays a concise status for three seconds and then closes.
When the status becomes visible, the watchapp triggers the backlight using the
same standard system behavior as the result screen.

The implementation must distinguish at least these user-facing categories:

| Category | Example display | Causes |
| --- | --- | --- |
| Cancelled | `Dictation cancelled` | User exits or rejects native dictation |
| No speech | `No speech heard` | Native dictation reports no speech |
| Dictation error | `Dictation error` | Dictation is disabled, cannot be created, or cannot start |
| Phone/voice connection | `Phone unavailable` | Native voice connectivity or initial AppMessage failure |
| Delivery | `Delivery failed` | Chunk, WebSocket, protocol, server error, or bridge failure |
| Server timeout | `WS server timed out` | Final server-result deadline expires |
| Result transfer timeout | `Result timed out` | Phone-to-watch result chunks stall |

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

The watchapp has the following top-level application states:

| State | Description | Valid next states |
| --- | --- | --- |
| `INITIALIZING` | Root window, AppMessage, request, and dictation are being created | `DICTATING`, `ERROR` |
| `DICTATING` | Native dictation owns the foreground interaction | `BRIDGING`, `ERROR` |
| `BRIDGING` | One or both logical bridges are transferring or waiting | `DISPLAYING_RESULT`, `ERROR` |
| `DISPLAYING_RESULT` | A complete success or failure result is visible; interaction cancels automatic close | closed |
| `ERROR` | A status is visible for three seconds | closed |

The watch/phone bridge owns these states:

| State | Description |
| --- | --- |
| `IDLE` | AppMessage has not started the request |
| `SESSION_BEGIN_PENDING` | `SESSION_BEGIN` is in the AppMessage outbox |
| `READY` | The session begin message was delivered |
| `TRANSFERRING` | Transcript chunks are being sent sequentially |
| `AWAITING_SEND_START` | The final transcript chunk was delivered; the 2-second watchdog is active |
| `AWAITING_RESULT` | `SEND_STARTED` arrived; no result chunk has arrived yet |
| `RECEIVING_RESULT` | Ordered result chunks are being reassembled |
| `COMPLETE` | The complete result is owned by the watch |
| `FAILED` | A watch/phone bridge error has terminated the request |

The phone/server bridge owns these states:

| State | Description |
| --- | --- |
| `IDLE` | No remote request is being prepared |
| `PREPARING` | PebbleKit JS is opening the socket or awaiting the transcript |
| `AWAITING_RESULT` | `WebSocket.send()` succeeded and the final result is pending |
| `COMPLETE` | The first validated result chunk proves the remote result reached PebbleKit JS |
| `FAILED` | A phone/server bridge error has terminated the request |

Unexpected events for the current state are ignored if they belong to another
request. An event with the current request identifier but an invalid type or
sequence is a protocol failure.

### 7.5 Exit and cleanup

On a complete application result, the watchapp cancels bridge timers and
retains the response while its layers use it. It closes after three seconds if
there is no interaction. Any button press cancels automatic close; UP and DOWN
continue to scroll, and BACK manually closes the app. Exit releases response,
transcript, layer, timer, and dictation resources.

On `ERROR`, cleanup may begin immediately, but the root window remains visible
for the three-second error duration before the app exits.

Closing the app must not leave an active dictation session or watch timer.
`dictation_session_stop()` is used only when cancelling an in-progress session;
it is not used to finalize successful dictation.

## 8. AppMessage protocol

### 8.1 General rules

- Protocol version: `2`.
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
| `0` | `PROTOCOL_VERSION` | integer | Must be `2` |
| `1` | `MESSAGE_TYPE` | integer | Value from section 8.3 |
| `2` | `REQUEST_ID` | C string | Active request identifier |
| `3` | `CHUNK_INDEX` | integer | Zero-based chunk position |
| `4` | `CHUNK_COUNT` | integer | Total number of chunks |
| `5` | `TOTAL_BYTES` | integer | UTF-8 transcript bytes, excluding C terminator |
| `6` | `CHUNK_TEXT` | C string | One valid UTF-8 transcript segment |
| `7` | `STATUS_CODE` | integer | Failure or terminal reason code |
| `8` | `SERVER_SUCCESS` | integer | Server result boolean encoded as exactly `0` or `1` |

Required keys depend on message type. A required key with the wrong type,
missing value, or out-of-range value is a protocol error.

### 8.3 Message types

| Value | Name | Direction | Required fields |
| ---: | --- | --- | --- |
| `1` | `SESSION_BEGIN` | Watch to JS | version, type, request ID |
| `2` | `TRANSCRIPT_CHUNK` | Watch to JS | version, type, request ID, chunk index, chunk count, total bytes, chunk text |
| `3` | `SESSION_END` | Watch to JS | version, type, request ID, status code |
| `10` | `SEND_STARTED` | JS to watch | version, type, request ID |
| `12` | `FAILURE` | JS to watch | version, type, request ID, status code |
| `13` | `SERVER_RESULT_CHUNK` | JS to watch | version, type, request ID, chunk index, chunk count, total bytes, chunk text, server success |

`SESSION_END` is a best-effort cleanup notification for a request that ends
before a complete server result. It does not require an application-level reply.

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

### 8.6 Server-result chunking and reassembly

PebbleKit JS transfers the final `response` back to the watch as
`SERVER_RESULT_CHUNK` messages:

1. The response is valid UTF-8 and contains at most 1,024 UTF-8 bytes.
2. Each chunk contains at most 256 UTF-8 bytes and ends at a Unicode code-point
  boundary.
3. `CHUNK_INDEX` starts at `0` and increases by exactly one.
4. `CHUNK_COUNT`, `TOTAL_BYTES`, and `SERVER_SUCCESS` are identical on every
  chunk.
5. An empty response is represented by one chunk with empty `CHUNK_TEXT` and
  `TOTAL_BYTES` equal to `0`.
6. PebbleKit JS waits for each AppMessage completion callback before sending
  the next result chunk.

The watch allocates only after validating the first chunk's metadata and the
1,024-byte limit. It rejects missing, duplicated, reordered, inconsistent,
oversized, or malformed UTF-8 chunks. It does not display a partial response.

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
  "version": 2,
  "type": "dictation",
  "requestId": "1a2b3c4d",
  "transcript": "Example transcription"
}
```

Rules:

- `version` is the JSON number `2`.
- `type` is the JSON string `"dictation"`.
- `requestId` exactly equals the AppMessage request identifier.
- `transcript` exactly equals the reconstructed transcription.
- No additional metadata is required in version 2.

### 9.3 Final result

Successful application processing is represented by:

```json
{
  "version": 2,
  "type": "result",
  "requestId": "1a2b3c4d",
  "success": true,
  "response": "Request accepted"
}
```

An application-level failure uses the same envelope with `success` equal to
`false` and a response explaining the result. Both values are completed server
results and are displayed verbatim by the watch; `success: false` is not a
bridge failure.

The result must be a WebSocket text frame containing valid JSON. It counts only
while PebbleKit JS awaits a result and only when `version`, `type`, and
`requestId` match the active request. `success` must be a JSON boolean.
`response` must be a JSON string whose UTF-8 representation is no larger than
1,024 bytes. Additional properties are ignored.

Malformed or binary frames and frames for another request are unrelated and
are ignored until a matching frame or timeout. A frame correlated to the
active request but containing a wrong version, unknown type, invalid success,
invalid response, or oversized response is an immediate protocol failure.

### 9.4 Server error

The server may reject a request with:

```json
{
  "version": 2,
  "type": "error",
  "requestId": "1a2b3c4d",
  "code": "invalid_request"
}
```

A valid error with the matching version and request identifier fails the
bridge immediately. `code` must be a nonempty string for logging but is not
shown verbatim on the watch. Additional properties are ignored. A malformed
correlated error is a protocol failure; a nonmatching error is unrelated.

### 9.5 PebbleKit JS completion

Immediately after successfully invoking `WebSocket.send()` with the request,
PebbleKit JS:

1. starts the exact server-result timer;
2. sends `SEND_STARTED` to the watch; and
3. waits for a matching result, matching server error, socket error,
   socket close, or timeout.

Phone-to-watch AppMessages must be serialized. If a remote result arrives
synchronously during `WebSocket.send()` or before `SEND_STARTED` reaches the
watch, PebbleKit JS holds it until `SEND_STARTED` is first in the AppMessage
queue.

After a matching result, PebbleKit JS closes the WebSocket and sends all
`SERVER_RESULT_CHUNK` messages sequentially. It keeps the request coordinator
alive until the final AppMessage completion callback. On a bridge failure it
sends `FAILURE` when possible, closes the WebSocket, and clears all request and
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

### 10.2 Remote server-result deadline

- Duration: 60,000 ms.
- Authoritative start point: immediately after `WebSocket.send()` successfully
  accepts the request without throwing.
- Authoritative owner: the PebbleKit JS WebSocket bridge.
- Success condition: PebbleKit JS processes a matching `result` frame before
  its timer callback runs.
- Failure conditions: timer expiry, a matching server error, WebSocket error,
  WebSocket close, or a malformed correlated frame before the result.

After receiving `SEND_STARTED`, the watch phone/server bridge starts a 62,000
ms liveness guard. This guard is deliberately longer and is not the server
deadline: the PebbleKit JS timer remains authoritative and has time to deliver
`FAILURE` after its 60-second timer fires.

On PebbleKit JS timeout, it sends `FAILURE` with
`STATUS_SERVER_RESULT_TIMEOUT`. If neither a result chunk nor `FAILURE` reaches
the watch before its 62-second guard, the watch shows `WS request timed out`,
sends best-effort `SESSION_END`, and exits after the error display.

### 10.3 Result-chunk inactivity deadline

- Duration: 3,000 ms.
- Owner: the watch/phone bridge.
- Start point: the first valid non-final `SERVER_RESULT_CHUNK`.
- Reset point: every subsequent valid non-final result chunk.
- Success condition: the final valid result chunk completes reassembly.
- Failure condition: timer expiry while the result is incomplete.

The first result chunk cancels the 62-second phone/server guard. A stalled
reverse transfer shows `Result timed out`, sends best-effort `SESSION_END`, and
exits after the error display.

### 10.4 Terminal display

- Duration: 3,000 ms.
- Start point: the complete result, error, or cancellation state is visible on
  the watch.
- Result interaction: any button press cancels the result timer. UP and DOWN
  also perform normal ScrollLayer scrolling. After cancellation, BACK is the
  only exit path.
- Error/cancellation interaction: unchanged; their timer remains active.
- End condition: timer expiry when still active, or manual BACK exit from an
  interacted-with result screen.

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
failure, `WebSocket.send()` throwing, malformed correlated result, matching
server error, socket error, socket close, or server-result timeout.

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
| `8` | `STATUS_SERVER_RESULT_TIMEOUT` | JS |
| `9` | `STATUS_TRANSCRIPT_DELIVERY_TIMEOUT` | Watch |
| `10` | `STATUS_WS_REQUEST_TIMEOUT` | Watch |
| `11` | `STATUS_RESULT_TRANSFER_TIMEOUT` | Watch |

The watch uses these codes for bridge error mapping and diagnostics. It shows a
server-controlled string only after validating and completely reassembling a
final application result. Bridge failures always use local status text.

## 12. Data handling and privacy

- The watchapp holds the transcription only in volatile memory for the current
  request.
- The watchapp holds the server response only until its terminal display is
  destroyed.
- PebbleKit JS holds transcript/result chunks and serialized JSON only until
  terminal cleanup.
- Neither component logs transcript or response content in production.
- Neither component writes transcript or response content to persistent
  storage.
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

### 13.3 Result and timing

13. A matching result received within 60 seconds of `WebSocket.send()` is
  transferred to the watch and displayed for three seconds unless the user
  presses a button.
14. Both `success: true` and `success: false` are completed application results
  with `Success` or `Failure` headings and one or three short vibrations,
  respectively.
15. AppMessage outbox success and `WebSocket.send()` success alone do not close
  or claim success.
16. An immediate synchronous result is still queued behind `SEND_STARTED`.
17. A malformed or binary frame and a wrong-request frame are ignored. A
  wrong-version, wrong-type, malformed, or oversized correlated frame fails
  the protocol and cannot produce a result screen.
18. A matching server error produces a three-second local delivery error.
19. Lack of `SEND_STARTED` within two seconds of final transcript-chunk delivery
  produces a three-second delivery error.
20. A 60-second server-result expiry reaches the watch before its 62-second
  liveness guard under normal bridge operation.
21. A result transfer that stalls for three seconds between valid chunks shows
  a result-transfer timeout.
22. Empty, multibyte, multi-chunk, and exactly 1,024-byte responses are
  reconstructed exactly; a 1,025-byte response is rejected rather than
  truncated.
23. Pressing UP or DOWN on a result screen cancels automatic close and still
  scrolls normally; pressing BACK then exits manually.
24. No timeout or failure path retries dictation, AppMessage transfer,
  WebSocket connection, or WebSocket send.

### 13.4 Cleanup

25. Completed result display leaves no active watch timer or dictation session.
26. Cancellation and every error path release transcript, response, and chunk
  state.
27. PebbleKit JS closes the request WebSocket and clears volatile request data
  after final result delivery, failure, or `SESSION_END`.
28. Transcript and response content are not persisted or logged by watchapp or
  PebbleKit JS.

## 14. SDK references

- [Dictation API](https://developer.repebble.com/docs/c/Foundation/Dictation/)
- [AppMessage API](https://developer.repebble.com/docs/c/Foundation/AppMessage/)
- [Launch Reason API](https://developer.repebble.com/docs/c/Foundation/Launch_Reason/)
- [Window and click subscriptions](https://developer.repebble.com/docs/c/User_Interface/Window/)
- Local `emery` SDK declaration:
  `/home/vagrant/.local/share/pebble-sdk/SDKs/4.17/sdk-core/pebble/emery/include/pebble.h`
