'use strict';

var protocol = require('./protocol');

// Change this build-time value to the WebSocket server reachable by the phone.
var WEBSOCKET_URL = 'ws://192.168.50.199:8080/';
var WS_REQUEST_TIMEOUT_MS = 3000;
var ACK_TIMEOUT_MS = 2000;

var activeSession = null;
var statusQueue = [];
var statusBusy = false;

function isActive(session) {
  return activeSession === session;
}

function clearTimer(timer) {
  if (timer !== null) {
    clearTimeout(timer);
  }
}

function removeQueuedStatuses(session) {
  var retained = [];
  for (var i = 0; i < statusQueue.length; i++) {
    if (statusQueue[i].session !== session) {
      retained.push(statusQueue[i]);
    }
  }
  statusQueue = retained;
}

function closeSocket(session) {
  if (!session.socket) {
    return;
  }

  session.socket.onopen = null;
  session.socket.onerror = null;
  session.socket.onclose = null;
  session.socket.onmessage = null;

  if (session.socket.readyState === 0 || session.socket.readyState === 1) {
    try {
      session.socket.close();
    } catch (error) {
      console.log('WebSocket close failed');
    }
  }
  session.socket = null;
}

function cleanupSession(session) {
  clearTimer(session.handoffTimer);
  clearTimer(session.ackTimer);
  session.handoffTimer = null;
  session.ackTimer = null;
  closeSocket(session);
  session.assembler.clear();
  session.transcript = null;
  removeQueuedStatuses(session);

  if (isActive(session)) {
    activeSession = null;
  }
}

function finishStatus(entry, succeeded) {
  statusBusy = false;
  if (entry.done) {
    entry.done(succeeded);
  }
  pumpStatusQueue();
}

function pumpStatusQueue() {
  if (statusBusy) {
    return;
  }

  while (statusQueue.length > 0 &&
         !isActive(statusQueue[0].session)) {
    statusQueue.shift();
  }
  if (statusQueue.length === 0) {
    return;
  }

  var entry = statusQueue.shift();
  statusBusy = true;
  try {
    Pebble.sendAppMessage(
        entry.payload,
        function() {
          finishStatus(entry, true);
        },
        function() {
          finishStatus(entry, false);
        });
  } catch (error) {
    finishStatus(entry, false);
  }
}

function enqueueStatus(session, type, statusCode, done) {
  if (!isActive(session)) {
    return;
  }

  var payload = {};
  payload[protocol.KEYS.PROTOCOL_VERSION] = protocol.VERSION;
  payload[protocol.KEYS.MESSAGE_TYPE] = type;
  payload[protocol.KEYS.REQUEST_ID] = session.requestId;
  if (typeof statusCode === 'number') {
    payload[protocol.KEYS.STATUS_CODE] = statusCode;
  }

  statusQueue.push({
    session: session,
    payload: payload,
    done: done
  });
  pumpStatusQueue();
}

function terminateWithFailure(session, statusCode) {
  if (!isActive(session) || session.terminal) {
    return;
  }

  session.terminal = true;
  clearTimer(session.handoffTimer);
  clearTimer(session.ackTimer);
  session.handoffTimer = null;
  session.ackTimer = null;
  closeSocket(session);

  enqueueStatus(
      session,
      protocol.MESSAGE_TYPES.FAILURE,
      statusCode,
      function() {
        cleanupSession(session);
      });
}

function terminateWithAck(session) {
  if (!isActive(session) || session.terminal) {
    return;
  }

  session.terminal = true;
  clearTimer(session.handoffTimer);
  clearTimer(session.ackTimer);
  session.handoffTimer = null;
  session.ackTimer = null;
  closeSocket(session);

  enqueueStatus(
      session,
      protocol.MESSAGE_TYPES.REMOTE_ACK,
      null,
      function() {
        cleanupSession(session);
      });
}

function handleServerMessage(session, data) {
  if (!isActive(session) || session.terminal || !session.sendStarted) {
    return;
  }

  var result = protocol.parseServerFrame(data, session.requestId);
  if (result.kind === 'ack') {
    terminateWithAck(session);
  } else if (result.kind === 'error') {
    terminateWithFailure(session, protocol.STATUS_CODES.SERVER_ERROR);
  }
}

function trySendTranscript(session) {
  if (!isActive(session) ||
      session.terminal ||
      session.sendStarted ||
      session.transcript === null ||
      !session.socket ||
      session.socket.readyState !== 1) {
    return;
  }

  var request;
  var serialized;
  try {
    request = protocol.buildRequest(session.requestId, session.transcript);
    serialized = JSON.stringify(request);
    session.socket.send(serialized);
  } catch (error) {
    terminateWithFailure(session, protocol.STATUS_CODES.TRANSFER);
    return;
  }

  session.sendStarted = true;
  clearTimer(session.handoffTimer);
  session.handoffTimer = null;
  session.ackTimer = setTimeout(function() {
    terminateWithFailure(session, protocol.STATUS_CODES.ACK_TIMEOUT);
  }, ACK_TIMEOUT_MS);

  enqueueStatus(
      session,
      protocol.MESSAGE_TYPES.SEND_STARTED,
      null,
      function(succeeded) {
        if (!succeeded && isActive(session)) {
          cleanupSession(session);
        }
      });
}

function openSocket(session) {
  try {
    session.socket = new WebSocket(WEBSOCKET_URL);
  } catch (error) {
    terminateWithFailure(session, protocol.STATUS_CODES.TRANSFER);
    return;
  }

  session.socket.onopen = function() {
    trySendTranscript(session);
  };

  session.socket.onerror = function() {
    terminateWithFailure(session, protocol.STATUS_CODES.TRANSFER);
  };

  session.socket.onclose = function() {
    terminateWithFailure(session, protocol.STATUS_CODES.TRANSFER);
  };

  session.socket.onmessage = function(event) {
    handleServerMessage(session, event.data);
  };
}

function startSession(message) {
  if (activeSession) {
    cleanupSession(activeSession);
  }

  var session = {
    requestId: message.requestId,
    assembler: new protocol.TranscriptAssembler(message.requestId),
    socket: null,
    transcript: null,
    handoffTimer: null,
    ackTimer: null,
    sendStarted: false,
    terminal: false
  };

  activeSession = session;
  openSocket(session);
}

function handleChunk(session, message) {
  if (!isActive(session) || session.terminal) {
    return;
  }

  var result;
  try {
    result = session.assembler.add(message);
  } catch (error) {
    terminateWithFailure(session, protocol.STATUS_CODES.PROTOCOL);
    return;
  }

  if (!result.complete) {
    return;
  }

  session.transcript = result.transcript;
  session.handoffTimer = setTimeout(function() {
    terminateWithFailure(session, protocol.STATUS_CODES.TRANSFER);
  }, WS_REQUEST_TIMEOUT_MS);
  trySendTranscript(session);
}

Pebble.addEventListener('ready', function() {
  console.log('Dictate WS PebbleKit JS ready');
});

Pebble.addEventListener('appmessage', function(event) {
  var message;
  try {
    message = protocol.parseWatchMessage(event.payload);
  } catch (error) {
    var rawRequestId = event.payload &&
        event.payload[protocol.KEYS.REQUEST_ID];
    if (activeSession &&
        (!rawRequestId || rawRequestId === activeSession.requestId)) {
      terminateWithFailure(activeSession, protocol.STATUS_CODES.PROTOCOL);
    }
    return;
  }

  if (message.type === protocol.MESSAGE_TYPES.SESSION_BEGIN) {
    startSession(message);
    return;
  }

  if (!activeSession || message.requestId !== activeSession.requestId) {
    return;
  }

  if (message.type === protocol.MESSAGE_TYPES.TRANSCRIPT_CHUNK) {
    handleChunk(activeSession, message);
  } else if (message.type === protocol.MESSAGE_TYPES.SESSION_END) {
    cleanupSession(activeSession);
  }
});
