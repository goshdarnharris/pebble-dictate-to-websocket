'use strict';

var protocol = require('./protocol');
var watchModule = require('./watch_bridge');
var websocketModule = require('./websocket_bridge');

var STATES = {
  RECEIVING_TRANSCRIPT: 'receiving_transcript',
  WAITING_TO_SEND: 'waiting_to_send',
  AWAITING_RESULT: 'awaiting_result',
  FORWARDING_TERMINAL: 'forwarding_terminal',
  CLOSED: 'closed'
};

function BridgeSession(requestId, options) {
  this.requestId = requestId;
  this.options = options;
  this.state = STATES.RECEIVING_TRANSCRIPT;
  this.terminal = false;
  this.watch = new watchModule.WatchBridge(
      requestId,
      options.appMessageTransport,
      function() {
        this.close();
      }.bind(this));
  this.server = new websocketModule.WebSocketBridge(
      requestId,
      options.websocketUrl,
      {
        WebSocket: options.WebSocket,
        timerApi: options.timerApi,
        handoffTimeoutMs: options.handoffTimeoutMs,
        resultTimeoutMs: options.resultTimeoutMs,
        onEvent: this.handleServerEvent.bind(this)
      });
  this.server.open();
}

BridgeSession.prototype.handleWatchMessage = function(message) {
  if (this.state === STATES.CLOSED || this.terminal) {
    return;
  }
  if (message.type === protocol.MESSAGE_TYPES.SESSION_END) {
    this.close();
    return;
  }
  if (message.type !== protocol.MESSAGE_TYPES.TRANSCRIPT_CHUNK ||
      this.state !== STATES.RECEIVING_TRANSCRIPT) {
    this.fail(protocol.STATUS_CODES.ERROR_PROTOCOL);
    return;
  }

  var result;
  try {
    result = this.watch.addChunk(message);
  } catch (error) {
    this.fail(protocol.STATUS_CODES.ERROR_PROTOCOL);
    return;
  }
  if (result.complete) {
    this.state = STATES.WAITING_TO_SEND;
    this.server.setTranscript(result.transcript);
  }
};

BridgeSession.prototype.handleServerEvent = function(event) {
  if (this.state === STATES.CLOSED || this.terminal) {
    return;
  }
  if (event.type === websocketModule.EVENTS.SEND_STARTED) {
    this.state = STATES.AWAITING_RESULT;
    this.watch.sendStatus(protocol.MESSAGE_TYPES.SEND_STARTED, null);
    return;
  }
  if (event.type === websocketModule.EVENTS.RESULT) {
    this.terminal = true;
    this.state = STATES.FORWARDING_TERMINAL;
    this.server.close();
    this.watch.sendResult(
        event.success,
        event.response,
        function(succeeded) {
          if (succeeded) {
            this.watch.markComplete();
          }
          this.close();
        }.bind(this));
    return;
  }
  if (event.type === websocketModule.EVENTS.FAILURE) {
    this.fail(this.statusForServerError(event.error));
  }
};

BridgeSession.prototype.statusForServerError = function(error) {
  if (error === websocketModule.ERRORS.RESULT_TIMEOUT) {
    return protocol.STATUS_CODES.ERROR_SERVER_RESULT_TIMEOUT;
  }
  if (error === websocketModule.ERRORS.PROTOCOL) {
    return protocol.STATUS_CODES.ERROR_PROTOCOL;
  }
  if (error === websocketModule.ERRORS.SERVER) {
    return protocol.STATUS_CODES.ERROR_SERVER;
  }
  return protocol.STATUS_CODES.ERROR_TRANSFER;
};

BridgeSession.prototype.failProtocol = function() {
  this.fail(protocol.STATUS_CODES.ERROR_PROTOCOL);
};

BridgeSession.prototype.fail = function(statusCode) {
  if (this.state === STATES.CLOSED || this.terminal) {
    return;
  }
  this.terminal = true;
  this.state = STATES.FORWARDING_TERMINAL;
  this.server.close();
  this.watch.sendStatus(
      protocol.MESSAGE_TYPES.FAILURE,
      statusCode,
      function() {
        this.close();
      }.bind(this));
};

BridgeSession.prototype.close = function() {
  if (this.state === STATES.CLOSED) {
    return;
  }
  this.server.close();
  this.watch.close();
  this.state = STATES.CLOSED;
  if (this.options.onClose) {
    this.options.onClose(this);
  }
};

module.exports = {
  BridgeSession: BridgeSession,
  STATES: STATES
};