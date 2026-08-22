'use strict';

var assert = require('assert');
var protocol = require('../../src/pkjs/protocol');
var packageManifest = require('../../package.json');

var REQUEST_ID = '1a2b3c4d';

function chunk(index, count, totalBytes, text) {
  return {
    version: protocol.VERSION,
    type: protocol.MESSAGE_TYPES.TRANSCRIPT_CHUNK,
    requestId: REQUEST_ID,
    chunkIndex: index,
    chunkCount: count,
    totalBytes: totalBytes,
    chunkText: text
  };
}

function watchPayload(type) {
  var payload = {};
  payload[protocol.KEYS.PROTOCOL_VERSION] = protocol.VERSION;
  payload[protocol.KEYS.MESSAGE_TYPE] = type;
  payload[protocol.KEYS.REQUEST_ID] = REQUEST_ID;
  return payload;
}

function testSingleChunk() {
  var text = 'hello';
  var assembler = new protocol.TranscriptAssembler(REQUEST_ID);
  var result = assembler.add(
      chunk(0, 1, protocol.utf8ByteLength(text), text));
  assert.deepStrictEqual(result, {
    complete: true,
    transcript: text
  });
}

function testMultiChunkUtf8() {
  var first = 'caf\u00e9 ';
  var second = '\ud83d\ude80 ready';
  var text = first + second;
  var assembler = new protocol.TranscriptAssembler(REQUEST_ID);

  assert.deepStrictEqual(
      assembler.add(chunk(0, 2, protocol.utf8ByteLength(text), first)),
      {complete: false});
  assert.deepStrictEqual(
      assembler.add(chunk(1, 2, protocol.utf8ByteLength(text), second)),
      {complete: true, transcript: text});
  assert.strictEqual(protocol.utf8ByteLength(text), 16);
}

function testInvalidChunkSequences() {
  var bytes = protocol.utf8ByteLength('ab');
  var duplicate = new protocol.TranscriptAssembler(REQUEST_ID);
  duplicate.add(chunk(0, 2, bytes, 'a'));
  assert.throws(function() {
    duplicate.add(chunk(0, 2, bytes, 'a'));
  }, /chunk index/);

  var missing = new protocol.TranscriptAssembler(REQUEST_ID);
  assert.throws(function() {
    missing.add(chunk(1, 2, bytes, 'b'));
  }, /chunk index/);

  var inconsistent = new protocol.TranscriptAssembler(REQUEST_ID);
  inconsistent.add(chunk(0, 2, bytes, 'a'));
  assert.throws(function() {
    inconsistent.add(chunk(1, 3, bytes, 'b'));
  }, /metadata/);

  var wrongBytes = new protocol.TranscriptAssembler(REQUEST_ID);
  assert.throws(function() {
    wrongBytes.add(chunk(0, 1, 99, 'ab'));
  }, /byte count/);
}

function testInvalidUtf16() {
  assert.throws(function() {
    protocol.utf8ByteLength('\ud83d');
  }, /surrogate/);
  assert.throws(function() {
    protocol.utf8ByteLength('\ude80');
  }, /surrogate/);
}

function testWatchMessageParsing() {
  var begin = watchPayload(protocol.MESSAGE_TYPES.SESSION_BEGIN);
  assert.deepStrictEqual(protocol.parseWatchMessage(begin), {
    version: protocol.VERSION,
    type: protocol.MESSAGE_TYPES.SESSION_BEGIN,
    requestId: REQUEST_ID
  });

  var payload = watchPayload(protocol.MESSAGE_TYPES.TRANSCRIPT_CHUNK);
  payload[protocol.KEYS.CHUNK_INDEX] = 0;
  payload[protocol.KEYS.CHUNK_COUNT] = 1;
  payload[protocol.KEYS.TOTAL_BYTES] = 2;
  payload[protocol.KEYS.CHUNK_TEXT] = 'ok';
  assert.strictEqual(protocol.parseWatchMessage(payload).chunkText, 'ok');

  payload[protocol.KEYS.CHUNK_INDEX] = 1;
  assert.throws(function() {
    protocol.parseWatchMessage(payload);
  }, /chunk/);

  var end = watchPayload(protocol.MESSAGE_TYPES.SESSION_END);
  end[protocol.KEYS.STATUS_CODE] = 99;
  assert.throws(function() {
    protocol.parseWatchMessage(end);
  }, /status/);
}

function testRequestShape() {
  assert.deepStrictEqual(
      protocol.buildRequest(REQUEST_ID, 'example'),
      {
        version: 1,
        type: 'dictation',
        requestId: REQUEST_ID,
        transcript: 'example'
      });
}

function testServerFrames() {
  assert.deepStrictEqual(
      protocol.parseServerFrame(
          JSON.stringify({
            version: 1,
            type: 'ack',
            requestId: REQUEST_ID
          }),
          REQUEST_ID),
      {kind: 'ack'});

  assert.deepStrictEqual(
      protocol.parseServerFrame(
          JSON.stringify({
            version: 1,
            type: 'error',
            requestId: REQUEST_ID,
            code: 'invalid_request'
          }),
          REQUEST_ID),
      {kind: 'error', code: 'invalid_request'});

  var unrelatedFrames = [
    'not json',
    JSON.stringify({version: 2, type: 'ack', requestId: REQUEST_ID}),
    JSON.stringify({version: 1, type: 'ack', requestId: 'ffffffff'}),
    JSON.stringify({version: 1, type: 'unknown', requestId: REQUEST_ID}),
    JSON.stringify({
      version: 1,
      type: 'error',
      requestId: REQUEST_ID,
      code: ''
    }),
    Buffer.from('binary')
  ];

  unrelatedFrames.forEach(function(frame) {
    assert.deepStrictEqual(
        protocol.parseServerFrame(frame, REQUEST_ID),
        {kind: 'unrelated'});
  });
}

function testManifestMessageKeys() {
  assert.deepStrictEqual(packageManifest.pebble.messageKeys, {
    PROTOCOL_VERSION: 0,
    MESSAGE_TYPE: 1,
    REQUEST_ID: 2,
    CHUNK_INDEX: 3,
    CHUNK_COUNT: 4,
    TOTAL_BYTES: 5,
    CHUNK_TEXT: 6,
    STATUS_CODE: 7
  });
  assert.deepStrictEqual(packageManifest.pebble.targetPlatforms, ['emery']);
}

testSingleChunk();
testMultiChunkUtf8();
testInvalidChunkSequences();
testInvalidUtf16();
testWatchMessageParsing();
testRequestShape();
testServerFrames();
testManifestMessageKeys();

console.log('All PebbleKit JS protocol tests passed.');
