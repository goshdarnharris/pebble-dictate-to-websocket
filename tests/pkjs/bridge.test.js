'use strict';

var assert = require('assert');
var protocol = require('../../src/pkjs/protocol');
var sessionModule = require('../../src/pkjs/bridge_session');
var watchModule = require('../../src/pkjs/watch_bridge');

var REQUEST_ID = '1a2b3c4d';

function FakeTimers() {
  this.nextId = 1;
  this.tasks = {};
}

FakeTimers.prototype.setTimeout = function(callback, durationMs) {
  var id = this.nextId++;
  this.tasks[id] = {callback: callback, durationMs: durationMs};
  return id;
};

FakeTimers.prototype.clearTimeout = function(id) {
  delete this.tasks[id];
};

FakeTimers.prototype.fire = function(durationMs) {
  var ids = Object.keys(this.tasks);
  for (var i = 0; i < ids.length; i++) {
    var task = this.tasks[ids[i]];
    if (task.durationMs === durationMs) {
      delete this.tasks[ids[i]];
      task.callback();
      return;
    }
  }
  throw new Error('No timer for ' + durationMs + ' ms');
};

function FakeWebSocket(url) {
  this.url = url;
  this.readyState = 0;
  this.sent = [];
  this.closed = false;
  FakeWebSocket.instances.push(this);
}

FakeWebSocket.instances = [];

FakeWebSocket.prototype.open = function() {
  this.readyState = 1;
  this.onopen();
};

FakeWebSocket.prototype.send = function(data) {
  this.sent.push(data);
  if (this.replyDuringSend) {
    this.onmessage({data: this.replyDuringSend});
  }
};

FakeWebSocket.prototype.receive = function(data) {
  this.onmessage({data: data});
};

FakeWebSocket.prototype.close = function() {
  this.closed = true;
  this.readyState = 3;
};

function FakeAppMessages() {
  this.sent = [];
  this.callbacks = [];
}

FakeAppMessages.prototype.send = function(payload, success, failure) {
  this.sent.push(payload);
  this.callbacks.push({success: success, failure: failure});
};

FakeAppMessages.prototype.succeed = function() {
  this.callbacks.shift().success();
};

FakeAppMessages.prototype.fail = function() {
  this.callbacks.shift().failure();
};

function transcriptChunk(text) {
  return {
    version: protocol.VERSION,
    type: protocol.MESSAGE_TYPES.TRANSCRIPT_CHUNK,
    requestId: REQUEST_ID,
    chunkIndex: 0,
    chunkCount: 1,
    totalBytes: protocol.utf8ByteLength(text),
    chunkText: text
  };
}

function resultFrame(success, response) {
  return JSON.stringify({
    version: protocol.VERSION,
    type: 'result',
    requestId: REQUEST_ID,
    success: success,
    response: response
  });
}

function createHarness() {
  FakeWebSocket.instances = [];
  var timers = new FakeTimers();
  var appMessages = new FakeAppMessages();
  var transport = new watchModule.AppMessageTransport(
      appMessages.send.bind(appMessages));
  var closed = false;
  var session = new sessionModule.BridgeSession(REQUEST_ID, {
    websocketUrl: 'ws://example.test/',
    WebSocket: FakeWebSocket,
    timerApi: timers,
    handoffTimeoutMs: 2000,
    resultTimeoutMs: 20000,
    appMessageTransport: transport,
    onClose: function() {
      closed = true;
    }
  });
  return {
    session: session,
    socket: FakeWebSocket.instances[0],
    timers: timers,
    appMessages: appMessages,
    isClosed: function() { return closed; }
  };
}

function messageType(payload) {
  return payload[protocol.KEYS.MESSAGE_TYPE];
}

function testResultWaitsBehindSendStarted() {
  var harness = createHarness();
  harness.socket.open();
  harness.session.handleWatchMessage(transcriptChunk('hello'));

  assert.strictEqual(harness.socket.sent.length, 1);
  assert.strictEqual(
      messageType(harness.appMessages.sent[0]),
      protocol.MESSAGE_TYPES.SEND_STARTED);

  harness.socket.receive(resultFrame(true, 'accepted'));
  assert.strictEqual(harness.appMessages.sent.length, 1);
  harness.appMessages.succeed();
  assert.strictEqual(
      messageType(harness.appMessages.sent[1]),
        protocol.MESSAGE_TYPES.SERVER_RESULT_CHUNK);
      assert.strictEqual(
        harness.appMessages.sent[1][protocol.KEYS.SERVER_SUCCESS], 1);
      assert.strictEqual(
        harness.appMessages.sent[1][protocol.KEYS.CHUNK_TEXT], 'accepted');
  harness.appMessages.succeed();
  assert.strictEqual(harness.isClosed(), true);
}

function testSynchronousReplyIsOrdered() {
  var harness = createHarness();
  harness.socket.open();
  harness.socket.replyDuringSend = resultFrame(false, 'rejected');
  harness.session.handleWatchMessage(transcriptChunk('hello'));

  assert.strictEqual(harness.appMessages.sent.length, 1);
  assert.strictEqual(
      messageType(harness.appMessages.sent[0]),
      protocol.MESSAGE_TYPES.SEND_STARTED);
  harness.appMessages.succeed();
  assert.strictEqual(
      messageType(harness.appMessages.sent[1]),
        protocol.MESSAGE_TYPES.SERVER_RESULT_CHUNK);
      assert.strictEqual(
        harness.appMessages.sent[1][protocol.KEYS.SERVER_SUCCESS], 0);
}

function testHandoffTimeout() {
  var harness = createHarness();
  harness.session.handleWatchMessage(transcriptChunk('hello'));
  harness.timers.fire(2000);

  assert.strictEqual(
      messageType(harness.appMessages.sent[0]),
      protocol.MESSAGE_TYPES.FAILURE);
  assert.strictEqual(
      harness.appMessages.sent[0][protocol.KEYS.STATUS_CODE],
      protocol.STATUS_CODES.ERROR_TRANSFER);
}

function testResultTimeoutQueuesAfterSendStarted() {
  var harness = createHarness();
  harness.socket.open();
  harness.session.handleWatchMessage(transcriptChunk('hello'));
  harness.timers.fire(20000);

  assert.strictEqual(harness.appMessages.sent.length, 1);
  harness.appMessages.succeed();
  assert.strictEqual(
      messageType(harness.appMessages.sent[1]),
      protocol.MESSAGE_TYPES.FAILURE);
  assert.strictEqual(
      harness.appMessages.sent[1][protocol.KEYS.STATUS_CODE],
        protocol.STATUS_CODES.ERROR_SERVER_RESULT_TIMEOUT);
}

function testAppMessageFailureClosesSession() {
  var harness = createHarness();
  harness.socket.open();
  harness.session.handleWatchMessage(transcriptChunk('hello'));
  harness.appMessages.fail();
  assert.strictEqual(harness.isClosed(), true);
}

function testMultiChunkResultIsDeliveredSequentially() {
  var harness = createHarness();
  var response = new Array(1025).join('x');
  harness.socket.open();
  harness.session.handleWatchMessage(transcriptChunk('hello'));
  harness.socket.receive(resultFrame(true, response));

  harness.appMessages.succeed();
  for (var index = 0; index < 4; index++) {
    var payload = harness.appMessages.sent[index + 1];
    assert.strictEqual(
        messageType(payload),
        protocol.MESSAGE_TYPES.SERVER_RESULT_CHUNK);
    assert.strictEqual(payload[protocol.KEYS.CHUNK_INDEX], index);
    assert.strictEqual(payload[protocol.KEYS.CHUNK_COUNT], 4);
    assert.strictEqual(payload[protocol.KEYS.TOTAL_BYTES], 1024);
    harness.appMessages.succeed();
  }
  assert.strictEqual(harness.isClosed(), true);
}

testResultWaitsBehindSendStarted();
testSynchronousReplyIsOrdered();
testHandoffTimeout();
testResultTimeoutQueuesAfterSendStarted();
testAppMessageFailureClosesSession();
testMultiChunkResultIsDeliveredSequentially();

console.log('All PebbleKit JS bridge tests passed.');