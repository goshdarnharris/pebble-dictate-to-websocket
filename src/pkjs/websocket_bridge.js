'use strict';

var BridgeTimer = require('./bridge_timer');
var protocol = require('./protocol');

var STATES = {
  IDLE: 'idle',
  CONNECTING: 'connecting',
  READY: 'ready',
  SENDING: 'sending',
  AWAITING_RESULT: 'awaiting_result',
  COMPLETE: 'complete',
  FAILED: 'failed',
  CLOSED: 'closed'
};

var ERRORS = {
  NONE: 'none',
  SERVER: 'server',
  PROTOCOL: 'protocol',
  SEND: 'send',
  HANDOFF_TIMEOUT: 'handoff_timeout',
  RESULT_TIMEOUT: 'result_timeout'
};

var EVENTS = {
  SEND_STARTED: 'send_started',
  RESULT: 'result',
  FAILURE: 'failure'
};

function WebSocketBridge(requestId, url, options) {
  this.requestId = requestId;
  this.url = url;
  this.WebSocket = options.WebSocket;
  this.onEvent = options.onEvent;
  this.socket = null;
  this.transcript = null;
  this.pendingFrames = [];
  this.state = STATES.IDLE;
  this.error = ERRORS.NONE;
  this.handoffTimeout = new BridgeTimer(
      options.handoffTimeoutMs, options.timerApi);
  this.resultTimeout = new BridgeTimer(
      options.resultTimeoutMs, options.timerApi);
}

WebSocketBridge.prototype.open = function() {
  if (this.state !== STATES.IDLE) {
    return;
  }

  this.state = STATES.CONNECTING;
  try {
    this.socket = new this.WebSocket(this.url);
  } catch (error) {
    this.fail(ERRORS.SERVER);
    return;
  }

  this.socket.onopen = function() {
    if (this.state !== STATES.CONNECTING) {
      return;
    }
    this.state = STATES.READY;
    this.trySend();
  }.bind(this);
  this.socket.onerror = function() {
    this.fail(ERRORS.SERVER);
  }.bind(this);
  this.socket.onclose = function() {
    this.fail(ERRORS.SERVER);
  }.bind(this);
  this.socket.onmessage = function(event) {
    if (this.state === STATES.SENDING) {
      this.pendingFrames.push(event.data);
      return;
    }
    this.handleFrame(event.data);
  }.bind(this);
};

WebSocketBridge.prototype.setTranscript = function(transcript) {
  if (this.transcript !== null || this.state === STATES.CLOSED ||
      this.state === STATES.FAILED) {
    this.fail(ERRORS.SEND);
    return;
  }

  this.transcript = transcript;
  this.handoffTimeout.start(function() {
    this.fail(ERRORS.HANDOFF_TIMEOUT);
  }.bind(this));
  this.trySend();
};

WebSocketBridge.prototype.trySend = function() {
  if (this.transcript === null || !this.socket ||
      this.socket.readyState !== 1 || this.state === STATES.SENDING ||
      this.state === STATES.AWAITING_RESULT ||
      this.state === STATES.COMPLETE ||
      this.state === STATES.FAILED || this.state === STATES.CLOSED) {
    return;
  }

  var serialized;
  try {
    serialized = JSON.stringify(
        protocol.buildRequest(this.requestId, this.transcript));
    this.state = STATES.SENDING;
    this.socket.send(serialized);
  } catch (error) {
    this.fail(ERRORS.SEND);
    return;
  }

  this.handoffTimeout.cancel();
  this.state = STATES.AWAITING_RESULT;
  this.resultTimeout.start(function() {
    this.fail(ERRORS.RESULT_TIMEOUT);
  }.bind(this));
  this.onEvent({type: EVENTS.SEND_STARTED});

  var pendingFrames = this.pendingFrames;
  this.pendingFrames = [];
  for (var i = 0; i < pendingFrames.length; i++) {
    this.handleFrame(pendingFrames[i]);
  }
};

WebSocketBridge.prototype.handleFrame = function(data) {
  if (this.state !== STATES.AWAITING_RESULT) {
    return;
  }

  var result = protocol.parseServerFrame(data, this.requestId);
  if (result.kind === 'result') {
    this.resultTimeout.cancel();
    this.state = STATES.COMPLETE;
    this.onEvent({
      type: EVENTS.RESULT,
      success: result.success,
      response: result.response
    });
  } else if (result.kind === 'server_error') {
    this.fail(ERRORS.SERVER);
  } else if (result.kind === 'protocol_error') {
    this.fail(ERRORS.PROTOCOL);
  }
};

WebSocketBridge.prototype.detachAndCloseSocket = function() {
  if (!this.socket) {
    return;
  }
  this.socket.onopen = null;
  this.socket.onerror = null;
  this.socket.onclose = null;
  this.socket.onmessage = null;
  if (this.socket.readyState === 0 || this.socket.readyState === 1) {
    try {
      this.socket.close();
    } catch (error) {
      console.log('WebSocket close failed');
    }
  }
  this.socket = null;
};

WebSocketBridge.prototype.fail = function(error) {
  if (this.state === STATES.FAILED || this.state === STATES.CLOSED ||
      this.state === STATES.COMPLETE) {
    return;
  }
  this.error = error;
  this.state = STATES.FAILED;
  this.handoffTimeout.cancel();
  this.resultTimeout.cancel();
  this.detachAndCloseSocket();
  this.onEvent({type: EVENTS.FAILURE, error: error});
};

WebSocketBridge.prototype.close = function() {
  if (this.state === STATES.CLOSED) {
    return;
  }
  this.handoffTimeout.cancel();
  this.resultTimeout.cancel();
  this.detachAndCloseSocket();
  this.transcript = null;
  this.pendingFrames = [];
  this.state = STATES.CLOSED;
};

module.exports = {
  WebSocketBridge: WebSocketBridge,
  STATES: STATES,
  ERRORS: ERRORS,
  EVENTS: EVENTS
};