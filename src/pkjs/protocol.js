'use strict';

var VERSION = 1;

var KEYS = {
  PROTOCOL_VERSION: 'PROTOCOL_VERSION',
  MESSAGE_TYPE: 'MESSAGE_TYPE',
  REQUEST_ID: 'REQUEST_ID',
  CHUNK_INDEX: 'CHUNK_INDEX',
  CHUNK_COUNT: 'CHUNK_COUNT',
  TOTAL_BYTES: 'TOTAL_BYTES',
  CHUNK_TEXT: 'CHUNK_TEXT',
  STATUS_CODE: 'STATUS_CODE'
};

var MESSAGE_TYPES = {
  SESSION_BEGIN: 1,
  TRANSCRIPT_CHUNK: 2,
  SESSION_END: 3,
  SEND_STARTED: 10,
  REMOTE_ACK: 11,
  FAILURE: 12
};

var STATUS_CODES = {
  NORMAL: 0,
  CANCELLED: 1,
  DICTATION: 2,
  NO_SPEECH: 3,
  CONNECTIVITY: 4,
  PROTOCOL: 5,
  TRANSFER: 6,
  SERVER_ERROR: 7,
  ACK_TIMEOUT: 8
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
    if (message.statusCode > STATUS_CODES.ACK_TIMEOUT) {
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
      message.version !== VERSION ||
      message.requestId !== requestId) {
    return {
      kind: 'unrelated'
    };
  }

  if (message.type === 'ack') {
    return {
      kind: 'ack'
    };
  }

  if (message.type === 'error' &&
      typeof message.code === 'string' &&
      message.code.length > 0) {
    return {
      kind: 'error',
      code: message.code
    };
  }

  return {
    kind: 'unrelated'
  };
}

module.exports = {
  VERSION: VERSION,
  KEYS: KEYS,
  MESSAGE_TYPES: MESSAGE_TYPES,
  STATUS_CODES: STATUS_CODES,
  ProtocolError: protocolError,
  TranscriptAssembler: TranscriptAssembler,
  buildRequest: buildRequest,
  parseServerFrame: parseServerFrame,
  parseWatchMessage: parseWatchMessage,
  utf8ByteLength: utf8ByteLength
};
