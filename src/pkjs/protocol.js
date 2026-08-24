'use strict';

var VERSION = 2;
var MAX_RESPONSE_BYTES = 1024;
var RESPONSE_CHUNK_BYTES = 256;

var KEYS = {
  PROTOCOL_VERSION: 'PROTOCOL_VERSION',
  MESSAGE_TYPE: 'MESSAGE_TYPE',
  REQUEST_ID: 'REQUEST_ID',
  CHUNK_INDEX: 'CHUNK_INDEX',
  CHUNK_COUNT: 'CHUNK_COUNT',
  TOTAL_BYTES: 'TOTAL_BYTES',
  CHUNK_TEXT: 'CHUNK_TEXT',
  STATUS_CODE: 'STATUS_CODE',
  SERVER_SUCCESS: 'SERVER_SUCCESS'
};

var MESSAGE_TYPES = {
  SESSION_BEGIN: 1,
  TRANSCRIPT_CHUNK: 2,
  SESSION_END: 3,
  SEND_STARTED: 10,
  FAILURE: 12,
  SERVER_RESULT_CHUNK: 13
};

var STATUS_CODES = {
  NORMAL: 0,
  CANCELLED: 1,
  ERROR_DICTATION: 2,
  ERROR_NO_SPEECH: 3,
  ERROR_PHONE_CONNECTIVITY: 4,
  ERROR_PROTOCOL: 5,
  ERROR_TRANSFER: 6,
  ERROR_SERVER: 7,
  ERROR_SERVER_RESULT_TIMEOUT: 8,
  ERROR_TRANSCRIPT_DELIVERY_TIMEOUT: 9,
  ERROR_WS_REQUEST_TIMEOUT: 10,
  ERROR_RESULT_TRANSFER_TIMEOUT: 11,
};

function protocolError(message) {
  var error = new Error(message);
  error.name = 'ProtocolError';
  return error;
}

function isUint32(value) {
  return typeof value === 'number' &&
      isFinite(value) &&
      Math.floor(value) === value &&
      value >= 0 &&
      value <= 0xffffffff;
}

function isRequestId(value) {
  return typeof value === 'string' &&
      value.length > 0 &&
      /^[\x20-\x7e]+$/.test(value);
}

function requireUint32(payload, key) {
  var value = payload[key];
  if (!isUint32(value)) {
    throw protocolError('Invalid integer field: ' + key);
  }
  return value;
}

function utf8ByteLength(text) {
  if (typeof text !== 'string') {
    throw protocolError('Expected a string');
  }

  var bytes = 0;
  for (var i = 0; i < text.length; i++) {
    var code = text.charCodeAt(i);
    if (code <= 0x7f) {
      bytes += 1;
    } else if (code <= 0x7ff) {
      bytes += 2;
    } else if (code >= 0xd800 && code <= 0xdbff) {
      if (i + 1 >= text.length) {
        throw protocolError('Unpaired high surrogate');
      }
      var low = text.charCodeAt(i + 1);
      if (low < 0xdc00 || low > 0xdfff) {
        throw protocolError('Unpaired high surrogate');
      }
      bytes += 4;
      i++;
    } else if (code >= 0xdc00 && code <= 0xdfff) {
      throw protocolError('Unpaired low surrogate');
    } else {
      bytes += 3;
    }
  }
  return bytes;
}

function parseWatchMessage(payload) {
  if (!payload || typeof payload !== 'object') {
    throw protocolError('Missing AppMessage payload');
  }

  var version = requireUint32(payload, KEYS.PROTOCOL_VERSION);
  var type = requireUint32(payload, KEYS.MESSAGE_TYPE);
  var requestId = payload[KEYS.REQUEST_ID];

  if (version !== VERSION) {
    throw protocolError('Unsupported protocol version');
  }
  if (!isRequestId(requestId)) {
    throw protocolError('Invalid request identifier');
  }

  var message = {
    version: version,
    type: type,
    requestId: requestId
  };

  if (type === MESSAGE_TYPES.TRANSCRIPT_CHUNK) {
    message.chunkIndex = requireUint32(payload, KEYS.CHUNK_INDEX);
    message.chunkCount = requireUint32(payload, KEYS.CHUNK_COUNT);
    message.totalBytes = requireUint32(payload, KEYS.TOTAL_BYTES);
    message.chunkText = payload[KEYS.CHUNK_TEXT];

    if (message.chunkCount === 0 ||
        message.totalBytes === 0 ||
        message.chunkIndex >= message.chunkCount ||
        typeof message.chunkText !== 'string' ||
        message.chunkText.length === 0) {
      throw protocolError('Invalid transcript chunk');
    }
    utf8ByteLength(message.chunkText);
  } else if (type === MESSAGE_TYPES.SESSION_END) {
    message.statusCode = requireUint32(payload, KEYS.STATUS_CODE);
    if (message.statusCode > STATUS_CODES.ERROR_RESULT_TRANSFER_TIMEOUT) {
      throw protocolError('Invalid status code');
    }
  } else if (type !== MESSAGE_TYPES.SESSION_BEGIN) {
    throw protocolError('Unexpected watch message type');
  }

  return message;
}

function TranscriptAssembler(requestId) {
  if (!isRequestId(requestId)) {
    throw protocolError('Invalid request identifier');
  }

  this.requestId = requestId;
  this.chunkCount = null;
  this.totalBytes = null;
  this.nextIndex = 0;
  this.chunks = [];
}

TranscriptAssembler.prototype.add = function(message) {
  if (!message ||
      message.type !== MESSAGE_TYPES.TRANSCRIPT_CHUNK ||
      message.requestId !== this.requestId) {
    throw protocolError('Chunk belongs to another request');
  }

  if (this.chunkCount === null) {
    this.chunkCount = message.chunkCount;
    this.totalBytes = message.totalBytes;
  } else if (message.chunkCount !== this.chunkCount ||
             message.totalBytes !== this.totalBytes) {
    throw protocolError('Inconsistent transcript metadata');
  }

  if (message.chunkIndex !== this.nextIndex) {
    throw protocolError('Unexpected transcript chunk index');
  }

  utf8ByteLength(message.chunkText);
  this.chunks.push(message.chunkText);
  this.nextIndex++;

  if (this.nextIndex < this.chunkCount) {
    return {
      complete: false
    };
  }

  var transcript = this.chunks.join('');
  if (utf8ByteLength(transcript) !== this.totalBytes) {
    throw protocolError('Transcript byte count mismatch');
  }

  return {
    complete: true,
    transcript: transcript
  };
};

TranscriptAssembler.prototype.clear = function() {
  this.chunks.length = 0;
  this.chunkCount = null;
  this.totalBytes = null;
  this.nextIndex = 0;
};

function buildRequest(requestId, transcript) {
  if (!isRequestId(requestId) || typeof transcript !== 'string') {
    throw protocolError('Cannot build request');
  }
  utf8ByteLength(transcript);

  return {
    version: VERSION,
    type: 'dictation',
    requestId: requestId,
    transcript: transcript
  };
}

function splitUtf8(text, maxChunkBytes) {
  var totalBytes = utf8ByteLength(text);
  if (totalBytes > MAX_RESPONSE_BYTES) {
    throw protocolError('Server response is too large');
  }
  if (!isUint32(maxChunkBytes) || maxChunkBytes === 0) {
    throw protocolError('Invalid response chunk size');
  }

  var chunks = [];
  var chunk = '';
  var chunkBytes = 0;
  for (var i = 0; i < text.length; i++) {
    var end = i + 1;
    var code = text.charCodeAt(i);
    if (code >= 0xd800 && code <= 0xdbff) {
      end++;
    }
    var character = text.substring(i, end);
    var characterBytes = utf8ByteLength(character);
    if (characterBytes > maxChunkBytes) {
      throw protocolError('Response chunk size cannot hold one character');
    }
    if (chunkBytes > 0 && chunkBytes + characterBytes > maxChunkBytes) {
      chunks.push(chunk);
      chunk = '';
      chunkBytes = 0;
    }
    chunk += character;
    chunkBytes += characterBytes;
    i = end - 1;
  }
  chunks.push(chunk);
  return chunks;
}

function buildServerResultChunks(requestId, success, response) {
  if (!isRequestId(requestId) || typeof success !== 'boolean' ||
      typeof response !== 'string') {
    throw protocolError('Cannot build server result chunks');
  }

  var totalBytes = utf8ByteLength(response);
  var chunks = splitUtf8(response, RESPONSE_CHUNK_BYTES);
  var payloads = [];
  for (var i = 0; i < chunks.length; i++) {
    var payload = {};
    payload[KEYS.PROTOCOL_VERSION] = VERSION;
    payload[KEYS.MESSAGE_TYPE] = MESSAGE_TYPES.SERVER_RESULT_CHUNK;
    payload[KEYS.REQUEST_ID] = requestId;
    payload[KEYS.CHUNK_INDEX] = i;
    payload[KEYS.CHUNK_COUNT] = chunks.length;
    payload[KEYS.TOTAL_BYTES] = totalBytes;
    payload[KEYS.CHUNK_TEXT] = chunks[i];
    payload[KEYS.SERVER_SUCCESS] = success ? 1 : 0;
    payloads.push(payload);
  }
  return payloads;
}

function parseServerFrame(data, requestId) {
  if (typeof data !== 'string' || !isRequestId(requestId)) {
    return {
      kind: 'unrelated'
    };
  }

  var message;
  try {
    message = JSON.parse(data);
  } catch (error) {
    return {
      kind: 'unrelated'
    };
  }

  if (!message ||
      Object.prototype.toString.call(message) !== '[object Object]' ||
      message.requestId !== requestId) {
    return {
      kind: 'unrelated'
    };
  }

  if (message.version !== VERSION) {
    return {
      kind: 'protocol_error'
    };
  }

  if (message.type === 'error' &&
      typeof message.code === 'string' &&
      message.code.length > 0) {
    return {
      kind: 'server_error',
      code: message.code
    };
  }

  if (message.type === 'result') {
    if (typeof message.success !== 'boolean' ||
        typeof message.response !== 'string') {
      return {kind: 'protocol_error'};
    }
    try {
      if (utf8ByteLength(message.response) > MAX_RESPONSE_BYTES) {
        return {kind: 'protocol_error'};
      }
    } catch (error) {
      return {kind: 'protocol_error'};
    }
    return {
      kind: 'result',
      success: message.success,
      response: message.response
    };
  }

  return {
    kind: 'protocol_error'
  };
}

module.exports = {
  VERSION: VERSION,
  MAX_RESPONSE_BYTES: MAX_RESPONSE_BYTES,
  RESPONSE_CHUNK_BYTES: RESPONSE_CHUNK_BYTES,
  KEYS: KEYS,
  MESSAGE_TYPES: MESSAGE_TYPES,
  STATUS_CODES: STATUS_CODES,
  ProtocolError: protocolError,
  TranscriptAssembler: TranscriptAssembler,
  buildRequest: buildRequest,
  buildServerResultChunks: buildServerResultChunks,
  parseServerFrame: parseServerFrame,
  parseWatchMessage: parseWatchMessage,
  splitUtf8: splitUtf8,
  utf8ByteLength: utf8ByteLength
};
