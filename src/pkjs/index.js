'use strict';

var protocol = require('./protocol');
var sessionModule = require('./bridge_session');
var watchModule = require('./watch_bridge');

// Change this build-time value to the WebSocket server reachable by the phone.
var WEBSOCKET_URL = 'ws://192.168.50.199:8080/';
var WS_REQUEST_TIMEOUT_MS = 2000;
var SERVER_RESULT_TIMEOUT_MS = 20000;

var activeSession = null;
var appMessageTransport = new watchModule.AppMessageTransport(
    function(payload, success, failure) {
      Pebble.sendAppMessage(payload, success, failure);
    });

function startSession(message) {
  if (activeSession) {
    activeSession.close();
  }
  activeSession = new sessionModule.BridgeSession(message.requestId, {
    websocketUrl: WEBSOCKET_URL,
    WebSocket: WebSocket,
    timerApi: {
      setTimeout: setTimeout,
      clearTimeout: clearTimeout
    },
    handoffTimeoutMs: WS_REQUEST_TIMEOUT_MS,
    resultTimeoutMs: SERVER_RESULT_TIMEOUT_MS,
    appMessageTransport: appMessageTransport,
    onClose: function(session) {
      if (activeSession === session) {
        activeSession = null;
      }
    }
  });
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
      activeSession.failProtocol();
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

  activeSession.handleWatchMessage(message);
});
