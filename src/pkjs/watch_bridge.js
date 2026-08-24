'use strict';

var protocol = require('./protocol');

var STATES = {
  RECEIVING_TRANSCRIPT: 'receiving_transcript',
  TRANSCRIPT_READY: 'transcript_ready',
  FORWARDING_RESULT: 'forwarding_result',
  COMPLETE: 'complete',
  FAILED: 'failed',
  CLOSED: 'closed'
};

var ERRORS = {
  NONE: 'none',
  PROTOCOL: 'protocol',
  APP_MESSAGE: 'app_message'
};

function AppMessageTransport(sendAppMessage) {
  this.sendAppMessage = sendAppMessage;
  this.queue = [];
  this.busy = false;
}

AppMessageTransport.prototype.enqueue = function(owner, payload, done) {
  this.queue.push({
    owner: owner,
    payload: payload,
    done: done
  });
  this.pump();
};

AppMessageTransport.prototype.cancel = function(owner) {
  var retained = [];
  for (var i = 0; i < this.queue.length; i++) {
    if (this.queue[i].owner !== owner) {
      retained.push(this.queue[i]);
    }
  }
  this.queue = retained;
};

AppMessageTransport.prototype.pump = function() {
  if (this.busy || this.queue.length === 0) {
    return;
  }

  var entry = this.queue.shift();
  this.busy = true;
  var finish = function(succeeded) {
    this.busy = false;
    entry.done(succeeded);
    this.pump();
  }.bind(this);

  try {
    this.sendAppMessage(
        entry.payload,
        function() {
          finish(true);
        },
        function() {
          finish(false);
        });
  } catch (error) {
    finish(false);
  }
};

function WatchBridge(requestId, transport, onDeliveryFailure) {
  this.requestId = requestId;
  this.transport = transport;
  this.onDeliveryFailure = onDeliveryFailure;
  this.assembler = new protocol.TranscriptAssembler(requestId);
  this.state = STATES.RECEIVING_TRANSCRIPT;
  this.error = ERRORS.NONE;
}

WatchBridge.prototype.addChunk = function(message) {
  if (this.state !== STATES.RECEIVING_TRANSCRIPT) {
    this.error = ERRORS.PROTOCOL;
    throw protocol.ProtocolError('Transcript is not being received');
  }

  var result;
  try {
    result = this.assembler.add(message);
  } catch (error) {
    this.error = ERRORS.PROTOCOL;
    throw error;
  }

  if (result.complete) {
    this.state = STATES.TRANSCRIPT_READY;
  }
  return result;
};

WatchBridge.prototype.sendStatus = function(type, statusCode, done) {
  if (this.state === STATES.CLOSED) {
    return;
  }

  var payload = {};
  payload[protocol.KEYS.PROTOCOL_VERSION] = protocol.VERSION;
  payload[protocol.KEYS.MESSAGE_TYPE] = type;
  payload[protocol.KEYS.REQUEST_ID] = this.requestId;
  if (typeof statusCode === 'number') {
    payload[protocol.KEYS.STATUS_CODE] = statusCode;
  }

  this.transport.enqueue(this, payload, function(succeeded) {
    if (this.state === STATES.CLOSED) {
      return;
    }
    if (!succeeded) {
      this.state = STATES.FAILED;
      this.error = ERRORS.APP_MESSAGE;
      this.onDeliveryFailure(this.error);
    }
    if (done) {
      done(succeeded);
    }
  }.bind(this));
};

WatchBridge.prototype.sendResult = function(success, response, done) {
  if (this.state === STATES.CLOSED) {
    return;
  }

  var payloads;
  try {
    payloads = protocol.buildServerResultChunks(
        this.requestId, success, response);
  } catch (error) {
    this.state = STATES.FAILED;
    this.error = ERRORS.PROTOCOL;
    this.onDeliveryFailure(this.error);
    return;
  }

  this.state = STATES.FORWARDING_RESULT;
  var sendNext = function(index) {
    this.transport.enqueue(this, payloads[index], function(succeeded) {
      if (this.state === STATES.CLOSED) {
        return;
      }
      if (!succeeded) {
        this.state = STATES.FAILED;
        this.error = ERRORS.APP_MESSAGE;
        this.onDeliveryFailure(this.error);
        return;
      }
      if (index + 1 < payloads.length) {
        sendNext(index + 1);
        return;
      }
      this.state = STATES.COMPLETE;
      if (done) {
        done(true);
      }
    }.bind(this));
  }.bind(this);
  sendNext(0);
};

WatchBridge.prototype.markComplete = function() {
  if (this.state !== STATES.CLOSED) {
    this.state = STATES.COMPLETE;
  }
};

WatchBridge.prototype.close = function() {
  if (this.state === STATES.CLOSED) {
    return;
  }
  this.transport.cancel(this);
  this.assembler.clear();
  this.state = STATES.CLOSED;
};

module.exports = {
  AppMessageTransport: AppMessageTransport,
  WatchBridge: WatchBridge,
  STATES: STATES,
  ERRORS: ERRORS
};